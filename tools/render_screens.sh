#!/usr/bin/env bash
#
# Renders every screen state to docs/screens/*.png.
#
# This drives the real src/ui.c through the real src/st7789.c; only the SPI
# and GPIO calls are faked, and the fake decodes the ST7789 command stream
# into a framebuffer. So the output reflects the actual glyph rendering,
# colour byte order and address-window offsets -- it is a layout check, not
# an illustration. Anything that overruns the 30-column row width or picks a
# poor colour pair shows up here.
#
# Needs: gcc, python3 with Pillow.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The mode menu shows the build's version, which CMake normally supplies.
# Rendered against the nearest tag so the gallery shows the shape of what an
# operator reads, rather than the "dev" fallback src/config.h carries for a
# harness with no build system behind it.
version_def=()
version="$(git -C "$root" describe --tags --abbrev=0 2>/dev/null || true)"
if [ -n "$version" ]; then
    version_def=(-DRFP_VERSION="\"$version\"")
fi

gcc -std=c11 -Wall -Wextra -Werror -O1 \
    "${version_def[@]}" \
    -I"$root/src" -I"$root/tests/host/fake_pico" -o "$work/render" \
    "$root/tests/host/render_screens.c" "$root/tests/host/fake_panel.c" \
    "$root/src/ui.c" "$root/src/st7789.c" "$root/src/modes.c"

( cd "$work" && ./render )

python3 - "$work" "$root/docs/screens" <<'PY'
import glob, os, sys
from PIL import Image, ImageDraw

work, outdir = sys.argv[1], sys.argv[2]
os.makedirs(outdir, exist_ok=True)
# Clear stale output, so renaming or renumbering a scene cannot leave an
# orphan PNG behind that still looks current.
for old in glob.glob(os.path.join(outdir, "*.png")):
    os.remove(old)
SCALE, PAD, LBL, COLS = 3, 14, 22, 2

imgs = []
for f in sorted(glob.glob(os.path.join(work, "*.ppm"))):
    im = Image.open(f).convert("RGB")
    assert im.size == (240, 135), f"{f}: unexpected size {im.size}"
    big = im.resize((240 * SCALE, 135 * SCALE), Image.NEAREST)
    name = os.path.splitext(os.path.basename(f))[0]
    big.save(os.path.join(outdir, name + ".png"))
    imgs.append((name, big))

w, h = imgs[0][1].size
rows = (len(imgs) + COLS - 1) // COLS
sheet = Image.new("RGB", (COLS * (w + PAD) + PAD, rows * (h + PAD + LBL) + PAD), (24, 24, 28))
d = ImageDraw.Draw(sheet)
for i, (name, im) in enumerate(imgs):
    x = PAD + (i % COLS) * (w + PAD)
    y = PAD + (i // COLS) * (h + PAD + LBL)
    d.text((x + 2, y + 5), name, fill=(190, 190, 200))
    sheet.paste(im, (x, y + LBL))
sheet.save(os.path.join(outdir, "all-screens.png"))
print(f"wrote {len(imgs)} screens + contact sheet to {outdir}")
PY
