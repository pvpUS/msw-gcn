#!/usr/bin/env python3
"""
gen_mapdb.py -- build proxy/mapdb.json: every MegaSkywars map's world origin,
its bounds, and its index in the console's g_maps[].

This is the file the whole delta approach rests on. MapManager.java hardcodes
each map's teleportLocation, all 31 maps live in the single world "world" at
fixed offsets >= 1800 blocks apart, and the .mworld scans are spawn-relative to
exactly those origins -- so the proxy identifies the map from the player's
absolute coordinates alone and converts absolute -> local with a subtraction.
No chunk fingerprinting, no plugin change, and no chunk decoder on the console.

Generated rather than transcribed for two reasons. The console index has to
match maps_gen.h exactly -- one wrong row loads the wrong map and every block
delta lands somewhere plausible and wrong -- and the map bounds come from the
.mworld headers, which are the same bytes the console will parse.

    python tools/gen_mapdb.py [MapManager.java] [out.json]

It also runs the checks on the scan-origin assumption that can be made without
a live server (see verify() -- the ones that need one are noted there).
"""

import json
import os
import re
import struct
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
MAPMANAGER = r"D:\Java Mods\MegaSkywars\src\me\pvpus\megaskywars\MapManager.java"
OUT = os.path.join(REPO, "proxy", "mapdb.json")

# The one map in g_maps[] with no server counterpart: a generated block-model
# showcase that only exists to eyeball shapes offline.
NOT_A_SERVER_MAP = {"model_gallery"}

# The hub. It is scanned and shipped exactly like a map -- same 400x400 dump,
# same spawn-relative coordinates, same .mworld -- but it is not in
# MapManager.addMaps(), because it is not a game. Its origin is hardcoded in
# two places in the plugin and they agree:
#
#   mapedge/BlockScanTask.buildTargets():  new Location(world, 0, 101, 0)
#   commands/SpawnCommand.onCommand():     p.teleport(... 0, 101, 0)
#
# The first is what the scan is relative to and the second is where a player
# actually stands, which is the same convention every map's teleportLocation
# follows. It matters that the hub is in here at all: without it the proxy
# reports "no map" whenever the account is not in a game, and the console spends
# every minute between games looking at a blank screen.
HUB_NAME = "spawn"
HUB = (0, (0, 101, 0))    # teams, origin


# ---------------------------------------------------------------------------
def parse_mapmanager(path):
    """name -> (teams, originX, originY, originZ), from MapManager.addMaps()."""
    put = re.compile(r'maps\.put\(\s*"(\w+)"')
    status = re.compile(r'new\s+MapStatus\s*\(\s*plugin\s*,\s*"(\w+)"\s*,\s*(\d+)')
    loc = re.compile(
        r'new\s+Location\(\s*Bukkit\.getWorld\("world"\)\s*,\s*'
        r'(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\)')

    out = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = put.search(line)
            if not m:
                continue
            name = m.group(1)
            ml = loc.search(line)
            if not ml:
                raise SystemExit(f"{name}: maps.put with no Location on the line")
            ms = status.search(line)
            teams = int(ms.group(2)) if ms else 0
            out[name] = (teams, int(ml.group(1)), int(ml.group(2)), int(ml.group(3)))
    if not out:
        raise SystemExit(f"no maps.put(...) found in {path}")
    return out


def parse_maps_gen(path):
    """[(slug, title)] in g_maps[] order -- the index MAP_SELECT carries."""
    row = re.compile(r'\{\s*"([^"]+)"\s*,\s*(\w+)_mworld\s*,')
    out = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = row.search(line)
            if m:
                out.append((m.group(2), m.group(1)))
    if not out:
        raise SystemExit(f"no g_maps[] rows found in {path}")
    return out


def parse_margins(path):
    """WORLD_MARGIN_XZ / WORLD_MARGIN_Y from world.h.

    The console pads every map with empty grid so the player can build out past
    the scan bounds, and blocks placed in that band are legal. The proxy has to
    accept deltas there or bridging off the edge of a map would silently stop
    updating."""
    txt = open(path, "r", encoding="utf-8").read()
    def find(name):
        m = re.search(r"#define\s+%s\s+(\d+)" % name, txt)
        if not m:
            raise SystemExit(f"{name} not found in {path}")
        return int(m.group(1))
    return find("WORLD_MARGIN_XZ"), find("WORLD_MARGIN_Y")


def read_mworld_header(path):
    """The .mworld header, per tools/compress_worlds.py. Coordinates here are
    spawn-relative: the scans were taken with the map's teleportLocation at the
    origin, which is precisely the assumption this file exists to record."""
    with open(path, "rb") as f:
        head = f.read(38)
    if head[:4] != b"MWL1":
        raise SystemExit(f"{path}: bad magic")
    minx, miny, minz = struct.unpack(">hhh", head[8:14])
    dimx, dimy, dimz = struct.unpack(">HHH", head[14:20])
    spawn = struct.unpack(">hhh", head[20:26])
    blocks = struct.unpack(">I", head[26:30])[0]
    return dict(min=[minx, miny, minz], dim=[dimx, dimy, dimz],
                spawn=list(spawn), blocks=blocks)


# ---------------------------------------------------------------------------
def verify(maps, margin_xz, margin_y):
    """The offline half of BBA-plan risk 2.

    What can be checked here: that every scan, placed at its recorded origin,
    lands inside the 0..255 build height, and that no two maps overlap. Both
    would fail loudly under a constant per-map offset in Y or XZ, which is the
    failure mode the plan warns about. What cannot be checked here, and still
    has to be done against the live server once, is the sign and axis of the
    XZ offset -- a map that is symmetric about its origin would pass everything
    below while being mirrored. Confirm one distinctive block per map."""
    problems = []

    for m in maps:
        ox, oy, oz = m["origin"]
        lo_y = m["min"][1] + oy
        hi_y = lo_y + m["dim"][1] - 1
        if lo_y < 0 or hi_y > 255:
            problems.append(
                f"{m['name']}: absolute Y {lo_y}..{hi_y} escapes 0..255 "
                f"(origin Y {oy}, scan Y {m['min'][1]}..{m['min'][1] + m['dim'][1] - 1})")
        if m["spawn"] != [0, 0, 0]:
            problems.append(f"{m['name']}: scan spawn {m['spawn']} is not the origin")

    # Overlap, in absolute coordinates and including the build margin, since two
    # maps whose margins meet would let a block placed in one land in the other.
    def box(m):
        ox, oy, oz = m["origin"]
        return (m["min"][0] + ox - margin_xz, m["min"][1] + oy - margin_y,
                m["min"][2] + oz - margin_xz,
                m["min"][0] + ox + m["dim"][0] - 1 + margin_xz,
                m["min"][1] + oy + m["dim"][1] - 1 + margin_y,
                m["min"][2] + oz + m["dim"][2] - 1 + margin_xz)

    for i in range(len(maps)):
        for j in range(i + 1, len(maps)):
            a, b = box(maps[i]), box(maps[j])
            if (a[0] <= b[3] and b[0] <= a[3] and a[1] <= b[4] and b[1] <= a[4]
                    and a[2] <= b[5] and b[2] <= a[5]):
                problems.append(f"{maps[i]['name']} overlaps {maps[j]['name']}")

    # The nearest-origin match needs origins comfortably further apart than any
    # map's own reach, or a player at the far edge of a big map is closer to the
    # neighbouring origin than to their own.
    worst = None
    for i in range(len(maps)):
        for j in range(i + 1, len(maps)):
            oa, ob = maps[i]["origin"], maps[j]["origin"]
            d = max(abs(oa[0] - ob[0]), abs(oa[2] - ob[2]))
            if worst is None or d < worst[0]:
                worst = (d, maps[i]["name"], maps[j]["name"])

    reach = max(max(abs(m["min"][0]), abs(m["min"][0] + m["dim"][0]),
                    abs(m["min"][2]), abs(m["min"][2] + m["dim"][2])) for m in maps)
    return problems, worst, reach


# ---------------------------------------------------------------------------
def main():
    mm_path = sys.argv[1] if len(sys.argv) > 1 else MAPMANAGER
    out_path = sys.argv[2] if len(sys.argv) > 2 else OUT

    if not os.path.exists(mm_path):
        raise SystemExit(
            f"MapManager.java not found at {mm_path}\n"
            f"  It lives outside the repo; see BBA-plan.md's References section.")

    plugin = parse_mapmanager(mm_path)
    teams, origin = HUB
    plugin[HUB_NAME] = (teams,) + origin
    console = parse_maps_gen(os.path.join(REPO, "source", "maps_gen.h"))
    margin_xz, margin_y = parse_margins(os.path.join(REPO, "source", "world.h"))
    print(f"plugin: {len(plugin)} maps   console: {len(console)} g_maps[] rows   "
          f"margin {margin_xz} xz / {margin_y} y")

    maps = []
    for index, (slug, title) in enumerate(console):
        if slug in NOT_A_SERVER_MAP:
            continue
        if slug not in plugin:
            raise SystemExit(f"{slug} is in g_maps[] but not in MapManager.addMaps()")
        teams, ox, oy, oz = plugin[slug]
        hdr = read_mworld_header(os.path.join(REPO, "data", slug + ".mworld"))
        row = dict(name=slug, title=title, index=index, teams=teams,
                   origin=[ox, oy, oz], **hdr)
        # The proxy needs to tell the hub from a map: the same MAP_SELECT is
        # sent either way, but the game state that goes with it is LOBBY rather
        # than WAITING, and nothing about a game applies while standing in it.
        if slug == HUB_NAME:
            row["hub"] = True
        maps.append(row)

    missing = set(plugin) - {m["name"] for m in maps}
    if missing:
        raise SystemExit(f"maps in the plugin with no g_maps[] entry: {sorted(missing)}")

    problems, worst, reach = verify(maps, margin_xz, margin_y)
    print(f"  closest two origins: {worst[0]} blocks ({worst[1]} / {worst[2]}); "
          f"the furthest any scan reaches from its own origin is {reach}")
    if problems:
        print("\n  ORIGIN CHECKS FAILED:")
        for p in problems:
            print(f"    {p}")
        raise SystemExit(1)
    print(f"  every scan sits inside 0..255 at its origin, and no two maps overlap")

    doc = {
        "_comment": ("Generated by tools/gen_mapdb.py -- do not edit. "
                     "Origins from MegaSkywars MapManager.addMaps(); bounds from "
                     "the .mworld headers; index is the row in the console's "
                     "g_maps[] that MAP_SELECT carries."),
        "_coords": ("origin is the map's teleportLocation in absolute world "
                    "coordinates. min/dim are the scan bounds in local (spawn-"
                    "relative) block space, so local = absolute - origin and the "
                    "scan occupies min .. min+dim-1 on each axis."),
        "margin": {"xz": margin_xz, "y": margin_y},
        "maps": maps,
    }
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    print(f"\n  wrote {out_path} ({len(maps)} maps)")


if __name__ == "__main__":
    main()
