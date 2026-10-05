#!/usr/bin/env python3
"""Build FAT test images: optionally MBR-partitioned, with and without the marker."""
import os, struct, shutil, subprocess, sys
from pyfatfs.PyFatFS import PyFatFS

def mkfs(path, mb, fat_bits, label):
    with open(path, "wb") as f: f.truncate(mb * 1024 * 1024)
    subprocess.run(["/usr/sbin/mkfs.vfat", "-F", str(fat_bits), "-n", label, path],
                   check=True, stdout=subprocess.DEVNULL)

def populate(path, payload_mb):
    fs = PyFatFS(path)
    with fs.openbin("/iprfp3G.dnld", "w") as f: f.write(os.urandom(payload_mb * 1024 * 1024))
    with fs.openbin("/factoryReset", "w") as f: pass
    fs.close()

def drop_marker(path):
    fs = PyFatFS(path); fs.remove("/factoryReset"); fs.close()

def wrap_mbr(fs_path, out_path, part_type=0x0C, start_lba=2048):
    fs_bytes = open(fs_path, "rb").read()
    n = len(fs_bytes) // 512
    mbr = bytearray(512 * start_lba)
    e = 446
    mbr[e+0] = 0x00                      # not bootable
    mbr[e+1:e+4] = b"\xfe\xff\xff"       # CHS, ignored
    mbr[e+4] = part_type
    mbr[e+5:e+8] = b"\xfe\xff\xff"
    struct.pack_into("<II", mbr, e+8, start_lba, n)
    mbr[510:512] = b"\x55\xaa"
    with open(out_path, "wb") as f:
        f.write(mbr); f.write(fs_bytes)

for bits, mb, payload in ((32, 256, 3), (16, 64, 2)):
    tag = f"fat{bits}"
    mkfs(f"{tag}-fs.img", mb, bits, "RFPDATA")
    populate(f"{tag}-fs.img", payload)
    shutil.copy(f"{tag}-fs.img", f"{tag}-fs-del.img")
    drop_marker(f"{tag}-fs-del.img")
    wrap_mbr(f"{tag}-fs.img", f"{tag}-mbr.img")
    wrap_mbr(f"{tag}-fs-del.img", f"{tag}-mbr-del.img")
    # Kept so the tests can prove the "card" was never written to.
    for v in ("fs", "mbr"):
        shutil.copy(f"{tag}-{v}.img", f"{tag}-{v}-pristine.img")
    print(f"built {tag}: superfloppy + MBR-partitioned, each with/without marker")
