/* A block's worlds front to back (VbgoDrawFront): a world draws only where none in front of it has, so the pixels,
 * tags and cells are the back to front order's. The cover, per pixel of an eye's rows: 0xFF drawn, 1 only a fill (the
 * backmost world's fill stays, so one replaces those in front), 0 nothing. */

#define VBGO_COVER_WIDTH 384
typedef struct
{
 uint8 row[8][VBGO_COVER_WIDTH];
 uint16 covered[8]; /* (pixels drawn per row: a row full from ClipX0 to ClipX1 leaves a world nothing to draw) */
 uint8 pad[16];     /* (the size a multiple of 32: every row starts on a cache line) */
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

/* A chunk's n rows: consecutive, sharing a map row, none outside the window or covered all along. They are the last n of 8
 * imagined rows, so the straight-line code for 8 finds each at a fixed offset: the pointers are the imagined first
 * row's, skip rows back from the chunk's first. */
typedef struct
{
 uint8 *tgt, *cv;
 uint16 *cnt;
 uint32_t *trow;
 uint16_t *crow;
 unsigned sy7; /* (the chunk's first row's y in its tiles) */
 int n, skip;
} VbgoFrontChunk;

/* A pointer some bytes back (maybe before its array: the imagined rows ahead of a chunk's first are never touched). */
#define VbgoBack(type, p, bytes) ((type)((uintptr_t)(p) - (uintptr_t)(bytes)))

/* Whether the run of pixels at x is covered in each of the chunk's rows (VbgoChunkCovered for a run shorter than 8). */
static VBGO_NOINLINE bool VbgoChunkCoveredRows(const VbgoFrontChunk *c, int x, unsigned run)
{
 const uint8 *const cov = c->cv + x;
 int v;
 unsigned j;
 for(v = c->skip; v < 8; v++)
  for(j = 0; j < run; j++)
   if(cov[v * VBGO_COVER_WIDTH + j] != 0xFF)
    return false;
 return true;
}

/* Whether the run of pixels at x is covered in each of the chunk's rows. */
static INLINE bool VbgoChunkCovered(const VbgoFrontChunk *c, int x, unsigned run)
{
 const uint8 *const cov = c->cv + x;
 if(run != 8)
  return VbgoChunkCoveredRows(c, x, run);
 /* (every chunk ends at imagined row 7, and most tiles aren't covered there) */
 if(!VbgoCovered8(cov + 7 * VBGO_COVER_WIDTH))
  return false;
#define VBGO_ROW_COVERED(v) case v: if(!VbgoCovered8(cov + (v) * VBGO_COVER_WIDTH)) return false;
 switch(c->skip)
 {
 VBGO_ROW_COVERED(0) VBGO_ROW_COVERED(1) VBGO_ROW_COVERED(2) VBGO_ROW_COVERED(3)
 VBGO_ROW_COVERED(4) VBGO_ROW_COVERED(5) VBGO_ROW_COVERED(6)
 }
#undef VBGO_ROW_COVERED
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

/* One row (imagined row v) of a whole tile (8 pixels from x) that can leave fills and isn't covered or straight-line
 * (see VbgoFrontTiles). Bit 0 of the result: it tagged a pixel, bit 1: it may have left fills. */
static VBGO_NOINLINE unsigned VbgoFrontRow8(const VbgoFrontChunk *c, int v, int x, uint32 bgsc, unsigned cell)
{
 uint8 *const tgt = c->tgt + v * 512, *const cv = c->cv + v * VBGO_COVER_WIDTH;
 uint16 *const cnt = c->cnt + v;
 uint32_t *const trow = c->trow + v;
 uint16_t *const crow = c->crow + v * VBGO_TT_CELL_ROW;
 const uint32 char_no = bgsc & 0x7FF, palette_selector = bgsc >> 14;
 const int flipped = (bgsc & 0x2000) != 0;
 const unsigned char_sub_y = ((bgsc & 0x1000) ? 7 : 0) ^ (c->sy7 + v - c->skip);
 const uint32 pixels = CHR_RAM[char_no * 8 + char_sub_y];
 const uint32_t tbits = vbgo_tt_call_bits() | VBGO_TT_BG_BITS(char_no, 0, char_sub_y, palette_selector, 0);
 uint32 c0, c1, p = pixels, shades[2], drawn[2], row[2];
 unsigned flags = 0;
 int opaque, fills;
 memcpy(&c0, cv + x, 4);
 memcpy(&c1, cv + x + 4, 4);
 opaque = ((pixels | (pixels >> 1)) & 0x5555) == 0x5555;
 fills = !opaque;
 if(flipped)
 {
  p = ((p >> 2) & 0x3333) | ((p & 0x3333) << 2);
  p = ((p >> 4) & 0x0F0F) | ((p & 0x0F0F) << 4);
  p = ((p >> 8) & 0x00FF) | ((p & 0x00FF) << 8);
 }
 memcpy(&shades[0], GPLT_Quad[palette_selector][p & 0xFF], 4);
 memcpy(&shades[1], GPLT_Quad[palette_selector][p >> 8], 4);
 if(!(c0 | c1))
  flags = vbgo_tt_tag_row8_fills(trow + x * VBGO_TT_COLUMN, crow + x * VBGO_TT_CELL_COLUMN, pixels, flipped, tbits, cell,
                                 1) != 0;
 else
  flags = VbgoTagRow8Front(trow + x * VBGO_TT_COLUMN, crow + x * VBGO_TT_CELL_COLUMN, cv + x, pixels, flipped, tbits,
                           fills, cell);
 if(fills)
  flags |= 2;
 memcpy(&drawn[0], OpaqueQuad[p & 0xFF], 4);
 memcpy(&drawn[1], OpaqueQuad[p >> 8], 4);
 if(pixels)
 {
  /* The shades go in where this row draws and nothing in front has: a cover byte of 0xFF, halved and doubled,
   * fills its byte, a fill's 1 doesn't. */
  const uint32 m0 = (c0 >> 1) & 0x7F7F7F7Fu, m1 = (c1 >> 1) & 0x7F7F7F7Fu;
  const uint32 own0 = drawn[0] & ~(m0 | m0 << 1), own1 = drawn[1] & ~(m1 | m1 << 1);
  memcpy(&row[0], tgt + x, 4);
  memcpy(&row[1], tgt + x + 4, 4);
  row[0] = (row[0] & ~own0) | (shades[0] & own0);
  row[1] = (row[1] & ~own1) | (shades[1] & own1);
  memcpy(tgt + x, &row[0], 4);
  memcpy(tgt + x + 4, &row[1], 4);
 }
 {
  const uint32 n0 = c0 | drawn[0] | (fills ? ~drawn[0] & 0x01010101u : 0u);
  const uint32 n1 = c1 | drawn[1] | (fills ? ~drawn[1] & 0x01010101u : 0u);
  *cnt += VbgoGained(c0, n0) + VbgoGained(c1, n1);
  memcpy(cv + x, &n0, 4);
  memcpy(cv + x + 4, &n1, 4);
 }
 return flags;
}

/* A whole tile of a chunk (8 pixels from x, its map entry and cell). */
typedef struct
{
 int x;
 uint32 bgsc;
 unsigned cell;
} VbgoFrontTile;

/* Byte j of a word as memory holds it (pixel j of a 4-pixel group). */
#ifdef MSB_FIRST
#define VBGO_BYTE(j) (0xFFu << (24 - 8 * (j)))
#else
#define VBGO_BYTE(j) (0xFFu << (8 * (j)))
#endif

/* Whether a tile can leave no fills, worked out the first time a row asks (-1: not yet). */
static INLINE bool VbgoNoFills(int *fills_here, uint32 bgsc, unsigned cell)
{
 if(*fills_here < 0)
  *fills_here = vbgo_tt_fills_from(bgsc & 0x7FF, cell);
 return !*fills_here;
}

/* Tag pixel k of imagined row v of a tile drawn whole: b is the row's tag bits (in-tile x 0, or 7 flipped: k ^ 7). */
#define VBGO_FRONT_TAG(v, k) \
 tag[(v) + (k) * VBGO_TT_COLUMN] = ((b ^ (uint32_t)(k) << 11) & ~(3u << 20)) | ((p << (20 - 2 * (k))) & (3u << 20)); \
 VBGO_ORDER;

/* Tag pixel k of imagined row v and store its cell, if own (its group's word: byte j) says the row draws it. */
#define VBGO_SLOW_TAG(v, own, j, k) \
 if((own) & VBGO_BYTE(j)) \
 { \
  tag[(v) + (k) * VBGO_TT_COLUMN] = ((b ^ (uint32_t)(k) << 11) & ~(3u << 20)) | ((p << (20 - 2 * (k))) & (3u << 20)); \
  crow[(v) * VBGO_TT_CELL_ROW + (k)] = (uint16_t)cell; \
 }

/* Imagined row v of a tile that leaves no fills, not straight-line: the drawn pixels nothing in front has drawn. */
#define VBGO_SLOW_ROW(v) \
 { \
  uint32 p = pixels, d0, d1, f0, f1, own0, own1; \
  slow_rows++; \
  if(pixels) \
  { \
   if(flipped) \
   { \
    p = ((p >> 2) & 0x3333) | ((p & 0x3333) << 2); \
    p = ((p >> 4) & 0x0F0F) | ((p & 0x0F0F) << 4); \
    p = ((p >> 8) & 0x00FF) | ((p & 0x00FF) << 8); \
   } \
   memcpy(&d0, OpaqueQuad[p & 0xFF], 4); \
   memcpy(&d1, OpaqueQuad[p >> 8], 4); \
   /* (as a mask, f | f >> 1 keeps a cover byte of 0xFF and drops a fill's 1: the row draws over fills) */ \
   f0 = c0 & 0xFEFEFEFEu; \
   f1 = c1 & 0xFEFEFEFEu; \
   own0 = d0 & ~(f0 | f0 >> 1); \
   own1 = d1 & ~(f1 | f1 >> 1); \
   if(own0 | own1) \
   { \
    uint32_t b = bits; \
    uint32 s0, s1, r0, r1; \
    VBGO_HIDE(b); \
    VBGO_SLOW_TAG(v, own0, 0, 0) VBGO_SLOW_TAG(v, own0, 1, 1) VBGO_SLOW_TAG(v, own0, 2, 2) \
    VBGO_SLOW_TAG(v, own0, 3, 3) VBGO_SLOW_TAG(v, own1, 0, 4) VBGO_SLOW_TAG(v, own1, 1, 5) \
    VBGO_SLOW_TAG(v, own1, 2, 6) VBGO_SLOW_TAG(v, own1, 3, 7) \
    memcpy(&s0, quad[p & 0xFF], 4); \
    memcpy(&s1, quad[p >> 8], 4); \
    memcpy(&r0, tgt + (v) * 512, 4); \
    memcpy(&r1, tgt + (v) * 512 + 4, 4); \
    r0 = (r0 & ~own0) | (s0 & own0); \
    r1 = (r1 & ~own1) | (s1 & own1); \
    memcpy(tgt + (v) * 512, &r0, 4); \
    memcpy(tgt + (v) * 512 + 4, &r1, 4); \
    c0 |= own0; \
    c1 |= own1; \
    memcpy(cov + (v) * VBGO_COVER_WIDTH, &c0, 4); \
    memcpy(cov + (v) * VBGO_COVER_WIDTH + 4, &c1, 4); \
    cnt[v] += ((own0 & 0x01010101u) + (own1 & 0x01010101u)) * 0x01010101u >> 24; \
    tagged |= 1; \
   } \
  } \
 }

/* Imagined row v of a chunk's tile: straight-line if every pixel is drawn and nothing in front has drawn any, else
 * VBGO_SLOW_ROW if the tile leaves no fills, else left for VbgoFrontRow8 (and counted if covered). */
#define VBGO_FRONT_ROW(v) \
 { \
  const uint32 pixels = *(const uint16 *)chr; \
  uint32 c0, c1; \
  memcpy(&c0, cov + (v) * VBGO_COVER_WIDTH, 4); \
  memcpy(&c1, cov + (v) * VBGO_COVER_WIDTH + 4, 4); \
  if((c0 & c1) == 0xFFFFFFFFu) \
   covered_rows++; \
  else if(((~(pixels | (pixels >> 1)) & 0x5555u) | c0 | c1) == 0) /* (every pixel drawn, nothing in front) */ \
  { \
   uint32 p = pixels, shades0, shades1; \
   uint32_t b = bits; \
   const uint32 all = 0xFFFFFFFFu; \
   if(flipped) \
   { \
    p = ((p >> 2) & 0x3333) | ((p & 0x3333) << 2); \
    p = ((p >> 4) & 0x0F0F) | ((p & 0x0F0F) << 4); \
    p = ((p >> 8) & 0x00FF) | ((p & 0x00FF) << 8); \
   } \
   VBGO_HIDE(b); \
   VBGO_FRONT_TAG(v, 0) VBGO_FRONT_TAG(v, 1) VBGO_FRONT_TAG(v, 2) VBGO_FRONT_TAG(v, 3) \
   VBGO_FRONT_TAG(v, 4) VBGO_FRONT_TAG(v, 5) VBGO_FRONT_TAG(v, 6) VBGO_FRONT_TAG(v, 7) \
   memcpy(crow + (v) * VBGO_TT_CELL_ROW, &cells, 4); \
   memcpy(crow + (v) * VBGO_TT_CELL_ROW + 2, &cells, 4); \
   memcpy(crow + (v) * VBGO_TT_CELL_ROW + 4, &cells, 4); \
   memcpy(crow + (v) * VBGO_TT_CELL_ROW + 6, &cells, 4); \
   VBGO_ORDER; \
   memcpy(&shades0, quad[p & 0xFF], 4); \
   memcpy(&shades1, quad[p >> 8], 4); \
   memcpy(tgt + (v) * 512, &shades0, 4); \
   memcpy(tgt + (v) * 512 + 4, &shades1, 4); \
   memcpy(cov + (v) * VBGO_COVER_WIDTH, &all, 4); \
   memcpy(cov + (v) * VBGO_COVER_WIDTH + 4, &all, 4); \
   VBGO_ORDER; \
   cnt[v] += 8; \
  } \
  else if(VbgoNoFills(&fills_here, bgsc, cell)) \
   VBGO_SLOW_ROW(v) \
  else \
   later |= 1u << (v); \
  chr += chr_step; \
  bits += bits_step; \
 }

/* A chunk's whole tiles: the rows' groups nothing in front has covered. */
static VBGO_NOINLINE void VbgoFrontTiles(const VbgoFrontChunk *c, const VbgoFrontTile *tile, int count)
{
 const VbgoFrontTile *const end = tile + count;
 unsigned covered_total = 0;

 for(; tile != end; tile++)
 {
  const int x = tile->x;
  const uint32 bgsc = tile->bgsc;
  const unsigned cell = tile->cell;
  const uint32 palette_selector = bgsc >> 14;
  const uint8 (*const quad)[4] = GPLT_Quad[palette_selector];
  const int flipped = (bgsc & 0x2000) != 0;
  const int vflip = (bgsc & 0x1000) != 0;
  const unsigned first_y = vflip ? 7 - c->sy7 : c->sy7;
  const uint8 *chr = (const uint8 *)&CHR_RAM[(bgsc & 0x7FF) * 8 + first_y];
  const int chr_step = vflip ? -2 : 2;
  /* (the tile's first row's tag bits: in-tile y first_y, in-tile x 0, or 7 flipped; and what each next row changes) */
  uint32_t bits = vbgo_tt_call_bits() | VBGO_TT_BG_BITS(bgsc & 0x7FF, 0, first_y, palette_selector, 0) |
                  (flipped ? 7u << 11 : 0u);
  const int bits_step = vflip ? -(1 << 14) : 1 << 14;
  const uint32 cells = (uint32)cell * 0x10001u;
  uint8 *const tgt = c->tgt + x, *const cov = c->cv + x;
  uint32_t *const tag = c->trow + x * VBGO_TT_COLUMN;
  uint16_t *const crow = c->crow + x * VBGO_TT_CELL_COLUMN;
  uint16 *const cnt = c->cnt;
  unsigned later = 0, covered_rows = 0, tagged = 0, slow_rows = 0;
  int fills_here = -1;

  switch(c->skip)
  {
  case 0: VBGO_FRONT_ROW(0)
  case 1: VBGO_FRONT_ROW(1)
  case 2: VBGO_FRONT_ROW(2)
  case 3: VBGO_FRONT_ROW(3)
  case 4: VBGO_FRONT_ROW(4)
  case 5: VBGO_FRONT_ROW(5)
  case 6: VBGO_FRONT_ROW(6)
  case 7: VBGO_FRONT_ROW(7)
  }
  covered_total += covered_rows;
  /* (only rows of a tile that can leave fills) */
  for(; later; later &= later - 1)
  {
   tagged |= VbgoFrontRow8(c, __builtin_ctz(later), x, bgsc, cell);
   slow_rows++;
  }
  if(covered_rows + slow_rows != (unsigned)c->n)
   tagged |= 1;
  if(tagged & 1)
  {
   const uint32 wbit = 1u << VBGO_TAG_WORLD(vbgo_tt_call_bits());
   vbgo_tt_wcells[x >> 3] |= wbit, vbgo_tt_wcells[(x + 7) >> 3] |= wbit;
  }
  if(tagged & 2)
   vbgo_tt_fillc[x >> 3] = vbgo_tt_fillc[(x + 7) >> 3] = 1;
 }
 VbgoFrontSkipped += covered_total;
 VbgoFrontRead += c->n * count - covered_total;
}
#undef VBGO_FRONT_ROW
#undef VBGO_SLOW_ROW
#undef VBGO_SLOW_TAG
#undef VBGO_FRONT_TAG
#undef VBGO_BYTE

/* DrawBGBlock for the front to back order, and for the H-bias and overplane worlds DrawBG draws: only the pixels
 * nothing in front has drawn. */
static VBGO_NOINLINE void DrawBGBlockFront(uint8 *fb_eye, VbgoCover *cover, unsigned block_no, bool lr,
                                           const VbgoWorldAttr *a)
{
 const uint16 *CHR16 = CHR_RAM;
 const uint16 *BGMap = DRAM;
 const uint32 SourceX_Size = 512 * (1u << a->scx), SourceY_Size = 512 * (1u << a->scy);
 const uint32 SourceX_Mask = a->over ? 0x1FFF : SourceX_Size - 1, SourceY_Mask = a->over ? 0x1FFF : SourceY_Size - 1;
 const bool inside = !a->over; /* (masked to the map's size, every source pixel is in it) */
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
  VbgoFrontChunk chunk;
  VbgoFrontTile tiles[VBGO_COVER_WIDTH / 8 + 1], *tile = tiles;
  int n = 0, first_y = 0;
  uint16 row_srcX = srcX;
  uint32 SourceY = 0, BGMap_Base, used_chr = 0xFFFF;
  int32 x;
  uint32 sx;
  /* (the next rows that share a map row, but not those outside the window or covered all along: nothing to draw; a
   * covered row ends the chunk, and the rows after it are the next) */
  for(; y < 8; y++)
  {
   const uint16 row_y = (uint16)((block_no * 8 + y) - a->gy);
   const uint16 srcY = a->my + row_y;
   const uint32 sy = ((uint32)(int32)(int16)srcY) & SourceY_Mask;
   if(row_y > a->window_height)
    continue;
   if(cover->covered[y] >= clip_width)
   {
    if(n)
     break;
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
   if(!n)
    first_y = y, chunk.sy7 = sy & 7;
   n++;
  }
  if(!n)
   break;
  {
   /* (the imagined first row's pointers: skip rows back from the first row's) */
   uint8 *const tgt = fb_eye + first_y * 512;
   const int skip = 8 - n;
   chunk.n = n;
   chunk.skip = skip;
   chunk.tgt = VbgoBack(uint8 *, tgt, skip * 512);
   chunk.cv = VbgoBack(uint8 *, cover->row[first_y], skip * VBGO_COVER_WIDTH);
   chunk.cnt = VbgoBack(uint16 *, &cover->covered[first_y], skip * sizeof(uint16));
   chunk.trow = VbgoBack(uint32_t *, vbgo_tt_row(tgt), skip * sizeof(uint32_t));
   chunk.crow = VbgoBack(uint16_t *, vbgo_tt_cell_row(tgt), skip * VBGO_TT_CELL_ROW * sizeof(uint16_t));
  }

  BGMap_Base = (a->bgmap_base << 12) | (((SourceY >> 3) & 0x3F) * 0x40) | (((SourceY << 3) & ~0xFFF) << a->scx);
  if(a->over)
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
   if(VbgoChunkCovered(&chunk, x, run))
   {
    VbgoFrontSkipped += n;
    x += run;
    sx += run;
    continue;
   }
   if(inside || (SourceX_Size > sx && SourceY_Size > SourceY))
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
      const uint32 s2 = sx & SourceX_Mask, column = (s2 >> 3) & 0x3F;
      const uint16 *entry;
      int32 last, from;
      if(!inside && !(SourceX_Size > s2 && SourceY_Size > SourceY))
      {
       if(bgsc_overplane != bgsc)
        break;
       x += 8;
       sx += 8;
       continue;
      }
      /* (the next tiles' entries are the next halfwords, to the end of the map's row or of the line) */
      entry = &BGMap[(BGMap_Base | ((s2 << 3) & ~0xFFF) | column) & 0xFFFF];
      last = x + 8 * (int32)(63 - column);
      if(last > final_x - 7)
       last = final_x - 7;
      from = x;
      while(x <= last && *entry == bgsc)
       entry++, x += 8;
      sx += x - from;
      if(x <= last)
       break;
     }
     continue;
    }
   }
   if(run == 8)
   {
    tile->x = x;
    tile->bgsc = bgsc;
    tile->cell = cell;
    tile++;
   }
   else
   {
    /* (the rest of this character's row inside the line, or what is left of the line: one run) */
    const int fills_here = vbgo_tt_fills_from(char_no, cell);
    const uint32 palette_selector = bgsc >> 14;
    for(i = 0; i < (unsigned)n; i++)
    {
     const int v = chunk.skip + i;
     const unsigned char_sub_y = ((bgsc & 0x1000) ? 7 : 0) ^ (chunk.sy7 + i);
     uint32 p = CHR16[char_no * 8 + char_sub_y];
     if(bgsc & 0x2000)
     {
      p = ((p >> 2) & 0x3333) | ((p & 0x3333) << 2);
      p = ((p >> 4) & 0x0F0F) | ((p & 0x0F0F) << 4);
      p = ((p >> 8) & 0x00FF) | ((p & 0x00FF) << 8);
     }
     p = (p >> (offset * 2)) & ((1u << (run * 2)) - 1);
     VbgoRunFront(chunk.tgt + v * 512, chunk.cv + v * VBGO_COVER_WIDTH, chunk.cnt + v, x, offset, run, p, bgsc,
                  vbgo_bits | VBGO_TT_BG_BITS(char_no, 0, char_sub_y, palette_selector, 0), cell, fills_here,
                  chunk.trow + v, chunk.crow + v * VBGO_TT_CELL_ROW, vbgo_wbit);
    }
   }
   x += run;
   sx += run;
  }
  if(tile != tiles)
   VbgoFrontTiles(&chunk, tiles, tile - tiles);
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
 * its next 32 passes, unless it gains a background world or a neighbor skipped a lot (what covers spans blocks). */
#define VBGO_FRONT_BIG 1024 /* (the groups skipped that suggest the neighbors will cover too) */

typedef struct
{
 uint32 retry; /* the pass it is tried again at */
 uint32 big;   /* the last pass it covered 2 in 5 or more and skipped VBGO_FRONT_BIG groups or more */
 uint8 worlds; /* the background worlds it had then */
} VbgoFrontMemo;
static VbgoFrontMemo VbgoFrontMemos[2][28];

static INLINE bool VbgoFrontBigNear(const VbgoFrontMemo *memo, unsigned block_no)
{
 return (block_no > 0 && PassNo - memo[-1].big <= 2) || (block_no < 27 && PassNo - memo[1].big <= 2);
}

static INLINE bool VbgoFrontTry(unsigned block_no, bool overplane, unsigned bg_worlds)
{
 const VbgoFrontMemo *const memo = &VbgoFrontMemos[DrawingFB][block_no];
 if(!overplane && (int32)(PassNo - memo->retry) < 0 && bg_worlds <= memo->worlds &&
    !VbgoFrontBigNear(memo, block_no))
  return false;
 VbgoFrontSkipped = VbgoFrontRead = 0;
 return true;
}

static INLINE void VbgoFrontDone(unsigned block_no, bool overplane, unsigned bg_worlds)
{
 VbgoFrontMemo *const memo = &VbgoFrontMemos[DrawingFB][block_no];
 if(overplane)
  return;
 if(VbgoFrontSkipped * 3 >= VbgoFrontRead * 2)
 {
  memo->retry = 0;
  if(VbgoFrontSkipped >= VBGO_FRONT_BIG)
   memo->big = PassNo;
 }
 else
  memo->retry = PassNo + 32;
 memo->worlds = bg_worlds;
}

/* A block's worlds front to back (none that can draw in it is affine: DrawAffine draws every pixel of its line). */
static VBGO_NOINLINE void VbgoDrawFront(uint8 block_no, uint8 *fb_l, uint8 *fb_r, const VbgoWorldAttr *attrs,
                                        int drawn_count, uint16 (*obj_lists)[1024], int obj_lists_used,
                                        const int *counts)
{
 static VbgoCover cover[2] __attribute__((aligned(32)));
 int k, y, lr, obj_index = obj_lists_used;

 for(lr = 0; lr < 2; lr++)
  for(y = 0; y < 8; y++)
  {
   /* (the drawn columns are whole cells of 8) */
   if((ClipX0 | (ClipX1 + 1)) & 7)
    memset(&cover[lr].row[y][ClipX0], 0, ClipX1 - ClipX0 + 1);
   else
    vbgo_fill8(&cover[lr].row[y][ClipX0], 0, (ClipX1 - ClipX0 + 1) / 8);
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
