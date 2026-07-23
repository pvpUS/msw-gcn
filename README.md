# msw-gcn
Mega Skywars - a voxel-island tech demo with a flying camera, built for two consoles:

- [`gcn/`](gcn/) - GameCube version, built with [devkitPPC + libogc](https://devkitpro.org/) (GX for rendering).
- [`n64/`](n64/) - Nintendo 64 port, built with [libdragon](https://libdragon.dev/) (RDP triangle rasterizer, transform/clipping done on the CPU).

Controls (same on both): main stick to fly forward/strafe, look stick/C-buttons to look around, A/B to rise/fall, Start to quit.

## Building the GameCube version

Requires devkitPro with the GameCube toolchain (`DEVKITPPC` exported):

```sh
cd gcn
make
```

Produces `msw-gcn.dol` / `msw-gcn.elf`.

## Building the N64 version

Requires the [libdragon toolchain](https://libdragon.dev/get-started/) (`N64_INST` exported):

```sh
cd n64
make
```

Produces `msw-n64.z64`.

### Porting notes

The N64 port is a from-scratch reimplementation of the same demo, not a recompile - the two toolchains don't share an API:

- GameCube rendering uses GX immediate mode with a hardware matrix stack (`guPerspective`/`guLookAt`/`GX_Begin`). Modern libdragon has no equivalent (its old GL wrapper was removed), so the N64 version transforms and near-plane-clips every quad on the CPU (see `n64/source/vecmath.h`) and submits pre-projected triangles via `rdpq_triangle`. There's no RSP-accelerated transform stage, so it won't match GX's throughput.
- The GameCube pad's analog C-stick becomes digital C-buttons on an N64 controller; libdragon emulates an analog `cstick_x/y` from those buttons, so the look controls behave the same in code but feel 8-directional rather than analog in practice.
