#include "vb.h"

#define BGM_AFFINE	0x2
#define BGM_OBJ		0x3

/* What the block being drawn reads (see BlockInputs in vip.c). */
#define USE_CHR(c) (UsedChr[(c) >> 5] |= 1u << ((c) & 31))
#define USE_BG_CHR(c) (USE_CHR(c), UsedBgChr[(c) >> 5] |= 1u << ((c) & 31))
#define USE_ROW(h) (UsedRows[((h) & 0xFFFF) >> 11] |= 1u << ((((h) & 0xFFFF) >> 6) & 31))



/* A map cell with nothing to draw (every pixel transparent, no fills) is stepped over to where the line leaves it: most
 * of a rotated map's pixels are in such cells. Steps under 1024 only (a cell crossed in a pixel or two isn't worth it). */
#define BLANK_RUN_LEN(k) \
 { \
  const uint32 blank_px = SourceX & 0xFFF, blank_py = SourceY & 0xFFF; \
  uint32 blank_kk = (uint32)(final_x - x), q; \
  if(dx > 0) { q = (uint32)(((uint64)(4095 - blank_px) * blank_rdx) >> 32); if(q < blank_kk) blank_kk = q; } \
  else if(dx < 0) { q = (uint32)(((uint64)blank_px * blank_rdx) >> 32); if(q < blank_kk) blank_kk = q; } \
  if(dy > 0) { q = (uint32)(((uint64)(4095 - blank_py) * blank_rdy) >> 32); if(q < blank_kk) blank_kk = q; } \
  else if(dy < 0) { q = (uint32)(((uint64)blank_py * blank_rdy) >> 32); if(q < blank_kk) blank_kk = q; } \
  (k) = (int32)blank_kk; \
 }
#define SKIP_BLANK_ROTATED \
   if(!vbgo_fills && (dx > -1024 && dx < 1024 && dy > -1024 && dy < 1024)) \
   { \
    uint32 blank_w[4]; \
    memcpy(blank_w, &CHR16[char_no * 8], 16); \
    if(!(blank_w[0] | blank_w[1] | blank_w[2] | blank_w[3])) \
    { \
     int32 blank_k; \
     BLANK_RUN_LEN(blank_k) \
     x += blank_k; \
     SourceX += (uint32)(dx * (blank_k + 1)); \
     SourceY += (uint32)(dy * (blank_k + 1)); \
     continue; \
    } \
   }

#if defined(__GNUC__)
#define VBGO_NOINLINE __attribute__((noinline))
#else
#define VBGO_NOINLINE
#endif
static VBGO_NOINLINE void VbgoAffineRun(uint8 *target, int x, unsigned int offset, unsigned int run, uint32 p, uint32 bgsc, uint32 cbits,
                          unsigned int cell, int fills, uint32_t *row, uint16_t *crow, uint32 wbit);

static void DrawBG(uint8 *target, uint16 RealY, bool lr, uint8 bgmap_base_raw, bool overplane, uint16 overplane_char, uint32 SourceX, uint32 SourceY, uint32 scx, uint32 scy, uint16 DestX, uint16 DestY, uint16 DestWidth, uint16 DestHeight)
{
 int x;
 const uint16 *CHR16 = CHR_RAM;
 const uint16 *BGMap = DRAM;
 uint32 BGMap_Base = bgmap_base_raw << 12;
 uint32 used_chr = 0xFFFF; /* (the last character marked used: the marks only need setting once) */
 int32 start_x, final_x;
 const uint32 bgsc_overplane = DRAM[overplane_char];
 const uint32 BGMap_XCount = 1 << scx;
 const uint32 BGMap_YCount = 1 << scy;
 const uint32 SourceX_Size = 512 * BGMap_XCount;
 const uint32 SourceY_Size = 512 * BGMap_YCount;
 const uint32 SourceX_Mask = overplane ? 0x1FFF : (SourceX_Size - 1);
 const uint32 SourceY_Mask = overplane ? 0x1FFF : (SourceY_Size - 1);

 if((uint16)(RealY - DestY) > DestHeight)
  return;

 DestX = sign_10_to_s16(DestX);

 if(DestX & 0x8000)
  SourceX -= DestX;

 start_x = (int16)DestX;
 final_x = (int16)DestX + DestWidth;

 if(start_x < 0)
  start_x = 0;

 if(final_x > 383)
  final_x = 383;

 if(start_x < ClipX0)
 {
  SourceX += ClipX0 - start_x;
  start_x = ClipX0;
 }
 if(final_x > ClipX1)
  final_x = ClipX1;

 if(start_x > final_x)
  return;

 /* Optimization: */
 SourceY &= SourceY_Mask;
 BGMap_Base |= (((SourceY >> 3) & 0x3F) * 0x40) | (((SourceY << 3) & ~0xFFF) << scx);

 USE_ROW(overplane_char);
 for(x = 0; x < (int)BGMap_XCount; x++)
  USE_ROW(BGMap_Base | (x << 12));

 for(x = start_x; x <= final_x; x++)
 {
  unsigned int char_sub_y;
  uint32 bgsc;
  uint32 char_no;
  uint32 palette_selector;
  uint32 hflip_xor;
  uint32 vflip_xor;

  SourceX &= SourceX_Mask;

  bgsc = bgsc_overplane;

  if(SourceX < SourceX_Size && SourceY < SourceY_Size)
   bgsc = BGMap[(BGMap_Base | ((SourceX << 3) & ~0xFFF) | ((SourceX >> 3) & 0x3F)) & 0xFFFF];

  char_no = bgsc & 0x7FF;
  palette_selector = bgsc >> 14;
  hflip_xor = (bgsc & 0x2000) ? 7 : 0;
  vflip_xor = (bgsc & 0x1000) ? 7 : 0;

  char_sub_y = vflip_xor ^ (SourceY & 0x7);

  if(!(SourceX & 7) && (x + 7) <= final_x)
  {
   uint32 pixels = CHR16[char_no * 8 + char_sub_y];

   if(char_no != used_chr)
   {
    USE_BG_CHR(char_no);
    used_chr = char_no;
   }
   if(pixels)
   {
    /* All eight shades at once, merged into the row under the drawn pixels. */
    uint32 p = pixels, shades[2], drawn[2], row[2];
    if(bgsc & 0x2000)
    {
     p = ((p >> 2) & 0x3333) | ((p & 0x3333) << 2);
     p = ((p >> 4) & 0x0F0F) | ((p & 0x0F0F) << 4);
     p = ((p >> 8) & 0x00FF) | ((p & 0x00FF) << 8);
    }
    memcpy(&shades[0], GPLT_Quad[palette_selector][p & 0xFF], 4);
    memcpy(&shades[1], GPLT_Quad[palette_selector][p >> 8], 4);
    if(((pixels | (pixels >> 1)) & 0x5555) == 0x5555) /* (every pixel drawn: the row is just the shades) */
    {
     memcpy(target + x, &shades[0], 4);
     memcpy(target + x + 4, &shades[1], 4);
    }
    else
    {
     memcpy(&drawn[0], OpaqueQuad[p & 0xFF], 4);
     memcpy(&drawn[1], OpaqueQuad[p >> 8], 4);
     memcpy(&row[0], target + x, 4);
     memcpy(&row[1], target + x + 4, 4);
     row[0] = (row[0] & ~drawn[0]) | (shades[0] & drawn[0]);
     row[1] = (row[1] & ~drawn[1]) | (shades[1] & drawn[1]);
     memcpy(target + x, &row[0], 4);
     memcpy(target + x + 4, &row[1], 4);
    }
   }

   x += 7;
   SourceX += 8;
  }
  else if(vbgo_row) /* VirtualBoyGo */
  {
   /* (the rest of this character's row inside the line, or what is left of the line: one run) */
   const unsigned int offset = SourceX & 7;
   unsigned int run = 8 - offset;
   uint32 p = CHR16[char_no * 8 + char_sub_y];
   if(run > (unsigned int)(final_x - x + 1))
    run = final_x - x + 1;
   if(char_no != used_chr)
   {
    USE_BG_CHR(char_no);
    used_chr = char_no;
   }
   if(bgsc & 0x2000)
   {
    p = ((p >> 2) & 0x3333) | ((p & 0x3333) << 2);
    p = ((p >> 4) & 0x0F0F) | ((p & 0x0F0F) << 4);
    p = ((p >> 8) & 0x00FF) | ((p & 0x00FF) << 8);
   }
   p = (p >> (offset * 2)) & ((1u << (run * 2)) - 1);
   VbgoAffineRun(target, x, offset, run, p, bgsc, vbgo_bits | VBGO_TT_BG_BITS(char_no, 0, char_sub_y, palette_selector, 0), vbgo_cell,
                 vbgo_tt_fills_from(char_no, vbgo_cell), vbgo_row, vbgo_crow, vbgo_wbit);
   x += run - 1;
   SourceX += run;
  }
  else
  {
   unsigned int char_sub_x = hflip_xor ^ (SourceX & 0x7);
   uint8 pixel = (CHR16[char_no * 8 + char_sub_y] >> (char_sub_x * 2)) & 0x3;

   if(char_no != used_chr)
   {
    USE_BG_CHR(char_no);
    used_chr = char_no;
   }
   if(pixel)
    target[x] = GPLT_Cache[palette_selector][pixel];
   SourceX++;
  }
 }
}

static void DrawAffine(uint8 *target, uint16 RealY, bool lr, uint32 ParamBase, uint32 BGMap_Base, bool OverplaneMode, uint16 OverplaneChar, uint32 scx, uint32 scy,
			uint16 DestX, uint16 DestY, uint16 DestWidth, uint16 DestHeight)
{
 const uint16 *CHR16 = CHR_RAM;
 const uint16 *BGMap = DRAM;
 uint32 used_chr = 0xFFFF; /* (as in DrawBG) */

 const uint32 BGMap_XCount = 1 << scx;
 const uint32 BGMap_YCount = 1 << scy;
 const uint32 SourceX_Size = 512 * BGMap_XCount;
 const uint32 SourceY_Size = 512 * BGMap_YCount;

 const uint16 *param_ptr = &DRAM[(ParamBase + 8 * (RealY - DestY)) & 0xFFFF];
 int16 mx = param_ptr[0], mp = (ParallaxDisabled ? 0 : param_ptr[1]), my = param_ptr[2], dx = param_ptr[3], dy = param_ptr[4];

 uint32 SourceX, SourceY;
 uint32 SourceX_Mask, SourceY_Mask;

 int32 start_x, final_x;
 const uint32 bgsc_overplane = DRAM[OverplaneChar];


 DestX = sign_10_to_s16(DestX);

 if((uint16)(RealY - DestY) > DestHeight)
  return;
 USE_ROW(ParamBase + 8 * (RealY - DestY)); /* (the line's 8-halfword group: one row) */

 SourceX = (int32)mx << 6;
 SourceY = (int32)my << 6;

 if(DestX & 0x8000)
 {
  SourceX += dx * (65536 - DestX);
  SourceY += dy * (65536 - DestX);
 }

 if(mp >= 0 && lr)
 {
  SourceX += dx * mp;
  SourceY += dy * mp;
 }
 else if(mp < 0 && !lr)
 {
  SourceX += dx * -mp;
  SourceY += dy * -mp;
 }

 if(OverplaneMode)
 {
  SourceX_Mask = 0x3FFFFFF;
  SourceY_Mask = 0x3FFFFFF;
 }
 else
 {
  SourceX_Mask = ((uint32)SourceX_Size << 9) - 1;
  SourceY_Mask = ((uint32)SourceY_Size << 9) - 1;
 }

 start_x = (int16)DestX;
 final_x = (int16)DestX + DestWidth;

 if(start_x < 0)
  start_x = 0;

 if(final_x > 383)
  final_x = 383;

 if(start_x < ClipX0)
 {
  SourceX += dx * (ClipX0 - start_x);
  SourceY += dy * (ClipX0 - start_x);
  start_x = ClipX0;
 }
 if(final_x > ClipX1)
  final_x = ClipX1;

 USE_ROW(OverplaneChar);

 /* (steps to a cell's edge: 4095 / |step|, as a 32-bit fraction - never over, so at most one pixel short) */
 const uint32 blank_rdx = dx ? 0xFFFFFFFFu / (uint32)(dx < 0 ? -dx : dx) : 0, blank_rdy = dy ? 0xFFFFFFFFu / (uint32)(dy < 0 ? -dy : dy) : 0;
if(dy == 0)	/* Optimization for no rotation. */
{
 int x;
 SourceY &= SourceY_Mask;

 if(SourceY >= (SourceY_Size << 9))
  return;

 BGMap_Base |= (((SourceY >> 6) & ~0xFFF) << scx) | (((SourceY >> 12) & 0x3F) * 0x40);
 /* Everything but char_sub_x is the same across one 8x8 map cell (SourceX >> 12; the map's edge is on a cell's),
    and a scaled line often stays in a cell for several pixels. */
 uint32 cell_at = ~0u, bgsc = 0, hflip_xor = 0, char_row = 0;
 const uint8 *plt = GPLT_Cache[0];
 unsigned int char_sub_y = 0;
 for(x = start_x; x <= final_x; x++)
 {
  unsigned int char_sub_x;
  uint32 pixel = 0;

  SourceX &= SourceX_Mask;

  if((SourceX >> 12) != cell_at)
  {
   cell_at = SourceX >> 12;
   bgsc = bgsc_overplane;

   if(SourceX < (SourceX_Size << 9))
    bgsc = BGMap[(BGMap_Base | ((SourceX >> 6) & ~0xFFF) | ((SourceX >> 12) & 0x3F)) & 0xFFFF];

   hflip_xor = ((int32)(bgsc << 18) >> 30) & 0xE;
   char_sub_y = (((int32)(bgsc << 19) >> 31) & 0x7) ^ ((SourceY >> 9) & 0x7);
   char_row = CHR16[((bgsc & 0x7FF) * 8) | char_sub_y];
   plt = GPLT_Cache[bgsc >> 14];
   if((bgsc & 0x7FF) != used_chr)
   {
    USE_BG_CHR(bgsc & 0x7FF);
    used_chr = bgsc & 0x7FF;
   }
  }

  char_sub_x = hflip_xor ^ ((SourceX >> 8) & 0xE);
  pixel = (char_row >> char_sub_x) & 0x3;

  if(pixel)
   target[x] = plt[pixel];

  SourceX += dx;
 }
}
else
{
 int x;
 /* As above, with the cell in both directions (SourceX >> 12, SourceY >> 12: 14 bits each); rotated, the row in
    the character can change from one pixel to the next. */
 uint32 cell_at = ~0u, bgsc = 0, char_no = 0, palette_selector = 0, hflip_xor = 0, vflip_xor = 0;
 for(x = start_x; x <= final_x; x++)
 {
  uint8 pixel = 0;
  unsigned int char_sub_y;
  unsigned int char_sub_x;

  SourceX &= SourceX_Mask;
  SourceY &= SourceY_Mask;

  if(((SourceX >> 12) | ((SourceY >> 12) << 14)) != cell_at)
  {
   cell_at = (SourceX >> 12) | ((SourceY >> 12) << 14);
   bgsc = bgsc_overplane;

   if(SourceX < (SourceX_Size << 9) && SourceY < (SourceY_Size << 9))
   {
    uint32 m_index = ((SourceX >> 6) & ~0xFFF) + (((SourceY >> 6) & ~0xFFF) << scx);
    uint32 sub_index = ((SourceX >> 12) & 0x3F) + (((SourceY >> 12) & 0x3F) * 0x40);

    bgsc = BGMap[(BGMap_Base | m_index | sub_index) & 0xFFFF];
   }
   char_no = bgsc & 0x7FF;
   palette_selector = bgsc >> 14;
   hflip_xor = bgsc & 0x2000 ? 7 : 0;
   vflip_xor = bgsc & 0x1000 ? 7 : 0;
   if(char_no != used_chr)
   {
    USE_BG_CHR(char_no);
    used_chr = char_no;
   }
   SKIP_BLANK_ROTATED
  }

  char_sub_y = vflip_xor ^ ((SourceY >> 9) & 0x7);
  char_sub_x = hflip_xor ^ ((SourceX >> 9) & 0x7);

  pixel = (CHR16[char_no * 8 + char_sub_y] >> (char_sub_x * 2)) & 0x3;

  if(pixel)
   target[x] = GPLT_Cache[palette_selector][pixel];

  SourceX += dx;
  SourceY += dy;
 }
}
}

static int obj_search_which;

/* A group's sprites sorted into the lists of the 28 blocks they reach (a sprite reaches two at most), as GatherOBJ walks them. */
typedef struct
{
 uint64 serial; /* ObjSerial when sorted (0: not sorted) */
 int32 start, end;
 uint16 first[29]; /* block b's list is entry[first[b]] up to entry[first[b + 1]] */
 uint16 entry[2048];
} ObjLists;
static ObjLists obj_sorted[4];
static uint64 obj_walked[4]; /* ObjSerial when the group was last walked for a block */
static uint32 obj_blocks[256]; /* by a sprite's JY low byte: the blocks (a bit each) GatherOBJ finds it in */
static bool obj_blocks_ready;

static void SortOBJ(ObjLists *sorted, int32 start_oam, int32 end_oam)
{
 int32 oam;
 int b, fill[28];
 if(!obj_blocks_ready)
 {
  int j;
  for(j = 0; j < 256; j++)
  {
   obj_blocks[j] = 0;
   for(b = 0; b < 28; b++)
    if((((uint32)(b * 8 - j + 7)) & 0xFF) < 15)
     obj_blocks[j] |= 1u << b;
  }
  obj_blocks_ready = true;
 }
 memset(sorted->first, 0, sizeof(sorted->first));
 oam = start_oam;
 do
 {
  uint32 m = obj_blocks[DRAM[(0x1E000 + (oam * 8) + 4) >> 1] & 0xFF];
  for(; m; m &= m - 1)
   sorted->first[__builtin_ctz(m) + 1]++;
 } while( (oam = (oam - 1) & 1023) != end_oam);
 for(b = 0; b < 28; b++)
 {
  sorted->first[b + 1] += sorted->first[b];
  fill[b] = sorted->first[b];
 }
 oam = start_oam;
 do
 {
  uint32 m = obj_blocks[DRAM[(0x1E000 + (oam * 8) + 4) >> 1] & 0xFF];
  for(; m; m &= m - 1)
   sorted->entry[fill[__builtin_ctz(m)]++] = oam;
 } while( (oam = (oam - 1) & 1023) != end_oam);
 sorted->start = start_oam, sorted->end = end_oam, sorted->serial = ObjSerial;
}

/* The OBJ group's sprites (in drawing order) with a row in the block that
 * starts at row block_y: the only ones DrawOBJ can draw on its 8 rows. */
static int GatherOBJ(uint16 *list, uint16 block_y)
{
 int32 oam;
 int count = 0;
 const int32 start_oam = SPT[obj_search_which];
 const int32 end_oam = obj_search_which ? SPT[obj_search_which - 1] : 1023;

 if(!(block_y & 7) && block_y < 224)
 {
  ObjLists *const sorted = &obj_sorted[obj_search_which];
  if((sorted->serial != ObjSerial || sorted->start != start_oam || sorted->end != end_oam) && obj_walked[obj_search_which] == ObjSerial)
   SortOBJ(sorted, start_oam, end_oam);
  if(sorted->serial == ObjSerial && sorted->start == start_oam && sorted->end == end_oam)
  {
   const int b = block_y >> 3;
   for(count = 0; count < sorted->first[b + 1] - sorted->first[b]; count++)
    list[count] = sorted->entry[sorted->first[b] + count];
   return count;
  }
  obj_walked[obj_search_which] = ObjSerial;
 }

 oam = start_oam;
 do
 {
  const uint32 jy = DRAM[(0x1E000 + (oam * 8) + 4) >> 1];

  if(((block_y - jy + 7) & 0xFF) < 15)
   list[count++] = oam;
 } while( (oam = (oam - 1) & 1023) != end_oam);

 return count;
}

static void DrawOBJ(uint8 *fb[2], uint16 Y, bool lron[2], const uint16 *list, int count)
{
 int k;
 const uint16 *CHR16 = CHR_RAM;

 for(k = 0; k < count; k++)
 {
  const int32 oam = list[k];
  int lr;
  uint32 pixels_save;
  uint32 char_no;
  uint32 jx, jp;
  uint32 palette_selector;
  uint32 vflip_xor;
  uint32 char_sub_y;
  bool jlron[2];
  const uint16 *oam_ptr = &DRAM[(0x1E000 + (oam * 8)) >> 1];
  const uint32 jy = oam_ptr[2];
  const uint32 tile_y = (Y - jy) & 0xFF;

  if(tile_y >= 8)
   continue;

  jx = oam_ptr[0];
  jp = ParallaxDisabled ? 0 : (oam_ptr[1] & 0x3FFF);
  palette_selector = oam_ptr[3] >> 14;
  vflip_xor = (oam_ptr[3] & 0x1000) ? 7 : 0;
  char_sub_y = vflip_xor ^ tile_y;
  jlron[0] = (bool)(oam_ptr[1] & 0x8000);
  jlron[1] = (bool)(oam_ptr[1] & 0x4000);
  char_no = oam_ptr[3] & 0x7FF;
  pixels_save = CHR16[char_no * 8 + char_sub_y];
  USE_CHR(char_no);

  for(lr = 0; lr < 2; lr++)
  {
   uint32 pixels;
   int32 x;
   if(!(jlron[lr] & lron[lr]))
    continue;

   pixels = pixels_save;
   x = sign_x_to_s32(10, (jx + (lr ? jp : -jp))); /* It may actually be 9, TODO? */

   if(x >= -7 && x < 384 && x + 7 >= ClipX0 && x <= ClipX1)	/* Make sure we always keep the pitch of our 384x8 buffer large enough(with padding before and after the visible space) */
   {
    uint8 *target = &fb[lr][x];

    if(oam_ptr[3] & 0x2000)
    {
     int meow;
     target += 7;

     for(meow = 8; meow; meow--)
     {
      if(pixels & 3)
       *target = JPLT_Cache[palette_selector][pixels & 3];
      target--;
      pixels >>= 2;
     }
    }
    else
    {
     int meow;
     for(meow = 8; meow; meow--)
     {
      if(pixels & 3)
       *target = JPLT_Cache[palette_selector][pixels & 3];
      target++;
      pixels >>= 2;
     }
    }

   }

  }
 }

}


/* A drawn world's attributes, decoded once per block. */
typedef struct
{
 uint32 bgmap_base, scy, scx, bgm, param_base;
 bool over, lron[2];
 uint16 gx, gp, gy, mx, mp, my, window_width, window_height, overplane_char;
 int tt_world;
} VbgoWorldAttr;


/* One ordinary background world's rows of the block, for one eye, a character at a time: what DrawBG draws row by row, pixel for
 * pixel and tag for tag, but the up to 8 rows that share a map row read each map entry once. */
static void DrawBGBlock(uint8 *fb_eye, unsigned block_no, bool lr, const VbgoWorldAttr *a)
{
 const uint16 *CHR16 = CHR_RAM;
 const uint16 *BGMap = DRAM;
 const uint32 SourceX_Size = 512 * (1u << a->scx), SourceY_Size = 512 * (1u << a->scy);
 const uint32 SourceX_Mask = SourceX_Size - 1, SourceY_Mask = SourceY_Size - 1;
 uint16 srcX = a->mx + (lr ? a->mp : -a->mp);
 uint16 DestX = a->gx + (lr ? a->gp : -a->gp);
 uint32 SourceX = (uint32)(int32)(int16)srcX;
 const uint32_t vbgo_bits = vbgo_tt_call_bits();
 const uint32_t vbgo_wbit = 1u << VBGO_TAG_WORLD(vbgo_bits);
 int32 start_x, final_x;
 int y = 0;

 DestX = sign_10_to_s16(DestX);
 if(DestX & 0x8000)
  SourceX -= DestX;
 start_x = (int16)DestX;
 final_x = (int16)DestX + a->window_width;
 if(start_x < 0)
  start_x = 0;
 if(final_x > 383)
  final_x = 383;
 if(start_x < ClipX0)
 {
  SourceX += ClipX0 - start_x;
  start_x = ClipX0;
 }
 if(final_x > ClipX1)
  final_x = ClipX1;
 if(start_x > final_x)
  return;

 while(y < 8)
 {
  uint8 *tgt[8];
  uint32_t *trow[8];
  uint16_t *crow[8];
  unsigned sy7[8];
  int n = 0;
  uint32 SourceY = 0, BGMap_Base, used_chr = 0xFFFF;
  int32 x;
  uint32 sx;
  /* (rows outside the window draw nothing) */
  for(; y < 8 && (uint16)((block_no * 8 + y) - a->gy) > a->window_height; y++)
   ;
  if(y == 8)
   break;
  for(; y < 8 && (uint16)((block_no * 8 + y) - a->gy) <= a->window_height; y++)
  {
   const uint16 srcY = a->my + ((uint16)(block_no * 8 + y) - a->gy);
   const uint32 sy = ((uint32)(int32)(int16)srcY) & SourceY_Mask;
   if(n && (sy >> 3) != (SourceY >> 3))
    break;
   SourceY = sy;
   tgt[n] = fb_eye + y * 512;
   trow[n] = vbgo_tt_row(tgt[n]);
   crow[n] = vbgo_tt_cell_row(tgt[n]);
   sy7[n] = sy & 7;
   n++;
  }

  BGMap_Base = (a->bgmap_base << 12) | (((SourceY >> 3) & 0x3F) * 0x40) | (((SourceY << 3) & ~0xFFF) << a->scx);
  USE_ROW(a->overplane_char);
  {
   int xs;
   for(xs = 0; xs < (1 << a->scx); xs++)
    USE_ROW(BGMap_Base | (xs << 12));
  }

  x = start_x;
  sx = SourceX;
  while(x <= final_x)
  {
   unsigned cell, offset, run, i;
   uint32 bgsc, char_no;
   sx &= SourceX_Mask;
   cell = (BGMap_Base | ((sx << 3) & ~0xFFF) | ((sx >> 3) & 0x3F)) & 0xFFFF;
   bgsc = BGMap[cell];
   char_no = bgsc & 0x7FF;
   if(char_no != used_chr)
   {
    USE_BG_CHR(char_no);
    used_chr = char_no;
   }
   offset = sx & 7;
   if(vbgo_tt_blank[char_no])
   {
    /* (a character that has no pixel can't fill either: it and the tiles after it with the same entry draw nothing) */
    uint32 blank_w[4];
    memcpy(blank_w, &CHR16[char_no * 8], 16);
    if(!(blank_w[0] | blank_w[1] | blank_w[2] | blank_w[3]))
    {
     run = offset ? 8 - offset : 8;
     x += run;
     sx += run;
     while(x + 7 <= final_x)
     {
      uint32 s2 = sx & SourceX_Mask;
      if(BGMap[(BGMap_Base | ((s2 << 3) & ~0xFFF) | ((s2 >> 3) & 0x3F)) & 0xFFFF] != bgsc)
       break;
      x += 8;
      sx += 8;
     }
     continue;
    }
   }
   if(!offset && x + 7 <= final_x)
   {
    const int flipped = (bgsc & 0x2000) != 0;
    const uint32 palette_selector = bgsc >> 14;
    const int fills_here = vbgo_tt_fills_from(char_no, cell);
    int tile_tagged = 0; /* (the most any of its rows tagged: 1 something, 2 a fill too) */
    for(i = 0; i < (unsigned)n; i++)
    {
     const unsigned char_sub_y = ((bgsc & 0x1000) ? 7 : 0) ^ sy7[i];
     uint32 pixels = CHR16[char_no * 8 + char_sub_y];
     if(pixels)
     {
      /* All eight shades at once, merged into the row under the drawn pixels. */
      uint32 p = pixels, shades[2], drawn[2], row[2];
      if(flipped)
      {
       p = ((p >> 2) & 0x3333) | ((p & 0x3333) << 2);
       p = ((p >> 4) & 0x0F0F) | ((p & 0x0F0F) << 4);
       p = ((p >> 8) & 0x00FF) | ((p & 0x00FF) << 8);
      }
      memcpy(&shades[0], GPLT_Quad[palette_selector][p & 0xFF], 4);
      memcpy(&shades[1], GPLT_Quad[palette_selector][p >> 8], 4);
      if(((pixels | (pixels >> 1)) & 0x5555) == 0x5555) /* (every pixel drawn: the row is just the shades) */
      {
       memcpy(tgt[i] + x, &shades[0], 4);
       memcpy(tgt[i] + x + 4, &shades[1], 4);
      }
      else
      {
       memcpy(&drawn[0], OpaqueQuad[p & 0xFF], 4);
       memcpy(&drawn[1], OpaqueQuad[p >> 8], 4);
       memcpy(&row[0], tgt[i] + x, 4);
       memcpy(&row[1], tgt[i] + x + 4, 4);
       row[0] = (row[0] & ~drawn[0]) | (shades[0] & drawn[0]);
       row[1] = (row[1] & ~drawn[1]) | (shades[1] & drawn[1]);
       memcpy(tgt[i] + x, &row[0], 4);
       memcpy(tgt[i] + x + 4, &row[1], 4);
      }
     }
     if(trow[i] && (pixels || fills_here))
     {
      const int tagged = vbgo_tt_tag_row8(trow[i] + x * VBGO_TT_COLUMN, crow[i] + x * VBGO_TT_CELL_COLUMN, pixels, flipped,
                                          vbgo_bits | VBGO_TT_BG_BITS(char_no, 0, char_sub_y, palette_selector, 0), char_no, cell);
      if(tagged > tile_tagged)
       tile_tagged = tagged;
     }
    }
    if(tile_tagged)
     vbgo_tt_wcells[x >> 3] |= vbgo_wbit, vbgo_tt_wcells[(x + 7) >> 3] |= vbgo_wbit;
    if(tile_tagged == 2)
     vbgo_tt_fillc[x >> 3] = vbgo_tt_fillc[(x + 7) >> 3] = 1;
    x += 8;
    sx += 8;
   }
   else
   {
    /* (the rest of this character's row inside the line, or what is left of the line: one run) */
    const int fills_here = vbgo_tt_fills_from(char_no, cell);
    const uint32 palette_selector = bgsc >> 14;
    run = 8 - offset;
    if(run > (unsigned int)(final_x - x + 1))
     run = final_x - x + 1;
    for(i = 0; i < (unsigned)n; i++)
    {
     const unsigned char_sub_y = ((bgsc & 0x1000) ? 7 : 0) ^ sy7[i];
     uint32 p = CHR16[char_no * 8 + char_sub_y];
     if(bgsc & 0x2000)
     {
      p = ((p >> 2) & 0x3333) | ((p & 0x3333) << 2);
      p = ((p >> 4) & 0x0F0F) | ((p & 0x0F0F) << 4);
      p = ((p >> 8) & 0x00FF) | ((p & 0x00FF) << 8);
     }
     p = (p >> (offset * 2)) & ((1u << (run * 2)) - 1);
     VbgoAffineRun(tgt[i], x, offset, run, p, bgsc, vbgo_bits | VBGO_TT_BG_BITS(char_no, 0, char_sub_y, palette_selector, 0), cell,
                   fills_here, trow[i], crow[i], vbgo_wbit);
    }
    x += run;
    sx += run;
   }
  }
 }
}

#include "vip_front.inc"

void VIP_DrawBlock(uint8 block_no, uint8 *fb_l, uint8 *fb_r)
{
 /* World by world, back to front, each through its 8 rows: each pixel gets the same writes in the same order (rows don't touch
  * each other), and a background world's rows share their map entries. */
 static uint16 obj_lists[32][1024];
 const uint16 *drawn[32];
 VbgoWorldAttr attrs[32];
 int counts[32], drawn_count = 0, obj_lists_used = 0, obj_index = 0, active = 0, bg_active = 0;
 bool front_ok = vbgo_tt_on != 0, overplane = false;
 int y, world, lr, k;
 for( y = 0; y < 8; y++)
 {
  memset(fb_l + y * 512, BKCOL, 384);
  memset(fb_r + y * 512, BKCOL, 384);
 }

 obj_search_which = 3;

 for(world = 31; world >= 0; world--)
 {
  const uint16 *world_ptr = &DRAM[(0x1D800 + world * 0x20) >> 1];
  bool end = world_ptr[0] & 0x40;
  uint32 bgm = (world_ptr[0] >> 12) & 3;

  if(end)
   break;

  counts[drawn_count] = 0;
  if(bgm == BGM_OBJ)
  {
   uint16 *obj_list = obj_lists[obj_lists_used++];
   const int obj_count = GatherOBJ(obj_list, block_no * 8);
   *UsedObjs = HashObjList(*UsedObjs, world | (world_ptr[0] >> 9 & 0x60), obj_list, obj_count);
   NoteSprites(world | (world_ptr[0] >> 9 & 0x60), obj_list, obj_count); /* (with its eyes) */
   counts[drawn_count] = obj_count;
   if(obj_search_which)
    obj_search_which--;
  }
  {
   VbgoWorldAttr *const a = &attrs[drawn_count];
   a->bgmap_base = world_ptr[0] & 0xF;
   a->over = world_ptr[0] & 0x80;
   a->scy = (world_ptr[0] >> 8) & 3;
   a->scx = (world_ptr[0] >> 10) & 3;
   a->bgm = (world_ptr[0] >> 12) & 3;
   a->lron[0] = (bool)(world_ptr[0] & 0x8000);
   a->lron[1] = (bool)(world_ptr[0] & 0x4000);
   a->gx = sign_11_to_s16(world_ptr[1]);
   a->gp = ParallaxDisabled ? 0 : sign_9_to_s16(world_ptr[2]);
   a->gy = sign_11_to_s16(world_ptr[3]);
   a->mx = world_ptr[4];
   a->mp = ParallaxDisabled ? 0 : sign_9_to_s16(world_ptr[5]);
   a->my = world_ptr[6];
   a->window_width = sign_11_to_s16(world_ptr[7]);
   a->window_height = (world_ptr[8] & 0x3FF);
   a->param_base = (world_ptr[9] & 0xFFF0);
   a->overplane_char = world_ptr[10];
   a->tt_world = (int)((world_ptr - &DRAM[0x1D800 >> 1]) / 16);
   /* (can it draw in this block: front to back needs more than one, and no affine one) */
   if((a->lron[0] || a->lron[1]) &&
      (bgm == BGM_OBJ ? counts[drawn_count] > 0 : (uint16)((uint16)(block_no * 8 - a->gy) + 7) <= a->window_height + 7))
   {
    active++;
    if(bgm != BGM_OBJ)
     bg_active++;
    if(bgm == BGM_AFFINE)
     front_ok = false;
    if(bgm != BGM_OBJ && a->over)
     overplane = true;
   }
  }
  drawn[drawn_count++] = world_ptr;
 }

 if(front_ok && active > 1 && VbgoFrontTry(block_no, overplane, bg_active))
 {
  VbgoDrawFront(block_no, fb_l, fb_r, attrs, drawn_count, obj_lists, obj_lists_used, counts);
  VbgoFrontDone(block_no, overplane, bg_active);
  return;
 }

 for(k = 0; k < drawn_count; k++)
 {
  VbgoWorldAttr *const a = &attrs[k];
  const uint32 bgmap_base = a->bgmap_base, scy = a->scy, scx = a->scx, bgm = a->bgm, param_base = a->param_base;
  const bool over = a->over;
  bool *const lron = a->lron;
  const uint16 gx = a->gx, gp = a->gp, gy = a->gy, mx = a->mx, mp = a->mp, my = a->my;
  const uint16 window_width = a->window_width, window_height = a->window_height, overplane_char = a->overplane_char;

  vbgo_tt_world = a->tt_world;

  if(bgm == BGM_OBJ)
  {
   const uint16 *const list = obj_lists[obj_index++];
   for(y = 0; y < 8; y++)
   {
    uint8 *fb[2] = { &fb_l[y * 512], &fb_r[y * 512] };
    DrawOBJ(fb, (block_no * 8) + y, lron, list, counts[k]);
   }
  }
  else if(bgm == BGM_AFFINE)
  {
   for(y = 0; y < 8; y++)
   {
    uint8 *fb[2] = { &fb_l[y * 512], &fb_r[y * 512] };
    for(lr = 0; lr < 2; lr++)
    {
     if(lron[lr])
     {
      DrawAffine(fb[lr], (block_no * 8) + y, lr, param_base, bgmap_base * 4096, over, overplane_char, scx, scy,
                        gx + (lr ? gp : -gp), gy, window_width, window_height);
     }
    }
   }
  }
  else if(bgm == 0 && !over && vbgo_tt_on)
  {
   for(lr = 0; lr < 2; lr++)
    if(lron[lr])
     DrawBGBlock(lr ? fb_r : fb_l, block_no, lr, a);
  }
  else
  for(y = 0; y < 8; y++)
  {
   uint8 *fb[2] = { &fb_l[y * 512], &fb_r[y * 512] };
   for(lr = 0; lr < 2; lr++)
   {
    uint16 srcX, srcY;
    uint16 RealY = (block_no * 8) + y;
    uint16 DestX;
    uint16 DestY;

    srcX = mx + (lr ? mp : -mp);
    srcY = my + (RealY - gy);

    DestX = gx + (lr ? gp : -gp);
    DestY = gy;

    if(lron[lr])
    {
     if(bgm == 1)	/* HBias */
     {
      if((uint16)(RealY - DestY) <= window_height) /* (DrawBG uses it only then) */
       USE_ROW(param_base + (((RealY - DestY) * 2) | lr));
      srcX += (int16)DRAM[(param_base + (((RealY - DestY) * 2) | lr)) & 0xFFFF];
     }

     DrawBG(fb[lr], RealY, lr, bgmap_base, over, overplane_char, (int32)(int16)srcX, (int32)(int16)srcY, scx, scy, DestX, DestY, window_width, window_height);
    }
   }
  }
 }
}
