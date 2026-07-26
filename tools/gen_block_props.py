#!/usr/bin/env python3
"""
gen_block_props.py -- emit source/block_props_gen.h: the per-global-block-id
gameplay properties the breaking/placing code needs, derived from the same
_blockids.txt global palette build_atlas.py uses (tile index == global id ==
line number - 1).

Everything here is transcribed from Minecraft 1.8.9 (MCP-919):

  * hardness   - Block.blockHardness, as passed to setHardness() in
                 Block.registerBlocks() (or the block class's constructor for
                 BlockLog/BlockLeaves/BlockStairs/BlockWall, which copy their
                 model block's). -1 == setBlockUnbreakable().
  * material   - only the distinctions the dig/drop rules actually make:
                 which tool speeds the block up (ItemPickaxe/ItemAxe/
                 ItemSpade.getStrVsBlock), whether a tool is *required* for a
                 drop (Material.setRequiresTool: rock/iron/anvil/web/snow),
                 and ItemSword's 1.5x plants/vine/leaves/gourd case.
  * harvest    - EntityPlayer.canHarvestBlock -> InventoryPlayer
                 .canHeldItemHarvest -> ItemStack.canHarvestBlock, i.e.
                 ItemPickaxe/ItemSpade/ItemSword.canHarvestBlock, reduced to
                 "which tool class, at what material tier".
  * drops      - Block.getItemDropped + quantityDropped + the handful of
                 dropBlockAsItemWithChance overrides (gravel -> flint 1/10,
                 tall grass -> seeds 1/8, ...). A drop id is either another
                 global block id or an appended item tile (ITEM_*_TILE from
                 block_book_gen.h, >= NUM_BLOCK_IDS), matching how
                 inventory.h's ItemStack.item is interpreted everywhere else.
  * placement  - family (base Material name) + data value, plus a per-family
                 "placement kind" and a data -> global id lookup, so
                 ItemBlock.onItemUse can re-derive a stair's facing/half, a
                 slab's half, a log's axis, ... from the click the way
                 Block.onBlockPlaced does. Families whose kind is PLAIN just
                 place the exact id that was picked up.

Blocks whose vanilla drop is an item this engine has no texture for (glowstone
dust, prismarine crystals, saplings, brewing stands, ...) fall back to dropping
the block itself, which is what its inventory icon would show anyway; the
exceptions that vanilla drops *nothing* for (glass, leaves, ice, vines) are
kept faithful and drop nothing.

Usage: gen_block_props.py [_blockids.txt] [source-dir]
"""
import os, sys

# ---- materials (only the dig/drop-relevant distinctions) -------------------
MATERIALS = ["AIR", "ROCK", "IRON", "ANVIL", "WOOD", "PLANTS", "VINE", "LEAVES",
             "GOURD", "GROUND", "GRASS", "SAND", "CLAY", "SNOW", "CRAFTED_SNOW",
             "CLOTH", "CARPET", "GLASS", "ICE", "WEB", "CIRCUITS", "WATER",
             "FIRE", "PISTON", "CAKE"]

# Material.setReplaceable(): a block placement may overwrite these outright
# instead of offsetting to the clicked side (World.canBlockBePlaced).
REPLACEABLE = {"AIR", "WATER", "FIRE", "VINE", "SNOW"}

# Which tool class gets its efficiency bonus on this material
# (ItemPickaxe/ItemAxe.getStrVsBlock's material test; ItemSpade goes through
# ItemTool's effectiveBlocks set, approximated here by material).
TOOL_NONE, TOOL_PICKAXE, TOOL_AXE, TOOL_SHOVEL, TOOL_SWORD = range(5)
SPEED_TOOL = {
    "ROCK": TOOL_PICKAXE, "IRON": TOOL_PICKAXE, "ANVIL": TOOL_PICKAXE,
    "GLASS": TOOL_PICKAXE, "ICE": TOOL_PICKAXE, "PISTON": TOOL_PICKAXE,
    "WOOD": TOOL_AXE, "PLANTS": TOOL_AXE, "VINE": TOOL_AXE, "GOURD": TOOL_AXE,
    "GROUND": TOOL_SHOVEL, "GRASS": TOOL_SHOVEL, "SAND": TOOL_SHOVEL,
    "CLAY": TOOL_SHOVEL, "SNOW": TOOL_SHOVEL, "CRAFTED_SNOW": TOOL_SHOVEL,
}
# Materials with setRequiresTool(): no drop unless the held item can harvest.
REQUIRES_TOOL = {"ROCK", "IRON", "ANVIL", "WEB", "SNOW", "CRAFTED_SNOW"}

# ---- placement kinds (Block.onBlockPlaced, see interact.c) -----------------
PLACE_KINDS = {
    "PLAIN": 0,     # place exactly the id that was picked up
    "STAIR": 1,     # BlockStairs: facing from the placer + half from hitY/side
    "SLAB": 2,      # BlockSlab: bit3 = top half
    "PILLAR": 3,    # BlockRotatedPillar (logs): axis bits from the clicked side
    "TORCH": 4,     # BlockTorch: 1=E 2=W 3=S 4=N 5=floor
    "HFACING": 5,   # BlockChest/BlockFurnace: meta = EnumFacing.getIndex() 2..5
    "LADDER": 6,    # BlockLadder: same encoding, but facing == the clicked side
    "GATE": 7,      # BlockFenceGate: meta = EnumFacing.getHorizontalIndex() 0..3
    "ANVIL": 8,     # BlockAnvil: horizontal index of the placer's facing, rotated
}

# ---- per-family properties -------------------------------------------------
# name: (hardness, material, harvestTool, harvestLevel, placementKind)
# harvestTool/harvestLevel are the ItemStack.canHarvestBlock test; they only
# matter when the material is in REQUIRES_TOOL.
P = {}
def fam(names, hardness, material, tool=TOOL_NONE, level=0, place="PLAIN"):
    for n in names.split():
        P[n] = (hardness, material, tool, level, place)

# stone family
fam("STONE",                      1.5,  "ROCK", TOOL_PICKAXE)
fam("COBBLESTONE MOSSY_COBBLESTONE JUKEBOX", 2.0, "ROCK", TOOL_PICKAXE)
fam("BRICK NETHER_BRICK",         2.0,  "ROCK", TOOL_PICKAXE)
fam("SMOOTH_BRICK",               1.5,  "ROCK", TOOL_PICKAXE)
fam("PRISMARINE",                 1.5,  "ROCK", TOOL_PICKAXE)
fam("SANDSTONE RED_SANDSTONE QUARTZ_BLOCK", 0.8, "ROCK", TOOL_PICKAXE)
fam("NETHERRACK",                 0.4,  "ROCK", TOOL_PICKAXE)
fam("OBSIDIAN",                  50.0,  "ROCK", TOOL_PICKAXE, 3)
fam("MONSTER_EGGS",               0.75, "ROCK", TOOL_PICKAXE)
fam("HARD_CLAY STAINED_CLAY",     1.25, "ROCK", TOOL_PICKAXE)
fam("COAL_BLOCK",                 5.0,  "ROCK", TOOL_PICKAXE)
fam("BEDROCK",                   -1.0,  "ROCK", TOOL_PICKAXE)
fam("ENCHANTMENT_TABLE",          5.0,  "ROCK", TOOL_PICKAXE)
fam("FURNACE DROPPER",            3.5,  "ROCK", TOOL_PICKAXE, 0, "HFACING")
fam("COAL_ORE IRON_ORE QUARTZ_ORE", 3.0, "ROCK", TOOL_PICKAXE, 1)
fam("GOLD_ORE DIAMOND_ORE",       3.0,  "ROCK", TOOL_PICKAXE, 2)
fam("NETHER_FENCE COBBLE_WALL",   2.0,  "ROCK", TOOL_PICKAXE)
fam("STONE_PLATE",                0.5,  "ROCK", TOOL_PICKAXE)
fam("PISTON_BASE",                0.5,  "PISTON")

# metal-material blocks (Material.iron): pickaxe, tiered
fam("IRON_BLOCK",                 5.0,  "IRON", TOOL_PICKAXE, 1)
fam("LAPIS_BLOCK",                3.0,  "IRON", TOOL_PICKAXE, 1)
fam("GOLD_BLOCK",                 3.0,  "IRON", TOOL_PICKAXE, 2)
fam("DIAMOND_BLOCK REDSTONE_BLOCK", 5.0, "IRON", TOOL_PICKAXE, 2)
fam("IRON_FENCE",                 5.0,  "IRON", TOOL_PICKAXE)
fam("IRON_TRAPDOOR",              5.0,  "IRON", TOOL_PICKAXE)
fam("HOPPER",                     3.0,  "IRON", TOOL_PICKAXE)
fam("CAULDRON",                   2.0,  "IRON", TOOL_PICKAXE)
fam("BREWING_STAND",              0.5,  "IRON", TOOL_PICKAXE)
fam("ANVIL",                      5.0,  "ANVIL", TOOL_PICKAXE, 0, "ANVIL")

# stairs / slabs inherit their model block's hardness (BlockStairs ctor)
fam("COBBLESTONE_STAIRS BRICK_STAIRS NETHER_BRICK_STAIRS", 2.0, "ROCK", TOOL_PICKAXE, 0, "STAIR")
fam("SMOOTH_STAIRS",              1.5,  "ROCK", TOOL_PICKAXE, 0, "STAIR")
fam("SANDSTONE_STAIRS RED_SANDSTONE_STAIRS QUARTZ_STAIRS", 0.8, "ROCK", TOOL_PICKAXE, 0, "STAIR")
fam("ACACIA_STAIRS BIRCH_WOOD_STAIRS DARK_OAK_STAIRS JUNGLE_WOOD_STAIRS "
    "SPRUCE_WOOD_STAIRS WOOD_STAIRS", 2.0, "WOOD", TOOL_AXE, 0, "STAIR")
fam("STEP STONE_SLAB2",           2.0,  "ROCK", TOOL_PICKAXE, 0, "SLAB")
fam("DOUBLE_STEP",                2.0,  "ROCK", TOOL_PICKAXE)
fam("WOOD_STEP",                  2.0,  "WOOD", TOOL_AXE, 0, "SLAB")
fam("WOOD_DOUBLE_STEP",           2.0,  "WOOD", TOOL_AXE)

# wood family
fam("WOOD BOOKSHELF",             2.0,  "WOOD", TOOL_AXE)
fam("LOG LOG_2",                  2.0,  "WOOD", TOOL_AXE, 0, "PILLAR")
fam("ACACIA_FENCE BIRCH_FENCE DARK_OAK_FENCE FENCE JUNGLE_FENCE SPRUCE_FENCE",
    2.0, "WOOD", TOOL_AXE)
fam("BIRCH_FENCE_GATE DARK_OAK_FENCE_GATE FENCE_GATE SPRUCE_FENCE_GATE",
    2.0, "WOOD", TOOL_AXE, 0, "GATE")
fam("WORKBENCH CHEST",            2.5,  "WOOD", TOOL_AXE, 0, "HFACING")
fam("NOTE_BLOCK",                 0.8,  "WOOD", TOOL_AXE)
fam("SIGN_POST WALL_SIGN",        1.0,  "WOOD", TOOL_AXE)
fam("WOODEN_DOOR JUNGLE_DOOR TRAP_DOOR", 3.0, "WOOD", TOOL_AXE)
fam("WOOD_PLATE WOOD_BUTTON",     0.5,  "WOOD", TOOL_AXE)
fam("HUGE_MUSHROOM_1 HUGE_MUSHROOM_2", 0.2, "WOOD", TOOL_AXE)

# ground / plants / misc
fam("GRASS MYCEL",                0.6,  "GRASS", TOOL_SHOVEL)
fam("DIRT SOIL",                  0.5,  "GROUND", TOOL_SHOVEL)
fam("GRAVEL",                     0.6,  "GROUND", TOOL_SHOVEL)
fam("SAND SOUL_SAND",             0.5,  "SAND", TOOL_SHOVEL)
fam("CLAY",                       0.6,  "CLAY", TOOL_SHOVEL)
fam("SPONGE HAY_BLOCK",           0.5,  "GRASS", TOOL_SHOVEL)
fam("SNOW",                       0.1,  "SNOW", TOOL_SHOVEL)          # snow_layer
fam("SNOW_BLOCK",                 0.2,  "CRAFTED_SNOW", TOOL_SHOVEL)
fam("SLIME_BLOCK",                0.0,  "CLAY")
fam("WOOL",                       0.8,  "CLOTH")
fam("CARPET",                     0.1,  "CARPET")
fam("GLASS STAINED_GLASS GLASS_PANE STAINED_GLASS_PANE", 0.3, "GLASS")
fam("GLOWSTONE SEA_LANTERN REDSTONE_LAMP_ON", 0.3, "GLASS")
fam("ICE PACKED_ICE",             0.5,  "ICE")
fam("WEB",                        4.0,  "WEB", TOOL_SWORD)
fam("LEAVES LEAVES_2",            0.2,  "LEAVES")
fam("VINE",                       0.2,  "VINE")
fam("YELLOW_FLOWER RED_ROSE DOUBLE_PLANT "
    "BROWN_MUSHROOM RED_MUSHROOM WATER_LILY CROPS NETHER_WARTS", 0.0, "PLANTS")
# tall grass and dead bushes are Material.vine, i.e. replaceable: a block
# placed into them overwrites them rather than landing on top.
fam("LONG_GRASS DEAD_BUSH",       0.0,  "VINE")
fam("COCOA",                      0.2,  "PLANTS")
fam("TORCH",                      0.0,  "CIRCUITS", TOOL_NONE, 0, "TORCH")
fam("LADDER",                     0.4,  "CIRCUITS", TOOL_NONE, 0, "LADDER")
fam("REDSTONE_WIRE LEVER STONE_BUTTON TRIPWIRE_HOOK", 0.0, "CIRCUITS")
fam("RAILS POWERED_RAIL",         0.7,  "CIRCUITS")
fam("SKULL",                      1.0,  "CIRCUITS")
fam("BED_BLOCK",                  0.2,  "WOOD", TOOL_AXE)
fam("FIRE",                       0.0,  "FIRE")
fam("WATER STATIONARY_WATER",   100.0,  "WATER")

# ---- drops -----------------------------------------------------------------
# family -> (dropSpec, minCount, maxCount, oneInN, altSpec)
# dropSpec is "SELF", None (drops nothing), a "MATERIAL[:data]" global block id,
# or "ITEM_<NAME>" naming an appended item tile. oneInN > 1 makes the primary
# drop a 1-in-N roll that falls back to altSpec.
DROPS = {
    "STONE":        ("COBBLESTONE", 1, 1, 1, None),   # only STONE:0 (see below)
    "GRASS":        ("DIRT", 1, 1, 1, None),
    "MYCEL":        ("DIRT", 1, 1, 1, None),
    "SOIL":         ("DIRT", 1, 1, 1, None),
    "GRAVEL":       ("ITEM_FLINT", 1, 1, 10, "SELF"),  # 10% flint, else gravel
    "CLAY":         ("ITEM_CLAY_BALL", 4, 4, 1, None),
    "COAL_ORE":     ("ITEM_COAL", 1, 1, 1, None),
    "DIAMOND_ORE":  ("ITEM_DIAMOND", 1, 1, 1, None),
    "QUARTZ_ORE":   ("ITEM_QUARTZ", 1, 1, 1, None),
    "REDSTONE_WIRE": ("ITEM_REDSTONE", 1, 1, 1, None),
    "WEB":          ("ITEM_STRING", 1, 1, 1, None),
    "SNOW_BLOCK":   ("ITEM_SNOWBALL", 4, 4, 1, None),
    "SNOW":         ("ITEM_SNOWBALL", 1, 1, 1, None),  # +1 per layer, in C
    "BOOKSHELF":    ("ITEM_BOOK", 3, 3, 1, None),
    "SIGN_POST":    ("ITEM_SIGN", 1, 1, 1, None),
    "WALL_SIGN":    ("ITEM_SIGN", 1, 1, 1, None),
    "WOODEN_DOOR":  ("ITEM_DOOR_WOOD", 1, 1, 1, None),
    "JUNGLE_DOOR":  ("ITEM_DOOR_WOOD", 1, 1, 1, None),
    "BED_BLOCK":    ("ITEM_BED", 1, 1, 1, None),
    "COCOA":        ("ITEM_COCOA_BEANS", 1, 1, 1, None),
    "NETHER_WARTS": ("ITEM_NETHER_WART", 1, 1, 1, None),
    "CROPS":        ("ITEM_WHEAT_SEEDS", 1, 1, 1, None),  # wheat at age 7, in C
    "LONG_GRASS":   ("ITEM_WHEAT_SEEDS", 1, 1, 8, None),  # 1/8 seeds, else nothing
    "HUGE_MUSHROOM_1": ("BROWN_MUSHROOM", 0, 2, 1, None),
    "HUGE_MUSHROOM_2": ("RED_MUSHROOM", 0, 2, 1, None),
    # vanilla drops nothing at all without silk touch / shears
    "GLASS": (None, 0, 0, 1, None),
    "STAINED_GLASS": (None, 0, 0, 1, None),
    "GLASS_PANE": (None, 0, 0, 1, None),
    "STAINED_GLASS_PANE": (None, 0, 0, 1, None),
    "ICE": (None, 0, 0, 1, None),
    "PACKED_ICE": (None, 0, 0, 1, None),
    "LEAVES": (None, 0, 0, 1, None),
    "LEAVES_2": (None, 0, 0, 1, None),
    "VINE": (None, 0, 0, 1, None),
    "DEAD_BUSH": (None, 0, 0, 1, None),
    "MONSTER_EGGS": (None, 0, 0, 1, None),
    "FIRE": (None, 0, 0, 1, None),
    "WATER": (None, 0, 0, 1, None),
    "STATIONARY_WATER": (None, 0, 0, 1, None),
    "BEDROCK": (None, 0, 0, 1, None),
}
# double slabs drop two of the matching single slab
DOUBLE_SLAB = {"DOUBLE_STEP": "STEP", "WOOD_DOUBLE_STEP": "WOOD_STEP"}


def split(bid):
    if ":" in bid:
        base, data = bid.split(":", 1)
        return base, int(data)
    return bid, 0


def main():
    ids_path = sys.argv[1] if len(sys.argv) > 1 else \
        r"C:\Users\awt12\Downloads\download (1)\BlockScans\_blockids.txt"
    src_dir = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "source"))

    ids = [l.strip() for l in open(ids_path, encoding="utf-8") if l.strip()]
    index = {bid: i for i, bid in enumerate(ids)}
    n = len(ids)

    # families, in first-appearance order, plus the data -> global id lookup
    families = []
    fam_index = {}
    for bid in ids:
        base, _ = split(bid)
        if base not in fam_index:
            fam_index[base] = len(families)
            families.append(base)
    variant = [[-1] * 16 for _ in families]
    for i, bid in enumerate(ids):
        base, data = split(bid)
        if 0 <= data < 16:
            variant[fam_index[base]][data] = i

    def resolve(spec, self_id, base):
        """dropSpec -> a global block id, an ITEM_* tile name, or -1."""
        if spec is None:
            return "-1"
        if spec == "SELF":
            return str(self_id)
        if spec.startswith("ITEM_"):
            return spec + "_TILE"
        if spec in index:
            return str(index[spec])
        # e.g. BROWN_MUSHROOM with no explicit data suffix
        if spec + ":0" in index:
            return str(index[spec + ":0"])
        print(f"  note: {base} drop {spec!r} is not in the palette -> nothing")
        return "-1"

    rows = []
    unknown = set()
    for i, bid in enumerate(ids):
        base, data = split(bid)
        if base in P:
            hardness, material, tool, level, place = P[base]
        else:
            unknown.add(base)
            hardness, material, tool, level, place = (1.0, "ROCK", TOOL_PICKAXE, 0, "PLAIN")

        drop, dmin, dmax, denom, alt = DROPS.get(base, ("SELF", 1, 1, 1, None))
        if base in DOUBLE_SLAB:                 # BlockDoubleStoneSlab: 2 halves
            half = DOUBLE_SLAB[base] + (f":{data}" if data else "")
            drop, dmin, dmax = (half if half in index else "SELF"), 2, 2
        if base == "STONE" and data != 0:       # granite/diorite/andesite drop themselves
            drop = "SELF"
        if base == "SNOW":                      # BlockSnow: one snowball per layer
            dmin = dmax = (data & 7) + 1
        if base == "COCOA":                     # BlockCocoa: 3 beans when ripe
            dmin = dmax = 3 if ((data >> 2) & 3) == 2 else 1

        rows.append((
            hardness,
            resolve(drop, i, base),
            resolve(alt, i, base),
            "MAT_" + material,
            SPEED_TOOL.get(material, TOOL_NONE),
            tool if material in REQUIRES_TOOL else TOOL_NONE,
            level,
            dmin, dmax, denom,
            1 if material in REPLACEABLE else 0,
            fam_index[base], data,
        ))

    if unknown:
        print(f"  WARNING: no property entry for {len(unknown)} families "
              f"(defaulted to stone-like): {', '.join(sorted(unknown))}")

    hpath = os.path.join(src_dir, "block_props_gen.h")
    with open(hpath, "w", encoding="utf-8") as f:
        f.write("/* Generated by tools/gen_block_props.py -- do not edit. */\n")
        f.write("#ifndef MSW_BLOCK_PROPS_GEN_H\n#define MSW_BLOCK_PROPS_GEN_H\n\n")
        f.write('#include "block_faces_gen.h"   /* NUM_BLOCK_IDS */\n')
        f.write('#include "block_book_gen.h"    /* ITEM_*_TILE drop ids */\n\n')

        f.write("/* Material, reduced to the distinctions Minecraft 1.8.9's dig-speed\n")
        f.write(" * and drop rules actually make (see ItemPickaxe/ItemAxe/ItemSpade/\n")
        f.write(" * ItemSword.getStrVsBlock and Material.setRequiresTool). */\n")
        f.write("enum {\n")
        for k, m in enumerate(MATERIALS):
            f.write(f"\tMAT_{m} = {k},\n")
        f.write("};\n\n")

        f.write("/* Tool classes (Item subclasses that override getStrVsBlock). */\n")
        f.write("enum {\n\tTOOL_NONE = 0,\n\tTOOL_PICKAXE = 1,\n\tTOOL_AXE = 2,\n"
                "\tTOOL_SHOVEL = 3,\n\tTOOL_SWORD = 4,\n};\n\n")

        f.write("/* How ItemBlock.onItemUse derives the placed block's data value\n")
        f.write(" * from the click (Block.onBlockPlaced); see interact.c. */\n")
        f.write("enum {\n")
        for k, v in sorted(PLACE_KINDS.items(), key=lambda kv: kv[1]):
            f.write(f"\tPLACE_{k} = {v},\n")
        f.write("};\n\n")

        f.write("typedef struct {\n")
        f.write("\tfloat hardness;   /* Block.blockHardness; < 0 = unbreakable   */\n")
        f.write("\ts16   drop;       /* dropped id (block id or item tile), -1 = none */\n")
        f.write("\ts16   dropAlt;    /* drop when the 1-in-dropDenom roll fails   */\n")
        f.write("\tu8    material;   /* MAT_*                                     */\n")
        f.write("\tu8    speedTool;  /* TOOL_* that gets its efficiency bonus here */\n")
        f.write("\tu8    harvestTool;/* TOOL_* required to drop (TOOL_NONE = hands)*/\n")
        f.write("\tu8    harvestLevel;/* tool material tier required (0 wood..3 diamond) */\n")
        f.write("\tu8    dropMin, dropMax;  /* quantityDropped() range, inclusive  */\n")
        f.write("\tu8    dropDenom;  /* 1 = always; N = 1-in-N chance, else dropAlt */\n")
        f.write("\tu8    replaceable;/* Material.isReplaceable(): a placed block\n")
        f.write("\t                   * overwrites this instead of sitting on it */\n")
        f.write("\tu16   family;     /* index into g_blockVariant[]                */\n")
        f.write("\tu8    data;       /* this id's vanilla metadata value           */\n")
        f.write("} BlockProps;\n\n")

        f.write(f"static const BlockProps g_blockProps[NUM_BLOCK_IDS] = {{\n")
        for i, r in enumerate(rows):
            (hardness, drop, alt, mat, speed, htool, level,
             dmin, dmax, denom, repl, family, data) = r
            f.write(f"\t{{ {hardness:.2f}f, {drop}, {alt}, {mat}, {speed}, {htool}, "
                    f"{level}, {dmin}, {dmax}, {denom}, {repl}, {family}, {data} }},"
                    f"  /* {i} {ids[i]} */\n")
        f.write("};\n\n")

        f.write(f"#define NUM_BLOCK_FAMILIES {len(families)}\n\n")
        f.write("/* How each family reacts to being placed (PLACE_*). */\n")
        f.write(f"static const u8 g_familyPlace[NUM_BLOCK_FAMILIES] = {{\n")
        f.write(",".join(str(PLACE_KINDS[P[b][4]] if b in P else 0) for b in families))
        f.write("\n};\n\n")

        f.write("/* family x data value -> global block id, -1 when that variant\n")
        f.write(" * never appeared in any scanned map (so it has no id here). Lets\n")
        f.write(" * placement re-derive an id after Block.onBlockPlaced picks a new\n")
        f.write(" * data value; a -1 means 'keep the id that was picked up'. */\n")
        f.write(f"static const s16 g_blockVariant[NUM_BLOCK_FAMILIES][16] = {{\n")
        for fi, b in enumerate(families):
            f.write("\t{" + ",".join(str(v) for v in variant[fi]) + f"}},  /* {b} */\n")
        f.write("};\n\n")
        f.write("#endif\n")

    n_drop_none = sum(1 for r in rows if r[1] == "-1")
    print(f"wrote {hpath}: {n} ids, {len(families)} families, "
          f"{n_drop_none} ids that drop nothing")


if __name__ == "__main__":
    main()
