/* See vbgo_tiletrack.h. */
#include "vbgo_tiletrack.h"

#include <string.h>

int vbgo_tt_on = 0;
int vbgo_tt_fill = VBGO_TT_FILLS_NONE;
uint32_t vbgo_tt_world = 0;
const uint8_t *vbgo_tt_block_base = NULL;
uint32_t *vbgo_tt_dst[2];
uint16_t *vbgo_tt_cdst[2];
static const uint8_t s_no_blank[2048];
const uint8_t *vbgo_tt_blank = s_no_blank;
uint8_t vbgo_tt_fill_cells[8192];
uint32_t vbgo_tt_clut_serial = 1;

/* Tags in the VB's own frame buffer layout - [slot][eye][column * VBGO_TT_COLUMN + row].
 * Each frame buffer shows one slot's; a pass that repeats the other buffer's
 * (vbgo_tiletrack_share_pass) shares that buffer's slot, so a third slot is
 * always free for a pass of its own. */
#define VBGO_TT_SLOTS 3
static uint32_t s_tags[VBGO_TT_SLOTS][2][VBGO_TT_WIDTH * VBGO_TT_COLUMN] __attribute__((aligned(32)));
/* ... and each tag's cell, both eyes per column (see VBGO_TT_CELL_COLUMN; only read where the tag is drawn). */
static uint16_t s_cells[VBGO_TT_SLOTS][VBGO_TT_WIDTH * VBGO_TT_CELL_COLUMN] __attribute__((aligned(32)));

/* One column of a band's tags (8) or cells (8 per eye): one whole cache line. dcbz skips reading what's about
 * to be cleared. */
static inline void ClearLine(void *line)
{
#ifdef GEKKO
   __asm__ volatile("dcbz 0,%0" : : "r"(line) : "memory");
#else
   memset(line, 0, 32);
#endif
}

/* Per slot: its drawing pass's stamp (a pass number; tags don't carry it), the tracking settings it was drawn
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
/* Per slot and block: the pass its tags last counted for (s_block_stamp also forgets blocks only so they aren't kept). */
static uint16_t s_band_pass[VBGO_TT_SLOTS][VBGO_TT_BLOCKS];
/* ... the characters its drawing used (a bit each), and per 8x8 cell the
 * version of what it shows (0: one a renderer can't count on - see
 * vbgo_tiletrack_block_drawn). */
#define VBGO_TT_CELLS (VBGO_TT_WIDTH / 8)
static uint32_t s_block_chr[VBGO_TT_SLOTS][VBGO_TT_BLOCKS][2048 / 32];
static uint32_t s_cell_version[VBGO_TT_SLOTS][VBGO_TT_BLOCKS][VBGO_TT_CELLS];
static uint64_t s_cell_black[VBGO_TT_SLOTS][VBGO_TT_BLOCKS]; /* (cells the colorizer passes over - see BlackCell) */
/* Per slot and block: the cells (bit per 8 columns, both eyes) that may hold a tag - elsewhere every tag is 0,
 * so clearing skips them. */
static uint64_t s_tagged[VBGO_TT_SLOTS][VBGO_TT_BLOCKS];
/* Per cell, the worlds its tags showed as last drawn (bit per world; all: unknown). */
static uint32_t s_cell_worlds[VBGO_TT_SLOTS][VBGO_TT_BLOCKS][VBGO_TT_CELLS];
/* The slot and block cpu_fb_write last reset (slot * VBGO_TT_BLOCKS + block), -1 once anything else may have
 * written their stamp, cell versions, black cells or worlds. */
static int s_cpu_reset = -1;
static uint32_t s_versions;
/* The block being drawn as it was (this buffer's last pass's, if s_old_valid),
 * and what that pass's copies said about tiles and sprites, to tell which
 * cells come out the same. */
static uint32_t s_old_tags[2][VBGO_TT_WIDTH * 8];
static uint16_t s_old_cells[2][VBGO_TT_WIDTH * 8];
static int s_old_valid;
static uint32_t s_prev_hash[2048];
static uint16_t s_prev_oam[VBGO_OAM_HALFWORDS];
static int s_prev_valid;           /* (the slot's last pass was this buffer's: the copies above are it) */
static uint32_t s_chr_changed[2048 / 32]; /* tiles whose hash differs from that pass's */
static uint32_t s_obj_changed[1024 / 32]; /* ... sprites whose attributes do */
static int s_block_chr_changed;          /* (the block being drawn uses such a tile) */
static uint16_t s_prev_stamp[2]; /* per frame buffer: its last pass's stamp, as its pass started */
static uint16_t s_block_was;     /* the block begun last: its slot's stamp before */
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

/* After a wrap-around: keep's last pass takes stamp 1 and its other bands
 * are forgotten (their old stamps would otherwise come round again). */
static void RestampSlot(int k)
{
   s_cpu_reset = -1;
   unsigned b;
   for (b = 0; b < VBGO_TT_BLOCKS; b++)
   {
      const int valid = s_band_pass[k][b] == s_stamp[k];
      s_block_stamp[k][b] = s_block_stamp[k][b] == s_stamp[k] ? 1 : 0;
      s_band_pass[k][b] = valid ? 1 : 0;
      if (!valid)
      {
         memset(s_cell_version[k][b], 0, sizeof(s_cell_version[k][b]));
         s_cell_black[k][b] = 0;
         memset(s_cell_worlds[k][b], 0xFF, sizeof(s_cell_worlds[k][b]));
      }
   }
   s_stamp[k] = 1;
}

/* A new stamp; on wrap-around forget every old band (but keep's), so a band
 * left untouched for 65535 passes can't come back to life. */
static void NextStamp(int keep)
{
   s_cpu_reset = -1;
   if (++s_next_stamp == 0)
   {
      int k;
      for (k = 0; k < VBGO_TT_SLOTS; k++)
         if (k != keep)
         {
            memset(s_block_stamp[k], 0, sizeof(s_block_stamp[k]));
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
   s_cpu_reset = -1;
   from = s_slot[fb];
   to = FreeSlot();
   memcpy(s_tags[to], s_tags[from], sizeof(s_tags[to]));
   memcpy(s_cells[to], s_cells[from], sizeof(s_cells[to]));
   memcpy(s_tagged[to], s_tagged[from], sizeof(s_tagged[to]));
   memcpy(s_block_stamp[to], s_block_stamp[from], sizeof(s_block_stamp[to]));
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
   s_cpu_reset = -1;
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
   s_cpu_reset = -1;
   memset(s_tags, 0, sizeof(s_tags));
   memset(s_block_stamp, 0, sizeof(s_block_stamp));
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
   s_cpu_reset = -1;
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
   s_cpu_reset = -1;
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
   vbgo_tt_cdst[0] = &s_cells[s_slot[fb]][block_no * 16];
   vbgo_tt_cdst[1] = &s_cells[s_slot[fb]][block_no * 16 + 8];
   vbgo_tt_world = 0;
   s_block_slot = s_slot[fb];
   s_block_no = block_no;
   if (block_no < VBGO_TT_BLOCKS)
   {
      /* (drawn or kept, the block's tags are this pass's from here on) */
      s_block_was = s_block_stamp[s_slot[fb]][block_no] == s_prev_stamp[fb] ? s_prev_stamp[fb] : 0;
      s_block_stamp[s_slot[fb]][block_no] = s_band_pass[s_slot[fb]][block_no] = s_stamp[s_slot[fb]];
   }
   else
      s_block_was = 0;
}

/* Before drawing into the block begun last: clear these cells' tags in both eyes. */
static void ClearCells(uint64_t cells)
{
   unsigned e, x;
   memset(vbgo_tt_ocells, 0, sizeof(vbgo_tt_ocells));
   for (; cells; cells &= cells - 1)
   {
      const unsigned c = (unsigned)__builtin_ctzll(cells);
      for (x = c * 8; x < c * 8 + 8; x++)
      {
         for (e = 0; e < 2; e++)
            ClearLine(vbgo_tt_dst[e] + x * VBGO_TT_COLUMN);
         ClearLine(vbgo_tt_cdst[0] + x * VBGO_TT_CELL_COLUMN);
      }
   }
}

bool vbgo_tiletrack_keep_block(void)
{
   return !vbgo_tt_on || s_block_was;
}

bool vbgo_tiletrack_keep_cells(uint64_t cells)
{
   if (!vbgo_tt_on)
      return true;
   if (!s_block_was)
      return false;
   /* (the cells drawn again start clear, the kept ones stay as they are) */
   ClearCells(s_tagged[s_block_slot][s_block_no] & ~cells);
   s_tagged[s_block_slot][s_block_no] &= cells;
   return true;
}

void vbgo_tiletrack_block_drawing(bool compare)
{
   unsigned e, x, c;
   uint64_t tagged;
   s_old_valid = 0;
   if (!vbgo_tt_on || s_block_slot < 0 || s_block_no >= VBGO_TT_BLOCKS)
      return;
   s_old_valid = compare && s_block_was && s_prev_valid;
   tagged = s_tagged[s_block_slot][s_block_no];
   for (e = 0; s_old_valid && e < 2; e++)
      for (c = 0; c < VBGO_TT_CELLS; c++)
      {
         uint32_t *to = &s_old_tags[e][c * 64];
         if (!(tagged >> c & 1))
         {
            /* (all 0 there, the cell's 8 columns at once; the cells needn't be copied: SameCell reads them only
             * under a drawn tag) */
            memset(to, 0, 64 * sizeof(*to));
            continue;
         }
         for (x = c * 8; x < c * 8 + 8; x++, to += 8)
         {
            /* (spelled out: on the Wii a memcpy call per column costs more than the copy itself) */
            const uint32_t *from = vbgo_tt_dst[e] + x * VBGO_TT_COLUMN;
            uint32_t cells[4];
            to[0] = from[0], to[1] = from[1], to[2] = from[2], to[3] = from[3];
            to[4] = from[4], to[5] = from[5], to[6] = from[6], to[7] = from[7];
            memcpy(cells, vbgo_tt_cdst[e] + x * VBGO_TT_CELL_COLUMN, sizeof(cells));
            memcpy(&s_old_cells[e][x * 8], cells, sizeof(cells));
         }
      }
   ClearCells(tagged);
   s_tagged[s_block_slot][s_block_no] = 0;
}

/* Whether cell c's tags show what they showed before the drawing: the same
 * tags (0 where nothing was drawn), tiles that hash the same, sprites with the same attributes. */
static int SameCell(unsigned c)
{
   unsigned e, x, y;
   for (e = 0; e < 2; e++)
      for (x = c * 8; x < c * 8 + 8; x++)
      {
         const uint32_t *was = &s_old_tags[e][x * 8], *is = vbgo_tt_dst[e] + x * VBGO_TT_COLUMN;
         const uint16_t *was_cell = &s_old_cells[e][x * 8], *is_cell = vbgo_tt_cdst[e] + x * VBGO_TT_CELL_COLUMN;
         /* (the column's 8 tags at once: most cells that differ differ there) */
         if ((was[0] ^ is[0]) | (was[1] ^ is[1]) | (was[2] ^ is[2]) | (was[3] ^ is[3]) | (was[4] ^ is[4]) |
             (was[5] ^ is[5]) | (was[6] ^ is[6]) | (was[7] ^ is[7]))
            return 0;
         for (y = 0; y < 8; y++)
         {
            uint64_t t;
            if (!is[y])
               continue;
            if (was_cell[y] != is_cell[y])
               return 0;
            t = vbgo_tag_compose(is[y], is_cell[y]);
            if (s_block_chr_changed && (s_chr_changed[VBGO_TAG_CHAR(t) >> 5] >> (VBGO_TAG_CHAR(t) & 31) & 1))
               return 0;
            if (VBGO_TAG_IS_OBJ(t) && (s_obj_changed[VBGO_TAG_OBJ_NO(t) >> 5] >> (VBGO_TAG_OBJ_NO(t) & 31) & 1))
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
   s_cpu_reset = -1;
   uint32_t *version, fresh = 0;
   uint64_t was_black, black = 0;
   unsigned c;
   if (!vbgo_tt_on || s_block_slot < 0 || s_block_no >= VBGO_TT_BLOCKS)
      return;
   memcpy(s_block_chr[s_block_slot][s_block_no], chr, sizeof(s_block_chr[0][0]));
   for (c = 0; c < VBGO_TT_CELLS; c++)
      s_tagged[s_block_slot][s_block_no] |= (uint64_t)((vbgo_tt_wcells[c] | vbgo_tt_ocells[c]) != 0) << c;
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
uint8_t vbgo_tt_ocells[48];

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
   s_cpu_reset = -1;
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
         s_tagged[to][e] = s_tagged[from][e];
         memcpy(s_block_chr[to][e], s_block_chr[from][e], sizeof(s_block_chr[to][e]));
         memcpy(s_cell_version[to][e], s_cell_version[from][e], sizeof(s_cell_version[to][e]));
         s_cell_black[to][e] = s_cell_black[from][e];
         memcpy(s_cell_worlds[to][e], s_cell_worlds[from][e], sizeof(s_cell_worlds[to][e]));
      }
   }
   /* (the blocks it repeated: the other buffer's tags, as this pass's) */
   for (x = 0; to != from && x < VBGO_TT_WIDTH; x++)
   {
      const unsigned rows = (block_no < VBGO_TT_BLOCKS ? block_no : VBGO_TT_BLOCKS) * 8;
      for (e = 0; e < 2; e++)
         memcpy(&s_tags[to][e][x * VBGO_TT_COLUMN], &s_tags[from][e][x * VBGO_TT_COLUMN], rows * sizeof(uint32_t));
      memcpy(&s_cells[to][x * VBGO_TT_CELL_COLUMN], &s_cells[from][x * VBGO_TT_CELL_COLUMN], 2 * rows * sizeof(uint16_t));
   }
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
   int slot;
   if (!vbgo_tt_on)
      return;
   s_cpu[fb & 1] = 1;
   Unshare(fb & 1);
   slot = s_slot[fb & 1];
   for (b = 0; b < bytes; b++)
   {
      const unsigned o = offset + b;
      const unsigned column = o / 64, row = (o % 64) * 4;
      if (column >= VBGO_TT_WIDTH)
         continue;
      for (p = 0; p < 4; p++)
         s_tags[slot][lr & 1][column * VBGO_TT_COLUMN + row + p] = 0;
      /* (a block reset again with nothing written there since is already as this leaves it) */
      if (row / 8 < VBGO_TT_BLOCKS && s_cpu_reset != (int)(slot * VBGO_TT_BLOCKS + row / 8))
      {
         s_block_stamp[slot][row / 8] = 0;
         memset(s_cell_version[slot][row / 8], 0, sizeof(s_cell_version[0][0]));
         s_cell_black[slot][row / 8] = 0;
         memset(s_cell_worlds[slot][row / 8], 0xFF, sizeof(s_cell_worlds[0][0]));
         s_cpu_reset = (int)(slot * VBGO_TT_BLOCKS + row / 8);
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
      view->band_key[x] = s_band_pass[slot][x] == s_stamp[slot] ? VBGO_TT_BAND_REACHED : 0;
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
      const int shown = (d & 4) && (d & 1) == (unsigned)fb;
      view->columns[x] = shown ? &s_tags[slot][(d >> 1) & 1][x * VBGO_TT_COLUMN] : NULL;
      view->cells[x] = shown ? &s_cells[slot][x * VBGO_TT_CELL_COLUMN + ((d >> 1) & 1) * 8] : NULL;
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
      const uint32_t *col = view.columns[x];
      const uint16_t *col_cells = view.cells[x];
      for (y = 0; y < VBGO_TT_HEIGHT; y++)
      {
         const uint64_t t = col ? vbgo_tag_compose(col[y], col_cells[vbgo_tt_cell_at(y)]) : 0;
         uint64_t out = 0;
         uint32_t cell = 0;
         if (t && vbgo_tag_drawn(t, view.band_key[y >> 3]) && (with_fills || VBGO_TAG_PIXEL(t)))
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
