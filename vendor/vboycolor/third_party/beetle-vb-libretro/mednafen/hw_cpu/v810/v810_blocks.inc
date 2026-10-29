/* The Wii's recompiler, part one (see V810::Block): ROM code decoded into blocks, and RunBlock, which the
 * interpreter loop calls to run them as Broadway code (v810_jit.inc). Included by v810_cpu.cpp. */

/* (libogc's; the core is built without its headers) */
extern "C" void DCFlushRange(void *start, uint32 length);
extern "C" void ICInvalidateRange(void *start, uint32 length);

/* How compiled code ends (v810_jit.inc): an interrupt is due before an op (it can go on there, at JitResumeAt), the
 * event handler it ran ended the frame (likewise), the interpreter must run the next op, or at a branch to
 * JitTarget that isn't linked yet. */
enum { JIT_EXIT_STOPPED = 0, JIT_EXIT_BRANCH = 1, JIT_EXIT_INTERPRET = 2, JIT_EXIT_FRAME_END = 3 };
/* JitStub[JIT_STUB_ENTER](P_REG, &timestamp, code) runs compiled code; the exit leaves the next PC in JitTarget. */
enum { JIT_STUB_ENTER, JIT_STUB_LEAVE, JIT_STUB_LEAVE_TS, JIT_STUB_EVENTS, JIT_STUB_INTERPRET, JIT_STUB_INTERPRET_FIRST,
       JIT_STUB_BRANCH, JIT_STUB_IDLE, JIT_STUB_READ8, JIT_STUB_READ16, JIT_STUB_READ32, JIT_STUB_WRITE8,
       JIT_STUB_WRITE16, JIT_STUB_WRITE32, JIT_STUB_WAIT, JIT_STUBS };
/* The code buffer is pages: hot code, then the cold code that goes with it (slow paths, exits), so what runs sits
 * packed together. A page is as far as a conditional branch reaches. */
enum { JIT_PAGE_WORDS = 32 * 1024 / 4, JIT_HOT_WORDS = 18 * 1024 / 4 };

namespace
{
enum BlockKind : uint8
{
   BK_NONE,
   /* reg-reg (r1 = arg1, r2 = arg2) */
   BK_MOV, BK_ADD, BK_SUB, BK_CMP, BK_SHL, BK_SHR, BK_SAR, BK_MUL, BK_DIV, BK_MULU, BK_DIVU, BK_OR, BK_AND,
   BK_XOR, BK_NOT,
   /* imm5 (r1 = arg1, r2 = arg2) */
   BK_MOV_I, BK_ADD_I, BK_SETF, BK_CMP_I, BK_SHL_I, BK_SHR_I, BK_SAR_I, BK_STSR,
   /* 16-bit immediate (r1 = source, r2 = destination, imm) */
   BK_MOVEA, BK_ADDI, BK_ORI, BK_ANDI, BK_XORI, BK_MOVHI,
   /* loads (r1 = base, r2 = destination, imm = displacement) */
   BK_LD_B, BK_LD_H, BK_LD_W,
   /* stores (r1 = data, r2 = base, imm = displacement) */
   BK_ST_B, BK_ST_H, BK_ST_W,
   BK_NOP,
   /* the ops that end a block (WAIT: Bcond that waits for the next event, from the idle patches) */
   BK_BCOND, BK_WAIT, BK_JMP, BK_JR, BK_JAL
};

/* By opcode (an instruction's top 7 bits), as v810_op_table.inc orders them. */
const uint8 kBlockKinds[128] = {
   BK_MOV, BK_MOV, BK_ADD, BK_ADD, BK_SUB, BK_SUB, BK_CMP, BK_CMP,
   BK_SHL, BK_SHL, BK_SHR, BK_SHR, BK_JMP, BK_JMP, BK_SAR, BK_SAR,
   BK_MUL, BK_MUL, BK_DIV, BK_DIV, BK_MULU, BK_MULU, BK_DIVU, BK_DIVU,
   BK_OR, BK_OR, BK_AND, BK_AND, BK_XOR, BK_XOR, BK_NOT, BK_NOT,
   BK_MOV_I, BK_MOV_I, BK_ADD_I, BK_ADD_I, BK_SETF, BK_SETF, BK_CMP_I, BK_CMP_I,
   BK_SHL_I, BK_SHL_I, BK_SHR_I, BK_SHR_I, BK_NONE, BK_NONE, BK_SAR_I, BK_SAR_I,
   BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_WAIT, BK_WAIT,
   BK_NONE, BK_NONE, BK_STSR, BK_STSR, BK_NONE, BK_NONE, BK_NONE, BK_NONE,
   BK_BCOND, BK_BCOND, BK_BCOND, BK_BCOND, BK_BCOND, BK_BCOND, BK_BCOND, BK_BCOND,
   BK_BCOND, BK_BCOND, BK_BCOND, BK_BCOND, BK_BCOND, BK_NOP, BK_BCOND, BK_BCOND,
   BK_MOVEA, BK_MOVEA, BK_ADDI, BK_ADDI, BK_JR, BK_JR, BK_JAL, BK_JAL,
   BK_ORI, BK_ORI, BK_ANDI, BK_ANDI, BK_XORI, BK_XORI, BK_MOVHI, BK_MOVHI,
   BK_LD_B, BK_LD_B, BK_LD_H, BK_LD_H, BK_WAIT, BK_WAIT, BK_LD_W, BK_LD_W,
   BK_ST_B, BK_ST_B, BK_ST_H, BK_ST_H, BK_WAIT, BK_WAIT, BK_ST_W, BK_ST_W,
   BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE,
   BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE,
};

enum { PAGE_FREE, PAGE_WRITING, PAGE_LIVE, PAGE_PROBATION };
enum { JIT_BYTES = 2 * 1024 * 1024 };
/* (in the DOL's own memory: MEM1, the faster to fetch code from) */
uint32 JitCode[JIT_BYTES / 4] __attribute__((aligned(32)));
static_assert(JIT_BYTES % (JIT_PAGE_WORDS * 4) == 0 && JIT_HOT_WORDS < JIT_PAGE_WORDS, "JitCode pages");

inline uint32 BranchWord(const uint32 *at, const void *to, unsigned link)
{
   return 18u << 26 | ((uint32)((const uint8 *)to - (const uint8 *)at) & 0x03FFFFFC) | link;
}

/* Compiled code by ROM address, for register jumps (v810_jit.inc), filled as RunBlock dispatches blocks. */
struct JitLookupEntry
{
   uint32 pc;
   void *code;
};
enum { JIT_LOOKUP_SIZE = 4096 };
JitLookupEntry JitLookup[JIT_LOOKUP_SIZE];

inline unsigned BlockOpLength(unsigned kind)
{
   return (kind >= BK_MOVEA && kind <= BK_ST_W) || kind == BK_JR || kind == BK_JAL ? 4 : 2;
}
} // namespace

void V810::FlushBlocks(void)
{
   static_assert(JIT_BYTES == JIT_PAGES * JIT_PAGE_WORDS * 4, "JIT_PAGES");
   static_assert(JIT_PAGES <= 256 && JIT_LINK_NOTES < 65536, "the sizes of JitAge and JitLinkNote");
   memset(BlockSlot, 0, sizeof(BlockSlot));
   BlockPoolUsed = 0;
   BlockFreed = NULL;
   JitNotesUsed = JitNotesFreed = 0;
   JitNotesStale = false;
   memset(JitPages, 0, sizeof(JitPages));
   JitAged = JitFreePages = 0;
   for(unsigned page = JIT_PAGES; page-- > 1;)
      JitFreePage[JitFreePages++] = page;
   JitWritePage = 0;
   JitPages[0].state = PAGE_WRITING;
   JitStubEnd = 0;
   JitCodeUsed = 0;
   JitColdUsed = JIT_HOT_WORDS;
   JitLinkSite = NULL;
   JitResumeAt = NULL;
   memset(JitLookup, 0, sizeof(JitLookup));
}

V810::Block *V810::FindBlock(uint32 pc)
{
   for(Block *found = BlockSlot[(pc >> 1) & (BLOCK_SLOTS - 1)]; found; found = found->next)
      if(found->pc == pc && found->code == PC_ptr)
         return found;
   return NULL;
}

const V810::Decoded *V810::DecodeBlock(uint32 pc)
{
   Decoded *const block = &BlockDecoded;
   const uint8 *code = PC_ptr;
   unsigned n = 0;
   while(n < BLOCK_MAX_OPS)
   {
      const uint32 tmpop = LoadU16_LE((const uint16 *)code);
      const unsigned opcode = tmpop >> 9, kind = kBlockKinds[opcode];
      const uint32 op_pc = pc + (code - PC_ptr);
      if(kind == BK_NONE)
         break;
      /* (as op_WAIT_BE and the rest check, else they're invalid ops) */
      if(kind == BK_WAIT && (!idlePatches || ((op_pc >> 24) & 7) != 7
                             || code != &FastMap[op_pc >> V810_FAST_MAP_SHIFT][op_pc]))
         break;
      BlockOp &op = block->ops[n++];
      op.kind = kind;
      op.opcode = opcode;
      op.r1 = op.r2 = 0;
      op.imm = 0;
      if(kind <= BK_STSR)
         op.r1 = tmpop & 0x1F, op.r2 = (tmpop >> 5) & 0x1F;
      else if(kind <= BK_LD_W)
         op.r1 = tmpop & 0x1F, op.r2 = (tmpop >> 5) & 0x1F, op.imm = LoadU16_LE((const uint16 *)(code + 2));
      else if(kind <= BK_ST_W)
         op.r1 = (tmpop >> 5) & 0x1F, op.r2 = tmpop & 0x1F, op.imm = LoadU16_LE((const uint16 *)(code + 2));
      else if(kind == BK_BCOND || kind == BK_WAIT)
         op.imm = tmpop & 0x1FE;
      else if(kind == BK_JMP)
         op.r1 = tmpop & 0x1F, op.r2 = (tmpop >> 5) & 0x1F;
      else if(kind == BK_JR || kind == BK_JAL)
         op.imm = ((tmpop & 0x3FF) << 16) | LoadU16_LE((const uint16 *)(code + 2));
      code += BlockOpLength(kind);
      if(kind >= BK_BCOND)
         break;
   }
   if(!n)
      return NULL;
   block->pc = pc;
   block->code = PC_ptr;
   block->count = n;
   return block;
}

/* The interpreter's idle check after a short branch back, for compiled code. */
v810_timestamp_t V810::JitIdleLoop(V810 *cpu, v810_timestamp_t timestamp, uint32 target)
{
   cpu->SetPC(target);
   return cpu->IdleLoop(timestamp, cpu->PC_ptr);
}

/* WaitTarget for a compiled WAIT (it moves next_event_ts itself). */
void V810::JitWaitTarget(V810 *cpu, v810_timestamp_t timestamp, uint32 pc, uint32 arg1)
{
   cpu->SetPC(pc);
   cpu->WaitTarget(timestamp, cpu->PC_ptr, arg1);
}

/* Runs compiled code from PC, if there is any (the interpreter loop checked that no event is due and no interrupt
 * pending). 1: it ran, 0: the interpreter must run the next op, -1: it ran the event handler and the frame ended. */
int V810::RunBlock(v810_timestamp_t &timestamp)
{
   const uint32 block_pc = PC_ptr - PC_base;
   if(((block_pc >> 24) & 7) != 7)
      return 0;
   void *code;
   if(JitResumeAt && block_pc == JitResumePc && lastop == JitResumeLastop)
      code = JitResumeAt; /* (its code runs the same from any way there, with that lastop) */
   else
   {
      /* (the interpreter's last op jumped, or an interrupt or exception was taken) */
      if(!BlockEntryOk && PC_ptr != BlockExpect)
         BlockEntryOk = true;
      Block *block = FindBlock(block_pc);
      if(!block)
      {
         const Decoded *const decoded = BlockEntryOk && JitPools() ? DecodeBlock(block_pc) : NULL;
         if(decoded)
         {
            void *const jit = CompileBlock(decoded);
            block = AddBlock(decoded, jit);
         }
         if(!block)
         {
            BlockExpect = PC_ptr + ((LoadU16_LE((const uint16 *)PC_ptr) >> 10) >= 0x28 ? 4 : 2);
            return 0;
         }
      }
      else if(block->jit)
         JitTouch(block);
      if(!block->jit)
      {
         BlockEntryOk = true; /* (whatever the interpreter's op does, it goes the same way every time) */
         return 0;
      }
      code = block->jit;
      if(block->code == &FastMap[block_pc >> V810_FAST_MAP_SHIFT][block_pc]) /* (as a jump there finds it) */
         JitLookup[(block_pc >> 1) & (JIT_LOOKUP_SIZE - 1)] = JitLookupEntry{block_pc, code};
      if(JitLinkSite && JitLinkTarget == block_pc)
         JitLink(JitLinkSite, code); /* (the branch that came here now jumps straight into this block's code) */
   }
   JitLinkSite = NULL;
   const uint32 exit =
       ((uint32 (*)(uint32 *, v810_timestamp_t *, void *))JitStub[JIT_STUB_ENTER])(P_REG, &timestamp, code);
   SetPC(JitTarget);
   const bool stopped = exit == JIT_EXIT_STOPPED || exit == JIT_EXIT_FRAME_END;
   BlockEntryOk = !stopped || !JitResumeAt; /* (stopped with nowhere to go on: the block ended there) */
   BlockExpect = PC_ptr;
   if(stopped)
   {
      JitResumePc = JitTarget;
      JitResumeLastop = lastop;
      return exit == JIT_EXIT_FRAME_END ? -1 : 1;
   }
   if(exit == JIT_EXIT_INTERPRET)
      return 0; /* (no event is due yet) */
   JitLinkTarget = JitTarget;
   return 1;
}
