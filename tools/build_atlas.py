#!/usr/bin/env python3
"""
build_atlas.py -- assemble a single block-texture atlas from the RKYfault
resource pack, one 16x16 tile per global block id (tile index == line number in
_blockids.txt). The GameCube client computes a tile's UV straight from the
block's global id, so no separate UV table is shipped.

Layout: ATLAS_COLS tiles per row, TILE px each, padded to a power-of-two square.
Output: <out>/atlas.png  (fed to gxtexconv -> atlas.tpl by the build)

Textures the pack does not provide per-variant (stained clay/glass, coloured
wool where a file is missing) are synthesised by tinting a neutral base with the
standard Minecraft dye colour, so every id still resolves to a real tile.
"""
import os, sys, hashlib
from PIL import Image

TILE = 16
ATLAS_COLS = 32
ATLAS_W = 512
ATLAS_H = 512

PACK = r"C:\Users\awt12\AppData\Roaming\.minecraft\resourcepacks\!                  §bRKYfault§3[16x]"
BLOCKS = os.path.join(PACK, "assets", "minecraft", "textures", "blocks")

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
    """load a blocks/*.png, crop to the top 16x16 frame (animations), RGBA."""
    if fname in _cache:
        return _cache[fname]
    path = os.path.join(BLOCKS, fname)
    if not os.path.exists(path):
        _cache[fname] = None
        return None
    im = Image.open(path).convert("RGBA")
    if im.width != im.height:                       # animation strip -> frame 0
        im = im.crop((0, 0, im.width, im.width))
    if im.size != (TILE, TILE):
        im = im.resize((TILE, TILE), Image.NEAREST)
    _cache[fname] = im
    return im

def solid(rgb):
    return Image.new("RGBA", (TILE, TILE), (rgb[0], rgb[1], rgb[2], 255))

def tint(base_img, rgb):
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

def hash_color(name):
    h = hashlib.md5(name.encode()).digest()
    return (90 + h[0] % 130, 90 + h[1] % 130, 90 + h[2] % 130)

def split(bid):
    if ":" in bid:
        b, d = bid.split(":", 1)
        try: return b, int(d)
        except ValueError: return b, 0
    return bid, 0

# direct base-name -> single texture file
DIRECT = {
    "STONE":"stone.png", "COBBLESTONE":"cobblestone.png",
    "MOSSY_COBBLESTONE":"cobblestone_mossy.png", "COBBLE_WALL":"cobblestone.png",
    "BEDROCK":"bedrock.png", "GRAVEL":"gravel.png", "SAND":"sand.png",
    "SOIL":"dirt.png", "GRASS":"grass_top.png", "MYCEL":"mycelium_top.png",
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
    "SPONGE":"sponge.png", "WORKBENCH":"planks_oak.png",
    "FURNACE":"furnace_front_off.png", "CHEST":"planks_oak.png",
    "BOOKSHELF":"bookshelf.png", "JUKEBOX":"planks_oak.png",
    "NOTE_BLOCK":"planks_oak.png", "ENCHANTMENT_TABLE":"enchanting_table_top.png",
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
    "NETHER_WARTS":"soul_sand.png", "BED_BLOCK":"bed_feet_top.png",
    "STONE_PLATE":"stone.png", "WOOD_PLATE":"planks_oak.png",
    "STONE_BUTTON":"stone.png", "WOOD_BUTTON":"planks_oak.png",
    "LEVER":"cobblestone.png", "TRIPWIRE_HOOK":"planks_oak.png",
    "REDSTONE_WIRE":"redstone_block.png", "RAILS":"rail_normal.png",
    "POWERED_RAIL":"rail_golden.png", "SIGN_POST":"planks_oak.png",
    "WALL_SIGN":"planks_oak.png", "WOODEN_DOOR":"door_wood_upper.png",
    "JUNGLE_DOOR":"door_jungle_upper.png", "DOUBLE_PLANT":"double_plant_grass_top.png",
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
    """returns a 16x16 RGBA Image for a full block id like 'WOOL:14'."""
    base, data = split(bid)

    if base in ("WOOL", "CARPET"):
        f = "wool_colored_%s.png" % WOOL_NAME.get(data, "white")
        im = load_tex(f)
        return im if im else solid(DYE.get(data, (200,200,200)))
    if base == "HARD_CLAY":
        return tint(load_tex("hardened_clay.png") or load_tex("clay.png"),
                    (152, 94, 68))
    if base == "STAINED_CLAY":
        return tint(load_tex("hardened_clay.png") or load_tex("clay.png"),
                    DYE.get(data, (150,100,80)))
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
        return load_tex("leaves_%s.png" % LEAF_SPECIES.get(data & 3, "oak")) \
               or solid((60,120,40))
    if base == "LEAVES_2":
        sp = "acacia" if (data & 1) == 0 else "big_oak"
        return load_tex("leaves_%s.png" % sp) or solid((60,120,40))
    if base in ("STEP", "DOUBLE_STEP"):
        slab = {0:"stone.png",1:"sandstone_normal.png",2:"planks_oak.png",
                3:"cobblestone.png",4:"brick.png",5:"stonebrick.png",
                6:"quartz_block_side.png",7:"nether_brick.png"}
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
        return load_tex("red_sandstone_normal.png") or solid((180,90,50))
    if base == "QUARTZ_BLOCK":
        q = {0:"quartz_block_side.png",1:"quartz_block_chiseled.png",
             2:"quartz_block_lines.png",3:"quartz_block_lines.png",
             4:"quartz_block_lines.png"}
        return load_tex(q.get(data, "quartz_block_side.png")) or solid((235,233,227))
    if base == "PRISMARINE":
        pm = {0:"prismarine_rough.png",1:"prismarine_bricks.png",2:"prismarine_dark.png"}
        return load_tex(pm.get(data, "prismarine_rough.png")) or solid((99,171,158))
    if base == "DIRT":
        dd = {0:"dirt.png",1:"coarse_dirt.png",2:"dirt_podzol_top.png"}
        return load_tex(dd.get(data, "dirt.png")) or solid((134,96,67))
    if base == "STONE":
        st = {0:"stone.png",1:"stone_granite.png",2:"stone_granite_smooth.png",
              3:"stone_diorite.png",4:"stone_diorite_smooth.png",
              5:"stone_andesite.png",6:"stone_andesite_smooth.png"}
        return load_tex(st.get(data, "stone.png")) or solid((130,130,130))
    if base == "MONSTER_EGGS":
        return load_tex("stonebrick.png") or solid((128,128,128))

    if base in DIRECT:
        im = load_tex(DIRECT[base])
        if im:
            return im

    # last resort: try a lowercased direct filename, else hashed colour
    guess = load_tex(base.lower() + ".png")
    if guess:
        return guess
    return solid(hash_color(bid))

def main():
    ids_path = sys.argv[1] if len(sys.argv) > 1 else \
        r"C:\Users\awt12\Downloads\download (1)\BlockScans\_blockids.txt"
    out_dir = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.join(os.path.dirname(__file__), "..", "data")
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    ids = [l.strip() for l in open(ids_path, encoding="utf-8") if l.strip()]
    atlas = Image.new("RGBA", (ATLAS_W, ATLAS_H), (0, 0, 0, 0))
    synth = 0
    for i, bid in enumerate(ids):
        tile = texture_for(bid)
        base, _ = split(bid)
        col = i % ATLAS_COLS
        row = i // ATLAS_COLS
        atlas.paste(tile, (col * TILE, row * TILE))
    outp = os.path.join(out_dir, "atlas.png")
    atlas.save(outp)
    print(f"atlas: {len(ids)} tiles -> {outp}  ({ATLAS_W}x{ATLAS_H})")
    print(f"  cols={ATLAS_COLS} tile={TILE}; tile index == global block id")

if __name__ == "__main__":
    main()
