#!/usr/bin/env python3
"""
gen_model_gallery.py -- writes model_gallery.txt, a synthetic scan (same
"x y z BLOCK_ID" format real BlockScans/*.txt dumps use) that lays every
custom block-shape family (slab/stair/fence/wall/pane/anvil/enchant
table/trapdoor/door -- see block_shapes.h) out on a stone shelf, one row per
family, one column per orientation/param variant that exists in
_blockids.txt.

Written straight into the existing BlockScans source dir so the next
`compress_worlds.py` run picks it up like any other map -- no changes to
that pipeline. Also emits source/gallery_tour_gen.h: one camera keyframe
framing each row head-on (index order matches MODEL_TEST_ROW in main.c),
plus a top-down overview keyframe at the end.

Re-run this + tools/compress_worlds.py any time a row's cell list changes.
"""
import os

SRC_DIR = r"C:\Users\awt12\Downloads\download (1)\BlockScans"
OUT_TXT = os.path.join(SRC_DIR, "model_gallery.txt")
OUT_LEGEND = os.path.join(SRC_DIR, "_model_gallery_legend.txt")  # leading _ = skipped by compress_worlds.py's scan glob
OUT_HEADER = os.path.join(os.path.dirname(__file__), "..", "source", "gallery_tour_gen.h")

COL_SPACING = 4   # x pitch between cells within a row
ROW_SPACING = 6   # z pitch between rows
FLOOR_Y = 0        # stone shelf surface
BLOCK_Y = 1        # test blocks / markers sit one block above the floor
MARKER_COL = -1    # column slot (relative to row start) used by the row's wool marker


def stairs_id(d):
    return "WOOD_STAIRS" if d == 0 else f"WOOD_STAIRS:{d}"


def anvil_id(d):
    return "ANVIL" if d == 0 else f"ANVIL:{d}"


def trapdoor_id(base, d):
    return base if d == 0 else f"{base}:{d}"


# Each row: (name, marker_wool_data, [(label, [(dx, dz, block_id), ...]), ...])
# (dx, dz) are relative to the cell's own column origin; the placement list
# lets a cell drop extra neighbor blocks (solid neighbor / same-shape line)
# needed to exercise FENCE/WALL/PANE connectivity.
ROWS = [
    ("slab", 5, [
        ("STEP (bottom half)", [(0, 0, "STEP")]),
        ("STEP:8 (top half)",  [(0, 0, "STEP:8")]),
    ]),
    ("stairs", 11, [
        (stairs_id(d), [(0, 0, stairs_id(d))]) for d in range(8)
    ]),
    ("fence", 5, [
        ("FENCE isolated",         [(0, 0, "FENCE")]),
        ("FENCE + solid neighbor", [(0, 0, "FENCE"), (1, 0, "STONE")]),
        ("FENCE line x3",          [(-1, 0, "FENCE"), (0, 0, "FENCE"), (1, 0, "FENCE")]),
    ]),
    ("fence_gate", 11, [
        (f"DARK_OAK_FENCE_GATE:{d}", [(0, 0, f"DARK_OAK_FENCE_GATE:{d}")])
        for d in [1, 2, 3, 6, 7]
    ]),
    ("wall", 8, [
        ("COBBLE_WALL isolated",         [(0, 0, "COBBLE_WALL")]),
        ("COBBLE_WALL + solid neighbor", [(0, 0, "COBBLE_WALL"), (1, 0, "STONE")]),
        ("COBBLE_WALL line x3",          [(-1, 0, "COBBLE_WALL"), (0, 0, "COBBLE_WALL"), (1, 0, "COBBLE_WALL")]),
        ("COBBLE_WALL:1",                [(0, 0, "COBBLE_WALL:1")]),
    ]),
    ("pane", 3, [
        ("STAINED_GLASS_PANE isolated",         [(0, 0, "STAINED_GLASS_PANE")]),
        ("STAINED_GLASS_PANE + solid neighbor", [(0, 0, "STAINED_GLASS_PANE"), (1, 0, "STONE")]),
        ("STAINED_GLASS_PANE line x3",          [(-1, 0, "STAINED_GLASS_PANE"), (0, 0, "STAINED_GLASS_PANE"), (1, 0, "STAINED_GLASS_PANE")]),
        ("IRON_FENCE isolated (iron bars)",     [(0, 0, "IRON_FENCE")]),
    ]),
    ("anvil", 14, [
        (anvil_id(d), [(0, 0, anvil_id(d))]) for d in [0, 1, 2, 3, 5, 7, 9]
    ]),
    ("enchant_table", 10, [
        ("ENCHANTMENT_TABLE", [(0, 0, "ENCHANTMENT_TABLE")]),
    ]),
    ("trapdoor", 1, [
        (trapdoor_id("TRAP_DOOR", d), [(0, 0, trapdoor_id("TRAP_DOOR", d))])
        for d in range(16)
    ] + [
        (trapdoor_id("IRON_TRAPDOOR", d), [(0, 0, trapdoor_id("IRON_TRAPDOOR", d))])
        for d in [0, 1, 2, 3, 12, 13]
    ]),
    ("door", 14, [
        ("WOODEN_DOOR:1", [(0, 0, "WOODEN_DOOR:1")]),
        ("WOODEN_DOOR:2", [(0, 0, "WOODEN_DOOR:2")]),
        ("WOODEN_DOOR:8", [(0, 0, "WOODEN_DOOR:8")]),
        ("JUNGLE_DOOR:9", [(0, 0, "JUNGLE_DOOR:9")]),
    ]),
]


def main():
    voxels = {}   # (x,y,z) -> block id string, last write wins
    legend_lines = []
    keyframes = []  # (name, x, y, z, yaw, pitch)

    all_x = []
    all_z = []

    for row_i, (row_name, marker_wool, cells) in enumerate(ROWS):
        row_z = row_i * ROW_SPACING
        legend_lines.append(f"\nrow {row_i}: {row_name}  (wool data {marker_wool} marks the row start)")

        marker_x = MARKER_COL * COL_SPACING
        voxels[(marker_x, BLOCK_Y, row_z)] = "WOOL" if marker_wool == 0 else f"WOOL:{marker_wool}"
        all_x.append(marker_x)
        all_z.append(row_z)

        for col_i, (label, placements) in enumerate(cells):
            col_x = col_i * COL_SPACING
            legend_lines.append(f"  col {col_i:2d} (x={col_x:3d}): {label}")
            for dx, dz, bid in placements:
                x, z = col_x + dx, row_z + dz
                voxels[(x, BLOCK_Y, z)] = bid
                all_x.append(x)
                all_z.append(z)

        num_cells = len(cells)
        row_width = (num_cells - 1) * COL_SPACING
        cam_x = row_width / 2.0
        # Every row shares the same Z corridor and -Z look direction, so any
        # row with a higher index (closer to the camera along +Z) would
        # otherwise sit between the camera and its actual target and steal
        # the frame. A steep downward pitch from well above the shelf keeps
        # the shot centered on the target row's own footprint instead of the
        # whole corridor -- near-ground clutter from other rows falls below
        # the frame instead of dominating it.
        cam_dist = 5.0 + row_width * 0.55
        cam_y = BLOCK_Y + 9.0 + row_width * 0.16
        keyframes.append((row_name, cam_x, cam_y, row_z + cam_dist, 0.0, -32.0))

    # floor: a solid stone plate under the whole gallery, +1 block margin
    min_x, max_x = min(all_x) - 1, max(all_x) + 1
    min_z, max_z = min(all_z) - 1, max(all_z) + 1
    for x in range(min_x, max_x + 1):
        for z in range(min_z, max_z + 1):
            voxels.setdefault((x, FLOOR_Y, z), "STONE")

    # top-down overview keyframe
    cx, cz = (min_x + max_x) / 2.0, (min_z + max_z) / 2.0
    half_span = max(max_x - min_x, max_z - min_z) / 2.0
    overview_y = BLOCK_Y + half_span / 0.7 + 4.0
    keyframes.append(("overview", cx, overview_y, cz, 0.0, -85.0))

    os.makedirs(SRC_DIR, exist_ok=True)
    with open(OUT_TXT, "w", encoding="utf-8") as f:
        for (x, y, z), bid in sorted(voxels.items()):
            f.write(f"{x} {y} {z} {bid}\n")
    print(f"wrote {OUT_TXT} ({len(voxels)} voxels)")

    with open(OUT_LEGEND, "w", encoding="utf-8") as f:
        f.write("model_gallery legend -- not compiled, for reading screenshots only\n")
        f.write("".join(l + "\n" for l in legend_lines))
    print(f"wrote {OUT_LEGEND}")

    os.makedirs(os.path.dirname(OUT_HEADER), exist_ok=True)
    with open(OUT_HEADER, "w", encoding="utf-8") as f:
        f.write("/* Generated by tools/gen_model_gallery.py -- do not edit. */\n")
        f.write("#ifndef MSW_GALLERY_TOUR_GEN_H\n#define MSW_GALLERY_TOUR_GEN_H\n\n")
        f.write('#include "camera.h"\n\n')
        for i, (name, *_rest) in enumerate(keyframes):
            f.write(f"#define GALLERY_ROW_{name.upper()} {i}\n")
        f.write(f"\n#define GALLERY_TOUR_COUNT {len(keyframes)}\n\n")
        f.write("static const CamKeyframe g_galleryTour[GALLERY_TOUR_COUNT] = {\n")
        for name, x, y, z, yaw, pitch in keyframes:
            f.write(f'\t{{ {x:.2f}f, {y:.2f}f, {z:.2f}f, {yaw:.2f}f, {pitch:.2f}f, "{name}" }},\n')
        f.write("};\n\n#endif\n")
    print(f"wrote {OUT_HEADER} ({len(keyframes)} keyframes)")


if __name__ == "__main__":
    main()
