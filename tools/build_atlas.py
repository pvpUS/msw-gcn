#!/usr/bin/env python3
"""
build_atlas.py -- assemble a single block-texture atlas from the RKYfault
resource pack, one 16x16 tile per global block id (tile index == line number in
_blockids.txt) used as the *side* face, plus extra tiles appended for blocks
whose top and/or bottom face differ from their sides (grass, logs, sandstone,
furnaces, ...). The GameCube client looks up the side tile straight from the
block's global id; top/bottom overrides come from a generated per-id table
(see write_face_table()) so no separate UV table is shipped for the common
case.

Layout: ATLAS_COLS tiles per row, TILE px each, padded to a power-of-two square.
Output: <out>/atlas.png            (fed to gxtexconv -> atlas.tpl by the build)
        <src>/block_faces_gen.h    (top/bottom tile-index overrides per id)

Textures the pack does not provide at all (hardened clay/terracotta, its dyed
variants) fall back to the vanilla 1.8.9 assets bundled with MCP-919. Textures
that are grayscale biome masks in vanilla (grass top, leaves, vines, tall
grass/ferns) are tinted with the standard "plains" biome colours so they don't
render as flat gray. Colours no pack provides per-variant at all (stained
glass, wool where a file is missing) are still synthesised by tinting a
neutral base with the standard Minecraft dye colour.
"""
import os, sys, hashlib
from PIL import Image

TILE = 16
ATLAS_COLS = 32
# Each tile is stored with PAD pixels of clamped (edge-replicated) border on
# every side, so that mipmap generation -- which box-filters neighbouring
# pixels together -- blends a tile's own edge color into itself instead of
# bleeding in a genuinely different neighbour tile. MAXLOD caps how many mip
# levels the client actually samples (see World_InitGX/GX_InitTexObjLOD);
# PAD must stay >= the box-filter's reach at MAXLOD, which roughly doubles
# per level, so PAD=8 comfortably covers MAXLOD=2 (levels 16px/8px/4px).
# CELL is a power of two so ATLAS_COLS x rows tiles pack into a
# power-of-two atlas -- gxtexconv silently emits an empty TPL for mipmap=yes
# on a non-power-of-two image, so this isn't optional.
PAD = 8
MAXLOD = 2
CELL = TILE + 2 * PAD
assert (CELL & (CELL - 1)) == 0, "CELL must be a power of two for mipmap gen"

PACK = r"C:\Users\awt12\AppData\Roaming\.minecraft\resourcepacks\!                  §bRKYfault§3[16x]"
BLOCKS = os.path.join(PACK, "assets", "minecraft", "textures", "blocks")

# vanilla 1.8.9 textures, used only for ids the resource pack doesn't ship at
# all (e.g. hardened clay / terracotta has no file in RKYfault).
FALLBACK = r"C:\Users\awt12\Downloads\MCP-919-main\MCP-919-main\temp\src\minecraft\assets\minecraft\textures\blocks"

# standard "plains" biome tint colours (net.minecraft.world.ColorizerGrass /
# ColorizerFoliage default: temperature=0.8, rainfall=0.4; spruce/birch use
# their own fixed constants regardless of biome).
GRASS_TINT   = (145, 189, 89)   # ColorizerGrass.getGrassColor(0.8, 0.4)
FOLIAGE_TINT = (119, 171, 47)   # ColorizerFoliage.getFoliageColor(0.8, 0.4)
PINE_TINT    = (97, 153, 97)    # ColorizerFoliage.getFoliageColorPine()
BIRCH_TINT   = (128, 167, 85)   # ColorizerFoliage.getFoliageColorBirch()

# standard dye / wool colours, indexed by data value 0..15
DYE = {
    0:(233,236,236), 1:(240,118,19), 2:(189,68,179), 3:(58,175,217),
    4:(248,198,39), 5:(112,185,25), 6:(237,141,172), 7:(62,68,71),
    8:(142,142,134),9:(21,137,145), 10:(121,42,172), 11:(53,57,157),
    12:(114,71,40), 13:(84,109,27), 14:(161,39,34), 15:(20,21,25),
}
WOOL_NAME = {0:"white",1:"orange",2:"magenta",3:"light_blue",4:"yellow",
    5:"lime",6:"pink",7:"gray",8:"silver",9:"cyan",10:"purple",11:"blue",
    12:"brown",13:"green",14:"red",15:"black"}
WOOD_SPECIES = {0:"oak",1:"spruce",2:"birch",3:"jungle",4:"acacia",5:"dark_oak"}
LOG_SPECIES  = {0:"oak",1:"spruce",2:"birch",3:"jungle"}
LEAF_SPECIES = {0:"oak",1:"spruce",2:"birch",3:"jungle"}

_cache = {}
def load_tex(fname):
    """load a blocks/*.png (RKYfault first, vanilla fallback), crop to the top
    16x16 frame (animations), RGBA."""
    if fname in _cache:
        return _cache[fname]
    im = None
    for base in (BLOCKS, FALLBACK):
        path = os.path.join(base, fname)
        if os.path.exists(path):
            im = Image.open(path).convert("RGBA")
            break
    if im is None:
        _cache[fname] = None
        return None
    if im.width != im.height:                       # animation strip -> frame 0
        im = im.crop((0, 0, im.width, im.width))
    if im.size != (TILE, TILE):
        im = im.resize((TILE, TILE), Image.NEAREST)
    _cache[fname] = im
    return im

def solid(rgb):
    return Image.new("RGBA", (TILE, TILE), (rgb[0], rgb[1], rgb[2], 255))

def tint(base_img, rgb):
    """Eased tint: keeps some of the base texture's own shading visible.
    Used for synthesised variants (stained clay/glass) where the base texture
    isn't a true grayscale biome mask."""
    if base_img is None:
        return solid(rgb)
    out = Image.new("RGBA", (TILE, TILE))
    bp = base_img.load(); op = out.load()
    for y in range(TILE):
        for x in range(TILE):
            r, g, b, a = bp[x, y]
            l = (r + g + b) / (3 * 255.0) * 0.6 + 0.4      # keep some texture
            op[x, y] = (int(rgb[0]*l), int(rgb[1]*l), int(rgb[2]*l), a)
    return out

def biome_tint(base_img, rgb):
    """Direct multiply against a texture's own grayscale value, matching how
    the game colours true biome masks (grass top, leaves, vines, tall grass):
    out = rgb * gray/255, alpha preserved for leaf/plant cutouts."""
    if base_img is None:
        return solid(rgb)
    out = Image.new("RGBA", (TILE, TILE))
    bp = base_img.load(); op = out.load()
    for y in range(TILE):
        for x in range(TILE):
            r, g, b, a = bp[x, y]
            l = (r + g + b) / (3 * 255.0)
            op[x, y] = (int(rgb[0]*l), int(rgb[1]*l), int(rgb[2]*l), a)
    return out

def pad_tile(im, pad):
    """Return a (w+2*pad)x(h+2*pad) canvas with im centered and its border
    pixels clamped/replicated outward, so mipmap box-filtering near the tile
    edge blends with a copy of the tile's own color rather than whatever is
    packed next to it in the atlas."""
    w, h = im.size
    canvas = Image.new("RGBA", (w + 2*pad, h + 2*pad))
    canvas.paste(im, (pad, pad))
    left = im.crop((0, 0, 1, h)).resize((pad, h), Image.NEAREST)
    right = im.crop((w-1, 0, w, h)).resize((pad, h), Image.NEAREST)
    canvas.paste(left, (0, pad))
    canvas.paste(right, (pad + w, pad))
    top = canvas.crop((0, pad, w + 2*pad, pad + 1)).resize((w + 2*pad, pad), Image.NEAREST)
    bottom = canvas.crop((0, pad + h - 1, w + 2*pad, pad + h)).resize((w + 2*pad, pad), Image.NEAREST)
    canvas.paste(top, (0, 0))
    canvas.paste(bottom, (0, pad + h))
    return canvas

def hash_color(name):
    h = hashlib.md5(name.encode()).digest()
    return (90 + h[0] % 130, 90 + h[1] % 130, 90 + h[2] % 130)

def split(bid):
    if ":" in bid:
        b, d = bid.split(":", 1)
        try: return b, int(d)
        except ValueError: return b, 0
    return bid, 0

# direct base-name -> single texture file (used for the side/default face;
# see topbottom_for() for ids whose top/bottom face differs from this)
DIRECT = {
    "STONE":"stone.png", "COBBLESTONE":"cobblestone.png",
    "MOSSY_COBBLESTONE":"cobblestone_mossy.png", "COBBLE_WALL":"cobblestone.png",
    "BEDROCK":"bedrock.png", "GRAVEL":"gravel.png", "SAND":"sand.png",
    "SOIL":"dirt.png", "GRASS":"grass_side.png", "MYCEL":"mycelium_side.png",
    "CLAY":"clay.png", "HARD_CLAY":"hardened_clay.png",
    "BRICK":"brick.png", "NETHER_BRICK":"nether_brick.png",
    "NETHERRACK":"netherrack.png", "SOUL_SAND":"soul_sand.png",
    "OBSIDIAN":"obsidian.png", "GLOWSTONE":"glowstone.png",
    "SEA_LANTERN":"sea_lantern.png", "GLASS":"glass.png",
    "BOOKSHELF":"bookshelf.png", "SPONGE":"sponge.png",
    "COAL_BLOCK":"coal_block.png", "COAL_ORE":"coal_ore.png",
    "IRON_BLOCK":"iron_block.png", "IRON_ORE":"iron_ore.png",
    "GOLD_BLOCK":"gold_block.png", "GOLD_ORE":"gold_ore.png",
    "DIAMOND_BLOCK":"diamond_block.png", "DIAMOND_ORE":"diamond_ore.png",
    "LAPIS_BLOCK":"lapis_block.png", "REDSTONE_BLOCK":"redstone_block.png",
    "QUARTZ_ORE":"quartz_ore.png", "REDSTONE_LAMP_ON":"redstone_lamp_on.png",
    "ICE":"ice.png", "PACKED_ICE":"ice_packed.png", "SNOW":"snow.png",
    "SNOW_BLOCK":"snow.png", "SLIME_BLOCK":"slime.png",
    "HAY_BLOCK":"hay_block_side.png", "WEB":"web.png",
    "WATER_LILY":"waterlily.png", "VINE":"vine.png", "LADDER":"ladder.png",
    "SPONGE":"sponge.png", "WORKBENCH":"crafting_table_side.png",
    "FURNACE":"furnace_side.png", "CHEST":"planks_oak.png",
    "BOOKSHELF":"bookshelf.png", "JUKEBOX":"planks_oak.png",
    "NOTE_BLOCK":"planks_oak.png", "ENCHANTMENT_TABLE":"enchanting_table_side.png",
    "DEAD_BUSH":"deadbush.png", "LONG_GRASS":"tallgrass.png",
    "YELLOW_FLOWER":"flower_dandelion.png", "RED_ROSE":"flower_rose.png",
    "BROWN_MUSHROOM":"mushroom_brown.png", "RED_MUSHROOM":"mushroom_red.png",
    "HUGE_MUSHROOM_1":"mushroom_block_skin_brown.png",
    "HUGE_MUSHROOM_2":"mushroom_block_skin_red.png",
    "TORCH":"torch_on.png", "FIRE":"lava_still.png",
    "WATER":"water_still.png", "STATIONARY_WATER":"water_still.png",
    "IRON_FENCE":"iron_bars.png", "IRON_TRAPDOOR":"iron_trapdoor.png",
    "TRAP_DOOR":"trapdoor.png", "ANVIL":"anvil_base.png",
    "HOPPER":"hopper_outside.png", "DROPPER":"furnace_side.png",
    "PISTON_BASE":"piston_side.png", "CAULDRON":"cauldron_side.png",
    "BREWING_STAND":"brewing_stand_base.png", "SKULL":"soul_sand.png",
    "COCOA":"log_jungle.png", "CROPS":"wheat_stage_7.png",
    "NETHER_WARTS":"soul_sand.png", "BED_BLOCK":"bed_feet_side.png",
    "STONE_PLATE":"stone.png", "WOOD_PLATE":"planks_oak.png",
    "STONE_BUTTON":"stone.png", "WOOD_BUTTON":"planks_oak.png",
    "LEVER":"cobblestone.png", "TRIPWIRE_HOOK":"planks_oak.png",
    "REDSTONE_WIRE":"redstone_block.png", "RAILS":"rail_normal.png",
    "POWERED_RAIL":"rail_golden.png", "SIGN_POST":"planks_oak.png",
    "WALL_SIGN":"planks_oak.png",
    "DOUBLE_PLANT":"double_plant_grass_top.png",
    "FENCE":"planks_oak.png", "FENCE_GATE":"planks_oak.png",
    "SPRUCE_FENCE":"planks_spruce.png", "SPRUCE_FENCE_GATE":"planks_spruce.png",
    "BIRCH_FENCE":"planks_birch.png", "BIRCH_FENCE_GATE":"planks_birch.png",
    "JUNGLE_FENCE":"planks_jungle.png", "ACACIA_FENCE":"planks_acacia.png",
    "DARK_OAK_FENCE":"planks_dark_oak.png", "DARK_OAK_FENCE_GATE":"planks_dark_oak.png",
    "NETHER_FENCE":"nether_brick.png",
    "WOOD_STAIRS":"planks_oak.png", "BIRCH_WOOD_STAIRS":"planks_birch.png",
    "SPRUCE_WOOD_STAIRS":"planks_spruce.png", "JUNGLE_WOOD_STAIRS":"planks_jungle.png",
    "ACACIA_STAIRS":"planks_acacia.png", "DARK_OAK_STAIRS":"planks_dark_oak.png",
    "BRICK_STAIRS":"brick.png", "COBBLESTONE_STAIRS":"cobblestone.png",
    "SMOOTH_STAIRS":"stonebrick.png", "NETHER_BRICK_STAIRS":"nether_brick.png",
    "QUARTZ_STAIRS":"quartz_block_side.png",
    "SANDSTONE_STAIRS":"sandstone_normal.png",
    "RED_SANDSTONE_STAIRS":"red_sandstone_normal.png",
}

def texture_for(bid):
    """returns a 16x16 RGBA Image for a full block id like 'WOOL:14'. This is
    the *side* face texture (and the only one, for ids with no top/bottom
    override -- see topbottom_for())."""
    base, data = split(bid)

    if base in ("WOOL", "CARPET"):
        f = "wool_colored_%s.png" % WOOL_NAME.get(data, "white")
        im = load_tex(f)
        return im if im else solid(DYE.get(data, (200,200,200)))
    if base == "HARD_CLAY":
        return load_tex("hardened_clay.png") or load_tex("clay.png")
    if base == "STAINED_CLAY":
        f = "hardened_clay_stained_%s.png" % WOOL_NAME.get(data, "white")
        im = load_tex(f)
        return im if im else tint(load_tex("hardened_clay.png") or
                                   load_tex("clay.png"), DYE.get(data, (150,100,80)))
    if base == "STAINED_GLASS" or base == "STAINED_GLASS_PANE":
        return tint(load_tex("glass.png"), DYE.get(data, (200,200,200)))
    if base in ("WOOD", "WOOD_STEP", "WOOD_DOUBLE_STEP"):
        return load_tex("planks_%s.png" % WOOD_SPECIES.get(data & 7, "oak")) \
               or solid((160,130,80))
    if base == "LOG":
        return load_tex("log_%s.png" % LOG_SPECIES.get(data & 3, "oak")) \
               or solid((120,90,55))
    if base == "LOG_2":
        sp = "acacia" if (data & 1) == 0 else "big_oak"
        return load_tex("log_%s.png" % sp) or solid((120,90,55))
    if base == "LEAVES":
        sp = LEAF_SPECIES.get(data & 3, "oak")
        leaf_tint = PINE_TINT if sp == "spruce" else BIRCH_TINT if sp == "birch" else FOLIAGE_TINT
        return biome_tint(load_tex("leaves_%s.png" % sp), leaf_tint)
    if base == "LEAVES_2":
        sp = "acacia" if (data & 1) == 0 else "big_oak"
        return biome_tint(load_tex("leaves_%s.png" % sp), FOLIAGE_TINT)
    if base == "VINE":
        return biome_tint(load_tex("vine.png"), FOLIAGE_TINT)
    if base == "LONG_GRASS":
        # data 0 = dead bush (drawn via DEAD_BUSH normally), 1 = tall grass, 2 = fern
        f = "fern.png" if data == 2 else "tallgrass.png"
        return biome_tint(load_tex(f), GRASS_TINT)
    if base in ("STEP", "DOUBLE_STEP"):
        slab = {0:"stone.png",1:"sandstone_normal.png",2:"planks_oak.png",
                3:"cobblestone.png",4:"brick.png",5:"stonebrick.png",
                6:"nether_brick.png",7:"quartz_block_side.png"}
        return load_tex(slab.get(data & 7, "stone.png")) or solid((150,150,150))
    if base in ("STONE_SLAB2",):
        return load_tex("red_sandstone_normal.png") or solid((180,90,60))
    if base == "SMOOTH_BRICK":
        sb = {0:"stonebrick.png",1:"stonebrick_mossy.png",
              2:"stonebrick_cracked.png",3:"stonebrick_carved.png"}
        return load_tex(sb.get(data, "stonebrick.png")) or solid((130,130,130))
    if base == "SANDSTONE":
        ss = {0:"sandstone_normal.png",1:"sandstone_carved.png",
              2:"sandstone_smooth.png"}
        return load_tex(ss.get(data, "sandstone_normal.png")) or solid((219,205,157))
    if base == "RED_SANDSTONE":
        rs = {0:"red_sandstone_normal.png",1:"red_sandstone_carved.png",
              2:"red_sandstone_smooth.png"}
        return load_tex(rs.get(data, "red_sandstone_normal.png")) or solid((180,90,50))
    if base == "QUARTZ_BLOCK":
        q = {0:"quartz_block_side.png",1:"quartz_block_chiseled.png",
             2:"quartz_block_lines.png",3:"quartz_block_lines.png",
             4:"quartz_block_lines.png"}
        return load_tex(q.get(data, "quartz_block_side.png")) or solid((235,233,227))
    if base == "PRISMARINE":
        pm = {0:"prismarine_rough.png",1:"prismarine_bricks.png",2:"prismarine_dark.png"}
        return load_tex(pm.get(data, "prismarine_rough.png")) or solid((99,171,158))
    if base == "DIRT":
        dd = {0:"dirt.png",1:"coarse_dirt.png",2:"dirt_podzol_side.png"}
        return load_tex(dd.get(data, "dirt.png")) or solid((134,96,67))
    if base == "STONE":
        st = {0:"stone.png",1:"stone_granite.png",2:"stone_granite_smooth.png",
              3:"stone_diorite.png",4:"stone_diorite_smooth.png",
              5:"stone_andesite.png",6:"stone_andesite_smooth.png"}
        return load_tex(st.get(data, "stone.png")) or solid((130,130,130))
    if base in ("WOODEN_DOOR", "JUNGLE_DOOR"):
        # bit3(8) = upper half (BlockDoor.java's getStateFromMeta: (meta & 8)
        # > 0 ? UPPER : LOWER) -- was previously hardcoded to *_upper.png
        # even for lower-half ids.
        sp = "wood" if base == "WOODEN_DOOR" else "jungle"
        half = "upper" if (data & 8) else "lower"
        return load_tex("door_%s_%s.png" % (sp, half)) or solid((110,85,50))
    if base == "MONSTER_EGGS":
        return load_tex("stonebrick.png") or solid((128,128,128))
    if base == "COBBLE_WALL":
        return load_tex("cobblestone_mossy.png" if data == 1 else "cobblestone.png") \
               or solid((130,130,130))
    if base == "DOUBLE_PLANT":
        dp = {0:("double_plant_sunflower_bottom.png","double_plant_sunflower_top.png"),
              1:("double_plant_syringa_bottom.png","double_plant_syringa_top.png"),
              2:("double_plant_grass_bottom.png","double_plant_grass_top.png"),
              3:("double_plant_fern_bottom.png","double_plant_fern_top.png"),
              4:("double_plant_rose_bottom.png","double_plant_rose_top.png"),
              5:("double_plant_paeonia_bottom.png","double_plant_paeonia_top.png")}
        lo, hi = dp.get(data & 7, dp[2])
        f = hi if (data & 8) else lo
        im = load_tex(f)
        if data & 7 in (2, 3):          # grass/fern double-plants are tinted
            return biome_tint(im, GRASS_TINT)
        return im or solid((60,140,50))

    if base in DIRECT:
        im = load_tex(DIRECT[base])
        if im:
            return im

    # last resort: try a lowercased direct filename, else hashed colour
    guess = load_tex(base.lower() + ".png")
    if guess:
        return guess
    return solid(hash_color(bid))

# Shape ids for non-full-cube blocks. Numeric values MUST match
# source/block_shapes.h's enum exactly -- both sides index the generated
# per-global-id tables (block_shapes_gen.h) by these values.
SHAPE_IDS = {
    "CUBE": 0, "SLAB": 1, "STAIR": 2, "FENCE": 3, "FENCE_GATE": 4,
    "WALL": 5, "PANE": 6, "ANVIL": 7, "ENCHANT_TABLE": 8, "TRAPDOOR": 9,
    "DOOR": 10,
}

def shape_for(bid):
    """Returns (shape_name, param_byte) for a block id -- the non-cube shape
    (if any) and a packed per-shape param (facing/open/top/etc. bits, meaning
    defined per shape). Extended incrementally per block family; ids not
    covered here default to CUBE (the existing full-cube fast path).

    For STAIR/SLAB the param is simply the block's own data value: the scan
    already captured real vanilla metadata per voxel (see _blockids.txt), and
    vanilla's own bit layout for these two families (ground-truthed against
    MCP-919's BlockStairs.java/BlockStoneSlab.java) is exactly what
    block_shapes.c decodes: bit3(8)=top half (SLAB), bits0-1=facing +
    bit2(4)=top half (STAIR) -- no repacking needed."""
    base, data = split(bid)

    if base.endswith("STAIRS"):
        return "STAIR", data
    if base in ("STEP", "WOOD_STEP", "STONE_SLAB2"):
        return "SLAB", data

    # FENCE_GATE param = raw data (bits0-1 facing, bit2 open -- ground-truthed
    # against BlockFenceGate.java's getStateFromMeta). FENCE/WALL shape is
    # purely neighbor-connectivity driven (no per-id data affects it -- wall's
    # own data selects its texture VARIANT, handled above in texture_for()),
    # so their param is unused; pass 0.
    if base.endswith("FENCE_GATE"):
        return "FENCE_GATE", data
    if base.endswith("FENCE") and base != "IRON_FENCE":  # IRON_FENCE = bars, a PANE (M4)
        return "FENCE", 0
    if base == "COBBLE_WALL":
        return "WALL", 0
    # PANE shape covers both iron bars and glass panes (vanilla's BlockPane
    # base class handles both identically, including cross-connecting to
    # each other -- see shapegrid_link's same-shape connect rule in world.c).
    if base in ("IRON_FENCE", "GLASS_PANE", "STAINED_GLASS_PANE"):
        return "PANE", 0

    # ANVIL param = raw data (bits0-1 facing, bits2+ damage -- ground-truthed
    # against BlockAnvil.java's getStateFromMeta/getMetaFromState).
    if base == "ANVIL":
        return "ANVIL", data
    if base == "ENCHANTMENT_TABLE":
        return "ENCHANT_TABLE", 0

    # TRAPDOOR/DOOR param = raw data, ground-truthed against
    # BlockTrapDoor.java/BlockDoor.java (see block_shapes.c for exact bit
    # layout + the door-specific simplification noted there).
    if base in ("TRAP_DOOR", "IRON_TRAPDOOR"):
        return "TRAPDOOR", data
    if base in ("WOODEN_DOOR", "JUNGLE_DOOR"):
        return "DOOR", data

    return "CUBE", 0

def topbottom_for(bid):
    """returns (top_img, bottom_img), each None if the face doesn't differ
    from the side texture returned by texture_for()."""
    base, data = split(bid)

    if base == "GRASS":
        return biome_tint(load_tex("grass_top.png"), GRASS_TINT), load_tex("dirt.png")
    if base == "MYCEL":
        return load_tex("mycelium_top.png"), load_tex("dirt.png")
    if base == "DIRT" and data == 2:
        return load_tex("dirt_podzol_top.png"), load_tex("dirt.png")
    if base == "LOG":
        sp = LOG_SPECIES.get(data & 3, "oak")
        top = load_tex("log_%s_top.png" % sp)
        return top, top
    if base == "LOG_2":
        sp = "acacia" if (data & 1) == 0 else "big_oak"
        top = load_tex("log_%s_top.png" % sp)
        return top, top
    if base == "SANDSTONE":
        return load_tex("sandstone_top.png"), load_tex("sandstone_bottom.png")
    if base == "RED_SANDSTONE" or base == "STONE_SLAB2":
        return load_tex("red_sandstone_top.png"), load_tex("red_sandstone_bottom.png")
    if base == "QUARTZ_BLOCK":
        qt = {0:"quartz_block_top.png",1:"quartz_block_chiseled_top.png",
              2:"quartz_block_lines_top.png",3:"quartz_block_lines_top.png",
              4:"quartz_block_lines_top.png"}
        top = load_tex(qt.get(data, "quartz_block_top.png"))
        bot = load_tex("quartz_block_bottom.png") if data == 0 else top
        return top, bot
    if base == "FURNACE":
        top = load_tex("furnace_top.png")
        return top, top
    if base == "HAY_BLOCK":
        top = load_tex("hay_block_top.png")
        return top, top
    if base == "WORKBENCH":
        return load_tex("crafting_table_top.png"), load_tex("planks_oak.png")
    if base == "BED_BLOCK":
        return load_tex("bed_feet_top.png"), load_tex("planks_oak.png")
    if base == "ENCHANTMENT_TABLE":
        return load_tex("enchanting_table_top.png"), load_tex("enchanting_table_bottom.png")
    if base == "ANVIL":
        # No dedicated bottom texture in vanilla -- anvil_base.png (the side/
        # default tile) covers the bottom too, only the top differs by damage.
        damage = data >> 2
        top = load_tex("anvil_top_damaged_%d.png" % damage)
        return top, None
    return None, None

def load_book_tile():
    """Crop a representative 16x16 tile from the enchanting table's floating
    book texture (entity sheet, not a blocks/*.png -- ModelBook's cover/spine
    boxes live in its top-left corner) for the static book box rendered above
    the table (see block_shapes.c's mesh_enchant_table -- no animation, per
    the task, so one flat tile stands in for the full multi-box page model)."""
    for base in (os.path.join(PACK, "assets", "minecraft", "textures", "entity"),
                 os.path.join(FALLBACK, "..", "entity")):
        path = os.path.join(base, "enchanting_table_book.png")
        if os.path.exists(path):
            return Image.open(path).convert("RGBA").crop((0, 0, TILE, TILE))
    return solid((139, 106, 65))

def main():
    ids_path = sys.argv[1] if len(sys.argv) > 1 else \
        r"C:\Users\awt12\Downloads\download (1)\BlockScans\_blockids.txt"
    out_dir = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.join(os.path.dirname(__file__), "..", "data")
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    ids = [l.strip() for l in open(ids_path, encoding="utf-8") if l.strip()]

    tiles = [None] * len(ids)          # tile index -> image; grows past len(ids)
    top_override = [0] * len(ids)      # global id -> tile index (self if none)
    bot_override = [0] * len(ids)

    for i, bid in enumerate(ids):
        side = texture_for(bid)
        tiles[i] = side
        top_override[i] = i
        bot_override[i] = i

        top, bot = topbottom_for(bid)
        if top is not None:
            top_override[i] = len(tiles)
            tiles.append(top)
        if bot is not None:
            bot_override[i] = len(tiles)
            tiles.append(bot)

    book_tile = len(tiles)
    tiles.append(load_book_tile())

    n_tiles = len(tiles)
    rows = (n_tiles + ATLAS_COLS - 1) // ATLAS_COLS
    pow2_rows = 1
    while pow2_rows < rows:
        pow2_rows *= 2
    rows = pow2_rows                    # gxtexconv needs a power-of-two image for mipmaps
    atlas_w = ATLAS_COLS * CELL
    atlas_h = rows * CELL

    atlas = Image.new("RGBA", (atlas_w, atlas_h), (0, 0, 0, 0))
    for i, im in enumerate(tiles):
        col = i % ATLAS_COLS
        row = i // ATLAS_COLS
        atlas.paste(pad_tile(im, PAD), (col * CELL, row * CELL))
    outp = os.path.join(out_dir, "atlas.png")
    atlas.save(outp)
    print(f"atlas: {len(ids)} block ids, {n_tiles} tiles -> {outp} ({atlas_w}x{atlas_h}, "
          f"{CELL}px cells = {TILE}px tile + {PAD}px pad)")
    print(f"  cols={ATLAS_COLS}; base tile index == global block id, "
          f"top/bottom overrides in block_faces_gen.h")

    write_face_table(ids, top_override, bot_override)
    write_atlas_geometry(atlas_w, atlas_h)
    write_shape_table(ids, book_tile)

def write_face_table(ids, top_override, bot_override):
    src_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "source"))
    hpath = os.path.join(src_dir, "block_faces_gen.h")
    n = len(ids)
    n_overridden = sum(1 for i in range(n) if top_override[i] != i or bot_override[i] != i)
    with open(hpath, "w", encoding="utf-8") as f:
        f.write("/* Generated by tools/build_atlas.py -- do not edit. */\n")
        f.write("#ifndef MSW_BLOCK_FACES_GEN_H\n#define MSW_BLOCK_FACES_GEN_H\n\n")
        f.write(f"#define NUM_BLOCK_IDS {n}\n\n")
        f.write("/* Atlas tile index to use for the +Y (top) face of global block id i;\n")
        f.write(" * equals i itself (the side tile) when the top face isn't overridden. */\n")
        f.write("static const u16 g_topTile[NUM_BLOCK_IDS] = {\n")
        f.write(",".join(str(v) for v in top_override))
        f.write("\n};\n\n")
        f.write("/* Same, for the -Y (bottom) face. */\n")
        f.write("static const u16 g_bottomTile[NUM_BLOCK_IDS] = {\n")
        f.write(",".join(str(v) for v in bot_override))
        f.write("\n};\n\n")
        f.write("#endif\n")
    print(f"  wrote {hpath} ({n_overridden} ids with a top/bottom override)")

def write_shape_table(ids, book_tile):
    """Emit the per-global-id non-cube shape table (source/block_shapes_gen.h)
    that world.c's mesher and World_BlockBoxes() collision query both consult
    to dispatch away from the default full-cube path. See shape_for()."""
    src_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "source"))
    hpath = os.path.join(src_dir, "block_shapes_gen.h")
    shapes = []
    params = []
    for bid in ids:
        name, param = shape_for(bid)
        shapes.append(SHAPE_IDS[name])
        params.append(param & 0xFF)
    with open(hpath, "w", encoding="utf-8") as f:
        f.write("/* Generated by tools/build_atlas.py -- do not edit. */\n")
        f.write("#ifndef MSW_BLOCK_SHAPES_GEN_H\n#define MSW_BLOCK_SHAPES_GEN_H\n\n")
        f.write('#include "block_shapes.h"\n\n')
        f.write(f"static const u8 g_blockShape[{len(ids)}] = {{\n")
        f.write(",".join(str(v) for v in shapes))
        f.write("\n};\n\n")
        f.write("/* Packed per-shape params (facing/open/top/etc.), meaning defined\n")
        f.write(" * per shape id -- see the emitters in block_shapes.c. */\n")
        f.write(f"static const u8 g_blockParam[{len(ids)}] = {{\n")
        f.write(",".join(str(v) for v in params))
        f.write("\n};\n\n")
        f.write("#endif\n")
    n_special = sum(1 for s in shapes if s != 0)
    print(f"  wrote {hpath} ({n_special} non-cube ids)")

    # Separate tiny header (not block_shapes_gen.h) so block_shapes.c -- which
    # needs this constant but not the (world.c-only) per-id shape tables above
    # -- doesn't pull in two unused 533-entry static arrays (-Wunused-variable
    # plus dead weight in its object file).
    bpath = os.path.join(src_dir, "block_book_gen.h")
    with open(bpath, "w", encoding="utf-8") as f:
        f.write("/* Generated by tools/build_atlas.py -- do not edit. */\n")
        f.write("#ifndef MSW_BLOCK_BOOK_GEN_H\n#define MSW_BLOCK_BOOK_GEN_H\n\n")
        f.write("/* Atlas tile for the enchanting table's static floating book\n")
        f.write(" * (mesh_enchant_table in block_shapes.c). */\n")
        f.write(f"#define ENCHANT_BOOK_TILE {book_tile}\n\n")
        f.write("#endif\n")
    print(f"  wrote {bpath}")

def write_atlas_geometry(atlas_w, atlas_h):
    """Emit the atlas layout constants world.c needs to turn a tile index
    into UV coordinates. Only atlas_h is actually data-dependent (grows with
    the tile count); the rest mirror the constants above so there's a single
    generated source of truth instead of two files that can drift apart."""
    src_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "source"))
    hpath = os.path.join(src_dir, "atlas_gen.h")
    with open(hpath, "w", encoding="utf-8") as f:
        f.write("/* Generated by tools/build_atlas.py -- do not edit. */\n")
        f.write("#ifndef MSW_ATLAS_GEN_H\n#define MSW_ATLAS_GEN_H\n\n")
        f.write(f"#define ATLAS_TILE {TILE}\n")
        f.write(f"#define ATLAS_PAD {PAD}\n")
        f.write(f"#define ATLAS_CELL {CELL}\n")
        f.write(f"#define ATLAS_COLS {ATLAS_COLS}\n")
        f.write(f"#define ATLAS_TEX_W {atlas_w}\n")
        f.write(f"#define ATLAS_TEX_H {atlas_h}\n")
        f.write(f"#define ATLAS_MAXLOD {MAXLOD}\n")
        f.write("\n#endif\n")
    print(f"  wrote {hpath} ({atlas_w}x{atlas_h}, maxlod={MAXLOD})")

if __name__ == "__main__":
    main()
