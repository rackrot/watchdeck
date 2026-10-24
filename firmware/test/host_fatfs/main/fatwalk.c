#include "fatwalk.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ramdisk.h"

#define EOC_MIN 0x0FFFFFF8u
#define BAD_CLUSTER 0x0FFFFFF7u
#define DIR_CLUSTERS_MAX 256
#define DEPTH_MAX 8

static struct {
    uint32_t spc, fatbase, fatsz, nfats, database, n_fatent;
    uint8_t *seen;
    fatwalk_t *out;
} g;

static uint16_t le16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t fat_entry(uint32_t c)
{
    const uint32_t off = c * 4;
    return le32(ramdisk_sector(g.fatbase + off / RAMDISK_SECTOR) + off % RAMDISK_SECTOR) & 0x0FFFFFFF;
}

static void problem(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void problem(const char *fmt, ...)
{
    if (g.out->problem[0]) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(g.out->problem, sizeof(g.out->problem), fmt, args);
    va_end(args);
}

// Follows the chain from start and marks its clusters, keeping up to cap of them in list. Returns its length, or
// 0 after a problem.
static uint32_t follow(uint32_t start, const char *what, uint32_t *list, uint32_t cap)
{
    uint32_t n = 0;
    for (uint32_t c = start;;) {
        if (c < 2 || c >= g.n_fatent) {
            problem("%s: chain leaves the volume at %lu", what, (unsigned long)c);
            return 0;
        }
        if (g.seen[c]) {
            problem("%s: cluster %lu is in two chains, or twice in one", what, (unsigned long)c);
            return 0;
        }
        g.seen[c] = 1;
        if (list) {
            if (n >= cap) {
                problem("%s: directory over %u clusters", what, (unsigned)cap);
                return 0;
            }
            list[n] = c;
        }
        n++;
        const uint32_t next = fat_entry(c);
        if (next == 0) {
            problem("%s: cluster %lu of the chain is free", what, (unsigned long)c);
            return 0;
        }
        if (next == BAD_CLUSTER) {
            problem("%s: cluster %lu of the chain is marked bad", what, (unsigned long)c);
            return 0;
        }
        if (next >= EOC_MIN) {
            return n;
        }
        c = next;
    }
}

static void short_name(const uint8_t *d, char out[13])
{
    size_t n = 0;
    for (int i = 0; i < 8 && d[i] != ' '; i++) {
        out[n++] = (char)(i == 0 && d[i] == 0x05 ? 0xE5 : d[i]);
    }
    if (d[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && d[i] != ' '; i++) {
            out[n++] = (char)d[i];
        }
    }
    out[n] = '\0';
}

static uint8_t sfn_sum(const uint8_t *d)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + d[i]);
    }
    return sum;
}

// A long-name sequence being read: its parts so far, the order number the next one needs and the checksum.
typedef struct {
    uint32_t parts;
    uint8_t next_ord, sum;
} lfn_t;

static void lfn_drop(lfn_t *l)
{
    g.out->orphan_lfn += l->parts;
    l->parts = 0;
}

// A long-name part: starts a sequence (the 0x40 flag) or continues it, else it's an orphan.
static void lfn_part(lfn_t *l, const uint8_t *d)
{
    const uint8_t ord = d[0] & 0x3F;
    if (d[0] & 0x40) {
        lfn_drop(l);
        if (ord == 0) {
            g.out->orphan_lfn++;
            return;
        }
        *l = (lfn_t){.parts = 1, .next_ord = (uint8_t)(ord - 1), .sum = d[13]};
    } else if (l->parts > 0 && ord == l->next_ord && ord > 0 && d[13] == l->sum) {
        l->parts++;
        l->next_ord--;
    } else {
        lfn_drop(l);
        g.out->orphan_lfn++;
    }
}

// The 8.3 entry after a sequence: it claims the sequence when the order numbers ran out and the checksum is its own.
static void lfn_end(lfn_t *l, const uint8_t *d)
{
    if (l->parts > 0 && (l->next_ord != 0 || l->sum != sfn_sum(d))) {
        lfn_drop(l);
    }
    l->parts = 0;
}

static bool walk_dir(uint32_t start, const char *path, int depth)
{
    uint32_t clusters[DIR_CLUSTERS_MAX];
    const uint32_t n = follow(start, path[0] ? path : "/", clusters, DIR_CLUSTERS_MAX);
    if (n == 0) {
        return false;
    }
    g.out->dirs++;
    g.out->dir_clusters += n;
    const uint32_t cluster_bytes = g.spc * RAMDISK_SECTOR;
    bool ended = false; // past the end marker, where every byte should be zero
    lfn_t lfn = {0};
    for (uint32_t i = 0; i < n; i++) {
        bool hidden = false;
        for (uint32_t s = 0; s < g.spc; s++) {
            const uint8_t *sector = ramdisk_sector(g.database + (clusters[i] - 2) * g.spc + s);
            for (int e = 0; e < RAMDISK_SECTOR / 32; e++) {
                const uint8_t *d = sector + e * 32;
                if (!ended && d[0] == 0x00) {
                    ended = true;
                    lfn_drop(&lfn);
                }
                if (ended) {
                    for (int b = 0; b < 32 && !hidden; b++) {
                        hidden = d[b] != 0;
                    }
                    continue;
                }
                const uint8_t attr = d[11];
                if (d[0] == 0xE5) {
                    lfn_drop(&lfn);
                    continue; // deleted
                }
                if ((attr & 0x3F) == 0x0F) {
                    lfn_part(&lfn, d);
                    continue;
                }
                lfn_end(&lfn, d);
                if ((attr & 0x08) || d[0] == '.') {
                    continue; // the volume label, or . and ..
                }
                char name[13];
                short_name(d, name);
                char sub[160];
                snprintf(sub, sizeof(sub), "%s/%s", path, name);
                const uint32_t cl = (uint32_t)le16(d + 20) << 16 | le16(d + 26);
                const uint32_t size = le32(d + 28);
                if (attr & 0x10) {
                    if (cl == 0 || depth >= DEPTH_MAX) {
                        problem("%s: directory with cluster %lu at depth %d", sub, (unsigned long)cl, depth);
                        return false;
                    }
                    if (!walk_dir(cl, sub, depth + 1)) {
                        return false;
                    }
                    continue;
                }
                g.out->files++;
                const uint32_t want = (size + cluster_bytes - 1) / cluster_bytes;
                const uint32_t got = cl == 0 ? 0 : follow(cl, sub, NULL, 0);
                if (cl != 0 && got == 0) {
                    return false;
                }
                if (got != want) {
                    problem("%s: %lu bytes need %lu clusters, the chain has %lu", sub, (unsigned long)size,
                            (unsigned long)want, (unsigned long)got);
                    return false;
                }
            }
        }
        g.out->hidden_dir_clusters += hidden;
    }
    lfn_drop(&lfn);
    return true;
}

bool fatwalk(fatwalk_t *out)
{
    memset(out, 0, sizeof(*out));
    g.out = out;
    const uint8_t *bs = ramdisk_sector(0);
    if (le16(bs + 11) != RAMDISK_SECTOR || le16(bs + 510) != 0xAA55 || le16(bs + 22) != 0) {
        problem("not a FAT32 volume with %u-byte sectors", RAMDISK_SECTOR);
        return false;
    }
    g.spc = bs[13];
    g.fatbase = le16(bs + 14);
    g.nfats = bs[16];
    g.fatsz = le32(bs + 36);
    g.database = g.fatbase + g.nfats * g.fatsz;
    g.n_fatent = (le32(bs + 32) - g.database) / g.spc + 2;
    g.seen = calloc(g.n_fatent, 1);
    const bool walked = walk_dir(le32(bs + 44), "", 0);
    for (uint32_t c = 2; c < g.n_fatent; c++) {
        if (fat_entry(c) == 0) {
            out->free++;
        } else if (g.seen[c]) {
            out->used++;
        } else {
            out->lost++;
        }
    }
    free(g.seen);
    out->fsinfo_free = le32(ramdisk_sector(le16(bs + 48)) + 488);
    for (uint32_t s = 0; g.nfats >= 2 && s < g.fatsz; s++) {
        if (memcmp(ramdisk_sector(g.fatbase + s), ramdisk_sector(g.fatbase + g.fatsz + s), RAMDISK_SECTOR) != 0) {
            out->fat_sectors_differ++;
        }
    }
    return walked && !out->problem[0];
}
