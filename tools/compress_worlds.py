#!/usr/bin/env python3
"""
compress_worlds.py -- pack Mega Skywars block-scan dumps into the .mworld
container consumed by the GameCube client.

Input : a directory of BlockScans/*.txt, each line "x y z BLOCK_ID".
        _blockids.txt lists every block id we must support (global palette).
Output: <out>/<map>.mworld  (one per scan) and <out>/maps_manifest.txt.

The codec is two independent layers, both re-implemented in C on the console:

  1. Structural transform - the point cloud is bucketed into vertical columns
     (the scans are effectively column-major already), each column encoded as
     air-gap / solid-run segments with a per-map palette. This strips the
     coordinate redundancy that dominates a raw dump.

  2. LZSS backend - a 64 KiB-window byte LZ over the structural stream. This is
     what actually captures cross-column repetition (repeated floors, walls,
     identical towers) that per-column RLE cannot see.

Coordinates are stored relative to the spawn point (the scans are already
spawn-centred at the origin); the header records the spawn and the bounding-box
origin so world = origin + grid position = spawn-relative coordinate.

Everything here is round-trip verified against a reference decoder before a file
is written, so the C decoder only has to match this spec.
"""

import os
import sys
import struct
import zlib   # only for a ratio reference in the report, never shipped

MAGIC = b"MWL1"
VERSION = 1

# ---------------------------------------------------------------------------
# varint (unsigned LEB128)
# ---------------------------------------------------------------------------
def put_uvarint(buf, v):
    assert v >= 0
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            buf.append(b | 0x80)
        else:
            buf.append(b)
            return

def get_uvarint(data, pos):
    shift = 0
    result = 0
    while True:
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, pos
        shift += 7

# ---------------------------------------------------------------------------
# LZSS  (flag-byte grouping, 16-bit distance, 8-bit length, min match 3)
# ---------------------------------------------------------------------------
MIN_MATCH = 3
MAX_MATCH = MIN_MATCH + 255      # 258
WINDOW = 65536                   # distance stored as (d-1) in a u16
MAX_CHAIN = 64                   # matcher effort cap (speed vs ratio)

def lz_compress(src):
    n = len(src)
    out = bytearray()
    # hash chains over 3-byte prefixes
    HSIZE = 1 << 16
    head = [-1] * HSIZE
    prev = [-1] * n

    def h3(i):
        return ((src[i] << 12) ^ (src[i + 1] << 6) ^ src[i + 2]) & (HSIZE - 1)

    def find(i, best_prev):
        """longest match for position i; returns (length, distance)."""
        if i + MIN_MATCH > n:
            return 0, 0
        j = head[h3(i)]
        best_len = MIN_MATCH - 1
        best_dist = 0
        limit = MAX_CHAIN
        max_here = min(MAX_MATCH, n - i)
        while j >= 0 and limit > 0:
            if i - j > WINDOW:
                break
            # quick reject on the byte past the current best
            if best_len < max_here and src[j + best_len] == src[i + best_len]:
                l = 0
                while l < max_here and src[j + l] == src[i + l]:
                    l += 1
                if l > best_len:
                    best_len = l
                    best_dist = i - j
                    if l >= max_here:
                        break
            j = prev[j]
            limit -= 1
        if best_len >= MIN_MATCH:
            return best_len, best_dist
        return 0, 0

    def insert(i):
        if i + MIN_MATCH > n:
            return
        hv = h3(i)
        prev[i] = head[hv]
        head[hv] = i

    i = 0
    flag_pos = 0
    flag_bit = 0
    flag_val = 0
    # we build tokens then flush per 8
    tokens = bytearray()

    def emit_flag_block():
        nonlocal flag_val, flag_bit, tokens
        out.append(flag_val)
        out.extend(tokens)
        tokens = bytearray()
        flag_val = 0
        flag_bit = 0

    while i < n:
        mlen, mdist = find(i, -1)
        # lazy: check i+1 for a strictly longer match
        if mlen >= MIN_MATCH and i + 1 < n:
            insert(i)
            n2, d2 = find(i + 1, -1)
            if n2 > mlen:
                # emit literal at i, defer to i+1
                tokens.append(src[i])          # literal (flag bit 0)
                flag_bit += 1
                if flag_bit == 8:
                    emit_flag_block()
                i += 1
                continue
            # fall through and emit match at i (already inserted)
            token_match = True
        elif mlen >= MIN_MATCH:
            insert(i)
            token_match = True
        else:
            token_match = False

        if token_match:
            d = mdist - 1
            L = mlen - MIN_MATCH
            tokens.append((d >> 8) & 0xFF)
            tokens.append(d & 0xFF)
            tokens.append(L & 0xFF)
            flag_val |= (1 << flag_bit)
            flag_bit += 1
            if flag_bit == 8:
                emit_flag_block()
            # insert the covered positions into the hash chains
            for k in range(1, mlen):
                if i + k + MIN_MATCH <= n:
                    insert(i + k)
            i += mlen
        else:
            insert(i)
            tokens.append(src[i])
            flag_bit += 1
            if flag_bit == 8:
                emit_flag_block()
            i += 1

    if flag_bit != 0:
        emit_flag_block()
    return bytes(out)

def lz_decompress(comp, expected):
    out = bytearray()
    pos = 0
    clen = len(comp)
    while len(out) < expected:
        flags = comp[pos]; pos += 1
        for bit in range(8):
            if len(out) >= expected:
                break
            if flags & (1 << bit):
                d = (comp[pos] << 8) | comp[pos + 1]; pos += 2
                L = comp[pos]; pos += 3 - 2  # advance length byte
                dist = d + 1
                mlen = L + MIN_MATCH
                start = len(out) - dist
                for k in range(mlen):
                    out.append(out[start + k])
            else:
                out.append(comp[pos]); pos += 1
    return bytes(out)

# ---------------------------------------------------------------------------
# global palette (block id -> index, from _blockids.txt)
# ---------------------------------------------------------------------------
def load_global_ids(path):
    ids = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            s = line.strip()
            if s:
                ids.append(s)
    id_to_global = {name: i for i, name in enumerate(ids)}
    return ids, id_to_global

# ---------------------------------------------------------------------------
# structural encode
# ---------------------------------------------------------------------------
def encode_map(lines, id_to_global):
    # parse
    pts = []  # (x,y,z,globalid)
    local_of = {}
    palette = []  # global ids in local order
    minx = miny = minz = None
    maxx = maxy = maxz = None
    unknown = set()
    for line in lines:
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 4:
            continue
        x = int(parts[0]); y = int(parts[1]); z = int(parts[2])
        name = parts[3]
        g = id_to_global.get(name)
        if g is None:
            unknown.add(name)
            continue
        if name not in local_of:
            local_of[name] = len(palette)
            palette.append(g)
        li = local_of[name]
        pts.append((x, y, z, li))
        if minx is None:
            minx = maxx = x; miny = maxy = y; minz = maxz = z
        else:
            if x < minx: minx = x
            if x > maxx: maxx = x
            if y < miny: miny = y
            if y > maxy: maxy = y
            if z < minz: minz = z
            if z > maxz: maxz = z

    if not pts:
        return None, unknown

    dimx = maxx - minx + 1
    dimy = maxy - miny + 1
    dimz = maxz - minz + 1

    # bucket into columns: col index = (x-minx)*dimz + (z-minz)
    columns = {}
    for (x, y, z, li) in pts:
        c = (x - minx) * dimz + (z - minz)
        columns.setdefault(c, []).append((y - miny, li))

    idbytes = 1 if len(palette) <= 256 else 2

    S = bytearray()
    col_keys = sorted(columns.keys())
    prev_c = 0
    for c in col_keys:
        cells = columns[c]
        cells.sort()  # by y then li
        # collapse to segments of contiguous y with equal palette index
        segs = []  # (ystart, run, li)
        cy = None
        cli = None
        cstart = None
        crun = 0
        for (y, li) in cells:
            if cy is not None and y == cy and li == cli:
                # duplicate coordinate/id, skip
                continue
            if cy is not None and y == cy + 1 and li == cli:
                crun += 1
                cy = y
                continue
            if cy is not None:
                segs.append((cstart, crun, cli))
            cstart = y; crun = 1; cli = li; cy = y
        if cy is not None:
            segs.append((cstart, crun, cli))

        put_uvarint(S, c - prev_c)          # skip (first is absolute index)
        prev_c = c
        put_uvarint(S, len(segs))
        prev_end = 0
        for (ystart, run, li) in segs:
            put_uvarint(S, ystart - prev_end)   # air gap
            put_uvarint(S, run - 1)             # run length (>=1)
            if idbytes == 1:
                S.append(li)
            else:
                S.append((li >> 8) & 0xFF)
                S.append(li & 0xFF)
            prev_end = ystart + run

    header = bytearray()
    header += MAGIC
    header.append(VERSION)
    header.append(idbytes)
    header += struct.pack(">H", len(palette))
    header += struct.pack(">hhh", minx, miny, minz)      # origin (spawn-relative)
    header += struct.pack(">HHH", dimx, dimy, dimz)
    header += struct.pack(">hhh", 0, 0, 0)               # spawn point (scan origin)
    header += struct.pack(">I", len(pts))
    header += struct.pack(">I", len(col_keys))           # non-empty column count
    header += struct.pack(">I", len(S))                  # raw structural size
    for g in palette:
        header += struct.pack(">H", g)

    comp = lz_compress(bytes(S))
    blob = bytes(header) + comp

    meta = dict(dimx=dimx, dimy=dimy, dimz=dimz, minx=minx, miny=miny, minz=minz,
                blocks=len(pts), palette=len(palette), rawS=len(S),
                comp=len(comp), header=len(header), idbytes=idbytes)
    return (blob, S, meta), unknown

# ---------------------------------------------------------------------------
# reference decode  (mirrors what the C client must do)
# ---------------------------------------------------------------------------
def decode_and_verify(blob, S_expected, meta):
    # header parse
    assert blob[:4] == MAGIC
    idbytes = blob[5]
    pcount = struct.unpack(">H", blob[6:8])[0]
    minx, miny, minz = struct.unpack(">hhh", blob[8:14])
    dimx, dimy, dimz = struct.unpack(">HHH", blob[14:20])
    spx, spy, spz = struct.unpack(">hhh", blob[20:26])
    blocks = struct.unpack(">I", blob[26:30])[0]
    ncol = struct.unpack(">I", blob[30:34])[0]
    rawS = struct.unpack(">I", blob[34:38])[0]
    off = 38
    palette = []
    for i in range(pcount):
        palette.append(struct.unpack(">H", blob[off:off+2])[0]); off += 2
    comp = blob[off:]
    S = lz_decompress(comp, rawS)
    assert S == S_expected, "LZSS round-trip mismatch"

    # structural decode -> set of (x,y,z,li)
    voxels = {}
    pos = 0
    c = 0
    total = 0
    for _ in range(ncol):
        skip, pos = get_uvarint(S, pos)
        c += skip
        x = c // dimz
        z = c % dimz
        nseg, pos = get_uvarint(S, pos)
        y = 0
        for _s in range(nseg):
            gap, pos = get_uvarint(S, pos)
            run, pos = get_uvarint(S, pos); run += 1
            if idbytes == 1:
                li = S[pos]; pos += 1
            else:
                li = (S[pos] << 8) | S[pos+1]; pos += 2
            y += gap
            for k in range(run):
                voxels[(x, y + k, z)] = li
            y += run
            total += run
    assert total == blocks, f"block count mismatch {total} != {blocks}"
    return True

# ---------------------------------------------------------------------------
def main():
    src_dir = sys.argv[1] if len(sys.argv) > 1 else \
        r"C:\Users\awt12\Downloads\download (1)\BlockScans"
    out_dir = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.join(os.path.dirname(__file__), "..", "data")
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    ids_path = os.path.join(src_dir, "_blockids.txt")
    global_ids, id_to_global = load_global_ids(ids_path)
    print(f"global palette: {len(global_ids)} block ids")

    manifest = []
    all_unknown = set()
    total_raw_txt = 0
    total_blob = 0
    total_zlib_ref = 0

    files = sorted(f for f in os.listdir(src_dir)
                   if f.endswith(".txt") and not f.startswith("_"))
    for fn in files:
        path = os.path.join(src_dir, fn)
        raw_txt = os.path.getsize(path)
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
        res, unknown = encode_map(lines, id_to_global)
        all_unknown |= unknown
        if res is None:
            print(f"  {fn}: empty, skipped")
            continue
        blob, S, meta = res
        decode_and_verify(blob, S, meta)   # will raise on any mismatch

        name = os.path.splitext(fn)[0]
        outp = os.path.join(out_dir, name + ".mworld")
        with open(outp, "wb") as f:
            f.write(blob)

        # reference: how well would zlib do on the same structural stream?
        z = len(zlib.compress(bytes(S), 9))
        naive = meta["blocks"] * 8   # s16 x,y,z + u16 id
        total_raw_txt += raw_txt
        total_blob += len(blob)
        total_zlib_ref += z + meta["header"]

        manifest.append((name, meta, len(blob)))
        print(f"  {name:18s} {meta['blocks']:7d} blk  "
              f"dim {meta['dimx']:3d}x{meta['dimy']:3d}x{meta['dimz']:3d}  "
              f"txt {raw_txt/1024:7.1f}K  naive {naive/1024:7.1f}K  "
              f"structS {meta['rawS']/1024:7.1f}K  "
              f"OURS {len(blob)/1024:7.1f}K  (zlib-ref {(z+meta['header'])/1024:7.1f}K)")

    print("\n--- totals ---")
    print(f"  raw text  : {total_raw_txt/1024/1024:.2f} MB")
    print(f"  our .mworld: {total_blob/1024/1024:.2f} MB  "
          f"({total_raw_txt/max(total_blob,1):.1f}x vs text)")
    print(f"  zlib ref  : {total_zlib_ref/1024/1024:.2f} MB (structural+zlib, not shipped)")
    if all_unknown:
        print(f"\n  WARNING: {len(all_unknown)} block ids not in _blockids.txt (dropped): "
              f"{sorted(all_unknown)[:20]}")

    # generated table for the client: maps_gen.h in source/.
    # bin2s emits `<name>_mworld[]` and `<name>_mworld_end[]` (both address
    # constants, valid in a static initializer) plus a size const that is NOT a
    # C constant-expression -- so we carry data+end and derive size at runtime.
    def pretty(n):
        return n.replace("_", " ").title()

    src_dir_out = os.path.join(os.path.dirname(__file__), "..", "source")
    src_dir_out = os.path.abspath(src_dir_out)
    hpath = os.path.join(src_dir_out, "maps_gen.h")
    with open(hpath, "w", encoding="utf-8") as f:
        f.write("/* Generated by tools/compress_worlds.py -- do not edit. */\n")
        f.write("#ifndef MSW_MAPS_GEN_H\n#define MSW_MAPS_GEN_H\n\n")
        f.write('#include "maps.h"\n\n')
        for name, meta, blen in manifest:
            f.write(f'#include "{name}_mworld.h"\n')
        f.write("\nstatic const MapEntry g_maps[] = {\n")
        for name, meta, blen in manifest:
            f.write(f'\t{{ "{pretty(name)}", {name}_mworld, '
                    f'{name}_mworld_end, {meta["blocks"]} }},\n')
        f.write("};\n\n")
        f.write("#define MAP_COUNT ((int)(sizeof(g_maps)/sizeof(g_maps[0])))\n\n")
        f.write("#endif\n")
    print(f"\n  wrote {hpath} ({len(manifest)} maps)")

if __name__ == "__main__":
    main()
