/* The Wii's recompiler, part two: where compiled blocks live. Pages go oldest first, as in CLOCK: the oldest few are on
 * probation (no jumps in from other pages), so that RunBlock sees any use. Included by v810_cpu.cpp. */

namespace
{
enum { JIT_PROBATION = 2 };
inline unsigned JitPageOf(const void *code) { return ((const uint32 *)code - JitCode) / JIT_PAGE_WORDS; }
inline bool InJitPage(const void *code, unsigned page) { return code && JitPageOf(code) == page; }
} // namespace

bool V810::JitPools(void)
{
   if(!BlockPool)
      BlockPool = (Block *)malloc(BLOCK_POOL * sizeof(Block));
   if(!JitNotes)
      JitNotes = (JitLinkNote *)malloc(JIT_LINK_NOTES * sizeof(JitLinkNote));
   return BlockPool && JitNotes;
}

/* (jit is the page being written's, or NULL when the block couldn't be compiled) */
V810::Block *V810::AddBlock(const Decoded *decoded, void *jit)
{
   while(!BlockFreed && BlockPoolUsed == BLOCK_POOL)
   {
      if(!JitEvictOldest())
         return NULL;
      JitKeepProbation();
   }
   Block *block;
   if(BlockFreed)
      block = BlockFreed, BlockFreed = block->next;
   else
      block = &BlockPool[BlockPoolUsed++];
   Block **const slot = &BlockSlot[(decoded->pc >> 1) & (BLOCK_SLOTS - 1)];
   block->pc = decoded->pc;
   block->code = decoded->code;
   block->jit = jit;
   block->next = *slot;
   *slot = block;
   JitPage &page = JitPages[JitWritePage];
   block->pageNext = page.blocks;
   page.blocks = block;
   return block;
}

/* A dispatch into a page on probation is a use: the page rejoins the young end. */
void V810::JitTouch(Block *block)
{
   const unsigned page = JitPageOf(block->jit);
   if(JitPages[page].state != PAGE_PROBATION)
      return;
   unsigned at = 0;
   while(JitAge[at] != page)
      ++at;
   memmove(&JitAge[at], &JitAge[at + 1], JitAged - at - 1);
   JitAge[JitAged - 1] = page;
   JitPages[page].state = PAGE_LIVE;
   JitKeepProbation();
}

/* A branch into another page is noted so that probation can undo it; with no note free it stays unlinked. */
void V810::JitLink(uint32 *site, void *code)
{
   const unsigned from = JitPageOf(site), to = JitPageOf(code);
   if(from != to && !JitNoteLink(site, to))
      return;
   *site = BranchWord(site, code, 0);
   DCFlushRange(site, 4);
   ICInvalidateRange(site, 4);
}

bool V810::JitNoteLink(uint32 *site, unsigned target)
{
   if(!JitNotesFreed && JitNotesUsed == JIT_LINK_NOTES && JitNotesStale)
      JitSweepNotes();
   size_t n;
   if(JitNotesFreed)
      n = JitNotesFreed, JitNotesFreed = JitNotes[n - 1].next;
   else if(JitNotesUsed < JIT_LINK_NOTES)
      n = ++JitNotesUsed;
   else
      return false;
   const unsigned word = site - JitCode;
   JitNotes[n - 1] = JitLinkNote{word, JitPages[word / JIT_PAGE_WORDS].gen, JitPages[target].notes};
   JitPages[target].notes = n;
   return true;
}

/* A note of a branch in a page dropped since is stale: those words are other code by now. */
void V810::JitSweepNotes(void)
{
   JitNotesStale = false;
   for(JitPage &page : JitPages)
      for(uint16 *at = &page.notes; *at;)
      {
         JitLinkNote &note = JitNotes[*at - 1];
         if(note.gen == JitPages[note.site / JIT_PAGE_WORDS].gen)
            at = &note.next;
         else
         {
            const uint16 n = *at;
            *at = note.next;
            note.next = JitNotesFreed;
            JitNotesFreed = n;
         }
      }
}

void V810::JitNextPage(void)
{
   JitPages[JitWritePage].state = PAGE_LIVE;
   JitAge[JitAged++] = JitWritePage;
   if(!JitFreePages)
      JitEvictOldest();
   const unsigned page = JitFreePage[--JitFreePages];
   JitPages[page].state = PAGE_WRITING;
   JitWritePage = page;
   JitCodeUsed = page ? page * JIT_PAGE_WORDS : JitStubEnd;
   JitColdUsed = page * JIT_PAGE_WORDS + JIT_HOT_WORDS;
   JitKeepProbation();
}

bool V810::JitEvictOldest(void)
{
   if(!JitAged)
      return false;
   const unsigned page = JitAge[0];
   if(JitPages[page].state != PAGE_PROBATION)
      JitProbate(page);
   memmove(&JitAge[0], &JitAge[1], --JitAged);
   JitEvict(page);
   return true;
}

/* With few pages free the oldest are on probation, so one is ready to go and any use of it is seen. The youngest never is:
 * JitTouch has just put the page RunBlock is about to run there. */
void V810::JitKeepProbation(void)
{
   if(JitFreePages >= JIT_PROBATION)
      return;
   for(unsigned at = 0; at < JIT_PROBATION && at + 1 < JitAged; ++at)
      if(JitPages[JitAge[at]].state != PAGE_PROBATION)
         JitProbate(JitAge[at]);
}

/* Afterwards RunBlock is the only way into the page: no branch from another page, no register jump (JitLookup). */
void V810::JitProbate(unsigned page)
{
   JitPage &probated = JitPages[page];
   probated.state = PAGE_PROBATION;
   for(size_t n = probated.notes; n;)
   {
      JitLinkNote &note = JitNotes[n - 1];
      const size_t after = note.next;
      if(note.gen == JitPages[note.site / JIT_PAGE_WORDS].gen)
      {
         uint32 *const site = JitCode + note.site;
         *site = BranchWord(site, JitStub[JIT_STUB_BRANCH], 1);
         DCFlushRange(site, 4);
         ICInvalidateRange(site, 4);
      }
      note.next = JitNotesFreed;
      JitNotesFreed = n;
      n = after;
   }
   probated.notes = 0;
   for(const Block *block = probated.blocks; block; block = block->pageNext)
   {
      JitLookupEntry &entry = JitLookup[(block->pc >> 1) & (JIT_LOOKUP_SIZE - 1)];
      if(block->jit && entry.code == block->jit)
         entry = JitLookupEntry{0, NULL};
   }
   JitForgetInto(page);
}

void V810::JitEvict(unsigned page)
{
   JitPage &evicted = JitPages[page];
   for(Block *block = evicted.blocks, *after; block; block = after)
   {
      after = block->pageNext;
      Block **at = &BlockSlot[(block->pc >> 1) & (BLOCK_SLOTS - 1)];
      while(*at != block)
         at = &(*at)->next;
      *at = block->next;
      block->next = BlockFreed;
      BlockFreed = block;
   }
   evicted.blocks = NULL;
   ++evicted.gen; /* (orphans the notes of the branches it held, in other pages) */
   JitNotesStale = true;
   evicted.state = PAGE_FREE;
   JitForgetInto(page);
   JitFreePage[JitFreePages++] = page;
}

/* (a stopped block's way on, and the branch waiting to be linked, may be in it) */
void V810::JitForgetInto(unsigned page)
{
   if(InJitPage(JitResumeAt, page))
      JitResumeAt = NULL;
   if(InJitPage(JitLinkSite, page))
      JitLinkSite = NULL;
}
