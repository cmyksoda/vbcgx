/****************************************************************************
 * WutMem1.h
 *
 * Keeps the core's MEM1 statics (vbgo_mem1.h) while another app, such as
 * the HOME Menu, has the foreground and MEM1 with it.
 ***************************************************************************/
#pragma once

#include <cstdint>

//!Registers the two callbacks below with ProcUI; call after ProcUI starts, from the core WHBGfxInit runs on.
void WutMem1KeepThroughForeground();

//!Copies every MEM1 block to MEM2 (ProcUI release callback).
uint32_t WutMem1OnRelease(void *context);

//!Takes the MEM1 blocks back at their addresses and copies them in (ProcUI acquire callback).
uint32_t WutMem1OnAcquire(void *context);
