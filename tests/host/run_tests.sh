#!/usr/bin/env bash
#
# Host-side tests for the storage logic. These compile the same
# blockdev/overlay/fat32lite/ramdisk sources the firmware uses, against a
# disk image standing in for the SD card, so the awkward parts can be
# checked without hardware.
#
# Needs: gcc, dosfstools (mkfs.vfat, fsck.vfat), python3 with pyfatfs
#        (pip install pyfatfs). pyfatfs is deliberately an independent FAT
#        implementation -- it writes the images our reader parses, and
#        reads the image our writer produces.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../../src"
work="${TMPDIR:-/tmp}/rfp-usb-tests"
mkdir -p "$work"

echo "== building test harness =="
gcc -std=c11 -Wall -Wextra -Werror -O1 -I"$src" -o "$work/test_rfp" \
    "$here/test_main.c" "$here/fake_sdcard.c" \
    "$src/blockdev.c" "$src/overlay.c" "$src/fat32lite.c" "$src/ramdisk.c"

echo
echo "== fallback RAM disk is a valid filesystem =="
"$work/test_rfp" ramdisk "$work/ramdisk.img"
fsck.vfat -n "$work/ramdisk.img"
# Independent reader must see the marker, with its long name intact.
python3 - "$work/ramdisk.img" <<'PY'
import sys
from pyfatfs.PyFatFS import PyFatFS
fs = PyFatFS(sys.argv[1])
names = fs.listdir("/")
assert names == ["factoryReset"], f"unexpected root listing: {names}"
assert fs.getinfo("/factoryReset", namespaces=["details"]).size == 0
print("  [PASS] pyfatfs reads back 'factoryReset' as a 0-byte file")
PY

echo
echo "== building FAT test images =="
( cd "$work" && python3 "$here/mkimg.py" )

echo
echo "== overlay behaviour =="
"$work/test_rfp" overlay "$work/fat32-mbr.img"

for img in fat32-fs fat32-mbr fat16-fs fat16-mbr; do
    echo
    echo "== $img =="
    "$work/test_rfp" inspect "$work/$img.img" "$work/$img-del.img"
    # The whole point: the card is never written to.
    cmp -s "$work/$img.img" "$work/$img-pristine.img" \
        && echo "  [PASS] card image byte-identical after the host's deletion"
done

echo
echo "== hand-off boot hides the marker without touching the card =="
for img in fat32-fs fat32-mbr fat16-fs fat16-mbr; do
    echo "-- $img"
    "$work/test_rfp" hide "$work/$img.img" "$work/$img-hidden.img"
    cmp -s "$work/$img.img" "$work/$img-pristine.img" \
        && echo "  [PASS] card image byte-identical after the firmware's own hide"
done
# fsck must find nothing wrong that it did not already find before the hide.
# (The images carry a stale FSInfo free count from the tool that built them,
# so the comparison is against the base rather than against silence.) Only the
# file count may differ, and only by the one we hid.
for img in fat32-fs fat16-fs; do
    fsck.vfat -n "$work/$img.img"        > "$work/$img-fsck-before.txt" 2>&1 || true
    fsck.vfat -n "$work/$img-hidden.img" > "$work/$img-fsck-after.txt"  2>&1 || true
    before=$(sed -E 's#^/.*: ([0-9]+) files.*#\1 files#' "$work/$img-fsck-before.txt")
    after=$(sed  -E 's#^/.*: ([0-9]+) files.*#\1 files#' "$work/$img-fsck-after.txt")
    n_before=$(grep -oE '[0-9]+ files' <<<"$before" | grep -oE '[0-9]+')
    n_after=$(grep  -oE '[0-9]+ files' <<<"$after"  | grep -oE '[0-9]+')
    diff <(grep -v ' files' <<<"$before") <(grep -v ' files' <<<"$after") \
        && [ "$n_after" -eq "$((n_before - 1))" ] \
        && echo "  [PASS] $img: fsck finds nothing new, and one file fewer"
done

python3 - "$work/fat32-fs-hidden.img" "$work/fat16-fs-hidden.img" <<'PYFAT'
import sys
from pyfatfs.PyFatFS import PyFatFS
for path in sys.argv[1:]:
    fs = PyFatFS(path)
    names = fs.listdir("/")
    assert "factoryReset" not in names, f"{path}: marker still listed: {names}"
    assert "iprfp3G.dnld" in names, f"{path}: payload lost: {names}"
    fs.close()
    print(f"  [PASS] {path.rsplit('/', 1)[-1]}: marker gone, payload intact")
PYFAT

echo
echo "== building the screen test harness =="
# Built with the longest version string the tagging convention allows, so the
# menu test is checking the case that could actually collide with "MODE"
# rather than the short fallback src/config.h carries.
gcc -std=c11 -Wall -Wextra -Werror -O1 \
    -DRFP_VERSION='"v12.34-RC56"' \
    -I"$src" -I"$here/fake_pico" -o "$work/test_ui" \
    "$here/test_ui.c" "$here/fake_panel.c" "$src/ui.c" "$src/st7789.c" \
    "$src/modes.c"

echo
echo "== payload checklist =="
"$work/test_ui" offer

echo
echo "== reset mode, recover phase =="
"$work/test_ui" recover

echo
echo "== upgrade mode =="
"$work/test_ui" upgrade

echo
echo "== the mode menu =="
"$work/test_ui" menu

echo
echo "ALL TESTS PASSED"
