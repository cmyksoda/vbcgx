/* See vbgo_tiletrack.h. */
#include "vbgo_tiletrack.h"

#include <string.h>

int vbgo_tt_on = 0;
int vbgo_tt_fill = VBGO_TT_FILLS_NONE;
uint32_t vbgo_tt_world = 0;
const uint8_t *vbgo_tt_block_base = NULL;
uint64_t *vbgo_tt_dst[2];
uint64_t vbgo_tt_stamp_bits = 0;
static const uint8_t s_no_blank[2048];
const uint8_t *vbgo_tt_blank = s_no_blank;
uint8_t vbgo_tt_fill_cells[8192];
uint32_t vbgo_tt_clut_serial = 1;

/* Tags in the VB's own frame buffer layout - [slot][eye][column * VBGO_TT_COLUMN + row].
 * Each frame buffer shows one slot's; a pass that repeats the other buffer's
 * (vbgo_tiletrack_share_pass) shares that buffer's slot, so a third slot is
 * always free for a pass of its own. */
#define VBGO_TT_SLOTS 3
static uint64_t s_tags[VBGO_TT_SLOTS][2][VBGO_TT_WIDTH * VBGO_TT_COLUMN];

/* Per slot: its drawing pass's stamp, the tracking settings it was drawn
 * with, and the character memory / tile hashes / blank flags as that pass
 * started. */
static uint16_t s_stamp[VBGO_TT_SLOTS];
static uint16_t s_next_stamp;
static unsigned s_settings[VBGO_TT_SLOTS];
static unsigned s_settings_serial = 1;
static uint16_t s_chr[VBGO_TT_SLOTS][2048 * 8];
static uint32_t s_hash[VBGO_TT_SLOTS][2048];
static uint8_t s_blank[VBGO_TT_SLOTS][2048];
/* The world attribute blocks and OBJ attributes as each slot's pass started. */
#define VBGO_WORLD_HALFWORDS (32 * 16)
#define VBGO_OAM_HALFWORDS (1024 * 4)
static uint16_t s_worlds[VBGO_TT_SLOTS][VBGO_WORLD_HALFWORDS];
static uint16_t s_oam[VBGO_TT_SLOTS][VBGO_OAM_HALFWORDS];
static int s_have_attrs[VBGO_TT_SLOTS];
static unsigned s_flat[VBGO_TT_SLOTS]; /* flat sprite palettes as its pass started (see the view) */
/* Per slot and block: the stamp of the pass whose tags it holds there (0:
 * none), so a block a pass keeps from this buffer's last one can take the
 * new stamp instead of being drawn again (vbgo_tiletrack_keep_block). */
#define VBGO_TT_BLOCKS (VBGO_TT_HEIGHT / 8)
static uint16_t s_block_stamp[VBGO_TT_SLOTS][VBGO_TT_BLOCKS];
/* Per slot and block: the stamp its tags carry (as drawn; a kept block's stays),
 * and the pass its tags last counted for (s_block_stamp also forgets blocks only so they aren't kept). */
static uint16_t s_band_stamp[VBGO_TT_SLOTS][VBGO_TT_BLOCKS], s_band_pass[VBGO_TT_SLOTS][VBGO_TT_BLOCKS];
/* ... the characters its drawing used (a bit each), and per 8x8 cell the
 * version of what it shows (0: one a renderer can't count on - see
 * vbgo_tiletrack_block_drawn). */
#define VBGO_TT_CELLS (VBGO_TT_WIDTH / 8)
static uint32_t s_block_chr[VBGO_TT_SLOTS][VBGO_TT_BLOCKS][2048 / 32];
static uint32_t s_cell_version[VBGO_TT_SLOTS][VBGO_TT_BLOCKS][VBGO_TT_CELLS];
static uint64_t s_cell_black[VBGO_TT_SLOTS][VBGO_TT_BLOCKS]; /* (cells the colorizer passes over - see BlackCell) */
/* Per cell, the worlds its tags showed as last drawn (bit per world; all: unknown). */
static uint32_t s_cell_worlds[VBGO_TT_SLOTS][VBGO_TT_BLOCKS][VBGO_TT_CELLS];
static uint32_t s_versions;
/* The block being drawn as it was (this buffer's last pass's, if s_old_valid),
 * and what that pass's copies said about tiles and sprites, to tell which
 * cells come out the same. */
static uint64_t s_old_tags[2][VBGO_TT_WIDTH * 8];
static int s_old_valid;
static uint16_t s_old_stamp;
static uint32_t s_prev_hash[2048];
static uint16_t s_prev_oam[VBGO_OAM_HALFWORDS];
static int s_prev_valid;           /* (the slot's last pass was this buffer's: the copies above are it) */
static uint32_t s_chr_changed[2048 / 32]; /* tiles whose hash differs from that pass's */
static uint32_t s_obj_changed[1024 / 32]; /* ... sprites whose attributes do */
static int s_block_chr_changed;          /* (the block being drawn uses such a tile) */
static uint16_t s_prev_stamp[2]; /* per frame buffer: its last pass's stamp, as its pass started */
static uint16_t s_block_was;     /* the block begun last: its slot's stamp before */
static uint16_t s_band_was;      /* (and the stamp its tags carried then) */
static int s_block_slot = -1;    /* ... its slot and number */
static unsigned s_block_no;

/* Per frame buffer: its slot, whether it has tags, and whether the CPU wrote
 * into it since its pass (or a reset since). */
static int s_slot[2] = {0, 1};
static int s_have[2];
static int s_cpu[2];
static int s_pass_open; /* a drawing pass has started (block 0 seen, or tracking switched on mid-pass) */
static int s_last_fb = -1;

/* Per output eye and column: which buffer and eye were shown there
 * (fb | lr << 1 | 4 for "active"), 0 = blank. */
static uint8_t s_disp[2][VBGO_TT_WIDTH];
static uint32_t s_disp_clut[2][VBGO_TT_WIDTH]; /* ... and vbgo_tt_clut_serial as it was */

static uint32_t HashChar(const uint16_t *rows)
{
   /* FNV-1a over the 8 rows' values (byte-order independent). */
   uint32_t h = 2166136261u;
   int i;
   for (i = 0; i < 8; i++)
   {
      h = (h ^ (rows[i] & 0xFF)) * 16777619u;
      h = (h ^ (rows[i] >> 8)) * 16777619u;
   }
   return h;
}

static int FreeSlot(void)
{
   int k = 0;
   while (k == s_slot[0] || k == s_slot[1])
      k++;
   return k;
}

/* After a wrap-around: keep's tags of its last pass take stamp 1 and the rest
 * go (a kept band's old stamp would otherwise come round again). */
static void RestampSlot(int k)
{
   unsigned b, e, x, y;
   for (b = 0; b < VBGO_TT_BLOCKS; b++)
   {
      const int valid = s_band_pass[k][b] == s_stamp[k];
      for (e = 0; e < 2; e++)
         for (x = 0; x < VBGO_TT_WIDTH; x++)
         {
            uint64_t *t = &s_tags[k][e][x * VBGO_TT_COLUMN + b * 8];
            for (y = 0; y < 8; y++)
               t[y] = valid && (t[y] >> 48) == s_band_stamp[k][b] ? (t[y] & 0xFFFFFFFFFFFFull) | ((uint64_t)1 << 48) : 0;
         }
      s_block_stamp[k][b] = s_block_stamp[k][b] == s_stamp[k] ? 1 : 0;
      s_band_stamp[k][b] = s_band_pass[k][b] = valid ? 1 : 0;
      if (!valid)
      {
         memset(s_cell_version[k][b], 0, sizeof(s_cell_version[k][b]));
         s_cell_black[k][b] = 0;
         memset(s_cell_worlds[k][b], 0xFF, sizeof(s_cell_worlds[k][b]));
      }
   }
   s_stamp[k] = 1;
}

/* A new stamp; on wrap-around forget every old tag (but keep's), so a pixel
 * left untouched for 65535 passes can't come back to life. */
static void NextStamp(int keep)
{
   if (++s_next_stamp == 0)
   {
      int k;
      for (k = 0; k < VBGO_TT_SLOTS; k++)
         if (k != keep)
         {
            memset(s_tags[k], 0, sizeof(s_tags[k]));
            memset(s_block_stamp[k], 0, sizeof(s_block_stamp[k]));
            memset(s_band_stamp[k], 0, sizeof(s_band_stamp[k]));
            memset(s_band_pass[k], 0, sizeof(s_band_pass[k]));
            memset(s_cell_version[k], 0, sizeof(s_cell_version[k]));
            memset(s_cell_black[k], 0, sizeof(s_cell_black[0]));
            memset(s_cell_worlds[k], 0xFF, sizeof(s_cell_worlds[k]));
         }
      memset(s_have, 0, sizeof(s_have));
      s_next_stamp = 1;
      if (keep >= 0)
      {
         RestampSlot(keep);
         s_next_stamp = 2;
      }
   }
   vbgo_tt_stamp_bits = (uint64_t)s_next_stamp << 48;
}

static void CopySlotInfo(int to, int from)
{
   memcpy(s_chr[to], s_chr[from], sizeof(s_chr[to]));
   memcpy(s_hash[to], s_hash[from], sizeof(s_hash[to]));
   memcpy(s_blank[to], s_blank[from], sizeof(s_blank[to]));
   memcpy(s_worlds[to], s_worlds[from], sizeof(s_worlds[to]));
   memcpy(s_oam[to], s_oam[from], sizeof(s_oam[to]));
   s_have_attrs[to] = s_have_attrs[from];
   s_flat[to] = s_flat[from];
   s_settings[to] = s_settings[from];
   s_stamp[to] = s_stamp[from];
}

/* Before fb's tags change: a slot of its own (with the same tags). */
static void Unshare(unsigned fb)
{
   int from, to;
   if (s_slot[fb] != s_slot[fb ^ 1])
      return;
   from = s_slot[fb];
   to = FreeSlot();
   memcpy(s_tags[to], s_tags[from], sizeof(s_tags[to]));
   memcpy(s_block_stamp[to], s_block_stamp[from], sizeof(s_block_stamp[to]));
   memcpy(s_band_stamp[to], s_band_stamp[from], sizeof(s_band_stamp[to]));
   memcpy(s_band_pass[to], s_band_pass[from], sizeof(s_band_pass[to]));
   memcpy(s_block_chr[to], s_block_chr[from], sizeof(s_block_chr[to]));
   memcpy(s_cell_version[to], s_cell_version[from], sizeof(s_cell_version[to]));
   memcpy(s_cell_black[to], s_cell_black[from], sizeof(s_cell_black[to]));
   memcpy(s_cell_worlds[to], s_cell_worlds[from], sizeof(s_cell_worlds[to]));
   CopySlotInfo(to, from);
   s_slot[fb] = to;
}

/* New tracking settings: no tags drawn before can stand for this pass's. */
static void ForgetBlocks(void)
{
   s_settings_serial++;
   memset(s_block_stamp, 0, sizeof(s_block_stamp));
   memset(s_cell_version, 0, sizeof(s_cell_version));
   memset(s_cell_black, 0, sizeof(s_cell_black));
   memset(s_cell_worlds, 0xFF, sizeof(s_cell_worlds));
}

/* The tiles as last hashed, with their hashes and blank flags. */
static uint16_t s_hashed_chr[2048 * 8];
static uint32_t s_hashed[2048];
static uint8_t s_hashed_blank[2048];
static int s_hashed_any;

static void StartPass(unsigned fb, const uint16_t *chr_ram, const uint16_t *dram)
{
   int c, slot;
   s_prev_stamp[fb] = s_have[fb] ? s_stamp[s_slot[fb]] : 0;
   Unshare(fb); /* (a slot of its own, holding the tags its last pass repeated) */
   slot = s_slot[fb];
   s_prev_valid = s_prev_stamp[fb] && s_stamp[slot] == s_prev_stamp[fb] && s_have_attrs[slot];
   if (s_prev_valid)
   {
      memcpy(s_prev_hash, s_hash[slot], sizeof(s_prev_hash));
      memcpy(s_prev_oam, s_oam[slot], sizeof(s_prev_oam));
   }
   NextStamp(-1);
   s_stamp[slot] = s_next_stamp;
   s_settings[slot] = s_settings_serial;
   memcpy(s_chr[slot], chr_ram, sizeof(s_chr[slot]));
   s_have_attrs[slot] = dram != NULL;
   if (dram)
   {
      memcpy(s_worlds[slot], &dram[0x1D800 >> 1], sizeof(s_worlds[slot]));
      memcpy(s_oam[slot], &dram[0x1E000 >> 1], sizeof(s_oam[slot]));
   }
   for (c = 0; c < 2048; c++)
   {
      /* (only tiles that changed since the last pass are hashed again) */
      const uint16_t *rows = &s_chr[slot][c * 8];
      if (!s_hashed_any || memcmp(rows, &s_hashed_chr[c * 8], 16))
      {
         memcpy(&s_hashed_chr[c * 8], rows, 16);
         s_hashed[c] = HashChar(rows);
         s_hashed_blank[c] = (rows[0] | rows[1] | rows[2] | rows[3] | rows[4] | rows[5] | rows[6] | rows[7]) == 0;
      }
   }
   s_hashed_any = 1;
   memcpy(s_hash[slot], s_hashed, sizeof(s_hash[slot]));
   memcpy(s_blank[slot], s_hashed_blank, sizeof(s_blank[slot]));
   memset(s_chr_changed, 0, sizeof(s_chr_changed));
   memset(s_obj_changed, 0, sizeof(s_obj_changed));
   for (c = 0; s_prev_valid && c < 2048; c++)
      s_chr_changed[c >> 5] |= (uint32_t)(s_hash[slot][c] != s_prev_hash[c]) << (c & 31);
   for (c = 0; s_prev_valid && c < 1024; c++)
      s_obj_changed[c >> 5] |= (uint32_t)(memcmp(&s_oam[slot][c * 4], &s_prev_oam[c * 4], 8) != 0) << (c & 31);
   /* (Tracking switched on mid-pass: the blocks before this one simply have
    * no tags with this stamp.) */
   s_have[fb] = 1;
   s_cpu[fb] = 0; /* (the pass draws every pixel of the buffer anew) */
   s_pass_open = 1;
}

void vbgo_tiletrack_set_enabled(bool enabled)
{
   if (!enabled && vbgo_tt_on)
   {
      /* What was tracked goes stale while off - nothing to show until the
       * next pass (switched back on: the screen shows tags again once a
       * frame drawn with tracking on is displayed). */
      s_pass_open = 0;
      s_have[0] = s_have[1] = 0;
      memset(s_disp, 0, sizeof(s_disp));
   }
   if (vbgo_tt_on != (enabled ? 1 : 0))
      ForgetBlocks();
   vbgo_tt_on = enabled ? 1 : 0;
}

bool vbgo_tiletrack_is_enabled(void) { return vbgo_tt_on != 0; }

void vbgo_tiletrack_reset(void)
{
   memset(s_tags, 0, sizeof(s_tags));
   memset(s_block_stamp, 0, sizeof(s_block_stamp));
   memset(s_band_stamp, 0, sizeof(s_band_stamp));
   memset(s_band_pass, 0, sizeof(s_band_pass));
   memset(s_cell_version, 0, sizeof(s_cell_version));
   memset(s_cell_black, 0, sizeof(s_cell_black));
   memset(s_cell_worlds, 0xFF, sizeof(s_cell_worlds));
   memset(s_have, 0, sizeof(s_have));
   s_slot[0] = 0;
   s_slot[1] = 1;
   memset(s_disp, 0, sizeof(s_disp));
   s_pass_open = 0;
   s_last_fb = -1;
   s_cpu[0] = s_cpu[1] = 1;
}

void vbgo_tiletrack_forget_buffer(unsigned fb, unsigned lr)
{
   Unshare(fb & 1);
   memset(s_tags[s_slot[fb & 1]][lr & 1], 0, sizeof(s_tags[0][0]));
   memset(s_block_stamp[s_slot[fb & 1]], 0, sizeof(s_block_stamp[0]));
   memset(s_cell_version[s_slot[fb & 1]], 0, sizeof(s_cell_version[0]));
   memset(s_cell_black[s_slot[fb & 1]], 0, sizeof(s_cell_black[0]));
   memset(s_cell_worlds[s_slot[fb & 1]], 0xFF, sizeof(s_cell_worlds[0]));
   s_cpu[fb & 1] = 1;
}

void vbgo_tiletrack_set_fill_mode(int mode)
{
   if (vbgo_tt_fill != mode)
      ForgetBlocks();
   vbgo_tt_fill = mode;
}

int vbgo_tiletrack_fill_mode(void) { return vbgo_tt_fill; }

void vbgo_tiletrack_set_fill_cells(const uint8_t *bits)
{
   ForgetBlocks();
   if (bits)
      memcpy(vbgo_tt_fill_cells, bits, sizeof(vbgo_tt_fill_cells));
   else
      memset(vbgo_tt_fill_cells, 0, sizeof(vbgo_tt_fill_cells));
}

void vbgo_tiletrack_begin_block(const uint8_t *drawing_buffers, const uint16_t *chr_ram, const uint16_t *dram,
                                unsigned block_no, unsigned fb, const uint8_t *jplt)
{
   if (!vbgo_tt_on)
      return;
   fb &= 1;
   /* Once per pass (games update tile graphics between frames), or right
    * after tracking was switched on mid-pass. */
   if (block_no == 0 || !s_pass_open || (int)fb != s_last_fb)
   {
      unsigned p;
      StartPass(fb, chr_ram, dram);
      s_flat[s_slot[fb]] = 0;
      if (jplt)
         for (p = 0; p < 4; p++)
            if (jplt[p * 4 + 1] == jplt[p * 4 + 2] && jplt[p * 4 + 2] == jplt[p * 4 + 3])
               s_flat[s_slot[fb]] |= 1u << p;
   }
   s_last_fb = (int)fb;
   vbgo_tt_block_base = drawing_buffers;
   vbgo_tt_blank = s_blank[s_slot[fb]];
   vbgo_tt_dst[0] = &s_tags[s_slot[fb]][0][block_no * 8];
   vbgo_tt_dst[1] = &s_tags[s_slot[fb]][1][block_no * 8];
   vbgo_tt_world = 0;
   s_block_slot = s_slot[fb];
   s_block_no = block_no;
   if (block_no < VBGO_TT_BLOCKS)
   {
      /* (drawn or kept, the block's tags are this pass's from here on) */
      s_block_was = s_block_stamp[s_slot[fb]][block_no] == s_prev_stamp[fb] ? s_prev_stamp[fb] : 0;
      s_band_was = s_band_stamp[s_slot[fb]][block_no];
      s_block_stamp[s_slot[fb]][block_no] = s_stamp[s_slot[fb]];
      s_band_stamp[s_slot[fb]][block_no] = s_band_pass[s_slot[fb]][block_no] = s_stamp[s_slot[fb]];
   }
   else
      s_block_was = 0;
}

bool vbgo_tiletrack_keep_block(void)
{
   if (!vbgo_tt_on)
      return true;
   if (!s_block_was)
      return false;
   s_band_stamp[s_block_slot][s_block_no] = s_band_was;
   return true;
}

bool vbgo_tiletrack_keep_cells(uint64_t cells)
{
   const uint16_t now = (uint16_t)(vbgo_tt_stamp_bits >> 48);
   unsigned e, x, y;
   if (!vbgo_tt_on)
      return true;
   if (!s_block_was)
      return false;
   if (__builtin_popcountll(cells) >= VBGO_TT_CELLS / 2)
   {
      /* (mostly kept: the cells drawn again start clear and take the band's stamp, the kept ones stay as they are) */
      for (e = 0; e < 2; e++)
         for (x = 0; x < VBGO_TT_WIDTH; x++)
            if (!(cells >> (x >> 3) & 1))
               memset(vbgo_tt_dst[e] + x * VBGO_TT_COLUMN, 0, 8 * sizeof(uint64_t));
      s_band_stamp[s_block_slot][s_block_no] = s_band_was;
      vbgo_tt_stamp_bits = (uint64_t)s_band_was << 48;
      return true;
   }
   /* (mostly drawn again: the kept cells' tags take this pass's stamp) */
   for (e = 0; e < 2; e++)
      for (x = 0; x < VBGO_TT_WIDTH; x++)
         if (cells >> (x >> 3) & 1)
         {
            uint64_t *t = vbgo_tt_dst[e] + x * VBGO_TT_COLUMN;
            for (y = 0; y < 8; y++)
               if ((t[y] >> 48) == s_band_was)
                  t[y] = (t[y] & 0xFFFFFFFFFFFFull) | ((uint64_t)now << 48);
         }
   return true;
}

void vbgo_tiletrack_block_drawing(bool compare)
{
   unsigned e, x;
   s_old_valid = 0;
   if (!compare || !vbgo_tt_on || !s_block_was || !s_prev_valid || s_block_slot < 0 || s_block_no >= VBGO_TT_BLOCKS)
      return;
   for (e = 0; e < 2; e++)
      for (x = 0; x < VBGO_TT_WIDTH; x++)
         memcpy(&s_old_tags[e][x * 8], vbgo_tt_dst[e] + x * VBGO_TT_COLUMN, 8 * sizeof(uint64_t));
   s_old_stamp = s_band_was;
   s_old_valid = 1;
}

/* Whether cell c's tags show what they showed before the drawing: the same
 * tags (but stamps), tiles that hash the same, sprites with the same attributes.
 * (A tag of the pass is never 0 but for its stamp: cell or sprite bit.) */
static int SameCell(unsigned c)
{
   const uint32_t was_stamp = (uint32_t)s_old_stamp << 16, now_stamp = (uint32_t)(vbgo_tt_stamp_bits >> 32);
   unsigned e, x, y;
   for (e = 0; e < 2; e++)
      for (x = c * 8; x < c * 8 + 8; x++)
      {
         const uint64_t *was = &s_old_tags[e][x * 8], *is = vbgo_tt_dst[e] + x * VBGO_TT_COLUMN;
         for (y = 0; y < 8; y++)
         {
            const uint32_t a_hi = (uint32_t)(was[y] >> 32), b_hi = (uint32_t)(is[y] >> 32);
            if ((a_hi & 0xFFFF0000u) != was_stamp)
            {
               if ((b_hi & 0xFFFF0000u) == now_stamp)
                  return 0;
               continue;
            }
            /* (a tag of that pass: the same now but for the stamp) */
            if (b_hi != (now_stamp | (a_hi & 0xFFFF)) || (uint32_t)was[y] != (uint32_t)is[y])
               return 0;
            if (s_block_chr_changed && (s_chr_changed[VBGO_TAG_CHAR(is[y]) >> 5] >> (VBGO_TAG_CHAR(is[y]) & 31) & 1))
               return 0;
            if (VBGO_TAG_IS_OBJ(is[y]) && (s_obj_changed[VBGO_TAG_OBJ_NO(is[y]) >> 5] >> (VBGO_TAG_OBJ_NO(is[y]) & 31) & 1))
               return 0;
         }
      }
   return 1;
}

/* Whether cell c, both eyes, shows nothing the colorizer colors: every pixel in shade 0 (black at
 * any brightness) and no fill tagged there - what's left was drawn black or not drawn by this pass. */
static int BlackCell(unsigned c)
{
   const uint8_t *fbs = vbgo_vip_framebuffers() + (size_t)(s_last_fb & 1) * 2 * 0x6000 + s_block_no * 2;
   unsigned e, x;
   if (vbgo_tt_fillc[c])
      return 0;
   for (e = 0; e < 2; e++)
      for (x = c * 8; x < c * 8 + 8; x++)
         if (fbs[e * 0x6000 + 64 * x] | fbs[e * 0x6000 + 64 * x + 1])
            return 0;
   return 1;
}

void vbgo_tiletrack_block_drawn(const uint32_t *chr, bool settled, uint64_t same_pixels, uint64_t unchanged)
{
   uint32_t *version, fresh = 0;
   uint64_t was_black, black = 0;
   unsigned c;
   vbgo_tt_stamp_bits = (uint64_t)s_next_stamp << 48; /* (after keep_cells' drawing) */
   if (!vbgo_tt_on || s_block_slot < 0 || s_block_no >= VBGO_TT_BLOCKS)
      return;
   memcpy(s_block_chr[s_block_slot][s_block_no], chr, sizeof(s_block_chr[0][0]));
   version = s_cell_version[s_block_slot][s_block_no];
   s_block_chr_changed = 0;
   for (c = 0; c < 2048 / 32; c++)
      s_block_chr_changed |= (chr[c] & s_chr_changed[c]) != 0;
   if (settled)
   {
      if (++s_versions == 0)
         s_versions = 1;
      fresh = s_versions;
   }
   was_black = s_cell_black[s_block_slot][s_block_no];
   for (c = 0; c < VBGO_TT_CELLS; c++)
      if ((same_pixels & ~unchanged) >> c & 1) /* (elsewhere 0: only a cell redrawn the same next time is asked) */
         black |= (uint64_t)BlackCell(c) << c;
   s_cell_black[s_block_slot][s_block_no] = black;
   for (c = 0; c < VBGO_TT_CELLS; c++)
      if (!(settled && s_block_was && version[c] &&
            ((unchanged >> c & 1) || (s_old_valid && (same_pixels >> c & 1) && SameCell(c)) ||
             ((same_pixels & was_black & black) >> c & 1))))
         version[c] = fresh;
   s_old_valid = 0;
}

uint32_t vbgo_tt_wcells[48];
uint8_t vbgo_tt_fillc[48];

uint64_t vbgo_tiletrack_world_cells(uint32_t changed, uint64_t cells)
{
   uint64_t maybe = 0;
   uint32_t *worlds;
   unsigned c;
   if (!vbgo_tt_on || s_block_slot < 0 || s_block_no >= VBGO_TT_BLOCKS)
      return cells;
   worlds = s_cell_worlds[s_block_slot][s_block_no];
   for (c = 0; c < VBGO_TT_CELLS; c++)
      if (cells >> c & 1)
      {
         if (!s_block_was || ((worlds[c] | vbgo_tt_wcells[c]) & changed))
            maybe |= 1ull << c;
         worlds[c] = vbgo_tt_wcells[c];
      }
   return maybe;
}

bool vbgo_tiletrack_pass_reusable(unsigned src_fb)
{
   src_fb &= 1;
   return !vbgo_tt_on || (s_have[src_fb] && !s_cpu[src_fb] && s_settings[s_slot[src_fb]] == s_settings_serial);
}

void vbgo_tiletrack_share_pass(unsigned fb, unsigned src_fb)
{
   fb &= 1;
   src_fb &= 1;
   if (!vbgo_tt_on)
      return;
   s_slot[fb] = s_slot[src_fb];
   s_have[fb] = 1;
   s_cpu[fb] = 0;
   s_pass_open = 0;
}

void vbgo_tiletrack_resume_pass(unsigned fb, unsigned block_no)
{
   unsigned e, x, y;
   int from, to;
   uint16_t old_stamp;
   fb &= 1;
   if (!vbgo_tt_on)
      return;
   from = s_slot[fb];
   to = s_slot[fb ^ 1] == from ? FreeSlot() : from;
   NextStamp(from);
   old_stamp = s_stamp[from];
   if (to != from)
   {
      CopySlotInfo(to, from);
      s_slot[fb] = to;
   }
   s_stamp[to] = s_next_stamp;
   for (e = 0; e < block_no && e < VBGO_TT_BLOCKS; e++)
   {
      s_block_stamp[to][e] = s_block_stamp[from][e] == old_stamp ? s_next_stamp : s_block_stamp[from][e];
      s_band_pass[to][e] = s_band_pass[from][e] == old_stamp ? s_next_stamp : s_band_pass[from][e];
      if (to != from)
      {
         s_band_stamp[to][e] = s_band_stamp[from][e];
         memcpy(s_block_chr[to][e], s_block_chr[from][e], sizeof(s_block_chr[to][e]));
         memcpy(s_cell_version[to][e], s_cell_version[from][e], sizeof(s_cell_version[to][e]));
         s_cell_black[to][e] = s_cell_black[from][e];
         memcpy(s_cell_worlds[to][e], s_cell_worlds[from][e], sizeof(s_cell_worlds[to][e]));
      }
   }
   /* (the blocks it repeated: the other buffer's tags, as this pass's) */
   for (e = 0; to != from && e < 2; e++)
      for (x = 0; x < VBGO_TT_WIDTH; x++)
         memcpy(&s_tags[to][e][x * VBGO_TT_COLUMN], &s_tags[from][e][x * VBGO_TT_COLUMN],
                (block_no < VBGO_TT_BLOCKS ? block_no : VBGO_TT_BLOCKS) * 8 * sizeof(uint64_t));
   s_have[fb] = 1;
   s_cpu[fb] = 0;
   s_pass_open = 1;
   s_last_fb = (int)fb;
}

void vbgo_tiletrack_display_column(unsigned fb, unsigned lr, unsigned dest_lr, unsigned column, bool display_active)
{
   if (!vbgo_tt_on || column >= VBGO_TT_WIDTH)
      return;
   s_disp[dest_lr & 1][column] = display_active ? (uint8_t)((fb & 1) | ((lr & 1) << 1) | 4) : 0;
   s_disp_clut[dest_lr & 1][column] = vbgo_tt_clut_serial;
}

void vbgo_tiletrack_cpu_fb_write(unsigned fb, unsigned lr, unsigned offset, unsigned bytes)
{
   /* Pixels the game's CPU writes straight into the framebuffer (not drawn
    * from tiles) - forget whatever tile was there. 4 pixels per byte,
    * 64 bytes per column. */
   unsigned b, p;
   if (!vbgo_tt_on)
      return;
   s_cpu[fb & 1] = 1;
   Unshare(fb & 1);
   for (b = 0; b < bytes; b++)
   {
      const unsigned o = offset + b;
      const unsigned column = o / 64, row = (o % 64) * 4;
      if (column >= VBGO_TT_WIDTH)
         continue;
      for (p = 0; p < 4; p++)
         s_tags[s_slot[fb & 1]][lr & 1][column * VBGO_TT_COLUMN + row + p] = 0;
      if (row / 8 < VBGO_TT_BLOCKS)
      {
         s_block_stamp[s_slot[fb & 1]][row / 8] = 0;
         memset(s_cell_version[s_slot[fb & 1]][row / 8], 0, sizeof(s_cell_version[0][0]));
         s_cell_black[s_slot[fb & 1]][row / 8] = 0;
         memset(s_cell_worlds[s_slot[fb & 1]][row / 8], 0xFF, sizeof(s_cell_worlds[0][0]));
      }
   }
}

static int ShownBuffer(unsigned eye)
{
   /* The buffer the eye's columns came from (they all do in practice). */
   unsigned x;
   for (x = 0; x < VBGO_TT_WIDTH; x++)
      if (s_disp[eye & 1][x])
         return s_disp[eye & 1][x] & 1;
   return -1;
}

bool vbgo_tiletrack_eye_view(unsigned eye, vbgo_tt_eye_view *view)
{
   unsigned x;
   const int fb = ShownBuffer(eye);
   int slot;
   if (fb < 0 || !s_have[fb])
      return false;
   slot = s_slot[fb];
   for (x = 0; x < VBGO_TT_BLOCKS; x++)
      view->band_stamp[x] = s_band_pass[slot][x] == s_stamp[slot] ? s_band_stamp[slot][x] : 0x10000u;
   view->hashes = s_hash[slot];
   view->blank = s_blank[slot];
   view->chr = s_chr[slot];
   view->worlds = s_have_attrs[slot] ? s_worlds[slot] : NULL;
   view->oam = s_have_attrs[slot] ? s_oam[slot] : NULL;
   view->cpu_drawn = s_cpu[fb];
   view->flat_obj_palettes = s_flat[slot];
   view->band_chr = &s_block_chr[slot][0][0];
   view->cell_version = &s_cell_version[slot][0][0];
   view->cell_worlds = &s_cell_worlds[slot][0][0];
   view->fb = fb;
   view->clut = s_disp_clut[eye & 1][0];
   for (x = 0; x < VBGO_TT_WIDTH; x++)
   {
      const uint8_t d = s_disp[eye & 1][x];
      view->columns[x] = (d & 4) && (d & 1) == (unsigned)fb ? &s_tags[slot][(d >> 1) & 1][x * VBGO_TT_COLUMN] : NULL;
      if (!view->columns[x] || ((d >> 1) & 1) != (eye & 1) || s_disp_clut[eye & 1][x] != view->clut)
         view->clut = 0;
   }
   return true;
}

bool vbgo_tiletrack_records(unsigned eye, uint64_t *records, uint32_t *cells, bool with_fills)
{
   vbgo_tt_eye_view view;
   unsigned x, y;
   if (!vbgo_tiletrack_eye_view(eye, &view))
      return false;
   for (x = 0; x < VBGO_TT_WIDTH; x++)
   {
      const uint64_t *col = view.columns[x];
      for (y = 0; y < VBGO_TT_HEIGHT; y++)
      {
         const uint64_t t = col ? col[y] : 0;
         uint64_t out = 0;
         uint32_t cell = 0;
         if (t && (t >> 48) == view.band_stamp[y >> 3] && (with_fills || VBGO_TAG_PIXEL(t)))
         {
            const unsigned chr = VBGO_TAG_CHAR(t);
            out = (uint64_t)view.hashes[chr] | ((uint64_t)VBGO_TAG_SUBX(t) << 32) | ((uint64_t)VBGO_TAG_SUBY(t) << 35) |
                  ((uint64_t)VBGO_TAG_PALETTE(t) << 38) | ((uint64_t)VBGO_TAG_IS_OBJ(t) << 40) |
                  ((uint64_t)VBGO_TAG_PIXEL(t) << 41) | ((uint64_t)VBGO_TAG_WORLD(t) << 43) | ((uint64_t)chr << 48) |
                  ((uint64_t)1 << 63);
            if (VBGO_TAG_HAS_CELL(t))
               cell = 0x80000000u | VBGO_TAG_CELL(t);
         }
         records[y * VBGO_TT_WIDTH + x] = out;
         if (cells)
            cells[y * VBGO_TT_WIDTH + x] = cell;
      }
   }
   return true;
}

const uint16_t *vbgo_tiletrack_chr_ram(void)
{
   const int fb = s_last_fb;
   return fb >= 0 && s_have[fb] ? s_chr[s_slot[fb]] : NULL;
}

uint32_t vbgo_tiletrack_hash_rows(const uint16_t rows[8]) { return HashChar(rows); }
