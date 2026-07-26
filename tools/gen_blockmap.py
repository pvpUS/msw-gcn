#!/usr/bin/env python3
"""
gen_blockmap.py -- the single versioned bridge between Minecraft 1.8's wire
format and this engine's ids, emitted for both sides of the GCLink link.

The engine has exactly one namespace for "a thing that can sit in a world cell
or an inventory slot": a **global id** == the 0-based line number in
`data/blockids.txt`, whose lines are Bukkit `MATERIAL[:data]` strings. The
proxy, however, receives 1.8 wire values: a chunk section carries
`stateId = blockId << 4 | meta`, and an inventory slot carries
`(itemId, damage)`. This tool derives the translation between the two from a
single source of truth and writes it out three ways:

  data/blockids.txt      (input)  the palette; line N == global id N
  proxy/blockmap.json    (output) stateId -> globalId, itemId -> engine item
  source/blockmap_gen.h  (output) BLOCKMAP_HASH + the sentinel id, for HELLO

`Material.getId()` is the authority for `MATERIAL -> numeric id`; it *is* the
vanilla numeric id, so it agrees with MCP-919's `Block.registerBlocks()` (see
the header of `data/materials.txt` for the spot-checks). Bukkit is used rather
than transcribing `registerBlocks()` because the palette keys are Bukkit names
and the mapping is then direct rather than via a second rename table.

**The palette is append-only.** A global id is baked into all 33 `.mworld`
blobs and into every generated atlas/shape/props table, so an insert anywhere
before the end silently reinterprets every map. New blocks go on the end, in
front of the `UNKNOWN_BLOCK` sentinel (which this tool asserts stays last).

`BLOCKMAP_HASH` is an FNV-1a over the palette text. Both ends send it in
`HELLO`; a mismatch means the console's baked-in tables and the proxy's JSON
disagree, which must fail loudly at connect rather than render garbage.

Usage: gen_blockmap.py [blockids.txt] [materials.txt]
"""
import json
import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

# Last palette line. Any 1.8 block state with no palette entry maps here.
# Rendered as a magenta/black placeholder (see build_atlas.py) so a palette gap
# is *visible* when it is deliberately loaded for diagnostics -- but the proxy
# drops unmapped states from the live world instead of sending this (T7): a
# placeholder appearing 20x/sec during a game is worse than nothing.
SENTINEL_NAME = "UNKNOWN_BLOCK"

# ---------------------------------------------------------------------------
# Items that are not blocks (itemId > 255, so no block state can collide).
#
# Maps a 1.8 item id to the engine item this client shows for it. The engine
# value is an atlas tile index >= NUM_BLOCK_IDS, named here by its
# `ITEM_<name>_TILE` define in source/block_book_gen.h and resolved at generate
# time; `None` means "the art does not exist yet" -- T13 adds the tile to
# build_atlas.py's ITEM_TEXTURES and re-running this picks it up with no edit
# here. Items *below* 256 need no entry: an item id <= 255 is a placeable
# block, so `(itemId, damage)` goes through the block state table instead.
#
# `damage` is ignored except for the entries listed in ITEM_DAMAGE below.
ITEM_MAP = {
    # -- the kit (KitManager.java:37-122, MapStatus.startGame:935-963) --------
    276: "DIAMOND_SWORD",
    278: "DIAMOND_PICKAXE",
    279: "DIAMOND_AXE",
    261: "BOW",
    262: "ARROW",
    346: "FISHING_ROD",
    322: "GOLDEN_APPLE",
    326: "WATER_BUCKET",
    327: "LAVA_BUCKET",
    # Bukkit calls it SNOW_BALL; the atlas tile has been ITEM_SNOWBALL_TILE
    # since long before this table, and it is also a snow-block drop, so the
    # tile keeps its name rather than churning block_props_gen.h.
    332: "SNOWBALL",
    368: "ENDER_PEARL",
    310: "DIAMOND_HELMET",
    311: "DIAMOND_CHESTPLATE",
    312: "DIAMOND_LEGGINGS",
    313: "DIAMOND_BOOTS",
    # -- items this engine already had (block drops, mined with) -------------
    277: "DIAMOND_SHOVEL",
    263: "COAL",
    264: "DIAMOND",
    331: "REDSTONE",
    406: "QUARTZ",
    318: "FLINT",
    337: "CLAY_BALL",
    287: "STRING",
    296: "WHEAT",
    295: "WHEAT_SEEDS",
    340: "BOOK",
    323: "SIGN",
    324: "DOOR_WOOD",
    355: "BED",
    351: "COCOA_BEANS",   # INK_SACK:3; see ITEM_DAMAGE
    372: "NETHER_WART",
}

# Item ids whose `damage` selects a different engine item. The key is the item
# id; the value maps a *masked* damage to an engine item name, with the mask
# applied first. Anything not listed falls back to ITEM_MAP's entry.
#
#   373 POTION -- 1.8 packs the potion into the damage value (org.bukkit.potion
#       .Potion.toDamageValue): bits 0-3 type, bit 5 (0x20) tier II, bit 6
#       (0x40) extended, bit 14 (0x4000) splash. Only the type and the splash
#       bit change the icon, so mask to 0x400F and ignore tier/duration. The
#       kit's three potions are SPEED extended splash (16450), REGEN extended
#       splash (16449) and FIRE_RESISTANCE extended (67).
#   351 INK_SACK -- the dye family; only cocoa beans (damage 3) are reachable
#       here, as a cocoa-pod drop.
ITEM_DAMAGE = {
    373: (0x400F, {
        0x4002: "SPLASH_SPEED",
        0x4001: "SPLASH_REGEN",
        0x0003: "FIRE_RESISTANCE_POTION",
    }),
    351: (0x000F, {
        3: "COCOA_BEANS",
    }),
}

# Blocks the plugin names that this client deliberately does not carry. Each is
# recorded here (rather than silently missing) so T1's coverage check has a
# complete answer: every Material the plugin references is either in the
# palette or in this table with a reason.
UNSUPPORTED = {
    "AIR":               "id 0 is absent everywhere; the engine stores air as -1",
    "BARRIER":           "avoidance minigame only (MirrorAvoidance/SafeZoneAvoidance); "
                         "invisible in vanilla anyway",
    "EMERALD_BLOCK":     "avoidance minigame only (DualityAvoidance spawn platform)",
    "CACTUS":            "map-select GUI icon only (MapManager.java oasis/oasisv2)",
    "DRAGON_EGG":        "kill-effect cosmetic (Cosmetics.java:594), never a world block",
    "PORTAL":            "particle effect argument only (Cosmetics.java:422)",
    "REDSTONE_TORCH_ON": "cosmetic-menu icon only (Cosmetics.java:57, IAvoidance)",
    "THIN_GLASS":        "cosmetic-menu icon only (Cosmetics.java:37 Shatter kill effect)",
    "ENDER_PORTAL":      "Lights Out mode, cut by decision -- the plugin rewrites it "
                         "above the player every tick and T7 drops unmapped states",
}


def split(bid):
    if ":" in bid:
        base, data = bid.split(":", 1)
        return base, int(data)
    return bid, 0


def fnv1a(data):
    """FNV-1a/32 over bytes; the HELLO handshake's palette fingerprint."""
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def load_materials(path):
    """name -> (numericId, 'block'|'item') from the vendored Bukkit dump."""
    out = {}
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        name, num, kind = line.split("\t")[:3]
        out[name] = (int(num), kind)
    return out


def load_item_tiles(src_dir):
    """ITEM_<name>_TILE -> atlas tile index, from block_book_gen.h."""
    path = os.path.join(src_dir, "block_book_gen.h")
    text = open(path, encoding="utf-8").read()
    return {m.group(1): int(m.group(2))
            for m in re.finditer(r"#define ITEM_([A-Z0-9_]+)_TILE (\d+)", text)}


def plugin_materials(plugin_src):
    """Every Material.<NAME> the MegaSkywars source names, for the coverage
    report. Absent (the plugin lives outside this repo) is not an error."""
    if not plugin_src or not os.path.isdir(plugin_src):
        return None
    found = set()
    for root, _dirs, files in os.walk(plugin_src):
        for fn in files:
            if not fn.endswith(".java"):
                continue
            with open(os.path.join(root, fn), encoding="utf-8", errors="replace") as f:
                found.update(re.findall(r"Material\.([A-Z_0-9]+)", f.read()))
    return found


def main():
    ids_path = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(REPO, "data", "blockids.txt")
    mats_path = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.join(REPO, "data", "materials.txt")
    src_dir = os.path.join(REPO, "source")
    proxy_dir = os.path.join(REPO, "proxy")

    raw = open(ids_path, "rb").read()
    ids = [l.strip() for l in raw.decode("utf-8").splitlines() if l.strip()]
    mats = load_materials(mats_path)
    tiles = load_item_tiles(src_dir)

    assert ids[-1] == SENTINEL_NAME, (
        f"{SENTINEL_NAME} must be the last line of {ids_path} -- append new "
        f"blocks in front of it (last line is {ids[-1]!r})")
    sentinel = len(ids) - 1

    # ---- blocks: stateId (= blockId << 4 | meta) -> globalId ---------------
    states = {}
    unmapped_names = []
    for gid, bid in enumerate(ids):
        if gid == sentinel:
            continue
        base, data = split(bid)
        info = mats.get(base)
        if info is None or info[1] != "block":
            unmapped_names.append(bid)
            continue
        num, _kind = info
        if not (0 <= num <= 255):
            unmapped_names.append(bid)
            continue
        if not (0 <= data <= 15):
            unmapped_names.append(bid)
            continue
        state = num << 4 | data
        prev = states.get(state)
        assert prev is None, (
            f"state {state} ({bid}) already claimed by global id {prev} "
            f"({ids[prev]}) -- two palette lines name the same 1.8 block state")
        states[state] = gid

    assert not unmapped_names, (
        "palette entries with no 1.8 block state: " + ", ".join(unmapped_names))

    # ---- items: itemId (+ damage) -> engine item id ------------------------
    items = {}
    missing_art = []
    for item_id, name in sorted(ITEM_MAP.items()):
        tile = tiles.get(name)
        if tile is None:
            missing_art.append(name)
        items[str(item_id)] = {"name": name, "engine": tile}
    item_damage = {}
    for item_id, (mask, table) in sorted(ITEM_DAMAGE.items()):
        entries = {}
        for dmg, name in sorted(table.items()):
            tile = tiles.get(name)
            if tile is None:
                missing_art.append(name)
            entries[str(dmg)] = {"name": name, "engine": tile}
        item_damage[str(item_id)] = {"mask": mask, "values": entries}

    palette_hash = fnv1a(raw)

    # ---- proxy/blockmap.json ----------------------------------------------
    os.makedirs(proxy_dir, exist_ok=True)
    doc = {
        "_comment": "Generated by tools/gen_blockmap.py -- do not edit. "
                    "Regenerate after any change to data/blockids.txt.",
        "paletteHash": palette_hash,
        "blockCount": len(ids),
        "sentinel": sentinel,
        "notes": {
            "states": "key is the 1.8 wire state id (blockId << 4 | meta); "
                      "value is this engine's global block id",
            "air": "block id 0 has no palette entry; the engine stores air as -1",
            "unmappedStates": "drop them (T7). 'sentinel' exists for offline "
                              "palette diagnostics, not for the live world",
            "items": "item ids <= 255 are placeable blocks -- look those up in "
                     "'states' as (itemId << 4 | damage) instead. 'engine' is "
                     "an atlas tile index >= blockCount, or null when the art "
                     "does not exist yet (T13)",
            "itemDamage": "checked before 'items': mask the damage, then look "
                          "it up; fall back to 'items' on a miss",
        },
        "states": {str(k): v for k, v in sorted(states.items())},
        "items": items,
        "itemDamage": item_damage,
        "unsupported": UNSUPPORTED,
    }
    json_path = os.path.join(proxy_dir, "blockmap.json")
    with open(json_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1, sort_keys=False)
        f.write("\n")

    # ---- source/blockmap_gen.h --------------------------------------------
    h_path = os.path.join(src_dir, "blockmap_gen.h")
    with open(h_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* Generated by tools/gen_blockmap.py -- do not edit. */\n")
        f.write("#ifndef MSW_BLOCKMAP_GEN_H\n#define MSW_BLOCKMAP_GEN_H\n\n")
        f.write("/* FNV-1a/32 over data/blockids.txt. Sent in the GCLink HELLO\n")
        f.write(" * both ways: the console's baked-in atlas/shape/props tables and\n")
        f.write(" * the proxy's blockmap.json are only interchangeable if this\n")
        f.write(" * matches, so a stale blockmap must fail the handshake rather\n")
        f.write(" * than render a whole map as the wrong blocks. */\n")
        f.write(f"#define BLOCKMAP_HASH 0x{palette_hash:08X}u\n\n")
        f.write("/* Last palette entry: the placeholder a 1.8 block state with no\n")
        f.write(" * global id resolves to. World_SetBlock clamps out-of-range ids\n")
        f.write(" * here -- world.c indexes g_blockShape[]/g_topTile[] by global id\n")
        f.write(" * with no bounds check of its own, so an id past the palette would\n")
        f.write(" * otherwise read off the end of those tables. */\n")
        f.write(f"#define BLOCKMAP_SENTINEL_ID {sentinel}\n\n")
        f.write("#endif\n")

    # ---- report ------------------------------------------------------------
    print(f"wrote {json_path}")
    print(f"wrote {h_path}")
    print(f"  palette {len(ids)} ids (sentinel {sentinel} = {SENTINEL_NAME}), "
          f"hash 0x{palette_hash:08X}")
    print(f"  {len(states)} block states, {len(items)} items, "
          f"{len(item_damage)} damage-keyed item ids")
    if missing_art:
        print(f"  note: {len(missing_art)} items have no atlas tile yet (T13): "
              f"{', '.join(sorted(set(missing_art)))}")

    # Coverage against the plugin's own Material set (T1's done-when).
    plugin_src = sys.argv[3] if len(sys.argv) > 3 else r"D:\Java Mods\MegaSkywars\src"
    used = plugin_materials(plugin_src)
    if used is None:
        print(f"  (plugin source not at {plugin_src}; skipped the coverage check)")
        return
    fams = set(split(b)[0] for b in ids)
    gaps = []
    for name in sorted(used):
        info = mats.get(name)
        if info is None or info[1] != "block":
            continue                      # an item, checked by ITEM_MAP instead
        if name in fams or name in UNSUPPORTED:
            continue
        gaps.append(name)
    print(f"  plugin names {len(used)} materials; "
          f"{len([m for m in used if mats.get(m, (0,''))[1] == 'block'])} are blocks")
    if gaps:
        print(f"  WARNING: {len(gaps)} plugin blocks neither in the palette nor "
              f"listed UNSUPPORTED: {', '.join(gaps)}")
    else:
        print("  every plugin block is in the palette or explicitly unsupported")


if __name__ == "__main__":
    main()
