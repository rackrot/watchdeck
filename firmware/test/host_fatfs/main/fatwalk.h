#pragma once

// A FAT32 checker that reads the RAM disk's sectors directly, apart from FatFs: it walks every directory from the
// root and follows every chain in FAT 1.

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t files, dirs;
    uint32_t used;               // clusters in some chain
    uint32_t lost;               // allocated clusters in no chain: what fsck.vfat reclaims
    uint32_t free;               // free clusters in FAT 1
    uint32_t fsinfo_free;        // the free count FSINFO stores (0xFFFFFFFF: unknown)
    uint32_t fat_sectors_differ; // FAT 1 and FAT 2 sectors that differ
    // Directory clusters with non-zero bytes past the directory's end marker. FatFs zeroes a directory's new cluster
    // only after linking it, so a cut between the two leaves stale bytes there; FatFs's lookups stop at the marker,
    // but it can still place a new entry past them, where no lookup finds it.
    uint32_t hidden_dir_clusters;
    char problem[200]; // the first structural problem, "" when there is none
} fatwalk_t;

// False when the volume has a problem fsck.vfat would repair by touching a file: a cluster in two chains or twice
// in one, a chain that runs into a free or bad cluster or off the volume, or a chain whose length doesn't match
// its file's size.
bool fatwalk(fatwalk_t *out);
