# msw-gcn

Mega Skywars - a voxel-island tech demo with a flying camera, built for the GameCube using [devkitPPC + libogc](https://devkitpro.org/) (GX for rendering).

Controls: main stick to fly forward/strafe, look stick/C-stick to look around, A/B to rise/fall, Start to quit.

## Building

Requires devkitPro with the GameCube toolchain (`DEVKITPPC` exported):

```sh
make
```

Produces `msw-gcn.dol` / `msw-gcn.elf`.
