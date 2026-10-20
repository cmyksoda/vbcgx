/* Mednafen - Multi-system Emulator
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <math.h>

#include <retro_inline.h>

#include "vb.h"
#include "vip.h"
#include "vbgo_tiletrack.h"

#include "../math_ops.h"
#include "../masmem.h"
#include "../state_helpers.h"

static uint8 FB[2][2][0x6000];
static uint16 CHR_RAM[0x8000 / sizeof(uint16)];
static uint16 DRAM[0x20000 / sizeof(uint16)];

/* Helper functions for the V810 VIP RAM read/write handlers.
 *  "Memory Array 16 (Write/Read) (16/8)" */
#define VIP__GETP16(array, address) ( (uint16 *)&((uint8 *)(array))[(address)] )

#ifdef MSB_FIRST
#define VIP__GETP8(array, address) ( &((uint8 *)(array))[(address) ^ 1] )
#else
#define VIP__GETP8(array, address) ( &((uint8 *)(array))[(address)] )
#endif

static INLINE void VIP_MA16W16(uint16 *array, const uint32 v810_address, const uint16 value)
{
   *(VIP__GETP16(array, v810_address)) = value;
}

static INLINE uint16 VIP_MA16R16(uint16 *array, const uint32 v810_address)
{
   return *(VIP__GETP16(array, v810_address));
}

static INLINE void VIP_MA16W8(uint16 *array, const uint32 v810_address, const uint8 value)
{
   *(VIP__GETP8(array, v810_address)) = value;
}

static INLINE uint8 VIP_MA16R8(uint16 *array, const uint32 v810_address)
{
   return *(VIP__GETP8(array, v810_address));
}


#define INT_SCAN_ERR	0x0001
#define INT_LFB_END	0x0002
#define INT_RFB_END	0x0004
#define INT_GAME_START	0x0008
#define INT_FRAME_START	0x0010

#define INT_SB_HIT	0x2000
#define INT_XP_END	0x4000
#define INT_TIME_ERR	0x8000

static uint16 InterruptPending;
static uint16 InterruptEnable;

static uint8 BRTA, BRTB, BRTC, REST;
static uint8 Repeat;

static void CopyFBColumnToTarget_Anaglyph(void) NO_INLINE;
static void CopyFBColumnToTarget_AnaglyphSlow(void) NO_INLINE;
static void CopyFBColumnToTarget_CScope(void) NO_INLINE;
static void CopyFBColumnToTarget_SideBySide(void) NO_INLINE;
static void CopyFBColumnToTarget_VLI(void) NO_INLINE;
static void CopyFBColumnToTarget_HLI(void) NO_INLINE;
static void (*CopyFBColumnToTarget)(void) = NULL;
static uint32 VB3DMode;
static uint32 VB3DReverse;
static uint32 VBPrescale;
static uint32 VBSBS_Separation;
static uint32 HLILUT[256];
static uint32 ColorLUT[2][256];
static int32 BrightnessCache[4];
static uint32 BrightCLUT[2][4];

static double ColorLUTNoGC[2][256][3];
static uint32 AnaSlowColorLUT[256][256];

/* A few settings: */
static bool InstantDisplayHack;
static bool AllowDrawSkip;

static bool VidSettingsDirty;
static bool ParallaxDisabled;
static uint32 Anaglyph_Colors[2];
static uint32 Default_Color;

static void MakeColorLUT(void)
{
   unsigned lr, i, l_b, r_b;

   for(lr = 0; lr < 2; lr++)
   {
      for(i = 0; i < 256; i++)
      {
         double prod    = (double)i / 255;
         double r       = prod; 
         double g       = prod; 
         double b       = prod;
         /* TODO: Use correct gamma curve, instead of approximation. */
         double r_prime = pow(r, 1.0 / 2.2);
         double g_prime = pow(g, 1.0 / 2.2);
         double b_prime = pow(b, 1.0 / 2.2);

         switch(VB3DMode)
         {
            case VB3DMODE_ANAGLYPH:
               r_prime = r_prime * ((Anaglyph_Colors[lr ^ VB3DReverse] >> 16) & 0xFF) / 255;
               g_prime = g_prime * ((Anaglyph_Colors[lr ^ VB3DReverse] >> 8) & 0xFF) / 255;
               b_prime = b_prime * ((Anaglyph_Colors[lr ^ VB3DReverse] >> 0) & 0xFF) / 255;
               break;
            default:
               r_prime = r_prime * ((Default_Color >> 16) & 0xFF) / 255;
               g_prime = g_prime * ((Default_Color >> 8) & 0xFF) / 255;
               b_prime = b_prime * ((Default_Color >> 0) & 0xFF) / 255;
               break;
         }
         ColorLUTNoGC[lr][i][0] = pow(r_prime, 2.2 / 1.0);
         ColorLUTNoGC[lr][i][1] = pow(g_prime, 2.2 / 1.0);
         ColorLUTNoGC[lr][i][2] = pow(b_prime, 2.2 / 1.0);

         ColorLUT[lr][i] = MAKECOLOR((int)(r_prime * 255), (int)(g_prime * 255), (int)(b_prime * 255), 0);
      }
   }

   /* Anaglyph slow-mode LUT calculation */
   for(l_b = 0; l_b < 256; l_b++)
   {
      for(r_b = 0; r_b < 256; r_b++)
      {
         double r_prime, g_prime, b_prime;
         double r = ColorLUTNoGC[0][l_b][0] + ColorLUTNoGC[1][r_b][0];
         double g = ColorLUTNoGC[0][l_b][1] + ColorLUTNoGC[1][r_b][1];
         double b = ColorLUTNoGC[0][l_b][2] + ColorLUTNoGC[1][r_b][2];

         if(r > 1.0)
            r = 1.0;
         if(g > 1.0)
            g = 1.0;
         if(b > 1.0)
            b = 1.0;

         r_prime = pow(r, 1.0 / 2.2);
         g_prime = pow(g, 1.0 / 2.2);
         b_prime = pow(b, 1.0 / 2.2);

         AnaSlowColorLUT[l_b][r_b] = MAKECOLOR(((int)(r_prime * 255)), ((int)(g_prime * 255)), ((int)(b_prime * 255)), 0);
      }
   }
   vbgo_tt_clut_serial++;
}

static void RecalcBrightnessCache(void)
{
   uint32 before[2][4];
   unsigned i, lr;
   memcpy(before, BrightCLUT, sizeof(before));
   int32 CumulativeTime = (BRTA + 1 + BRTB + 1 + BRTC + 1 + REST + 1) + 1;
   int32 MaxTime = 128;

   BrightnessCache[0] = 0;
   BrightnessCache[1] = 0;
   BrightnessCache[2] = 0;
   BrightnessCache[3] = 0;

   for(i = 0; i < Repeat + 1; i++)
   {
      int32 btemp[4];

      if((i * CumulativeTime) >= MaxTime)
         break;

      btemp[1] = (i * CumulativeTime) + BRTA;
      if(btemp[1] > MaxTime)
         btemp[1] = MaxTime;
      btemp[1] -= (i * CumulativeTime);
      if(btemp[1] < 0)
         btemp[1] = 0;


      btemp[2] = (i * CumulativeTime) + BRTA + 1 + BRTB;
      if(btemp[2] > MaxTime)
         btemp[2] = MaxTime;
      btemp[2] -= (i * CumulativeTime) + BRTA + 1;
      if(btemp[2] < 0)
         btemp[2] = 0;

      btemp[3] = (i * CumulativeTime) + BRTA + BRTB + BRTC + 1;
      if(btemp[3] > MaxTime)
         btemp[3] = MaxTime;
      btemp[3] -= (i * CumulativeTime) + 1;
      if(btemp[3] < 0)
         btemp[3] = 0;

      BrightnessCache[1] += btemp[1];
      BrightnessCache[2] += btemp[2];
      BrightnessCache[3] += btemp[3];
   }

   for(i = 0; i < 4; i++)
      BrightnessCache[i] = 255 * BrightnessCache[i] / MaxTime;

   for(lr = 0; lr < 2; lr++)
      for(i = 0; i < 4; i++)
         BrightCLUT[lr][i] = ColorLUT[lr][BrightnessCache[i]];
   if(memcmp(before, BrightCLUT, sizeof(before)))
      vbgo_tt_clut_serial++;
}

static void Recalc3DModeStuff(bool non_rgb_output)
{
   switch(VB3DMode)
   {
      default: 
         CopyFBColumnToTarget = CopyFBColumnToTarget_Anaglyph;
         if(((Anaglyph_Colors[0] & 0xFF) && (Anaglyph_Colors[1] & 0xFF)) ||
               ((Anaglyph_Colors[0] & 0xFF00) && (Anaglyph_Colors[1] & 0xFF00)) ||
               ((Anaglyph_Colors[0] & 0xFF0000) && (Anaglyph_Colors[1] & 0xFF0000)) ||
               non_rgb_output)
            CopyFBColumnToTarget = CopyFBColumnToTarget_AnaglyphSlow;
         break;

      case VB3DMODE_CSCOPE:
         CopyFBColumnToTarget = CopyFBColumnToTarget_CScope;
         break;

      case VB3DMODE_SIDEBYSIDE:
         CopyFBColumnToTarget = CopyFBColumnToTarget_SideBySide;
         break;

      case VB3DMODE_VLI:
         CopyFBColumnToTarget = CopyFBColumnToTarget_VLI;
         break;

      case VB3DMODE_HLI:
         CopyFBColumnToTarget = CopyFBColumnToTarget_HLI;
         break;
   }
   RecalcBrightnessCache();
}

void VIP_Set3DMode(uint32 mode, bool reverse, uint32 prescale, uint32 sbs_separation)
{
   uint32_t p;

   VB3DMode         = mode;
   VB3DReverse      = reverse ? 1 : 0;
   VBPrescale       = prescale;
   VBSBS_Separation = sbs_separation;

   VidSettingsDirty = true;

   for(p = 0; p < 256; p++)
   {
      unsigned i, ps, shifty;
      uint8 s[4];
      uint32 v   = 0;

      s[0] = (p >> 0) & 0x3;
      s[1] = (p >> 2) & 0x3;
      s[2] = (p >> 4) & 0x3;
      s[3] = (p >> 6) & 0x3;

      for(i = 0, shifty = 0; i < 4; i++)
      {
         for(ps = 0; ps < prescale; ps++)
         {
            v |= s[i] << shifty;
            shifty += 2;
         }
      }

      HLILUT[p] = v;
   }
}

static void ForgetPasses(void);

void VIP_SetParallaxDisable(bool disabled)
{
   if(ParallaxDisabled != disabled)
      ForgetPasses();
   ParallaxDisabled = disabled;
}

void VIP_SetDefaultColor(uint32 default_color)
{
   Default_Color = default_color;

   VidSettingsDirty = true;
}


void VIP_SetAnaglyphColors(uint32 lcolor, uint32 rcolor)
{
   Anaglyph_Colors[0] = lcolor;
   Anaglyph_Colors[1] = rcolor;

   VidSettingsDirty = true;
}

void VIP_SetInstantDisplayHack(bool val)
{
   InstantDisplayHack = val;
}

void VIP_SetAllowDrawSkip(bool val)
{
   AllowDrawSkip = val;
}


static uint16 FRMCYC;

static uint16 DPCTRL;
static bool DisplayActive;

#define XPCTRL_XP_RST	0x0001
#define XPCTRL_XP_EN	0x0002
static uint16 XPCTRL;
static uint16 SBCMP;	/* Derived from XPCTRL */

static uint16 SPT[4];	/* SPT0~SPT3, 5f848~5f84e */
static uint16 GPLT[4];
static uint8 GPLT_Cache[4][4];
/* A character row byte's four pixels, in screen order: their shades, and which of them are drawn. */
static uint8 GPLT_Quad[4][256][4];
static uint8 OpaqueQuad[256][4];

static INLINE void Recalc_GPLT_Cache(int which)
{
   unsigned i, k;
   for(i = 0; i < 4; i++)
      GPLT_Cache[which][i] = (GPLT[which] >> (i * 2)) & 3;
   for(i = 0; i < 256; i++)
      for(k = 0; k < 4; k++)
         GPLT_Quad[which][i][k] = GPLT_Cache[which][(i >> (k * 2)) & 3];
}

static uint16 JPLT[4];
static uint8 JPLT_Cache[4][4];

static INLINE void Recalc_JPLT_Cache(int which)
{
   unsigned i;
   for(i = 0; i < 4; i++)
      JPLT_Cache[which][i] = (JPLT[which] >> (i * 2)) & 3;
}


static uint16 BKCOL;

static int32 last_ts;

static int32 Column;
static int32 ColumnCounter;

static int32 DisplayRegion;
static bool DisplayFB;

static int32 GameFrameCounter;

static int32 DrawingCounter;
static bool DrawingActive;
static bool DrawingFB;
static uint32 DrawingBlock;
static int32 SB_Latch;
static int32 SBOUT_InactiveTime;

/* A pass whose inputs haven't changed since the other buffer's last pass
 * started draws exactly what that one drew, so its blocks are copied. */
static uint32 DrawSerial;    /* bumped by every change to what drawing reads */
static uint32 PassSerial[2]; /* DrawSerial as each buffer's last pass started */
static bool PassClean[2];    /* that pass drew with unchanged inputs, and the CPU hasn't written into the buffer since */
static bool PassTouched[2];  /* the CPU wrote into the buffer during its pass */
static bool PassRepeats;     /* the pass in progress copies the other buffer's */
static bool PassAlreadyCopied; /* ... whose blocks this buffer already holds */
static uint32 PassStartedAt; /* DrawSerial as the pass in progress started */
/* A block whose inputs haven't changed since this buffer last drew it still
 * holds that drawing, so it's kept instead of drawn again. Drawing notes what
 * a block reads (vip_draw.inc); writes note when, in DrawSerial terms. */
static uint32 GlobalChangedAt;    /* world attributes, palettes, background color */
static uint32 SptChangedAt;       /* sprite groups (some games move them every frame) */
static uint64 ObjSerial = 1;      /* bumped by every change to a sprite attribute or group: sorted sprite lists stay good until it moves */
static uint32 RowChangedAt[1024]; /* DRAM, per 64 halfwords */
static uint32 ChrChangedAt[2048];
static uint32 ObjChangedAt[32];   /* sprite attributes, per 8 rows the sprite was or is on */
static uint32 ObjEntryChangedAt[1024];
static uint32 WorldChangedAt[28]; /* a background world's attributes, per block its window covers (or did) */
static uint32 WorldCellChangedAt[28][48]; /* (the same, per cell, for windows narrower than the screen) */
static uint64 WorldDirty; /* (BlockChange: the cells only a world's change covers) */
static uint32 WorldBlockChangedAt[32][28]; /* (per world: the blocks its change covers) */
static uint32 ChangedWorlds; /* (BlockChange: the worlds changed there since it was drawn) */
static bool SpritesChanged; /* (BlockChange: and its sprites, or their characters) */
#define MAX_BLOCK_SPRITES 96
typedef struct
{
   uint16 oam, chr;
   uint8 world;    /* (and its eyes, in bits 5 and 6) */
   int16 x[2]; /* each eye's */
   uint16 entry[4]; /* its attributes as drawn */
} SpriteDrawn;

typedef struct
{
   bool valid;     /* the buffer holds this drawing (the CPU hasn't written over it) */
   uint32 drawnAt; /* DrawSerial when drawn, or last found unchanged */
   uint32 passAt;  /* ... and as that pass started (the tile tracker's blank tiles are the pass's) */
   uint32 chr[2048 / 32], rows[1024 / 32];
   uint32 bgChr[2048 / 32]; /* (the characters background drawing used) */
   uint16 worlds[32 * 16]; /* (the world attributes as it was drawn) */
   uint64 objs;    /* its sprite lists, hashed (see ObjListsHash) */
   int sprites;    /* the sprites its lists hold (more than MAX_BLOCK_SPRITES: -1) */
   SpriteDrawn sprite[MAX_BLOCK_SPRITES];
} BlockInputs;
static BlockInputs Blocks[2][28];
/* Each buffer's side-by-side picture has a half of the surface of its own,
 * where a column's block is converted again only if the buffer's pixels there
 * (per 8x8 cell) or the brightness table changed since. */
static uint32 CellContent[2][28][48]; /* a version per cell's pixels, 0: unknown */
/* Per frame buffer and cell column: a bit per (output eye, column of the cell) that has found every block's CellContent converted
 * since the last edit to any of them (bit 8 * eye + column - cell * 8); every edit clears the bits, even one that left it as it was. */
static uint16 CellClean[2][48];
static void EditBlock(int fb) { memset(CellClean[fb], 0, sizeof(CellClean[fb])); }
static uint32 Contents;
static uint32 ShownContent[2][2][384][28]; /* [fb][output eye][column][block]: CellContent as converted */
static uint32 ShownClut[2][2][384];        /* ... vbgo_tt_clut_serial as converted */
static uint32 ShownVersion[2][2][384];     /* ... FBVersion as converted, and from which eye's buffer (0: not all of it) */
static uint8 ShownLr[2][2][384];
static int ShownFB;
/* VirtualBoyGo: the colorizer reads the picture, never shows it, so each eye is kept compact - per
 * frame buffer and column, its 2-bit pixels and the 4 values they stand for (vbgo_tiletrack.h). */
static uint8 EyeBits[2][2][384][56];
static uint32 EyeClut[2][2][384][4];
const uint8 *vbgo_vip_eye_bits(int eye) { return &EyeBits[ShownFB][eye][0][0]; }
const uint32 *vbgo_vip_eye_clut(int eye) { return &EyeClut[ShownFB][eye][0][0]; }

int vbgo_vip_shown_fb(void) { return ShownFB; }

static uint32 NewContent(void)
{
   if(++Contents == 0)
      Contents = 1;
   return Contents;
}
static uint32 UnusedInputs[2048 / 32];
static uint32 *UsedChr = UnusedInputs, *UsedRows = UnusedInputs, *UsedBgChr = UnusedInputs; /* the block being drawn's */
static BlockInputs *Drawing; /* (its sprites go here) */
static int ClipX0 = 0, ClipX1 = 383; /* the columns VIP_DrawBlock draws (a block's changed cells) */
static uint64 UnusedObjs, *UsedObjs = &UnusedObjs;
#define OBJS_START 1469598103934665603ull

static uint64 HashObjList(uint64 h, int world, const uint16 *list, int count)
{
   int i;
   h = (h ^ (uint64)(world | count << 7)) * 1099511628211ull;
   for(i = 0; i < count; i++)
      h = (h ^ list[i]) * 1099511628211ull;
   return h;
}

static uint64 ObjListsHash(uint32 block_no);
static uint64 GatherBlockSprites(uint32 block_no, BlockInputs *into);

static void NoteSprites(int world, const uint16 *list, int count)
{
   int k;
   if(!Drawing)
      return;
   for(k = 0; k < count && Drawing->sprites >= 0; k++)
   {
      const uint16 *oam = &DRAM[(0x1E000 + list[k] * 8) >> 1];
      const uint32 jp = ParallaxDisabled ? 0 : (oam[1] & 0x3FFF);
      SpriteDrawn *drawn = &Drawing->sprite[Drawing->sprites];
      if(Drawing->sprites == MAX_BLOCK_SPRITES)
      {
         Drawing->sprites = -1;
         break;
      }
      drawn->oam = list[k];
      drawn->chr = oam[3] & 0x7FF;
      drawn->world = world;
      drawn->x[0] = sign_x_to_s32(10, oam[0] - jp);
      drawn->x[1] = sign_x_to_s32(10, oam[0] + jp);
      memcpy(drawn->entry, oam, sizeof(drawn->entry));
      Drawing->sprites++;
   }
}
/* What each frame shows, to tell a frame that shows the same picture as the
 * last: each buffer's contents get a fresh version when drawn or written, the
 * display's brightness and settings a serial, and a frame that changes either
 * while it's displayed shows a picture of its own. */
static uint32 Versions;          /* (the last version handed out) */
static uint32 FBVersion[2];
static uint32 DisplaySerial;
static bool FrameChanged;
static uint32 ShownKey[2];       /* FBVersion of the buffer shown and DisplaySerial, as this frame started */
static uint32 FramePicture, LastFramePicture;

uint32 vbgo_vip_last_frame_picture(void) { return LastFramePicture; }
/* Until when VIP register reads can't change but at an event (XPSTTS's SBOUT bit times out on its own). */
int32 vbgo_vip_steady_until(int32 timestamp) { return timestamp < SBOUT_InactiveTime ? SBOUT_InactiveTime : 0x7FFFFFFF; }

/* (the 2^32nd version on can't pass for one a column was converted at: those are forgotten) */
static uint32 NextVersion(void)
{
   if(++Versions == 0)
   {
      memset(ShownVersion, 0, sizeof(ShownVersion));
      ++Versions;
   }
   return Versions;
}

static void NewContents(int fb)
{
   FBVersion[fb] = NextVersion();
   if(fb == DisplayFB)
      FrameChanged = true;
}

static void DisplayChanged(void)
{
   DisplaySerial++;
   FrameChanged = true;
}

/* At a frame's start (after the buffers swapped): the picture of the frame
 * that just ended, and the one this frame starts showing. With the instant
 * display hack the frame's output is drawn right here, from the buffers as
 * they are now; else it was drawn column by column while it ran. */
static void StartFramePicture(void)
{
   const bool sameKey = ShownKey[0] == FBVersion[DisplayFB] && ShownKey[1] == DisplaySerial;
   if(InstantDisplayHack)
      LastFramePicture = sameKey ? LastFramePicture : NextVersion();
   else
      LastFramePicture = FrameChanged ? NextVersion() : FramePicture;
   if(FrameChanged || !sameKey)
      FramePicture = NextVersion();
   ShownKey[0] = FBVersion[DisplayFB];
   ShownKey[1] = DisplaySerial;
   FrameChanged = false;
}

static void ForgetBlocks(void);

static void ForgetPasses(void)
{
   DrawSerial++;
   ObjSerial++;
   ForgetBlocks();
   NewContents(0);
   NewContents(1);
   DisplayChanged();
   PassClean[0] = PassClean[1] = false;
   PassRepeats = false;
}

static void ForgetBlocks(void)
{
   int fb, b;
   for(fb = 0; fb < 2; fb++)
      for(b = 0; b < 28; b++)
      {
         Blocks[fb][b].valid = false;
         memset(CellContent[fb][b], 0, sizeof(CellContent[fb][b]));
         EditBlock(fb);
      }
}

/* The serial runs for hours, but not forever. */
static void RestartSerials(void)
{
   memset(RowChangedAt, 0, sizeof(RowChangedAt));
   memset(ChrChangedAt, 0, sizeof(ChrChangedAt));
   memset(ObjChangedAt, 0, sizeof(ObjChangedAt));
   memset(ObjEntryChangedAt, 0, sizeof(ObjEntryChangedAt));
   memset(WorldChangedAt, 0, sizeof(WorldChangedAt));
   memset(WorldCellChangedAt, 0, sizeof(WorldCellChangedAt));
   memset(WorldBlockChangedAt, 0, sizeof(WorldBlockChangedAt));
   GlobalChangedAt = SptChangedAt = 0;
   DrawSerial = 0;
   ForgetPasses();
}

/* The blocks a background world's window covers with these attributes: the
 * rows DrawBG and DrawAffine draw. */
static void WorldRowsChanged(const uint16 *attr, uint32 at, unsigned world)
{
   /* (DrawBG and DrawAffine draw from DestX to DestX + width: either eye, or parallax off) */
   const uint16 gy = sign_11_to_s16(attr[3]), height = attr[8] & 0x3FF;
   const uint16 gx = sign_11_to_s16(attr[1]), gp = sign_9_to_s16(attr[2]), width = sign_11_to_s16(attr[7]);
   int x0 = 384, x1 = -1, k, y;
   for(k = 0; k < 3; k++)
   {
      const int32 dest = (int16)sign_10_to_s16((uint16)(gx + (k == 0 ? -gp : k == 1 ? gp : 0)));
      const int32 start = dest < 0 ? 0 : dest, final = dest + width > 383 ? 383 : dest + width;
      if(start <= final)
      {
         x0 = start < x0 ? start : x0;
         x1 = final > x1 ? final : x1;
      }
   }
   if(x0 > x1)
      return;
   for(y = 0; y < 224; y++)
      if((uint16)(y - gy) <= height)
      {
         WorldBlockChangedAt[world][y >> 3] = at;
         if(x0 == 0 && x1 == 383)
            WorldChangedAt[y >> 3] = at;
         else
         {
            int c;
            for(c = x0 >> 3; c <= x1 >> 3; c++)
               WorldCellChangedAt[y >> 3][c] = at;
         }
         y |= 7;
      }
}

static void ObjRowsChanged(uint32 jy, uint32 at)
{
   ObjChangedAt[(jy & 0xFF) >> 3] = at;
   ObjChangedAt[((jy + 7) & 0xFF) >> 3] = at;
}

/* DRAM's halfword at byte address a is about to become v. */
static void DramChanging(uint32 a, uint16 v)
{
   const uint32 h = a >> 1;
   const uint32 at = ++DrawSerial;
   RowChangedAt[h >> 6] = at;
   if(h >= (0x1D800 >> 1) && h < (0x1DC00 >> 1))
   {
      /* (a world's: its window's blocks, old and new - unless it changes which
       * worlds draw) */
      uint16 *attr = &DRAM[h & ~15u];
      const uint16 control = attr[0];
      const bool objWorld = ((control >> 12) & 3) == 3;
      const bool toObj = (h & 15) == 0 && ((v >> 12) & 3) == 3;
      const unsigned world = (h - (0x1D800 >> 1)) >> 4;
      if((h & 15) == 0 && ((control ^ v) & 0x40))
         GlobalChangedAt = at;
      else if(objWorld || toObj)
      {
         /* (sprites in or out, or their eyes: every block's sprite lists, and its window as a background) */
         if((h & 15) == 0 && ((control ^ v) & 0xF000))
         {
            SptChangedAt = at;
            if(!objWorld || !toObj)
               WorldRowsChanged(attr, at, world);
         }
      }
      else
      {
         const uint16 was = attr[h & 15];
         WorldRowsChanged(attr, at, world);
         attr[h & 15] = v;
         WorldRowsChanged(attr, at, world);
         attr[h & 15] = was;
      }
   }
   else if(h >= (0x1E000 >> 1))
   {
      ObjSerial++;
      ObjEntryChangedAt[(h - (0x1E000 >> 1)) >> 2] = at;
      /* (a sprite's: the rows it's on, and will be on) */
      ObjRowsChanged(DRAM[(h & ~3u) + 2], at);
      if((h & 3) == 2)
         ObjRowsChanged(v, at);
   }
}

/* What changed since the block was drawn: 0 nothing, 1 only its sprites
 * (some moved, came or went, or show another tile), 2 more. */
/* 0: nothing it reads changed; 1: only some cells (sprites, narrow worlds);
 * 3: worlds as wide as the screen, maybe sprites too (drawn again, and worked
 * out after: see vbgo_tiletrack_world_cells); 2: anything else. */
static int BlockChange(const BlockInputs *block)
{
   int i, change = 0, wholeWorld;
   bool worldsSame = false;
   WorldDirty = 0;
   if(!block->valid || GlobalChangedAt > block->drawnAt)
      return 2;
   wholeWorld = WorldChangedAt[DrawingBlock] > block->drawnAt;
   /* (world attributes written since but back as they were - a game switching worlds off and on between passes: nothing
    * to redraw, whatever the writes marked; only the worlds whose writes marked this block are compared) */
   {
      bool marked = wholeWorld;
      for(i = 0; i < 48 && !marked; i++)
         marked = WorldCellChangedAt[DrawingBlock][i] > block->drawnAt;
      if(marked)
      {
         worldsSame = true;
         for(i = 0; i < 32 && worldsSame; i++)
            if(WorldBlockChangedAt[i][DrawingBlock] > block->drawnAt)
               worldsSame = !memcmp(&block->worlds[i * 16], &DRAM[(0x1D800 >> 1) + i * 16], 16 * sizeof(uint16));
         if(worldsSame)
            wholeWorld = 0;
      }
   }
   for(i = 0; i < 1024 / 32; i++)
   {
      uint32 m;
      for(m = block->rows[i]; m; m &= m - 1)
         if(RowChangedAt[i * 32 + __builtin_ctz(m)] > block->drawnAt)
            return 2;
   }
   for(i = 0; i < 2048 / 32; i++)
   {
      uint32 m;
      for(m = block->chr[i]; m; m &= m - 1)
      {
         const int c = i * 32 + __builtin_ctz(m);
         if(ChrChangedAt[c] > block->passAt)
         {
            if(block->bgChr[i] >> (c & 31) & 1)
               return 2;
            change = 1;
         }
      }
   }
   if(ObjChangedAt[DrawingBlock] > block->drawnAt)
      change = 1;
   if(!change && SptChangedAt > block->drawnAt && ObjListsHash(DrawingBlock) != block->objs)
      change = 1;
   for(i = 0; i < 48 && !worldsSame; i++)
      WorldDirty |= (uint64)(WorldCellChangedAt[DrawingBlock][i] > block->drawnAt) << i;
   ChangedWorlds = 0;
   if(wholeWorld || WorldDirty)
      for(i = 0; i < 32; i++)
         ChangedWorlds |= (uint32)(WorldBlockChangedAt[i][DrawingBlock] > block->drawnAt) << i;
   SpritesChanged = change != 0;
   if(wholeWorld)
      return 3;
   return change || WorldDirty ? 1 : 0;
}

static uint64 SpriteCells(const SpriteDrawn *drawn)
{
   uint64 cells = 0;
   int eye;
   for(eye = 0; eye < 2; eye++)
   {
      const int x = drawn->x[eye];
      if(x > -8 && x < 384)
      {
         const int first = (x < 0 ? 0 : x) >> 3, last = (x + 7 > 383 ? 383 : x + 7) >> 3;
         cells |= ((2ull << last) - 1) & ~((1ull << first) - 1);
      }
   }
   return cells;
}

/* A block drawn again because only its sprites changed: the cells the
 * changed, new and gone ones cover (else as before). */
static uint64 SpriteChangedCells(const BlockInputs *was, int wasCount, uint32 drawnAt, uint32 passAt, const BlockInputs *is)
{
   uint64 cells = 0;
   uint8 isAt[256], wasAt[256]; /* (open addressing by OAM entry and world: a list's index + 1, 0 free) */
   int i, k;
   if(wasCount < 0 || is->sprites < 0)
      return ~0ull;
   memset(isAt, 0, sizeof(isAt));
   memset(wasAt, 0, sizeof(wasAt));
   for(k = 0; k < is->sprites; k++)
   {
      unsigned h = (is->sprite[k].oam * 5 + is->sprite[k].world * 41) & 255;
      while(isAt[h] && !(is->sprite[isAt[h] - 1].oam == is->sprite[k].oam && is->sprite[isAt[h] - 1].world == is->sprite[k].world))
         h = (h + 1) & 255;
      if(!isAt[h])
         isAt[h] = k + 1;
   }
   for(i = 0; i < wasCount; i++)
   {
      unsigned h = (was->sprite[i].oam * 5 + was->sprite[i].world * 41) & 255;
      while(wasAt[h] && !(was->sprite[wasAt[h] - 1].oam == was->sprite[i].oam && was->sprite[wasAt[h] - 1].world == was->sprite[i].world))
         h = (h + 1) & 255;
      if(!wasAt[h])
         wasAt[h] = i + 1;
   }
   for(i = 0; i < wasCount; i++)
   {
      const SpriteDrawn *a = &was->sprite[i];
      const SpriteDrawn *b = NULL;
      unsigned h = (a->oam * 5 + a->world * 41) & 255;
      while(isAt[h] && !b)
      {
         if(is->sprite[isAt[h] - 1].oam == a->oam && is->sprite[isAt[h] - 1].world == a->world)
            b = &is->sprite[isAt[h] - 1];
         h = (h + 1) & 255;
      }
      /* (an entry written and written back as it was drawn is no change) */
      if(!b || (ObjEntryChangedAt[a->oam] > drawnAt && memcmp(a->entry, b->entry, sizeof(a->entry))) || ChrChangedAt[a->chr] > passAt)
         cells |= SpriteCells(a) | (b ? SpriteCells(b) : 0);
   }
   for(k = 0; k < is->sprites; k++)
   {
      const SpriteDrawn *b = &is->sprite[k];
      int found = 0;
      unsigned h = (b->oam * 5 + b->world * 41) & 255;
      while(wasAt[h] && !found)
      {
         found = was->sprite[wasAt[h] - 1].oam == b->oam && was->sprite[wasAt[h] - 1].world == b->world;
         h = (h + 1) & 255;
      }
      if(!found)
         cells |= SpriteCells(b);
   }
   return cells;
}

/* Whether nothing the block just drawn read had changed since its pass
 * started (what the tile tracker's per-pass copies show matches it). */
static bool BlockSettled(const BlockInputs *block)
{
   int i;
   if(GlobalChangedAt > PassStartedAt || WorldChangedAt[DrawingBlock] > PassStartedAt || ObjChangedAt[DrawingBlock] > PassStartedAt)
      return false;
   for(i = 0; i < 48; i++)
      if(WorldCellChangedAt[DrawingBlock][i] > PassStartedAt)
         return false;
   for(i = 0; i < 2048 / 32; i++)
   {
      uint32 m;
      for(m = block->chr[i]; m; m &= m - 1)
         if(ChrChangedAt[i * 32 + __builtin_ctz(m)] > PassStartedAt)
            return false;
   }
   return true;
}

static void BeginPass(void)
{
   const int src = !DrawingFB;
   if(DrawSerial >= 0xF0000000u)
      RestartSerials();
   PassStartedAt = DrawSerial;
   PassRepeats = PassClean[src] && PassSerial[src] == DrawSerial && vbgo_tiletrack_pass_reusable(src);
   PassAlreadyCopied = PassRepeats && PassClean[DrawingFB] && PassSerial[DrawingFB] == PassSerial[src];
   PassSerial[DrawingFB] = DrawSerial;
   PassClean[DrawingFB] = false;
   PassTouched[DrawingFB] = false;
   if(PassRepeats)
   {
      vbgo_tiletrack_share_pass(DrawingFB, src);
      FBVersion[DrawingFB] = FBVersion[src];
   }
   else
      NewContents(DrawingFB);
}

static void CopyRepeatedBlock(void)
{
   int lr, x;
   if(PassAlreadyCopied && !PassTouched[DrawingFB])
      return;
   for(lr = 0; lr < 2; lr++)
   {
      const uint8 *from = FB[!DrawingFB][lr] + DrawingBlock * 2;
      uint8 *to = FB[DrawingFB][lr] + DrawingBlock * 2;
      for(x = 0; x < 384; x++)
      {
         to[64 * x + 0] = from[64 * x + 0];
         to[64 * x + 1] = from[64 * x + 1];
      }
   }
}

static void CheckIRQ(void)
{
   VBIRQ_Assert(VBIRQ_SOURCE_VIP, (bool)(InterruptEnable & InterruptPending));
}


bool VIP_Init(void)
{
   InstantDisplayHack = false;
   AllowDrawSkip = false;
   ParallaxDisabled = false;
   Anaglyph_Colors[0] = 0xFF0000;
   Anaglyph_Colors[1] = 0x0000FF;
   VB3DMode = VB3DMODE_ANAGLYPH;
   Default_Color = 0xFFFFFF;
   VB3DReverse = 0;
   VBPrescale = 1;
   VBSBS_Separation = 0;

   VidSettingsDirty = true;

   return(true);
}

void VIP_Power(void)
{
   unsigned i, k;

   for(i = 0; i < 256; i++)
      for(k = 0; k < 4; k++)
         OpaqueQuad[i][k] = ((i >> (k * 2)) & 3) ? 0xFF : 0;

   Repeat = 0;
   SB_Latch = 0;
   SBOUT_InactiveTime = -1;
   last_ts = 0;

   Column = 0;
   ColumnCounter = 259;

   DisplayRegion = 0;
   DisplayFB = 0;

   GameFrameCounter = 0;

   DrawingCounter = 0;
   DrawingActive = false;
   DrawingFB = 0;
   DrawingBlock = 0;

   DPCTRL = 2;
   DisplayActive = false;



   memset(FB, 0, 0x6000 * 2 * 2);
   memset(CHR_RAM, 0, 0x8000);
   memset(DRAM, 0, 0x20000);

   InterruptPending = 0;
   InterruptEnable = 0;

   BRTA = 0;
   BRTB = 0;
   BRTC = 0;
   REST = 0;

   FRMCYC = 0;

   XPCTRL = 0;
   SBCMP = 0;

   for(i = 0; i < 4; i++)
   {
      SPT[i] = 0;
      GPLT[i] = 0;
      JPLT[i] = 0;

      Recalc_GPLT_Cache(i);
      Recalc_JPLT_Cache(i);
   }

   BKCOL = 0;
   ForgetPasses();
}

static INLINE uint16 ReadRegister(int32 timestamp, uint32 A)
{
   uint16_t ret = 0;

   switch(A & 0xFE)
   {
      case 0x00:
         ret = InterruptPending;
         break;

      case 0x02:
         ret = InterruptEnable;
         break;

      case 0x20:
         ret = DPCTRL & 0x702;
         if((DisplayRegion & 1) && DisplayActive)
         {
            unsigned int DPBSY = 1 << ((DisplayRegion >> 1) & 1);

            if(DisplayFB)
               DPBSY <<= 2;

            ret |= DPBSY << 2;
         }
#if 0
         if(!(DisplayRegion & 1))	/* FIXME? (Had to do it this way for Galactic Pinball...) */
#endif
         ret |= 1 << 6;
         break;

         /* Note: Upper bits of BRTA, BRTB, BRTC, and REST(?) are 0 when read(on real hardware) */
      case 0x24:
         ret = BRTA;
         break;

      case 0x26:
         ret = BRTB;
         break;

      case 0x28:
         ret = BRTC;
         break;

      case 0x2A:
         ret = REST;
         break;

      case 0x30:
         ret = 0xFFFF;
         break;

      case 0x40:
         ret = XPCTRL & 0x2;
         if(DrawingActive)
         {
            ret |= (1 + DrawingFB) << 2;
         }
         if(timestamp < SBOUT_InactiveTime)
         {
            ret |= 0x8000;
            ret |= /*DrawingBlock*/SB_Latch << 8;
         }
         break;     /* XPSTTS, read-only */

      case 0x44:
         ret = 2;	/* VIP version.  2 is a known valid version, while the validity of other numbers is unknown, so we'll just go with 2. */
         break;

      case 0x48:
      case 0x4a:
      case 0x4c:
      case 0x4e:
         ret = SPT[(A >> 1) & 3];
         break;

      case 0x60:
      case 0x62:
      case 0x64:
      case 0x66:
         ret = GPLT[(A >> 1) & 3];
         break;

      case 0x68:
      case 0x6a:
      case 0x6c:
      case 0x6e:
         ret = JPLT[(A >> 1) & 3];
         break;

      case 0x70:
         ret = BKCOL;
         break;
   }

   return(ret);
}

static INLINE void WriteRegister(int32 timestamp, uint32 A, uint16 V)
{
   switch(A & 0xFE)
   {
      case 0x00:
         break; /* Interrupt pending, read-only */
      case 0x02:
         InterruptEnable = V & 0xE01F;
         CheckIRQ();
         break;
      case 0x04:
         InterruptPending &= ~V;
         CheckIRQ();
         break;

      case 0x20:
         break; /* Display control, read-only. */

      case 0x22:
         if(DPCTRL != (V & 0x703) || (V & 1))
            DisplayChanged();
         DPCTRL = V & (0x703); /* Display-control, write-only */
         if(V & 1)
         {
            DisplayActive = false;
            InterruptPending &= ~(INT_TIME_ERR | INT_FRAME_START | INT_GAME_START | INT_RFB_END | INT_LFB_END | INT_SCAN_ERR);
            CheckIRQ();
         }
         break;

      case 0x24:
         if(BRTA != (V & 0xFF))
            DisplayChanged();
         BRTA = V & 0xFF;	/* BRTA */
         RecalcBrightnessCache();
         break;

      case 0x26:
         if(BRTB != (V & 0xFF))
            DisplayChanged();
         BRTB = V & 0xFF;	/* BRTB */
         RecalcBrightnessCache();
         break;

      case 0x28:
         if(BRTC != (V & 0xFF))
            DisplayChanged();
         BRTC = V & 0xFF;	/* BRTC */
         RecalcBrightnessCache();
         break;

      case 0x2A:
         if(REST != (V & 0xFF))
            DisplayChanged();
         REST = V & 0xFF;	/* REST */
         RecalcBrightnessCache();
         break;

      case 0x2E:
         FRMCYC = V & 0xF;	/* FRMCYC, write-only? */
         break;

      case 0x30:
         break;	/* CTA, read-only( */

      case 0x40:
         break;	/* XPSTTS, read-only */

      case 0x42:
         XPCTRL = V & 0x0002;	/* XPCTRL, write-only */
         SBCMP = (V >> 8) & 0x1F;

         if(V & 1)
         {
            if(PassRepeats)
               vbgo_tiletrack_resume_pass(DrawingFB, DrawingBlock);
            PassRepeats = false;
            NewContents(DrawingFB);
            FrameChanged = true;
            DrawingFB         = DisplayFB;
            DisplayFB        ^= 1;
            DrawingActive     = 0;
            DrawingCounter    = 0;
            InterruptPending &= ~(INT_SB_HIT | INT_XP_END | INT_TIME_ERR);
            CheckIRQ();
         }
         break;

      case 0x44:
         break;	/* Version Control, read-only? */

      case 0x48:
      case 0x4a:
      case 0x4c:
      case 0x4e:
         if(SPT[(A >> 1) & 3] != (V & 0x3FF))
            SptChangedAt = ++DrawSerial, ObjSerial++;
         SPT[(A >> 1) & 3] = V & 0x3FF;
         break;

      case 0x60:
      case 0x62: 
      case 0x64:
      case 0x66:
         if(GPLT[(A >> 1) & 3] != (V & 0xFC))
            GlobalChangedAt = ++DrawSerial;
         GPLT[(A >> 1) & 3] = V & 0xFC;
         Recalc_GPLT_Cache((A >> 1) & 3);
         break;

      case 0x68:
      case 0x6a:
      case 0x6c:
      case 0x6e:
         if(JPLT[(A >> 1) & 3] != (V & 0xFC))
            GlobalChangedAt = ++DrawSerial;
         JPLT[(A >> 1) & 3] = V & 0xFC;
         Recalc_JPLT_Cache((A >> 1) & 3);
         break;

      case 0x70:
         if(BKCOL != (V & 0x3))
            GlobalChangedAt = ++DrawSerial;
         BKCOL = V & 0x3;
         break;
   }
}

/* Don't update the VIP state on reads/writes, 
 * the event system will update it with enough precision 
 * as far as VB software cares.
 */

uint8 VIP_Read8(int32 timestamp, uint32 A)
{
   switch(A >> 16)
   {
      case 0x0:
      case 0x1:
         if((A & 0x7FFF) >= 0x6000)
            return VIP_MA16R8(CHR_RAM, (A & 0x1FFF) | ((A >> 2) & 0x6000));
         return FB[(A >> 15) & 1][(A >> 16) & 1][A & 0x7FFF];
      case 0x2:
      case 0x3:
         return VIP_MA16R8(DRAM, A & 0x1FFFF);
      case 0x4:
      case 0x5:
         if(A >= 0x5E000)
            return ReadRegister(timestamp, A);
         break;
      case 0x6:
         break;

      case 0x7:
         if(A >= 0x8000)
            return VIP_MA16R8(CHR_RAM, A & 0x7FFF);
         break;
      default:
         break;
   }

   return 0;
}

const uint8 *VIP_FBBase(void)
{
   return &FB[0][0][0];
}

/* The CPU's word read from a frame buffer, as its two halfword reads (false: A isn't one). */
bool VIP_ReadFBWord(uint32 A, uint32 *V)
{
   if((A >> 16) > 1 || (A & 0x7FFF) >= 0x6000 || (A & 3))
      return false;
   const uint8 *fb = &FB[(A >> 15) & 1][(A >> 16) & 1][A & 0x7FFF];
   *V = LoadU16_LE((const uint16 *)fb) | ((uint32)LoadU16_LE((const uint16 *)(fb + 2)) << 16);
   return true;
}

uint16 VIP_Read16(int32 timestamp, uint32 A)
{
   switch(A >> 16)
   {
      case 0x0:
      case 0x1:
         if((A & 0x7FFF) >= 0x6000)
            return VIP_MA16R16(CHR_RAM, (A & 0x1FFF) | ((A >> 2) & 0x6000));
         return LoadU16_LE((uint16 *)&FB[(A >> 15) & 1][(A >> 16) & 1][A & 0x7FFF]);
      case 0x2:
      case 0x3:
         return VIP_MA16R16(DRAM, A & 0x1FFFF);
      case 0x4:
      case 0x5: 
         if(A >= 0x5E000)
            return ReadRegister(timestamp, A);
         break;
      case 0x6:
         break;
      case 0x7:
         if(A >= 0x8000)
            return VIP_MA16R16(CHR_RAM, A & 0x7FFF);
         break;
      default:
         break;
   }

   return 0;
}

void VIP_Write8(int32 timestamp, uint32 A, uint8 V)
{
   switch(A >> 16)
   {
      case 0x0:
      case 0x1:
         if((A & 0x7FFF) >= 0x6000)
         {
            const uint32 a = (A & 0x1FFF) | ((A >> 2) & 0x6000);
            if(VIP_MA16R8(CHR_RAM, a) != V)
               ChrChangedAt[a >> 4] = ++DrawSerial;
            VIP_MA16W8(CHR_RAM, a, V);
         }
         else
         {
            PassClean[(A >> 15) & 1] = false;
            PassTouched[(A >> 15) & 1] = true;
            NewContents((A >> 15) & 1);
            if((A & 63) < 56)
               Blocks[(A >> 15) & 1][(A & 63) >> 1].valid = false, CellContent[(A >> 15) & 1][(A & 63) >> 1][((A & 0x7FFF) >> 9) % 48] = 0,
                  CellClean[(A >> 15) & 1][((A & 0x7FFF) >> 9) % 48] = 0;
            FB[(A >> 15) & 1][(A >> 16) & 1][A & 0x7FFF] = V;
         }
         break;

      case 0x2:
      case 0x3:
         if(VIP_MA16R8(DRAM, A & 0x1FFFF) != V)
         {
            const uint16 old = VIP_MA16R16(DRAM, A & 0x1FFFE);
            DramChanging(A & 0x1FFFF, (A & 1) ? ((old & 0x00FF) | (V << 8)) : ((old & 0xFF00) | V));
            if((A & 0x1FFFF) >= 0x1DC00 && (A & 0x1FFFF) < 0x1E000)
               DisplayChanged();
         }
         VIP_MA16W8(DRAM, A & 0x1FFFF, V);
         break;

      case 0x4:
      case 0x5:
         if(A >= 0x5E000)
            WriteRegister(timestamp, A, V);
         break;

      case 0x6:
         break;

      case 0x7:
         if(A >= 0x8000)
         {
            if(VIP_MA16R8(CHR_RAM, A & 0x7FFF) != V)
               ChrChangedAt[(A & 0x7FFF) >> 4] = ++DrawSerial;
            VIP_MA16W8(CHR_RAM, A & 0x7FFF, V);
         }
         break;
   }
}

/* The CPU's word write into a frame buffer, as its two halfword writes (false: A isn't one). */
bool VIP_WriteFBWord(uint32 A, uint32 V)
{
   if((A >> 16) > 1 || (A & 0x7FFF) >= 0x6000 || (A & 3))
      return false;
   const int fb = (A >> 15) & 1, lr = (A >> 16) & 1;
   PassClean[fb] = false;
   PassTouched[fb] = true;
   NewContents(fb);
   for(int half = 0; half < 2; half++)
   {
      const uint32 a = A + 2 * half;
      if((a & 63) < 56)
         Blocks[fb][(a & 63) >> 1].valid = false, CellContent[fb][(a & 63) >> 1][((a & 0x7FFF) >> 9) % 48] = 0,
         CellClean[fb][((a & 0x7FFF) >> 9) % 48] = 0;
      StoreU16_LE((uint16 *)&FB[fb][lr][a & 0x7FFF], half ? V >> 16 : V & 0xFFFF);
   }
   vbgo_tiletrack_cpu_fb_write(fb, lr, A & 0x7FFF, 4); /* VirtualBoyGo */
   return true;
}

void VIP_Write16(int32 timestamp, uint32 A, uint16 V)
{
   switch(A >> 16)
   {
      case 0x0:
      case 0x1:
         if((A & 0x7FFF) >= 0x6000)
         {
            const uint32 a = (A & 0x1FFF) | ((A >> 2) & 0x6000);
            if(VIP_MA16R16(CHR_RAM, a) != V)
               ChrChangedAt[a >> 4] = ++DrawSerial;
            VIP_MA16W16(CHR_RAM, a, V);
         }
         else
         {
            PassClean[(A >> 15) & 1] = false;
            PassTouched[(A >> 15) & 1] = true;
            NewContents((A >> 15) & 1);
            if((A & 63) < 56)
               Blocks[(A >> 15) & 1][(A & 63) >> 1].valid = false, CellContent[(A >> 15) & 1][(A & 63) >> 1][((A & 0x7FFF) >> 9) % 48] = 0,
                  CellClean[(A >> 15) & 1][((A & 0x7FFF) >> 9) % 48] = 0;
            StoreU16_LE((uint16 *)&FB[(A >> 15) & 1][(A >> 16) & 1][A & 0x7FFF], V);
         }
         break;

      case 0x2:
      case 0x3:
         if(VIP_MA16R16(DRAM, A & 0x1FFFF) != V)
         {
            DramChanging(A & 0x1FFFF, V);
            if((A & 0x1FFFF) >= 0x1DC00 && (A & 0x1FFFF) < 0x1E000)
               DisplayChanged();
         }
         VIP_MA16W16(DRAM, A & 0x1FFFF, V);
         break;
      case 0x4:
      case 0x5:
         if(A >= 0x5E000)
            WriteRegister(timestamp, A, V);
         break;
      case 0x6:
         break;
      case 0x7:
         if(A >= 0x8000)
         {
            if(VIP_MA16R16(CHR_RAM, A & 0x7FFF) != V)
               ChrChangedAt[(A & 0x7FFF) >> 4] = ++DrawSerial;
            VIP_MA16W16(CHR_RAM, A & 0x7FFF, V);
         }
         break;
   }
}

static struct MDFN_Surface *surface;
static bool skip;

void VIP_StartFrame(EmulateSpecStruct *espec)
{
   if(espec->VideoFormatChanged || VidSettingsDirty)
   {
      MakeColorLUT();
      Recalc3DModeStuff(espec->surface->format.colorspace != MDFN_COLORSPACE_RGB);
   }

   espec->DisplayRect.x = 0;
   espec->DisplayRect.y = 0;

   switch(VB3DMode)
   {
      default:
         espec->DisplayRect.w = 384;
         espec->DisplayRect.h = 224;
         break;

      case VB3DMODE_VLI:
         espec->DisplayRect.w = 768 * VBPrescale;
         espec->DisplayRect.h = 224;
         break;

      case VB3DMODE_HLI:
         espec->DisplayRect.w = 384;
         espec->DisplayRect.h = 448 * VBPrescale;
         break;

      case VB3DMODE_CSCOPE:
         espec->DisplayRect.w = 512;
         espec->DisplayRect.h = 384;
         break;

      case VB3DMODE_SIDEBYSIDE:
         espec->DisplayRect.w = 768 + VBSBS_Separation;
         espec->DisplayRect.h = 224;
         break;
   }

   surface = espec->surface;
   skip    = false;
   
   if(VidSettingsDirty)
   {
      memset(ShownClut, 0, sizeof(ShownClut));
      memset(EyeClut, 0, sizeof(EyeClut)); /* (as the surface: black until each column is copied again) */
#if defined(WANT_32BPP)
	  memset(surface->pixels, 0, surface->pitch32 * surface->h * 4);
#elif defined(WANT_16BPP)
	  memset(surface->pixels16, 0, surface->pitch32 * surface->h * 2);
#endif

      VidSettingsDirty = false;
   }
}

void VIP_ResetTS(void)
{
   if(SBOUT_InactiveTime >= 0)
      SBOUT_InactiveTime -= last_ts;
   last_ts = 0;
}

#include "vip_draw.inc"

/* The sprite lists VIP_DrawBlock would gather for the block now (hashed, and
 * into's sprites). */
static uint64 GatherBlockSprites(uint32 block_no, BlockInputs *into)
{
   uint16 list[1024];
   BlockInputs *const was = Drawing;
   uint64 h = OBJS_START;
   int world;
   Drawing = into;
   into->sprites = 0;
   obj_search_which = 3;
   for(world = 31; world >= 0; world--)
   {
      const uint16 control = DRAM[(0x1D800 + world * 0x20) >> 1];
      if(control & 0x40)
         break;
      if(((control >> 12) & 3) == BGM_OBJ)
      {
         const int count = GatherOBJ(list, block_no * 8);
         h = HashObjList(h, world | (control >> 9 & 0x60), list, count);
         NoteSprites(world | (control >> 9 & 0x60), list, count);
         if(obj_search_which)
            obj_search_which--;
      }
   }
   Drawing = was;
   return h;
}

static uint64 ObjListsHash(uint32 block_no)
{
   static BlockInputs scratch;
   return GatherBlockSprites(block_no, &scratch);
}

/* Whether a character can tag a pixel: it has pixels now, or the pass's blank table (fills) doesn't call it blank. */
static INLINE bool MayTag(uint32 chr)
{
   const uint16 *rows = &CHR_RAM[chr * 8];
   return !vbgo_tt_blank[chr] || (rows[0] | rows[1] | rows[2] | rows[3] | rows[4] | rows[5] | rows[6] | rows[7]);
}

/* Cells plus the ones background world w could tag in the block being drawn, as DrawBG would walk its map (a
 * source character at a time), until they're all of them; ~0 for an affine world. */
static uint64 WorldCellsNow(unsigned w, uint64 cells)
{
   const uint16 *world_ptr = &DRAM[(0x1D800 + w * 0x20) >> 1];
   const uint32 bgm = (world_ptr[0] >> 12) & 3, scy = (world_ptr[0] >> 8) & 3, scx = (world_ptr[0] >> 10) & 3;
   const bool over = world_ptr[0] & 0x80;
   const uint16 gx = sign_11_to_s16(world_ptr[1]), gp = ParallaxDisabled ? 0 : sign_9_to_s16(world_ptr[2]);
   const uint16 gy = sign_11_to_s16(world_ptr[3]), mx = world_ptr[4];
   const uint16 mp = ParallaxDisabled ? 0 : sign_9_to_s16(world_ptr[5]), my = world_ptr[6];
   const uint16 window_width = sign_11_to_s16(world_ptr[7]), window_height = world_ptr[8] & 0x3FF;
   const uint32 param_base = world_ptr[9] & 0xFFF0, bgsc_overplane = DRAM[world_ptr[10]];
   const uint32 SourceX_Size = 512 << scx, SourceY_Size = 512 << scy;
   const uint32 SourceX_Mask = over ? 0x1FFF : SourceX_Size - 1, SourceY_Mask = over ? 0x1FFF : SourceY_Size - 1;
   uint32 walked[16][3]; /* (a line walks the same map entries as one already walked: skipped) */
   int lr, y, nwalked = 0;
   if(bgm == BGM_AFFINE)
      return ~0ull;
   if(bgm == BGM_OBJ)
      return cells;
   for(lr = 0; lr < 2; lr++)
   {
      if(!(world_ptr[0] & (0x8000 >> lr)))
         continue;
      for(y = 0; y < 8 && cells != (1ull << 48) - 1; y++)
      {
         const uint16 RealY = DrawingBlock * 8 + y;
         uint16 srcX = mx + (lr ? mp : -mp), DestX = gx + (lr ? gp : -gp);
         const uint16 srcY = my + (RealY - gy);
         uint32 SourceX, SourceY, BGMap_Base;
         int32 start_x, final_x, x;
         if(bgm == 1)
            srcX += (int16)DRAM[(param_base + (((RealY - gy) * 2) | lr)) & 0xFFFF];
         if((uint16)(RealY - gy) > window_height)
            continue;
         SourceX = (int32)(int16)srcX;
         SourceY = (int32)(int16)srcY;
         DestX = sign_10_to_s16(DestX);
         if(DestX & 0x8000)
            SourceX -= DestX;
         start_x = (int16)DestX;
         final_x = (int16)DestX + window_width;
         if(start_x < 0)
            start_x = 0;
         if(final_x > 383)
            final_x = 383;
         SourceY &= SourceY_Mask;
         BGMap_Base = ((world_ptr[0] & 0xF) << 12) | (((SourceY >> 3) & 0x3F) * 0x40) | (((SourceY << 3) & ~0xFFF) << scx);
         {
            const uint32 key[3] = { BGMap_Base | (uint32)(SourceY < SourceY_Size) << 31, SourceX & SourceX_Mask,
                                    (uint32)start_x | (uint32)(final_x + 1) << 16 };
            int k;
            for(k = 0; k < nwalked && memcmp(walked[k], key, sizeof(key)); k++)
               ;
            if(k < nwalked)
               continue;
            memcpy(walked[nwalked++], key, sizeof(key));
         }
         for(x = start_x; x <= final_x;)
         {
            uint32 bgsc = bgsc_overplane;
            int32 step;
            SourceX &= SourceX_Mask;
            if(SourceX < SourceX_Size && SourceY < SourceY_Size)
               bgsc = DRAM[(BGMap_Base | ((SourceX << 3) & ~0xFFF) | ((SourceX >> 3) & 0x3F)) & 0xFFFF];
            step = 8 - (int32)(SourceX & 7);
            if(MayTag(bgsc & 0x7FF))
            {
               const int32 end = x + step - 1 < final_x ? x + step - 1 : final_x;
               cells |= (~0ull >> (63 - (end >> 3))) & ~((1ull << (x >> 3)) - 1);
            }
            x += step;
            SourceX += step;
         }
      }
   }
   return cells;
}

/* A block whose screen-wide worlds changed: cells plus the ones they tagged before or could tag now, if every one
 * of them is a background world (~0 if not, or if what they tagged isn't known). */
static uint64 ChangedWorldCells(uint32 changed, uint64 cells)
{
   int w;
   cells |= vbgo_tiletrack_world_cells_were(changed);
   for(w = 31; w >= 0 && cells != ~0ull && cells != (1ull << 48) - 1; w--)
   {
      if(DRAM[(0x1D800 + w * 0x20) >> 1] & 0x40)
         break;
      if(changed >> w & 1)
         cells = WorldCellsNow(w, cells);
   }
   return cells;
}

static INLINE void CopyFBColumnToTarget_Anaglyph_BASE(const bool DisplayActive_arg, const int lr)
{
   int y, y_sub;
   const int fb = DisplayFB;

#if defined(WANT_8BPP)
   uint8  *target = surface->pixels8  + Column;
#elif defined(WANT_16BPP)
   uint16 *target = surface->pixels16 + Column;
#else
   uint32 *target = surface->pixels   + Column;
#endif
   const int32 pitchinpix = surface->pitchinpix;
   const uint8 *fb_source = &FB[fb][lr][64 * Column];

   if (DisplayActive_arg)
   {
      if (lr)
      {
         for(y = 56; y; y--)
         {
            uint32 source_bits = *fb_source;

            for(y_sub = 4; y_sub; y_sub--)
            {
               uint32 pixel  = BrightCLUT[lr][source_bits & 3];
               *target      |= pixel;

               source_bits >>= 2;
               target       += pitchinpix;
            }
            fb_source++;
         }
      }
      else
      {
         for(y = 56; y; y--)
         {
            uint32 source_bits = *fb_source;

            for(y_sub = 4; y_sub; y_sub--)
            {
               uint32 pixel  = BrightCLUT[lr][source_bits & 3];
               *target       = pixel;

               source_bits >>= 2;
               target       += pitchinpix;
            }
            fb_source++;
         }
      }
   }
   else
   {
      if (lr)
      {
         for(y = 56; y; y--)
         {
            for(y_sub = 4; y_sub; y_sub--)
            {
               *target      |= 0;
               target       += pitchinpix;
            }
            fb_source++;
         }
      }
      else
      {
         for(y = 56; y; y--)
         {
            for(y_sub = 4; y_sub; y_sub--)
            {
               *target       = 0;
               target       += pitchinpix;
            }
            fb_source++;
         }
      }
   }
}

static void CopyFBColumnToTarget_Anaglyph(void)
{
   const int lr = (DisplayRegion & 2) >> 1;

   if(!lr)
      CopyFBColumnToTarget_Anaglyph_BASE(DisplayActive, 0);
   else
      CopyFBColumnToTarget_Anaglyph_BASE(DisplayActive, 1);
}

static uint32 AnaSlowBuf[384][224];

static INLINE void CopyFBColumnToTarget_AnaglyphSlow_BASE(const bool DisplayActive_arg, const int lr)
{
   const int fb = DisplayFB;
   const uint8 *fb_source = &FB[fb][lr][64 * Column];

   if(!lr)
   {
      uint32 *target = AnaSlowBuf[Column];

      if (DisplayActive_arg)
      {
         int y;
         for(y = 56; y; y--)
         {
            int y_sub;
            uint32 source_bits = *fb_source;

            for(y_sub = 4; y_sub; y_sub--)
            {
               uint32 pixel  = BrightnessCache[source_bits & 3];
               *target       = pixel;
               source_bits >>= 2;
               target++;
            }
            fb_source++;
         }
      }
      else
      {
         int y;
         for(y = 56; y; y--)
         {
            int y_sub;

            for(y_sub = 4; y_sub; y_sub--)
            {
               *target       = 0;
               target++;
            }
            fb_source++;
         }
      }
   }
   else
   {
      int y;
      uint32         *target = surface->pixels + Column;
      const uint32 *left_src = AnaSlowBuf[Column];
      const int32    pitch32 = surface->pitch32;

      for(y = 56; y; y--)
      {
         int y_sub;
         uint32 source_bits = *fb_source;

         for(y_sub = 4; y_sub; y_sub--)
         {
            uint32 pixel  = AnaSlowColorLUT
               [*left_src]
               [DisplayActive_arg ? BrightnessCache[source_bits & 3] : 0];

            *target       = pixel;

            source_bits >>= 2;
            target       += pitch32;
            left_src++;
         }
         fb_source++;
      }
   }
}

static void CopyFBColumnToTarget_AnaglyphSlow(void)
{
   const int lr = (DisplayRegion & 2) >> 1;

   if(!lr)
      CopyFBColumnToTarget_AnaglyphSlow_BASE(DisplayActive, 0);
   else
      CopyFBColumnToTarget_AnaglyphSlow_BASE(DisplayActive, 1);
}

static void CopyFBColumnToTarget_CScope_BASE(const bool DisplayActive_arg, const int lr, const int dest_lr)
{
   int y, y_sub;
   const int fb = DisplayFB;
   const uint8 *fb_source = &FB[fb][lr][64 * Column];

   if(dest_lr)
   {
      uint32 *target = surface->pixels + (512 - 16 - 1) + (Column) 
         * surface->pitch32;
      if(DisplayActive_arg)
      {
         for(y = 56; y; y--)
         {
            uint32 source_bits = *fb_source;

            for(y_sub = 4; y_sub; y_sub--)
            {
               *target       = BrightCLUT[lr][source_bits & 3];
               source_bits >>= 2;
               target--;
            }
            fb_source++;
         }
      }
      else
      {
         for(y = 56; y; y--)
         {
            for(y_sub = 4; y_sub; y_sub--)
            {
               *target       = 0;
               target--;
            }
            fb_source++;
         }
      }
   }
   else
   {
      uint32 *target = surface->pixels + 16 + (383 - Column) * surface->pitch32;
      if(DisplayActive_arg)
      {
         for(y = 56; y; y--)
         {
            uint32 source_bits = *fb_source;

            for(y_sub = 4; y_sub; y_sub--)
            {
               *target       = BrightCLUT[lr][source_bits & 3];
               source_bits >>= 2;
               target++;
            }
            fb_source++;
         }
      }
      else
      {
         for(y = 56; y; y--)
         {
            for(y_sub = 4; y_sub; y_sub--)
            {
               *target       = 0;
               target++;
            }
            fb_source++;
         }
      }
   }
}

static void CopyFBColumnToTarget_CScope(void)
{
   const int lr = (DisplayRegion & 2) >> 1;

   if(!lr)
      CopyFBColumnToTarget_CScope_BASE(DisplayActive, 0, 0 ^ VB3DReverse);
   else
      CopyFBColumnToTarget_CScope_BASE(DisplayActive, 1, 1 ^ VB3DReverse);
}

static void CopyFBColumnToTarget_SideBySide_BASE(const bool DisplayActive_arg, const int lr, const int dest_lr)
{
   const int fb = DisplayFB;
   const int eye = dest_lr & 1;
   const uint8 *fb_source = &FB[fb][lr][64 * Column];
   uint32 *shown = ShownContent[fb][eye][Column];
   int block;
   bool unknown, sameBits, reuse;

   ShownFB = fb;
   if(DisplayActive_arg)
   {
      const bool sameClut = ShownClut[fb][eye][Column] == vbgo_tt_clut_serial;
      ShownClut[fb][eye][Column] = vbgo_tt_clut_serial;
      /* (a buffer as it was converted, with the same brightness table: nothing to copy) */
      if(sameClut && FBVersion[fb] && ShownVersion[fb][eye][Column] == FBVersion[fb] && ShownLr[fb][eye][Column] == lr)
         return;
      sameBits = ShownLr[fb][eye][Column] == lr; /* (the bits are the shown eye's as they were converted) */
      memcpy(EyeClut[fb][eye][Column], BrightCLUT[lr], sizeof(EyeClut[0][0][0]));
      /* (no block's content edited since every one was found converted: the loop would skip them all) */
      if(sameBits && (CellClean[fb][Column >> 3] >> (eye << 3 | (Column & 7)) & 1))
      {
         ShownVersion[fb][eye][Column] = FBVersion[fb];
         return;
      }
      reuse = sameClut || sameBits;
      unknown = false;
      for(block = 0; block < 28; block++, fb_source += 2)
      {
         const uint32 content = CellContent[fb][block][Column >> 3];
         if(content)
         {
            if(reuse && shown[block] == content)
               continue;
         }
         else
            unknown = true;
         shown[block] = content;
         EyeBits[fb][eye][Column][block * 2] = fb_source[0];
         EyeBits[fb][eye][Column][block * 2 + 1] = fb_source[1];
      }
      ShownVersion[fb][eye][Column] = FBVersion[fb];
      ShownLr[fb][eye][Column] = lr;
      if(unknown)
         CellClean[fb][Column >> 3] &= ~(1 << (eye << 3 | (Column & 7)));
      else
         CellClean[fb][Column >> 3] |= 1 << (eye << 3 | (Column & 7));
   }
   else
   {
      ShownVersion[fb][eye][Column] = 0;
      CellClean[fb][Column >> 3] &= ~(1 << (eye << 3 | (Column & 7)));
      memset(EyeClut[fb][eye][Column], 0, sizeof(EyeClut[0][0][0]));
      for(block = 0; block < 28; block++)
         shown[block] = 0;
   }
}

static void CopyFBColumnToTarget_SideBySide(void)
{
   const int lr = (DisplayRegion & 2) >> 1;

   if(!lr)
      CopyFBColumnToTarget_SideBySide_BASE(DisplayActive, 0, 0 ^ VB3DReverse);
   else
      CopyFBColumnToTarget_SideBySide_BASE(DisplayActive, 1, 1 ^ VB3DReverse);
}

static INLINE void CopyFBColumnToTarget_VLI_BASE(const bool DisplayActive_arg, const int lr, const int dest_lr)
{
   const int fb           = DisplayFB;
   uint32 *target         = surface->pixels + Column * 2 * VBPrescale + dest_lr;
   const int32 pitch32    = surface->pitch32;
   const uint8 *fb_source = &FB[fb][lr][64 * Column];

   if(DisplayActive_arg)
   {
      int y;
      for(y = 56; y; y--)
      {
         int y_sub;
         uint32 source_bits = *fb_source;

         for(y_sub = 4; y_sub; y_sub--)
         {
            uint32 ps;
            uint32 tv = BrightCLUT[0][source_bits & 3];
            for(ps = 0; ps < VBPrescale; ps++)
               target[ps * 2] = tv;

            source_bits >>= 2;
            target += pitch32;
         }
         fb_source++;
      }
   }
   else
   {
      int y;
      for(y = 56; y; y--)
      {
         int y_sub;

         for(y_sub = 4; y_sub; y_sub--)
         {
            uint32 ps;
            uint32 tv = 0;
            for(ps = 0; ps < VBPrescale; ps++)
               target[ps * 2] = tv;

            target       += pitch32;
         }
         fb_source++;
      }
   }
}

static void CopyFBColumnToTarget_VLI(void)
{
   const int lr = (DisplayRegion & 2) >> 1;

   if(!lr)
      CopyFBColumnToTarget_VLI_BASE(DisplayActive, 0, 0 ^ VB3DReverse);
   else
      CopyFBColumnToTarget_VLI_BASE(DisplayActive, 1, 1 ^ VB3DReverse);
}

static INLINE void CopyFBColumnToTarget_HLI_BASE(const bool DisplayActive_arg, const int lr, const int dest_lr)
{
   const int fb = DisplayFB;
   const int32 pitch32 = surface->pitch32;
   uint32 *target = surface->pixels + Column + dest_lr * pitch32;
   const uint8 *fb_source = &FB[fb][lr][64 * Column];

   if(VBPrescale <= 4)
   {
      int y;
      for(y = 56; y; y--)
      {
         int y_sub;
         uint32 source_bits = HLILUT[*fb_source];

         for(y_sub = 4 * VBPrescale; y_sub; y_sub--)
         {
            if(DisplayActive_arg)
               *target = BrightCLUT[0][source_bits & 3];
            else
               *target = 0;

            target += pitch32 * 2;
            source_bits >>= 2;
         }
         fb_source++;
      }
   }
   else
   {
      int y;
      for(y = 56; y; y--)
      {
         int y_sub;
         uint32 source_bits = *fb_source;

         for(y_sub = 4; y_sub; y_sub--)
         {
            uint32 ps;
            for(ps = 0; ps < VBPrescale; ps++)
            {
               if(DisplayActive_arg)
                  *target = BrightCLUT[0][source_bits & 3];
               else
                  *target = 0;

               target += pitch32 * 2;
            }

            source_bits >>= 2;
         }
         fb_source++;
      }
   }
}

static void CopyFBColumnToTarget_HLI(void)
{
   const int lr = (DisplayRegion & 2) >> 1;

   if (!lr)
      CopyFBColumnToTarget_HLI_BASE(DisplayActive, 0, 0 ^ VB3DReverse);
   else
      CopyFBColumnToTarget_HLI_BASE(DisplayActive, 1, 1 ^ VB3DReverse);
}

/* Columns x0-x1 of the block just drawn into the drawing buffer's frame buffer; differs[cell] gets the bits that
   changed there in either eye (each column is read as it's overwritten: on the Wii every column is its own cache line). */
static void PackBlock(uint8 (*buffers)[512 * 8], int x0, int x1, uint8 *differs)
{
   int lr, x, k;
   for(lr = 0; lr < 2; lr++)
   {
      const uint8 *rows = buffers[lr] + 8;
      uint8 *to = FB[DrawingFB][lr] + DrawingBlock * 2;
      for(x = x0; x <= x1 && (x & 3); x++)
      {
         const uint8 lo = rows[x + 512 * 0] | (rows[x + 512 * 1] << 2) | (rows[x + 512 * 2] << 4) | (rows[x + 512 * 3] << 6);
         const uint8 hi = rows[x + 512 * 4] | (rows[x + 512 * 5] << 2) | (rows[x + 512 * 6] << 4) | (rows[x + 512 * 7] << 6);
         differs[x >> 3] |= (to[64 * x] ^ lo) | (to[64 * x + 1] ^ hi);
         to[64 * x + 0] = lo, to[64 * x + 1] = hi;
      }
      /* 4 columns at a time, a byte each in a word: each row masked to the bits that stay in its byte, as the byte
         arithmetic above keeps them */
      for(; x + 3 <= x1; x += 4)
      {
         static const uint32 keep[4] = {0xFFFFFFFFu, 0x3F3F3F3Fu, 0x0F0F0F0Fu, 0x03030303u};
         uint32 lo = 0, hi = 0, word;
         uint8 bytes[8];
         for(k = 0; k < 4; k++)
         {
            memcpy(&word, rows + x + 512 * k, 4);
            lo |= (word & keep[k]) << (2 * k);
            memcpy(&word, rows + x + 512 * (k + 4), 4);
            hi |= (word & keep[k]) << (2 * k);
         }
         memcpy(bytes, &lo, 4);
         memcpy(bytes + 4, &hi, 4);
         for(k = 0; k < 4; k++)
         {
            uint8 *at = to + 64 * (x + k);
            differs[(x + k) >> 3] |= (at[0] ^ bytes[k]) | (at[1] ^ bytes[4 + k]);
            at[0] = bytes[k], at[1] = bytes[4 + k];
         }
      }
      for(; x <= x1; x++)
      {
         const uint8 lo = rows[x + 512 * 0] | (rows[x + 512 * 1] << 2) | (rows[x + 512 * 2] << 4) | (rows[x + 512 * 3] << 6);
         const uint8 hi = rows[x + 512 * 4] | (rows[x + 512 * 5] << 2) | (rows[x + 512 * 6] << 4) | (rows[x + 512 * 7] << 6);
         differs[x >> 3] |= (to[64 * x] ^ lo) | (to[64 * x + 1] ^ hi);
         to[64 * x + 0] = lo, to[64 * x + 1] = hi;
      }
   }
}

v810_timestamp_t MDFN_FASTCALL VIP_Update(const v810_timestamp_t timestamp)
{
   int32 clocks = timestamp - last_ts;
   int32 running_timestamp = timestamp;

   while(clocks > 0)
   {
      int32 chunk_clocks = clocks;

      if(DrawingCounter > 0 && chunk_clocks > DrawingCounter)
         chunk_clocks = DrawingCounter;
      if(chunk_clocks > ColumnCounter)
         chunk_clocks = ColumnCounter;

      running_timestamp += chunk_clocks;

      if(DrawingCounter > 0)
      {
         DrawingCounter -= chunk_clocks;
         if(DrawingCounter <= 0)
         {
            MDFN_ALIGN(8) uint8 DrawingBuffers[2][512 * 8];	/* Don't decrease this from 512 unless you adjust vip_draw.inc(including areas that draw off-visible >= 384 and >= -7 for speed reasons) */

            BlockInputs *block = &Blocks[DrawingFB][DrawingBlock];
            if(skip && InstantDisplayHack && AllowDrawSkip)
               PassTouched[DrawingFB] = true, NewContents(DrawingFB), block->valid = false, memset(CellContent[DrawingFB][DrawingBlock], 0, sizeof(CellContent[0][0])), EditBlock(DrawingFB);
            else if(PassRepeats && DrawSerial == PassSerial[DrawingFB] && PassClean[!DrawingFB])
            {
               CopyRepeatedBlock();
               /* (it holds the other buffer's drawing now, with what that read) */
               if(Blocks[!DrawingFB][DrawingBlock].valid)
                  *block = Blocks[!DrawingFB][DrawingBlock];
               else
                  block->valid = false;
               memcpy(CellContent[DrawingFB][DrawingBlock], CellContent[!DrawingFB][DrawingBlock], sizeof(CellContent[0][0]));
               EditBlock(DrawingFB);
            }
            else
            {
               if(PassRepeats)
               {
                  /* (its inputs changed: the rest is drawn) */
                  NewContents(DrawingFB);
                  vbgo_tiletrack_resume_pass(DrawingFB, DrawingBlock);
                  PassRepeats = false;
                  PassAlreadyCopied = false;
               }
               vbgo_tiletrack_begin_block(DrawingBuffers[0], CHR_RAM, DRAM, DrawingBlock, DrawingFB, &JPLT_Cache[0][0]);
               int change = BlockChange(block);
               if(change == 3 && block->valid)
               {
                  /* (screen-wide worlds redrawn only where they were or can be, as narrow worlds are, when that's under
                   * a third of the block - each drawing walks every world; after it wasn't, not tried for a while) */
                  static uint8 Unpredictable[2][28];
                  uint8 *const skip = &Unpredictable[DrawingFB][DrawingBlock];
                  if(*skip)
                     --*skip;
                  else
                  {
                     const uint64 cells = ChangedWorldCells(ChangedWorlds, WorldDirty);
                     if(cells != ~0ull && __builtin_popcountll(cells & ((1ull << 48) - 1)) <= 15)
                        WorldDirty = cells, change = 1;
                     else
                        *skip = 4;
                  }
               }
               if(!change && vbgo_tiletrack_keep_block())
                  block->drawnAt = DrawSerial, block->passAt = PassStartedAt;
               else
               {
                  /* (what the buffer held, to tell which cells come out the same) */
                  static BlockInputs now; /* (only its sprites changed: their lists now) */
                  const uint64 all = (1ull << 48) - 1;
                  uint8 differs[48] = {}; /* (what PackBlock changed, per cell) */
                  const bool hadDrawing = block->valid;
                  uint64 same = 0, unchanged = change == 0 ? all : 0, dirty = all, spriteDirty = all;
                  bool partial = false;
                  int c;
                  if(change == 1)
                  {
                     now.objs = GatherBlockSprites(DrawingBlock, &now);
                     spriteDirty = SpriteChangedCells(block, block->sprites, block->drawnAt, block->passAt, &now) & all;
                     dirty = (spriteDirty | WorldDirty) & all;
                     unchanged = ~dirty & all;
                     /* (the cells it covers drawn again, the rest kept) */
                     partial = hadDrawing && dirty != all && vbgo_tiletrack_keep_cells(unchanged);
                  }
                  if(partial)
                  {
                     block->objs = now.objs;
                     block->sprites = now.sprites;
                     memcpy(block->sprite, now.sprite, sizeof(SpriteDrawn) * (now.sprites > 0 ? now.sprites : 0));
                     UsedChr = block->chr;
                     UsedRows = block->rows;
                     UsedBgChr = block->bgChr;
                     memset(vbgo_tt_wcells, 0, sizeof(vbgo_tt_wcells));
                     memset(vbgo_tt_fillc, 0, sizeof(vbgo_tt_fillc));
                     for(c = 0; c < 48;)
                     {
                        int end = c;
                        while(end < 48 && (dirty >> end & 1))
                           end++;
                        if(end == c)
                        {
                           c++;
                           continue;
                        }
                        ClipX0 = c * 8;
                        ClipX1 = end * 8 - 1;
                        VIP_DrawBlock(DrawingBlock, DrawingBuffers[0] + 8, DrawingBuffers[1] + 8);
                        PackBlock(DrawingBuffers, ClipX0, ClipX1, differs);
                        c = end;
                     }
                     ClipX0 = 0;
                     ClipX1 = 383;
                     /* (cells drawn again only for a world's change: as before unless that world shows there) */
                     unchanged |= WorldDirty & ~spriteDirty & ~vbgo_tiletrack_world_cells(ChangedWorlds, dirty);
                  }
                  else
                  {
                     /* (the cells sprites changed in, before the drawing records them anew) */
                     if(change == 3)
                     {
                        spriteDirty = 0;
                        if(SpritesChanged)
                        {
                           now.objs = GatherBlockSprites(DrawingBlock, &now);
                           spriteDirty = SpriteChangedCells(block, block->sprites, block->drawnAt, block->passAt, &now) & all;
                        }
                     }
                     vbgo_tiletrack_block_drawing(change == 2);
                     memset(block->chr, 0, sizeof(block->chr));
                     memset(block->rows, 0, sizeof(block->rows));
                     memset(block->bgChr, 0, sizeof(block->bgChr));
                     block->objs = OBJS_START;
                     block->sprites = 0;
                     UsedChr = block->chr;
                     UsedRows = block->rows;
                     UsedBgChr = block->bgChr;
                     UsedObjs = &block->objs;
                     Drawing = block;
                     memset(vbgo_tt_wcells, 0, sizeof(vbgo_tt_wcells));
                     memset(vbgo_tt_fillc, 0, sizeof(vbgo_tt_fillc));
                     VIP_DrawBlock(DrawingBlock, DrawingBuffers[0] + 8, DrawingBuffers[1] + 8);
                     PackBlock(DrawingBuffers, 0, 383, differs);
                     {
                        const uint64 maybe = vbgo_tiletrack_world_cells(change == 3 ? ChangedWorlds : 0, all);
                        if(change == 3)
                           unchanged = ~(maybe | spriteDirty) & all;
                     }
                  }
                  /* (a partial drawing changes only its cells' pixels) */
                  if(partial)
                     same = ~dirty & all;
                  if(hadDrawing)
                     for(c = 0; c < 48; c++)
                        if(!partial || (dirty >> c & 1))
                           same |= (uint64)!differs[c] << c;
                  {
                     const uint32 fresh = NewContent();
                     for(c = 0; c < 48; c++)
                        if(!(same >> c & 1) || !CellContent[DrawingFB][DrawingBlock][c])
                           CellContent[DrawingFB][DrawingBlock][c] = fresh, CellClean[DrawingFB][c] = 0;
                  }
                  block->valid = true;
                  block->drawnAt = DrawSerial;
                  block->passAt = PassStartedAt;
                  memcpy(block->worlds, &DRAM[0x1D800 >> 1], sizeof(block->worlds));
                  UsedChr = UsedRows = UsedBgChr = UnusedInputs;
                  UsedObjs = &UnusedObjs;
                  Drawing = NULL;
                  vbgo_tiletrack_block_drawn(block->chr, BlockSettled(block), same, unchanged);
               }
            }

            SBOUT_InactiveTime = running_timestamp + 1120;
            SB_Latch = DrawingBlock;	/* Not exactly correct, but probably doesn't matter. */

            DrawingBlock++;
            if(DrawingBlock == 28)
            {
               DrawingActive = false;
               PassRepeats = false;
               PassClean[DrawingFB] = DrawSerial == PassSerial[DrawingFB] && !PassTouched[DrawingFB];

               InterruptPending |= INT_XP_END;
               CheckIRQ();
            }
            else
               DrawingCounter += 1120 * 4;
         }
      }

      ColumnCounter -= chunk_clocks;
      if(ColumnCounter == 0)
      {
         if(DisplayRegion & 1)
         {
            if(!(Column & 3))
            {
               const int lr = (DisplayRegion & 2) >> 1;
               uint16 ctdata = VIP_MA16R16(DRAM, 0x1DFFE - ((Column >> 2) * 2) - (lr ? 0 : 0x200));

               if((ctdata >> 8) != Repeat)
               {
                  Repeat = ctdata >> 8;
                  RecalcBrightnessCache();
               }
            }
            if(!skip && !InstantDisplayHack)
               CopyFBColumnToTarget();
         }

         ColumnCounter = 259;
         Column++;
         if(Column == 384)
         {
            Column = 0;

            if(DisplayActive)
            {
               if(DisplayRegion & 1)	/* Did we just finish displaying an active region? */
               {
                  if(DisplayRegion & 2)	/* finished displaying right eye */
                     InterruptPending |= INT_RFB_END;
                  else		/* Otherwise, left eye */
                     InterruptPending |= INT_LFB_END;

                  CheckIRQ();
               }
            }

            DisplayRegion = (DisplayRegion + 1) & 3;

            if(DisplayRegion == 0)	/* New frame start */
            {
               DisplayActive = DPCTRL & 0x2;

               if(DisplayActive)
               {
                  InterruptPending |= INT_FRAME_START;
                  CheckIRQ();
               }
               GameFrameCounter++;
               if(GameFrameCounter > FRMCYC) /* New game frame start? */
               {
                  InterruptPending |= INT_GAME_START;
                  CheckIRQ();

                  if(XPCTRL & XPCTRL_XP_EN)
                  {
                     DisplayFB      = DrawingFB;
                     DrawingFB     ^= 1;
                     DrawingBlock   = 0;
                     DrawingActive  = true;
                     DrawingCounter = 1120 * 4;
                     BeginPass();
                  }

                  GameFrameCounter = 0;
               }
               StartFramePicture();

               if(!skip && InstantDisplayHack)
               {
                  int lr;
                  /* Ugly kludge, fix in the future. */
                  int32 save_DisplayRegion = DisplayRegion;
                  int32 save_Column = Column;
                  uint8 save_Repeat = Repeat;

                  for(lr = 0; lr < 2; lr++)
                  {
                     DisplayRegion = lr << 1;
                     for(Column = 0; Column < 384; Column++)
                     {
                        if(!(Column & 3))
                        {
                           uint16 ctdata = VIP_MA16R16(DRAM, 0x1DFFE - ((Column >> 2) * 2) - (lr ? 0 : 0x200));

                           if((ctdata >> 8) != Repeat)
                           {
                              Repeat = ctdata >> 8;
                              RecalcBrightnessCache();
                           }
                        }

                        CopyFBColumnToTarget();
                     }
                  }
                  DisplayRegion = save_DisplayRegion;
                  Column = save_Column;
                  Repeat = save_Repeat;
                  RecalcBrightnessCache();
               }

               VB_ExitLoop();
            }
         }
      }

      clocks -= chunk_clocks;
   }

   last_ts = timestamp;

   return (timestamp + ColumnCounter);
}

int VIP_StateAction(StateMem *sm, int load, int data_only)
{
   SFORMAT StateRegs[] =
   {
      SFARRAY(FB[0][0], 0x6000 * 2 * 2),
      SFARRAY16(CHR_RAM, 0x8000 / sizeof(uint16)),
      SFARRAY16(DRAM, 0x20000 / sizeof(uint16)),

      SFVAR(InterruptPending),
      SFVAR(InterruptEnable),

      SFVAR(BRTA),
      SFVAR(BRTB), 
      SFVAR(BRTC),
      SFVAR(REST),

      SFVAR(FRMCYC),
      SFVAR(DPCTRL),

      SFVAR(DisplayActive),

      SFVAR(XPCTRL),
      SFVAR(SBCMP),
      SFARRAY16(SPT, 4),
      SFARRAY16(GPLT, 4),	/* FIXME */
      SFARRAY16(JPLT, 4),

      SFVAR(BKCOL),

      SFVAR(Column),
      SFVAR(ColumnCounter),

      SFVAR(DisplayRegion),
      SFVAR(DisplayFB),

      SFVAR(GameFrameCounter),

      SFVAR(DrawingCounter),

      SFVAR(DrawingActive),
      SFVAR(DrawingFB),
      SFVAR(DrawingBlock),

      SFVAR(SB_Latch),
      SFVAR(SBOUT_InactiveTime),

      SFVAR(Repeat),
      SFEND
   };

   int ret = MDFNSS_StateAction(sm, load, data_only, StateRegs, "VIP", false);

   if(load)
   {
      int i;
      RecalcBrightnessCache();
      for(i = 0; i < 4; i++)
      {
         Recalc_GPLT_Cache(i);
         Recalc_JPLT_Cache(i);
      }
      ForgetPasses();
   }

   return(ret);
}

uint32 VIP_GetRegister(const unsigned int id, char *special, const uint32 special_len)
{
   switch(id)
   {
      case VIP_GSREG_IPENDING:
         return InterruptPending;
      case VIP_GSREG_IENABLE:
         return InterruptEnable;
      case VIP_GSREG_DPCTRL:
         return DPCTRL;
      case VIP_GSREG_BRTA:
         return BRTA;
      case VIP_GSREG_BRTB:
         return BRTB;
      case VIP_GSREG_BRTC:
         return BRTC;
      case VIP_GSREG_REST:
         return REST;
      case VIP_GSREG_FRMCYC:
         return FRMCYC;
      case VIP_GSREG_XPCTRL:
         return XPCTRL | (SBCMP << 8);
      case VIP_GSREG_SPT0:
      case VIP_GSREG_SPT1:
      case VIP_GSREG_SPT2:
      case VIP_GSREG_SPT3:
         return SPT[id - VIP_GSREG_SPT0];
      case VIP_GSREG_GPLT0:
      case VIP_GSREG_GPLT1:
      case VIP_GSREG_GPLT2:
      case VIP_GSREG_GPLT3:
         return GPLT[id - VIP_GSREG_GPLT0];
      case VIP_GSREG_JPLT0:
      case VIP_GSREG_JPLT1:
      case VIP_GSREG_JPLT2:
      case VIP_GSREG_JPLT3:
         return JPLT[id - VIP_GSREG_JPLT0];
      case VIP_GSREG_BKCOL:
         return BKCOL;
   }

   return 0xDEADBEEF;
}

void VIP_SetRegister(const unsigned int id, const uint32 value)
{
   switch(id)
   {
      case VIP_GSREG_IPENDING:
         InterruptPending = value & 0xE01F;
         CheckIRQ();
         break;

      case VIP_GSREG_IENABLE:
         InterruptEnable = value & 0xE01F;
         CheckIRQ();
         break;

      case VIP_GSREG_DPCTRL:
         DPCTRL = value & 0x703;	/* FIXME(Lower bit?) */
         break;

      case VIP_GSREG_BRTA:
         BRTA = value & 0xFF;
         RecalcBrightnessCache();
         break;

      case VIP_GSREG_BRTB:
         BRTB = value & 0xFF;
         RecalcBrightnessCache();
         break;

      case VIP_GSREG_BRTC:
         BRTC = value & 0xFF;
         RecalcBrightnessCache();
         break;

      case VIP_GSREG_REST:
         REST = value & 0xFF;
         RecalcBrightnessCache();
         break;

      case VIP_GSREG_FRMCYC:
         FRMCYC = value & 0xF;
         break;

      case VIP_GSREG_XPCTRL:
         XPCTRL = value & 0x2;
         SBCMP = (value >> 8) & 0x1f;
         break;

      case VIP_GSREG_SPT0:
      case VIP_GSREG_SPT1:
      case VIP_GSREG_SPT2:
      case VIP_GSREG_SPT3:
         SPT[id - VIP_GSREG_SPT0] = value & 0x3FF;
         ObjSerial++;
         break;

      case VIP_GSREG_GPLT0:
      case VIP_GSREG_GPLT1:
      case VIP_GSREG_GPLT2:
      case VIP_GSREG_GPLT3:
         GPLT[id - VIP_GSREG_GPLT0] = value & 0xFC;
         Recalc_GPLT_Cache(id - VIP_GSREG_GPLT0);
         break;

      case VIP_GSREG_JPLT0:
      case VIP_GSREG_JPLT1:
      case VIP_GSREG_JPLT2:
      case VIP_GSREG_JPLT3:
         JPLT[id - VIP_GSREG_JPLT0] = value & 0xFC;
         Recalc_JPLT_Cache(id - VIP_GSREG_JPLT0);
         break;

      case VIP_GSREG_BKCOL:
         BKCOL = value & 0x03;
         break;
   }
}
