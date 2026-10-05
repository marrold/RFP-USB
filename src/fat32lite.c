#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "blockdev.h"
#include "fat32lite.h"

#define SECTOR_SZ 512
#define ENT_SZ    32

static struct {
    bool       valid;
    fat_type_t type;
    uint32_t   vol_lba;        /* first sector of the volume */
    uint8_t    sec_per_clus;
    uint8_t    num_fats;
    uint32_t   fat_lba;        /* first FAT sector, absolute */
    uint32_t   fat_sectors;
    uint32_t   root_lba;       /* FAT12/16 fixed root, absolute */
    uint32_t   root_sectors;   /* FAT12/16 */
    uint32_t   root_cluster;   /* FAT32 */
    uint32_t   data_lba;       /* sector of cluster 2, absolute */
    uint32_t   clusters;
} vol;

static fat_file_t files[MAX_TRACKED_FILES];
static int        file_count;

/* One-sector cache. FAT chains and directories are read nearly
 * sequentially, so even a single entry removes most of the traffic. */
static uint8_t  cache_buf[SECTOR_SZ];
static uint32_t cache_lba = 0xFFFFFFFFu;
static bool     cache_ok;

static const uint8_t *read_sector(uint32_t lba) {
    if (cache_ok && cache_lba == lba) return cache_buf;
    cache_ok = blockdev_read(lba, cache_buf, 1);
    cache_lba = lba;
    return cache_ok ? cache_buf : NULL;
}

static void cache_invalidate(void) { cache_ok = false; cache_lba = 0xFFFFFFFFu; }

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t clus_to_lba(uint32_t clus) {
    return vol.data_lba + (clus - 2) * vol.sec_per_clus;
}

/* ------------------------------------------------------------- volume ---- */

static bool looks_like_bpb(const uint8_t *s) {
    if (!(s[0] == 0xEB || s[0] == 0xE9)) return false;
    uint16_t bps = rd16(s + 11);
    uint8_t  spc = s[13];
    return bps == SECTOR_SZ && spc && (spc & (spc - 1)) == 0;
}

static bool parse_bpb(uint32_t lba) {
    const uint8_t *s = read_sector(lba);
    if (!s) return false;

    if (memcmp(s + 3, "EXFAT   ", 8) == 0) {
        vol.type = FATFS_EXFAT;
        return false;
    }
    if (rd16(s + 11) != SECTOR_SZ) return false;

    vol.vol_lba      = lba;
    vol.sec_per_clus = s[13];
    vol.num_fats     = s[16];
    if (!vol.sec_per_clus || !vol.num_fats) return false;

    uint32_t rsvd       = rd16(s + 14);
    uint32_t root_ents  = rd16(s + 17);
    uint32_t tot_sec    = rd16(s + 19) ? rd16(s + 19) : rd32(s + 32);
    uint32_t fat_sz     = rd16(s + 22) ? rd16(s + 22) : rd32(s + 36);
    if (!rsvd || !fat_sz || !tot_sec) return false;

    uint32_t root_sectors = (root_ents * ENT_SZ + SECTOR_SZ - 1) / SECTOR_SZ;

    vol.fat_lba      = lba + rsvd;
    vol.fat_sectors  = fat_sz;
    vol.root_lba     = vol.fat_lba + vol.num_fats * fat_sz;
    vol.root_sectors = root_sectors;
    vol.data_lba     = vol.root_lba + root_sectors;

    uint32_t meta = rsvd + vol.num_fats * fat_sz + root_sectors;
    if (tot_sec <= meta) return false;
    vol.clusters = (tot_sec - meta) / vol.sec_per_clus;

    if (vol.clusters < 4085)       vol.type = FATFS_FAT12;
    else if (vol.clusters < 65525) vol.type = FATFS_FAT16;
    else                           vol.type = FATFS_FAT32;

    if (vol.type == FATFS_FAT32) vol.root_cluster = rd32(s + 44);
    return true;
}

bool fat_scan_volume(void) {
    memset(&vol, 0, sizeof vol);
    cache_invalidate();

    const uint8_t *s = read_sector(0);
    if (!s || rd16(s + 510) != 0xAA55) return false;

    /* A boot sector and an MBR both end in 0x55AA; the jump instruction is
     * what tells them apart. */
    if (looks_like_bpb(s)) {
        vol.valid = parse_bpb(0);
        return vol.valid;
    }

    /* MBR: take the first partition with a FAT-ish type and a sane start. */
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = s + 446 + i * 16;
        uint8_t  type  = e[4];
        uint32_t start = rd32(e + 8);
        uint32_t count = rd32(e + 12);
        if (!type || !start || !count) continue;
        switch (type) {
            case 0x01: case 0x04: case 0x06: case 0x0B:
            case 0x0C: case 0x0E: case 0x07:
                cache_invalidate();
                if (parse_bpb(start)) { vol.valid = true; return true; }
                break;
            default: break;
        }
    }
    return false;
}

fat_type_t fat_type(void) { return vol.type; }

bool fat_can_inspect(void) { return vol.valid; }

const char *fat_type_str(void) {
    switch (vol.type) {
        case FATFS_FAT12: return "FAT12";
        case FATFS_FAT16: return "FAT16";
        case FATFS_FAT32: return "FAT32";
        case FATFS_EXFAT: return "exFAT";
        default:          return "?";
    }
}

/* ---------------------------------------------------------------- FAT ---- */

#define CLUS_EOC 0xFFFFFFFFu

static uint32_t next_cluster(uint32_t clus) {
    if (vol.type == FATFS_FAT32) {
        uint32_t off = clus * 4;
        const uint8_t *s = read_sector(vol.fat_lba + off / SECTOR_SZ);
        if (!s) return CLUS_EOC;
        uint32_t v = rd32(s + off % SECTOR_SZ) & 0x0FFFFFFFu;
        return v >= 0x0FFFFFF8u ? CLUS_EOC : v;
    }
    if (vol.type == FATFS_FAT16) {
        uint32_t off = clus * 2;
        const uint8_t *s = read_sector(vol.fat_lba + off / SECTOR_SZ);
        if (!s) return CLUS_EOC;
        uint32_t v = rd16(s + off % SECTOR_SZ);
        return v >= 0xFFF8u ? CLUS_EOC : v;
    }
    if (vol.type == FATFS_FAT12) {
        /* 12-bit entries are not sector aligned, so a pair can straddle
         * two sectors and the halves must be fetched separately. */
        uint32_t off = clus + clus / 2;
        const uint8_t *s = read_sector(vol.fat_lba + off / SECTOR_SZ);
        if (!s) return CLUS_EOC;
        uint8_t lo = s[off % SECTOR_SZ];
        s = read_sector(vol.fat_lba + (off + 1) / SECTOR_SZ);
        if (!s) return CLUS_EOC;
        uint8_t hi = s[(off + 1) % SECTOR_SZ];
        uint32_t v = (uint32_t)lo | ((uint32_t)hi << 8);
        v = (clus & 1) ? (v >> 4) : (v & 0x0FFF);
        return v >= 0xFF8u ? CLUS_EOC : v;
    }
    return CLUS_EOC;
}

/* --------------------------------------------------------- directories --- */

/* Where the next directory sector lives. FAT12/16 roots are a flat run of
 * sectors; a FAT32 root is an ordinary cluster chain. */
typedef struct {
    uint32_t clus;      /* 0 for the flat FAT12/16 root */
    uint32_t lba;
    uint32_t remaining; /* sectors left in the flat root */
    uint32_t in_clus;   /* sectors consumed in the current cluster */
} dir_iter_t;

static void dir_begin(dir_iter_t *it) {
    if (vol.type == FATFS_FAT32) {
        it->clus = vol.root_cluster;
        it->lba  = clus_to_lba(it->clus);
        it->in_clus = 0;
        it->remaining = 0;
    } else {
        it->clus = 0;
        it->lba  = vol.root_lba;
        it->remaining = vol.root_sectors;
        it->in_clus = 0;
    }
}

/* Returns the next directory sector and its LBA, or NULL at the end. */
static const uint8_t *dir_next(dir_iter_t *it, uint32_t *lba_out) {
    if (it->clus == 0) {
        if (!it->remaining) return NULL;
        it->remaining--;
        *lba_out = it->lba;
        return read_sector(it->lba++);
    }
    if (it->in_clus == vol.sec_per_clus) {
        uint32_t nxt = next_cluster(it->clus);
        if (nxt == CLUS_EOC || nxt < 2 || nxt >= vol.clusters + 2) return NULL;
        it->clus = nxt;
        it->lba = clus_to_lba(nxt);
        it->in_clus = 0;
    }
    it->in_clus++;
    *lba_out = it->lba;
    return read_sector(it->lba++);
}

static const uint8_t lfn_slots[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

static uint8_t sfn_checksum(const uint8_t *sfn) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + sfn[i]);
    return sum;
}

static void sfn_to_name(const uint8_t *e, char *out) {
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) out[n++] = (char)e[i];
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) out[n++] = (char)e[i];
    }
    out[n] = '\0';
}

static bool name_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return !*a && !*b;
}

/* Where an entry sits on the medium, for a callback that means to rewrite it.
 * `sector` is walk_root's copy of the whole directory sector, `offset` the 8.3
 * record within it, and `lfn_offset` the first of the long-name records that
 * belong to it -- equal to `offset` when there are none, or when the run
 * started in the previous sector and so cannot be reached from this one.
 */
typedef struct {
    uint32_t       lba;
    const uint8_t *sector;
    uint16_t       offset;
    uint16_t       lfn_offset;
} dir_pos_t;

/* Walks the root directory, handing each entry's name and 8.3 record to cb.
 * Stops early if cb returns false. */
typedef bool (*dir_cb_t)(const char *name, const uint8_t *ent,
                         const dir_pos_t *pos, void *ctx);

static void walk_root(dir_cb_t cb, void *ctx) {
    if (!vol.valid) return;

    dir_iter_t it;
    dir_begin(&it);

    char     lfn[64];
    bool     have_lfn = false;
    uint8_t  lfn_sum = 0;
    /* Where the run of long-name records started, so a callback can mark the
     * whole set of them rather than orphaning them. */
    uint32_t lfn_lba = 0;
    uint16_t lfn_off = 0;
    lfn[0] = '\0';

    /* The callback may read further sectors (walking a file's FAT chain),
     * which would overwrite the shared cache this sector lives in -- so
     * take a copy before handing any entry out. */
    uint8_t dir[SECTOR_SZ];

    const uint8_t *sec;
    uint32_t dir_lba;
    uint32_t guard = 0;
    while ((sec = dir_next(&it, &dir_lba)) != NULL && guard++ < 4096) {
        memcpy(dir, sec, SECTOR_SZ);

        for (int o = 0; o < SECTOR_SZ; o += ENT_SZ) {
            const uint8_t *e = dir + o;
            if (e[0] == 0x00) return;              /* end of directory */
            if (e[0] == 0xE5) { have_lfn = false; continue; }  /* deleted */

            uint8_t attr = e[11];
            if ((attr & 0x3F) == 0x0F) {           /* long name fragment */
                int seq = e[0] & 0x1F;
                if (seq < 1 || seq > 4) { have_lfn = false; continue; }
                if (e[0] & 0x40) {
                    have_lfn = true;
                    lfn_sum  = e[13];
                    lfn_lba  = dir_lba;
                    lfn_off  = (uint16_t)o;
                    memset(lfn, 0, sizeof lfn);
                }
                if (!have_lfn || e[13] != lfn_sum) { have_lfn = false; continue; }
                for (int i = 0; i < 13; i++) {
                    uint16_t ch = (uint16_t)(e[lfn_slots[i]] | (e[lfn_slots[i] + 1] << 8));
                    size_t idx = (size_t)(seq - 1) * 13 + (size_t)i;
                    if (idx >= sizeof lfn - 1) break;
                    /* Non-Latin-1 code points are not representable on the
                     * 8x16 display font, so they become '?'. */
                    lfn[idx] = (ch == 0 || ch == 0xFFFF) ? '\0' : (ch < 0x80 ? (char)ch : '?');
                }
                continue;
            }
            if (attr & 0x08) { have_lfn = false; continue; }   /* volume label */

            bool named_by_lfn = have_lfn && lfn_sum == sfn_checksum(e) && lfn[0];

            char name[64];
            if (named_by_lfn) {
                memcpy(name, lfn, sizeof name);
                name[sizeof name - 1] = '\0';
            } else {
                sfn_to_name(e, name);
            }
            have_lfn = false;

            dir_pos_t pos = {
                .lba        = dir_lba,
                .sector     = dir,
                .offset     = (uint16_t)o,
                .lfn_offset = (named_by_lfn && lfn_lba == dir_lba && lfn_off < o)
                                  ? lfn_off : (uint16_t)o,
            };

            if (!cb(name, e, &pos, ctx)) return;
        }
    }
}

/* --------------------------------------------------------- public API ---- */

typedef struct { const char *want; bool found; } find_ctx_t;

static bool find_cb(const char *name, const uint8_t *ent,
                    const dir_pos_t *pos, void *ctx) {
    (void)ent; (void)pos;
    find_ctx_t *c = ctx;
    if (name_eq_ci(name, c->want)) { c->found = true; return false; }
    return true;
}

typedef struct { const char *want; bool found; bool hidden; } hide_ctx_t;

static bool hide_cb(const char *name, const uint8_t *ent,
                    const dir_pos_t *pos, void *ctx) {
    (void)ent;
    hide_ctx_t *c = ctx;
    if (!name_eq_ci(name, c->want)) return true;

    c->found = true;

    /* A copy, not walk_root's buffer: the write goes through the block layer,
     * which may read as it goes, and the sector the callback was handed is the
     * one thing guaranteed to still be valid. */
    uint8_t sector[SECTOR_SZ];
    memcpy(sector, pos->sector, sizeof sector);

    /* 0xE5 on the 8.3 record is what a host writes to delete a file, and on
     * the long-name records with it: an orphaned run of those is ignored by
     * every driver that matches them against a following 8.3 record, but one
     * that salvages orphans instead would still list the file. */
    for (uint16_t o = pos->lfn_offset; o <= pos->offset; o += ENT_SZ)
        sector[o] = 0xE5;

    c->hidden = blockdev_write(pos->lba, sector, 1);
    return false;
}

bool fat_hide_in_root(const char *name) {
    cache_invalidate();   /* the directory is about to change under the cache */
    hide_ctx_t ctx = { .want = name, .found = false, .hidden = false };
    walk_root(hide_cb, &ctx);
    cache_invalidate();
    return !ctx.found || ctx.hidden;
}

bool fat_find_in_root(const char *name) {
    cache_invalidate();   /* the host may have rewritten these sectors */
    find_ctx_t ctx = { .want = name, .found = false };
    walk_root(find_cb, &ctx);
    return ctx.found;
}

static bool collect_cb(const char *name, const uint8_t *ent,
                       const dir_pos_t *pos, void *ctx) {
    (void)pos; (void)ctx;
    if (file_count >= MAX_TRACKED_FILES) return false;
    if (ent[11] & 0x10) return true;    /* skip subdirectories */

    fat_file_t *f = &files[file_count];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name);
    f->size = rd32(ent + 28);

    uint32_t clus = (uint32_t)rd16(ent + 26) | ((uint32_t)rd16(ent + 20) << 16);
    if (vol.type != FATFS_FAT32) clus = rd16(ent + 26);

    /* Coalesce the cluster chain into contiguous runs. A file copied onto a
     * freshly formatted card is normally a single run. */
    uint32_t guard = 0;
    while (clus >= 2 && clus < vol.clusters + 2 && guard++ < 1000000u) {
        uint32_t lba = clus_to_lba(clus);
        if (f->n_extents &&
            f->extents[f->n_extents - 1].start_lba +
                f->extents[f->n_extents - 1].sectors == lba) {
            f->extents[f->n_extents - 1].sectors += vol.sec_per_clus;
        } else if (f->n_extents < MAX_EXTENTS_PER_FILE) {
            f->extents[f->n_extents].start_lba = lba;
            f->extents[f->n_extents].sectors   = vol.sec_per_clus;
            f->n_extents++;
        } else {
            f->extents_truncated = true;
            break;
        }
        clus = next_cluster(clus);
        if (clus == CLUS_EOC) break;
    }

    file_count++;
    return true;
}

void fat_scan_files(void) {
    file_count = 0;
    cache_invalidate();
    walk_root(collect_cb, NULL);
}

const fat_file_t *fat_files(void)  { return files; }
int               fat_file_count(void) { return file_count; }

const char *fat_file_for_lba(uint32_t lba) {
    /* Host reads are overwhelmingly sequential, so remembering the last hit
     * turns the common case into one comparison. */
    static int last = -1;
    if (last >= 0 && last < file_count) {
        const fat_file_t *f = &files[last];
        for (int e = 0; e < f->n_extents; e++)
            if (lba >= f->extents[e].start_lba &&
                lba < f->extents[e].start_lba + f->extents[e].sectors)
                return f->name;
    }
    for (int i = 0; i < file_count; i++) {
        const fat_file_t *f = &files[i];
        for (int e = 0; e < f->n_extents; e++)
            if (lba >= f->extents[e].start_lba &&
                lba < f->extents[e].start_lba + f->extents[e].sectors) {
                last = i;
                return f->name;
            }
    }
    return NULL;
}
