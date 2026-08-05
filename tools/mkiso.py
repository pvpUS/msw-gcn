#!/usr/bin/env python3
"""Build a bootable GameCube disc image from msw-gcn.dol.

Usage: python tools/mkiso.py [--dol msw-gcn.dol] [--out msw-gcn.iso]

Lays out a standard GCM:

    0x00000000  boot.bin        disc header, game id, and the offsets below
    0x00000440  bi2.bin         country code + simulated memory size
    0x00002440  apploader       header + tools/apploader (built here)
                fst.bin         root entry only, no user files
                main.dol        the game

The apploader is compiled from tools/apploader/ with devkitPPC, so no part of
a retail disc is needed to make this bootable.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Disc identity. 'G' marks a GameCube disc, then a 2-char game code, then the
# region ('E' = NTSC-U, matching the country code written into bi2.bin), then a
# maker code -- 'HB' rather than a real publisher's so this cannot collide with
# a retail game id in Dolphin's game list.
GAME_ID = b"GSWEHB"
GAME_NAME = b"Mega Skywars"

APPLOADER_OFF = 0x2440
APPLOADER_VERSION = b"msw-gcn"

BOOT_BIN_SIZE = 0x440
BI2_SIZE = 0x2000
SIMULATED_MEM_SIZE = 0x01800000
COUNTRY_USA = 1

CFG_MAGIC = 0x4D535749  # 'MSWI', the patch point inside the apploader


def align(x, n):
    return (x + n - 1) & ~(n - 1)


def find_tool(name):
    """Locate a devkitPPC binary without requiring it on PATH."""
    exe = shutil.which(name)
    if exe:
        return exe
    for base in (os.environ.get("DEVKITPPC"), "C:/devkitPro/devkitPPC",
                 "/opt/devkitpro/devkitPPC"):
        if not base:
            continue
        for cand in (os.path.join(base, "bin", name),
                     os.path.join(base, "bin", name + ".exe")):
            if os.path.isfile(cand):
                return cand
    sys.exit("error: %s not found; set DEVKITPPC or put devkitPPC on PATH" % name)


def build_apploader(builddir):
    """Compile tools/apploader into a raw image, returning (blob, entry)."""
    src = os.path.join(ROOT, "tools", "apploader", "apploader.c")
    ld = os.path.join(ROOT, "tools", "apploader", "apploader.ld")
    elf = os.path.join(builddir, "apploader.elf")

    gcc = find_tool("powerpc-eabi-gcc")
    objcopy = find_tool("powerpc-eabi-objcopy")
    nm = find_tool("powerpc-eabi-nm")

    # gcc needs a writable %TEMP% for its intermediates; inherited environments
    # here sometimes point it at an unwritable directory, so pin it to build/.
    env = dict(os.environ)
    env["TMP"] = env["TEMP"] = env["TMPDIR"] = builddir

    subprocess.run([
        gcc, "-o", elf, src,
        "-mcpu=750", "-msdata=none", "-G0",
        "-Os", "-Wall", "-ffreestanding", "-fno-builtin",
        "-fno-asynchronous-unwind-tables", "-fno-strict-aliasing",
        "-nostdlib", "-nostartfiles", "-Wl,-T," + ld, "-Wl,--build-id=none",
        # gcc calls libgcc's out-of-line _savegpr_/_restgpr_ helpers when it
        # optimises for size, so libgcc has to be on the link line even though
        # nothing else from the toolchain's runtime is used.
        "-lgcc",
    ], check=True, env=env, cwd=builddir)

    syms = {}
    out = subprocess.run([nm, elf], check=True, capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)

    entry = syms["_start"]
    end = syms["_end"]
    base = 0x81200000

    binpath = os.path.join(builddir, "apploader.bin")
    subprocess.run([objcopy, "-O", "binary", elf, binpath], check=True, env=env)
    blob = bytearray(open(binpath, "rb").read())

    # objcopy stops at the end of .data; pad with the zeros the apploader's
    # .bss expects, since nothing on the console will clear it for us.
    blob += b"\0" * (end - base - len(blob))
    if len(blob) != end - base:
        sys.exit("error: apploader image is %#x bytes but _end says %#x"
                 % (len(blob), end - base))

    return blob, entry


def patch_dol_offset(blob, dol_off):
    """Tell the apploader where the .dol landed on the disc."""
    magic = struct.pack(">I", CFG_MAGIC)
    at = blob.find(magic)
    if at < 0 or blob.find(magic, at + 4) >= 0:
        sys.exit("error: expected exactly one %r patch point in the apploader" % magic)
    blob[at + 4:at + 8] = struct.pack(">I", dol_off)


def build_fst():
    """Root directory entry and an empty string table -- no user files."""
    # flags/name-offset, parent index, number of entries (the root counts).
    return struct.pack(">III", 1 << 24, 0, 1) + b"\0"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dol", default=os.path.join(ROOT, "msw-gcn.dol"))
    ap.add_argument("--out", default=os.path.join(ROOT, "msw-gcn.iso"))
    ap.add_argument("--builddir", default=os.path.join(ROOT, "build"))
    args = ap.parse_args()

    if not os.path.isfile(args.dol):
        sys.exit("error: %s not found -- run make first" % args.dol)
    os.makedirs(args.builddir, exist_ok=True)

    dol = open(args.dol, "rb").read()
    apploader, apploader_entry = build_apploader(args.builddir)

    fst = build_fst()
    fst_off = align(APPLOADER_OFF + 0x20 + len(apploader), 0x100)
    dol_off = align(fst_off + len(fst), 0x8000)
    user_off = align(dol_off + len(dol), 0x8000)

    patch_dol_offset(apploader, dol_off)

    # --- boot.bin -------------------------------------------------------
    boot = bytearray(BOOT_BIN_SIZE)
    boot[0x00:0x06] = GAME_ID
    boot[0x06] = 0                                        # disc number
    boot[0x07] = 0                                        # version
    boot[0x08] = 0                                        # audio streaming off
    boot[0x09] = 0                                        # stream buffer size
    struct.pack_into(">I", boot, 0x1C, 0xC2339F3D)        # GameCube magic word
    boot[0x20:0x20 + len(GAME_NAME)] = GAME_NAME
    struct.pack_into(">I", boot, 0x420, dol_off)
    struct.pack_into(">I", boot, 0x424, fst_off)
    struct.pack_into(">I", boot, 0x428, len(fst))
    struct.pack_into(">I", boot, 0x42C, len(fst))         # max fst size
    struct.pack_into(">I", boot, 0x430, user_off)
    struct.pack_into(">I", boot, 0x434, 0)                # no user files

    # --- bi2.bin --------------------------------------------------------
    bi2 = bytearray(BI2_SIZE)
    struct.pack_into(">I", bi2, 0x00, 0)                  # no debug monitor
    struct.pack_into(">I", bi2, 0x04, SIMULATED_MEM_SIZE)
    struct.pack_into(">I", bi2, 0x08, 0)                  # argument offset
    struct.pack_into(">I", bi2, 0x0C, 0)                  # debug flag: none
    struct.pack_into(">I", bi2, 0x18, COUNTRY_USA)
    struct.pack_into(">I", bi2, 0x1C, 1)

    # --- apploader header -----------------------------------------------
    head = bytearray(0x20)
    head[0x00:0x00 + len(APPLOADER_VERSION)] = APPLOADER_VERSION
    struct.pack_into(">I", head, 0x10, apploader_entry)
    struct.pack_into(">I", head, 0x14, align(len(apploader), 32))
    struct.pack_into(">I", head, 0x18, 0)                 # no trailer
    apploader += b"\0" * (align(len(apploader), 32) - len(apploader))

    # --- assemble --------------------------------------------------------
    img = bytearray()

    def place(off, data):
        if len(img) > off:
            sys.exit("error: layout overlap at %#x" % off)
        img.extend(b"\0" * (off - len(img)))
        img.extend(data)

    place(0x00000000, boot)
    place(BOOT_BIN_SIZE, bi2)
    place(APPLOADER_OFF, head + apploader)
    place(fst_off, fst)
    place(dol_off, dol)
    img.extend(b"\0" * (align(len(img), 0x8000) - len(img)))

    with open(args.out, "wb") as f:
        f.write(img)

    print("%s: %.2f MB" % (args.out, len(img) / 1048576.0))
    print("  id %s  apploader %d bytes (entry %08x)"
          % (GAME_ID.decode(), len(apploader), apploader_entry))
    print("  fst %#x  dol %#x (%d bytes)" % (fst_off, dol_off, len(dol)))


if __name__ == "__main__":
    main()
