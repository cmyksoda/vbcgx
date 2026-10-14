/****************************************************************************
 * FCE Ultra GX
 *
 * Daryl Borth 2008-2026
 *
 * fceusupport.h
 *
 * FCEU Support Functions
 ****************************************************************************/
#pragma once
#include <gccore.h>
#include <libretro.h>
extern bool romLoaded;
extern unsigned char *nesrom;
void InitVBCore();
void RunVBFrame();
void PowerNES();
void CloseVBGame();
void SetVBInput(uint32_t buttons);
void VBSetGeometry(unsigned width, unsigned height, float aspect);
extern unsigned vbWidth, vbHeight;
extern float vbAspect;
void SetPalette();
extern "C" bool vboycolor_has_pack(void);
void SetVBAudioRate();
