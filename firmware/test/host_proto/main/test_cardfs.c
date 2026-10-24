// Host tests for cardfs.c on the host's own file system: the allowlist, the small-file write, the recovery logic
// and which new entries a call asks the room function about. host_fatfs runs the same code on FatFs, with the real
// room function, and cuts the power at every sector write.
#define _XOPEN_SOURCE 700
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cardfs.h"
#include "esp_rom_crc.h"
#include "psa/crypto.h"
#include "unity.h"

static char root[] = "/tmp/cfsXXXXXX";
static bool have_root;
static char s_room_log[256]; // "<dir>:<run>,<run>;" for every room call
static esp_err_t s_room_result;

static esp_err_t stub_room(const char *dir, const uint8_t *runs, size_t n, void *ctx)
{
    size_t len = strlen(s_room_log);
    len += snprintf(s_room_log + len, sizeof(s_room_log) - len, "%s:", dir);
    for (size_t i = 0; i < n; i++) {
        len += snprintf(s_room_log + len, sizeof(s_room_log) - len, "%s%u", i ? "," : "", (unsigned)runs[i]);
    }
    snprintf(s_room_log + len, sizeof(s_room_log) - len, ";");
    return s_room_result;
}

static int remove_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    return remove(path);
}

// A fresh, empty, writable root for each test, with room everywhere.
static void fresh_root(void)
{
    if (have_root) {
        nftw(root, remove_one, 8, FTW_DEPTH | FTW_PHYS);
        strcpy(root, "/tmp/cfsXXXXXX");
    }
    TEST_ASSERT_NOT_NULL(mkdtemp(root));
    have_root = true;
    s_room_log[0] = '\0';
    s_room_result = ESP_OK;
    static const cardfs_volume_t vol = {.room = stub_room};
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_init(root, &vol));
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_set_writable(true));
}

static void path_of(char *out, const char *rel)
{
    sprintf(out, "%s/%s", root, rel);
}

// The file's bytes as a string, or NULL when absent.
static char *slurp(const char *rel)
{
    static char buf[8192];
    char path[128];
    path_of(path, rel);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static void plant(const char *rel, const void *data, size_t len)
{
    char path[128];
    path_of(path, rel);
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL(len, fwrite(data, 1, len, f));
    fclose(f);
}

static void plant_str(const char *rel, const char *text)
{
    plant(rel, text, strlen(text));
}

static bool exists(const char *rel)
{
    char path[128];
    path_of(path, rel);
    struct stat st;
    return stat(path, &st) == 0;
}

static long size_of(const char *rel)
{
    char path[128];
    path_of(path, rel);
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void make_dir(const char *rel)
{
    char path[128];
    path_of(path, rel);
    TEST_ASSERT_EQUAL(0, mkdir(path, 0755));
}

// An intent record built from the documented layout, independently of cardfs.c.
static size_t make_record(uint8_t *out, const char *payload)
{
    const uint32_t len = strlen(payload);
    memcpy(out, "WDI1", 4);
    for (int i = 0; i < 4; i++) {
        out[4 + i] = (uint8_t)(len >> (8 * i));
        out[8 + i] = (uint8_t)(7 >> (8 * i));
    }
    size_t hash_len;
    TEST_ASSERT_EQUAL(PSA_SUCCESS,
                      psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)payload, len, out + 12, 32, &hash_len));
    const uint32_t crc = esp_rom_crc32_le(0, out, 44);
    for (int i = 0; i < 4; i++) {
        out[44 + i] = (uint8_t)(crc >> (8 * i));
    }
    memcpy(out + 48, payload, len);
    return 48 + len;
}

void test_cardfs_allowlist_passes_our_names(void)
{
    TEST_ASSERT_TRUE(cardfs_name_ok("wifi.txt"));
    TEST_ASSERT_TRUE(cardfs_name_ok("watchdeck/sleep.cfg"));
    TEST_ASSERT_TRUE(cardfs_name_ok("watchdeck/sl5b1f0c7e.img"));
    TEST_ASSERT_TRUE(cardfs_name_ok("watchdeck/a_b-c.9"));
    TEST_ASSERT_TRUE(cardfs_name_ok("watchdeck/noext"));
    TEST_ASSERT_TRUE(cardfs_name_ok("watchdeck/selftest.json")); // a long name is allowed, it just takes more entries
    char name[80] = "watchdeck/";
    memset(name + 10, 'a', 63);
    name[73] = '\0';
    TEST_ASSERT_TRUE(cardfs_name_ok(name)); // 63 characters
}

void test_cardfs_allowlist_refuses_everything_else(void)
{
    TEST_ASSERT_FALSE(cardfs_name_ok("WIFI.TXT"));
    TEST_ASSERT_FALSE(cardfs_name_ok("Wifi.txt"));
    TEST_ASSERT_FALSE(cardfs_name_ok("watchdeck/Sleep.cfg"));
    TEST_ASSERT_FALSE(cardfs_name_ok("watchdeck/../wifi.txt"));
    TEST_ASSERT_FALSE(cardfs_name_ok("/wifi.txt"));
    TEST_ASSERT_FALSE(cardfs_name_ok("watchdeck/.x"));
    TEST_ASSERT_FALSE(cardfs_name_ok("watchdeck/a/b"));
    TEST_ASSERT_FALSE(cardfs_name_ok("other.txt"));
    TEST_ASSERT_FALSE(cardfs_name_ok("watchdeck"));
    TEST_ASSERT_FALSE(cardfs_name_ok("watchdeck/"));
    TEST_ASSERT_FALSE(cardfs_name_ok("watchdeck/x."));        // FatFs would drop the dot
    TEST_ASSERT_FALSE(cardfs_name_ok("watchdeck/wifi.new"));  // an intent record
    TEST_ASSERT_FALSE(cardfs_name_ok("watchdeck/sleep.new")); // an intent record
    TEST_ASSERT_FALSE(cardfs_name_ok("watchdeck/a b"));
    char name[80] = "watchdeck/";
    memset(name + 10, 'a', 64);
    name[74] = '\0';
    TEST_ASSERT_FALSE(cardfs_name_ok(name)); // 64 characters
}

void test_cardfs_write_small_creates_a_new_file_and_leaves_no_record(void)
{
    fresh_root();
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_small("wifi.txt", "Home\nsecret12\n", 14));
    TEST_ASSERT_EQUAL_STRING("Home\nsecret12\n", slurp("wifi.txt"));
    TEST_ASSERT_EQUAL_STRING("", slurp("watchdeck/wifi.new")); // emptied, not deleted
}

void test_cardfs_write_small_replaces_a_file(void)
{
    fresh_root();
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_small("watchdeck/sleep.cfg", "{\"a\":1111111111}", 16));
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_small("watchdeck/sleep.cfg", "{}", 2));
    TEST_ASSERT_EQUAL_STRING("{}", slurp("watchdeck/sleep.cfg"));
    TEST_ASSERT_EQUAL_STRING("", slurp("watchdeck/sleep.new"));
#if CONFIG_WATCHDECK_TEST_CARD // set for this test app (CMakeLists.txt), as in the self-test firmware
    // The card self-test's small file has its own record.
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_small("watchdeck/selftest.cfg", "selftest 1\n", 11));
    TEST_ASSERT_EQUAL_STRING("selftest 1\n", slurp("watchdeck/selftest.cfg"));
    TEST_ASSERT_EQUAL_STRING("", slurp("watchdeck/selftest.new"));
#else
#error "host_proto builds cardfs.c with CONFIG_WATCHDECK_TEST_CARD so the self-test row is covered"
#endif
}

void test_cardfs_write_small_refuses_other_names_and_sizes(void)
{
    fresh_root();
    static uint8_t big[CARDFS_SMALL_MAX + 1];
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cardfs_write_small("watchdeck/x.cfg", "{}", 2));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cardfs_write_small("WIFI.TXT", "x\n", 2));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, cardfs_write_small("wifi.txt", big, sizeof(big)));
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_small("wifi.txt", big, CARDFS_SMALL_MAX));
}

void test_cardfs_write_while_not_writable_fails_and_keeps_the_old_bytes(void)
{
    fresh_root();
    plant_str("wifi.txt", "Old\n");
    cardfs_set_writable(false);
    TEST_ASSERT_FALSE(cardfs_writable());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_write_small("wifi.txt", "New\n", 4));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_write_new("watchdeck/a.bin", "x", 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_remove("wifi.txt"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_recover());
    TEST_ASSERT_EQUAL_STRING("Old\n", slurp("wifi.txt"));
    TEST_ASSERT_FALSE(exists("watchdeck"));
}

void test_cardfs_write_whose_record_cannot_be_created_keeps_the_old_file(void)
{
    fresh_root();
    plant_str("wifi.txt", "Old\n");
    make_dir("watchdeck");
    make_dir("watchdeck/wifi.new"); // a directory where the record goes
    TEST_ASSERT_EQUAL(ESP_FAIL, cardfs_write_small("wifi.txt", "New\n", 4));
    TEST_ASSERT_EQUAL_STRING("Old\n", slurp("wifi.txt"));
}

// The failed read of that record taints cardfs: nothing writes until writes are turned off and on (the remount).
void test_cardfs_failed_access_stops_writes_until_writes_are_turned_off_and_on(void)
{
    fresh_root();
    make_dir("watchdeck");
    make_dir("watchdeck/wifi.new");
    TEST_ASSERT_FALSE(cardfs_tainted());
    TEST_ASSERT_EQUAL(ESP_FAIL, cardfs_write_small("wifi.txt", "New\n", 4));
    TEST_ASSERT_TRUE(cardfs_tainted());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_write_small("watchdeck/sleep.cfg", "{}", 2));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_write_new("watchdeck/a.bin", "x", 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_recover());
    plant_str("watchdeck/b.bin", "y");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_remove("watchdeck/b.bin"));
    char buf[4];
    size_t len;
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_read("watchdeck/b.bin", buf, sizeof(buf), &len)); // reads go on
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_set_writable(true));                              // not enough on its own
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_write_small("watchdeck/sleep.cfg", "{}", 2));
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_set_writable(false));
    TEST_ASSERT_FALSE(cardfs_tainted());
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_set_writable(true));
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_small("watchdeck/sleep.cfg", "{}", 2));
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_remove("watchdeck/b.bin"));
}

static void tally_entry(const char *name, size_t size, bool is_dir, void *ctx)
{
    (*(int *)ctx)++;
}

// Detached (shared, or between an unmount and a mount) every call that touches the card says INVALID_STATE, never
// NOT_FOUND, so a caller can tell "the card isn't there" from "the file is absent".
void test_cardfs_detached_refuses_reads_and_writes_until_attached(void)
{
    fresh_root();
    plant_str("wifi.txt", "Home\n");
    make_dir("watchdeck");
    cardfs_detach();
    TEST_ASSERT_FALSE(cardfs_writable());
    char buf[16];
    size_t len;
    const uint8_t digest[1] = {0};
    int n = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_read("wifi.txt", buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_read("watchdeck/none.cfg", buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_verify("wifi.txt", 5, digest, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_list(tally_entry, &n));
    TEST_ASSERT_EQUAL(0, n);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_write_small("wifi.txt", "New\n", 4));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_write_new("watchdeck/a.bin", "x", 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_remove("wifi.txt"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_recover());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_set_writable(true));
    TEST_ASSERT_EQUAL_STRING("Home\n", slurp("wifi.txt"));

    TEST_ASSERT_EQUAL(ESP_OK, cardfs_attach());
    TEST_ASSERT_FALSE(cardfs_writable()); // writes wait for set_writable(true)
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_read("wifi.txt", buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, cardfs_read("watchdeck/none.cfg", buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_write_small("wifi.txt", "New\n", 4));
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_set_writable(true));
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_small("wifi.txt", "New\n", 4));
    TEST_ASSERT_EQUAL_STRING("New\n", slurp("wifi.txt"));
}

static esp_err_t s_space_result;

static esp_err_t stub_space(uint64_t *total, uint64_t *free_bytes, void *ctx)
{
    *total = 1000;
    *free_bytes = 400;
    return s_space_result;
}

void test_cardfs_free_space_asks_the_volume_and_refuses_when_detached_or_unsupported(void)
{
    fresh_root();
    uint64_t total = 0, free_bytes = 0;
    // fresh_root's volume has no space function.
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, cardfs_free_space(&total, &free_bytes));
    TEST_ASSERT_FALSE(cardfs_tainted());

    const cardfs_volume_t vol = {.room = stub_room, .space = stub_space};
    s_space_result = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_init(root, &vol));
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_free_space(&total, &free_bytes));
    TEST_ASSERT_EQUAL_UINT64(1000, total);
    TEST_ASSERT_EQUAL_UINT64(400, free_bytes);

    // A read error from the count is a failed card access.
    s_space_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, cardfs_free_space(&total, &free_bytes));
    TEST_ASSERT_TRUE(cardfs_tainted());

    // Detached, the volume isn't asked at all.
    cardfs_detach();
    total = free_bytes = 0;
    s_space_result = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_free_space(&total, &free_bytes));
    TEST_ASSERT_EQUAL_UINT64(0, total);
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_attach());
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_free_space(&total, &free_bytes));
    TEST_ASSERT_EQUAL_UINT64(1000, total);
}

// A detach also clears a taint: the remount it brackets is what the taint asks for.
void test_cardfs_detach_and_attach_clear_a_taint(void)
{
    fresh_root();
    make_dir("watchdeck");
    make_dir("watchdeck/wifi.new");
    TEST_ASSERT_EQUAL(ESP_FAIL, cardfs_write_small("wifi.txt", "New\n", 4));
    TEST_ASSERT_TRUE(cardfs_tainted());
    cardfs_detach();
    TEST_ASSERT_FALSE(cardfs_tainted());
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_attach());
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_set_writable(true));
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_small("watchdeck/sleep.cfg", "{}", 2));
}

static int s_prepare_calls;
static esp_err_t s_prepare_result;

static esp_err_t stub_prepare(void *ctx)
{
    s_prepare_calls++;
    return s_prepare_result;
}

void test_cardfs_init_needs_a_volume_and_runs_its_prepare(void)
{
    const cardfs_volume_t no_room = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cardfs_init("/tmp", NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cardfs_init("/tmp", &no_room));
    const cardfs_volume_t vol = {.prepare = stub_prepare, .room = stub_room};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cardfs_init("/a/root/too/long", &vol));

    fresh_root();
    s_prepare_calls = 0;
    s_prepare_result = ESP_ERR_NOT_SUPPORTED; // a volume cardfs can't write, e.g. not FAT32
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, cardfs_init(root, &vol));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, cardfs_set_writable(true));
    TEST_ASSERT_FALSE(cardfs_writable());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cardfs_write_small("wifi.txt", "x\n", 2));
    s_prepare_result = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_set_writable(true)); // after a remount
    TEST_ASSERT_TRUE(cardfs_writable());
    TEST_ASSERT_EQUAL(3, s_prepare_calls);
}

// A hash that fails is not a torn record: the record stays, and nothing reports success.
void test_cardfs_record_that_cannot_be_hashed_is_kept(void)
{
    fresh_root();
    plant_str("wifi.txt", "Old\n");
    make_dir("watchdeck");
    uint8_t rec[128];
    const size_t rec_len = make_record(rec, "New\n");
    plant("watchdeck/wifi.new", rec, rec_len);
    cardfs_test_fail_sha256(0);
    TEST_ASSERT_EQUAL(ESP_FAIL, cardfs_recover());
    TEST_ASSERT_EQUAL(rec_len, size_of("watchdeck/wifi.new"));
    TEST_ASSERT_EQUAL_STRING("Old\n", slurp("wifi.txt"));
    cardfs_test_fail_sha256(0);
    TEST_ASSERT_EQUAL(ESP_FAIL, cardfs_write_small("wifi.txt", "Newer\n", 6)); // its pending record can't be checked
    TEST_ASSERT_EQUAL(rec_len, size_of("watchdeck/wifi.new"));
    TEST_ASSERT_EQUAL_STRING("Old\n", slurp("wifi.txt"));
    cardfs_test_fail_sha256(-1);
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_recover());
    TEST_ASSERT_EQUAL_STRING("New\n", slurp("wifi.txt"));
    TEST_ASSERT_EQUAL_STRING("", slurp("watchdeck/wifi.new"));
}

// The runs follow the order of the creates: watchdeck/ (a long name: 2 entries) before wifi.txt, the record
// before the file. A file that exists, and anything inside a watchdeck/ this call creates, needs no room. The
// 8.3 names take one entry, sl0a1b2c3d.img (14 characters, a 10-character body) three.
void test_cardfs_room_is_asked_for_each_new_entry_in_order(void)
{
    fresh_root();
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_small("wifi.txt", "Home\n", 5));
    TEST_ASSERT_EQUAL_STRING(":2,1;", s_room_log);
    s_room_log[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_small("wifi.txt", "Away\n", 5));
    TEST_ASSERT_EQUAL_STRING("", s_room_log);
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_small("watchdeck/sleep.cfg", "{}", 2));
    TEST_ASSERT_EQUAL_STRING("watchdeck:1,1;", s_room_log);
    s_room_log[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_new("watchdeck/sl0a1b2c3d.img", "abc", 3));
    TEST_ASSERT_EQUAL_STRING("watchdeck:3;", s_room_log);
    s_room_log[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_new("watchdeck/0a1b2c3d.img", "abc", 3));
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_new("watchdeck/a.b.c", "abc", 3)); // two dots: a long name
    TEST_ASSERT_EQUAL_STRING("watchdeck:1;watchdeck:2;", s_room_log);

    fresh_root();
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_new("watchdeck/sl0a1b2c3d.img", "abc", 3));
    TEST_ASSERT_EQUAL_STRING(":2;", s_room_log);
}

void test_cardfs_full_directory_refuses_before_writing_anything(void)
{
    fresh_root();
    s_room_result = CARDFS_ERR_DIR_FULL;
    TEST_ASSERT_EQUAL(CARDFS_ERR_DIR_FULL, cardfs_write_small("wifi.txt", "Home\n", 5));
    TEST_ASSERT_EQUAL(CARDFS_ERR_DIR_FULL, cardfs_write_new("watchdeck/a.bin", "x", 1));
    TEST_ASSERT_FALSE(exists("watchdeck"));
    TEST_ASSERT_FALSE(exists("wifi.txt"));

    make_dir("watchdeck");
    plant_str("wifi.txt", "Old\n");
    TEST_ASSERT_EQUAL(CARDFS_ERR_DIR_FULL, cardfs_write_small("wifi.txt", "New\n", 4)); // the record is new
    TEST_ASSERT_EQUAL_STRING("Old\n", slurp("wifi.txt"));
    TEST_ASSERT_FALSE(exists("watchdeck/wifi.new"));
    s_room_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, cardfs_write_new("watchdeck/a.bin", "x", 1));
    TEST_ASSERT_FALSE(exists("watchdeck/a.bin"));
}

// A record an earlier write left is applied before a new write to the same key, and while it can't be, the new
// write is refused and the record kept.
void test_cardfs_pending_record_is_applied_before_a_new_write_and_never_dropped(void)
{
    fresh_root();
    make_dir("watchdeck");
    uint8_t rec[128];
    const size_t rec_len = make_record(rec, "New\n");
    plant("watchdeck/wifi.new", rec, rec_len); // wifi.txt is absent, so the redo must create it
    s_room_result = CARDFS_ERR_DIR_FULL;
    TEST_ASSERT_EQUAL(CARDFS_ERR_DIR_FULL, cardfs_write_small("wifi.txt", "Newer\n", 6));
    TEST_ASSERT_EQUAL(rec_len, size_of("watchdeck/wifi.new"));
    TEST_ASSERT_EQUAL(ESP_FAIL, cardfs_recover());
    TEST_ASSERT_EQUAL(rec_len, size_of("watchdeck/wifi.new"));
    TEST_ASSERT_FALSE(exists("wifi.txt"));

    s_room_result = ESP_OK;
    plant_str("wifi.txt", "Ne"); // torn
    s_room_log[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_small("wifi.txt", "Newer\n", 6));
    TEST_ASSERT_EQUAL_STRING("Newer\n", slurp("wifi.txt"));
    TEST_ASSERT_EQUAL_STRING("", slurp("watchdeck/wifi.new"));
    TEST_ASSERT_EQUAL_STRING("", s_room_log);
}

void test_cardfs_recover_with_no_record_touches_nothing(void)
{
    fresh_root();
    plant_str("wifi.txt", "Old\n");
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_recover());
    TEST_ASSERT_EQUAL_STRING("Old\n", slurp("wifi.txt"));
    TEST_ASSERT_FALSE(exists("watchdeck"));
}

void test_cardfs_recover_with_an_empty_record_touches_nothing(void)
{
    fresh_root();
    plant_str("wifi.txt", "Old\n");
    make_dir("watchdeck");
    plant("watchdeck/wifi.new", "", 0);
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_recover());
    TEST_ASSERT_EQUAL_STRING("Old\n", slurp("wifi.txt"));
    TEST_ASSERT_EQUAL_STRING("", slurp("watchdeck/wifi.new"));
    TEST_ASSERT_FALSE(exists("watchdeck/sleep.new")); // an absent record stays absent
}

// Plants wifi.txt "Old\n" and a record for "New\n" damaged by damage(), then recovers.
static void recover_damaged(void (*damage)(uint8_t *rec, size_t *len))
{
    fresh_root();
    plant_str("wifi.txt", "Old\n");
    make_dir("watchdeck");
    uint8_t rec[128];
    size_t len = make_record(rec, "New\n");
    damage(rec, &len);
    plant("watchdeck/wifi.new", rec, len);
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_recover());
    TEST_ASSERT_EQUAL_STRING("Old\n", slurp("wifi.txt"));
    TEST_ASSERT_EQUAL_STRING("", slurp("watchdeck/wifi.new"));
}

static void bad_magic(uint8_t *rec, size_t *len)
{
    rec[0] = 'X';
}

static void bad_header_crc(uint8_t *rec, size_t *len)
{
    rec[8] ^= 1; // the generation, which only the CRC covers
}

static void short_payload(uint8_t *rec, size_t *len)
{
    *len -= 1;
}

static void long_payload(uint8_t *rec, size_t *len)
{
    rec[(*len)++] = 'x';
}

static void short_header(uint8_t *rec, size_t *len)
{
    *len = 30;
}

static void bad_sha(uint8_t *rec, size_t *len)
{
    rec[48] = 'n'; // the payload, which only the SHA-256 covers
}

void test_cardfs_recover_clears_a_record_with_a_bad_magic(void)
{
    recover_damaged(bad_magic);
}

void test_cardfs_recover_clears_a_record_with_a_bad_header_crc(void)
{
    recover_damaged(bad_header_crc);
}

void test_cardfs_recover_clears_a_record_with_a_short_or_long_payload(void)
{
    recover_damaged(short_payload);
    recover_damaged(long_payload);
    recover_damaged(short_header);
}

void test_cardfs_recover_clears_a_record_with_a_bad_sha256(void)
{
    recover_damaged(bad_sha);
}

// Plants a good record for "New\n" with wifi.txt as given (NULL: absent), recovers twice.
static void recover_good(const char *current)
{
    fresh_root();
    if (current) {
        plant_str("wifi.txt", current);
    }
    make_dir("watchdeck");
    uint8_t rec[128];
    plant("watchdeck/wifi.new", rec, make_record(rec, "New\n"));
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_recover());
    TEST_ASSERT_EQUAL_STRING("New\n", slurp("wifi.txt"));
    TEST_ASSERT_EQUAL_STRING("", slurp("watchdeck/wifi.new"));
    TEST_ASSERT_EQUAL_STRING(current ? "" : ":1;", s_room_log); // only a missing wifi.txt needs a new entry
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_recover());
    TEST_ASSERT_EQUAL_STRING("New\n", slurp("wifi.txt"));
}

void test_cardfs_recover_applies_a_good_record_over_old_torn_missing_or_new(void)
{
    recover_good("Old\n");
    recover_good("Ne");
    recover_good("");
    recover_good(NULL);
    recover_good("New\n");
    recover_good("New\nand more\n");
}

void test_cardfs_recover_handles_both_records_at_once(void)
{
    fresh_root();
    plant_str("wifi.txt", "Old\n");
    make_dir("watchdeck");
    plant_str("watchdeck/sleep.cfg", "{\"old\":1}");
    uint8_t rec[128];
    plant("watchdeck/wifi.new", rec, make_record(rec, "New\n"));
    size_t len = make_record(rec, "{}");
    rec[0] = 'X';
    plant("watchdeck/sleep.new", rec, len);
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_recover());
    TEST_ASSERT_EQUAL_STRING("New\n", slurp("wifi.txt"));
    TEST_ASSERT_EQUAL_STRING("{\"old\":1}", slurp("watchdeck/sleep.cfg"));
    TEST_ASSERT_EQUAL_STRING("", slurp("watchdeck/wifi.new"));
    TEST_ASSERT_EQUAL_STRING("", slurp("watchdeck/sleep.new"));
}

void test_cardfs_write_new_is_write_once_and_verify_checks_size_and_hash(void)
{
    fresh_root();
    static uint8_t img[100000];
    for (size_t i = 0; i < sizeof(img); i++) {
        img[i] = (uint8_t)(i * 7);
    }
    uint8_t sha[32];
    size_t sha_len;
    TEST_ASSERT_EQUAL(PSA_SUCCESS, psa_hash_compute(PSA_ALG_SHA_256, img, sizeof(img), sha, 32, &sha_len));
    const char *rel = "watchdeck/sl012345.img";

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, cardfs_verify(rel, sizeof(img), sha, 16));
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_new(rel, img, sizeof(img)));
    s_room_log[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED, cardfs_write_new(rel, img, 10));
    TEST_ASSERT_EQUAL_STRING("", s_room_log);
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_verify(rel, sizeof(img), sha, 16));
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_verify(rel, sizeof(img), sha, 32));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, cardfs_verify(rel, sizeof(img) - 1, sha, 16));
    sha[3] ^= 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, cardfs_verify(rel, sizeof(img), sha, 16));
    sha[3] ^= 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cardfs_verify(rel, sizeof(img), sha, 33));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cardfs_write_new("wifi.txt", "x\n", 2));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cardfs_write_new("watchdeck/sleep.cfg", "{}", 2));

    // A flipped byte in the middle: same size, wrong hash.
    char path[128];
    path_of(path, rel);
    FILE *f = fopen(path, "r+b");
    fseek(f, 50000, SEEK_SET);
    fputc(img[50000] ^ 0xff, f);
    fclose(f);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, cardfs_verify(rel, sizeof(img), sha, 16));
}

void test_cardfs_read_remove_and_list(void)
{
    fresh_root();
    char buf[16];
    size_t len;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, cardfs_read("wifi.txt", buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, cardfs_list(NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_small("wifi.txt", "Home\n", 5));
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_read("wifi.txt", buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(5, len);
    TEST_ASSERT_EQUAL_MEMORY("Home\n", buf, 5);
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_read("wifi.txt", buf, 5, &len));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, cardfs_read("wifi.txt", buf, 4, &len));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cardfs_read("other.txt", buf, sizeof(buf), &len));

    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_new("watchdeck/a.bin", "abc", 3));
    plant_str("watchdeck/Photo.JPG", "jpeg"); // a person's file
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cardfs_remove("watchdeck/Photo.JPG"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cardfs_remove("watchdeck/wifi.new"));
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_remove("wifi.txt"));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, cardfs_remove("wifi.txt"));
    TEST_ASSERT_TRUE(exists("watchdeck/Photo.JPG"));
}

// Records each entry, and reads it back through cardfs: the callback runs outside the mutex.
static void count_entry(const char *name, size_t size, bool is_dir, void *ctx)
{
    char *seen = ctx;
    char line[96];
    snprintf(line, sizeof(line), "%s:%u:%d;", name, (unsigned)size, is_dir);
    strcat(seen, line);
    char rel[64];
    snprintf(rel, sizeof(rel), "watchdeck/%s", name);
    char buf[8];
    size_t len;
    if (!is_dir && cardfs_name_ok(rel)) {
        TEST_ASSERT_EQUAL(ESP_OK, cardfs_read(rel, buf, sizeof(buf), &len));
        TEST_ASSERT_EQUAL(size, len);
    }
}

void test_cardfs_list_shows_every_entry_of_watchdeck(void)
{
    fresh_root();
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_new("watchdeck/a.bin", "abc", 3));
    char seen[256] = "";
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_list(count_entry, seen));
    TEST_ASSERT_EQUAL_STRING("a.bin:3:0;", seen);
    make_dir("watchdeck/sub");
    seen[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_list(count_entry, seen));
    TEST_ASSERT_NOT_NULL(strstr(seen, "a.bin:3:0;"));
    TEST_ASSERT_NOT_NULL(strstr(seen, "sub:"));
}

// An entry that can't be stat'ed (here a dangling link) fails the whole listing, with no callback at all.
void test_cardfs_list_reports_a_stat_error_as_an_error(void)
{
    fresh_root();
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_new("watchdeck/a.bin", "abc", 3));
    char path[128];
    path_of(path, "watchdeck/gone");
    TEST_ASSERT_EQUAL(0, symlink("/nonexistent/target", path));
    char seen[256] = "";
    TEST_ASSERT_EQUAL(ESP_FAIL, cardfs_list(count_entry, seen));
    TEST_ASSERT_EQUAL_STRING("", seen);
}

void test_cardfs_run(void)
{
    RUN_TEST(test_cardfs_allowlist_passes_our_names);
    RUN_TEST(test_cardfs_allowlist_refuses_everything_else);
    RUN_TEST(test_cardfs_write_small_creates_a_new_file_and_leaves_no_record);
    RUN_TEST(test_cardfs_write_small_replaces_a_file);
    RUN_TEST(test_cardfs_write_small_refuses_other_names_and_sizes);
    RUN_TEST(test_cardfs_write_while_not_writable_fails_and_keeps_the_old_bytes);
    RUN_TEST(test_cardfs_write_whose_record_cannot_be_created_keeps_the_old_file);
    RUN_TEST(test_cardfs_failed_access_stops_writes_until_writes_are_turned_off_and_on);
    RUN_TEST(test_cardfs_detached_refuses_reads_and_writes_until_attached);
    RUN_TEST(test_cardfs_free_space_asks_the_volume_and_refuses_when_detached_or_unsupported);
    RUN_TEST(test_cardfs_detach_and_attach_clear_a_taint);
    RUN_TEST(test_cardfs_init_needs_a_volume_and_runs_its_prepare);
    RUN_TEST(test_cardfs_record_that_cannot_be_hashed_is_kept);
    RUN_TEST(test_cardfs_room_is_asked_for_each_new_entry_in_order);
    RUN_TEST(test_cardfs_full_directory_refuses_before_writing_anything);
    RUN_TEST(test_cardfs_pending_record_is_applied_before_a_new_write_and_never_dropped);
    RUN_TEST(test_cardfs_recover_with_no_record_touches_nothing);
    RUN_TEST(test_cardfs_recover_with_an_empty_record_touches_nothing);
    RUN_TEST(test_cardfs_recover_clears_a_record_with_a_bad_magic);
    RUN_TEST(test_cardfs_recover_clears_a_record_with_a_bad_header_crc);
    RUN_TEST(test_cardfs_recover_clears_a_record_with_a_short_or_long_payload);
    RUN_TEST(test_cardfs_recover_clears_a_record_with_a_bad_sha256);
    RUN_TEST(test_cardfs_recover_applies_a_good_record_over_old_torn_missing_or_new);
    RUN_TEST(test_cardfs_recover_handles_both_records_at_once);
    RUN_TEST(test_cardfs_write_new_is_write_once_and_verify_checks_size_and_hash);
    RUN_TEST(test_cardfs_read_remove_and_list);
    RUN_TEST(test_cardfs_list_shows_every_entry_of_watchdeck);
    RUN_TEST(test_cardfs_list_reports_a_stat_error_as_an_error);
    if (have_root) {
        nftw(root, remove_one, 8, FTW_DEPTH | FTW_PHYS);
    }
}
