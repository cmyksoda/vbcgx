/* A block's worlds front to back (VbgoDrawFront): a world draws only where none in front of it has, so the pixels,
 * tags and cells are the back to front order's. The cover, per pixel of an eye's rows: 0xFF drawn, 1 only a fill (the
 * backmost world's fill stays, so one replaces those in front), 0 nothing. */

#define VBGO_COVER_WIDTH 384
typedef struct
{
 uint8 row[8][VBGO_COVER_WIDTH];
 uint16 covered[8]; /* (pixels drawn per row: a row full from ClipX0 to ClipX1 leaves a world nothing to draw) */
} VbgoCover;

/* The groups of 8 pixels a block's drawing stepped over (covered) and drew, to tell whether this way paid. */
static unsigned VbgoFrontSkipped, VbgoFrontRead;

static INLINE bool VbgoCovered8(const uint8 *cov)
{
 uint32 c0, c1;
 memcpy(&c0, cov, 4);
 memcpy(&c1, cov + 4, 4);
 return (c0 & c1) == 0xFFFFFFFFu;
}

/* Whether the run of pixels at x is covered in each of the n rows. */
static INLINE bool VbgoRowsCovered(uint8 *const *cov, int n, int x, unsigned run)
{
 int i;
 unsigned j;
 for(i = 0; i < n; i++)
  if(run == 8)
  {
   if(!VbgoCovered8(cov[i] + x))
    return false;
  }
  else
   for(j = 0; j < run; j++)
    if(cov[i][x + j] != 0xFF)
     return false;
 return true;
}

/* How many bytes a cover word gained as 0xFF between its two states (each is 0, 1 or 0xFF: bit 7 tells). */
static INLINE unsigned VbgoGained(uint32 old, uint32 now)
{
 return ((((old ^ now) >> 7) & 0x01010101u) * 0x01010101u) >> 24;
}

/* vbgo_tt_tag_row8 for the pixels nothing in front has drawn (the cover cov); whether it tagged any. */
static VBGO_NOINLINE int VbgoTagRow8Front(uint32_t *d, uint16_t *c, const uint8 *cov, unsigned pixels, int flipped,
                                          uint32_t bits, int fills, unsigned cell)
{
 int any = 0;
 unsigned k;
 for(k = 0; k < 8; k++)
 {
  const unsigned n = flipped ? 7 - k : k, pv = (pixels >> (2 * n)) & 3;
  if(cov[k] == 0xFF)
   continue;
  if(pv || fills)
  {
   d[k * VBGO_TT_COLUMN] = bits | (n << 11) | (pv << 20);
   c[k * VBGO_TT_CELL_COLUMN] = (uint16_t)cell;
   any = 1;
  }
 }
 return any;
}

/* VbgoAffineRun for the pixels nothing in front has drawn. */
static VBGO_NOINLINE void VbgoRunFront(uint8 *target, uint8 *cov, uint16 *covered, int x, unsigned int offset,
                                       unsigned int run, uint32 p, uint32 bgsc, uint32 cbits, unsigned int cell,
                                       int fills, uint32_t *row, uint16_t *crow, uint32 wbit)
{
 unsigned int j;
 for(j = 0; j < run; j++)
 {
  const int at = x + j;
  const uint32 pixel = (p >> (j * 2)) & 3;
  const uint32 in_tile = ((bgsc & 0x2000) ? 7 : 0) ^ (offset + j);
  if(cov[at] == 0xFF)
   continue;
  if(pixel)
  {
   target[at] = GPLT_Cache[bgsc >> 14][pixel];
   row[at * VBGO_TT_COLUMN] = cbits | in_tile << 11 | pixel << 20;
   crow[at * VBGO_TT_CELL_COLUMN] = (uint16_t)cell;
   vbgo_tt_wcells[at >> 3] |= wbit;
   cov[at] = 0xFF;
   ++*covered;
  }
  else if(fills)
  {
   row[at * VBGO_TT_COLUMN] = cbits | in_tile << 11;
   crow[at * VBGO_TT_CELL_COLUMN] = (uint16_t)cell;
   vbgo_tt_wcells[at >> 3] |= wbit, vbgo_tt_fillc[at >> 3] = 1;
   cov[at] = 1;
  }
 }
}

/* DrawBGBlock for the front to back order, and for the H-bias and overplane worlds DrawBG draws: only the pixels
 * nothing in front has drawn. */
static VBGO_NOINLINE void DrawBGBlockFront(uint8 *fb_eye, VbgoCover *cover, unsigned block_no, bool lr,
                                           const VbgoWorldAttr *a)
{
 const uint16 *CHR16 = CHR_RAM;
 const uint16 *BGMap = DRAM;
 const uint32 SourceX_Size = 512 * (1u << a->scx), SourceY_Size = 512 * (1u << a->scy);
 const uint32 SourceX_Mask = a->over ? 0x1FFF : SourceX_Size - 1, SourceY_Mask = a->over ? 0x1FFF : SourceY_Size - 1;
 const uint32 bgsc_overplane = DRAM[a->overplane_char];
 const bool hbias = a->bgm == 1;
 const uint16 srcX = a->mx + (lr ? a->mp : -a->mp);
 uint16 DestX = a->gx + (lr ? a->gp : -a->gp);
 uint32 SourceX_shift = 0;
 const uint32_t vbgo_bits = vbgo_tt_call_bits();
 const uint32_t vbgo_wbit = 1u << VBGO_TAG_WORLD(vbgo_bits);
 const unsigned clip_width = ClipX1 - ClipX0 + 1;
 int32 start_x, final_x;
 int y = 0;

 DestX = sign_10_to_s16(DestX);
 if(DestX & 0x8000)
  SourceX_shift -= DestX;
 start_x = (int16)DestX;
 final_x = (int16)DestX + a->window_width;
 if(start_x < 0)
  start_x = 0;
 if(final_x > 383)
  final_x = 383;
 if(start_x < ClipX0)
 {
  SourceX_shift += ClipX0 - start_x;
  start_x = ClipX0;
 }
 if(final_x > ClipX1)
  final_x = ClipX1;
 if(start_x > final_x)
  return;

 while(y < 8)
 {
  uint8 *tgt[8], *cv[8];
  uint16 *cnt[8];
  uint32_t *trow[8];
  uint16_t *crow[8];
  unsigned sy7[8];
  int n = 0;
  uint16 row_srcX = srcX;
  uint32 SourceY = 0, BGMap_Base, used_chr = 0xFFFF;
  int32 x;
  uint32 sx;
  /* (the next rows that share a map row, but not those outside the window or covered all along: nothing to draw) */
  for(; y < 8; y++)
  {
   const uint16 row_y = (uint16)((block_no * 8 + y) - a->gy);
   const uint16 srcY = a->my + row_y;
   const uint32 sy = ((uint32)(int32)(int16)srcY) & SourceY_Mask;
   if(row_y > a->window_height)
    continue;
   if(cover->covered[y] >= clip_width)
   {
    VbgoFrontSkipped += (final_x - start_x + 8) >> 3;
    continue;
   }
   if(n && (hbias || (sy >> 3) != (SourceY >> 3)))
    break;
   if(hbias)
   {
    USE_ROW(a->param_base + ((row_y * 2) | lr));
    row_srcX += (int16)DRAM[(a->param_base + ((row_y * 2) | lr)) & 0xFFFF];
   }
   SourceY = sy;
   tgt[n] = fb_eye + y * 512;
   cv[n] = cover->row[y];
   cnt[n] = &cover->covered[y];
   trow[n] = vbgo_tt_row(tgt[n]);
   crow[n] = vbgo_tt_cell_row(tgt[n]);
   sy7[n] = sy & 7;
   n++;
  }
  if(!n)
   break;

  BGMap_Base = (a->bgmap_base << 12) | (((SourceY >> 3) & 0x3F) * 0x40) | (((SourceY << 3) & ~0xFFF) << a->scx);
  USE_ROW(a->overplane_char);
  {
   int xs;
   for(xs = 0; xs < (1 << a->scx); xs++)
    USE_ROW(BGMap_Base | (xs << 12));
  }

  x = start_x;
  sx = (uint32)(int32)(int16)row_srcX + SourceX_shift;
  while(x <= final_x)
  {
   unsigned cell, offset, run, i;
   uint32 bgsc, char_no;
   sx &= SourceX_Mask;
   offset = sx & 7;
   run = 8 - offset;
   if(run > (unsigned int)(final_x - x + 1))
    run = final_x - x + 1;
   /* (a tile covered in every row is stepped over before its map entry is read) */
   if(VbgoRowsCovered(cv, n, x, run))
   {
    VbgoFrontSkipped += n;
    x += run;
    sx += run;
    continue;
   }
   if(SourceX_Size > sx && SourceY_Size > SourceY)
   {
    cell = (BGMap_Base | ((sx << 3) & ~0xFFF) | ((sx >> 3) & 0x3F)) & 0xFFFF;
    bgsc = BGMap[cell];
   }
   else
   {
    cell = a->overplane_char & 0xFFFF;
    bgsc = bgsc_overplane;
   }
   char_no = bgsc & 0x7FF;
   if(char_no != used_chr)
   {
    USE_BG_CHR(char_no);
    used_chr = char_no;
   }
   if(vbgo_tt_blank[char_no])
   {
    /* (a character that has no pixel can't fill either: it and the tiles after it with the same entry draw nothing) */
    uint32 blank_w[4];
    memcpy(blank_w, &CHR16[char_no * 8], 16);
    if(!(blank_w[0] | blank_w[1] | blank_w[2] | blank_w[3]))
    {
     x += run;
     sx += run;
     while(x + 7 <= final_x)
     {
      const uint32 s2 = sx & SourceX_Mask;
      const uint32 entry = SourceX_Size > s2 && SourceY_Size > SourceY ?
                           BGMap[(BGMap_Base | ((s2 << 3) & ~0xFFF) | ((s2 >> 3) & 0x3F)) & 0xFFFF] : bgsc_overplane;
      if(entry != bgsc)
       break;
      x += 8;
      sx += 8;
     }
     continue;
    }
   }
   if(run == 8)
   {
    const int flipped = (bgsc & 0x2000) != 0;
    const uint32 palette_selector = bgsc >> 14;
    const int fills_here = vbgo_tt_fills_from(char_no, cell);
    const uint16 *const chr_rows = &CHR16[char_no * 8];
    const uint32_t tile_bits = vbgo_bits | VBGO_TT_BG_BITS(char_no, 0, 0, palette_selector, 0);
    int tile_tagged = 0, tile_fills = 0;
    for(i = 0; i < (unsigned)n; i++)
    {
     const unsigned char_sub_y = ((bgsc & 0x1000) ? 7 : 0) ^ sy7[i];
     const uint32 pixels = chr_rows[char_sub_y];
     uint32 c0, c1;
     memcpy(&c0, cv[i] + x, 4);
     memcpy(&c1, cv[i] + x + 4, 4);
     if((c0 & c1) == 0xFFFFFFFFu)
     {
      ++VbgoFrontSkipped;
      continue;
     }
     ++VbgoFrontRead;
     if(pixels || fills_here)
     {
      const int opaque = ((pixels | (pixels >> 1)) & 0x5555) == 0x5555;
      const int fills = fills_here && !opaque;
      const uint32_t tbits = tile_bits | char_sub_y << 14;
      uint32 p = pixels, shades[2], drawn[2], row[2];
      if(flipped)
      {
       p = ((p >> 2) & 0x3333) | ((p & 0x3333) << 2);
       p = ((p >> 4) & 0x0F0F) | ((p & 0x0F0F) << 4);
       p = ((p >> 8) & 0x00FF) | ((p & 0x00FF) << 8);
      }
      memcpy(&shades[0], GPLT_Quad[palette_selector][p & 0xFF], 4);
      memcpy(&shades[1], GPLT_Quad[palette_selector][p >> 8], 4);
      if(opaque && !(c0 | c1))
      {
       /* (every pixel drawn, nothing in front: straight-line; pixel k shows tile pixel k, or 7 - k flipped) */
       uint32_t *const tag = trow[i] + x * VBGO_TT_COLUMN;
       const uint32_t b = tbits | (flipped ? 7u << 11 : 0u);
       const int step = flipped ? -(1 << 11) : (1 << 11);
       const uint32 cells = (uint32)cell * 0x10001u, all = 0xFFFFFFFFu;
#define VBGO_FRONT_TAG(k) tag[(k) * VBGO_TT_COLUMN] = (b + (uint32_t)((k) * step)) | ((p >> ((k) * 2)) & 3) << 20;
       VBGO_FRONT_TAG(0) VBGO_FRONT_TAG(1) VBGO_FRONT_TAG(2) VBGO_FRONT_TAG(3)
       VBGO_FRONT_TAG(4) VBGO_FRONT_TAG(5) VBGO_FRONT_TAG(6) VBGO_FRONT_TAG(7)
#undef VBGO_FRONT_TAG
       memcpy(crow[i] + x * VBGO_TT_CELL_COLUMN, &cells, 4);
       memcpy(crow[i] + x * VBGO_TT_CELL_COLUMN + 2, &cells, 4);
       memcpy(crow[i] + x * VBGO_TT_CELL_COLUMN + 4, &cells, 4);
       memcpy(crow[i] + x * VBGO_TT_CELL_COLUMN + 6, &cells, 4);
       memcpy(tgt[i] + x, &shades[0], 4);
       memcpy(tgt[i] + x + 4, &shades[1], 4);
       memcpy(cv[i] + x, &all, 4);
       memcpy(cv[i] + x + 4, &all, 4);
       *cnt[i] += 8;
       tile_tagged = 1;
      }
      else
      {
       if(!(c0 | c1))
        tile_tagged |= vbgo_tt_tag_row8(trow[i] + x * VBGO_TT_COLUMN, crow[i] + x * VBGO_TT_CELL_COLUMN, pixels,
                                        flipped, tbits, char_no, cell) != 0;
       else
        tile_tagged |= VbgoTagRow8Front(trow[i] + x * VBGO_TT_COLUMN, crow[i] + x * VBGO_TT_CELL_COLUMN, cv[i] + x,
                                        pixels, flipped, tbits, fills, cell);
       tile_fills |= fills;
       memcpy(&drawn[0], OpaqueQuad[p & 0xFF], 4);
       memcpy(&drawn[1], OpaqueQuad[p >> 8], 4);
       if(pixels)
       {
        /* The shades go in where this row draws and nothing in front has: a cover byte of 0xFF, halved and doubled,
         * fills its byte, a fill's 1 doesn't. */
        const uint32 m0 = (c0 >> 1) & 0x7F7F7F7Fu, m1 = (c1 >> 1) & 0x7F7F7F7Fu;
        const uint32 own0 = drawn[0] & ~(m0 | m0 << 1), own1 = drawn[1] & ~(m1 | m1 << 1);
        memcpy(&row[0], tgt[i] + x, 4);
        memcpy(&row[1], tgt[i] + x + 4, 4);
        row[0] = (row[0] & ~own0) | (shades[0] & own0);
        row[1] = (row[1] & ~own1) | (shades[1] & own1);
        memcpy(tgt[i] + x, &row[0], 4);
        memcpy(tgt[i] + x + 4, &row[1], 4);
       }
       {
        const uint32 n0 = c0 | drawn[0] | (fills ? ~drawn[0] & 0x01010101u : 0u);
        const uint32 n1 = c1 | drawn[1] | (fills ? ~drawn[1] & 0x01010101u : 0u);
        *cnt[i] += VbgoGained(c0, n0) + VbgoGained(c1, n1);
        memcpy(cv[i] + x, &n0, 4);
        memcpy(cv[i] + x + 4, &n1, 4);
       }
      }
     }
    }
    if(tile_tagged)
     vbgo_tt_wcells[x >> 3] |= vbgo_wbit, vbgo_tt_wcells[(x + 7) >> 3] |= vbgo_wbit;
    if(tile_fills)
     vbgo_tt_fillc[x >> 3] = vbgo_tt_fillc[(x + 7) >> 3] = 1;
   }
   else
   {
    /* (the rest of this character's row inside the line, or what is left of the line: one run) */
    const int fills_here = vbgo_tt_fills_from(char_no, cell);
    const uint32 palette_selector = bgsc >> 14;
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
     VbgoRunFront(tgt[i], cv[i], cnt[i], x, offset, run, p, bgsc,
                  vbgo_bits | VBGO_TT_BG_BITS(char_no, 0, char_sub_y, palette_selector, 0), cell, fills_here, trow[i],
                  crow[i], vbgo_wbit);
    }
   }
   x += run;
   sx += run;
  }
 }
}

/* DrawOBJ for one row of both eyes, the sprites last to first (the last drawn is in front), each pixel only where
 * nothing in front has drawn. */
static VBGO_NOINLINE void DrawOBJFront(uint8 *fb[2], VbgoCover *cover, int y, uint16 Y, const bool lron[2],
                                       const uint16 *list, int count)
{
 const uint16 *CHR16 = CHR_RAM;
 int k;

 for(k = count - 1; k >= 0; k--)
 {
  const int32 oam = list[k];
  const uint16 *oam_ptr = &DRAM[(0x1E000 + (oam * 8)) >> 1];
  const uint32 tile_y = (Y - oam_ptr[2]) & 0xFF;
  const uint32 jp = ParallaxDisabled ? 0 : (oam_ptr[1] & 0x3FFF);
  const uint32 palette_selector = oam_ptr[3] >> 14;
  const uint32 char_sub_y = ((oam_ptr[3] & 0x1000) ? 7 : 0) ^ tile_y;
  const uint32 char_no = oam_ptr[3] & 0x7FF;
  const int flipped = (oam_ptr[3] & 0x2000) != 0;
  uint32 pixels;
  int lr;

  if(tile_y >= 8)
   continue;
  pixels = CHR16[char_no * 8 + char_sub_y];
  USE_CHR(char_no);
  if(!pixels)
   continue;
  for(lr = 0; lr < 2; lr++)
  {
   const int32 x = sign_x_to_s32(10, (oam_ptr[0] + (lr ? jp : -jp)));
   uint8 *const row = fb[lr], *const cv = cover[lr].row[y];
   uint16 *const covered = &cover[lr].covered[y];

   if(!(lron[lr] && (oam_ptr[1] & (lr ? 0x4000 : 0x8000))) || *covered >= ClipX1 - ClipX0 + 1 || x < -7 ||
      x >= 384 || x + 7 < ClipX0 || x > ClipX1)
    continue;
   {
    const uint8 *const plt = JPLT_Cache[palette_selector];
    uint32_t *const trow = vbgo_tt_row(row);
    uint16_t *const crow = vbgo_tt_cell_row(row);
    const uint32_t obits = vbgo_tt_call_bits() | char_no | (char_sub_y << 14) | (palette_selector << 17) | (1u << 19);
    const uint16_t ocell = VBGO_TT_OBJ_CELL(oam);
    int any = 0, n;

    if(x >= ClipX0 && x + 7 <= ClipX1)
    {
     uint32 c0, c1;
     memcpy(&c0, cv + x, 4);
     memcpy(&c1, cv + x + 4, 4);
     if((c0 & c1) == 0xFFFFFFFFu)
      continue;
     if(!((c0 | c1) & 0x80808080u))
     {
      /* (none of the 8 covered: all the row's pixels, as they show from the left) */
      uint32 p = pixels, d0, d1, n0, n1;
      uint32_t *const tag = trow + x * VBGO_TT_COLUMN;
      if(flipped)
      {
       p = ((p >> 2) & 0x3333) | ((p & 0x3333) << 2);
       p = ((p >> 4) & 0x0F0F) | ((p & 0x0F0F) << 4);
       p = ((p >> 8) & 0x00FF) | ((p & 0x00FF) << 8);
      }
      if(((pixels | (pixels >> 1)) & 0x5555) == 0x5555)
      {
       const uint32_t b = obits | (flipped ? 7u << 11 : 0u);
       const int step = flipped ? -(1 << 11) : (1 << 11);
       const uint32 cells = (uint32)ocell * 0x10001u;
#define VBGO_FRONT_OBJ(j) \
 { \
  const uint32 pv = (p >> ((j) * 2)) & 3; \
  row[x + (j)] = plt[pv]; \
  tag[(j) * VBGO_TT_COLUMN] = (b + (uint32_t)((j) * step)) | pv << 20; \
 }
       VBGO_FRONT_OBJ(0) VBGO_FRONT_OBJ(1) VBGO_FRONT_OBJ(2) VBGO_FRONT_OBJ(3)
       VBGO_FRONT_OBJ(4) VBGO_FRONT_OBJ(5) VBGO_FRONT_OBJ(6) VBGO_FRONT_OBJ(7)
#undef VBGO_FRONT_OBJ
       memcpy(crow + x * VBGO_TT_CELL_COLUMN, &cells, 4);
       memcpy(crow + x * VBGO_TT_CELL_COLUMN + 2, &cells, 4);
       memcpy(crow + x * VBGO_TT_CELL_COLUMN + 4, &cells, 4);
       memcpy(crow + x * VBGO_TT_CELL_COLUMN + 6, &cells, 4);
       d0 = d1 = 0xFFFFFFFFu;
      }
      else
      {
       for(n = 0; n < 8; n++)
       {
        const uint32 pv = (p >> (2 * n)) & 3;
        if(pv)
        {
         row[x + n] = plt[pv];
         tag[n * VBGO_TT_COLUMN] = obits | (uint32_t)(flipped ? 7 - n : n) << 11 | pv << 20;
         crow[(x + n) * VBGO_TT_CELL_COLUMN] = ocell;
        }
       }
       memcpy(&d0, OpaqueQuad[p & 0xFF], 4);
       memcpy(&d1, OpaqueQuad[p >> 8], 4);
      }
      n0 = c0 | d0;
      n1 = c1 | d1;
      memcpy(cv + x, &n0, 4);
      memcpy(cv + x + 4, &n1, 4);
      *covered += VbgoGained(c0, n0) + VbgoGained(c1, n1);
      vbgo_tt_ocells[x >> 3] = vbgo_tt_ocells[(x + 7) >> 3] = 1;
      continue;
     }
    }
    for(n = 0; n < 8; n++)
    {
     const uint32 pv = (pixels >> (2 * n)) & 3;
     const int32 at = x + (flipped ? 7 - n : n);
     if(pv && at >= ClipX0 && at <= ClipX1 && cv[at] != 0xFF)
     {
      row[at] = plt[pv];
      trow[at * VBGO_TT_COLUMN] = obits | (uint32_t)n << 11 | pv << 20;
      crow[at * VBGO_TT_CELL_COLUMN] = ocell;
      cv[at] = 0xFF;
      ++*covered;
      any = 1;
     }
    }
    if(any)
     vbgo_tt_ocells[x > 0 ? x >> 3 : 0] = vbgo_tt_ocells[x + 7 < 384 ? (x + 7) >> 3 : 47] = 1;
   }
  }
 }
}

/* Front to back costs a little more for each group it draws and saves each one covered, but DrawBG draws an overplane
 * world a row at a time. A block without one that had under 2 in 5 of its groups covered is drawn the usual way for
 * its next 8 passes of its buffer, then tried again. */
static uint8 VbgoFrontWait[2][28];

static INLINE bool VbgoFrontTry(unsigned block_no, bool overplane)
{
 uint8 *const wait = &VbgoFrontWait[DrawingFB][block_no];
 if(*wait && !overplane)
 {
  --*wait;
  return false;
 }
 VbgoFrontSkipped = VbgoFrontRead = 0;
 return true;
}

static INLINE void VbgoFrontDone(unsigned block_no, bool overplane)
{
 if(!overplane && VbgoFrontSkipped * 3 < VbgoFrontRead * 2)
  VbgoFrontWait[DrawingFB][block_no] = 8;
}

/* A block's worlds front to back (none that can draw in it is affine: DrawAffine draws every pixel of its line). */
static VBGO_NOINLINE void VbgoDrawFront(uint8 block_no, uint8 *fb_l, uint8 *fb_r, const VbgoWorldAttr *attrs,
                                        int drawn_count, uint16 (*obj_lists)[1024], int obj_lists_used,
                                        const int *counts)
{
 static VbgoCover cover[2];
 int k, y, lr, obj_index = obj_lists_used;

 for(lr = 0; lr < 2; lr++)
  for(y = 0; y < 8; y++)
  {
   memset(&cover[lr].row[y][ClipX0], 0, ClipX1 - ClipX0 + 1);
   cover[lr].covered[y] = 0;
  }
 for(k = drawn_count - 1; k >= 0; k--)
 {
  const VbgoWorldAttr *const a = &attrs[k];

  vbgo_tt_world = a->tt_world;
  if(a->bgm == BGM_OBJ)
  {
   const uint16 *const list = obj_lists[--obj_index];
   if(counts[k])
    for(y = 0; y < 8; y++)
    {
     uint8 *fb[2] = { &fb_l[y * 512], &fb_r[y * 512] };
     if(cover[0].covered[y] < ClipX1 - ClipX0 + 1 || cover[1].covered[y] < ClipX1 - ClipX0 + 1)
      DrawOBJFront(fb, cover, y, (block_no * 8) + y, a->lron, list, counts[k]);
    }
  }
  else if(a->bgm != BGM_AFFINE)
   for(lr = 0; lr < 2; lr++)
    if(a->lron[lr])
     DrawBGBlockFront(lr ? fb_r : fb_l, &cover[lr], block_no, lr, a);
 }
}
