#include <ctype.h>
#include <string.h>

#include "config.h"
#include "ramdisk.h"

/* Layout: sector 0 boot, sector 1 FAT, sector 2 root directory, the rest
 * data. One sector per cluster gives 125 clusters, comfortably inside
 * FAT12's range. */
#define RD_RSVD_SECTORS  1
#define RD_FAT_SECTORS   1
#define RD_ROOT_ENTRIES  16
#define RD_ROOT_SECTORS  ((RD_ROOT_ENTRIES * 32) / 512)

#define LBA_BOOT  0
#define LBA_FAT   (RD_RSVD_SECTORS)
#define LBA_ROOT  (RD_RSVD_SECTORS + RD_FAT_SECTORS)

/* Copied verbatim into the BPB and the volume-label directory entry. */
_Static_assert(sizeof(FALLBACK_VOLUME_LABEL) - 1 == 11,
               "FALLBACK_VOLUME_LABEL must be 11 characters, space padded");
_Static_assert(RD_ROOT_SECTORS == 1, "root directory is assumed to be one sector");

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24;
}

static uint8_t sfn_checksum(const uint8_t *sfn) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + sfn[i]);
    return sum;
}

/* Derives a "FACTOR~1" style short name. Good enough for the handful of
 * names this disk ever holds; there is nothing to collide with. */
static void make_short_name(const char *name, uint8_t *sfn) {
    memset(sfn, ' ', 11);

    const char *dot = strrchr(name, '.');
    size_t stem_len = dot ? (size_t)(dot - name) : strlen(name);

    int n = 0;
    for (size_t i = 0; i < stem_len && n < 6; i++) {
        unsigned char c = (unsigned char)name[i];
        if (isalnum(c) || strchr("$%'-_@~`!(){}^#&", c)) sfn[n++] = (uint8_t)toupper(c);
    }
    /* Always tilde-suffix: the stem is truncated or case-folded in every
     * case we care about, so the long name is never redundant. */
    sfn[n < 6 ? n : 6] = '~';
    sfn[(n < 6 ? n : 6) + 1] = '1';

    if (dot) {
        int e = 0;
        for (const char *p = dot + 1; *p && e < 3; p++) {
            unsigned char c = (unsigned char)*p;
            if (isalnum(c)) sfn[8 + e++] = (uint8_t)toupper(c);
        }
    }
}

int fat_build_dir_entries(const char *name, uint32_t size, uint8_t attr,
                          uint8_t *out, int max_entries) {
    size_t len = strlen(name);
    int lfn_count = (int)((len + 12) / 13);       /* 13 UTF-16 units per entry */
    if (lfn_count < 1) lfn_count = 1;
    if (lfn_count + 1 > max_entries) return 0;

    uint8_t sfn[11];
    make_short_name(name, sfn);
    uint8_t sum = sfn_checksum(sfn);

    /* LFN entries are stored in reverse: the last chunk of the name comes
     * first on disk, and the entry nearest the 8.3 entry holds chunk 1. */
    static const uint8_t char_slots[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (int seq = lfn_count; seq >= 1; seq--) {
        uint8_t *e = out + (lfn_count - seq) * 32;
        memset(e, 0, 32);
        e[0] = (uint8_t)(seq | (seq == lfn_count ? 0x40 : 0));
        e[11] = 0x0F;   /* ATTR_LONG_NAME */
        e[13] = sum;
        for (int i = 0; i < 13; i++) {
            size_t idx = (size_t)(seq - 1) * 13 + (size_t)i;
            uint16_t ch;
            if (idx < len)        ch = (uint8_t)name[idx];
            else if (idx == len)  ch = 0x0000;   /* terminator */
            else                  ch = 0xFFFF;   /* pad */
            put16(e + char_slots[i], ch);
        }
    }

    uint8_t *s = out + lfn_count * 32;
    memset(s, 0, 32);
    memcpy(s, sfn, 11);
    s[11] = attr;
    put16(s + 14, 0x0000);        /* create time 00:00:00 */
    put16(s + 16, 0x5821);        /* create date 2024-01-01 */
    put16(s + 18, 0x5821);
    put16(s + 22, 0x0000);
    put16(s + 24, 0x5821);
    put32(s + 28, size);
    return lfn_count + 1;
}

static void build_boot_sector(uint8_t *b) {
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    memcpy(b + 3, "MSWIN4.1", 8);
    put16(b + 11, 512);                 /* bytes per sector */
    b[13] = 1;                          /* sectors per cluster */
    put16(b + 14, RD_RSVD_SECTORS);
    b[16] = 1;                          /* one FAT */
    put16(b + 17, RD_ROOT_ENTRIES);
    put16(b + 19, RAMDISK_SECTORS);
    b[21] = 0xF8;                       /* fixed disk */
    put16(b + 22, RD_FAT_SECTORS);
    put16(b + 24, 1);                   /* sectors per track */
    put16(b + 26, 1);                   /* heads */
    put32(b + 28, 0);                   /* hidden sectors */
    put32(b + 32, 0);                   /* total sectors (32-bit) -- unused */
    b[36] = 0x80;                       /* drive number */
    b[38] = 0x29;                       /* extended boot signature */
    put32(b + 39, 0x52465055);          /* volume serial */
    memcpy(b + 43, FALLBACK_VOLUME_LABEL, 11);
    memcpy(b + 54, "FAT12   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
}

static void build_fat_sector(uint8_t *b) {
    /* Entries 0 and 1 are reserved; everything else is free, since the
     * marker file is zero length and so owns no clusters. */
    b[0] = 0xF8; b[1] = 0xFF; b[2] = 0xFF;
}

static void build_root_sector(uint8_t *b) {
    memcpy(b, FALLBACK_VOLUME_LABEL, 11);
    b[11] = 0x08;                       /* ATTR_VOLUME_ID */
    fat_build_dir_entries(MARKER_FILENAME, 0, 0x20 /* ATTR_ARCHIVE */,
                          b + 32, RD_ROOT_ENTRIES - 1);
}

void ramdisk_read_sector(uint32_t lba, uint8_t *buf) {
    memset(buf, 0, 512);
    switch (lba) {
        case LBA_BOOT: build_boot_sector(buf); break;
        case LBA_FAT:  build_fat_sector(buf);  break;
        case LBA_ROOT: build_root_sector(buf); break;
        default:       break;           /* data area reads back as zeros */
    }
}
