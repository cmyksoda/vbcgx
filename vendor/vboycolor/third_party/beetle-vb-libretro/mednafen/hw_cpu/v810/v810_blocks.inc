/* The Wii's recompiler, part one (see V810::Block): ROM code decoded into blocks, and RunBlock, which the
 * interpreter loop calls to run them as Broadway code (v810_jit.inc). Included by v810_cpu.cpp. */

/* (libogc's; the core is built without its headers) */
extern "C" void DCFlushRange(void *start, uint32 length);
extern "C" void ICInvalidateRange(void *start, uint32 length);

/* How compiled code ends (v810_jit.inc): an event or interrupt is due before an op (it can go on there, at
 * JitResumeAt), the interpreter must run the next op, or at a branch to JitTarget (BACK: one back that the idle
 * check must see). */
enum { JIT_EXIT_STOPPED = 0, JIT_EXIT_BRANCH = 1, JIT_EXIT_BRANCH_BACK = 2, JIT_EXIT_INTERPRET = 3 };
/* JitStub[JIT_STUB_ENTER](P_REG, &timestamp, code) runs compiled code; the exit leaves the next PC in JitTarget. */
enum { JIT_STUB_ENTER, JIT_STUB_LEAVE, JIT_STUB_STOPPED, JIT_STUB_INTERPRET, JIT_STUB_INTERPRET_FIRST, JIT_STUB_BRANCH,
       JIT_STUB_BRANCH_DUE, JIT_STUB_BRANCH_BACK, JIT_STUB_READ8, JIT_STUB_READ16, JIT_STUB_READ32, JIT_STUB_WRITE8,
       JIT_STUB_WRITE16, JIT_STUB_WRITE32, JIT_STUBS };

namespace
{
enum BlockKind : uint8
{
   BK_NONE,
   /* reg-reg (r1 = arg1, r2 = arg2) */
   BK_MOV, BK_ADD, BK_SUB, BK_CMP, BK_SHL, BK_SHR, BK_SAR, BK_MUL, BK_DIV, BK_MULU, BK_DIVU, BK_OR, BK_AND, BK_XOR, BK_NOT,
   /* imm5 (r1 = arg1, r2 = arg2) */
   BK_MOV_I, BK_ADD_I, BK_SETF, BK_CMP_I, BK_SHL_I, BK_SHR_I, BK_SAR_I, BK_STSR,
   /* 16-bit immediate (r1 = source, r2 = destination, imm) */
   BK_MOVEA, BK_ADDI, BK_ORI, BK_ANDI, BK_XORI, BK_MOVHI,
   /* loads (r1 = base, r2 = destination, imm = displacement) */
   BK_LD_B, BK_LD_H, BK_LD_W,
   /* stores (r1 = data, r2 = base, imm = displacement) */
   BK_ST_B, BK_ST_H, BK_ST_W,
   BK_NOP,
   /* the ops that end a block */
   BK_BCOND, BK_JMP, BK_JR, BK_JAL
};

/* By opcode (an instruction's top 7 bits), as v810_op_table.inc orders them. */
const uint8 kBlockKinds[128] = {
   BK_MOV, BK_MOV, BK_ADD, BK_ADD, BK_SUB, BK_SUB, BK_CMP, BK_CMP,
   BK_SHL, BK_SHL, BK_SHR, BK_SHR, BK_JMP, BK_JMP, BK_SAR, BK_SAR,
   BK_MUL, BK_MUL, BK_DIV, BK_DIV, BK_MULU, BK_MULU, BK_DIVU, BK_DIVU,
   BK_OR, BK_OR, BK_AND, BK_AND, BK_XOR, BK_XOR, BK_NOT, BK_NOT,
   BK_MOV_I, BK_MOV_I, BK_ADD_I, BK_ADD_I, BK_SETF, BK_SETF, BK_CMP_I, BK_CMP_I,
   BK_SHL_I, BK_SHL_I, BK_SHR_I, BK_SHR_I, BK_NONE, BK_NONE, BK_SAR_I, BK_SAR_I,
   BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE,
   BK_NONE, BK_NONE, BK_STSR, BK_STSR, BK_NONE, BK_NONE, BK_NONE, BK_NONE,
   BK_BCOND, BK_BCOND, BK_BCOND, BK_BCOND, BK_BCOND, BK_BCOND, BK_BCOND, BK_BCOND,
   BK_BCOND, BK_BCOND, BK_BCOND, BK_BCOND, BK_BCOND, BK_NOP, BK_BCOND, BK_BCOND,
   BK_MOVEA, BK_MOVEA, BK_ADDI, BK_ADDI, BK_JR, BK_JR, BK_JAL, BK_JAL,
   BK_ORI, BK_ORI, BK_ANDI, BK_ANDI, BK_XORI, BK_XORI, BK_MOVHI, BK_MOVHI,
   BK_LD_B, BK_LD_B, BK_LD_H, BK_LD_H, BK_NONE, BK_NONE, BK_LD_W, BK_LD_W,
   BK_ST_B, BK_ST_B, BK_ST_H, BK_ST_H, BK_NONE, BK_NONE, BK_ST_W, BK_ST_W,
   BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE,
   BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE, BK_NONE,
};

inline unsigned BlockOpLength(unsigned kind)
{
   return (kind >= BK_MOVEA && kind <= BK_ST_W) || kind == BK_JR || kind == BK_JAL ? 4 : 2;
}
} // namespace

void V810::FlushBlocks(void)
{
   memset(BlockSlot, 0, sizeof(BlockSlot));
   BlockArenaUsed = 0;
   JitCodeUsed = 0;
   JitLinkSite = NULL;
   JitResumeAt = NULL;
}

V810::Block *V810::FindBlock(uint32 pc, bool decode)
{
   Block **slot = &BlockSlot[(pc >> 1) & (BLOCK_SLOTS - 1)];
   for(Block *found = *slot; found; found = found->next)
      if(found->pc == pc && found->code == PC_ptr)
         return found;
   if(!decode)
      return NULL;
   if(!BlockArena && !(BlockArena = (uint8 *)malloc(BLOCK_ARENA)))
      return NULL;
   const size_t most = sizeof(Block) + (BLOCK_MAX_OPS - 1) * sizeof(BlockOp);
   if(BlockArenaUsed + most > BLOCK_ARENA)
   {
      FlushBlocks();
      slot = &BlockSlot[(pc >> 1) & (BLOCK_SLOTS - 1)];
   }
   Block *block = (Block *)(BlockArena + BlockArenaUsed);
   const uint8 *code = PC_ptr;
   unsigned n = 0;
   while(n < BLOCK_MAX_OPS)
   {
      const uint32 tmpop = LoadU16_LE((const uint16 *)code);
      const unsigned opcode = tmpop >> 9, kind = kBlockKinds[opcode];
      if(kind == BK_NONE)
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
      else if(kind == BK_BCOND)
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
   block->jit = NULL;
   block->jitTried = 0;
   block->next = *slot;
   BlockArenaUsed += (sizeof(Block) + (n - 1) * sizeof(BlockOp) + 7) & ~(size_t)7;
   *slot = block;
   return block;
}

/* Runs compiled code from PC, if there is any, while no event is due and no interrupt pending (the interpreter
 * loop checked both before calling). False if the interpreter must run the next op. */
bool V810::RunBlock(v810_timestamp_t &timestamp)
{
   const uint32 block_pc = PC_ptr - PC_base;
   if(((block_pc >> 24) & 7) != 7)
      return false;
   void *code;
   if(JitResumeAt && block_pc == JitResumePc && lastop == JitResumeLastop)
      code = JitResumeAt; /* (its code runs the same from any way there, with that lastop) */
   else
   {
      /* (the interpreter's last op jumped, or an interrupt or exception was taken) */
      if(!BlockEntryOk && PC_ptr != BlockExpect)
         BlockEntryOk = true;
      Block *const block = FindBlock(block_pc, BlockEntryOk);
      if(!block)
      {
         BlockExpect = PC_ptr + ((LoadU16_LE((const uint16 *)PC_ptr) >> 10) >= 0x28 ? 4 : 2);
         return false;
      }
      if(!block->jitTried)
      {
         block->jitTried = 1;
         block->jit = CompileBlock(block);
      }
      if(!block->jit)
      {
         BlockEntryOk = true; /* (whatever the interpreter's op does, it goes the same way every time) */
         return false;
      }
      code = block->jit;
      if(JitLinkSite && JitLinkTarget == block_pc)
      {
         /* (the branch that came here now jumps straight into this block's code) */
         *JitLinkSite = 0x48000000 | ((uint32)((uint8 *)code - (uint8 *)JitLinkSite) & 0x03FFFFFC);
         DCFlushRange(JitLinkSite, 4);
         ICInvalidateRange(JitLinkSite, 4);
      }
   }
   JitLinkSite = NULL;
   const uint32 exit = ((uint32 (*)(uint32 *, v810_timestamp_t *, void *))JitStub[JIT_STUB_ENTER])(P_REG, &timestamp, code);
   SetPC(JitTarget);
   BlockEntryOk = exit != JIT_EXIT_STOPPED || !JitResumeAt; /* (stopped with nowhere to go on: the block ended there) */
   BlockExpect = PC_ptr;
   if(exit == JIT_EXIT_STOPPED)
   {
      JitResumePc = JitTarget;
      JitResumeLastop = lastop;
   }
   else if(exit == JIT_EXIT_INTERPRET)
      return false; /* (no event is due yet) */
   else
   {
      JitLinkTarget = JitTarget;
      if(exit == JIT_EXIT_BRANCH_BACK)
      {
         if(IdleQuiet)
            IdleQuiet--;
         else
            timestamp = IdleLoop(timestamp, PC_ptr);
      }
   }
   return true;
}
