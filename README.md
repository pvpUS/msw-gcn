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

The compressed maps (`data/*.mworld`), the atlas (`data/atlas.tpl`), the glyph
sheet (`data/font.tpl`) and the map table (`source/maps_gen.h`) are generated
from the block scans, the resource pack and MCP-919's assets. The block palette
(`data/blockids.txt`) is vendored in the repo, so only the map scans themselves
live outside it. Requires Python 3 with Pillow, plus `gxtexconv` from devkitPro:

```sh
python tools/compress_worlds.py <BlockScans dir> data
python tools/build_atlas.py                       # reads data/blockids.txt
python tools/gen_block_props.py
python tools/gen_blockmap.py                      # proxy/blockmap.json + the HELLO hash
python tools/build_font.py

cd data
gxtexconv -i atlas.png -o atlas.tpl colfmt=5 mipmap=yes minlod=0 maxlod=2
gxtexconv -i font.png  -o font.tpl  colfmt=0 mipmap=no
rm atlas.png atlas.h font.png font.h
```

`colfmt=5` is **RGB5A3** — 16-bit colour with 3-bit alpha, half the size of the
RGBA8 the atlas used to be baked at (2.6 MB against 5.25 MB, all of it
`.rodata` in the DOL and therefore all of it heap you don't get). The world
shader only alpha-*tests*, so 3 bits of alpha is exactly enough, and 16×16
pixel art has too few distinct colours per tile for the 5-bit channels to show.
`colfmt=14` (CMPR, ~0.7 MB) is the emergency lever if more is ever needed, at
the cost of visible DXT1 blocking on pixel art.

`colfmt=0` is **I4** for the font: GX expands an I4 texel to (I,I,I,I), so one
4-bit channel is both the glyph and its cutout.

`mipmap=yes` silently emits a truncated TPL if the source PNG is not
power-of-two — check the output size before trusting it.

## Building

Requires devkitPro with the GameCube toolchain (`DEVKITPPC` / `DEVKITPRO`
exported):

```sh
make
```

Produces `msw-gcn.dol` / `msw-gcn.elf`. `.mworld` and `.tpl` files under `data/`
are embedded via `bin2s`.
