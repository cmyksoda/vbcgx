/* The Wii's recompiler, part three: Broadway code for a block (see V810::Block). Every op does exactly what its
 * case in v810_oploop.inc does, cycles and lastop included. Included by v810_cpu.cpp. */

#include <utility>
#include <vector>

namespace
{
/* Kept for the whole block: the V810 registers (P_REG, then S_REG), the cycle count and where it
 * lives, the PSW, and work RAM. */
enum { JR_RAM = 27, JR_PSW = 28, JR_TSP = 29, JR_TS = 30, JR_REGS = 31 };
enum { BO_TRUE = 12, BO_FALSE = 4, CR_LT = 0, CR_GT = 1, CR_EQ = 2 };
/* The PSW flags an op sets, by group (S and Z always go together). */
enum { FLAG_SZ = 1, FLAG_OV = 2, FLAG_CY = 4 };
/* The flags an op sets when it does nothing else (the ALU ops); 0 for the rest, whose flags are always worked out. */
unsigned FlagsSetBy(int kind)
{
   switch(kind)
   {
   case BK_ADD: case BK_SUB: case BK_CMP: case BK_ADD_I: case BK_CMP_I: case BK_ADDI:
   case BK_SHL: case BK_SHR: case BK_SAR: case BK_SHL_I: case BK_SHR_I: case BK_SAR_I:
      return FLAG_SZ | FLAG_OV | FLAG_CY;
   case BK_OR: case BK_AND: case BK_XOR: case BK_NOT: case BK_ORI: case BK_XORI: case BK_ANDI:
      return FLAG_SZ | FLAG_OV;
   }
   return 0;
}

class PpcWriter
{
public:
   uint32 *at, *end;
   bool full;

   PpcWriter(uint32 *from, uint32 *to) : at(from), end(to), full(false) {}
   void Word(uint32 w) { if(at < end) *at++ = w; else full = true; }

   void Li(unsigned rt, int32 v) { D(14, rt, 0, v); }
   void Lis(unsigned rt, int32 v) { D(15, rt, 0, v); }
   void Imm32(unsigned rt, uint32 v)
   {
      if((int32)v >= -32768 && (int32)v <= 32767)
      {
         Li(rt, (int32)v);
         return;
      }
      Lis(rt, (int32)(v >> 16));
      if(v & 0xFFFF)
         Ori(rt, rt, v & 0xFFFF);
   }
   void Addi(unsigned rt, unsigned ra, int32 v) { D(14, rt, ra, v); } /* (never ra 0: that reads as the constant 0) */
   void Subfic(unsigned rt, unsigned ra, int32 v) { D(8, rt, ra, v); }
   void Addis(unsigned rt, unsigned ra, int32 v) { D(15, rt, ra, v); }
   void AddicDot(unsigned rt, unsigned ra, int32 v) { D(13, rt, ra, v); }
   void Ori(unsigned ra, unsigned rs, uint32 v) { D(24, rs, ra, v); }
   void Xori(unsigned ra, unsigned rs, uint32 v) { D(26, rs, ra, v); }
   void AndiDot(unsigned ra, unsigned rs, uint32 v) { D(28, rs, ra, v); }
   void Cmpwi(unsigned ra, int32 v) { D(11, 0, ra, v); }
   void Cmplwi(unsigned ra, uint32 v) { D(10, 0, ra, (int32)v); }
   void Mulli(unsigned rt, unsigned ra, int32 v) { D(7, rt, ra, v); }
   void Lwz(unsigned rt, int32 d, unsigned ra) { D(32, rt, ra, d); }
   void Stw(unsigned rs, int32 d, unsigned ra) { D(36, rs, ra, d); }
   void Lbz(unsigned rt, int32 d, unsigned ra) { D(34, rt, ra, d); }
   void Stwu(unsigned rs, int32 d, unsigned ra) { D(37, rs, ra, d); }
   void Lmw(unsigned rt, int32 d, unsigned ra) { D(46, rt, ra, d); }
   void Stmw(unsigned rs, int32 d, unsigned ra) { D(47, rs, ra, d); }

   void Cmpw(unsigned ra, unsigned rb) { X(0, ra, rb, 0); }
   void Or(unsigned ra, unsigned rs, unsigned rb, unsigned rc = 0) { X(rs, ra, rb, 444, rc); }
   void Mr(unsigned ra, unsigned rs) { Or(ra, rs, rs); }
   void And(unsigned ra, unsigned rs, unsigned rb, unsigned rc = 0) { X(rs, ra, rb, 28, rc); }
   void Xor(unsigned ra, unsigned rs, unsigned rb, unsigned rc = 0) { X(rs, ra, rb, 316, rc); }
   void Nor(unsigned ra, unsigned rs, unsigned rb, unsigned rc = 0) { X(rs, ra, rb, 124, rc); }
   void Slw(unsigned ra, unsigned rs, unsigned rb, unsigned rc = 0) { X(rs, ra, rb, 24, rc); }
   void Srw(unsigned ra, unsigned rs, unsigned rb, unsigned rc = 0) { X(rs, ra, rb, 536, rc); }
   void Sraw(unsigned ra, unsigned rs, unsigned rb, unsigned rc = 0) { X(rs, ra, rb, 792, rc); }
   void Srawi(unsigned ra, unsigned rs, unsigned sh, unsigned rc = 0) { X(rs, ra, sh, 824, rc); }
   void Cntlzw(unsigned ra, unsigned rs) { X(rs, ra, 0, 26); }
   void Extsb(unsigned ra, unsigned rs) { X(rs, ra, 0, 954); }
   void Extsh(unsigned ra, unsigned rs) { X(rs, ra, 0, 922); }
   void Lwzx(unsigned rt, unsigned ra, unsigned rb) { X(rt, ra, rb, 23); }
   void Lbzx(unsigned rt, unsigned ra, unsigned rb) { X(rt, ra, rb, 87); }
   void Lhbrx(unsigned rt, unsigned ra, unsigned rb) { X(rt, ra, rb, 790); }
   void Lwbrx(unsigned rt, unsigned ra, unsigned rb) { X(rt, ra, rb, 534); }
   void Stbx(unsigned rs, unsigned ra, unsigned rb) { X(rs, ra, rb, 215); }
   void Sthbrx(unsigned rs, unsigned ra, unsigned rb) { X(rs, ra, rb, 918); }
   void Stwbrx(unsigned rs, unsigned ra, unsigned rb) { X(rs, ra, rb, 662); }

   void Add(unsigned rt, unsigned ra, unsigned rb) { XO(rt, ra, rb, 0, 266, 0); }
   void Subf(unsigned rt, unsigned ra, unsigned rb, unsigned rc = 0) { XO(rt, ra, rb, 0, 40, rc); } /* (rb - ra) */
   void AddcoDot(unsigned rt, unsigned ra, unsigned rb) { XO(rt, ra, rb, 1, 10, 1); }
   void SubfcoDot(unsigned rt, unsigned ra, unsigned rb) { XO(rt, ra, rb, 1, 8, 1); }
   void Mullw(unsigned rt, unsigned ra, unsigned rb) { XO(rt, ra, rb, 0, 235, 0); }
   void Mulhw(unsigned rt, unsigned ra, unsigned rb) { XO(rt, ra, rb, 0, 75, 0); }
   void Mulhwu(unsigned rt, unsigned ra, unsigned rb) { XO(rt, ra, rb, 0, 11, 0); }
   void Divw(unsigned rt, unsigned ra, unsigned rb) { XO(rt, ra, rb, 0, 491, 0); }
   void Divwu(unsigned rt, unsigned ra, unsigned rb) { XO(rt, ra, rb, 0, 459, 0); }

   void Rlwinm(unsigned ra, unsigned rs, unsigned sh, unsigned mb, unsigned me, unsigned rc = 0)
   {
      Word(21u << 26 | rs << 21 | ra << 16 | sh << 11 | mb << 6 | me << 1 | rc);
   }
   void Rlwimi(unsigned ra, unsigned rs, unsigned sh, unsigned mb, unsigned me)
   {
      Word(20u << 26 | rs << 21 | ra << 16 | sh << 11 | mb << 6 | me << 1);
   }

   void Mfcr(unsigned rt) { Word(31u << 26 | rt << 21 | 19 << 1); }
   void Mfxer(unsigned rt) { Mfspr(rt, 1); }
   void Mflr(unsigned rt) { Mfspr(rt, 8); }
   void Mtlr(unsigned rs) { Mtspr(8, rs); }
   void Mfctr(unsigned rt) { Mfspr(rt, 9); }
   void Mtctr(unsigned rs) { Mtspr(9, rs); }
   void Blr() { Word(0x4E800020); }
   void Blrl() { Word(0x4E800021); }
   /* b / bl to code already written */
   void BTo(const uint32 *to, unsigned link = 0) { Word(BranchWord(at, to, link)); }
   /* Branches to be pointed somewhere later (Patch). */
   uint32 *B() { uint32 *w = at; Word(18u << 26); return full ? NULL : w; }
   uint32 *Bc(unsigned bo, unsigned bi)
   {
      uint32 *w = at;
      Word(16u << 26 | bo << 21 | bi << 16);
      return full ? NULL : w;
   }
   uint32 *Bdz() { return Bc(18, 0); }
   void Patch(uint32 *w, const uint32 *to)
   {
      if(!w)
         return;
      const int32 d = (int32)((const uint8 *)to - (const uint8 *)w);
      if((*w >> 26) != 18 && (d < -32768 || d > 32764))
         full = true; /* (out of reach) */
      *w |= (*w >> 26) == 18 ? (uint32)d & 0x03FFFFFC : (uint32)d & 0xFFFC;
   }

private:
   void D(unsigned op, unsigned rt, unsigned ra, int32 imm)
   {
      Word(op << 26 | rt << 21 | ra << 16 | ((uint32)imm & 0xFFFF));
   }
   void X(unsigned rs, unsigned ra, unsigned rb, unsigned xo, unsigned rc = 0)
   {
      Word(31u << 26 | rs << 21 | ra << 16 | rb << 11 | xo << 1 | rc);
   }
   void XO(unsigned rt, unsigned ra, unsigned rb, unsigned oe, unsigned xo, unsigned rc)
   {
      Word(31u << 26 | rt << 21 | ra << 16 | rb << 11 | oe << 10 | xo << 1 | rc);
   }
   void Mfspr(unsigned rt, unsigned spr)
   {
      Word(31u << 26 | rt << 21 | (spr & 31) << 16 | (spr >> 5) << 11 | 339 << 1);
   }
   void Mtspr(unsigned spr, unsigned rs)
   {
      Word(31u << 26 | rs << 21 | (spr & 31) << 16 | (spr >> 5) << 11 | 467 << 1);
   }
};

const int32 kLastopUnknown = -2147483647 - 1;
} // namespace

/* Code for the block, or NULL. CTR holds the cycles left before the next event (from ENTER on), so the interpreter's
 * per-op check is one bdz after a one-cycle op. It returns a JIT_EXIT_* (v810_blocks.inc), next PC in JitTarget. */
void *V810::CompileBlock(const Decoded *block, bool retried)
{
   const uint8 *const base = (const uint8 *)P_REG;
   const int32 offPsw = (const uint8 *)&S_REG[PSW] - base, offNext = (const uint8 *)&next_event_ts - base;
   const int32 offPending = (const uint8 *)&IPendingCache - base, offLastop = (const uint8 *)&lastop - base;
   const int32 offTarget = (const uint8 *)&JitTarget - base, offLinkSite = (const uint8 *)&JitLinkSite - base;
   const int32 offIdleQuiet = (const uint8 *)&IdleQuiet - base, offResumeAt = (const uint8 *)&JitResumeAt - base;
   const int32 offIdleHead = (const uint8 *)&IdleHead - base, offNotices = (const uint8 *)&EventNotices - base;
   const int32 offWaitAt = (const uint8 *)&WaitAt - base;
   const int32 offSystem = (const uint8 *)S_REG - base, offRunning = (const uint8 *)&Running - base;
   const int32 offHandler = (const uint8 *)&JitEventHandler - base;
   for(unsigned b = 0; b < 256; ++b)
      if(MemReadBus32[b] || MemWriteBus32[b])
         return NULL;
   for(int32 off : {offPsw, offNext, offPending, offLastop, offTarget, offLinkSite, offIdleQuiet, offIdleHead,
                    offNotices, offWaitAt, offResumeAt, offSystem + 4 * 31, offRunning, offHandler})
      if(off < 0 || off > 32767)
         return NULL;
   if(!JitWorkRam)
      return NULL;
   const unsigned n = block->count;
   static_assert(JIT_STUBS <= sizeof(JitStub) / sizeof(JitStub[0]), "JitStub");
   static_assert(JIT_LOOKUP_SIZE == 4096 && sizeof(JitLookupEntry) == 8, "BK_JMP's index into JitLookup");

   const size_t page = JitWritePage;
   uint32 *const flushFrom = JitCode + JitCodeUsed, *const coldFrom = JitCode + JitColdUsed;
   PpcWriter w(flushFrom, JitCode + page * JIT_PAGE_WORDS + JIT_HOT_WORDS),
       away(coldFrom, JitCode + (page + 1) * JIT_PAGE_WORDS);
   /* (w and away trade places while the code that seldom runs is written: slow paths, exits) */
   bool inCold = false;
   auto Cold = [&]() { if(!inCold) std::swap(w, away); inCold = true; };
   auto Hot = [&]() { if(inCold) std::swap(w, away); inCold = false; };
   /* ts = next_event_ts - CTR, and back */
   auto CtrToTs = [&]() {
      w.Mfctr(9);
      w.Lwz(0, offNext, JR_REGS);
      w.Subf(JR_TS, 9, 0);
      w.Stw(JR_TS, 0, JR_TSP);
   };
   auto TsToCtr = [&]() {
      w.Lwz(JR_TS, 0, JR_TSP);
      w.Lwz(0, offNext, JR_REGS);
      w.Subf(9, JR_TS, 0);
      w.Mtctr(9);
   };
   if(!JitStubEnd)
   {
      /* ENTER(P_REG, &timestamp, code) */
      JitStub[JIT_STUB_ENTER] = w.at;
      w.Stwu(1, -32, 1);
      w.Mflr(0);
      w.Stw(0, 36, 1);
      w.Stmw(JR_RAM, 12, 1);
      w.Mr(JR_REGS, 3);
      w.Mr(JR_TSP, 4);
      TsToCtr();
      w.Lwz(JR_PSW, offPsw, JR_REGS);
      w.Imm32(JR_RAM, (uint32)(uintptr_t)JitWorkRam);
      w.Mtlr(5);
      w.Blr();
      /* LEAVE(exit code), and LEAVE_TS with the cycle count already in JR_TS and *timestamp */
      JitStub[JIT_STUB_LEAVE] = w.at;
      CtrToTs();
      JitStub[JIT_STUB_LEAVE_TS] = w.at;
      w.Stw(JR_PSW, offPsw, JR_REGS);
      w.Lmw(JR_RAM, 12, 1);
      w.Lwz(0, 36, 1);
      w.Mtlr(0);
      w.Addi(1, 1, 32);
      w.Blr();
      /* Exits, reached by bl with their data in the words after it: the PC, then (all but INTERPRET_FIRST, before
       * a block's first op, where lastop stays as it is) lastop. A branch's bl is the one RunBlock links. */
      for(unsigned stub : {JIT_STUB_INTERPRET, JIT_STUB_INTERPRET_FIRST, JIT_STUB_BRANCH})
      {
         JitStub[stub] = w.at;
         w.Mflr(5);
         w.Lwz(4, 0, 5);
         w.Stw(4, offTarget, JR_REGS);
         if(stub == JIT_STUB_INTERPRET)
         {
            w.Lwz(4, 4, 5);
            w.Stw(4, offLastop, JR_REGS);
         }
         if(stub == JIT_STUB_BRANCH)
         {
            w.Addi(5, 5, -4);
            w.Stw(5, offLinkSite, JR_REGS);
         }
         w.Li(3, stub == JIT_STUB_BRANCH ? JIT_EXIT_BRANCH : JIT_EXIT_INTERPRET);
         w.BTo(JitStub[JIT_STUB_LEAVE]);
      }
      /* EVENTS (data: PC, lastop, where to leave to, where to go on or 0): before an op, runs the event handler as
       * the interpreter loop would while one is due, then goes on unless the frame ended or an interrupt is due. */
      JitStub[JIT_STUB_EVENTS] = w.at;
      w.Mflr(5);
      w.Lwz(4, 4, 5);
      w.Stw(4, offLastop, JR_REGS);
      w.Stwu(1, -16, 1);
      w.Stw(5, 8, 1);
      CtrToTs();
      w.Stw(JR_PSW, offPsw, JR_REGS);
      uint32 *const loop = w.at;
      w.Lwz(0, offNext, JR_REGS);
      w.Cmpw(JR_TS, 0);
      uint32 *const noEvent = w.Bc(BO_TRUE, CR_LT);
      /* (counted as the interpreter loop counts its handler calls, for WaitTarget) */
      w.Lwz(6, offNotices, JR_REGS);
      w.Addi(6, 6, 1);
      w.Stw(6, offNotices, JR_REGS);
      {
         /* A column boundary that only counts (instant display hack: no column table read, no region end, no drawing
          * step in reach) gets VIP_Update's and the handler's bookkeeping here, without the calls; all else calls. */
         int32 *lastTs, *columnCounter, *column, *displayRegion, *drawingCounter, *nextVip, *nextTimer, *nextInput;
         bool *instantHack;
         VIP_ColumnStateAt(&lastTs, &columnCounter, &column, &displayRegion, &drawingCounter, &instantHack);
         VB_EventTimesAt(&nextVip, &nextTimer, &nextInput);
         std::vector<uint32 *> slow;
         auto Load = [&](unsigned rt, unsigned base, const void *at) {
            w.Imm32(base, (uint32)(uintptr_t)at);
            w.Lwz(rt, 0, base);
         };
         Load(3, 10, nextVip);
         w.Cmpw(JR_TS, 3);
         slow.push_back(w.Bc(BO_TRUE, CR_LT));
         Load(3, 10, nextTimer);
         w.Cmpw(JR_TS, 3);
         slow.push_back(w.Bc(BO_FALSE, CR_LT));
         Load(3, 10, nextInput);
         w.Cmpw(JR_TS, 3);
         slow.push_back(w.Bc(BO_FALSE, CR_LT));
         w.Imm32(7, (uint32)(uintptr_t)instantHack);
         w.Lbz(7, 0, 7);
         w.Cmpwi(7, 0);
         slow.push_back(w.Bc(BO_TRUE, CR_EQ));
         Load(8, 7, lastTs);
         w.Subf(8, 8, JR_TS); /* (clocks since VIP_Update last ran) */
         Load(3, 9, columnCounter);
         w.Cmpw(8, 3);
         slow.push_back(w.Bc(BO_TRUE, CR_LT));
         w.Addi(4, 3, 259);
         w.Cmpw(8, 4);
         slow.push_back(w.Bc(BO_FALSE, CR_LT)); /* (a second boundary passed too) */
         Load(4, 10, column);
         w.Cmpwi(4, 383);
         slow.push_back(w.Bc(BO_TRUE, CR_EQ));
         Load(6, 11, displayRegion);
         w.AndiDot(6, 6, 1);
         uint32 *const hidden = w.Bc(BO_TRUE, CR_EQ);
         w.AndiDot(6, 4, 3);
         slow.push_back(w.Bc(BO_TRUE, CR_EQ));
         w.Patch(hidden, w.at);
         Load(6, 12, drawingCounter);
         w.Cmpwi(6, 0);
         uint32 *const notDrawing = w.Bc(BO_FALSE, CR_GT);
         w.Cmpw(6, 8);
         slow.push_back(w.Bc(BO_FALSE, CR_GT));
         w.Subf(6, 8, 6);
         w.Stw(6, 0, 12);
         w.Patch(notDrawing, w.at);
         w.Subf(6, 3, 8);
         w.Subfic(6, 6, 259);
         w.Stw(6, 0, 9);
         w.Addi(4, 4, 1);
         w.Stw(4, 0, 10);
         w.Stw(JR_TS, 0, 7);
         w.Add(6, JR_TS, 6);
         w.Imm32(10, (uint32)(uintptr_t)nextVip);
         w.Stw(6, 0, 10);
         /* (the handler's CalcNextTS: the earliest of the three) */
         for(int32 *other : {nextTimer, nextInput})
         {
            Load(3, 10, other);
            w.Cmpw(6, 3);
            uint32 *const earlier = w.Bc(BO_TRUE, CR_LT);
            w.Mr(6, 3);
            w.Patch(earlier, w.at);
         }
         w.Stw(6, offNext, JR_REGS);
         w.Patch(w.B(), loop);
         for(uint32 *at : slow)
            w.Patch(at, w.at);
      }
      w.Mr(3, JR_TS);
      w.Lwz(12, offHandler, JR_REGS);
      w.Mtlr(12);
      w.Blrl();
      w.Stw(3, offNext, JR_REGS);
      w.Lbz(0, offRunning, JR_REGS);
      w.Cmpwi(0, 0);
      uint32 *const frameEnd = w.Bc(BO_TRUE, CR_EQ);
      w.Patch(w.B(), loop);
      w.Patch(noEvent, w.at);
      w.Lwz(0, offPending, JR_REGS);
      w.Cmpwi(0, 0);
      uint32 *const interrupt = w.Bc(BO_FALSE, CR_EQ);
      w.Lwz(5, 8, 1);
      w.Lwz(6, 12, 5);
      w.Cmpwi(6, 0);
      uint32 *const nowhere = w.Bc(BO_TRUE, CR_EQ);
      w.Lwz(0, offNext, JR_REGS);
      w.Subf(9, JR_TS, 0);
      w.Mtctr(9);
      w.Addi(1, 1, 16);
      w.Mtlr(6);
      w.Blr();
      w.Patch(frameEnd, w.at);
      w.Li(3, JIT_EXIT_FRAME_END);
      uint32 *const leave = w.B();
      w.Patch(interrupt, w.at);
      w.Patch(nowhere, w.at);
      w.Li(3, JIT_EXIT_STOPPED);
      w.Patch(leave, w.at);
      w.Lwz(5, 8, 1);
      w.Addi(1, 1, 16);
      w.Lwz(4, 0, 5);
      w.Stw(4, offTarget, JR_REGS);
      w.Lwz(4, 8, 5);
      w.Stw(4, offResumeAt, JR_REGS);
      w.BTo(JitStub[JIT_STUB_LEAVE_TS]);
      /* IDLE (data: the branch target): the interpreter's idle check after a short branch back */
      JitStub[JIT_STUB_IDLE] = w.at;
      w.Stwu(1, -16, 1);
      w.Mflr(0);
      w.Stw(0, 20, 1);
      CtrToTs();
      w.Stw(JR_PSW, offPsw, JR_REGS);
      w.Imm32(3, (uint32)(uintptr_t)this);
      w.Mr(4, JR_TS);
      w.Lwz(5, 20, 1);
      w.Lwz(5, 0, 5);
      w.Imm32(12, (uint32)(uintptr_t)&JitIdleLoop);
      w.Mtlr(12);
      w.Blrl();
      w.Stw(3, 0, JR_TSP);
      TsToCtr();
      w.Lwz(12, 20, 1);
      w.Addi(12, 12, 4);
      w.Mtlr(12);
      w.Addi(1, 1, 16);
      w.Blr();
      /* WAIT (data: its PC, its displacement): WaitTarget before a taken WAIT, which then sets the cycle count */
      JitStub[JIT_STUB_WAIT] = w.at;
      w.Stwu(1, -16, 1);
      w.Mflr(0);
      w.Stw(0, 20, 1);
      CtrToTs();
      w.Stw(JR_PSW, offPsw, JR_REGS);
      w.Imm32(3, (uint32)(uintptr_t)this);
      w.Mr(4, JR_TS);
      w.Lwz(7, 20, 1);
      w.Lwz(5, 0, 7);
      w.Lwz(6, 4, 7);
      w.Imm32(12, (uint32)(uintptr_t)&JitWaitTarget);
      w.Mtlr(12);
      w.Blrl();
      w.Lwz(12, 20, 1);
      w.Addi(12, 12, 8);
      w.Mtlr(12);
      w.Addi(1, 1, 16);
      w.Blr();
      /* Memory accesses through the C side, fn(timestamp, r4, r5), with the cycle count and PSW where it sees
       * them. */
      const void *const fns[6] = {(const void *)&::MemRead8,   (const void *)&::MemRead16,
                                  (const void *)&::VBReadWord, (const void *)&::MemWrite8,
                                  (const void *)&::MemWrite16, (const void *)&::VBWriteWord};
      /* Branches to miss[] unless r4 is a frame buffer address (region 0, below 0x20000, under 0x6000 of its 0x8000;
       * word: 4-aligned), as VIP_ReadFBWord and VIP_WriteFBWord check one; A & 0x7FFF left in rOffset. */
      auto CheckFrameBuffer = [&](uint32 *miss[4], unsigned rOffset, bool word) {
         w.Rlwinm(0, 4, 8, 29, 31, 1);
         miss[0] = w.Bc(BO_FALSE, CR_EQ);
         w.Rlwinm(rOffset, 4, 0, 5, 14, 1);
         miss[1] = w.Bc(BO_FALSE, CR_EQ);
         w.Rlwinm(rOffset, 4, 0, 17, 31);
         w.Cmplwi(rOffset, 0x6000);
         miss[2] = w.Bc(BO_FALSE, CR_LT);
         if(word)
         {
            w.AndiDot(0, 4, 3);
            miss[3] = w.Bc(BO_FALSE, CR_EQ);
         }
      };
      for(unsigned k = 0; k < 6; ++k)
      {
         JitStub[JIT_STUB_READ8 + k] = w.at;
         if(k < 3)
         {
            /* A frame buffer read has no side effects: its bytes straight from FB, no call. */
            uint32 *miss[4] = {};
            CheckFrameBuffer(miss, 5, k == 2);
            /* (FB[(A >> 15) & 1][(A >> 16) & 1][FBOffset(A)], as VIP_Read8 indexes it) */
            w.Rlwinm(0, 4, 21, 26, 26);
            w.Rlwinm(6, 4, 23, 25, 25);
            w.Rlwimi(0, 4, 31, 27, 31);
            w.Or(0, 0, 6);
            w.Mulli(0, 0, 768);
            w.Rlwinm(5, 4, 27, 22, 30);
            if(k == 0)
               w.Rlwimi(5, 4, 0, 31, 31);
            w.Add(5, 5, 0);
            w.Imm32(6, (uint32)(uintptr_t)VIP_FBBase());
            if(k == 0)
               w.Lbzx(3, 5, 6);
            else
               w.Lhbrx(3, 5, 6);
            if(k == 2)
            {
               /* (a word's two halfwords are a band apart: vip.c's FB_BAND) */
               w.Addi(5, 5, 768);
               w.Lhbrx(0, 5, 6);
               w.Rlwimi(3, 0, 16, 0, 15);
            }
            w.Blr();
            for(uint32 *m : miss)
               w.Patch(m, w.at);
         }
         if(k == 5)
         {
            /* A frame buffer word write: VIP_WriteFBWord does it, and nothing it does looks at the cycle count or the
             * PSW, so neither is synced. */
            uint32 *miss[4] = {};
            CheckFrameBuffer(miss, 6, true);
            w.Stwu(1, -16, 1);
            w.Mflr(0);
            w.Stw(0, 20, 1);
            w.Mfctr(0); /* (CTR holds the cycles left, and C code may change it) */
            w.Stw(0, 8, 1);
            w.Rlwinm(3, 4, 0, 5, 31);
            w.Mr(4, 5);
            w.Imm32(12, (uint32)(uintptr_t)&::VIP_WriteFBWord);
            w.Mtlr(12);
            w.Blrl();
            w.Lwz(0, 8, 1);
            w.Mtctr(0);
            w.Lwz(0, 20, 1);
            w.Mtlr(0);
            w.Addi(1, 1, 16);
            w.Blr();
            for(uint32 *m : miss)
               w.Patch(m, w.at);
         }
         w.Stwu(1, -16, 1);
         w.Mflr(0);
         w.Stw(0, 20, 1);
         CtrToTs();
         w.Stw(JR_PSW, offPsw, JR_REGS);
         w.Mr(3, JR_TSP);
         w.Imm32(12, (uint32)(uintptr_t)fns[k]);
         w.Mtlr(12);
         w.Blrl();
         TsToCtr();
         w.Lwz(0, 20, 1);
         w.Mtlr(0);
         w.Addi(1, 1, 16);
         w.Blr();
      }
      while((w.at - JitCode) & 7)
         w.Word(0);
      if(w.full)
         return NULL;
      JitCodeUsed = JitStubEnd = w.at - JitCode;
   }

   uint32 *const start = w.at;
   /* flagsOf: the op before, whose flags in flagsLeft are still to work out (from CR0 and XER as it left them) */
   struct Exit
   {
      uint32 offset; int32 lastop; uint32 stub; std::vector<uint32 *> from;
      const BlockOp *flagsOf; unsigned flagsLeft;
   };
   std::vector<Exit> exits;
   std::vector<std::pair<uint32, uint32 *>> opAt; /* (each op's offset and code) */
   /* A branch to the exit before the op at offset (lastop as the op before left it). */
   auto ExitBefore = [&](uint32 *from, uint32 offset, int32 lastopNow, uint32 stub) {
      for(Exit &e : exits)
         if(e.offset == offset && e.stub == stub)
         {
            e.from.push_back(from);
            return;
         }
      exits.push_back(Exit{offset, lastopNow, stub, {from}, NULL, 0});
   };
   auto Get = [&](unsigned host, unsigned vreg) {
      if(vreg)
         w.Lwz(host, 4 * vreg, JR_REGS);
      else
         w.Li(host, 0);
   };
   auto Put = [&](unsigned vreg, unsigned host) {
      if(vreg) /* (what the interpreter writes to r0 is never seen: it zeroes r0 before every op) */
         w.Stw(host, 4 * vreg, JR_REGS);
   };
   /* The flags of the op being compiled that are left for its exit to work out (its next op sets them again); the
    * lambdas below leave those out. */
   unsigned skip = 0;
   /* PSW Z and S from CR0; OV and CY from XER. */
   auto FlagsSZ = [&]() {
      if(skip & FLAG_SZ)
         return;
      w.Mfcr(12);
      w.Rlwimi(JR_PSW, 12, 3, 31, 31);
      w.Rlwimi(JR_PSW, 12, 2, 30, 30);
   };
   auto FlagsOVCY = [&](bool borrow) {
      const unsigned groups = ~skip & (FLAG_OV | FLAG_CY);
      if(!groups)
         return;
      w.Mfxer(11);
      if(groups & FLAG_OV)
         w.Rlwimi(JR_PSW, 11, 4, 29, 29);
      if(groups & FLAG_CY)
      {
         w.Rlwimi(JR_PSW, 11, 6, 28, 28);
         if(borrow) /* (PowerPC's carry from a subtraction means no borrow) */
            w.Xori(JR_PSW, JR_PSW, PSW_CY);
      }
   };
   auto ClearOV = [&]() {
      if(!(skip & FLAG_OV))
         w.Rlwinm(JR_PSW, JR_PSW, 0, 30, 28);
   };
   /* A shift's CY: the last bit shifted out (a shift by 0 clears it); r4 the value, r3 a register shift's count */
   auto ShiftCY = [&](const BlockOp &o) {
      if(skip & FLAG_CY)
         return;
      if(o.kind == BK_SHL_I || o.kind == BK_SHR_I || o.kind == BK_SAR_I)
      {
         if(!o.r1)
            w.Rlwinm(JR_PSW, JR_PSW, 0, 29, 27);
         else
         {
            w.Rlwinm(6, 4, o.kind == BK_SHL_I ? o.r1 : (33 - o.r1) & 31, 31, 31);
            w.Rlwimi(JR_PSW, 6, 3, 28, 28);
         }
         return;
      }
      /* (srw by 32 or more gives 0, as a shift by 0 must) */
      if(o.kind == BK_SHL)
         w.Subfic(6, 3, 32);
      else
         w.Addi(6, 3, -1);
      w.Srw(6, 4, 6);
      w.Rlwimi(JR_PSW, 6, 3, 28, 28);
   };
   auto FlagsOf = [&](const BlockOp &o) {
      switch(o.kind)
      {
      case BK_ADD: case BK_SUB: case BK_CMP:
         FlagsSZ();
         FlagsOVCY(o.kind != BK_ADD);
         break;
      case BK_ADD_I: case BK_CMP_I:
         FlagsSZ();
         FlagsOVCY(o.kind == BK_CMP_I);
         break;
      case BK_ADDI:
         FlagsSZ();
         FlagsOVCY(false);
         break;
      case BK_SHL: case BK_SHR: case BK_SAR: case BK_SHL_I: case BK_SHR_I: case BK_SAR_I:
         ShiftCY(o);
         FlagsSZ();
         ClearOV();
         break;
      default:
         FlagsSZ();
         ClearOV();
         break;
      }
   };
   /* An op's last cycles, then the check before the op at next: out there if the event is due. One cycle needs
    * just a bdz, as CTR is at least 1 when an op starts; after the C side ran (signedOnly) it may not be. */
   auto Spend = [&](int32 cycles, uint32 next, int32 lastopNext, bool signedOnly = false) {
      if(cycles == 1 && !signedOnly)
      {
         ExitBefore(w.Bdz(), next, lastopNext, JIT_STUB_EVENTS);
         return;
      }
      w.Mfctr(9);
      if(cycles)
      {
         w.AddicDot(9, 9, -cycles);
         w.Mtctr(9);
      }
      else
         w.Cmpwi(9, 0);
      ExitBefore(w.Bc(BO_FALSE, CR_GT), next, lastopNext, JIT_STUB_EVENTS);
   };
   /* The same with the cycle count in r9. */
   auto SpendR9 = [&](int32 plus, uint32 next, int32 lastopNext) {
      w.Mfctr(10);
      w.Subf(10, 9, 10, plus ? 0 : 1);
      if(plus)
         w.AddicDot(10, 10, -plus);
      w.Mtctr(10);
      ExitBefore(w.Bc(BO_FALSE, CR_GT), next, lastopNext, JIT_STUB_EVENTS);
   };
   /* CTR minus one, no check (an op's first cycle) */
   auto FirstCycle = [&]() {
      uint32 *const bdz = w.Bdz();
      w.Patch(bdz, w.at);
   };
   /* After an access that called the C side: an interrupt it raised is taken before the next op. */
   auto PendingExit = [&](uint32 next, int32 lastopNext) {
      w.Lwz(0, offPending, JR_REGS);
      w.Cmpwi(0, 0);
      ExitBefore(w.Bc(BO_FALSE, CR_EQ), next, lastopNext, JIT_STUB_EVENTS);
   };
   /* Into r4: a load or store's address. */
   auto Address = [&](unsigned vbase, uint32 imm) {
      Get(4, vbase);
      if(sign_16(imm))
         w.Addi(4, 4, (int32)sign_16(imm));
   };
   /* The load and store cycles that depend on the op before: known, or (a block's first) into r9 at run time. */
   auto LoadExtra = [&](int32 before, bool word) -> int32 {
      if(before != kLastopUnknown)
         return before < 0 ? 0 : before == LASTOP_LD ? (word ? 3 : 1) : (word ? 4 : 2);
      w.Lwz(0, offLastop, JR_REGS);
      w.Li(9, 0);
      w.Cmpwi(0, 0);
      uint32 *negative = w.Bc(BO_TRUE, CR_LT);
      w.Li(9, word ? 4 : 2);
      w.Cmpwi(0, LASTOP_LD);
      uint32 *other = w.Bc(BO_FALSE, CR_EQ);
      w.Li(9, word ? 3 : 1);
      w.Patch(other, w.at);
      w.Patch(negative, w.at);
      return -1;
   };
   auto StoreExtra = [&](int32 before, bool word) -> int32 {
      if(before != kLastopUnknown)
         return before == LASTOP_ST ? (word ? 3 : 1) : 0;
      w.Lwz(0, offLastop, JR_REGS);
      w.Li(9, 0);
      w.Cmpwi(0, LASTOP_ST);
      uint32 *other = w.Bc(BO_FALSE, CR_EQ);
      w.Li(9, word ? 3 : 1);
      w.Patch(other, w.at);
      return -1;
   };
   /* r3 = 1 if condition cond (0-15, as Bcond's and SETF's) holds, else 0 */
   auto Condition = [&](unsigned cond) {
      switch(cond & 7)
      {
      case COND_V: w.Rlwinm(3, JR_PSW, 30, 31, 31); break;
      case COND_C: w.Rlwinm(3, JR_PSW, 29, 31, 31); break;
      case COND_Z: w.Rlwinm(3, JR_PSW, 0, 31, 31); break;
      case COND_NH:
         w.Rlwinm(3, JR_PSW, 0, 31, 31);
         w.Rlwinm(4, JR_PSW, 29, 31, 31);
         w.Or(3, 3, 4);
         break;
      case COND_S: w.Rlwinm(3, JR_PSW, 31, 31, 31); break;
      case COND_T: w.Li(3, 1); break;
      case COND_LT: case COND_LE:
         w.Rlwinm(3, JR_PSW, 31, 31, 31);
         w.Rlwinm(4, JR_PSW, 30, 31, 31);
         w.Xor(3, 3, 4);
         if((cond & 7) == COND_LE)
         {
            w.Rlwinm(4, JR_PSW, 0, 31, 31);
            w.Or(3, 3, 4);
         }
         break;
      }
      if(cond >= 8) /* (8-15 are 0-7 negated) */
         w.Xori(3, 3, 1);
   };
   /* CR0 EQ if the condition cond (0-7, not T) fails: shorter than Condition's 0 or 1 in r3 */
   auto TestCondition = [&](unsigned cond) {
      switch(cond)
      {
      case COND_V: w.AndiDot(3, JR_PSW, PSW_OV); break;
      case COND_C: w.AndiDot(3, JR_PSW, PSW_CY); break;
      case COND_Z: w.AndiDot(3, JR_PSW, PSW_Z); break;
      case COND_NH: w.AndiDot(3, JR_PSW, PSW_Z | PSW_CY); break;
      case COND_S: w.AndiDot(3, JR_PSW, PSW_S); break;
      case COND_LT: case COND_LE:
         /* (adding 2 carries S into OV's bit, leaving S xor OV there; Z stays in bit 0) */
         w.Addi(3, JR_PSW, 2);
         w.AndiDot(3, 3, cond == COND_LE ? PSW_OV | PSW_Z : PSW_OV);
         break;
      }
   };
   /* Events due at a branch target, then straight on (EVENTS' data: leaving, it starts at the target afresh). */
   auto EventsAt = [&](uint32 target, unsigned opcode, uint32 *goOn) {
      w.BTo(JitStub[JIT_STUB_EVENTS], 1);
      w.Word(target);
      w.Word(opcode);
      w.Word(0);
      w.Word((uint32)(uintptr_t)goOn);
   };
   /* A branch's cycles and a known target: straight on into its code once compiled (RunBlock points the bl there),
    * else back to the C side; events due first. back: the interpreter's idle check after a short branch back. */
   auto BranchTo = [&](uint32 target, unsigned opcode, bool back, int32 cycles) {
      w.Li(4, opcode);
      w.Stw(4, offLastop, JR_REGS);
      uint32 *due = NULL, *idle = NULL;
      if(cycles == 1 && !back)
         due = w.Bdz();
      else
      {
         w.Mfctr(9);
         if(back)
            w.Addi(9, 9, -cycles);
         else
            w.AddicDot(9, 9, -cycles);
         w.Mtctr(9);
         if(back)
         {
            w.Lwz(10, offIdleQuiet, JR_REGS);
            w.Cmpwi(10, 0);
            idle = w.Bc(BO_TRUE, CR_EQ);
            w.Addi(10, 10, -1);
            w.Stw(10, offIdleQuiet, JR_REGS);
         }
         if(back)
            w.Cmpwi(9, 0);
         due = w.Bc(BO_FALSE, CR_GT);
      }
      uint32 *const link = w.at;
      w.BTo(JitStub[JIT_STUB_BRANCH], 1);
      w.Word(target);
      Cold();
      if(idle)
      {
         w.Patch(idle, w.at);
         w.BTo(JitStub[JIT_STUB_IDLE], 1);
         w.Word(target);
         w.Mfctr(9);
         w.Cmpwi(9, 0);
         w.Patch(w.Bc(BO_TRUE, CR_GT), link);
      }
      w.Patch(due, w.at);
      EventsAt(target, opcode, link);
      Hot();
   };

   uint32 offset = 0;
   int32 lastopNow = kLastopUnknown;
   bool ended = false;
   for(unsigned i = 0; i < n && !ended; ++i)
   {
      opAt.push_back(std::make_pair(offset, w.at));
      const BlockOp &op = block->ops[i];
      const unsigned arg1 = op.r1, arg2 = op.r2;
      const uint32 pc = block->pc + offset;
      /* (flags the next op sets again: only this op's exit could see them, and it works them out) */
      skip = i + 1 < n ? FlagsSetBy(op.kind) & FlagsSetBy(block->ops[i + 1].kind) : 0;
      switch(op.kind)
      {
      case BK_MOV:
         Get(3, arg1);
         Put(arg2, 3);
         break;
      case BK_ADD: case BK_SUB: case BK_CMP:
         Get(3, arg1);
         Get(4, arg2);
         if(op.kind == BK_ADD)
            w.AddcoDot(5, 4, 3);
         else
            w.SubfcoDot(5, 3, 4);
         FlagsSZ();
         FlagsOVCY(op.kind != BK_ADD);
         if(op.kind != BK_CMP)
            Put(arg2, 5);
         break;
      case BK_OR: case BK_AND: case BK_XOR:
         Get(3, arg1);
         Get(4, arg2);
         if(op.kind == BK_OR)
            w.Or(5, 3, 4, 1);
         else if(op.kind == BK_AND)
            w.And(5, 3, 4, 1);
         else
            w.Xor(5, 3, 4, 1);
         FlagsSZ();
         ClearOV();
         Put(arg2, 5);
         break;
      case BK_NOT:
         Get(3, arg1);
         w.Nor(5, 3, 3, 1);
         FlagsSZ();
         ClearOV();
         Put(arg2, 5);
         break;
      case BK_MOV_I:
         w.Li(3, (int32)sign_5(arg1));
         Put(arg2, 3);
         break;
      case BK_ADD_I: case BK_CMP_I:
         Get(4, arg2);
         w.Li(3, (int32)sign_5(arg1));
         if(op.kind == BK_ADD_I)
            w.AddcoDot(5, 4, 3);
         else
            w.SubfcoDot(5, 3, 4);
         FlagsSZ();
         FlagsOVCY(op.kind == BK_CMP_I);
         if(op.kind == BK_ADD_I)
            Put(arg2, 5);
         break;
      case BK_SHL_I: case BK_SHR_I: case BK_SAR_I:
         Get(4, arg2);
         ShiftCY(op);
         if(!arg1)
         {
            w.Mr(5, 4);
            w.Cmpwi(5, 0);
         }
         else
         {
            if(op.kind == BK_SHL_I)
               w.Rlwinm(5, 4, arg1, 0, 31 - arg1, 1);
            else if(op.kind == BK_SHR_I)
               w.Rlwinm(5, 4, 32 - arg1, arg1, 31, 1);
            else
               w.Srawi(5, 4, arg1, 1);
         }
         FlagsSZ();
         ClearOV();
         Put(arg2, 5);
         break;
      case BK_SHL: case BK_SHR: case BK_SAR:
         Get(3, arg1);
         w.Rlwinm(3, 3, 0, 27, 31);
         Get(4, arg2);
         ShiftCY(op);
         if(op.kind == BK_SHL)
            w.Slw(5, 4, 3, 1);
         else if(op.kind == BK_SHR)
            w.Srw(5, 4, 3, 1);
         else
            w.Sraw(5, 4, 3, 1);
         FlagsSZ();
         ClearOV();
         Put(arg2, 5);
         break;
      case BK_SETF:
         Condition(arg1 & 15);
         Put(arg2, 3);
         break;
      case BK_STSR:
         if((arg1 & 31) == PSW)
            Put(arg2, JR_PSW);
         else
         {
            w.Lwz(3, offSystem + 4 * (arg1 & 31), JR_REGS);
            Put(arg2, 3);
         }
         break;
      case BK_MOVEA:
         if(arg1)
         {
            Get(4, arg1);
            w.Addi(5, 4, (int32)sign_16(op.imm));
         }
         else
            w.Li(5, (int32)sign_16(op.imm));
         Put(arg2, 5);
         break;
      case BK_ADDI:
         Get(4, arg1);
         w.Li(3, (int32)sign_16(op.imm));
         w.AddcoDot(5, 4, 3);
         FlagsSZ();
         FlagsOVCY(false);
         Put(arg2, 5);
         break;
      case BK_ORI: case BK_XORI:
         Get(4, arg1);
         if(op.kind == BK_ORI)
            w.Ori(5, 4, op.imm);
         else
            w.Xori(5, 4, op.imm);
         w.Cmpwi(5, 0);
         FlagsSZ();
         ClearOV();
         Put(arg2, 5);
         break;
      case BK_ANDI:
         Get(4, arg1);
         w.AndiDot(5, 4, op.imm);
         FlagsSZ();
         ClearOV();
         Put(arg2, 5);
         break;
      case BK_MOVHI:
         if(arg1)
         {
            Get(4, arg1);
            w.Addis(5, 4, (int32)op.imm);
         }
         else
            w.Lis(5, (int32)op.imm);
         Put(arg2, 5);
         break;
      case BK_LD_B: case BK_LD_H: case BK_LD_W:
      {
         const bool word = op.kind == BK_LD_W;
         const uint32 next = offset + 4;
         const unsigned clear = op.kind == BK_LD_H ? 1 : word ? 2 : 0; /* (low address bits an access ignores) */
         Address(arg1, op.imm);
         w.Rlwinm(5, 4, 8, 29, 31);
         w.Cmpwi(5, 5);
         uint32 *slow = w.Bc(BO_FALSE, CR_EQ);
         /* (work RAM, little-endian) */
         w.Rlwinm(6, 4, 0, 16, 31 - clear);
         if(op.kind == BK_LD_B)
            w.Lbzx(3, JR_RAM, 6);
         else if(op.kind == BK_LD_H)
            w.Lhbrx(3, JR_RAM, 6);
         else
            w.Lwbrx(3, JR_RAM, 6);
         uint32 *onward = NULL;
         for(int path = 0; path < 2; ++path)
         {
            if(path)
            {
               onward = w.at;
               Cold();
               w.Patch(slow, w.at);
               if(clear)
                  w.Rlwinm(4, 4, 0, 0, 31 - clear);
               FirstCycle();
               w.BTo(JitStub[op.kind == BK_LD_B   ? JIT_STUB_READ8
                             : op.kind == BK_LD_H ? JIT_STUB_READ16
                                                  : JIT_STUB_READ32],
                     1);
            }
            if(op.kind == BK_LD_B)
               w.Extsb(3, 3);
            else if(op.kind == BK_LD_H)
               w.Extsh(3, 3);
            Put(arg2, 3);
            const int32 extra = LoadExtra(lastopNow, word);
            if(extra < 0)
               SpendR9(path ? 0 : 1, next, LASTOP_LD);
            else
               Spend(extra + !path, next, LASTOP_LD, path);
            if(path)
               PendingExit(next, LASTOP_LD);
         }
         w.BTo(onward);
         Hot();
         lastopNow = LASTOP_LD;
         offset = next;
         continue;
      }
      case BK_ST_B: case BK_ST_H: case BK_ST_W:
      {
         const bool word = op.kind == BK_ST_W;
         const uint32 next = offset + 4;
         const unsigned clear = op.kind == BK_ST_H ? 1 : word ? 2 : 0;
         Address(arg2, op.imm);
         Get(5, arg1);
         w.Rlwinm(6, 4, 8, 29, 31);
         w.Cmpwi(6, 5);
         uint32 *slow = w.Bc(BO_FALSE, CR_EQ);
         w.Rlwinm(7, 4, 0, 16, 31 - clear);
         if(op.kind == BK_ST_B)
            w.Stbx(5, JR_RAM, 7);
         else if(op.kind == BK_ST_H)
            w.Sthbrx(5, JR_RAM, 7);
         else
            w.Stwbrx(5, JR_RAM, 7);
         uint32 *onward = NULL;
         for(int path = 0; path < 2; ++path)
         {
            if(path)
            {
               onward = w.at;
               Cold();
               w.Patch(slow, w.at);
               if(clear)
                  w.Rlwinm(4, 4, 0, 0, 31 - clear);
               if(op.kind == BK_ST_B)
                  w.Rlwinm(5, 5, 0, 24, 31);
               else if(op.kind == BK_ST_H)
                  w.Rlwinm(5, 5, 0, 16, 31);
               FirstCycle();
               w.BTo(JitStub[op.kind == BK_ST_B   ? JIT_STUB_WRITE8
                             : op.kind == BK_ST_H ? JIT_STUB_WRITE16
                                                  : JIT_STUB_WRITE32],
                     1);
            }
            const int32 extra = StoreExtra(lastopNow, word);
            if(extra < 0)
               SpendR9(path ? 0 : 1, next, LASTOP_ST);
            else
               Spend(extra + !path, next, LASTOP_ST, path);
            if(path)
               PendingExit(next, LASTOP_ST);
         }
         w.BTo(onward);
         Hot();
         lastopNow = LASTOP_ST;
         offset = next;
         continue;
      }
      case BK_MUL: case BK_MULU:
         Get(3, arg1);
         Get(4, arg2);
         w.Mullw(5, 4, 3);
         if(op.kind == BK_MUL)
            w.Mulhw(6, 4, 3);
         else
            w.Mulhwu(6, 4, 3);
         Put(30, 6);
         Put(arg2, 5);
         w.Cmpwi(5, 0);
         FlagsSZ();
         /* OV: the high word isn't just the low word's sign (unsigned: isn't 0) */
         if(op.kind == BK_MUL)
         {
            w.Srawi(7, 5, 31);
            w.Xor(7, 7, 6);
         }
         else
            w.Mr(7, 6);
         w.Cntlzw(7, 7);
         w.Rlwinm(7, 7, 27, 31, 31);
         w.Xori(7, 7, 1);
         w.Rlwimi(JR_PSW, 7, 2, 29, 29);
         lastopNow = -1;
         offset += 2;
         Spend(13, offset, lastopNow);
         continue;
      case BK_DIV: case BK_DIVU:
      {
         Get(3, arg1);
         Get(4, arg2);
         w.Cmpwi(3, 0);
         ExitBefore(w.Bc(BO_TRUE, CR_EQ), offset, lastopNow, /* (it raises the exception) */
                    lastopNow == kLastopUnknown ? JIT_STUB_INTERPRET_FIRST : JIT_STUB_INTERPRET);
         uint32 *normal = NULL, *normal2 = NULL, *done = NULL;
         if(op.kind == BK_DIV)
         {
            /* 0x80000000 / -1: OV, and r30 = 0 */
            w.Cmpwi(3, -1);
            normal = w.Bc(BO_FALSE, CR_EQ);
            w.Lis(9, (int32)0x8000);
            w.Cmpw(4, 9);
            normal2 = w.Bc(BO_FALSE, CR_EQ);
            w.Li(6, 0);
            Put(30, 6);
            Put(arg2, 9);
            w.Cmpwi(9, 0);
            FlagsSZ();
            w.Ori(JR_PSW, JR_PSW, PSW_OV);
            done = w.B();
            w.Patch(normal, w.at);
            w.Patch(normal2, w.at);
         }
         if(op.kind == BK_DIV)
            w.Divw(5, 4, 3);
         else
            w.Divwu(5, 4, 3);
         w.Mullw(6, 5, 3);
         w.Subf(6, 6, 4); /* (the remainder) */
         Put(30, 6);
         Put(arg2, 5);
         w.Cmpwi(5, 0);
         FlagsSZ();
         ClearOV();
         w.Patch(done, w.at);
         lastopNow = -1;
         offset += 2;
         Spend(op.kind == BK_DIV ? 38 : 36, offset, lastopNow);
         continue;
      }
      case BK_NOP:
         break;
      case BK_BCOND:
      {
         const unsigned cond = op.opcode & 0x0F;
         uint32 *notTaken = NULL;
         if(cond != COND_T)
         {
            TestCondition(cond & 7);
            notTaken = w.Bc(cond >= 8 ? BO_FALSE : BO_TRUE, CR_EQ);
         }
         BranchTo(pc + (sign_9(op.imm) & 0xFFFFFFFE), op.opcode, op.imm >= 0x1C0, 3); /* (back 64 bytes at most) */
         if(notTaken)
         {
            w.Patch(notTaken, w.at);
            if(op.imm >= 0x1C0) /* (as the interpreter: a loop left is looked at afresh) */
            {
               w.Li(10, -1);
               w.Stw(10, offIdleHead, JR_REGS);
            }
            BranchTo(pc + 2, op.opcode, false, 1);
         }
         ended = true;
         continue;
      }
      case BK_WAIT:
      {
         /* (as op_WAIT_BE and the rest: taken, the cycle count jumps to the next event, then 3 more) */
         const unsigned cond = op.opcode == 0x36 ? COND_Z : op.opcode == 0x37 ? COND_NZ : op.opcode == 0x64 ? COND_NS
                               : op.opcode == 0x65 ? COND_LT : op.opcode == 0x6C ? COND_C : COND_T;
         const uint32 target = pc + (sign_9(op.imm) & 0xFFFFFFFE);
         uint32 *notTaken = NULL;
         if(cond != COND_T)
         {
            TestCondition(cond & 7);
            notTaken = w.Bc(cond >= 8 ? BO_FALSE : BO_TRUE, CR_EQ);
         }
         w.Li(4, op.opcode);
         w.Stw(4, offLastop, JR_REGS);
         if(WaitBody(&FastMap[pc >> V810_FAST_MAP_SHIFT][pc], op.imm) >= 0)
         {
            w.BTo(JitStub[JIT_STUB_WAIT], 1);
            w.Word(pc);
            w.Word(op.imm);
         }
         else
         {
            w.Li(10, 0); /* (as WaitTarget for a loop it can't skip) */
            w.Stw(10, offWaitAt, JR_REGS);
         }
         w.Li(9, -3);
         w.Mtctr(9);
         uint32 *const skip = w.B();
         uint32 *const link = w.at;
         w.BTo(JitStub[JIT_STUB_BRANCH], 1);
         w.Word(target);
         w.Patch(skip, w.at);
         EventsAt(target, op.opcode, link);
         if(notTaken)
         {
            w.Patch(notTaken, w.at);
            w.Li(10, 0);
            w.Stw(10, offWaitAt, JR_REGS);
            BranchTo(pc + 2, op.opcode, false, 1);
         }
         ended = true;
         continue;
      }
      case BK_JR: case BK_JAL:
         if(op.kind == BK_JAL)
         {
            w.Imm32(3, pc + 4);
            w.Stw(3, 4 * 31, JR_REGS);
         }
         BranchTo(pc + (sign_26(op.imm) & 0xFFFFFFFE), op.opcode, false, 3);
         ended = true;
         continue;
      case BK_JMP:
      {
         Get(5, arg1);
         w.Rlwinm(5, 5, 0, 0, 30);
         w.Stw(5, offTarget, JR_REGS);
         w.Li(4, op.opcode);
         w.Stw(4, offLastop, JR_REGS);
         w.Mfctr(9);
         w.AddicDot(9, 9, -3);
         w.Mtctr(9);
         uint32 *const due = w.Bc(BO_FALSE, CR_GT);
         /* (straight on when the target's code is in JitLookup, else back to the C side, which finds it) */
         w.Rlwinm(6, 5, 2, 17, 28);
         w.Imm32(7, (uint32)(uintptr_t)JitLookup);
         w.Lwzx(8, 7, 6);
         w.Cmpw(8, 5);
         uint32 *const miss = w.Bc(BO_FALSE, CR_EQ);
         w.Addi(7, 7, 4);
         w.Lwzx(8, 7, 6);
         w.Mtlr(8);
         w.Blr();
         Cold();
         w.Patch(due, w.at);
         w.Patch(miss, w.at);
         w.Li(3, JIT_EXIT_BRANCH);
         w.BTo(JitStub[JIT_STUB_LEAVE]);
         Hot();
         ended = true;
         continue;
      }
      }
      /* (the ALU ops and NOP: one cycle, lastop the opcode) */
      lastopNow = op.opcode;
      offset += BlockOpLength(op.kind);
      Spend(1, offset, lastopNow);
      if(skip)
         for(Exit &e : exits)
            if(e.offset == offset && e.stub == JIT_STUB_EVENTS)
               e.flagsOf = &op, e.flagsLeft = skip;
   }
   skip = 0;
   if(!ended)
      ExitBefore(w.B(), offset, lastopNow, JIT_STUB_INTERPRET);
   Cold();
   for(const Exit &e : exits)
   {
      for(uint32 *from : e.from)
         w.Patch(from, w.at);
      if(e.flagsLeft)
      {
         skip = ~e.flagsLeft & (FLAG_SZ | FLAG_OV | FLAG_CY);
         FlagsOf(*e.flagsOf);
         skip = 0;
      }
      w.BTo(JitStub[e.stub], 1);
      w.Word(block->pc + e.offset);
      if(e.stub != JIT_STUB_INTERPRET_FIRST)
         w.Word((uint32)e.lastop);
      if(e.stub == JIT_STUB_EVENTS)
      {
         /* (none when no op of this block is there) */
         uint32 *resume = NULL;
         for(const auto &at : opAt)
            if(at.first == e.offset)
               resume = at.second;
         w.Word((uint32)(uintptr_t)resume);
         w.Word((uint32)(uintptr_t)resume);
      }
   }
   Hot();
   while((w.at - JitCode) & 3)
      w.Word(0);

   if(w.full || away.full)
   {
      /* (it didn't fit in what is left of its page: it goes in the next, or nowhere if it's too big for a page) */
      if(retried)
         return NULL;
      JitNextPage();
      return CompileBlock(block, true);
   }
   JitCodeUsed = w.at - JitCode;
   JitColdUsed = away.at - JitCode;
   DCFlushRange(flushFrom, (w.at - flushFrom) * 4);
   ICInvalidateRange(flushFrom, (w.at - flushFrom) * 4);
   DCFlushRange(coldFrom, (away.at - coldFrom) * 4);
   ICInvalidateRange(coldFrom, (away.at - coldFrom) * 4);
   return start;
}
