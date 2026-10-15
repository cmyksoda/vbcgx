#include "vb.h"

#define BGM_AFFINE	0x2
#define BGM_OBJ		0x3

/* What the block being drawn reads (see BlockInputs in vip.c). */
#define USE_CHR(c) (UsedChr[(c) >> 5] |= 1u << ((c) & 31))
#define USE_BG_CHR(c) (USE_CHR(c), UsedBgChr[(c) >> 5] |= 1u << ((c) & 31))
#define USE_ROW(h) (UsedRows[((h) & 0xFFFF) >> 11] |= 1u << ((((h) & 0xFFFF) >> 6) & 31))


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
    memcpy(&drawn[0], OpaqueQuad[p & 0xFF], 4);
    memcpy(&drawn[1], OpaqueQuad[p >> 8], 4);
    memcpy(&row[0], target + x, 4);
    memcpy(&row[1], target + x + 4, 4);
    row[0] = (row[0] & ~drawn[0]) | (shades[0] & drawn[0]);
    row[1] = (row[1] & ~drawn[1]) | (shades[1] & drawn[1]);
    memcpy(target + x, &row[0], 4);
    memcpy(target + x + 4, &row[1], 4);
   }

   x += 7;
   SourceX += 8;
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
   if((bgsc & 0x7FF) != used_chr)
   {
    USE_BG_CHR(bgsc & 0x7FF);
    used_chr = bgsc & 0x7FF;
   }
  }

  char_sub_x = hflip_xor ^ ((SourceX >> 8) & 0xE);
  pixel = (char_row >> char_sub_x) & 0x3;

  if(pixel)
   target[x] = GPLT_Cache[bgsc >> 14][pixel];

  SourceX += dx;
 }
}
else
{
 int x;
 for(x = start_x; x <= final_x; x++)
 {
  uint32 bgsc;
  uint32 char_no;
  uint32 palette_selector;
  uint32 hflip_xor;
  uint32 vflip_xor;
  uint8 pixel = 0;
  unsigned int char_sub_y;
  unsigned int char_sub_x;

  SourceX &= SourceX_Mask;
  SourceY &= SourceY_Mask;

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

  char_sub_y = vflip_xor ^ ((SourceY >> 9) & 0x7);
  char_sub_x = hflip_xor ^ ((SourceX >> 9) & 0x7);

  pixel = (CHR16[char_no * 8 + char_sub_y] >> (char_sub_x * 2)) & 0x3;
  if(char_no != used_chr)
  {
   USE_BG_CHR(char_no);
   used_chr = char_no;
  }

  if(pixel)
   target[x] = GPLT_Cache[palette_selector][pixel];

  SourceX += dx;
  SourceY += dy;
 }
}
}

static int obj_search_which;

/* The OBJ group's sprites (in drawing order) with a row in the block that
 * starts at row block_y: the only ones DrawOBJ can draw on its 8 rows. */
static int GatherOBJ(uint16 *list, uint16 block_y)
{
 int32 oam;
 int count = 0;
 const int32 start_oam = SPT[obj_search_which];
 const int32 end_oam = obj_search_which ? SPT[obj_search_which - 1] : 1023;

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

   if(x >= -7 && x < 384)	/* Make sure we always keep the pitch of our 384x8 buffer large enough(with padding before and after the visible space) */
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


void VIP_DrawBlock(uint8 block_no, uint8 *fb_l, uint8 *fb_r)
{
 /* Row by row, each row through every world back to front: each pixel gets the
  * same writes in the same order, while the row's pixels and tags stay in the cache. */
 static uint16 obj_lists[32][1024];
 const uint16 *drawn[32];
 int counts[32], drawn_count = 0, obj_lists_used = 0;
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
   *UsedObjs = HashObjList(*UsedObjs, world, obj_list, obj_count);
   NoteSprites(world, obj_list, obj_count);
   counts[drawn_count] = obj_count;
   if(obj_search_which)
    obj_search_which--;
  }
  drawn[drawn_count++] = world_ptr;
 }

 for(y = 0; y < 8; y++)
 {
  uint8 *fb[2] = { &fb_l[y * 512], &fb_r[y * 512] };
  int obj_index = 0;

  for(k = 0; k < drawn_count; k++)
  {
  const uint16 *world_ptr = drawn[k];

  uint32 bgmap_base = world_ptr[0] & 0xF;
  bool over = world_ptr[0] & 0x80;
  uint32 scy = (world_ptr[0] >> 8) & 3;
  uint32 scx = (world_ptr[0] >> 10) & 3;
  uint32 bgm = (world_ptr[0] >> 12) & 3;
  bool lron[2] =  { (bool)(world_ptr[0] & 0x8000), (bool)(world_ptr[0] & 0x4000) };

  uint16 gx = sign_11_to_s16(world_ptr[1]);
  uint16 gp = ParallaxDisabled ? 0 : sign_9_to_s16(world_ptr[2]);
  uint16 gy = sign_11_to_s16(world_ptr[3]);
  uint16 mx = world_ptr[4];
  uint16 mp = ParallaxDisabled ? 0 : sign_9_to_s16(world_ptr[5]);
  uint16 my = world_ptr[6];
  uint16 window_width = sign_11_to_s16(world_ptr[7]);
  uint16 window_height = (world_ptr[8] & 0x3FF);
  uint32 param_base = (world_ptr[9] & 0xFFF0);
  uint16 overplane_char = world_ptr[10];

  vbgo_tt_world = (int)((world_ptr - &DRAM[0x1D800 >> 1]) / 16);

   if(bgm == BGM_OBJ)
    DrawOBJ(fb, (block_no * 8) + y, lron, obj_lists[obj_index++], counts[k]);
   else if(bgm == BGM_AFFINE)
   {
    for(lr = 0; lr < 2; lr++)
    {
     if(lron[lr])
     {
      DrawAffine(fb[lr], (block_no * 8) + y, lr, param_base, bgmap_base * 4096, over, overplane_char, scx, scy,
                        gx + (lr ? gp : -gp), gy, window_width, window_height);
     }
    }
   }
   else
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
