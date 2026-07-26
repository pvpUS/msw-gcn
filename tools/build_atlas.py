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

def min_alpha(im):
    """Smallest alpha value anywhere in a tile (255 for a None/absent face,
    which reads as 'same as the side texture' -- see topbottom_for)."""
    if im is None:
        return 255
    return im.getchannel("A").getextrema()[0]

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
    "LAVA":"lava_still.png", "STATIONARY_LAVA":"lava_still.png",
    "TNT":"tnt_side.png",
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

def checker(a, b, cell=4):
    """Two-colour checkerboard, for the palette sentinel's placeholder tile."""
    im = Image.new("RGBA", (TILE, TILE))
    p = im.load()
    for y in range(TILE):
        for x in range(TILE):
            c = a if ((x // cell) + (y // cell)) % 2 == 0 else b
            p[x, y] = (c[0], c[1], c[2], 255)
    return im

def texture_for(bid):
    """returns a 16x16 RGBA Image for a full block id like 'WOOL:14'. This is
    the *side* face texture (and the only one, for ids with no top/bottom
    override -- see topbottom_for())."""
    base, data = split(bid)

    # The palette sentinel (last line of data/blockids.txt): the id a 1.8 block
    # state with no global id resolves to, and what World_SetBlock clamps an
    # out-of-range id to. Deliberately the loudest thing on screen -- it only
    # ever appears when the palette is wrong, and it is only ever *loaded* for
    # offline diagnostics (the proxy drops unmapped states rather than sending
    # this; see tools/gen_blockmap.py).
    if base == "UNKNOWN_BLOCK":
        return checker((255, 0, 220), (16, 16, 16))

    if base == "SAND":
        # data 1 = red sand (BlockSand.EnumType). Previously SAND fell through
        # to DIRECT["SAND"] = sand.png for every data value, so red sand
        # (SAND:1) rendered as ordinary yellow sand.
        return load_tex("red_sand.png" if data == 1 else "sand.png") \
               or solid((219, 207, 163))
    if base == "RED_ROSE":
        # BlockFlower.EnumFlowerType by data: 0 poppy, 1 blue orchid, 2 allium,
        # 3 azure bluet, 4-7 tulips (red/orange/white/pink), 8 oxeye daisy.
        # Was hardcoded to flower_rose.png (poppy) for every variant.
        fl = {0: "flower_rose.png", 1: "flower_blue_orchid.png",
              2: "flower_allium.png", 3: "flower_houstonia.png",
              4: "flower_tulip_red.png", 5: "flower_tulip_orange.png",
              6: "flower_tulip_white.png", 7: "flower_tulip_pink.png",
              8: "flower_oxeye_daisy.png"}
        return load_tex(fl.get(data, "flower_rose.png")) or solid((200, 60, 60))

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
    "DOOR": 10, "CROSS": 11, "TORCH": 12, "LADDER": 13, "VINE": 14,
    "PLATE": 15, "CHEST": 16, "SKULL": 17,
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

    # Air-passable decorations rendered as their vanilla non-cube models (see
    # block_shapes.c). CROSS = diagonal crossed planes (flowers, tall grass,
    # ferns, dead bush, saplings, small mushrooms, and each half of a double
    # plant); all pass 0 for param (no per-id data affects the shape). Their
    # collision is empty -- BlockShape_Boxes returns 0 boxes, so the player
    # walks through, matching vanilla.
    if base in ("YELLOW_FLOWER", "RED_ROSE", "LONG_GRASS", "DEAD_BUSH",
                "DOUBLE_PLANT", "BROWN_MUSHROOM", "RED_MUSHROOM", "SAPLING"):
        return "CROSS", 0

    # TORCH param = raw meta (BlockTorch.getStateFromMeta: 1=E 2=W 3=S 4=N
    # facing, 5=standing). LADDER param = facing meta (2=N 3=S 4=W 5=E,
    # EnumFacing.getFront). VINE param = attach bitmask (1=S 2=W 4=N 8=E).
    # SKULL param = meta (bits0-2 = EnumFacing.getFront: 1=floor, 2..5 = wall).
    if base == "TORCH":
        return "TORCH", data
    if base == "LADDER":
        return "LADDER", data
    if base == "VINE":
        return "VINE", data
    if base.endswith("PLATE"):          # STONE_PLATE, WOOD_PLATE, ...
        return "PLATE", 0
    if base.endswith("CHEST"):          # CHEST, TRAPPED_CHEST, ENDER_CHEST
        return "CHEST", data
    if base == "SKULL":
        return "SKULL", data

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
    if base == "TNT":
        return load_tex("tnt_top.png"), load_tex("tnt_bottom.png")
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

def load_book_tiles():
    """Two 16x16 tiles for the static closed floating book rendered above the
    enchanting table (block_shapes.c's mesh_enchant_table): the leather cover
    (its two flat faces) and the white page edges (its four sides). Cropped
    from the entity book sheet (not a blocks/*.png) using ModelBook's texture
    layout -- cover face at uv [0,0..6,10], a page at uv [24,10..29,18] -- then
    stretched to fill a full tile (the source regions are near-uniform, so the
    NEAREST upscale just keeps the pixel-art look). No animation/tilt: this
    engine has no rotation, so a flat closed book stands in for vanilla's
    open, bobbing multi-box page model."""
    for base in (os.path.join(PACK, "assets", "minecraft", "textures", "entity"),
                 os.path.join(FALLBACK, "..", "entity")):
        path = os.path.join(base, "enchanting_table_book.png")
        if os.path.exists(path):
            im = Image.open(path).convert("RGBA")
            cover = im.crop((0, 0, 6, 10)).resize((TILE, TILE), Image.NEAREST)
            pages = im.crop((24, 10, 29, 18)).resize((TILE, TILE), Image.NEAREST)
            return cover, pages
    return solid((139, 106, 65)), solid((222, 216, 198))

def load_steve_tiles():
    """Three 16x16 tiles for the default Steve head used on every skull
    (block_shapes.c's mesh_skull -- skulls are TESRs with no block model, and
    per the task all default to Steve). Cropped from the 64x64 player skin's
    head faces: front (8,8..16,16) for the four sides, top (8,0..16,8) and
    bottom (16,0..24,8) for +Y/-Y, each an 8x8 region scaled up to a full tile
    (NEAREST keeps the pixel-art look)."""
    for base in (os.path.join(PACK, "assets", "minecraft", "textures", "entity"),
                 os.path.join(FALLBACK, "..", "entity")):
        path = os.path.join(base, "steve.png")
        if os.path.exists(path):
            im = Image.open(path).convert("RGBA")
            front = im.crop((8, 8, 16, 16)).resize((TILE, TILE), Image.NEAREST)
            top = im.crop((8, 0, 16, 8)).resize((TILE, TILE), Image.NEAREST)
            bottom = im.crop((16, 0, 24, 8)).resize((TILE, TILE), Image.NEAREST)
            return front, top, bottom
    fallback = solid((198, 134, 66))    # skin tone, if no skin file is found
    return fallback, fallback, fallback

# Held/dropped ITEMS (as opposed to placeable blocks). Each gets one appended
# atlas tile whose index doubles as its inventory ItemStack.item id -- being
# >= NUM_BLOCK_IDS is exactly how helditem.c/hud.c tell "flat item" from
# "block". Emitted as ITEM_<NAME>_TILE in block_book_gen.h; source/items.h
# gives the tools their dig properties and block_props_gen.h references the
# rest as block drops (coal from coal ore, clay balls from clay, ...).
# (name, textures/items/<file>, fallback solid colour)
ITEM_TEXTURES = [
    ("DIAMOND_SWORD",   "diamond_sword.png",    (90, 210, 220)),
    ("DIAMOND_PICKAXE", "diamond_pickaxe.png",  (90, 210, 220)),
    ("DIAMOND_AXE",     "diamond_axe.png",      (90, 210, 220)),
    ("DIAMOND_SHOVEL",  "diamond_shovel.png",   (90, 210, 220)),
    ("COAL",            "coal.png",             (30, 30, 30)),
    ("DIAMOND",         "diamond.png",          (90, 210, 220)),
    ("REDSTONE",        "redstone_dust.png",    (200, 30, 30)),
    ("QUARTZ",          "quartz.png",           (230, 225, 215)),
    ("FLINT",           "flint.png",            (60, 55, 55)),
    ("CLAY_BALL",       "clay_ball.png",        (160, 165, 180)),
    ("STRING",          "string.png",           (230, 230, 230)),
    ("SNOWBALL",        "snowball.png",         (240, 250, 255)),
    ("WHEAT",           "wheat.png",            (215, 190, 80)),
    ("WHEAT_SEEDS",     "seeds_wheat.png",      (140, 170, 70)),
    ("BOOK",            "book_normal.png",      (170, 130, 90)),
    ("SIGN",            "sign.png",             (160, 130, 80)),
    ("DOOR_WOOD",       "door_wood.png",        (150, 120, 70)),
    ("BED",             "bed.png",              (190, 60, 60)),
    ("COCOA_BEANS",     "dye_powder_brown.png", (120, 70, 40)),
    ("NETHER_WART",     "nether_wart.png",      (150, 30, 40)),
]

# Block-breaking crack overlay (destroy_stage_0..9), drawn over the block the
# player is currently mining -- see interact.c / World_DrawBreakOverlay. These
# are blocks/*.png, not items/*.png, and are appended as one contiguous run so
# the client can index them as DESTROY_STAGE_TILE + stage.
DESTROY_STAGES = 10

def load_item_tex(fname):
    """Load a textures/items/*.png (RKYfault first, vanilla 1.8.9 fallback),
    cropped/resized to a 16x16 RGBA tile. Items live under items/, not blocks/,
    so this is separate from load_tex(). Used for the handful of held ITEMS
    (as opposed to placeable blocks) this engine needs -- e.g. the diamond
    sword; see helditem.c."""
    for base in (os.path.join(PACK, "assets", "minecraft", "textures", "items"),
                 os.path.join(FALLBACK, "..", "items")):
        path = os.path.join(base, fname)
        if os.path.exists(path):
            im = Image.open(path).convert("RGBA")
            if im.width != im.height:                   # animation strip -> frame 0
                im = im.crop((0, 0, im.width, im.width))
            if im.size != (TILE, TILE):
                im = im.resize((TILE, TILE), Image.NEAREST)
            return im
    return None

def main():
    ids_path = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(os.path.dirname(__file__), "..", "data", "blockids.txt")
    out_dir = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.join(os.path.dirname(__file__), "..", "data")
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    ids = [l.strip() for l in open(ids_path, encoding="utf-8") if l.strip()]

    tiles = [None] * len(ids)          # tile index -> image; grows past len(ids)
    top_override = [0] * len(ids)      # global id -> tile index (self if none)
    bot_override = [0] * len(ids)
    opaque = [0] * len(ids)            # global id -> 1 if a full occluder cube

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

        # Face-culling occluder test: a full cube's shared face may be culled
        # against this block only if it is *itself* a full cube whose every
        # rendered face fully passes the world shader's alpha test (GEQUAL 128).
        # A see-through cube (glass centre, gaps between leaves) leaves holes the
        # alpha test discards, so culling behind it would show the deleted face
        # -- you'd look straight through the terrain into the void. Non-cube
        # shapes never reliably cover the voxel boundary, so they never occlude.
        # Consumed as g_blockOpaque[] by world.c's occ_opaque().
        name, _param = shape_for(bid)
        face_min = min(min_alpha(side), min_alpha(top), min_alpha(bot))
        opaque[i] = 1 if (name == "CUBE" and face_min >= 128) else 0

    book_cover, book_pages = load_book_tiles()
    book_cover_tile = len(tiles)
    tiles.append(book_cover)
    book_pages_tile = len(tiles)
    tiles.append(book_pages)

    # The anvil's working-surface texture (#top in vanilla anvil.json), used
    # only on the up-face of the anvil's top box (block_shapes.c mesh_anvil);
    # every other anvil face uses anvil_base (#body). Undamaged variant only --
    # this engine doesn't track the per-anvil damage level.
    anvil_top_tile = len(tiles)
    tiles.append(load_tex("anvil_top_damaged_0.png") or solid((67, 67, 71)))

    # Default Steve head tiles for skulls (block_shapes.c mesh_skull): side
    # (front face) on the 4 walls, top and bottom for +Y/-Y.
    steve_side, steve_top, steve_bottom = load_steve_tiles()
    skull_side_tile = len(tiles);   tiles.append(steve_side)
    skull_top_tile = len(tiles);    tiles.append(steve_top)
    skull_bottom_tile = len(tiles); tiles.append(steve_bottom)

    # Held/dropped ITEMS -- not placeable blocks, so they have no global block
    # id. Each one's atlas tile index (appended past every block tile) doubles
    # as the inventory ItemStack.item id: being >= NUM_BLOCK_IDS is exactly how
    # helditem.c tells "flat item" from "block", while hud.c's tile_icon() draws
    # it straight from that index like any other slot icon. See ITEM_TEXTURES.
    item_tiles = {}
    for name, fname, fallback_rgb in ITEM_TEXTURES:
        item_tiles[name] = len(tiles)
        tiles.append(load_item_tex(fname) or solid(fallback_rgb))

    # Block-breaking crack overlay, one tile per destroy stage, contiguous.
    destroy_stage_tile = len(tiles)
    for i in range(DESTROY_STAGES):
        tiles.append(load_tex(f"destroy_stage_{i}.png") or
                     Image.new("RGBA", (TILE, TILE), (0, 0, 0, 0)))

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
    write_shape_table(ids, opaque, book_cover_tile, book_pages_tile, anvil_top_tile,
                      skull_side_tile, skull_top_tile, skull_bottom_tile,
                      item_tiles, destroy_stage_tile, n_tiles)

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

def write_shape_table(ids, opaque, book_cover_tile, book_pages_tile, anvil_top_tile,
                      skull_side_tile, skull_top_tile, skull_bottom_tile,
                      item_tiles, destroy_stage_tile, n_tiles):
    """Emit the per-global-id non-cube shape table (source/block_shapes_gen.h)
    that world.c's mesher and World_BlockBoxes() collision query both consult
    to dispatch away from the default full-cube path. See shape_for(). Also
    emits g_blockOpaque[] (the face-culling occluder flag, see the main loop)."""
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
        f.write("/* 1 iff this id is a full cube that fully occludes: every rendered\n")
        f.write(" * face texture passes the world alpha test, so an adjacent cube's\n")
        f.write(" * shared face may be culled against it. 0 for non-cube shapes and\n")
        f.write(" * for see-through cubes (glass, leaves) -- culling behind those\n")
        f.write(" * would show the deleted face through their gaps. See occ_opaque()\n")
        f.write(" * in world.c and the opacity test in this tool's main loop. */\n")
        f.write(f"static const u8 g_blockOpaque[{len(ids)}] = {{\n")
        f.write(",".join(str(v) for v in opaque))
        f.write("\n};\n\n")
        f.write("#endif\n")
    n_special = sum(1 for s in shapes if s != 0)
    n_transp = sum(1 for i, s in enumerate(shapes) if s == 0 and not opaque[i])
    print(f"  wrote {hpath} ({n_special} non-cube ids, "
          f"{n_transp} see-through cubes)")

    # Separate tiny header (not block_shapes_gen.h) so block_shapes.c -- which
    # needs this constant but not the (world.c-only) per-id shape tables above
    # -- doesn't pull in two unused 533-entry static arrays (-Wunused-variable
    # plus dead weight in its object file).
    bpath = os.path.join(src_dir, "block_book_gen.h")
    with open(bpath, "w", encoding="utf-8") as f:
        f.write("/* Generated by tools/build_atlas.py -- do not edit. */\n")
        f.write("#ifndef MSW_BLOCK_BOOK_GEN_H\n#define MSW_BLOCK_BOOK_GEN_H\n\n")
        f.write("/* Extra atlas tiles that block_shapes.c's mesh emitters need by\n")
        f.write(" * index (not part of the per-global-id side/top/bottom tables). */\n\n")
        f.write("/* Enchanting table's static floating book (mesh_enchant_table):\n")
        f.write(" * leather cover on its two flat faces, page edges on its sides. */\n")
        f.write(f"#define ENCHANT_BOOK_COVER_TILE {book_cover_tile}\n")
        f.write(f"#define ENCHANT_BOOK_PAGES_TILE {book_pages_tile}\n\n")
        f.write("/* Anvil working-surface texture for the top box's up-face only\n")
        f.write(" * (mesh_anvil); all other anvil faces use anvil_base. */\n")
        f.write(f"#define ANVIL_TOP_TILE {anvil_top_tile}\n\n")
        f.write("/* Default Steve head tiles for skulls (mesh_skull): front face\n")
        f.write(" * on the 4 sides, plus the head's top and bottom. */\n")
        f.write(f"#define SKULL_SIDE_TILE {skull_side_tile}\n")
        f.write(f"#define SKULL_TOP_TILE {skull_top_tile}\n")
        f.write(f"#define SKULL_BOTTOM_TILE {skull_bottom_tile}\n\n")
        f.write("/* Held/dropped ITEMS (not placeable blocks): the atlas tile\n")
        f.write(" * index is used directly as the inventory ItemStack.item id.\n")
        f.write(" * Being >= NUM_BLOCK_IDS is how helditem.c distinguishes a\n")
        f.write(" * flat item from a block. See source/items.h for the tools'\n")
        f.write(" * dig properties and block_props_gen.h for the drop tables. */\n")
        for name, _fname, _rgb in ITEM_TEXTURES:
            f.write(f"#define ITEM_{name}_TILE {item_tiles[name]}\n")
        f.write("\n/* Block-breaking crack overlay: DESTROY_STAGE_TILE + stage,\n")
        f.write(" * stage in 0..DESTROY_STAGE_COUNT-1 (vanilla destroy_stage_N). */\n")
        f.write(f"#define DESTROY_STAGE_TILE {destroy_stage_tile}\n")
        f.write(f"#define DESTROY_STAGE_COUNT {DESTROY_STAGES}\n\n")
        f.write("/* Total tiles packed into the atlas (block tiles + face\n")
        f.write(" * overrides + the extras above); an id/tile past this is a bug. */\n")
        f.write(f"#define NUM_ATLAS_TILES {n_tiles}\n\n")
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
