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
uint8_t vbgo_tt_bkcol;

/* Tags in the VB's own frame buffer layout - [slot][eye][column * VBGO_TT_COLUMN + row]. Each frame buffer shows one
 * slot's; a pass repeating the other buffer's (vbgo_tiletrack_share_pass) shares its slot: a third is always free. */
#define VBGO_TT_SLOTS 3
/* (64-byte aligned, like the other statics depth reads: see FB in vip.c) */
static uint32_t s_tags[VBGO_TT_SLOTS][2][VBGO_TT_WIDTH * VBGO_TT_COLUMN] __attribute__((aligned(64)));
/* ... and each tag's cell, [slot][eye][row * VBGO_TT_CELL_ROW + column] (8 more rows to draw a block aside in). Only
 * read where the tag is drawn, so they're cleared only as whole lines (16 columns), to claim what drawing writes. */
static uint16_t s_cells[VBGO_TT_SLOTS][2][(VBGO_TT_HEIGHT + 8) * VBGO_TT_CELL_ROW] __attribute__((aligned(64)));

/* One column of a band's tags (8), or a row of 16 columns' cells: one whole cache line. dcbz skips reading what's
 * about to be cleared. */
static inline void ClearLine(void *line)
{
#ifdef GEKKO
   __asm__ volatile("dcbz 0,%0" : : "r"(line) : "memory");
#else
   memset(line, 0, 32);
#endif
}

/* Per slot: its drawing pass's stamp (a pass number; tags don't carry it), the tracking settings it was drawn with, and
 * the character memory / tile hashes / blank flags as that pass started. */
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
/* Per slot: the core's change serial (and epoch) its characters, hashes and sprite attributes are up to date with, so
 * the next pass copies only what the core stamped after it. */
static uint32_t s_sync_at[VBGO_TT_SLOTS];
static uint32_t s_sync_epoch[VBGO_TT_SLOTS];
static unsigned s_flat[VBGO_TT_SLOTS]; /* flat sprite palettes as its pass started (see the view) */

/* Per slot and block: the stamp of the pass whose tags it holds there (0: none), so a block a pass keeps from this
 * buffer's last one can take the new stamp instead of being drawn again (vbgo_tiletrack_keep_block). */
#define VBGO_TT_BLOCKS (VBGO_TT_HEIGHT / 8)
static uint16_t s_block_stamp[VBGO_TT_SLOTS][VBGO_TT_BLOCKS];
/* Per slot and block: the pass its tags last counted for (s_block_stamp also forgets blocks only so they aren't
 * kept). */
static uint16_t s_band_pass[VBGO_TT_SLOTS][VBGO_TT_BLOCKS];
/* ... the characters its drawing used (a bit each), and per 8x8 cell the version of what it shows (0: one a renderer
 * can't count on - see vbgo_tiletrack_block_drawn). */
#define VBGO_TT_CELLS (VBGO_TT_WIDTH / 8)
static uint32_t s_block_chr[VBGO_TT_SLOTS][VBGO_TT_BLOCKS][2048 / 32];
static uint32_t s_cell_version[VBGO_TT_SLOTS][VBGO_TT_BLOCKS][VBGO_TT_CELLS];
static uint64_t s_cell_black[VBGO_TT_SLOTS][VBGO_TT_BLOCKS]; /* (cells the colorizer passes over - see BlackCell) */
/* Per slot and block: the cells (bit per 8 columns, both eyes) that may hold a tag - elsewhere every tag is 0,
 * so clearing skips them. */
static uint64_t s_tagged[VBGO_TT_SLOTS][VBGO_TT_BLOCKS];
/* ... and of those, the cells that may hold a sprite's tag (more than hold one, never less). */
static uint64_t s_obj_cells[VBGO_TT_SLOTS][VBGO_TT_BLOCKS];
/* ... and the cells that may hold a fill's tag (also a superset). */
static uint64_t s_filled_cells[VBGO_TT_SLOTS][VBGO_TT_BLOCKS];

/* Per slot: the blocks whose tags and cells are still slot s_lazy_from's (a pass unsharing its buffer copies a
 * block only if it keeps it), and for those the cells where the slot's own stale tags may be nonzero. */
static uint32_t s_lazy[VBGO_TT_SLOTS];
static int s_lazy_from[VBGO_TT_SLOTS];
static uint64_t s_stale[VBGO_TT_SLOTS][VBGO_TT_BLOCKS];

/* Per cell, the worlds its tags showed as last drawn (bit per world; all: unknown). */
static uint32_t s_cell_worlds[VBGO_TT_SLOTS][VBGO_TT_BLOCKS][VBGO_TT_CELLS];
/* Per cell, its left eye's motion record (see vbgo_tt_motion); kept wherever the cell versions are. */
static vbgo_tt_motion s_motion[VBGO_TT_SLOTS][VBGO_TT_BLOCKS][VBGO_TT_CELLS] __attribute__((aligned(32)));
/* Per block, a bit per cell whose record may still hold for its version (the rest can't: the renderer skips them). */
static uint64_t s_motion_cells[VBGO_TT_SLOTS][VBGO_TT_BLOCKS];
/* Per slot, the blocks (a bit each) whose drawing in its latest pass was given motion records. */
static uint32_t s_motion_noted[VBGO_TT_SLOTS];

/* Per slot and block, whether it was drawn (or kept) with background color 0, which no colorizer lights;
 * cleared when a state is loaded, its frame buffer's pixels then not those its tags were drawn with. */
static uint8_t s_band_bk0[VBGO_TT_SLOTS][VBGO_TT_BLOCKS];
/* ... and the cells (bit per 8 columns) the CPU has written into since. */
static uint64_t s_cpu_cells[VBGO_TT_SLOTS][VBGO_TT_BLOCKS];

/* Per slot, the blocks (a bit each) cpu_fb_write has reset, none once anything else may have written their stamp,
 * cell versions, black cells or worlds. */
static uint32_t s_cpu_reset[VBGO_TT_SLOTS];
static void ForgetCpuResets(void) { memset(s_cpu_reset, 0, sizeof(s_cpu_reset)); }
static uint32_t s_versions;

/* The block being drawn as it was (this buffer's last pass's, if s_old_valid), and what that pass's copies said about
 * tiles and sprites, to tell which cells come out the same. */
static int s_old_valid;
/* Such a block is drawn aside, in the spare rows, and put in place after: its old tags and cells (or its slot's stale
 * ones, if lazy) stay where they are meanwhile. */
static int s_aside;
static const uint32_t *s_aside_tags[2];
static const uint16_t *s_aside_cells[2];
/* (the cells that may hold old tags to clear, and whether the block is lazy) */
static uint64_t s_aside_clear;
static int s_aside_lazy;
static uint32_t s_prev_hash[2048];
static uint16_t s_prev_oam[VBGO_OAM_HALFWORDS];
static int s_prev_valid;           /* (the slot's last pass was this buffer's: the copies above are it) */
static uint32_t s_chr_changed[2048 / 32]; /* tiles whose hash differs from that pass's */
static uint32_t s_obj_changed[1024 / 32]; /* ... sprites whose attributes do */
static int s_chr_any_changed;            /* (any at all, so a block need not look) */
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
   ForgetCpuResets();
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
   ForgetCpuResets();
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
      memset(s_motion, 0, sizeof(s_motion)); /* (their stamps are of the old count) */
      memset(s_motion_cells, 0, sizeof(s_motion_cells));
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
   s_sync_at[to] = s_sync_at[from];
   s_sync_epoch[to] = s_sync_epoch[from];
   s_flat[to] = s_flat[from];
   s_settings[to] = s_settings[from];
   s_stamp[to] = s_stamp[from];
}

/* Lazy block b of slot k as its source holds it (lines with no tags in either only cleared, if at all). */
static void CopyBlock(int k, unsigned b)
{
   const int from = s_lazy_from[k];
   const uint64_t tagged = s_tagged[from][b], stale = s_stale[k][b];
   unsigned c, e, x;
   for (c = 0; c < VBGO_TT_CELLS; c++)
   {
      const int copy = (int)(tagged >> c & 1);
      if (!copy && !(stale >> c & 1))
         continue;
      for (e = 0; e < 2; e++)
      {
         for (x = c * 8; x < c * 8 + 8; x++)
         {
            uint32_t *to = &s_tags[k][e][x * VBGO_TT_COLUMN + b * 8];
            const uint32_t *src = &s_tags[from][e][x * VBGO_TT_COLUMN + b * 8];
            ClearLine(to);
            if (copy)
            {
               to[0] = src[0], to[1] = src[1], to[2] = src[2], to[3] = src[3];
               to[4] = src[4], to[5] = src[5], to[6] = src[6], to[7] = src[7];
            }
         }
         for (x = 0; copy && x < 8; x++)
            memcpy(&s_cells[k][e][(b * 8 + x) * VBGO_TT_CELL_ROW + c * 8],
                   &s_cells[from][e][(b * 8 + x) * VBGO_TT_CELL_ROW + c * 8], 8 * sizeof(uint16_t));
      }
   }
   s_lazy[k] &= ~(1u << b);
}

static void Settle(int k)
{
   while (s_lazy[k])
      CopyBlock(k, (unsigned)__builtin_ctz(s_lazy[k]));
}

/* A free slot about to be reused: its lazy blocks are its own stale tags again. */
static void Drop(int k)
{
   unsigned b;
   for (b = 0; b < VBGO_TT_BLOCKS; b++)
      if (s_lazy[k] >> b & 1)
         s_tagged[k][b] = s_obj_cells[k][b] = s_filled_cells[k][b] = s_stale[k][b];
   s_lazy[k] = 0;
}

/* Before slot k's tags change: the slots still lazy from it copy what they need (free ones needn't). */
static void SettleFrom(int k)
{
   int j;
   for (j = 0; j < VBGO_TT_SLOTS; j++)
      if (s_lazy[j] && s_lazy_from[j] == k)
      {
         if (j == s_slot[0] || j == s_slot[1])
            Settle(j);
         else
            Drop(j);
      }
}

/* Before fb's tags change: a slot of its own (with the same tags; lazily: see s_lazy). */
static void Unshare(unsigned fb, int lazily)
{
   int from, to;
   if (s_slot[fb] != s_slot[fb ^ 1])
      return;
   ForgetCpuResets();
   from = s_slot[fb];
   to = FreeSlot();
   Settle(from); /* (no chains) */
   SettleFrom(to);
   Drop(to);

   if (lazily)
   {
      memcpy(s_stale[to], s_tagged[to], sizeof(s_stale[to]));
      s_lazy[to] = (1u << VBGO_TT_BLOCKS) - 1;
      s_lazy_from[to] = from;
   }
   else
   {
      memcpy(s_tags[to], s_tags[from], sizeof(s_tags[to]));
      memcpy(s_cells[to], s_cells[from], sizeof(s_cells[to]));
      s_lazy[to] = 0;
   }

   memcpy(s_tagged[to], s_tagged[from], sizeof(s_tagged[to]));
   memcpy(s_obj_cells[to], s_obj_cells[from], sizeof(s_obj_cells[to]));
   memcpy(s_filled_cells[to], s_filled_cells[from], sizeof(s_filled_cells[to]));
   memcpy(s_block_stamp[to], s_block_stamp[from], sizeof(s_block_stamp[to]));
   memcpy(s_band_pass[to], s_band_pass[from], sizeof(s_band_pass[to]));
   memcpy(s_band_bk0[to], s_band_bk0[from], sizeof(s_band_bk0[to]));
   memcpy(s_cpu_cells[to], s_cpu_cells[from], sizeof(s_cpu_cells[to]));
   memcpy(s_block_chr[to], s_block_chr[from], sizeof(s_block_chr[to]));
   memcpy(s_cell_version[to], s_cell_version[from], sizeof(s_cell_version[to]));
   memcpy(s_cell_black[to], s_cell_black[from], sizeof(s_cell_black[to]));
   memcpy(s_cell_worlds[to], s_cell_worlds[from], sizeof(s_cell_worlds[to]));
   memcpy(s_motion[to], s_motion[from], sizeof(s_motion[to]));
   memcpy(s_motion_cells[to], s_motion_cells[from], sizeof(s_motion_cells[to]));
   CopySlotInfo(to, from);
   s_slot[fb] = to;
}

/* New tracking settings: no tags drawn before can stand for this pass's. */
static void ForgetBlocks(void)
{
   ForgetCpuResets();
   s_settings_serial++;
   memset(s_block_stamp, 0, sizeof(s_block_stamp));
   memset(s_cell_version, 0, sizeof(s_cell_version));
   memset(s_cell_black, 0, sizeof(s_cell_black));
   memset(s_cell_worlds, 0xFF, sizeof(s_cell_worlds));
   memset(s_motion, 0, sizeof(s_motion));
   memset(s_motion_cells, 0, sizeof(s_motion_cells));
   memset(s_band_bk0, 0, sizeof(s_band_bk0));
}

/* The tiles as last hashed, with their hashes and blank flags. */
static uint16_t s_hashed_chr[2048 * 8];
static uint32_t s_hashed[2048];
static uint8_t s_hashed_blank[2048];
static int s_hashed_any;
static uint32_t s_hashed_at, s_hashed_epoch; /* the serial (and epoch) they are up to date with */

/* The pass's copies of the character memory, its hashes and the sprite attributes: whole, or - when the slot and hashes
 * were brought up to date in this epoch - only the characters and sprites the core stamped since. */
static void StartPass(unsigned fb, const uint16_t *chr_ram, const uint16_t *dram, const vbgo_tt_serials *serials)
{
   int c, slot, sparse;
   s_prev_stamp[fb] = s_have[fb] ? s_stamp[s_slot[fb]] : 0;
   Unshare(fb, 1); /* (a slot of its own, holding the tags its last pass repeated) */
   slot = s_slot[fb];
   SettleFrom(slot);
   s_motion_noted[slot] = 0;

   sparse = serials && dram && s_have_attrs[slot] && s_sync_epoch[slot] == serials->epoch && s_hashed_any &&
            s_hashed_epoch == serials->epoch;
   s_prev_valid = s_prev_stamp[fb] && s_stamp[slot] == s_prev_stamp[fb] && s_have_attrs[slot];
   if (s_prev_valid && !sparse)
   {
      memcpy(s_prev_hash, s_hash[slot], sizeof(s_prev_hash));
      memcpy(s_prev_oam, s_oam[slot], sizeof(s_prev_oam));
   }

   NextStamp(-1);
   s_stamp[slot] = s_next_stamp;
   s_settings[slot] = s_settings_serial;
   if (!sparse)
      memcpy(s_chr[slot], chr_ram, sizeof(s_chr[slot]));
   s_have_attrs[slot] = dram != NULL;
   if (dram)
   {
      memcpy(s_worlds[slot], &dram[0x1D800 >> 1], sizeof(s_worlds[slot]));
      if (!sparse)
         memcpy(s_oam[slot], &dram[0x1E000 >> 1], sizeof(s_oam[slot]));
   }

   if (sparse)
   {
      /* (a character or sprite the core didn't stamp since is as it was; its hash too) */
      for (c = 0; c < 2048; c++)
         if (serials->chr_at[c] > s_hashed_at && memcmp(&chr_ram[c * 8], &s_hashed_chr[c * 8], 16))
         {
            const uint16_t *rows = &chr_ram[c * 8];
            memcpy(&s_hashed_chr[c * 8], rows, 16);
            s_hashed[c] = HashChar(rows);
            s_hashed_blank[c] = (rows[0] | rows[1] | rows[2] | rows[3] | rows[4] | rows[5] | rows[6] | rows[7]) == 0;
         }
   }
   else
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
   s_hashed_at = serials ? serials->serial : 0;
   s_hashed_epoch = serials ? serials->epoch : 0;
   if (!sparse)
   {
      memcpy(s_hash[slot], s_hashed, sizeof(s_hash[slot]));
      memcpy(s_blank[slot], s_hashed_blank, sizeof(s_blank[slot]));
   }

   memset(s_chr_changed, 0, sizeof(s_chr_changed));
   memset(s_obj_changed, 0, sizeof(s_obj_changed));
   if (sparse)
   {
      const uint32_t at = s_sync_at[slot];
      for (c = 0; c < 2048; c++)
         if (serials->chr_at[c] > at)
         {
            memcpy(&s_chr[slot][c * 8], &chr_ram[c * 8], 16);
            if (s_prev_valid && s_hash[slot][c] != s_hashed[c])
               s_chr_changed[c >> 5] |= 1u << (c & 31);
            s_hash[slot][c] = s_hashed[c];
            s_blank[slot][c] = s_hashed_blank[c];
         }
      for (c = 0; c < 1024; c++)
         if (serials->oam_at[c] > at && memcmp(&s_oam[slot][c * 4], &dram[(0x1E000 >> 1) + c * 4], 8))
         {
            if (s_prev_valid)
               s_obj_changed[c >> 5] |= 1u << (c & 31);
            memcpy(&s_oam[slot][c * 4], &dram[(0x1E000 >> 1) + c * 4], 8);
         }
   }
   else if (s_prev_valid)
   {
      for (c = 0; c < 2048; c += 32)
      {
         uint32_t bits = 0;
         unsigned j;
         for (j = 0; j < 32; j++)
            bits |= (uint32_t)(s_hash[slot][c + j] != s_prev_hash[c + j]) << j;
         s_chr_changed[c >> 5] = bits;
      }
      for (c = 0; c < 1024; c += 32)
      {
         uint32_t bits = 0;
         unsigned j;
         for (j = 0; j < 32; j++)
            bits |= (uint32_t)(memcmp(&s_oam[slot][(c + j) * 4], &s_prev_oam[(c + j) * 4], 8) != 0) << j;
         s_obj_changed[c >> 5] = bits;
      }
   }
   s_sync_at[slot] = serials ? serials->serial : 0;
   s_sync_epoch[slot] = serials ? serials->epoch : 0;
   s_chr_any_changed = 0;
   for (c = 0; c < 2048 / 32; c++)
      s_chr_any_changed |= s_chr_changed[c] != 0;

   /* (Tracking switched on mid-pass: the blocks before this one simply have no tags with this stamp.) */
   s_have[fb] = 1;
   s_cpu[fb] = 0; /* (the pass draws every pixel of the buffer anew) */
   s_pass_open = 1;
}

void vbgo_tiletrack_set_enabled(bool enabled)
{
   if (!enabled && vbgo_tt_on)
   {
      /* What was tracked goes stale while off: nothing to show until a frame drawn with tracking on is displayed. */
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
   ForgetCpuResets();
   memset(s_tags, 0, sizeof(s_tags));
   memset(s_lazy, 0, sizeof(s_lazy));
   memset(s_block_stamp, 0, sizeof(s_block_stamp));
   memset(s_band_pass, 0, sizeof(s_band_pass));
   memset(s_band_bk0, 0, sizeof(s_band_bk0));
   memset(s_cell_version, 0, sizeof(s_cell_version));
   memset(s_cell_black, 0, sizeof(s_cell_black));
   memset(s_cell_worlds, 0xFF, sizeof(s_cell_worlds));
   memset(s_motion, 0, sizeof(s_motion));
   memset(s_motion_cells, 0, sizeof(s_motion_cells));
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
   ForgetCpuResets();
   Unshare(fb & 1, 0);
   Settle(s_slot[fb & 1]);
   SettleFrom(s_slot[fb & 1]);
   memset(s_tags[s_slot[fb & 1]][lr & 1], 0, sizeof(s_tags[0][0]));
   memset(s_block_stamp[s_slot[fb & 1]], 0, sizeof(s_block_stamp[0]));
   memset(s_cell_version[s_slot[fb & 1]], 0, sizeof(s_cell_version[0]));
   memset(s_cell_black[s_slot[fb & 1]], 0, sizeof(s_cell_black[0]));
   memset(s_cell_worlds[s_slot[fb & 1]], 0xFF, sizeof(s_cell_worlds[0]));
   memset(s_motion[s_slot[fb & 1]], 0, sizeof(s_motion[0]));
   memset(s_motion_cells[s_slot[fb & 1]], 0, sizeof(s_motion_cells[0]));
   memset(s_band_bk0[s_slot[fb & 1]], 0, sizeof(s_band_bk0[0]));
   s_cpu[fb & 1] = 1;
}

void vbgo_tiletrack_forget_motion(void)
{
   memset(s_motion, 0, sizeof(s_motion));
   memset(s_motion_cells, 0, sizeof(s_motion_cells));
   memset(s_band_bk0, 0, sizeof(s_band_bk0));
}

void vbgo_tiletrack_set_fill_mode(int mode)
{
   if (vbgo_tt_fill != mode)
      ForgetBlocks();
   vbgo_tt_fill = mode;
}

void vbgo_tiletrack_set_fill_cells(const uint8_t *bits)
{
   ForgetBlocks();
   if (bits)
      memcpy(vbgo_tt_fill_cells, bits, sizeof(vbgo_tt_fill_cells));
   else
      memset(vbgo_tt_fill_cells, 0, sizeof(vbgo_tt_fill_cells));
}

void vbgo_tiletrack_begin_block(const uint8_t *drawing_buffers, const uint16_t *chr_ram, const uint16_t *dram,
                                unsigned block_no, unsigned fb, const uint8_t *jplt, const vbgo_tt_serials *serials)
{
   ForgetCpuResets();
   if (!vbgo_tt_on)
      return;
   fb &= 1;

   /* Once per pass (games update tile graphics between frames), or right after tracking was switched on mid-pass. */
   if (block_no == 0 || !s_pass_open || (int)fb != s_last_fb)
   {
      unsigned p;
      StartPass(fb, chr_ram, dram, serials);
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
   vbgo_tt_cdst[0] = &s_cells[s_slot[fb]][0][block_no * 8 * VBGO_TT_CELL_ROW];
   vbgo_tt_cdst[1] = &s_cells[s_slot[fb]][1][block_no * 8 * VBGO_TT_CELL_ROW];
   vbgo_tt_world = 0;
   s_block_slot = s_slot[fb];
   s_block_no = block_no;

   if (block_no < VBGO_TT_BLOCKS)
   {
      /* (drawn or kept, the block's tags are this pass's from here on) */
      s_block_was = s_block_stamp[s_slot[fb]][block_no] == s_prev_stamp[fb] ? s_prev_stamp[fb] : 0;
      s_block_stamp[s_slot[fb]][block_no] = s_band_pass[s_slot[fb]][block_no] = s_stamp[s_slot[fb]];
      s_band_bk0[s_slot[fb]][block_no] = vbgo_tt_bkcol == 0;
      s_cpu_cells[s_slot[fb]][block_no] = 0;
   }
   else
      s_block_was = 0;
}

/* Before drawing into the block begun last: clear these cells' tags in both eyes. */
static void ClearCells(uint64_t cells)
{
   unsigned x;
   /* (cell lines span two 8x8 cells: claimed where both are cleared; the pointers are kept in locals so the clears
    * don't reload them) */
   uint64_t pairs = cells & (cells >> 1) & 0x5555555555555555ull;
   uint32_t *const tags0 = vbgo_tt_dst[0], *const tags1 = vbgo_tt_dst[1];
   uint16_t *const cells0 = vbgo_tt_cdst[0], *const cells1 = vbgo_tt_cdst[1];
   memset(vbgo_tt_ocells, 0, sizeof(vbgo_tt_ocells));
   for (; cells; cells &= cells - 1)
   {
      const unsigned c = (unsigned)__builtin_ctzll(cells);
      uint32_t *line0 = tags0 + c * 8 * VBGO_TT_COLUMN, *line1 = tags1 + c * 8 * VBGO_TT_COLUMN;
      for (x = 0; x < 8; x++, line0 += VBGO_TT_COLUMN, line1 += VBGO_TT_COLUMN)
      {
         ClearLine(line0);
         ClearLine(line1);
      }
   }
   for (; pairs; pairs &= pairs - 1)
   {
      const unsigned c = (unsigned)__builtin_ctzll(pairs);
      uint16_t *line0 = cells0 + c * 8, *line1 = cells1 + c * 8;
      for (x = 0; x < 8; x++, line0 += VBGO_TT_CELL_ROW, line1 += VBGO_TT_CELL_ROW)
      {
         ClearLine(line0);
         ClearLine(line1);
      }
   }
}

/* The block begun last as its pass holds it (kept). */
static void Materialize(void)
{
   if (s_block_no < VBGO_TT_BLOCKS && (s_lazy[s_block_slot] >> s_block_no & 1))
      CopyBlock(s_block_slot, s_block_no);
}

bool vbgo_tiletrack_keep_block(void)
{
   if (!vbgo_tt_on)
      return true;
   if (s_block_was)
      Materialize();
   return s_block_was != 0;
}

bool vbgo_tiletrack_adopt_block(unsigned fb)
{
   int to, from;
   unsigned b, c, e, x;
   uint64_t clear, copy;
   ForgetCpuResets();
   if (!vbgo_tt_on)
      return true;
   fb &= 1;
   to = s_slot[fb];
   from = s_slot[fb ^ 1];
   b = s_block_no;
   if (to == from || s_block_slot != to || b >= VBGO_TT_BLOCKS || !s_have[fb ^ 1] ||
       s_block_stamp[from][b] != s_stamp[from] || s_settings[from] != s_settings[to])
      return false;
   if (s_lazy[from] >> b & 1)
      CopyBlock(from, b);
   SettleFrom(to);
   /* (as CopyBlock: lines with no tags in either only cleared, if at all; a lazy block's own are its stale ones) */
   clear = (s_lazy[to] >> b & 1) ? s_stale[to][b] : s_tagged[to][b];
   copy = s_tagged[from][b];
   s_lazy[to] &= ~(1u << b);
   for (c = 0; c < VBGO_TT_CELLS; c++)
   {
      if (!((clear | copy) >> c & 1))
         continue;
      for (e = 0; e < 2; e++)
      {
         for (x = c * 8; x < c * 8 + 8; x++)
         {
            uint32_t *const line = &s_tags[to][e][x * VBGO_TT_COLUMN + b * 8];
            ClearLine(line);
            if (copy >> c & 1)
               vbgo_copy8(line, &s_tags[from][e][x * VBGO_TT_COLUMN + b * 8], 4);
         }
         for (x = 0; (copy >> c & 1) && x < 8; x++)
            vbgo_copy8(&s_cells[to][e][(b * 8 + x) * VBGO_TT_CELL_ROW + c * 8],
                       &s_cells[from][e][(b * 8 + x) * VBGO_TT_CELL_ROW + c * 8], 2);
      }
   }
   /* (the versions too: equal ones mean the same tags and pixels, so a picture painted from either buffer serves both;
    * the records were made against the other pass, so none holds) */
   memcpy(s_block_chr[to][b], s_block_chr[from][b], sizeof(s_block_chr[0][0]));
   memcpy(s_cell_version[to][b], s_cell_version[from][b], sizeof(s_cell_version[0][0]));
   s_cell_black[to][b] = s_cell_black[from][b];
   memcpy(s_cell_worlds[to][b], s_cell_worlds[from][b], sizeof(s_cell_worlds[0][0]));
   s_tagged[to][b] = s_tagged[from][b];
   s_obj_cells[to][b] = s_obj_cells[from][b];
   s_filled_cells[to][b] = s_filled_cells[from][b];
   memset(s_motion[to][b], 0, sizeof(s_motion[0][0]));
   s_motion_cells[to][b] = 0;
   return true;
}

bool vbgo_tiletrack_keep_cells(uint64_t cells)
{
   if (!vbgo_tt_on)
      return true;
   if (!s_block_was)
      return false;
   Materialize();
   /* (the cells drawn again start clear, the kept ones stay as they are) */
   ClearCells(s_tagged[s_block_slot][s_block_no] & ~cells);
   s_tagged[s_block_slot][s_block_no] &= cells;
   s_obj_cells[s_block_slot][s_block_no] &= cells;
   s_filled_cells[s_block_slot][s_block_no] &= cells;
   return true;
}

void vbgo_tiletrack_block_drawing(bool compare)
{
   unsigned e;
   uint64_t tagged;
   int lazy;

   s_old_valid = 0;
   s_aside = 0;
   if (!vbgo_tt_on || s_block_slot < 0 || s_block_no >= VBGO_TT_BLOCKS)
      return;

   s_old_valid = compare && s_block_was && s_prev_valid;
   tagged = s_tagged[s_block_slot][s_block_no];
   /* (a lazy block's old tags are still in its source; its own stale ones are what's cleared) */
   lazy = (int)(s_lazy[s_block_slot] >> s_block_no & 1);
   if (s_old_valid)
   {
      /* (drawn aside, past the band: the old tags stay where they are for SameCell, and block_drawn puts the new ones
       * in their place) */
      for (e = 0; e < 2; e++)
      {
         s_aside_tags[e] = lazy ? &s_tags[s_lazy_from[s_block_slot]][e][s_block_no * 8] : vbgo_tt_dst[e];
         s_aside_cells[e] = lazy ? &s_cells[s_lazy_from[s_block_slot]][e][s_block_no * 8 * VBGO_TT_CELL_ROW]
                                 : vbgo_tt_cdst[e];
         vbgo_tt_dst[e] = &s_tags[s_block_slot][e][VBGO_TT_HEIGHT];
         vbgo_tt_cdst[e] = &s_cells[s_block_slot][e][VBGO_TT_HEIGHT * VBGO_TT_CELL_ROW];
      }
      s_aside = 1;
      s_aside_clear = lazy ? s_stale[s_block_slot][s_block_no] : tagged;
      s_aside_lazy = lazy;
      memset(vbgo_tt_ocells, 0, sizeof(vbgo_tt_ocells));
   }
   else
   {
      ClearCells(lazy ? s_stale[s_block_slot][s_block_no] : tagged);
      s_lazy[s_block_slot] &= ~(1u << s_block_no);
   }
   s_tagged[s_block_slot][s_block_no] = 0;
   s_obj_cells[s_block_slot][s_block_no] = 0;
   s_filled_cells[s_block_slot][s_block_no] = 0;
}

/* Whether cell c's tags show what they showed before the drawing: the same
 * tags (0 where nothing was drawn), tiles that hash the same, sprites with the same attributes. */
static __attribute__((noinline)) int SameCell(unsigned c)
{
   /* (out of line: inlined in VIP_Update it spilled three values a pixel. Any failing test answers 0, so a run of
    * one tile's pixels asks once whether the tile changed, and only a column with a sprite asks about sprites) */
   const uint32_t *const chr_changed = s_block_chr_changed ? s_chr_changed : NULL;
   unsigned e, x, y;
   for (e = 0; e < 2; e++)
   {
      const uint32_t *was = s_aside_tags[e] + c * 8 * VBGO_TT_COLUMN, *is = vbgo_tt_dst[e] + c * 8 * VBGO_TT_COLUMN;
      const uint16_t *was_cell = s_aside_cells[e] + c * 8, *is_cell = vbgo_tt_cdst[e] + c * 8;
      uint32_t known = ~0u; /* (a tile found unchanged) */
      for (x = 0; x < 8; x++, was += VBGO_TT_COLUMN, is += VBGO_TT_COLUMN, was_cell++, is_cell++)
      {
         uint32_t tags[8], differ = 0, any = 0;
         for (y = 0; y < 8; y++)
            tags[y] = is[y], differ |= was[y] ^ tags[y], any |= tags[y];
         /* (the column's 8 tags at once: most cells that differ differ there) */
         if (differ)
            return 0;
#pragma GCC unroll 8
         for (y = 0; y < 8; y++)
         {
            const uint32_t tag = tags[y];
            if (!tag)
               continue;
            if (was_cell[y * VBGO_TT_CELL_ROW] != is_cell[y * VBGO_TT_CELL_ROW])
               return 0;
            if (chr_changed && VBGO_TAG_CHAR(tag) != known)
            {
               if (chr_changed[VBGO_TAG_CHAR(tag) >> 5] >> (VBGO_TAG_CHAR(tag) & 31) & 1)
                  return 0;
               known = VBGO_TAG_CHAR(tag);
            }
         }
         if (VBGO_TAG_IS_OBJ(any))
            for (y = 0; y < 8; y++)
            {
               const uint64_t t = vbgo_tag_compose(tags[y], is_cell[y * VBGO_TT_CELL_ROW]);
               if (VBGO_TAG_IS_OBJ(t) && (s_obj_changed[VBGO_TAG_OBJ_NO(t) >> 5] >> (VBGO_TAG_OBJ_NO(t) & 31) & 1))
                  return 0;
            }
      }
   }
   return 1;
}

/* The block drawn aside goes in its place and its rows are zero again; the cells found the same keep their lines, which
 * already hold their tags (not if lazy: they hold stale ones). */
static void PutAside(uint64_t same)
{
   const uint64_t tagged = s_tagged[s_block_slot][s_block_no];
   const uint64_t skip = s_aside_lazy ? 0 : same;
   const uint64_t put = (s_aside_clear | tagged) & ~skip, copy = tagged & ~skip;
   /* (cell lines span two 8x8 cells: claimed where both are put) */
   uint64_t pairs = copy & (copy >> 1) & 0x5555555555555555ull, m;
   uint32_t *const to_tags[2] = { &s_tags[s_block_slot][0][s_block_no * 8], &s_tags[s_block_slot][1][s_block_no * 8] };
   uint16_t *const to_cells[2] = { &s_cells[s_block_slot][0][s_block_no * 8 * VBGO_TT_CELL_ROW],
                                   &s_cells[s_block_slot][1][s_block_no * 8 * VBGO_TT_CELL_ROW] };
   uint32_t *const aside[2] = { vbgo_tt_dst[0], vbgo_tt_dst[1] };
   const uint16_t *const aside_cells[2] = { vbgo_tt_cdst[0], vbgo_tt_cdst[1] };
   unsigned e, x, y;

   for (m = put | tagged; m; m &= m - 1)
   {
      const unsigned c = (unsigned)__builtin_ctzll(m);
      const int mine = (int)(put >> c & 1), full = (int)(tagged >> c & 1);
      for (e = 0; e < 2; e++)
         for (x = c * 8; x < c * 8 + 8; x++)
         {
            uint32_t *const to = to_tags[e] + x * VBGO_TT_COLUMN, *const from = aside[e] + x * VBGO_TT_COLUMN;
            if (mine)
            {
               ClearLine(to);
               if (full)
                  vbgo_copy8(to, from, 4);
            }
            if (full)
               ClearLine(from);
         }
   }
   for (m = copy; m; m &= m - 1)
   {
      const unsigned c = (unsigned)__builtin_ctzll(m);
      const int claim = (int)(pairs >> (c & ~1u) & 1);
      for (e = 0; e < 2; e++)
         for (y = 0; y < 8; y++)
         {
            uint16_t *const to = to_cells[e] + y * VBGO_TT_CELL_ROW + c * 8;
            if (claim && !(c & 1))
               ClearLine(to);
            vbgo_copy8(to, aside_cells[e] + y * VBGO_TT_CELL_ROW + c * 8, 2);
         }
   }
   s_lazy[s_block_slot] &= ~(1u << s_block_no);
   s_aside = 0;
}

/* Whether cell c, both eyes, shows nothing the colorizer colors: every pixel in shade 0 (black at
 * any brightness) and no fill tagged there - what's left was drawn black or not drawn by this pass. */
static int BlackCell(unsigned c)
{
   unsigned e;
   if (vbgo_tt_fillc[c])
      return 0;
   for (e = 0; e < 2; e++)
   {
      /* (the pixels as the drawing packed them into the frame buffer's band: a few cache lines) */
      const uint32_t *w = vbgo_vip_block_pixels((unsigned)s_last_fb, e, s_block_no) + c * 4;
      if (w[0] | w[1] | w[2] | w[3])
         return 0;
   }
   return 1;
}

void vbgo_tiletrack_block_drawn(const uint32_t *chr, bool settled, uint64_t same_pixels, uint64_t unchanged)
{
   ForgetCpuResets();
   uint32_t *version, fresh = 0;
   uint64_t was_black, black = 0, same = 0;
   unsigned c;

   if (!vbgo_tt_on || s_block_slot < 0 || s_block_no >= VBGO_TT_BLOCKS)
      return;

   memcpy(s_block_chr[s_block_slot][s_block_no], chr, sizeof(s_block_chr[0][0]));
   {
      /* (the cells' flags as bits, 32 and 16 at a time: 64-bit shifts cost much more on the Wii) */
      uint32_t tagged_lo = 0, tagged_hi = 0, obj_lo = 0, obj_hi = 0, fill_lo = 0, fill_hi = 0;
      for (c = 0; c < 32; c++)
         tagged_lo |= (uint32_t)((vbgo_tt_wcells[c] | vbgo_tt_ocells[c]) != 0) << c,
         obj_lo |= (uint32_t)(vbgo_tt_ocells[c] != 0) << c,
         fill_lo |= (uint32_t)(vbgo_tt_fillc[c] != 0) << c;
      for (; c < VBGO_TT_CELLS; c++)
         tagged_hi |= (uint32_t)((vbgo_tt_wcells[c] | vbgo_tt_ocells[c]) != 0) << (c - 32),
         obj_hi |= (uint32_t)(vbgo_tt_ocells[c] != 0) << (c - 32),
         fill_hi |= (uint32_t)(vbgo_tt_fillc[c] != 0) << (c - 32);
      s_tagged[s_block_slot][s_block_no] |= (uint64_t)tagged_hi << 32 | tagged_lo;
      s_obj_cells[s_block_slot][s_block_no] |= (uint64_t)obj_hi << 32 | obj_lo;
      s_filled_cells[s_block_slot][s_block_no] |= (uint64_t)fill_hi << 32 | fill_lo;
   }
   version = s_cell_version[s_block_slot][s_block_no];
   s_block_chr_changed = 0;
   for (c = 0; s_chr_any_changed && c < 2048 / 32; c++)
      s_block_chr_changed |= (chr[c] & s_chr_changed[c]) != 0;

   if (settled)
   {
      if (++s_versions == 0)
         s_versions = 1;
      fresh = s_versions;
   }

   was_black = s_cell_black[s_block_slot][s_block_no];
   {
      /* (only where the pixels came out the same; the 64-bit masks in 32-bit halves) */
      const uint64_t asked = same_pixels & ~unchanged & ((1ull << VBGO_TT_CELLS) - 1);
      uint32_t half[2] = { (uint32_t)asked, (uint32_t)(asked >> 32) };
      unsigned w;
      for (w = 0; w < 2; w++)
         for (; half[w]; half[w] &= half[w] - 1)
         {
            c = w * 32 + (unsigned)__builtin_ctz(half[w]);
            if (BlackCell(c)) /* (elsewhere 0: only a cell redrawn the same next time is asked) */
               black |= 1ull << c;
         }
   }
   s_cell_black[s_block_slot][s_block_no] = black;

   {
      /* (the cells that keep their version: nothing it shows could have changed, or its tags came out the same, or it
       * was black and is) */
      const uint64_t compared = s_old_valid ? same_pixels & ~unchanged : 0, blacks = same_pixels & was_black & black;
      const uint32_t keep_unchanged[2] = { (uint32_t)unchanged, (uint32_t)(unchanged >> 32) };
      const uint32_t keep_asked[2] = { (uint32_t)compared, (uint32_t)(compared >> 32) };
      const uint32_t keep_black[2] = { (uint32_t)blacks, (uint32_t)(blacks >> 32) };
      uint32_t stale[2] = { 0, 0 }; /* (cells whose record can no longer hold) */
      for (c = 0; c < VBGO_TT_CELLS; c++)
      {
         const unsigned w = c >> 5;
         const uint32_t bit = 1u << (c & 31);
         if (!(settled && s_block_was && version[c] &&
               ((keep_unchanged[w] & bit) || ((keep_asked[w] & bit) && SameCell(c) && (same |= 1ull << c)))))
         {
            if (settled && s_block_was && version[c] && (keep_black[w] & bit))
               /* (kept black: its tags may differ from its record's) */
               s_motion[s_block_slot][s_block_no][c].version = 0;
            else
               version[c] = fresh;
            stale[w] |= bit;
         }
      }
      s_motion_cells[s_block_slot][s_block_no] &= ~((uint64_t)stale[1] << 32 | stale[0]);
   }

   if (s_aside)
      PutAside(same);
   s_old_valid = 0;
}

uint32_t vbgo_tt_wcells[48];
uint32_t vbgo_tt_copy_bands = ~0u;
uint8_t vbgo_tt_fillc[48];
uint8_t vbgo_tt_ocells[48];

uint64_t vbgo_tiletrack_world_cells(uint32_t changed, uint64_t cells)
{
   uint32_t todo[2], maybe[2] = { 0, 0 }, *worlds;
   unsigned c, w;
   if (!vbgo_tt_on || s_block_slot < 0 || s_block_no >= VBGO_TT_BLOCKS)
      return cells;
   worlds = s_cell_worlds[s_block_slot][s_block_no];
   cells &= (1ull << VBGO_TT_CELLS) - 1;
   todo[0] = (uint32_t)cells, todo[1] = (uint32_t)(cells >> 32);
   for (w = 0; w < 2; w++)
      for (; todo[w]; todo[w] &= todo[w] - 1)
      {
         c = w * 32 + (unsigned)__builtin_ctz(todo[w]);
         if (!s_block_was || ((worlds[c] | vbgo_tt_wcells[c]) & changed))
            maybe[w] |= 1u << (c & 31);
         worlds[c] = vbgo_tt_wcells[c];
      }
   return (uint64_t)maybe[1] << 32 | maybe[0];
}

uint64_t vbgo_tiletrack_world_cells_were(uint32_t worlds)
{
   uint64_t cells = 0;
   const uint32_t *was;
   unsigned c;
   if (!vbgo_tt_on || s_block_slot < 0 || s_block_no >= VBGO_TT_BLOCKS || !s_block_was)
      return ~0ull;
   was = s_cell_worlds[s_block_slot][s_block_no];
   for (c = 0; c < VBGO_TT_CELLS; c++)
      cells |= (uint64_t)((was[c] & worlds) != 0) << c;
   return cells;
}

/* Cell c's bits (at `at`) of note_motion's masks, without branches: given the drawing's new version (the rest are
 * kept), and per offset (worlds0, worlds1) shown only by worlds that moved by it, now and in the other pass. */
#define VBGO_MOTION_BITS(c, at)                                                                             \
   do                                                                                                       \
   {                                                                                                        \
      const uint32_t shown = vbgo_tt_wcells[c] & ~still, then = was_worlds[c];                              \
      fresh_bits |= (uint32_t)(version[c] == fresh) << (at);                                                \
      now0 |= (uint32_t)((shown != 0) & ((shown & ~worlds0) == 0)) << (at);                                 \
      src0 |= (uint32_t)((then & ~worlds0) == 0) << (at);                                                   \
      if (n > 1)                                                                                            \
      {                                                                                                     \
         now1 |= (uint32_t)((shown != 0) & ((shown & ~worlds1) == 0)) << (at);                              \
         src1 |= (uint32_t)((then & ~worlds1) == 0) << (at);                                                \
      }                                                                                                     \
   } while (0)

unsigned vbgo_tiletrack_note_motion(uint32_t moved, uint32_t still, const int8_t *dx, const uint64_t *bad)
{
   const unsigned b = s_block_no;
   const int slot = s_block_slot;
   int from, d[2];
   const uint32_t *version, *was, *was_worlds, fresh = s_versions;
   uint32_t todo = moved, worlds0 = 0, worlds1 = 0, half[2];
   uint64_t bad_here[2] = { 0, 0 }, now[2], src[2], cells[2], all = 0, claimed;
   uint64_t unmade = 0; /* (cells of `all` given no record) */
   vbgo_tt_motion *rec;
   unsigned g, n, c, k, made = 0;
   if (!vbgo_tt_on || slot < 0 || b >= VBGO_TT_BLOCKS || s_last_fb < 0 || !fresh)
      return 0;
   from = s_slot[s_last_fb ^ 1];
   if (from == slot || !s_have[s_last_fb ^ 1] || (s_lazy[from] >> b & 1) || s_band_pass[from][b] != s_stamp[from])
      return 0;
   version = s_cell_version[slot][b], was = s_cell_version[from][b], was_worlds = s_cell_worlds[from][b];
   rec = s_motion[slot][b];

   /* (the offsets, up to two (the rest record nothing), each with its worlds) */
   for (n = 0; todo && n < 2; n++)
   {
      uint32_t w, group = still;
      d[n] = dx[__builtin_ctz(todo)];
      for (w = todo; w; w &= w - 1)
         if (dx[__builtin_ctz(w)] == d[n])
            group |= 1u << __builtin_ctz(w), bad_here[n] |= bad[__builtin_ctz(w)];
      todo &= ~group;
      if (n)
         worlds1 = group;
      else
         worlds0 = group;
   }
   {
      uint32_t fresh_bits = 0, now0 = 0, src0 = 0, now1 = 0, src1 = 0;
      for (c = 0; c < 32; c++)
         VBGO_MOTION_BITS(c, c);
      now[0] = now0, src[0] = src0, now[1] = now1, src[1] = src1, cells[0] = fresh_bits;
      fresh_bits = now0 = src0 = now1 = src1 = 0;
      for (; c < VBGO_TT_CELLS; c++)
         VBGO_MOTION_BITS(c, c - 32);
      now[0] |= (uint64_t)now0 << 32, src[0] |= (uint64_t)src0 << 32;
      now[1] |= (uint64_t)now1 << 32, src[1] |= (uint64_t)src1 << 32;
      cells[0] |= (uint64_t)fresh_bits << 32;
   }

   /* (per offset: source columns in cell k0, and in k1 too where they span two) */
   {
      const uint64_t base = cells[0] & ~(s_obj_cells[slot][b] | s_filled_cells[slot][b]);
      const uint64_t from_other = s_obj_cells[from][b] | s_filled_cells[from][b];
      for (g = 0; g < n; g++)
      {
         const int k0 = d[g] >> 3, k1 = (d[g] + 7) >> 3;
         const uint64_t from_cells = src[g] & ~from_other;
         uint64_t ok = now[g] & base & ~bad_here[g];
         ok &= k0 >= 0 ? from_cells >> k0 : from_cells << -k0;
         ok &= k1 >= 0 ? from_cells >> k1 : from_cells << -k1;
         cells[g] = ok, all |= ok;
      }
   }

   /* (a record line holds two cells: claimed whole where either may have one, the other then left 0) */
   half[0] = (uint32_t)(all | all >> 1) & 0x55555555u, half[1] = (uint32_t)((all | all >> 1) >> 32) & 0x5555u;
   claimed = (uint64_t)(half[1] | half[1] << 1) << 32 | (half[0] | half[0] << 1);
   for (k = 0; k < 2; k++)
      for (; half[k]; half[k] &= half[k] - 1)
         ClearLine(&rec[k * 32 + (unsigned)__builtin_ctz(half[k])]);
   for (g = 0; g < n; g++)
   {
      half[0] = (uint32_t)cells[g], half[1] = (uint32_t)(cells[g] >> 32);
      for (k = 0; k < 2; k++)
         for (; half[k]; half[k] &= half[k] - 1)
         {
            int sx;
            uint32_t was0, was1;
            c = k * 32 + (unsigned)__builtin_ctz(half[k]);
            sx = (int)c * 8 + d[g];
            was0 = was[(unsigned)sx >> 3], was1 = was[(unsigned)(sx + 7) >> 3];
            if (!was0 || !was1) /* (the other pass's cells it shows need versions) */
            {
               unmade |= 1ull << c;
               continue;
            }
            rec[c].version = version[c];
            rec[c].from[0] = was0;
            rec[c].from[1] = was1;
            rec[c].stamp = s_stamp[from];
            rec[c].dx = (int8_t)d[g];
            made++;
         }
   }
   s_motion_cells[slot][b] = (s_motion_cells[slot][b] & ~claimed) | (all & ~unmade);
   s_motion_noted[slot] |= (uint32_t)(made != 0) << b;
   return made;
}
#undef VBGO_MOTION_BITS

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
   ForgetCpuResets();
   unsigned e, x;
   int from, to;
   uint16_t old_stamp;

   fb &= 1;
   if (!vbgo_tt_on)
      return;

   from = s_slot[fb];
   to = s_slot[fb ^ 1] == from ? FreeSlot() : from;
   Settle(from);
   SettleFrom(to);
   if (to != from)
      Drop(to);

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
         s_band_bk0[to][e] = s_band_bk0[from][e];
         s_cpu_cells[to][e] = s_cpu_cells[from][e];
         s_tagged[to][e] = s_tagged[from][e];
         s_obj_cells[to][e] = s_obj_cells[from][e];
         s_filled_cells[to][e] = s_filled_cells[from][e];
         memcpy(s_block_chr[to][e], s_block_chr[from][e], sizeof(s_block_chr[to][e]));
         memcpy(s_cell_version[to][e], s_cell_version[from][e], sizeof(s_cell_version[to][e]));
         s_cell_black[to][e] = s_cell_black[from][e];
         memcpy(s_cell_worlds[to][e], s_cell_worlds[from][e], sizeof(s_cell_worlds[to][e]));
         memcpy(s_motion[to][e], s_motion[from][e], sizeof(s_motion[to][e]));
         s_motion_cells[to][e] = s_motion_cells[from][e];
      }
   }

   for (e = block_no; to != from && e < VBGO_TT_BLOCKS; e++)
      s_block_stamp[to][e] = 0; /* (leftovers of whatever pass last used the slot) */

   /* (the blocks it repeated: the other buffer's tags, as this pass's) */
   for (x = 0; to != from && x < VBGO_TT_WIDTH; x++)
   {
      const unsigned rows = (block_no < VBGO_TT_BLOCKS ? block_no : VBGO_TT_BLOCKS) * 8;
      for (e = 0; e < 2; e++)
         memcpy(&s_tags[to][e][x * VBGO_TT_COLUMN], &s_tags[from][e][x * VBGO_TT_COLUMN], rows * sizeof(uint32_t));
   }
   for (e = 0; to != from && e < 2; e++)
      memcpy(s_cells[to][e], s_cells[from][e],
             (block_no < VBGO_TT_BLOCKS ? block_no : VBGO_TT_BLOCKS) * 8 * VBGO_TT_CELL_ROW * sizeof(uint16_t));

   s_have[fb] = 1;
   s_cpu[fb] = 0;
   s_pass_open = 1;
   s_last_fb = (int)fb;
}

void vbgo_tiletrack_display_column(unsigned fb, unsigned lr, unsigned dest_lr, unsigned column, bool display_active)
{
   if (!vbgo_tt_on || column >= VBGO_TT_WIDTH)
      return;
   const uint8_t shown = display_active ? (uint8_t)((fb & 1) | ((lr & 1) << 1) | 4) : 0;
   if (s_disp[dest_lr & 1][column] != shown)
      s_disp[dest_lr & 1][column] = shown;
   if (s_disp_clut[dest_lr & 1][column] != vbgo_tt_clut_serial) /* (a line left clean is not written back) */
      s_disp_clut[dest_lr & 1][column] = vbgo_tt_clut_serial;
}

void vbgo_tiletrack_cpu_fb_write(unsigned fb, unsigned lr, unsigned offset, unsigned bytes)
{
   /* Pixels the game's CPU writes straight into the frame buffer aren't drawn from tiles, so forget the tag there.
    * 4 pixels per byte, 64 bytes per column. */
   unsigned b, p;
   int slot;

   if (!vbgo_tt_on)
      return;

   s_cpu[fb & 1] = 1;
   if (s_slot[fb & 1] == s_slot[(fb & 1) ^ 1])
      Unshare(fb & 1, 0);
   slot = s_slot[fb & 1];
   if (s_lazy[0] | s_lazy[1] | s_lazy[2])
   {
      Settle(slot);
      SettleFrom(slot);
   }

   if (bytes == 4 && !(offset & 3) && offset / 64 < VBGO_TT_WIDTH && (offset % 64) * 4 / 8 + 1 < VBGO_TT_BLOCKS)
   {
      /* (a word: 16 pixels of a column, 8 in each of two blocks - the bytes' work below, at once) */
      const unsigned column = offset / 64, row = (offset % 64) * 4, block = row / 8;
      uint32_t *const tags = &s_tags[slot][lr & 1][column * VBGO_TT_COLUMN + row];
      s_cpu_cells[slot][block] |= 1ull << (column / 8), s_cpu_cells[slot][block + 1] |= 1ull << (column / 8);
      if (s_tagged[slot][block] >> (column / 8) & 1)
         memset(tags, 0, 8 * sizeof(*tags));
      if (s_tagged[slot][block + 1] >> (column / 8) & 1)
         memset(tags + 8, 0, 8 * sizeof(*tags));
      for (b = block; b <= block + 1; b++)
         if (!(s_cpu_reset[slot] >> b & 1))
         {
            s_block_stamp[slot][b] = 0;
            memset(s_cell_version[slot][b], 0, sizeof(s_cell_version[0][0]));
            s_cell_black[slot][b] = 0;
            memset(s_cell_worlds[slot][b], 0xFF, sizeof(s_cell_worlds[0][0]));
            memset(s_motion[slot][b], 0, sizeof(s_motion[0][0]));
            s_motion_cells[slot][b] = 0;
            s_cpu_reset[slot] |= 1u << b;
         }
      return;
   }

   for (b = 0; b < bytes; b++)
   {
      const unsigned o = offset + b;
      const unsigned column = o / 64, row = (o % 64) * 4;
      if (column >= VBGO_TT_WIDTH)
         continue;
      if (row / 8 < VBGO_TT_BLOCKS)
         s_cpu_cells[slot][row / 8] |= 1ull << (column / 8);
      /* (a cell that may hold no tag has all of them 0 already) */
      if (row / 8 >= VBGO_TT_BLOCKS || (s_tagged[slot][row / 8] >> (column / 8) & 1))
         for (p = 0; p < 4; p++)
            s_tags[slot][lr & 1][column * VBGO_TT_COLUMN + row + p] = 0;
      /* (a block reset again with nothing written there since is already as this leaves it) */
      if (row / 8 < VBGO_TT_BLOCKS && !(s_cpu_reset[slot] >> (row / 8) & 1))
      {
         s_block_stamp[slot][row / 8] = 0;
         memset(s_cell_version[slot][row / 8], 0, sizeof(s_cell_version[0][0]));
         s_cell_black[slot][row / 8] = 0;
         memset(s_cell_worlds[slot][row / 8], 0xFF, sizeof(s_cell_worlds[0][0]));
         memset(s_motion[slot][row / 8], 0, sizeof(s_motion[0][0]));
         s_motion_cells[slot][row / 8] = 0;
         s_cpu_reset[slot] |= 1u << (row / 8);
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
   Settle(slot);
   view->clean_bands = 0;
   for (x = 0; x < VBGO_TT_BLOCKS; x++)
   {
      view->band_key[x] = s_band_pass[slot][x] == s_stamp[slot] ? VBGO_TT_BAND_REACHED : 0;
      view->clean_bands |= (uint32_t)(s_band_pass[slot][x] == s_stamp[slot] && s_band_bk0[slot][x]) << x;
   }

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
   view->tagged = s_tagged[slot];
   view->obj_cells = s_obj_cells[slot];
   view->filled_cells = s_filled_cells[slot];
   view->fb = fb;
   view->clut = s_disp_clut[eye & 1][0];
   view->motion = &s_motion[slot][0][0];
   view->motion_cells = s_motion_cells[slot];
   view->motion_noted = s_motion_noted[slot];
   view->cpu_cells = s_cpu_cells[slot];
   view->stamp = s_stamp[slot];

   for (x = 0; x < VBGO_TT_WIDTH; x++)
   {
      const uint8_t d = s_disp[eye & 1][x];
      const int shown = (d & 4) && (d & 1) == (unsigned)fb;
      view->columns[x] = shown ? &s_tags[slot][(d >> 1) & 1][x * VBGO_TT_COLUMN] : NULL;
      view->cells[x] = shown ? &s_cells[slot][(d >> 1) & 1][x] : NULL;
      if (!view->columns[x] || ((d >> 1) & 1) != (eye & 1) || s_disp_clut[eye & 1][x] != view->clut)
         view->clut = 0;
   }

   return true;
}
