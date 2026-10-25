// Format card (cardmkfs.c, compiled as is) over disk images: 128 GiB, 8 GiB and a GPT'd 16 GiB card as sparse files,
// and a 512 MiB RAM disk cut at every write of the format. Each image goes through cardmkfs_run as the board runs it:
// a FatFs drive ("1:") on a diskio whose window the io moves, like sdcard.c's partition diskio. After it, the disk is
// read the way the board's next boot reads it (find_partition: a GPT header at LBA 1, else the whole disk), mounted,
// and written through cardfs. Then fsck.vfat -n checks the partition and file -s names the disk and the partition,
// when they're installed (fsck has no offset option, so the image file keeps a copy of every sector past
// CARDMKFS_PART_LBA in a second file, the partition alone).
//
// On the linux target IDF's vfs_linux.c replaces open, pread, pwrite, ftruncate, close and unlink with esp_vfs_*
// calls, which know only the registered prefixes (/card, /fmt). So the image files use the raw system calls, and the
// files on the volume use open/read/write (glibc's fopen bypasses the replacement and finds no /fmt).
#include "test_format.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cardfs.h"
#include "cardmkfs.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "ramdisk.h"
#include "unity.h"

#define SS CARDMKFS_SECTOR
#define FMT_ROOT "/fmt"
#define GIB_SECTORS (1024u * 1024 * 1024 / SS)
#define IMG_DIR "build"
#define SHIPPED_PART_LBA 2048 // where the card's GPT partition starts as it ships

/* ---- A sparse image file, its partition mirrored into a second file ---- */

static int s_img = -1, s_part = -1;
static uint32_t s_img_sectors;

static DSTATUS img_init(unsigned char pdrv)
{
    return 0;
}

static DRESULT img_read(unsigned char pdrv, unsigned char *buf, uint32_t sector, UINT count)
{
    if (sector >= s_img_sectors || count > s_img_sectors - sector) {
        return RES_PARERR;
    }
    const long n = (long)count * SS;
    return syscall(SYS_pread64, s_img, buf, n, (off_t)sector * SS) == n ? RES_OK : RES_ERROR;
}

static DRESULT img_write(unsigned char pdrv, const unsigned char *buf, uint32_t sector, UINT count)
{
    if (sector >= s_img_sectors || count > s_img_sectors - sector) {
        return RES_PARERR;
    }
    for (UINT i = 0; i < count; i++) {
        const uint32_t lba = sector + i;
        const unsigned char *p = buf + (size_t)i * SS;
        if (syscall(SYS_pwrite64, s_img, p, SS, (off_t)lba * SS) != SS) {
            return RES_ERROR;
        }
        if (lba >= CARDMKFS_PART_LBA &&
            syscall(SYS_pwrite64, s_part, p, SS, (off_t)(lba - CARDMKFS_PART_LBA) * SS) != SS) {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

static DRESULT img_ioctl(unsigned char pdrv, unsigned char cmd, void *buf)
{
    return cmd == CTRL_SYNC ? RES_OK : RES_PARERR; // only used through the window below
}

static const ff_diskio_impl_t s_img_impl = {
    .init = img_init, .status = img_init, .read = img_read, .write = img_write, .ioctl = img_ioctl};

static void img_path(char *out, size_t n, const char *name, const char *suffix)
{
    snprintf(out, n, IMG_DIR "/format-%s%s.img", name, suffix);
}

static int sys_create(const char *path)
{
    return (int)syscall(SYS_openat, AT_FDCWD, path, O_RDWR | O_CREAT | O_TRUNC, 0644);
}

static void img_open(const char *name, uint32_t sectors)
{
    char path[96];
    img_path(path, sizeof(path), name, "");
    s_img = sys_create(path);
    img_path(path, sizeof(path), name, ".part");
    s_part = sys_create(path);
    TEST_ASSERT_TRUE_MESSAGE(s_img >= 0 && s_part >= 0, "can't create the images in " IMG_DIR);
    // Sparse: unwritten sectors read as zeros and cost no disk space.
    TEST_ASSERT_EQUAL(0, syscall(SYS_ftruncate, s_img, (off_t)sectors * SS));
    TEST_ASSERT_EQUAL(0, syscall(SYS_ftruncate, s_part, (off_t)(sectors - CARDMKFS_PART_LBA) * SS));
    s_img_sectors = sectors;
}

static void img_close(void)
{
    syscall(SYS_close, s_img);
    syscall(SYS_close, s_part);
    s_img = s_part = -1;
}

// The images' apparent size is the card's (128 GiB), so they don't outlive the test.
static void img_remove(const char *name)
{
    char path[96];
    img_path(path, sizeof(path), name, "");
    syscall(SYS_unlinkat, AT_FDCWD, path, 0);
    img_path(path, sizeof(path), name, ".part");
    syscall(SYS_unlinkat, AT_FDCWD, path, 0);
}

/* ---- The board's view: a windowed drive over a backing disk, and the io cardmkfs_run writes through ---- */

static const ff_diskio_impl_t *s_back; // the whole disk: the image or the RAM disk
static uint32_t s_back_sectors;
static uint32_t s_win_first, s_win_sectors;
static char s_drive[3];
static FATFS *s_fs;
static uint8_t s_work[CARDFS_PIECE]; // the board's work buffer size
static const cardfs_volume_t s_vol = {
    .prepare = cardfs_fat_prepare, .room = cardfs_fat_room, .space = cardfs_fat_space, .ctx = s_drive};

static DRESULT win_read(unsigned char pdrv, unsigned char *buf, uint32_t sector, UINT count)
{
    if (sector >= s_win_sectors || count > s_win_sectors - sector) {
        return RES_PARERR;
    }
    return s_back->read(0, buf, s_win_first + sector, count);
}

static DRESULT win_write(unsigned char pdrv, const unsigned char *buf, uint32_t sector, UINT count)
{
    if (sector >= s_win_sectors || count > s_win_sectors - sector) {
        return RES_PARERR; // never outside the window
    }
    return s_back->write(0, buf, s_win_first + sector, count);
}

// As sdcard.c's part_disk_ioctl: no GET_BLOCK_SIZE and no CTRL_TRIM.
static DRESULT win_ioctl(unsigned char pdrv, unsigned char cmd, void *buf)
{
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buf = s_win_sectors;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buf = SS;
        return RES_OK;
    default:
        return RES_ERROR;
    }
}

static const ff_diskio_impl_t s_win_impl = {
    .init = img_init, .status = img_init, .read = win_read, .write = win_write, .ioctl = win_ioctl};

static esp_err_t io_write(uint32_t lba, const uint8_t *buf, void *ctx)
{
    TEST_ASSERT_TRUE(lba < s_back_sectors);
    return s_back->write(0, buf, lba, 1) == RES_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t io_read(uint32_t lba, uint8_t *buf, void *ctx)
{
    TEST_ASSERT_TRUE(lba < s_back_sectors);
    return s_back->read(0, buf, lba, 1) == RES_OK ? ESP_OK : ESP_FAIL;
}

static void io_window(uint32_t first, uint32_t sectors, void *ctx)
{
    s_win_first = first;
    s_win_sectors = sectors;
}

static const cardmkfs_io_t s_io = {.write = io_write, .read = io_read, .set_window = io_window};

// The second FatFs drive and its VFS path, once.
static void use_disk(const ff_diskio_impl_t *back, uint32_t sectors)
{
    if (!s_fs) {
        BYTE pdrv;
        TEST_ASSERT_EQUAL(ESP_OK, ff_diskio_get_drive(&pdrv));
        ff_diskio_register(pdrv, &s_win_impl);
        s_drive[0] = (char)('0' + pdrv);
        s_drive[1] = ':';
        const esp_vfs_fat_conf_t conf = {.base_path = FMT_ROOT, .fat_drive = s_drive, .max_files = 4};
        TEST_ASSERT_EQUAL(ESP_OK, esp_vfs_fat_register(&conf, &s_fs));
    }
    s_back = back;
    s_back_sectors = sectors;
    io_window(0, sectors, NULL);
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static void read_sector(uint32_t lba, uint8_t *buf)
{
    TEST_ASSERT_EQUAL(RES_OK, s_back->read(0, buf, lba, 1));
}

static void write_sector(uint32_t lba, const uint8_t *buf)
{
    TEST_ASSERT_EQUAL(RES_OK, s_back->write(0, buf, lba, 1));
}

// What sdcard.c's find_partition makes of the disk at the next boot: the first GPT entry when LBA 1 holds a GPT
// header, else the whole disk (FatFs then reads any MBR itself). Returns whether a GPT was found.
static bool boot_window(void)
{
    uint8_t s[SS];
    read_sector(1, s);
    if (memcmp(s, "EFI PART", 8) != 0) {
        io_window(0, s_back_sectors, NULL);
        return false;
    }
    read_sector(le32(s + 72), s);
    io_window(le32(s + 32), le32(s + 40) - le32(s + 32) + 1, NULL);
    return true;
}

static FRESULT mount_fmt(void)
{
    return f_mount(s_fs, s_drive, 1);
}

static void unmount_fmt(void)
{
    f_mount(NULL, s_drive, 0);
}

// A GPT as the card ships with: a protective MBR, the header and entries at LBA 1 and 2, the backup ones at the end,
// and one Microsoft basic data partition [first, capacity - 34]. No CRCs: find_partition doesn't read them.
static void write_gpt(uint32_t first)
{
    static const uint8_t BASIC_DATA[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                                           0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
    const uint32_t cap = s_back_sectors;
    uint8_t s[SS] = {0};
    s[446 + 4] = 0xEE;
    put_le32(s + 446 + 8, 1);
    put_le32(s + 446 + 12, cap - 1);
    s[510] = 0x55;
    s[511] = 0xAA;
    write_sector(0, s);

    uint8_t entry[SS] = {0};
    memcpy(entry, BASIC_DATA, 16);
    memset(entry + 16, 0x5A, 16); // the partition's own GUID
    put_le64(entry + 32, first);
    put_le64(entry + 40, cap - 34);
    for (int i = 0; i < 7; i++) {
        entry[56 + 2 * i] = (uint8_t)"tf-data"[i];
    }
    write_sector(2, entry);
    write_sector(cap - 33, entry);

    for (int backup = 0; backup < 2; backup++) {
        memset(s, 0, sizeof(s));
        memcpy(s, "EFI PART", 8);
        put_le32(s + 8, 0x00010000);
        put_le32(s + 12, 92);
        put_le64(s + 24, backup ? cap - 1 : 1);
        put_le64(s + 32, backup ? 1 : cap - 1);
        put_le64(s + 40, 34);
        put_le64(s + 48, cap - 34);
        put_le64(s + 72, backup ? cap - 33 : 2);
        put_le32(s + 80, 128);
        put_le32(s + 84, 128);
        write_sector(backup ? cap - 1 : 1, s);
    }
}

static void put_file(const char *rel, const char *text)
{
    char path[64];
    snprintf(path, sizeof(path), FMT_ROOT "/%s", rel);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL(strlen(text), write(fd, text, strlen(text)));
    TEST_ASSERT_EQUAL(0, close(fd));
}

static bool file_is(const char *rel, const char *text)
{
    char path[64], buf[128] = {0};
    snprintf(path, sizeof(path), FMT_ROOT "/%s", rel);
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n == (ssize_t)strlen(text) && memcmp(buf, text, n) == 0;
}

// An old FAT32 volume in the GPT partition at `first`, holding old.txt.
static void old_gpt_volume(uint32_t first)
{
    write_gpt(first);
    TEST_ASSERT_TRUE(boot_window());
    const MKFS_PARM opt = {.fmt = FM_FAT32 | FM_SFD, .n_fat = 2, .au_size = 4096};
    TEST_ASSERT_EQUAL(FR_OK, f_mkfs(s_drive, &opt, s_work, sizeof(s_work)));
    TEST_ASSERT_EQUAL(FR_OK, mount_fmt());
    put_file("old.txt", "the card's old contents");
    unmount_fmt();
}

static int run_tool(const char *cmd)
{
    printf("$ %s\n", cmd);
    fflush(stdout);
    const int st = system(cmd);
    fflush(stdout);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// fsck.vfat -n on the partition and file -s on both images, when they're installed (CI's IDF image has neither).
static void check_with_tools(const char *name)
{
    char disk[96], part[96], cmd[256];
    img_path(disk, sizeof(disk), name, "");
    img_path(part, sizeof(part), name, ".part");
    if (system("command -v file >/dev/null 2>&1") == 0) {
        snprintf(cmd, sizeof(cmd), "file -s %s %s", disk, part);
        run_tool(cmd);
    }
    if (system("command -v fsck.vfat >/dev/null 2>&1") != 0) {
        printf("fsck.vfat isn't installed, so %s wasn't checked\n", part);
        return;
    }
    snprintf(cmd, sizeof(cmd), "fsck.vfat -n -v %s", part);
    TEST_ASSERT_EQUAL_MESSAGE(0, run_tool(cmd), "fsck.vfat found a problem");
}

// The format as the board runs it, then the checks every image gets: the tables, the volume where the next boot finds
// it, the pinned FAT32 geometry, and cardfs writing to it.
static void format_and_check(uint32_t cluster_bytes, uint32_t n_fatent)
{
    const uint32_t cap = s_back_sectors;
    cardmkfs_step_t step;
    int fresult;
    TEST_ASSERT_EQUAL_HEX(ESP_OK,
                          cardmkfs_run(&s_io, s_drive, cap, 0x77617463, s_work, sizeof(s_work), &step, &fresult));
    TEST_ASSERT_EQUAL(CARDMKFS_STEP_DONE, step);

    uint8_t s[SS], want[SS];
    cardmkfs_layout_t layout;
    TEST_ASSERT_TRUE(cardmkfs_layout(cap, &layout));
    TEST_ASSERT_EQUAL_UINT32(cluster_bytes, layout.cluster_bytes);
    cardmkfs_mbr(&layout, 0x77617463, want);
    read_sector(0, s);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, s, SS);
    TEST_ASSERT_EQUAL_HEX8(0x0C, s[446 + 4]);
    TEST_ASSERT_EQUAL_UINT32(CARDMKFS_PART_LBA, le32(s + 446 + 8));
    TEST_ASSERT_EQUAL_UINT32(cap - CARDMKFS_PART_LBA, le32(s + 446 + 12));
    for (int i = 1; i < 4; i++) {
        TEST_ASSERT_EQUAL_HEX8(0, s[446 + 16 * i + 4]); // one entry only
    }
    static const uint8_t ZERO[SS];
    for (uint32_t lba = 1; lba < CARDMKFS_HEAD_SECTORS; lba++) {
        read_sector(lba, s);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(ZERO, s, SS); // the primary GPT is gone
    }
    for (uint32_t lba = cap - CARDMKFS_GPT_TAIL; lba < cap; lba++) {
        read_sector(lba, s);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(ZERO, s, SS); // and the backup
    }

    TEST_ASSERT_FALSE(boot_window()); // no GPT: the whole disk, and FatFs finds the partition in the MBR
    TEST_ASSERT_EQUAL(FR_OK, mount_fmt());
    TEST_ASSERT_EQUAL(FS_FAT32, s_fs->fs_type);
    TEST_ASSERT_EQUAL_UINT32(CARDMKFS_PART_LBA, s_fs->volbase); // once, not twice
    TEST_ASSERT_EQUAL_UINT32(cluster_bytes, (uint32_t)s_fs->csize * SS);
    TEST_ASSERT_EQUAL_UINT32(n_fatent, s_fs->n_fatent);
    TEST_ASSERT_EQUAL(2, s_fs->n_fats);
    TEST_ASSERT_EQUAL_UINT32(0, (s_fs->database - s_fs->volbase) % CARDMKFS_ALIGN);
    TEST_ASSERT_EQUAL_UINT32(0, s_fs->database % 8192); // 4 MiB aligned on the card, not only in the partition
    TEST_ASSERT_TRUE((uint64_t)s_fs->fsize * (SS / 4) >= s_fs->n_fatent); // the FAT holds every cluster

    TEST_ASSERT_EQUAL_HEX(ESP_OK, cardfs_init(FMT_ROOT, &s_vol));
    TEST_ASSERT_EQUAL_HEX(ESP_OK, cardfs_set_writable(true));
    uint64_t total = 0, free_bytes = 0;
    TEST_ASSERT_EQUAL_HEX(ESP_OK, cardfs_free_space(&total, &free_bytes));
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(n_fatent - 2) * cluster_bytes, total);
    TEST_ASSERT_EQUAL_UINT64(total - cluster_bytes, free_bytes); // the root directory's cluster
    const uint8_t runs[] = {2, 1};                               // watchdeck's long name, then wifi.txt
    TEST_ASSERT_EQUAL_HEX(ESP_OK, cardfs_fat_room("", runs, 2, s_drive));
    static const char WIFI[] = "Home\nsecret12\n";
    TEST_ASSERT_EQUAL_HEX(ESP_OK, cardfs_write_small("wifi.txt", WIFI, sizeof(WIFI) - 1));
    static const char SLEEP[] = "{\"t\":\"sleep\",\"offline_after_s\":10}";
    TEST_ASSERT_EQUAL_HEX(ESP_OK, cardfs_write_small("watchdeck/sleep.cfg", SLEEP, sizeof(SLEEP) - 1));
    TEST_ASSERT_EQUAL_HEX(ESP_OK, cardfs_recover());
    TEST_ASSERT_FALSE(cardfs_tainted());
    TEST_ASSERT_TRUE(file_is("wifi.txt", WIFI));
    TEST_ASSERT_TRUE(file_is("watchdeck/sleep.cfg", SLEEP));
    TEST_ASSERT_FALSE(file_is("old.txt", "the card's old contents"));
    cardfs_detach();
    unmount_fmt();
}

void test_format_layout_by_card_size(void)
{
    cardmkfs_layout_t l;
    TEST_ASSERT_FALSE(cardmkfs_layout(CARDMKFS_MIN_SECTORS - 1, &l));
    const struct {
        uint32_t capacity, cluster;
    } rows[] = {
        {CARDMKFS_MIN_SECTORS, 4096},
        {8 * GIB_SECTORS + CARDMKFS_PART_LBA - 1, 4096}, // the partition one sector under 8 GiB
        {8 * GIB_SECTORS + CARDMKFS_PART_LBA, 8192},
        {16 * GIB_SECTORS + CARDMKFS_PART_LBA - 1, 8192},
        {16 * GIB_SECTORS + CARDMKFS_PART_LBA, 16384},
        {32 * GIB_SECTORS + CARDMKFS_PART_LBA - 1, 16384},
        {32 * GIB_SECTORS + CARDMKFS_PART_LBA, 32768},
        {UINT32_MAX, 32768}, // 2 TiB, the 32-bit LBA limit
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        TEST_ASSERT_TRUE(cardmkfs_layout(rows[i].capacity, &l));
        TEST_ASSERT_EQUAL_UINT32(CARDMKFS_PART_LBA, l.part_lba);
        TEST_ASSERT_EQUAL_UINT32(rows[i].capacity - CARDMKFS_PART_LBA, l.part_sectors);
        TEST_ASSERT_EQUAL_UINT32(rows[i].cluster, l.cluster_bytes);
    }

    uint8_t mbr[SS];
    TEST_ASSERT_TRUE(cardmkfs_layout(CARDMKFS_MIN_SECTORS, &l));
    cardmkfs_mbr(&l, 0x12345678, mbr);
    // CHS (cylinder/head/sector) 0/130/3 (LBA 8192) to 65/69/4 (LBA 1048575); start 8192, 1040384 sectors.
    const uint8_t small[16] = {0x00, 0x82, 0x03, 0x00, 0x0C, 0x45, 0x04, 0x41,
                               0x00, 0x20, 0x00, 0x00, 0x00, 0xE0, 0x0F, 0x00};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(small, mbr + 446, 16);
    TEST_ASSERT_EQUAL_UINT32(0x12345678, le32(mbr + 440));
    TEST_ASSERT_EQUAL_HEX8(0x55, mbr[510]);
    TEST_ASSERT_EQUAL_HEX8(0xAA, mbr[511]);
    TEST_ASSERT_TRUE(cardmkfs_layout(128 * GIB_SECTORS, &l));
    cardmkfs_mbr(&l, 0, mbr);
    const uint8_t big[16] = {0x00, 0x82, 0x03, 0x00, 0x0C, 0xFE, 0xFF, 0xFF, // past cylinder 1023: LBA only
                             0x00, 0x20, 0x00, 0x00, 0x00, 0xE0, 0xFF, 0x0F};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(big, mbr + 446, 16);

    // Too small: refused before any write.
    ramdisk_create(CARDMKFS_MIN_SECTORS - 1, 1);
    use_disk(&ramdisk_impl, CARDMKFS_MIN_SECTORS - 1);
    ramdisk_reset_count();
    cardmkfs_step_t step;
    int fresult;
    TEST_ASSERT_EQUAL_HEX(ESP_ERR_INVALID_SIZE, cardmkfs_run(&s_io, s_drive, CARDMKFS_MIN_SECTORS - 1, 0, s_work,
                                                             sizeof(s_work), &step, &fresult));
    TEST_ASSERT_EQUAL(CARDMKFS_STEP_LAYOUT, step);
    TEST_ASSERT_EQUAL(0, ramdisk_writes());
}

// The user's card: 128 GiB, as SDXC cards ship, exFAT in an MBR partition, which IDF's FatFs can't mount (the board's
// NO_CARD state that Format card is for).
void test_format_128_gib_exfat_card(void)
{
    const uint32_t cap = 128 * GIB_SECTORS;
    img_open("128g", cap);
    use_disk(&s_img_impl, cap);
    uint8_t s[SS] = {0};
    s[446 + 4] = 0x07;
    put_le32(s + 446 + 8, 32768);
    put_le32(s + 446 + 12, cap - 32768);
    s[510] = 0x55;
    s[511] = 0xAA;
    write_sector(0, s);
    memset(s, 0, sizeof(s));
    memcpy(s,
           "\xEB\x76\x90"
           "EXFAT   ",
           11);
    s[510] = 0x55;
    s[511] = 0xAA;
    write_sector(32768, s);
    TEST_ASSERT_FALSE(boot_window());
    TEST_ASSERT_NOT_EQUAL(FR_OK, mount_fmt());
    unmount_fmt();

    // n_fatent from f_mkfs's FAT32 arithmetic (ff.c 6401-6443) worked by hand: 4193024 clusters, 2 FATs of 32768
    // sectors, 8192 reserved.
    format_and_check(32768, 4193026);
    img_close();
    check_with_tools("128g");
    img_remove("128g");
}

void test_format_8_gib_blank_card(void)
{
    const uint32_t cap = 8 * GIB_SECTORS;
    img_open("8g", cap);
    use_disk(&s_img_impl, cap);
    format_and_check(4096, 2091010); // FATs of 16377 sectors, 8206 reserved
    img_close();
    check_with_tools("8g");
    img_remove("8g");
}

// The card as it ships: GPT, a FAT32 partition from LBA 2048. Without the wipe the next boot would read the old
// header at LBA 1 and mount the old volume, which the new partition at 8192 no longer matches.
void test_format_gpt_card(void)
{
    const uint32_t cap = 16 * GIB_SECTORS;
    img_open("16g-gpt", cap);
    use_disk(&s_img_impl, cap);
    old_gpt_volume(SHIPPED_PART_LBA);
    format_and_check(8192, 2094082); // FATs of 16381 sectors, 8198 reserved
    img_close();
    check_with_tools("16g-gpt");
    img_remove("16g-gpt");
}

// A cut at any write of the format: the next boot finds the old volume whole (the format hadn't reached the GPT
// header yet), or no file system at all, never a mix; and once the MBR lands, the new volume.
void test_format_cut_at_every_write(void)
{
    const uint32_t cap = CARDMKFS_MIN_SECTORS;
    ramdisk_create(cap, 0xc0ffee); // unwritten sectors hold old bytes, as on a used card
    use_disk(&ramdisk_impl, cap);
    old_gpt_volume(SHIPPED_PART_LBA);
    ramdisk_snapshot();

    ramdisk_reset_count();
    cardmkfs_step_t step;
    int fresult;
    TEST_ASSERT_EQUAL_HEX(ESP_OK, cardmkfs_run(&s_io, s_drive, cap, 1, s_work, sizeof(s_work), &step, &fresult));
    const uint32_t writes = ramdisk_writes();
    uint32_t old = 0, none = 0;
    static const ramdisk_tear_t TEARS[] = {RAMDISK_WHOLE, RAMDISK_TEAR_HEAD, RAMDISK_TEAR_TAIL};
    for (size_t t = 0; t < sizeof(TEARS) / sizeof(TEARS[0]); t++) {
        for (uint32_t k = 0; k < writes; k++) {
            ramdisk_restore();
            ramdisk_cut_after(k, TEARS[t]);
            const esp_err_t err = cardmkfs_run(&s_io, s_drive, cap, 1, s_work, sizeof(s_work), &step, &fresult);
            ramdisk_power_on();
            const bool gpt = boot_window();
            const FRESULT res = mount_fmt();
            if (k == writes - 1 && TEARS[t] == RAMDISK_TEAR_TAIL) {
                // The MBR's last half is its table and signature, and its first half was already zero.
                TEST_ASSERT_FALSE(gpt);
                TEST_ASSERT_EQUAL(FR_OK, res);
                TEST_ASSERT_EQUAL_UINT32(CARDMKFS_PART_LBA, s_fs->volbase);
            } else {
                TEST_ASSERT_NOT_EQUAL(ESP_OK, err);
                if (res == FR_OK) {
                    TEST_ASSERT_TRUE_MESSAGE(gpt && file_is("old.txt", "the card's old contents"),
                                             "a cut left a volume that isn't the old one");
                    old++;
                } else {
                    TEST_ASSERT_EQUAL(FR_NO_FILESYSTEM, res);
                    none++;
                }
            }
            unmount_fmt();
        }
    }
    printf("format of a %lu-sector card: %lu sector writes, cut at each three ways: the old volume %lu times, no "
           "file system %lu times\n",
           (unsigned long)cap, (unsigned long)writes, (unsigned long)old, (unsigned long)none);
    TEST_ASSERT_TRUE(old > 0 && none > 0);

    ramdisk_restore();
    TEST_ASSERT_EQUAL_HEX(ESP_OK, cardmkfs_run(&s_io, s_drive, cap, 1, s_work, sizeof(s_work), &step, &fresult));
    TEST_ASSERT_FALSE(boot_window());
    TEST_ASSERT_EQUAL(FR_OK, mount_fmt());
    TEST_ASSERT_EQUAL(FS_FAT32, s_fs->fs_type);
    TEST_ASSERT_FALSE(file_is("old.txt", "the card's old contents"));
    unmount_fmt();
}
