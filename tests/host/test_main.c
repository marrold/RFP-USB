/* Host-side exercise of the storage logic.
 *
 * Usage:
 *   test_rfp ramdisk <out.img>
 *       Writes the synthetic FAT12 fallback volume out, so an independent
 *       tool can judge whether it is a valid filesystem.
 *
 *   test_rfp overlay <base.img>
 *       Exercises the copy-on-write cache directly: hit/miss behaviour,
 *       the read path's batching of mixed hits and misses, and what
 *       happens when it fills up.
 *
 *   test_rfp inspect <base.img> [modified.img]
 *       Parses base.img. If modified.img is given, every sector that
 *       differs is replayed into the copy-on-write overlay -- exactly what
 *       happens when a host rewrites those sectors -- and the volume is
 *       re-read through it. base.img itself is never altered, which is the
 *       property the whole tool rests on.
 *
 *   test_rfp hide <base.img> <out.img>
 *       Hides the marker the way a hand-off boot does, then writes the
 *       volume as the host would now see it, so an independent FAT reader
 *       can judge whether the directory is still sound.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blockdev.h"
#include "config.h"
#include "fat32lite.h"
#include "overlay.h"
#include "ramdisk.h"
#include "sdcard.h"

bool fake_sdcard_load(const char *path);
const uint8_t *fake_sdcard_image(void);

static int failures;

static void check(bool cond, const char *what) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) failures++;
}

/* The in-memory "card" against the file it was loaded from. fake_sdcard never
 * writes back, so any divergence means a write that should have gone to the
 * overlay reached the card instead. */
static bool card_matches_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return false; }

    bool same = true;
    uint8_t want[512];
    for (uint32_t lba = 0; lba < sdcard_sector_count(); lba++) {
        if (fread(want, 1, sizeof want, f) != sizeof want) break;
        if (memcmp(want, fake_sdcard_image() + (size_t)lba * 512, sizeof want) != 0) {
            printf("    card differs at LBA %lu\n", (unsigned long)lba);
            same = false;
            break;
        }
    }
    fclose(f);
    return same;
}

static void report_volume(void) {
    printf("  filesystem: %s\n", fat_type_str());
    fat_scan_files();
    printf("  %d file(s) in root:\n", fat_file_count());
    for (int i = 0; i < fat_file_count(); i++) {
        const fat_file_t *f = &fat_files()[i];
        printf("    %-28s %9lu bytes  %u extent(s)%s\n",
               f->name, (unsigned long)f->size, f->n_extents,
               f->extents_truncated ? " (truncated)" : "");
        for (int e = 0; e < f->n_extents; e++)
            printf("        LBA %lu..%lu\n",
                   (unsigned long)f->extents[e].start_lba,
                   (unsigned long)(f->extents[e].start_lba + f->extents[e].sectors - 1));
    }
}

static int cmd_ramdisk(const char *out) {
    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    uint8_t sec[512];
    for (uint32_t lba = 0; lba < RAMDISK_SECTORS; lba++) {
        ramdisk_read_sector(lba, sec);
        fwrite(sec, 1, sizeof sec, f);
    }
    fclose(f);
    printf("wrote %s (%d sectors)\n", out, RAMDISK_SECTORS);
    return 0;
}

static int cmd_inspect(const char *base, const char *modified) {
    if (!fake_sdcard_load(base)) { fprintf(stderr, "cannot load %s\n", base); return 1; }
    blockdev_configure(BLOCKDEV_SD, false);   /* writes go to the overlay */

    printf("base image:\n");
    check(fat_scan_volume(), "volume parses");
    report_volume();
    bool present_before = fat_find_in_root(MARKER_FILENAME);
    check(present_before, MARKER_FILENAME " found in root");

    if (!modified) return failures;

    /* Replay the host's edit as a sequence of sector writes. */
    FILE *m = fopen(modified, "rb");
    if (!m) { perror(modified); return 1; }

    uint32_t changed = 0;
    uint8_t  sec[512];
    for (uint32_t lba = 0; lba < sdcard_sector_count(); lba++) {
        if (fread(sec, 1, 512, m) != 512) break;
        uint8_t cur[512];
        sdcard_read(lba, cur, 1);
        if (memcmp(cur, sec, 512) == 0) continue;
        if (!blockdev_write(lba, sec, 1)) {
            printf("  overlay refused LBA %lu\n", (unsigned long)lba);
            break;
        }
        changed++;
    }
    fclose(m);

    printf("\nafter replaying the host's deletion (%lu sector(s), %u overlay slot(s) used):\n",
           (unsigned long)changed, overlay_used());
    check(changed > 0, "the edit touched at least one sector");
    check(!overlay_overflowed(), "overlay absorbed the edit without overflowing");

    check(fat_scan_volume(), "volume still parses through the overlay");
    report_volume();
    check(!fat_find_in_root(MARKER_FILENAME), MARKER_FILENAME " gone from the host's view");

    /* The point of the exercise: the card is untouched, so a power cycle
     * brings the marker back. */
    overlay_reset();
    check(fat_scan_volume() && fat_find_in_root(MARKER_FILENAME),
          MARKER_FILENAME " returns once the overlay is cleared");

    return failures;
}

/* The firmware hides the marker itself on a hand-off boot, which is the one
 * place it writes a directory sector rather than just reading one. The write
 * has to land in the overlay -- never on the card -- and has to leave a
 * directory an independent reader still accepts, long-name records included.
 */
static int cmd_hide(const char *base, const char *out) {
    if (!fake_sdcard_load(base)) { fprintf(stderr, "cannot load %s\n", base); return 1; }
    blockdev_configure(BLOCKDEV_SD, false);   /* writes go to the overlay */

    check(fat_scan_volume(), "volume parses");
    check(fat_find_in_root(MARKER_FILENAME), MARKER_FILENAME " found in root");

    check(fat_hide_in_root(MARKER_FILENAME), "the hand-off hide reports success");
    check(!fat_find_in_root(MARKER_FILENAME), MARKER_FILENAME " gone from the host's view");
    check(overlay_used() == 1, "exactly one directory sector was diverted");
    check(card_matches_file(base), "the card itself was not written to");

    /* Hiding it again is what a boot would do after the host had already
     * deleted it, and must not be treated as a failure. */
    check(fat_hide_in_root(MARKER_FILENAME), "hiding an already-absent marker still succeeds");

    /* The whole point: a power cycle drops the overlay and the marker is back.
     * Re-hide it afterwards so the image written out below is the host's view. */
    overlay_reset();
    check(fat_scan_volume() && fat_find_in_root(MARKER_FILENAME),
          MARKER_FILENAME " returns once the overlay is cleared");
    check(fat_hide_in_root(MARKER_FILENAME), "re-hidden for the image dump");

    report_volume();

    /* Dump what the host reads -- card plus overlay -- for an independent
     * reader to pass judgement on. */
    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    uint8_t sec[512];
    bool read_ok = true;
    for (uint32_t lba = 0; lba < sdcard_sector_count(); lba++) {
        if (!blockdev_read(lba, sec, 1)) { read_ok = false; break; }
        fwrite(sec, 1, sizeof sec, f);
    }
    fclose(f);
    check(read_ok, "the whole overlaid volume reads back");

    return failures;
}

/* The read path coalesces runs of overlay misses into single multi-sector
 * card reads. That optimisation is easy to get subtly wrong, so check a
 * large read straddling a scattered set of overlaid sectors. */
static int cmd_overlay(const char *base) {
    if (!fake_sdcard_load(base)) { fprintf(stderr, "cannot load %s\n", base); return 1; }
    blockdev_configure(BLOCKDEV_SD, false);

    /* Scatter writes so hits and misses interleave awkwardly: adjacent, a
     * lone sector, and a run. */
    const uint32_t written[] = { 100, 101, 102, 137, 150, 151 };
    const uint32_t n_written = sizeof written / sizeof written[0];

    uint8_t patch[512];
    for (uint32_t i = 0; i < n_written; i++) {
        memset(patch, (int)(0xA0 + i), sizeof patch);
        check(blockdev_write(written[i], patch, 1), "overlay accepts a write");
    }
    check(overlay_used() == n_written, "one slot consumed per distinct sector");

    /* Rewriting an existing sector must reuse its slot rather than a new one. */
    memset(patch, 0x5A, sizeof patch);
    blockdev_write(written[0], patch, 1);
    check(overlay_used() == n_written, "rewriting a sector reuses its slot");

    /* One big read across the lot, compared against what we expect
     * sector by sector. */
    uint8_t *got = malloc(80 * 512);
    check(blockdev_read(90, got, 80), "multi-sector read spanning hits and misses");

    bool all_ok = true;
    for (uint32_t s = 0; s < 80; s++) {
        uint32_t lba = 90 + s;
        uint8_t expect[512];
        bool overlaid = false;
        for (uint32_t i = 0; i < n_written; i++)
            if (written[i] == lba) { overlaid = true; memset(expect, (int)(i == 0 ? 0x5A : 0xA0 + i), 512); }
        if (!overlaid) memcpy(expect, fake_sdcard_image() + (size_t)lba * 512, 512);
        if (memcmp(expect, got + (size_t)s * 512, 512) != 0) {
            printf("    mismatch at LBA %lu (%s)\n", (unsigned long)lba,
                   overlaid ? "overlaid" : "from card");
            all_ok = false;
        }
    }
    check(all_ok, "every sector in the range reads back correctly");
    free(got);

    /* Fill it up, then confirm the writes past the end are swallowed rather
     * than failed, that what is already held still reads back, and that a
     * discarded sector still reads as the card has it. */
    overlay_reset();
    memset(patch, 0xC3, sizeof patch);
    uint32_t accepted = 0;
    for (uint32_t lba = 1000; lba < 1000 + OVERLAY_SLOTS + 32; lba++)
        if (blockdev_write(lba, patch, 1)) accepted++;

    check(accepted == OVERLAY_SLOTS + 32, "every write is reported as succeeding");
    check(overlay_used() == OVERLAY_SLOTS, "holds exactly OVERLAY_SLOTS sectors");
    check(overlay_is_full(), "reports itself full");
    check(overlay_overflowed(), "flags the discard for anyone looking");

    uint8_t back[512];
    check(blockdev_read(1000, back, 1) && back[0] == 0xC3,
          "sectors stored before the overflow still read back");

    /* The last LBA of the run cannot have been stored, so it must still read
     * through to the card untouched. */
    uint32_t spilled = 1000 + OVERLAY_SLOTS + 31;
    check(!overlay_contains(spilled), "the spilled sector was not stored");
    check(blockdev_read(spilled, back, 1) &&
          memcmp(back, fake_sdcard_image() + (size_t)spilled * 512, 512) == 0,
          "a discarded sector still reads as the card has it");

    return failures;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "overlay") == 0)  return cmd_overlay(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "ramdisk") == 0) return cmd_ramdisk(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "inspect") == 0)
        return cmd_inspect(argv[2], argc > 3 ? argv[3] : NULL);
    if (argc >= 4 && strcmp(argv[1], "hide") == 0) return cmd_hide(argv[2], argv[3]);

    fprintf(stderr,
            "usage: %s ramdisk <out.img> | overlay <base.img>\n"
            "       %s inspect <base.img> [modified.img] | hide <base.img> <out.img>\n",
            argv[0], argv[0]);
    return 2;
}
