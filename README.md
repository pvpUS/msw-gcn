# msw-gcn

Mega Skywars for the GameCube — a voxel engine that loads compressed Minecraft
map scans, selectable from an in-game menu and textured from a resource pack.
Built with [devkitPPC + libogc](https://devkitpro.org/) (GX for rendering).

At boot you get a **main menu** listing every embedded map with its block count.
Pick one and it decompresses on the console and builds the world; walk around,
mine and build, and press Start to return to the menu. **Y** instead dials the
Node proxy (`proxy/`) and plays a live MegaSkywars game over the Broadband
Adapter.

Controls — the pad has no spare inputs, so this is all of them. Every gameplay
row is rebindable (or unbindable) from **Settings ▸ Controls** in the command
palette; the menu column is fixed, so no remap can leave you unable to reach the
menu that made it.

| | |
|---|---|
| Menu | D-Pad / stick to move, **A** load, **Y** play online, **Start** quit |
| Move / look | main stick, C-stick |
| **L** | mine / attack |
| **Y** | place / use |
| **R** | sprint |
| **A** / **B** | jump / sneak (confirm / cancel in a menu) |
| **X** | inventory screen |
| D-Pad ◀ ▶ | held hotbar slot |
| D-Pad ▲ | command palette (`/join`, `/team`, `/start`, canned chat) |
| D-Pad ▼ | drop item — offline, the perf overlay |
| **Z** | hold to show the chat log |
| **Start** | back to the menu, or the pause menu online |

## Settings

**Start ▸ Settings** in the command palette, with D-Pad ◀ ▶ to change a value:

| | |
|---|---|
| FOV | 30–110°, default 60. The held item keeps its own fixed 60° so it does not slide off the corner of the screen — the same split vanilla makes. |
| View bobbing | On/off. This engine's bob is on the held item, not the camera. |
| Auto sprint | **On by default** — sprints whenever you walk forward, so R is only needed if you turn it off. |
| Sensitivity | 0.5× to 8× the base C-stick look rate. |
| Controls | Rebind any gameplay action to any button, or to **None**. A button used twice is drawn in red rather than refused. |

Menu navigation (D-Pad, **A**, **B**, **Start**) is deliberately *not*
rebindable, so no remap can leave you unable to reach the screen that made it.

**Settings last for the session and are not saved.** Memory card persistence was
built and working — record format, checksum, validation, slot A/B, full
save/load round trip verified against a real card — and then removed, because a
*successful* `CARD_Mount` anywhere in this program stops the world from
rendering: the frame clears to the sky colour and only the display-list geometry
vanishes, while the entire 2D HUD keeps drawing. See the note at the top of
[`source/settings.h`](source/settings.h) for what was ruled out. Persistence can
return once that is understood; nothing about the settings code needs to change
for it.

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

### Wii

```sh
make wii          # msw-wii.dol
make dist         # dist/apps/msw-gcn/ -- copy apps/ to an SD card for the HBC
```

Both consoles build from this one tree and differ in exactly three places: the
toolchain rules, the libraries, and `source/pad.c`. `wii_rules` defines
`HW_RVL`, which every platform `#ifdef` keys off.

**Networking is why this target exists.** Nintendont cannot carry the GameCube
build's traffic — its "BBA emulation" pattern-matches Nintendo SDK socket calls
and redirects them to IOS sockets, while `-lbba` drives the adapter's hardware
over EXI, so no adapter is ever there and `if_config` fails. On Wii, libogc
offers the *same* `if_config`/`net_*` API over the console's own network stack,
so `source/net.c`, GCLink and the proxy are **identical on both platforms** and
DHCP just works.

Controllers, in the order `pad.c` picks them:

| Source | Notes |
|---|---|
| Wii U / Mayflash GameCube adapter | USB, vendor-class. The Mayflash's switch must be in **Wii U** mode (in PC mode it is a different device); only the black data plug is needed. |
| Native controller ports | RVL-001 only — later models, including the RVL-101, have none. |
| Classic Controller / Wii U Pro | The fallback, and the only genuine remap. |

The first backend with something connected wins the whole channel rather than
the sources being merged, because a stick has to come from one place. The boot
screen prints which one answered — that is the difference between "the adapter
isn't detected" and "it is detected and the mapping is wrong", which nothing
else on a console will tell you.

Every backend is translated into *GameCube* button bits and GameCube analog
ranges (`MSW_BTN_*` in `source/pad.h`), so `input.c` and `settings.c` are the
same code on both platforms and the two adapter paths feel exactly like the
console build. Only the Classic Controller is remapped: face buttons and D-pad
keep their names, the analog shoulders keep theirs, the sticks land on main and
C, Plus is Start, and **both** ZL and ZR give the GameCube's single Z.

### Disc image

```sh
make iso
```

Produces `msw-gcn.iso`, a bootable GameCube disc image (~5 MB — it is not padded
out to the 1.36 GB a retail disc uses, since nothing needs it to be). Dolphin,
Swiss and Nintendont take the `.dol` directly; the image is for loaders and
burners that only accept a disc.

`tools/mkiso.py` lays out the GCM and compiles the apploader in
`tools/apploader/`. That apploader is written from the published BS2 interface
rather than lifted from a retail disc, so nothing here depends on owning a
particular game: the bootrom calls `main()` repeatedly, and each call names the
next chunk of disc to copy and where it goes, so walking the DOL's section table
is the whole job. `mkiso.py` patches the disc offset of the `.dol` into the
built apploader and pads the raw image out to `_end`, because nothing on the
console clears the apploader's own `.bss`.

Two constraints there that Dolphin does not enforce, so a change breaking either
boots fine in the emulator and black-screens everywhere else:

- **No transfer may exceed 64 DVD sectors (128 KB)**, so a section bigger than
  that is handed back a chunk per `main()` call. Dolphin will quite happily read
  the whole 4.9 MB data section in one go.
- **Flush the loaded sections, don't invalidate them.** A console's DVD DMA
  writes behind the data cache, but a loader emulating the drive off SD or USB
  (Nintendont, Swiss) copies with the CPU and leaves those lines dirty — `dcbi`
  there throws the game away right before the jump into it.
