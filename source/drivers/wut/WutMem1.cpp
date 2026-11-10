/****************************************************************************
 * WutMem1.cpp
 *
 * MEM1 for the core's hottest statics (vbgo_mem1.h), taken before WHBGfxInit
 * claims the rest for its buffers, and kept through the HOME Menu.
 ***************************************************************************/
#include <coreinit/debug.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memfrmheap.h>
#include <coreinit/memheap.h>
#include <cstring>
#include <proc_ui/procui.h>

#include "WutMem1.h"
#include "emu/vbgo_mem1.h"

namespace
{
	// (what WHB's buffers take at worst: 1080p TV color and depth, the GamePad's, with room to spare)
	constexpr uint32_t kForGraphics = 22u << 20;
	// Under WHB's 100, so ProcUI runs our acquire before WHB's takes the rest of MEM1
	constexpr uint32_t kCallbackPriority = 50;
	constexpr unsigned kMaxBlocks = 64;

	// A block in MEM1 and its copy in MEM2, which holds it while another app has MEM1
	struct Block { void *at; void *copy; uint32_t bytes; };
	Block blocks[kMaxBlocks];
	unsigned blockCount;
	bool released;
}

void *vbgo_mem1(size_t bytes)
{
	// (the copy is also where the block lives when MEM1 has no room)
	void *const copy = MEMAllocFromDefaultHeapEx(bytes, 64);
	const MEMHeapHandle mem1 = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
	void *at = nullptr;
	if (copy && mem1 && blockCount < kMaxBlocks && MEMGetAllocatableSizeForFrmHeapEx(mem1, 64) >= bytes + kForGraphics)
		at = MEMAllocFromFrmHeapEx(mem1, bytes, 64);
	if (at)
		blocks[blockCount++] = { at, copy, static_cast<uint32_t>(bytes) };
	else
		at = copy;
	if (at)
		memset(at, 0, bytes);
	return at;
}

uint32_t WutMem1OnRelease(void *)
{
	for (unsigned i = 0; i < blockCount; ++i)
		memcpy(blocks[i].copy, blocks[i].at, blocks[i].bytes);
	released = true;
	return 0;
}

uint32_t WutMem1OnAcquire(void *)
{
	if (!released)
		return 0;
	released = false;

	// The blocks were MEM1's first: freed and taken again in order, they land where they were, whether or not the
	// system rebuilt MEM1's heap meanwhile
	const MEMHeapHandle mem1 = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
	MEMFreeToFrmHeap(mem1, MEM_FRM_HEAP_FREE_HEAD);
	for (unsigned i = 0; i < blockCount; ++i)
	{
		const Block &block = blocks[i];
		if (MEMAllocFromFrmHeapEx(mem1, block.bytes, 64) != block.at)
			OSFatal("VBoy Color GX could not take back its MEM1 after the HOME Menu.");
		memcpy(block.at, block.copy, block.bytes);
	}
	return 0;
}

void WutMem1KeepThroughForeground()
{
	if (!blockCount)
		return;
	ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, WutMem1OnRelease, nullptr, kCallbackPriority);
	ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, WutMem1OnAcquire, nullptr, kCallbackPriority);
}
