# --------------------------------------------------------------------------
# Generates patched copies of the Beetle VB core's VIP (video) sources and
# returns the vip.c path in VBGO_PATCHED_VIP_SOURCE. Two independent changes:
#
# 1. Shade tag (always on): every output pixel carries the Virtual Boy shade
#    index (0-3) it was drawn with, plus the current brightness of the
#    brightest shade - what Multicolor palettes need. Explained below.
# 2. Tile tracking hooks (experimental, off unless the frontend enables it):
#    calls into core/emu/vbgo_tiletrack.c from the ~6 places vip_draw.inc
#    puts a tile pixel on screen (and, for background tiles, where it leaves
#    one transparent - "fills"), plus where blocks are drawn, displayed or
#    overwritten by the CPU - so per-tile color packs know which tile, which
#    pixel of it and which BG map cell every screen pixel came from. See
#    vbgo_tiletrack.h.
#
# Why: per-shade color palettes (Red Viper-style colorization - see
# core/emu/ShadeColorizer.h) need to know *which* of the VB's 4 shades each
# pixel is, not just how bright it ended up. Brightness alone can't tell
# shades apart: games set the 3 shade brightnesses freely (BRTA/BRTB/BRTC)
# and ramp all of them during fades, so two different shades can land on the
# same output value. The core knows the index at exactly one point - its
# 4-entry BrightCLUT (shade index -> output color), rebuilt by
# RecalcBrightnessCache() on every brightness change - so that's where the
# tag goes. The brightest shade's level (BrightnessCache[3]) rides along so
# the colorizer can apply a game's fades as one uniform dim of the whole
# palette, rather than dimming each shade against a guess of what that
# particular game's "normal" brightness is.
#
# The tag lives in the top ("X") byte of the core's XRGB8888 output, which
# the libretro API defines as unused - bits 0-1 the shade index, bits 2-7
# BrightnessCache[3] >> 2 (0-63). The RGB the core outputs is byte-for-byte
# unchanged, so any code that ignores the tag sees the exact same frame as
# before. Emulator::RunFrame always overwrites that byte with opaque alpha
# before uploading anyway.
#
# Done as generated copies with exact-text replacements (rather than editing
# the submodule, or a .patch file applied with git) so the submodule stays
# pristine and nothing beyond CMake itself is needed at build time - Android
# Studio's CMake runs this exactly like the desktop build does. Line endings
# are normalized first, so a CRLF checkout (git autocrlf on Windows) matches
# too. Every anchor must match exactly once: if a future submodule bump
# changes one, configuration fails loudly here instead of a feature silently
# turning into a no-op.
# --------------------------------------------------------------------------
# Replaces the single occurrence of ANCHOR in the variable named VAR.
function(_vbgo_replace_once VAR ANCHOR REPLACEMENT WHERE)
    string(FIND "${${VAR}}" "${ANCHOR}" FIRST_HIT)
    string(FIND "${${VAR}}" "${ANCHOR}" LAST_HIT REVERSE)
    if(FIRST_HIT EQUAL -1 OR NOT FIRST_HIT EQUAL LAST_HIT)
        message(FATAL_ERROR
            "PatchBeetleVip: expected exactly one occurrence of\n${ANCHOR}\nin ${WHERE}. "
            "The beetle-vb-libretro submodule has probably changed - update this anchor in "
            "cmake/PatchBeetleVip.cmake to match.")
    endif()
    string(REPLACE "${ANCHOR}" "${REPLACEMENT}" RESULT "${${VAR}}")
    set(${VAR} "${RESULT}" PARENT_SCOPE)
endfunction()

# Writes TEXT to PATH via a temp file + configure_file(COPYONLY), so the
# generated source's timestamp only changes when its content does - otherwise
# every configure would force the (large) VIP translation unit to recompile.
function(_vbgo_write_if_changed PATH TEXT)
    file(WRITE "${PATH}.tmp" "${TEXT}")
    configure_file("${PATH}.tmp" "${PATH}" COPYONLY)
endfunction()

function(vbgo_generate_patched_vip VB_CORE_DIR OUT_DIR)
    set(VIP_SOURCE "${VB_CORE_DIR}/mednafen/vb/vip.c")
    set(DRAW_SOURCE "${VB_CORE_DIR}/mednafen/vb/vip_draw.inc")

    # Re-run configure (and so this function) whenever upstream's files change,
    # e.g. after a submodule bump.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VIP_SOURCE}" "${DRAW_SOURCE}")

    file(READ "${VIP_SOURCE}" VIP)
    file(READ "${DRAW_SOURCE}" DRAW)
    string(REPLACE "\r\n" "\n" VIP "${VIP}")
    string(REPLACE "\r\n" "\n" DRAW "${DRAW}")

    # ---- 1. Shade tag ------------------------------------------------------
    _vbgo_replace_once(VIP
        [=[BrightCLUT[lr][i] = ColorLUT[lr][BrightnessCache[i]];]=]
        [=[
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
      BrightCLUT[lr][i] = ColorLUT[lr][BrightnessCache[i]] | (uint32)i | ((uint32)(BrightnessCache[3] >> 2) << 2);
#else
      BrightCLUT[lr][i] = ColorLUT[lr][BrightnessCache[i]] | ((uint32)i << 24) | ((uint32)(BrightnessCache[3] >> 2) << 26);
#endif
]=]
        "vip.c (RecalcBrightnessCache's BrightCLUT assignment)")

    # ---- 2. Tile tracking hooks: vip.c ---------------------------------------
    _vbgo_replace_once(VIP
        [=[#include "vip.h"]=]
        [=[#include "vip.h"
#include "vbgo_tiletrack.h" /* VirtualBoyGo */]=]
        "vip.c (includes)")
    # The frame buffers, for the libretro core's state loading (see
    # vbgo_tiletrack_forget_buffer).
    _vbgo_replace_once(VIP
        [=[static uint8 FB[2][2][0x6000];]=]
        [=[static uint8 FB[2][2][0x6000];
const uint8_t *vbgo_vip_framebuffers(void) { return &FB[0][0][0]; } /* VirtualBoyGo */]=]
        "vip.c (the frame buffers)")
    # Displayed column -> output tags (side-by-side is the only 3D mode the
    # frontend uses; this function is what writes it).
    _vbgo_replace_once(VIP
        [=[   const int eye = dest_lr & 1;
   const uint8 *fb_source = &FB[fb][lr][64 * Column];
]=]
        [=[   const int eye = dest_lr & 1;
   const uint8 *fb_source = &FB[fb][lr][64 * Column];

   vbgo_tiletrack_display_column(fb, lr, dest_lr, Column, DisplayActive_arg); /* VirtualBoyGo */
]=]
        "vip.c (CopyFBColumnToTarget_SideBySide_BASE)")
    # Games drawing straight into the framebuffer with the CPU: no tile there.
    _vbgo_replace_once(VIP
        [=[            FB[(A >> 15) & 1][(A >> 16) & 1][A & 0x7FFF] = V;]=]
        [=[         {
            FB[(A >> 15) & 1][(A >> 16) & 1][A & 0x7FFF] = V;
            vbgo_tiletrack_cpu_fb_write((A >> 15) & 1, (A >> 16) & 1, A & 0x7FFF, 1); /* VirtualBoyGo */
         }]=]
        "vip.c (VIP_Write8 framebuffer store)")
    _vbgo_replace_once(VIP
        [=[            StoreU16_LE((uint16 *)&FB[(A >> 15) & 1][(A >> 16) & 1][A & 0x7FFF], V);]=]
        [=[         {
            StoreU16_LE((uint16 *)&FB[(A >> 15) & 1][(A >> 16) & 1][A & 0x7FFF], V);
            vbgo_tiletrack_cpu_fb_write((A >> 15) & 1, (A >> 16) & 1, A & 0x7FFF, 2); /* VirtualBoyGo */
         }]=]
        "vip.c (VIP_Write16 framebuffer store)")

    # ---- 2. Tile tracking hooks: vip_draw.inc ---------------------------------
    # Normal BG maps: remember which map cell (halfword index in DRAM) each
    # character comes from - the overplane character when outside the map.
    _vbgo_replace_once(DRAW
        [=[ uint32 BGMap_Base = bgmap_base_raw << 12;]=]
        [=[ uint32 BGMap_Base = bgmap_base_raw << 12;
 unsigned vbgo_cell = 0; /* VirtualBoyGo */
 uint32_t *const vbgo_row = vbgo_tt_row(target); /* VirtualBoyGo: this row's tags (NULL = not tracking) */
 uint16_t *const vbgo_crow = vbgo_tt_cell_row(target); /* VirtualBoyGo: ... and their cells */
 const uint32_t vbgo_bits = vbgo_tt_call_bits(); /* VirtualBoyGo */
 const uint32_t vbgo_wbit = 1u << VBGO_TAG_WORLD(vbgo_bits); /* VirtualBoyGo: cells this world tags (vbgo_tt_wcells) */]=]
        "vip_draw.inc (DrawBG locals)")
    _vbgo_replace_once(DRAW
        [=[  bgsc = bgsc_overplane;

  if(SourceX < SourceX_Size && SourceY < SourceY_Size)
   bgsc = BGMap[(BGMap_Base | ((SourceX << 3) & ~0xFFF) | ((SourceX >> 3) & 0x3F)) & 0xFFFF];]=]
        [=[  bgsc = bgsc_overplane;
  vbgo_cell = overplane_char & 0xFFFF; /* VirtualBoyGo */

  if(SourceX < SourceX_Size && SourceY < SourceY_Size)
  {
   vbgo_cell = (BGMap_Base | ((SourceX << 3) & ~0xFFF) | ((SourceX >> 3) & 0x3F)) & 0xFFFF; /* VirtualBoyGo */
   bgsc = BGMap[vbgo_cell];
  }]=]
        "vip_draw.inc (DrawBG map read)")
    # Blank-tile runs found in a character's first row, for its other rows in the same block
    # (same map row, same start and clip): per world and eye.
    _vbgo_replace_once(DRAW
        [=[static void DrawBG(uint8 *target, uint16 RealY,]=]
        [=[/* VirtualBoyGo: where runs of blank tiles end, by the 8-pixel column they start at */
typedef struct { uint32 serial, sx, base, mask; int32 x0, x1; uint32 known[2]; uint16 end[48]; } VbgoBlankRuns;
static VbgoBlankRuns vbgo_blank_runs[64];
static uint32 vbgo_runs_serial = 1;
static VbgoBlankRuns *vbgo_runs; /* (the call's; at file scope, so the drawing loop keeps its registers) */

static void DrawBG(uint8 *target, uint16 RealY,]=]
        "vip_draw.inc (DrawBG blank-run cache)")
    _vbgo_replace_once(DRAW
        [=[  USE_ROW(BGMap_Base | (x << 12));

 for(x = start_x; x <= final_x; x++)
 {]=]
        [=[  USE_ROW(BGMap_Base | (x << 12));

 vbgo_runs = NULL; /* VirtualBoyGo */
 if(vbgo_row && !overplane)
 {
  VbgoBlankRuns *const r = &vbgo_blank_runs[(vbgo_tt_world & 31) * 2 + (lr ? 1 : 0)];
  if(r->serial != vbgo_runs_serial || r->sx != SourceX || r->base != BGMap_Base || r->mask != SourceX_Mask ||
     r->x0 != start_x || r->x1 != final_x)
  {
   r->serial = vbgo_runs_serial, r->sx = SourceX, r->base = BGMap_Base, r->mask = SourceX_Mask;
   r->x0 = start_x, r->x1 = final_x;
   r->known[0] = r->known[1] = 0;
  }
  vbgo_runs = r;
 }

 for(x = start_x; x <= final_x; x++)
 {]=]
        "vip_draw.inc (DrawBG blank-run lookup)")
    _vbgo_replace_once(DRAW
        [=[ obj_search_which = 3;
]=]
        [=[ obj_search_which = 3;
 vbgo_runs_serial++; /* VirtualBoyGo: (the block's memory may differ from the last one's) */
]=]
        "vip_draw.inc (VIP_DrawBlock serial)")
    # The unrolled 8-pixel path (a whole character row at once) tags its 8
    # pixels afterwards - pixel k shows in-tile x k, or 7 - k when the
    # character is flipped horizontally; transparent ones may become fills.
    # Background drawing only draws pixels 0-383 of the call's row, so its
    # tags go straight to the row's tags (vbgo_row) - no per-pixel lookup.
    # Runs of blank character rows (no pixels, no fills) draw and tag nothing:
    # step over them without the rest of the loop. Same marks, same everything.
    _vbgo_replace_once(DRAW
        [=[    USE_BG_CHR(char_no);
    used_chr = char_no;
   }
   if(pixels)]=]
        [=[    USE_BG_CHR(char_no);
    used_chr = char_no;
   }
   while(!pixels && (x + 15) <= final_x && !(vbgo_row && vbgo_tt_fills_from(char_no, vbgo_cell))) /* VirtualBoyGo */
   {
    /* (a map entry that can't fill anywhere: the same entry again is the same blank row) */
    const uint32 blank_bgsc = (!vbgo_row || !vbgo_tt_fill || vbgo_tt_blank[char_no]) ? bgsc : 0x10000;
    /* (a blank tile is blank in every row: where its run ends is known from the character's first row) */
    const int vbgo_run_known = vbgo_runs && vbgo_tt_blank[char_no];
    if(vbgo_run_known && (vbgo_runs->known[x >> 8] >> ((x >> 3) & 31) & 1))
    {
     const int32 to = vbgo_runs->end[x >> 3];
     SourceX = (SourceX + (uint32)(to - x)) & SourceX_Mask;
     x = to;
     vbgo_cell = (BGMap_Base | ((SourceX << 3) & ~0xFFF) | ((SourceX >> 3) & 0x3F)) & 0xFFFF;
     bgsc = BGMap[vbgo_cell];
    }
    else if(!overplane)
    {
     const int32 from = x;
     /* (along a map's row of 64 cells the next cell is the next halfword) */
     unsigned c = (SourceX >> 3) & 0x3F;
     uint32 row_start = (BGMap_Base | ((SourceX << 3) & ~0xFFF)) & 0xFFFF;
     do
     {
      x += 8;
      SourceX = (SourceX + 8) & SourceX_Mask;
      if(++c == 64)
       c = 0, row_start = (BGMap_Base | ((SourceX << 3) & ~0xFFF)) & 0xFFFF;
      bgsc = BGMap[row_start | c];
     } while(bgsc == blank_bgsc && (x + 15) <= final_x);
     vbgo_cell = row_start | c;
     if(vbgo_run_known)
      vbgo_runs->end[from >> 3] = (uint16)x, vbgo_runs->known[from >> 8] |= 1u << ((from >> 3) & 31);
    }
    else
     do
     {
      x += 8;
      SourceX = (SourceX + 8) & SourceX_Mask;
      bgsc = bgsc_overplane;
      vbgo_cell = overplane_char & 0xFFFF;
      if(SourceX < SourceX_Size && SourceY < SourceY_Size)
      {
       vbgo_cell = (BGMap_Base | ((SourceX << 3) & ~0xFFF) | ((SourceX >> 3) & 0x3F)) & 0xFFFF;
       bgsc = BGMap[vbgo_cell];
      }
     } while(bgsc == blank_bgsc && (x + 15) <= final_x);
    char_no = bgsc & 0x7FF;
    palette_selector = bgsc >> 14;
    char_sub_y = ((bgsc & 0x1000) ? 7 : 0) ^ (SourceY & 0x7);
    pixels = CHR16[char_no * 8 + char_sub_y];
    if(char_no != used_chr)
    {
     USE_BG_CHR(char_no);
     used_chr = char_no;
    }
   }
   if(pixels)]=]
        "vip_draw.inc (DrawBG blank character rows)")
    _vbgo_replace_once(DRAW
        [=[   x += 7;
   SourceX += 8;]=]
        [=[   if(vbgo_row) /* VirtualBoyGo */
   {
    const int tagged = vbgo_tt_tag_row8(vbgo_row + x * VBGO_TT_COLUMN, vbgo_crow + x * VBGO_TT_CELL_COLUMN, pixels,
                                        bgsc & 0x2000, vbgo_bits | VBGO_TT_BG_BITS(char_no, 0, char_sub_y, palette_selector, 0),
                                        char_no, vbgo_cell);
    if(tagged)
     vbgo_tt_wcells[x >> 3] |= vbgo_wbit, vbgo_tt_wcells[(x + 7) >> 3] |= vbgo_wbit;
    if(tagged == 2)
     vbgo_tt_fillc[x >> 3] = vbgo_tt_fillc[(x + 7) >> 3] = 1;
   }

   x += 7;
   SourceX += 8;]=]
        "vip_draw.inc (DrawBG 8-pixel path)")
    _vbgo_replace_once(DRAW
        [=[   if(pixel)
    target[x] = GPLT_Cache[palette_selector][pixel];
   SourceX++;]=]
        [=[   if(pixel)
   {
    target[x] = GPLT_Cache[palette_selector][pixel];
    if(vbgo_row) /* VirtualBoyGo */
     vbgo_row[x * VBGO_TT_COLUMN] = vbgo_bits | VBGO_TT_BG_BITS(char_no, char_sub_x, char_sub_y, palette_selector, pixel),
     vbgo_crow[x * VBGO_TT_CELL_COLUMN] = (uint16_t)vbgo_cell, vbgo_tt_wcells[x >> 3] |= vbgo_wbit;
   }
   else if(vbgo_row && vbgo_tt_fills_from(char_no, vbgo_cell)) /* VirtualBoyGo */
    vbgo_tt_fill_at(&vbgo_row[x * VBGO_TT_COLUMN], &vbgo_crow[x * VBGO_TT_CELL_COLUMN],
                    vbgo_bits | VBGO_TT_BG_BITS(char_no, char_sub_x, char_sub_y, palette_selector, 0), vbgo_cell),
    vbgo_tt_wcells[x >> 3] |= vbgo_wbit, vbgo_tt_fillc[x >> 3] = 1;
   SourceX++;]=]
        "vip_draw.inc (DrawBG per-pixel store)")
    # Affine (scaled/rotated) maps - no-rotation path, where char_sub_x is a
    # bit offset (2 bits per pixel), and the general path.
    _vbgo_replace_once(DRAW
        [=[ const uint16 *param_ptr = &DRAM[(ParamBase + 8 * (RealY - DestY)) & 0xFFFF];]=]
        [=[ const uint16 *param_ptr = &DRAM[(ParamBase + 8 * (RealY - DestY)) & 0xFFFF];
 unsigned vbgo_cell = 0; /* VirtualBoyGo */
 int vbgo_fills = 0; /* VirtualBoyGo: whether the current cell may leave fills */
 uint32_t *const vbgo_row = vbgo_tt_row(target); /* VirtualBoyGo: this row's tags (NULL = not tracking) */
 uint16_t *const vbgo_crow = vbgo_tt_cell_row(target); /* VirtualBoyGo: ... and their cells */
 const uint32_t vbgo_bits = vbgo_tt_call_bits(); /* VirtualBoyGo */
 const uint32_t vbgo_wbit = 1u << VBGO_TAG_WORLD(vbgo_bits); /* VirtualBoyGo: cells this world tags (vbgo_tt_wcells) */
 uint32_t vbgo_cbits = 0; /* VirtualBoyGo: the current cell's part of its pixels' tags */
 int vbgo_wgroup = -1; /* VirtualBoyGo: the last 8 columns marked in vbgo_tt_wcells */]=]
        "vip_draw.inc (DrawAffine locals)")
    _vbgo_replace_once(DRAW
        [=[   bgsc = bgsc_overplane;

   if(SourceX < (SourceX_Size << 9))
    bgsc = BGMap[(BGMap_Base | ((SourceX >> 6) & ~0xFFF) | ((SourceX >> 12) & 0x3F)) & 0xFFFF];]=]
        [=[   bgsc = bgsc_overplane;
   vbgo_cell = OverplaneChar & 0xFFFF; /* VirtualBoyGo */

   if(SourceX < (SourceX_Size << 9))
   {
    vbgo_cell = (BGMap_Base | ((SourceX >> 6) & ~0xFFF) | ((SourceX >> 12) & 0x3F)) & 0xFFFF; /* VirtualBoyGo */
    bgsc = BGMap[vbgo_cell];
    USE_ROW(vbgo_cell);
   }
   vbgo_fills = vbgo_row && vbgo_tt_fills_from(bgsc & 0x7FF, vbgo_cell); /* VirtualBoyGo */]=]
        "vip_draw.inc (DrawAffine no-rotation map read)")
    _vbgo_replace_once(DRAW
        [=[   char_row = CHR16[((bgsc & 0x7FF) * 8) | char_sub_y];]=]
        [=[   char_row = CHR16[((bgsc & 0x7FF) * 8) | char_sub_y];
   vbgo_cbits = vbgo_bits | VBGO_TT_BG_BITS(bgsc & 0x7FF, 0, char_sub_y, bgsc >> 14, 0); /* VirtualBoyGo */]=]
        "vip_draw.inc (DrawAffine no-rotation cell tag)")
    _vbgo_replace_once(DRAW
        [=[  if(pixel)
   target[x] = plt[pixel];

  SourceX += dx;
 }]=]
        [=[  if(pixel)
  {
   target[x] = plt[pixel];
   if(vbgo_row) /* VirtualBoyGo: char_sub_x is even, so << 10 puts its half in VBGO_TT_BG_BITS's column field */
   {
    vbgo_row[x * VBGO_TT_COLUMN] = vbgo_cbits | char_sub_x << 10 | pixel << 20;
    vbgo_crow[x * VBGO_TT_CELL_COLUMN] = (uint16_t)vbgo_cell;
    if((x >> 3) != vbgo_wgroup)
     vbgo_tt_wcells[vbgo_wgroup = x >> 3] |= vbgo_wbit;
   }
  }
  else if(vbgo_fills) /* VirtualBoyGo */
   vbgo_tt_fill_at(&vbgo_row[x * VBGO_TT_COLUMN], &vbgo_crow[x * VBGO_TT_CELL_COLUMN], vbgo_cbits | char_sub_x << 10, vbgo_cell),
   vbgo_tt_wcells[x >> 3] |= vbgo_wbit, vbgo_tt_fillc[x >> 3] = 1;

  SourceX += dx;
 }]=]
        "vip_draw.inc (DrawAffine no-rotation store)")
    # The unrotated line one pixel a step (an unscaled affine map): a map cell's pixels are one run of its character
    # row, drawn and tagged together. A whole opaque cell is straight-line code; the rest is in VbgoAffineRun.
    _vbgo_replace_once(DRAW
        [=[static void DrawAffine(uint8 *target, uint16 RealY, bool lr, uint32 ParamBase,]=]
        [=[#if defined(__GNUC__)
#define VBGO_NOINLINE __attribute__((noinline))
#else
#define VBGO_NOINLINE
#endif
/* VirtualBoyGo: an unrotated affine run (DrawAffine) that isn't a whole opaque cell's: fills, the line's ends,
 * transparent pixels. Pixel j is at x + j and shows in-tile x offset + j, or 7 - offset - j flipped. */
static VBGO_NOINLINE void VbgoAffineRun(uint8 *target, int x, unsigned int offset, unsigned int run, uint32 p, uint32 bgsc, uint32 cbits,
                          unsigned int cell, int fills, uint32_t *row, uint16_t *crow, uint32 wbit)
{
 unsigned int j;
 if(fills)
 {
  for(j = 0; j < run; j++)
  {
   const unsigned int char_sub_x = ((bgsc & 0x2000) ? 0xE : 0) ^ ((offset + j) * 2);
   const uint32 pixel = (p >> (j * 2)) & 3;
   if(pixel)
   {
    target[x + j] = GPLT_Cache[bgsc >> 14][pixel];
    row[(x + j) * VBGO_TT_COLUMN] = cbits | char_sub_x << 10 | pixel << 20;
    crow[(x + j) * VBGO_TT_CELL_COLUMN] = (uint16_t)cell;
    vbgo_tt_wcells[(x + j) >> 3] |= wbit;
   }
   else
    vbgo_tt_fill_at(&row[(x + j) * VBGO_TT_COLUMN], &crow[(x + j) * VBGO_TT_CELL_COLUMN], cbits | char_sub_x << 10, cell),
    vbgo_tt_wcells[(x + j) >> 3] |= wbit, vbgo_tt_fillc[(x + j) >> 3] = 1;
  }
 }
 else if(p)
 {
  /* All the run's shades at once, merged into the row under the drawn pixels (as DrawBG's whole-row path does). */
  uint32 shades[2], drawn[2], line[2];
  memcpy(&shades[0], GPLT_Quad[bgsc >> 14][p & 0xFF], 4);
  memcpy(&shades[1], GPLT_Quad[bgsc >> 14][p >> 8], 4);
  memcpy(&drawn[0], OpaqueQuad[p & 0xFF], 4);
  memcpy(&drawn[1], OpaqueQuad[p >> 8], 4);
  memcpy(&line[0], target + x, 4);
  memcpy(&line[1], target + x + 4, 4);
  line[0] = (line[0] & ~drawn[0]) | (shades[0] & drawn[0]);
  line[1] = (line[1] & ~drawn[1]) | (shades[1] & drawn[1]);
  memcpy(target + x, &line[0], 4);
  memcpy(target + x + 4, &line[1], 4);
  if(row)
  {
   const int flipped = (bgsc & 0x2000) != 0;
   unsigned int in_tile = flipped ? 7 - offset : offset;
   const uint32 drawn_mask = (p | (p >> 1)) & 0x5555; /* (bit 2j: pixel j is drawn) */
   unsigned int first, last;
   for(j = 0; j < run; j++, in_tile += flipped ? -1 : 1)
    if((drawn_mask >> (j * 2)) & 1)
     row[(x + j) * VBGO_TT_COLUMN] = cbits | in_tile << 11 | ((p >> (j * 2)) & 3) << 20, crow[(x + j) * VBGO_TT_CELL_COLUMN] = (uint16_t)cell;
   /* (the 8 columns holding the first and the last drawn pixel) */
   first = __builtin_ctz(drawn_mask) >> 1;
   last = (31 - __builtin_clz(drawn_mask)) >> 1;
   vbgo_tt_wcells[(x + first) >> 3] |= wbit;
   vbgo_tt_wcells[(x + last) >> 3] |= wbit;
  }
 }
}

static void DrawAffine(uint8 *target, uint16 RealY, bool lr, uint32 ParamBase,]=]
        "vip_draw.inc (DrawAffine run helper)")
    _vbgo_replace_once(DRAW
        [=[ const uint8 *plt = GPLT_Cache[0];
 unsigned int char_sub_y = 0;
 for(x = start_x; x <= final_x; x++)
 {]=]
        [=[ const uint8 *plt = GPLT_Cache[0];
 unsigned int char_sub_y = 0;
 if(dx == 512) /* VirtualBoyGo */
 {
  /* (a map's edge is on a cell's, so SourceX wraps only between runs) */
  x = start_x;
  while(x <= final_x)
  {
   uint32 p;
   unsigned int offset, run;

   SourceX &= SourceX_Mask;
   bgsc = bgsc_overplane;
   vbgo_cell = OverplaneChar & 0xFFFF;
   if(SourceX < (SourceX_Size << 9))
   {
    vbgo_cell = (BGMap_Base | ((SourceX >> 6) & ~0xFFF) | ((SourceX >> 12) & 0x3F)) & 0xFFFF;
    bgsc = BGMap[vbgo_cell];
    USE_ROW(vbgo_cell);
   }
   vbgo_fills = vbgo_row && vbgo_tt_fills_from(bgsc & 0x7FF, vbgo_cell);
   char_sub_y = (((int32)(bgsc << 19) >> 31) & 0x7) ^ ((SourceY >> 9) & 0x7);
   p = CHR16[((bgsc & 0x7FF) * 8) | char_sub_y];
   vbgo_cbits = vbgo_bits | VBGO_TT_BG_BITS(bgsc & 0x7FF, 0, char_sub_y, bgsc >> 14, 0);
   if((bgsc & 0x7FF) != used_chr)
   {
    USE_BG_CHR(bgsc & 0x7FF);
    used_chr = bgsc & 0x7FF;
   }
   /* (the run: this cell's pixels from where the line enters it, in screen order, two bits each) */
   if(bgsc & 0x2000)
   {
    p = ((p >> 2) & 0x3333) | ((p & 0x3333) << 2);
    p = ((p >> 4) & 0x0F0F) | ((p & 0x0F0F) << 4);
    p = ((p >> 8) & 0x00FF) | ((p & 0x00FF) << 8);
   }
   offset = (SourceX >> 9) & 7;
   run = 8 - offset;
   if(run > (unsigned)(final_x - x + 1))
    run = final_x - x + 1;
   p = (p >> (offset * 2)) & ((1u << (run * 2)) - 1);
   if(vbgo_row && !vbgo_fills && run == 8 && ((p | (p >> 1)) & 0x5555) == 0x5555)
   {
    /* (a whole cell, every pixel drawn: straight-line; pixel j shows in-tile x j, or 7 - j flipped) */
    uint32 shades[2];
    uint32_t *const tag = vbgo_row + x * VBGO_TT_COLUMN;
    const uint32 b = vbgo_cbits | ((bgsc & 0x2000) ? 7u << 11 : 0u);
    const int step = (bgsc & 0x2000) ? -(1 << 11) : (1 << 11);
    const uint32 cells = (uint32)vbgo_cell * 0x10001u;
    memcpy(&shades[0], GPLT_Quad[bgsc >> 14][p & 0xFF], 4);
    memcpy(&shades[1], GPLT_Quad[bgsc >> 14][p >> 8], 4);
    memcpy(target + x, &shades[0], 4);
    memcpy(target + x + 4, &shades[1], 4);
#define VBGO_AFFINE_TAG(j) tag[(j) * VBGO_TT_COLUMN] = (b + (uint32)((j) * step)) | ((p >> ((j) * 2)) & 3) << 20;
    VBGO_AFFINE_TAG(0) VBGO_AFFINE_TAG(1) VBGO_AFFINE_TAG(2) VBGO_AFFINE_TAG(3)
    VBGO_AFFINE_TAG(4) VBGO_AFFINE_TAG(5) VBGO_AFFINE_TAG(6) VBGO_AFFINE_TAG(7)
#undef VBGO_AFFINE_TAG
    memcpy(vbgo_crow + x * VBGO_TT_CELL_COLUMN, &cells, 4);
    memcpy(vbgo_crow + x * VBGO_TT_CELL_COLUMN + 2, &cells, 4);
    memcpy(vbgo_crow + x * VBGO_TT_CELL_COLUMN + 4, &cells, 4);
    memcpy(vbgo_crow + x * VBGO_TT_CELL_COLUMN + 6, &cells, 4);
    vbgo_tt_wcells[x >> 3] |= vbgo_wbit;
    vbgo_tt_wcells[(x + 7) >> 3] |= vbgo_wbit;
   }
   else if(vbgo_fills || p)
    VbgoAffineRun(target, x, offset, run, p, bgsc, vbgo_cbits, vbgo_cell, vbgo_fills, vbgo_row, vbgo_crow, vbgo_wbit);
   x += run;
   SourceX += run << 9;
  }
 }
 else
 for(x = start_x; x <= final_x; x++)
 {]=]
        "vip_draw.inc (DrawAffine no-rotation run)")
    _vbgo_replace_once(DRAW
        [=[    bgsc = BGMap[(BGMap_Base | m_index | sub_index) & 0xFFFF];
   }]=]
        [=[    vbgo_cell = (BGMap_Base | m_index | sub_index) & 0xFFFF; /* VirtualBoyGo */
    bgsc = BGMap[vbgo_cell];
    USE_ROW(vbgo_cell);
   }
   else
    vbgo_cell = OverplaneChar & 0xFFFF; /* VirtualBoyGo */
   vbgo_fills = vbgo_row && vbgo_tt_fills_from(bgsc & 0x7FF, vbgo_cell); /* VirtualBoyGo */]=]
        "vip_draw.inc (DrawAffine general map read)")
    _vbgo_replace_once(DRAW
        [=[  if(pixel)
   target[x] = GPLT_Cache[palette_selector][pixel];

  SourceX += dx;
  SourceY += dy;]=]
        [=[  if(pixel)
  {
   target[x] = GPLT_Cache[palette_selector][pixel];
   if(vbgo_row) /* VirtualBoyGo */
    vbgo_row[x * VBGO_TT_COLUMN] = vbgo_bits | VBGO_TT_BG_BITS(char_no, char_sub_x, char_sub_y, palette_selector, pixel),
    vbgo_crow[x * VBGO_TT_CELL_COLUMN] = (uint16_t)vbgo_cell, vbgo_tt_wcells[x >> 3] |= vbgo_wbit;
  }
  else if(vbgo_fills) /* VirtualBoyGo */
   vbgo_tt_fill_at(&vbgo_row[x * VBGO_TT_COLUMN], &vbgo_crow[x * VBGO_TT_CELL_COLUMN],
                   vbgo_bits | VBGO_TT_BG_BITS(char_no, char_sub_x, char_sub_y, palette_selector, 0), vbgo_cell),
   vbgo_tt_wcells[x >> 3] |= vbgo_wbit, vbgo_tt_fillc[x >> 3] = 1;

  SourceX += dx;
  SourceY += dy;]=]
        "vip_draw.inc (DrawAffine general store)")
    # Sprites: pixel i of the char row is stored on loop iteration i (8 - meow),
    # whichever direction the target walks for horizontal flip. No map cell -
    # the OBJ number instead (both eyes draw the same OBJ, so it ties a
    # sprite's pixels in one eye to the other's); no fills (a sprite's
    # transparent pixels aren't part of it).
    _vbgo_replace_once(DRAW
        [=[      if(pixels & 3)
       *target = JPLT_Cache[palette_selector][pixels & 3];
      target--;]=]
        [=[      if(pixels & 3)
      {
       *target = JPLT_Cache[palette_selector][pixels & 3];
       if(vbgo_orow && (unsigned)(target - fb[lr] - ClipX0) <= (unsigned)(ClipX1 - ClipX0)) /* VirtualBoyGo */
        vbgo_orow[(target - fb[lr]) * VBGO_TT_COLUMN] = vbgo_obits | ((8 - meow) << 11) | ((pixels & 3) << 20),
        vbgo_ocrow[(target - fb[lr]) * VBGO_TT_CELL_COLUMN] = vbgo_ocell;
      }
      target--;]=]
        "vip_draw.inc (DrawOBJ flipped store)")
    _vbgo_replace_once(DRAW
        [=[      if(pixels & 3)
       *target = JPLT_Cache[palette_selector][pixels & 3];
      target++;]=]
        [=[      if(pixels & 3)
      {
       *target = JPLT_Cache[palette_selector][pixels & 3];
       if(vbgo_orow && (unsigned)(target - fb[lr] - ClipX0) <= (unsigned)(ClipX1 - ClipX0)) /* VirtualBoyGo */
        vbgo_orow[(target - fb[lr]) * VBGO_TT_COLUMN] = vbgo_obits | ((8 - meow) << 11) | ((pixels & 3) << 20),
        vbgo_ocrow[(target - fb[lr]) * VBGO_TT_CELL_COLUMN] = vbgo_ocell;
      }
      target++;]=]
        "vip_draw.inc (DrawOBJ store)")
    # Which world (layer) is drawing.
    _vbgo_replace_once(DRAW
        [=[    uint8 *target = &fb[lr][x];
]=]
        [=[    uint8 *target = &fb[lr][x];
    /* VirtualBoyGo: this sprite row's tags - all but the pixel's place in the tile and its value */
    uint32_t *const vbgo_orow = vbgo_tt_row(fb[lr]);
    uint16_t *const vbgo_ocrow = vbgo_tt_cell_row(fb[lr]);
    const uint32_t vbgo_obits = vbgo_tt_call_bits() | char_no | (char_sub_y << 14) | (palette_selector << 17) | (1u << 19);
    const uint16_t vbgo_ocell = VBGO_TT_OBJ_CELL(oam);
    if(vbgo_orow) /* VirtualBoyGo: the cells this sprite row may tag */
     vbgo_tt_ocells[x > 0 ? x >> 3 : 0] = vbgo_tt_ocells[x + 7 < 384 ? (x + 7) >> 3 : 47] = 1;
]=]
        "vip_draw.inc (DrawOBJ sprite row)")

    # vip.c #includes "vip_draw.inc"; a quoted include resolves next to the
    # including file first, so the generated vip.c picks up the generated
    # vip_draw.inc beside it.
    _vbgo_write_if_changed("${OUT_DIR}/vip.c" "${VIP}")
    _vbgo_write_if_changed("${OUT_DIR}/vip_draw.inc" "${DRAW}")

    set(VBGO_PATCHED_VIP_SOURCE "${OUT_DIR}/vip.c" PARENT_SCOPE)
endfunction()
