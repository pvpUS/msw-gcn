# msw-gcn

Mega Skywars for the GameCube — a voxel engine that loads compressed Minecraft
map scans, selectable from an in-game menu and textured from a resource pack.
Built with [devkitPPC + libogc](https://devkitpro.org/) (GX for rendering).

At boot you get a **main menu** listing every embedded map with its block count.
Pick one and it decompresses on the console and builds the world; fly around and
press Start to return to the menu.

Controls:
- **Menu:** D-Pad / main stick to move the selection, **A** to load, **Start** to quit.
- **In a world:** main stick to fly forward/strafe, C-stick to look, **A/B** to rise/fall, **Start** back to the menu.

## World format & compression

Maps come from block-scan dumps (`x y z BLOCK_ID` per line, spawn-centred at the
origin). `tools/compress_worlds.py` packs each into a `.mworld` blob using a
two-layer codec that is re-implemented on the console:

1. **Structural transform** — the point cloud is bucketed into vertical columns
   (coordinates stored relative to the spawn point), each column encoded as
   air-gap / solid-run segments against a per-map palette. This strips the
   coordinate redundancy that dominates a raw dump.
2. **LZSS backend** — a 64 KiB-window byte LZ over the structural stream, which
   captures the cross-column repetition (repeated floors, walls, towers) that
   per-column RLE cannot see.

Across the 31 sample maps this is **~49× smaller than the text dumps** (84 MB →
1.7 MB), and every blob is round-trip verified against a reference decoder before
it is written. The console decoder lives in [source/lz.c](source/lz.c) and
[source/world.c](source/world.c).

Each world is rebuilt into a 1-bit occupancy grid for face culling, then meshed
once into a GX display list (exposed faces only, atlas-textured, with
Minecraft-style directional shading) that is replayed each frame.

## Textures

`tools/build_atlas.py` assembles a single 512×512 texture atlas — one 16×16 tile
per block id — from a resource pack (defaults to the RKYfault pack path in the
script). The tile index is the block's global id, so the console derives UVs
directly with no side table. Variants the pack lacks per-colour (stained clay /
glass) are synthesised by tinting a neutral base.

## Regenerating the embedded data

The compressed maps (`data/*.mworld`), the atlas (`data/atlas.tpl`) and the map
table (`source/maps_gen.h`) are generated from the block scans and resource pack.
Requires Python 3 with Pillow, plus `gxtexconv` from devkitPro:

```sh
python tools/compress_worlds.py <BlockScans dir> data
python tools/build_atlas.py     <BlockScans>/_blockids.txt data
gxtexconv -i data/atlas.png -o data/atlas.tpl colfmt=6 && rm data/atlas.png
```

## Building

Requires devkitPro with the GameCube toolchain (`DEVKITPPC` / `DEVKITPRO`
exported):

```sh
make
```

Produces `msw-gcn.dol` / `msw-gcn.elf`. `.mworld` and `.tpl` files under `data/`
are embedded via `bin2s`.
