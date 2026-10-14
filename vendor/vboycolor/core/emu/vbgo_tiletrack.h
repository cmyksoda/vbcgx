/* VirtualBoyGo tile tracking - records, for every pixel the Virtual Boy's
 * VIP draws, which 8x8 character (tile) it came from, where inside that
 * tile, and which background map cell put it there, so a frontend can
 * recolor by tile (per-game color packs) instead of only by shade.
 * Experimental.
 *
 * Hooked into the Beetle VB core through generated copies of vip.c and
 * vip_draw.inc (see cmake/PatchBeetleVip.cmake) - the submodule itself stays
 * untouched. Plain C, shared by the core (vip.c) and the frontend.
 *
 * How it works: while the VIP draws a frame buffer, every drawn pixel gets a
 * 64-bit tag (character slot, pixel inside the tile, palette, layer, map
 * cell - see VBGO_TAG_*) written straight into a tag buffer laid out like
 * the VB's own column-major frame buffer, double-buffered the same way. Each
 * drawing pass has a stamp; tags from older passes simply don't match it, so
 * nothing is cleared per frame. At the start of each pass the character
 * memory is snapshotted and every tile hashed once, so a tag's tile is known
 * by content (hash) even after the game has streamed new graphics in.
 *
 * Fills: background (non-sprite) tiles' transparent pixels can be tagged too
 * - where nothing else is drawn, the backmost non-blank tile's transparent
 * pixel is what the player sees there, so a color pack can color it (a court
 * surface between its speckles, say). VBGO_TT_FILLS_ALL tags all of them
 * (captures need to know every one); VBGO_TT_FILLS_PACK only those in map
 * cells the pack has fill colors for (cheap enough for the headset).
 *
 * Off by default; while off, the core draws exactly as upstream (only a
 * flag check per drawn pixel). Turn on with vbgo_tiletrack_set_enabled().
 */
#ifndef VBGO_TILETRACK_H
#define VBGO_TILETRACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VBGO_TT_WIDTH  384
#define VBGO_TT_HEIGHT 224
#define VBGO_TT_EYE_PIXELS (VBGO_TT_WIDTH * VBGO_TT_HEIGHT)
/* Tags per column in the tag buffer (rows 0-223 used). Not 256: a 2 KB stride
 * puts a block's tags across all columns in 4 of the Wii's 128 L1 sets. */
#define VBGO_TT_COLUMN 260

/* ---- Tags (the tracker's own per-pixel format) -------------------------- */

#define VBGO_TAG_CHAR(t)     ((unsigned)(t) & 0x7FF)          /* character slot 0-2047 */
#define VBGO_TAG_INDEX(t)    ((unsigned)((t) >> 11) & 63)     /* pixel in the tile: y * 8 + x (tile space, flips undone) */
#define VBGO_TAG_SUBX(t)     ((unsigned)((t) >> 11) & 7)
#define VBGO_TAG_SUBY(t)     ((unsigned)((t) >> 14) & 7)
#define VBGO_TAG_PALETTE(t)  ((unsigned)((t) >> 17) & 3)      /* GPLT/JPLT palette number */
#define VBGO_TAG_IS_OBJ(t)   ((unsigned)((t) >> 19) & 1)      /* 1 = sprite (OBJ) */
#define VBGO_TAG_PIXEL(t)    ((unsigned)((t) >> 20) & 3)      /* raw 2-bit value; 0 = a fill (transparent pixel) */
#define VBGO_TAG_WORLD(t)    ((unsigned)((t) >> 22) & 31)     /* world (layer) that drew it */
#define VBGO_TAG_HAS_CELL(t) ((unsigned)((t) >> 27) & 1)      /* drawn from a BG map cell (not a sprite) */
#define VBGO_TAG_CELL(t)     ((unsigned)((t) >> 28) & 0xFFFF) /* that cell's halfword index in VIP DRAM */
#define VBGO_TAG_OBJ_NO(t)   ((unsigned)((t) >> 28) & 0x3FF)  /* sprites: the OBJ (0-1023) - same bits, no cell */
#define VBGO_TAG_STAMP(t)    ((unsigned)((t) >> 48))          /* drawing pass that wrote it */

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
/* Forget every tag (a new game, or a save state loaded: what's in the frame
 * buffers now wasn't drawn by anything tracked - a game that draws with its
 * CPU and never runs a drawing pass would otherwise keep showing the old tags). */
void vbgo_tiletrack_reset(void);
/* Forget one frame buffer eye's tags, as if the CPU had drawn all of it (a
 * save state put other pixels there). The libretro core's way of loading a
 * state: rewinding and run-ahead load states only a frame or two away, whose
 * buffers still hold nearly what was tracked - only the ones that changed a
 * lot are forgotten (platform/libretro/libretro_core.cpp). */
void vbgo_tiletrack_forget_buffer(unsigned fb, unsigned lr);
/* The VIP's frame buffers, FB[fb][lr][0x6000] (a function the patched vip.c
 * adds - see cmake/PatchBeetleVip.cmake). */
const uint8_t *vbgo_vip_framebuffers(void);

#define VBGO_TT_FILLS_NONE 0
#define VBGO_TT_FILLS_PACK 1 /* only in cells marked with vbgo_tiletrack_set_fill_cells */
#define VBGO_TT_FILLS_ALL  2
void vbgo_tiletrack_set_fill_mode(int mode);
int vbgo_tiletrack_fill_mode(void);
/* 65536-bit map (8 KB): bit n set = BG map cell n has fill colors. Copied. */
void vbgo_tiletrack_set_fill_cells(const uint8_t *bits);

/* What one eye showed in the frame most recently output by retro_run():
 * per column, the 224 tags of that column (NULL if the column was blank),
 * plus the stamp tags of each 8-row band must carry to belong to it and the
 * drawing pass's tile hashes / character memory (index by VBGO_TAG_CHAR). */
typedef struct
{
   const uint64_t *columns[VBGO_TT_WIDTH];
   /* Compare with (tag >> 48) for a pixel in band y / 8 (none matches a band the pass didn't reach).
    * A band the pass kept carries the stamp it was drawn with, so keeping one rewrites no tags. */
   uint32_t band_stamp[VBGO_TT_HEIGHT / 8];
   const uint32_t *hashes;   /* 2048 */
   const uint8_t *blank;     /* 2048: 1 = all 64 pixels transparent */
   const uint16_t *chr;      /* 2048 * 8 rows (2 bits per pixel, pixel 0 in the low bits) */
   /* As the drawing pass started (NULL if the core didn't say): the 32 world
    * attribute blocks (16 halfwords each, VIP 0x3D800 - which eyes draw a
    * world, its type, parallax...) and the OBJ attribute memory (1024 x 4
    * halfwords, VIP 0x3E000 - a sprite's JX, JP/JLON/JRON, JY, char). */
   const uint16_t *worlds;
   const uint16_t *oam;
   /* 1 if the game's CPU wrote into this buffer since its drawing pass
    * (pixels no tile drew - see vbgo_tiletrack_cpu_fb_write), or what's in
    * it isn't known (a reset since) - 0: every pixel shown came from the pass. */
   int cpu_drawn;
   /* Sprite palettes (bit per JPLT0-3) that showed every pixel value 1-3 in
    * one shade as the pass started - a silhouette: games flash a hit enemy
    * by switching it to such a palette. */
   unsigned flat_obj_palettes;
   /* Per 8-row band (28 of 64 words): the characters the drawing used there
    * - every character a tag of the band can name (a bit each). */
   const uint32_t *band_chr;
   /* Per 8x8 cell [band * 48 + cell]: the version of what it shows; equal nonzero versions mean
    * the same tags (but stamps), pixels and per-pass copies of what they read. */
   const uint32_t *cell_version;
   int fb; /* the frame buffer shown */
   /* vbgo_tt_clut_serial as every column was shown, or 0 if they differed (or
    * a column showed nothing, or the other eye): the core's shades to colors. */
   uint32_t clut;
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

/* The eye's frame as records (row-major, VBGO_TT_EYE_PIXELS) and, if cells
 * isn't NULL, their map cells. Fills are included only if with_fills. */
bool vbgo_tiletrack_records(unsigned eye, uint64_t *records, uint32_t *cells, bool with_fills);

/* Character memory as of the last drawing pass (2048 tiles of 8 rows),
 * everything the game had loaded - on screen or not. NULL until a frame was
 * drawn with tracking on. */
const uint16_t *vbgo_tiletrack_chr_ram(void);

/* The hash records use for a tile with these 8 rows. */
uint32_t vbgo_tiletrack_hash_rows(const uint16_t rows[8]);

/* ---- Core hooks (called from the patched vip.c / vip_draw.inc only) ---- */

extern int vbgo_tt_on;
extern int vbgo_tt_fill;
extern uint32_t vbgo_tt_world;
extern const uint8_t *vbgo_tt_block_base;
extern uint64_t *vbgo_tt_dst[2];
extern uint64_t vbgo_tt_stamp_bits;
extern const uint8_t *vbgo_tt_blank;
extern uint8_t vbgo_tt_fill_cells[8192];
/* Bumped by the core whenever its shades-to-colors table changes. */
extern uint32_t vbgo_tt_clut_serial;

#define VBGO_TT_CELL_BITS(cell) (((uint64_t)1 << 27) | ((uint64_t)(cell) << 28))
/* Sprites: no cell, but which OBJ drew the pixel (the same in both eyes). */
#define VBGO_TT_OBJ_BITS(obj) ((uint64_t)((obj) & 0x3FF) << 28)

/* Background (map) drawing - DrawBG/DrawAffine - only ever draws pixels 0-383
 * of one row, so its tags are found once per call: pixel x of the row
 * row_target (that call's target) is tagged at row[x * VBGO_TT_COLUMN].
 * NULL while tracking is off. */
static inline uint64_t *vbgo_tt_row(const void *row_target)
{
   ptrdiff_t i;
   if (!vbgo_tt_on)
      return (uint64_t *)0;
   i = (const uint8_t *)row_target - vbgo_tt_block_base;
   return &vbgo_tt_dst[(i >> 12) & 1][(i >> 9) & 7];
}

/* What's the same for every tag of one drawing call: the pass's stamp and
 * the world drawing. */
static inline uint64_t vbgo_tt_call_bits(void) { return vbgo_tt_stamp_bits | ((uint64_t)vbgo_tt_world << 22); }

/* The rest of a BG pixel's tag (pv 0 = a fill). */
#define VBGO_TT_BG_BITS(chr, sx, sy, pal, pv, cell)                                                       \
   (VBGO_TT_CELL_BITS(cell) | (uint64_t)((uint32_t)(chr) | ((uint32_t)(sx) << 11) | ((uint32_t)(sy) << 14) | \
                                         ((uint32_t)(pal) << 17) | ((uint32_t)(pv) << 20)))

/* A transparent pixel of a BG tile (only if vbgo_tt_fills_from) at d: tagged
 * as a fill (tag: pv 0) unless something was already drawn or filled there
 * this pass - the backmost tile wins, drawn pixels always win. */
static inline void vbgo_tt_fill_at(uint64_t *d, uint64_t tag)
{
   if ((*d >> 48) != (tag >> 48))
      *d = tag;
}

/* Whether a BG tile drawn from this cell may leave fills: non-blank tiles
 * only, and in VBGO_TT_FILLS_PACK mode only cells the pack fills. */
static inline int vbgo_tt_fills_from(unsigned chr, unsigned cell)
{
   return vbgo_tt_fill && !vbgo_tt_blank[chr] &&
          (vbgo_tt_fill == 2 || ((vbgo_tt_fill_cells[cell >> 3] >> (cell & 7)) & 1));
}

/* A whole character row drawn by DrawBG's 8-pixel path: its 8 screen pixels
 * are tagged at d, d + VBGO_TT_COLUMN, ... pixels: the row as stored (tile
 * pixel n in bits 2n); flipped: drawn mirrored (screen pixel k shows tile
 * pixel 7 - k). bits: vbgo_tt_call_bits() | VBGO_TT_BG_BITS(chr, 0, sy, pal,
 * 0, cell). Drawn pixels and fills as one at a time would tag them. */
static inline int vbgo_tt_tag_row8(uint64_t *d, unsigned pixels, int flipped, uint64_t bits, unsigned chr, unsigned cell)
{
   /* (a row with every pixel drawn leaves no fills) */
   const int fills = ((pixels | (pixels >> 1)) & 0x5555) != 0x5555 && vbgo_tt_fills_from(chr, cell);
   const unsigned x7 = flipped ? 7 : 0;
   unsigned k;
   if (!fills)
   {
      /* (unrolled: screen pixel k is d[k * VBGO_TT_COLUMN], tile pixel n = k, or 7 - k flipped) */
#define VBGO_TT_PUT(k, n)                                                                      \
   if ((pixels >> (2 * (n))) & 3)                                                               \
      d[(k) * VBGO_TT_COLUMN] = bits | (uint64_t)(((n) << 11) | (((pixels >> (2 * (n))) & 3) << 20));
      if (!pixels)
         return 0;
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
      return 1;
   }
   if (flipped) /* screen order: reverse the 8 2-bit fields */
   {
      pixels = ((pixels >> 2) & 0x3333) | ((pixels & 0x3333) << 2);
      pixels = ((pixels >> 4) & 0x0F0F) | ((pixels & 0x0F0F) << 4);
      pixels = ((pixels >> 8) & 0x00FF) | ((pixels & 0x00FF) << 8);
   }
   for (k = 0; k < 8; k++, d += VBGO_TT_COLUMN, pixels >>= 2)
   {
      const uint64_t t = bits | (uint64_t)(((k ^ x7) << 11) | ((pixels & 3) << 20));
      if (pixels & 3)
         *d = t;
      else
         vbgo_tt_fill_at(d, t);
   }
   return 1;
}

/* dram: the VIP's DRAM (VIP 0x20000-0x3FFFF as halfwords), for the world and
 * OBJ attributes - may be NULL. jplt: the sprite palettes as shades (4 x 4:
 * palette, pixel value -> shade) - may be NULL. */
void vbgo_tiletrack_begin_block(const uint8_t *drawing_buffers, const uint16_t *chr_ram, const uint16_t *dram,
                                unsigned block_no, unsigned fb, const uint8_t *jplt);
void vbgo_tiletrack_display_column(unsigned fb, unsigned lr, unsigned dest_lr, unsigned column, bool display_active);
void vbgo_tiletrack_cpu_fb_write(unsigned fb, unsigned lr, unsigned offset, unsigned bytes);
/* A drawing pass that repeats the other buffer's last one (nothing it reads
 * changed since that pass started): whether src_fb's tags can stand for it,
 * sharing them for the pass into fb, and - if its inputs change partway -
 * fb's own tags from block_no on (the blocks before as src_fb's). */
/* After vbgo_tiletrack_begin_block, for a block the VIP doesn't draw because
 * it would draw what this buffer's last pass did (the frame buffer still holds
 * it): keeps that pass's tags for it as this pass's. False if they're gone
 * (the block must be drawn after all). */
bool vbgo_tiletrack_keep_block(void);
/* The same for only some of its cells (bit per 8 columns), the rest drawn again. */
bool vbgo_tiletrack_keep_cells(uint64_t cells);
/* Before drawing the block begun last (rather than keeping it); compare: tell
 * the cells that came out the same by their tags (else only by unchanged). */
void vbgo_tiletrack_block_drawing(bool compare);
/* After drawing it: the characters it used (2048 bits), whether nothing it read
 * had changed since its pass started (else no versions - see cell_version), and
 * per cell (bit per 8 columns) whether its pixels came out as this buffer last
 * held them, and whether nothing it shows could have changed. */
void vbgo_tiletrack_block_drawn(const uint32_t *chr, bool settled, uint64_t same_pixels, uint64_t unchanged);
/* After drawing some cells of the block: which of them may show something new
 * when only the changed worlds' attributes changed (a changed world shows
 * there, as last drawn or now - elsewhere every tag comes out the same). */
uint64_t vbgo_tiletrack_world_cells(uint32_t changed, uint64_t cells);
/* Per cell of the block being drawn, the background worlds that tagged a pixel
 * there (cleared before drawing: more than shows, never less). */
extern uint32_t vbgo_tt_wcells[48];
bool vbgo_tiletrack_pass_reusable(unsigned src_fb);
void vbgo_tiletrack_share_pass(unsigned fb, unsigned src_fb);
void vbgo_tiletrack_resume_pass(unsigned fb, unsigned block_no);

#ifdef __cplusplus
}
#endif

#endif
