/* VirtualBoyGo tile tracking (experimental): tags every pixel the VIP draws with the 8x8 character and BG map cell it
 * came from, so a frontend can recolor by tile (per-game color packs). Hooked in through generated copies of vip.c and
 * vip_draw.inc (cmake/PatchBeetleVip.cmake), so the submodule stays untouched; while off, it draws as upstream. */
#ifndef VBGO_TILETRACK_H
#define VBGO_TILETRACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VBGO_TT_WIDTH  384
#define VBGO_TT_HEIGHT 224
/* Tags per column in the tag buffer (rows 0-223 used): a band's 8 tags are one aligned 32-byte line, and
 * unlike 256 (a 1 KB stride) the columns spread over the Wii's 128 L1 sets. */
#define VBGO_TT_COLUMN 264
/* Each eye's cells row by row, as the VIP draws them (a 32-byte line is 16 columns of one row, so a row of drawing
 * touches few lines); rows padded to 25 lines (odd, so the rows spread over the L1 sets). */
#define VBGO_TT_CELL_COLUMN 1
#define VBGO_TT_CELL_ROW 400
/* Row y of a column's cells, from its first (cells[x] in the view, vbgo_tt_cell_row's row). */
static inline unsigned vbgo_tt_cell_at(unsigned y) { return y * VBGO_TT_CELL_ROW; }

/* Copies and fills of runs of doublewords (the memory 4-byte aligned at least, n constant where it can be). On the Wii
 * they go through the FPU's 8-byte registers, one load and one store a doubleword, where a memcpy or memset call costs
 * far more than the copy itself for these short runs. */
#ifdef GEKKO
typedef double __attribute__((may_alias, aligned(4))) vbgo_dword;
static inline void vbgo_copy8(void *dst, const void *src, unsigned n)
{
   vbgo_dword *const d = (vbgo_dword *)dst;
   const vbgo_dword *const s = (const vbgo_dword *)src;
   /* (spelled out, up to 8: as a loop the compiler turns it back into a memcpy call) */
   if (n > 0) d[0] = s[0];
   if (n > 1) d[1] = s[1];
   if (n > 2) d[2] = s[2];
   if (n > 3) d[3] = s[3];
   if (n > 4) d[4] = s[4];
   if (n > 5) d[5] = s[5];
   if (n > 6) d[6] = s[6];
   if (n > 7) d[7] = s[7];
}
static inline void vbgo_fill8(void *dst, unsigned byte, unsigned n)
{
   const uint32_t two[2] = { byte * 0x01010101u, byte * 0x01010101u };
   double d = *(const vbgo_dword *)two;
   unsigned i;
   __asm__("" : "+f"(d)); /* (kept in a floating-point register, not stored as two words) */
   for (i = 0; i < n; i++)
      ((vbgo_dword *)dst)[i] = d;
}
#else
static inline void vbgo_copy8(void *dst, const void *src, unsigned n) { memcpy(dst, src, n * 8); }
static inline void vbgo_fill8(void *dst, unsigned byte, unsigned n) { memset(dst, (int)byte, n * 8); }
#endif

/* ---- Tags (the tracker's own per-pixel format) -------------------------- */

/* Stored as a 32-bit tag (bits 0-27 below, 28-31 0) and a 16-bit cell; read as one value,
 * vbgo_tag_compose(tag, cell), which the macros take. */

#define VBGO_TAG_CHAR(t)     ((unsigned)(t) & 0x7FF)          /* character slot 0-2047 */
/* pixel in the tile: y * 8 + x (tile space, flips undone) */
#define VBGO_TAG_INDEX(t)    ((unsigned)((t) >> 11) & 63)
#define VBGO_TAG_SUBX(t)     ((unsigned)((t) >> 11) & 7)
#define VBGO_TAG_SUBY(t)     ((unsigned)((t) >> 14) & 7)
#define VBGO_TAG_PALETTE(t)  ((unsigned)((t) >> 17) & 3)      /* GPLT/JPLT palette number */
#define VBGO_TAG_IS_OBJ(t)   ((unsigned)((t) >> 19) & 1)      /* 1 = sprite (OBJ) */
#define VBGO_TAG_PIXEL(t)    ((unsigned)((t) >> 20) & 3)      /* raw 2-bit value; 0 = a fill (transparent pixel) */
#define VBGO_TAG_WORLD(t)    ((unsigned)((t) >> 22) & 31)     /* world (layer) that drew it */
#define VBGO_TAG_HAS_CELL(t) ((unsigned)((t) >> 27) & 1)      /* drawn from a BG map cell (not a sprite) */
#define VBGO_TAG_CELL(t)     ((unsigned)((t) >> 32) & 0xFFFF) /* that cell's halfword index in VIP DRAM */
#define VBGO_TAG_OBJ_NO(t)   ((unsigned)((t) >> 32) & 0x3FF)  /* sprites: the OBJ (0-1023) - same bits, no cell */

static inline uint64_t vbgo_tag_compose(uint32_t tag, uint16_t cell) { return (uint64_t)cell << 32 | tag; }

/* A run's key: the tag but for the pixel in the tile and its value (all of a run's tags share it). */
#define VBGO_TAG_RUN_BITS (0x7FFull | (3ull << 17) | (1ull << 19) | (31ull << 22) | (1ull << 27) | (0xFFFFull << 32))

/* A tag's words, for 32-bit compares: the high word is the cell (a run's share vbgo_tag_key_hi(key)), and
 * none is VBGO_TAG_NO_HI. A drawn tag's low word is never 0: it has the cell or sprite bit. */
static inline uint32_t vbgo_tag_hi(uint64_t t) { return (uint32_t)(t >> 32); }
static inline uint32_t vbgo_tag_lo(uint64_t t) { return (uint32_t)t; }
static inline uint32_t vbgo_tag_key_hi(uint64_t key) { return (uint32_t)(key >> 32); }
#define VBGO_TAG_NO_HI 0xFFFFFFFFu

/* Whether t was drawn by the pass, in a band whose key is band_key (0 if the pass didn't reach it). A block's tags are
 * cleared before it's drawn, so in a reached band a nonzero tag is one the pass drew. */
static inline int vbgo_tag_drawn(uint64_t t, uint32_t band_key) { return (vbgo_tag_lo(t) & band_key) != 0; }
#define VBGO_TT_BAND_REACHED 0xFFFFFFFFu

/* ---- Records (captures, .tiles files, debug tools) ----------------------- */

/* Field layout of one record (a tag resolved to the tile's content hash). */
#define VBGO_TT_HASH(v)    ((uint32_t)(v))              /* hash of the tile's 8x8 pixel data */
#define VBGO_TT_SUBX(v)    ((unsigned)((v) >> 32) & 7)  /* x inside the tile (tile space, flips undone) */
#define VBGO_TT_SUBY(v)    ((unsigned)((v) >> 35) & 7)  /* y inside the tile */
#define VBGO_TT_PALETTE(v) ((unsigned)((v) >> 38) & 3)  /* GPLT/JPLT palette number used */
#define VBGO_TT_IS_OBJ(v)  ((unsigned)((v) >> 40) & 1)  /* 1 = sprite (OBJ), 0 = background map */
#define VBGO_TT_PIXEL(v)   ((unsigned)((v) >> 41) & 3)  /* raw 2-bit tile pixel value (1-3; 0 = a fill) */
#define VBGO_TT_WORLD(v)   ((unsigned)((v) >> 43) & 31) /* world (layer) 0-31 that drew it */
#define VBGO_TT_CHAR(v)    ((unsigned)((v) >> 48) & 0x7FF) /* character slot (debug only - slots get reused) */
#define VBGO_TT_VALID(v)   ((unsigned)((v) >> 63) & 1)
/* Map cells, stored beside records (a uint32 each): bit 31 set = drawn
 * from that BG map cell (halfword index in VIP DRAM, low 16 bits). */
#define VBGO_TT_CELL_VALID(c) ((unsigned)((c) >> 31) & 1)
#define VBGO_TT_CELL(c)       ((unsigned)(c) & 0xFFFF)

/* ---- Frontend API ------------------------------------------------------ */

void vbgo_tiletrack_set_enabled(bool enabled);
bool vbgo_tiletrack_is_enabled(void);
/* Forget every tag (a new game, or a save state loaded): the frame buffers now hold pixels nothing tracked drew, and a
 * game that draws with its CPU and never runs a drawing pass would otherwise keep showing the old tags. */
void vbgo_tiletrack_reset(void);
/* Forget one frame buffer eye's tags, as if the CPU had drawn all of it (a save state put other pixels there). The
 * libretro core calls it only for buffers that changed a lot: rewind and run-ahead load states a frame or two away,
 * whose buffers still hold nearly what was tracked (platform/libretro/libretro_core.cpp). */
void vbgo_tiletrack_forget_buffer(unsigned fb, unsigned lr);
/* Forget which cells only moved (a save state loaded: the buffers' pixels may differ from what was recorded). */
void vbgo_tiletrack_forget_motion(void);
/* The VIP's frame buffers, FB[fb][lr][0x6000], each stored by band (vip.c's FBOffset), through a function the patched
 * vip.c adds - see cmake/PatchBeetleVip.cmake. */
const uint8_t *vbgo_vip_framebuffers(void);
/* An eye's pixels of a block as the frame buffer holds them: 192 words of two columns' two bytes each, all zero where
 * the pixels are. */
const uint32_t *vbgo_vip_block_pixels(unsigned fb, unsigned lr, unsigned block);
/* An eye of the picture last shown (the side-by-side surface isn't written): per column, 56 bytes
 * of 2-bit pixels (row y: byte y / 4, bits (y % 4) * 2) and the 4 pixel values they index, each the
 * 32-bit word the surface would have held (0: the display was off). */
const uint8_t *vbgo_vip_eye_bits(int eye);
const uint32_t *vbgo_vip_eye_clut(int eye);

/* An eye's pixel y in column x, as the 4 bytes the surface would have held there */
static inline uint32_t vbgo_eye_word(const uint8_t *bits, const uint32_t *clut, unsigned x, unsigned y)
{
   return clut[x * 4 + (bits[x * 56 + (y >> 2)] >> ((y & 3) * 2) & 3)];
}

/* Fills: background tiles' transparent pixels tagged too. Where nothing else is drawn, the backmost non-blank tile's
 * transparent pixel is what the player sees, so a pack can color it (a court surface between its speckles). */
#define VBGO_TT_FILLS_NONE 0
#define VBGO_TT_FILLS_PACK 1 /* only in cells marked with vbgo_tiletrack_set_fill_cells (cheap for the headset) */
#define VBGO_TT_FILLS_ALL  2 /* every one (captures need to know them all) */
void vbgo_tiletrack_set_fill_mode(int mode);
/* 65536-bit map (8 KB): bit n set = BG map cell n has fill colors. Copied. */
void vbgo_tiletrack_set_fill_cells(const uint8_t *bits);

/* A left-eye 8x8 cell drawn as the other frame buffer's pass showed columns x + dx to x + dx + 7 of its band, the
 * camera having only moved: the same tags, cells, pixels and tiles' hashes, none a sprite's or a fill. */
typedef struct
{
   uint32_t version; /* the cell's version it holds for (0: none) */
   uint32_t from[2]; /* that pass's versions of the cells holding columns x + dx and x + dx + 7 */
   uint16_t stamp;   /* that pass's stamp */
   int8_t dx;
} vbgo_tt_motion;

/* What one eye showed in the frame most recently output by retro_run(): per column, the 224 tags and cells of that
 * column (NULL if the column was blank), plus each 8-row band's key (which of its tags the pass drew) and the drawing
 * pass's tile hashes / character memory (index by VBGO_TAG_CHAR). */
typedef struct
{
   const uint32_t *columns[VBGO_TT_WIDTH];
   const uint16_t *cells[VBGO_TT_WIDTH]; /* (row y at vbgo_tt_cell_at(y); meaningful only where the tag is drawn) */
   /* For vbgo_tag_drawn on a pixel in band y / 8: VBGO_TT_BAND_REACHED, or 0 for a band the pass didn't
    * reach (its tags are an older pass's). */
   uint32_t band_key[VBGO_TT_HEIGHT / 8];
   /* As the pass started, so a tag's tile is known by content even after the game streams in new graphics. */
   const uint32_t *hashes;   /* 2048 */
   const uint8_t *blank;     /* 2048: 1 = all 64 pixels transparent */
   const uint16_t *chr;      /* 2048 * 8 rows (2 bits per pixel, pixel 0 in the low bits) */
   /* As the drawing pass started (NULL if the core didn't say): the 32 world attribute blocks (16 halfwords each, VIP
    * 0x3D800 - which eyes draw a world, its type, parallax...) and the OBJ attribute memory (1024 x 4 halfwords, VIP
    * 0x3E000 - a sprite's JX, JP/JLON/JRON, JY, char). */
   const uint16_t *worlds;
   const uint16_t *oam;
   /* 1 if the game's CPU wrote into this buffer since its drawing pass
    * (pixels no tile drew - see vbgo_tiletrack_cpu_fb_write), or what's in
    * it isn't known (a reset since) - 0: every pixel shown came from the pass. */
   int cpu_drawn;
   /* Sprite palettes (bit per JPLT0-3) that showed every pixel value 1-3 in one shade as the pass started: a
    * silhouette, which games use to flash a hit enemy. */
   unsigned flat_obj_palettes;
   /* Per 8-row band (28 of 64 words): the characters the drawing used there
    * - every character a tag of the band can name (a bit each). */
   const uint32_t *band_chr;
   /* Per 8x8 cell [band * 48 + cell]: the version of what it shows; equal nonzero versions mean
    * the same tags, pixels and per-pass copies of what they read. */
   const uint32_t *cell_version;
   /* Per 8x8 cell [band * 48 + cell], both eyes: a bit per world (layer) whose tags may be there (a
    * superset; sprites' tags not included). */
   const uint32_t *cell_worlds;
   /* Per band, a bit per 8x8 cell that may hold a tag in either eye (0: all its tags are 0). */
   const uint64_t *tagged;
   /* Per band, a bit per 8x8 cell that may hold a sprite's tag in either eye (only where tagged says one may be). */
   const uint64_t *obj_cells;
   /* Per band, a bit per 8x8 cell that may hold a fill's tag in either eye (only where tagged says one may be). */
   const uint64_t *filled_cells;
   int fb; /* the frame buffer shown */
   /* vbgo_tt_clut_serial as every column was shown, or 0 if they differed (or
    * a column showed nothing, or the other eye): the core's shades to colors. */
   uint32_t clut;
   /* The left eye's per 8x8 cell [band * 48 + cell] (see vbgo_tt_motion), and the stamp of the pass shown. */
   const vbgo_tt_motion *motion;
   uint16_t stamp;
   /* Per band, a bit per cell whose motion record may hold (none elsewhere); a bit per band given records by the pass. */
   const uint64_t *motion_cells;
   uint32_t motion_noted;
   /* Lane F: a bit per band drawn (or kept) with background color 0: every lit pixel shown there is a tile's, tagged
    * with its pixel value, but in the cells the CPU wrote into since (cpu_cells: per band, a bit per 8 columns). */
   uint32_t clean_bands;
   const uint64_t *cpu_cells;
} vbgo_tt_eye_view;

/* World attribute fields (w = 16 halfwords of one world). */
#define VBGO_WORLD_LON(w)   (((w)[0] >> 15) & 1)
#define VBGO_WORLD_RON(w)   (((w)[0] >> 14) & 1)
#define VBGO_WORLD_TYPE(w)  (((w)[0] >> 12) & 3) /* 0 normal, 1 H-bias, 2 affine, 3 OBJ */
#define VBGO_WORLD_END(w)   (((w)[0] >> 6) & 1)
#define VBGO_WORLD_MAP(w)   ((w)[0] & 15)
/* A sprite's parallax (signed: left eye x = JX - JP, right eye x = JX + JP). */
static inline int vbgo_obj_parallax(const uint16_t *oam, unsigned obj)
{
   const int jp = oam[obj * 4 + 1] & 0x3FFF;
   return (jp & 0x200) ? (jp & 0x3FF) - 0x400 : (jp & 0x3FF);
}

/* False if that eye has shown nothing tracked yet. */
bool vbgo_tiletrack_eye_view(unsigned eye, vbgo_tt_eye_view *view);

/* ---- Core hooks (called from the patched vip.c / vip_draw.inc only) ---- */

extern int vbgo_tt_on;
extern int vbgo_tt_fill;
extern uint32_t vbgo_tt_world;
extern const uint8_t *vbgo_tt_block_base;
extern uint32_t *vbgo_tt_dst[2];
extern uint16_t *vbgo_tt_cdst[2];
extern const uint8_t *vbgo_tt_blank;
extern uint8_t vbgo_tt_fill_cells[8192];
/* Bumped by the core whenever its shades-to-colors table changes. */
extern uint32_t vbgo_tt_clut_serial;
/* Lane F: the VIP's background color (BKCOL) as the block about to begin is drawn. */
extern uint8_t vbgo_tt_bkcol;

/* Sprites have no cell: their cell is which OBJ drew the pixel (the same in both eyes). */
#define VBGO_TT_OBJ_CELL(obj) ((uint16_t)((obj) & 0x3FF))

/* DrawBG/DrawAffine only ever draw pixels 0-383 of one row, so a call finds its tags once: pixel x of the row
 * row_target (the call's target) is tagged at vbgo_tt_row(row_target)[x * VBGO_TT_COLUMN], its cell at
 * vbgo_tt_cell_row(row_target)[x * VBGO_TT_CELL_COLUMN]. NULL while tracking is off. */
static inline uint32_t *vbgo_tt_row(const void *row_target)
{
   ptrdiff_t i;
   if (!vbgo_tt_on)
      return (uint32_t *)0;
   i = (const uint8_t *)row_target - vbgo_tt_block_base;
   return &vbgo_tt_dst[(i >> 12) & 1][(i >> 9) & 7];
}
static inline uint16_t *vbgo_tt_cell_row(const void *row_target)
{
   ptrdiff_t i;
   if (!vbgo_tt_on)
      return (uint16_t *)0;
   i = (const uint8_t *)row_target - vbgo_tt_block_base;
   return &vbgo_tt_cdst[(i >> 12) & 1][((i >> 9) & 7) * VBGO_TT_CELL_ROW];
}

/* What's the same for every tag of one drawing call: the world drawing. */
static inline uint32_t vbgo_tt_call_bits(void) { return vbgo_tt_world << 22; }

/* The rest of a BG pixel's tag (pv 0 = a fill); its cell is stored beside it. */
#define VBGO_TT_BG_BITS(chr, sx, sy, pal, pv)                                                                  \
   ((uint32_t)(chr) | ((uint32_t)(sx) << 11) | ((uint32_t)(sy) << 14) | ((uint32_t)(pal) << 17) | \
    ((uint32_t)(pv) << 20) | (1u << 27))

/* A transparent pixel of a BG tile (only if vbgo_tt_fills_from) at d, its cell at c: tagged
 * as a fill (tag: pv 0) unless something was already drawn or filled there
 * this pass (the block starts clear) - the backmost tile wins, drawn pixels always win. */
static inline void vbgo_tt_fill_at(uint32_t *d, uint16_t *c, uint32_t tag, unsigned cell)
{
   if (!*d)
      *d = tag, *c = (uint16_t)cell;
}

/* Whether a BG tile drawn from this cell may leave fills: non-blank tiles
 * only, and in VBGO_TT_FILLS_PACK mode only cells the pack fills. */
static inline int vbgo_tt_fills_from(unsigned chr, unsigned cell)
{
   return vbgo_tt_fill && !vbgo_tt_blank[chr] &&
          (vbgo_tt_fill == 2 || ((vbgo_tt_fill_cells[cell >> 3] >> (cell & 7)) & 1));
}

/* A whole character row drawn by DrawBG's 8-pixel path, tagged as one pixel at a time would be (pixel k: tag
 * d[k * VBGO_TT_COLUMN], cell c[k * VBGO_TT_CELL_COLUMN]). pixels: the row as stored (tile pixel n in bits 2n);
 * flipped: drawn mirrored (pixel k shows 7 - k). bits: vbgo_tt_call_bits() | VBGO_TT_BG_BITS(chr, 0, sy, pal, 0).
 * fills_from: vbgo_tt_fills_from(chr, cell), if the caller has it. */
static inline int vbgo_tt_tag_row8_fills(uint32_t *d, uint16_t *c, unsigned pixels, int flipped, uint32_t bits,
                                         unsigned cell, int fills_from)
{
   /* (a row with every pixel drawn leaves no fills) */
   const int opaque = ((pixels | (pixels >> 1)) & 0x5555) == 0x5555;
   const int fills = !opaque && fills_from;
   const unsigned x7 = flipped ? 7 : 0;
   unsigned k;
   if (!fills)
   {
      /* (unrolled: screen pixel k is d[k * VBGO_TT_COLUMN], tile pixel n = k, or 7 - k flipped) */
#define VBGO_TT_SET(k, n)                                                                          \
      d[(k) * VBGO_TT_COLUMN] = bits | ((n) << 11) | (((pixels >> (2 * (n))) & 3) << 20), \
      c[(k) * VBGO_TT_CELL_COLUMN] = (uint16_t)cell;
#define VBGO_TT_PUT(k, n)                                                                          \
   if ((pixels >> (2 * (n))) & 3)                                                                   \
      VBGO_TT_SET(k, n)
      if (!pixels)
         return 0;
      if (opaque) /* (every pixel drawn: none needs its own test) */
      {
         if (flipped)
         {
            VBGO_TT_SET(0, 7) VBGO_TT_SET(1, 6) VBGO_TT_SET(2, 5) VBGO_TT_SET(3, 4)
            VBGO_TT_SET(4, 3) VBGO_TT_SET(5, 2) VBGO_TT_SET(6, 1) VBGO_TT_SET(7, 0)
         }
         else
         {
            VBGO_TT_SET(0, 0) VBGO_TT_SET(1, 1) VBGO_TT_SET(2, 2) VBGO_TT_SET(3, 3)
            VBGO_TT_SET(4, 4) VBGO_TT_SET(5, 5) VBGO_TT_SET(6, 6) VBGO_TT_SET(7, 7)
         }
         return 1;
      }
      if (flipped)
      {
         VBGO_TT_PUT(0, 7) VBGO_TT_PUT(1, 6) VBGO_TT_PUT(2, 5) VBGO_TT_PUT(3, 4)
         VBGO_TT_PUT(4, 3) VBGO_TT_PUT(5, 2) VBGO_TT_PUT(6, 1) VBGO_TT_PUT(7, 0)
      }
      else
      {
         VBGO_TT_PUT(0, 0) VBGO_TT_PUT(1, 1) VBGO_TT_PUT(2, 2) VBGO_TT_PUT(3, 3)
         VBGO_TT_PUT(4, 4) VBGO_TT_PUT(5, 5) VBGO_TT_PUT(6, 6) VBGO_TT_PUT(7, 7)
      }
#undef VBGO_TT_PUT
#undef VBGO_TT_SET
      return 1;
   }
   if (flipped) /* screen order: reverse the 8 2-bit fields */
   {
      pixels = ((pixels >> 2) & 0x3333) | ((pixels & 0x3333) << 2);
      pixels = ((pixels >> 4) & 0x0F0F) | ((pixels & 0x0F0F) << 4);
      pixels = ((pixels >> 8) & 0x00FF) | ((pixels & 0x00FF) << 8);
   }
   for (k = 0; k < 8; k++, d += VBGO_TT_COLUMN, c += VBGO_TT_CELL_COLUMN, pixels >>= 2)
   {
      const uint32_t t = bits | ((k ^ x7) << 11) | ((pixels & 3) << 20);
      if (pixels & 3)
         *d = t, *c = (uint16_t)cell;
      else
         vbgo_tt_fill_at(d, c, t, cell);
   }
   return 2; /* (fills may be among them) */
}

/* The same, working out fills_from itself. */
static inline int vbgo_tt_tag_row8(uint32_t *d, uint16_t *c, unsigned pixels, int flipped, uint32_t bits, unsigned chr,
                                   unsigned cell)
{
   return vbgo_tt_tag_row8_fills(d, c, pixels, flipped, bits, cell, vbgo_tt_fills_from(chr, cell));
}

/* The core's change serials: its serial now, and the one each character and sprite (4 halfwords of OAM) was last
 * changed at. The epoch moves when they are reset (a state loaded, power on), so older stamps don't count. */
typedef struct
{
   const uint32_t *chr_at; /* 2048 */
   const uint32_t *oam_at; /* 1024 */
   uint32_t serial, epoch;
} vbgo_tt_serials;

/* dram: the VIP's DRAM (VIP 0x20000-0x3FFFF as halfwords), for the world and
 * OBJ attributes - may be NULL. jplt: the sprite palettes as shades (4 x 4:
 * palette, pixel value -> shade) - may be NULL. serials: may be NULL (every pass then copies and hashes it all). */
void vbgo_tiletrack_begin_block(const uint8_t *drawing_buffers, const uint16_t *chr_ram, const uint16_t *dram,
                                unsigned block_no, unsigned fb, const uint8_t *jplt, const vbgo_tt_serials *serials);
void vbgo_tiletrack_display_column(unsigned fb, unsigned lr, unsigned dest_lr, unsigned column, bool display_active);
void vbgo_tiletrack_cpu_fb_write(unsigned fb, unsigned lr, unsigned offset, unsigned bytes);
/* After vbgo_tiletrack_begin_block, for a block the VIP doesn't draw because it would draw what this buffer's last pass
 * did (the frame buffer still holds it): keeps that pass's tags for it as this pass's. False if they're gone (the block
 * must be drawn after all). */
bool vbgo_tiletrack_keep_block(void);
/* The same for only some of its cells (bit per 8 columns), the rest drawn again. */
bool vbgo_tiletrack_keep_cells(uint64_t cells);
/* Lane G: for a block the VIP copies from the other buffer, whose drawing it would make again: takes that pass's tags
 * for it. False if they're gone (the block must be drawn after all). */
bool vbgo_tiletrack_adopt_block(unsigned fb);
/* Before drawing the block begun last (rather than keeping it); compare: tell
 * the cells that came out the same by their tags (else only by unchanged). */
void vbgo_tiletrack_block_drawing(bool compare);
/* After drawing it: the characters it used (2048 bits), whether nothing it read had changed since its pass started
 * (else no versions - see cell_version), and per cell (bit per 8 columns) whether its pixels came out as this buffer
 * last held them, and whether nothing it shows could have changed. */
void vbgo_tiletrack_block_drawn(const uint32_t *chr, bool settled, uint64_t same_pixels, uint64_t unchanged);
/* After drawing some cells of the block: which of them may show something new
 * when only the changed worlds' attributes changed (a changed world shows
 * there, as last drawn or now - elsewhere every tag comes out the same). */
uint64_t vbgo_tiletrack_world_cells(uint32_t changed, uint64_t cells);
/* The cells these worlds tagged in the block begun last, as it was drawn (~0: not known). */
uint64_t vbgo_tiletrack_world_cells_were(uint32_t worlds);
/* After vbgo_tiletrack_block_drawn, for a block drawn whole: its left eye's motion records (see vbgo_tt_motion) against
 * the other frame buffer's pass. moved: background worlds whose attributes are as when that buffer drew the block but
 * for MX; dx[w]: a column world w shows at x it showed at x + dx[w] then; bad[w]: cells (bit per 8 columns) where
 * world w may show a map entry or character changed since. still: worlds that draw nothing in the left eye. Returns the
 * records made. */
unsigned vbgo_tiletrack_note_motion(uint32_t moved, uint32_t still, const int8_t *dx, const uint64_t *bad);
/* A bit per band (block) whose motion records paid in the renderer lately: the rest are noted only now and then. */
extern uint32_t vbgo_tt_copy_bands;
/* Per cell of the block being drawn, the background worlds that tagged a pixel
 * there (cleared before drawing: more than shows, never less). */
extern uint32_t vbgo_tt_wcells[48];
/* Cells of the block being drawn where a fill may have been tagged. */
extern uint8_t vbgo_tt_fillc[48];
/* Cells of the block being drawn where a sprite may have been tagged (cleared by the tracker before drawing). */
extern uint8_t vbgo_tt_ocells[48];
/* A drawing pass that repeats the other buffer's last one (nothing it reads changed since that pass started): whether
 * src_fb's tags can stand for it, sharing them for the pass into fb, and - if its inputs change partway - fb's own
 * tags from block_no on (the blocks before as src_fb's). */
bool vbgo_tiletrack_pass_reusable(unsigned src_fb);
void vbgo_tiletrack_share_pass(unsigned fb, unsigned src_fb);
void vbgo_tiletrack_resume_pass(unsigned fb, unsigned block_no);

#ifdef __cplusplus
}
#endif

#endif
