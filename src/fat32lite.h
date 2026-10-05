/* A small read-only FAT12/16/32 reader that goes through the block layer.
 *
 * FatFs cannot be used for this: in the carlk3 library it is bound directly
 * to the SD driver, so it would see the card rather than the overlaid view
 * the host has. Answering "has the host deleted the marker yet?" requires
 * reading exactly what the host would read.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

typedef enum {
    FATFS_NONE,
    FATFS_FAT12,
    FATFS_FAT16,
    FATFS_FAT32,
    FATFS_EXFAT,   /* recognised but not inspectable */
} fat_type_t;

typedef struct {
    uint32_t start_lba;
    uint32_t sectors;
} fat_extent_t;

typedef struct {
    char         name[64];
    uint32_t     size;
    uint8_t      n_extents;
    bool         extents_truncated;  /* too fragmented to map fully */
    fat_extent_t extents[MAX_EXTENTS_PER_FILE];
} fat_file_t;

/* Parses the MBR (if any) and the BPB. Cheap; call before anything else. */
bool       fat_scan_volume(void);
fat_type_t fat_type(void);
const char *fat_type_str(void);

/* Walks the root directory and records files with their on-disk extents,
 * for the activity readout. Comparatively expensive -- boot-time only. */
void             fat_scan_files(void);
const fat_file_t *fat_files(void);
int               fat_file_count(void);

/* Which file covers this LBA, or NULL. */
const char *fat_file_for_lba(uint32_t lba);

/* Walks the root directory looking for name. Cheap enough to call whenever
 * host writes settle. Returns false on exFAT or an unparsable volume, so
 * callers must check fat_can_inspect() to tell "absent" from "unknown". */
bool fat_find_in_root(const char *name);
bool fat_can_inspect(void);

/* Hides a root-directory file from the host by marking its directory records
 * deleted. The write goes wherever the block layer sends it -- outside SD card
 * mode that is the RAM overlay, so the card is untouched and the file returns
 * at the next power cycle.
 *
 * Only the directory records are touched: the FAT chain is left as it is, so
 * the host's view of free space does not grow. That is exactly right for a
 * zero-length marker and would leak clusters for anything larger.
 *
 * True if the file is no longer visible, which includes it never having been
 * there. False means the write failed and the file is still on offer. */
bool fat_hide_in_root(const char *name);
