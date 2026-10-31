/****************************************************************************
 * FCE Ultra GX
 *
 * Daryl Borth 2008-2026
 *
 * fceugx.cpp
 *
 * This file controls overall program flow. Most things start and end here!
 ****************************************************************************/
#include <malloc.h>
#include "fceugx.h"
#include "fceusupport.h"
#include "fceuram.h"
#include "menu.h"
#include "preferences.h"
#include "fileop.h"
#include "filebrowser.h"
#include "videosupport.h"
#include "pad.h"
#include "filelist.h"
#include "libgui/Gui.h"
#include "drivers/Platform.h"
#include "drivers/Thread.h"
#ifdef __WIIU__
#include "drivers/Logger.h"
#include "drivers/wut/WutPlatform.h"
static WutPlatform platformInstance;
bool vbgo_jit_attach(void);
#else
#include "drivers/ogc/wii/WiiPlatform.h"
#include "drivers/ogc/wii/mem2.h"
static WiiPlatform platformInstance;
#endif
Platform *platform = &platformInstance;
AppRequest appRequest = AppRequest::NONE;
char appPath[MAXPATHLEN] = {};
unsigned char *nesrom = nullptr;
static bool autoboot = false;
static void SaveAppData()
{
    SavePrefsAndWait();
    if (romLoaded) SaveRAMAuto(SILENT);
}
void ExitApp()
{
    WaitForBackgroundTasks(15000);
    SaveAppData();
    CloseVBGame();
    retro_deinit();
    Thread::JoinAll();
    platform->requestExit(EmuSettings.exitAction, autoboot);
}
int main(int argc, char **argv)
{
    PlatformConfig config;
    config.canvasWidth = 640;
    config.canvasHeight = 480;
#ifdef __WIIU__
    // (data/images_hd: drawn for 1920x1080, which is 3x the 640x480 menu across and 2.25x down)
    config.assetScaleX = 3.0f;
    config.assetScaleY = 2.25f;
#endif
    platform->init(config);
    platform->setSaveHandler(SaveAppData);
#ifdef __WIIU__
    // (this thread runs the game, and the recompiler's code area belongs to the core it claims it from)
    if (!vbgo_jit_attach())
        LOG_WARN("[JIT] no codegen area: every game runs in the interpreter");
#endif
    InitFileOpThreads();
    MountAllFAT();
    GuiImageData::setDecodeScratch(malloc(IMAGE_DECODE_SCRATCH_SIZE), IMAGE_DECODE_SCRATCH_SIZE);
    fontSystem = new GuiTextRenderer(font_ttf, font_ttf_size,
        platform->getVideo()->getGlyphRenderer(), platform->getVideo()->getUIScale());
    textTranslator = new GuiTextTranslator();
    textTranslator->loadLanguage(en_lang, en_lang_size);
    DefaultSettings();
    ApplySettings();
    platform->getVideo()->startMenuVideo();
    if (argc && argv[0]) CreateAppPath(argv[0]);
#ifndef __WIIU__
    InitMem2Manager();
#endif
    savebuffer = (unsigned char *)memalign(FILE_BUFFER_ALIGN, SAVEBUFFERSIZE);
    browserList = (BROWSERENTRY *)memalign(32, sizeof(BROWSERENTRY) * MAX_BROWSER_SIZE);
    nesrom = (unsigned char *)memalign(FILE_BUFFER_ALIGN, 16 * 1024 * 1024);
    if (!savebuffer || !browserList || !nesrom) {
        ErrorPrompt("Not enough memory");
        ExitApp();
        return 1;
    }
    InitGUIThreads();
    InitVBCore();
    // (a loader such as WiiFlow passes the game's folder and file name, as FCE Ultra GX takes them: it starts at once)
    if (argc > 2 && argv[1] && argv[2]) {
        LoadPrefs();
        if (strncmp(argv[1], "sd", 2) == 0) {
            EmuSettings.saveDevice = DEVICE_SD;
            EmuSettings.loadDevice = DEVICE_SD;
        } else if (strncmp(argv[1], "usb", 3) == 0) {
            EmuSettings.saveDevice = DEVICE_USB;
            EmuSettings.loadDevice = DEVICE_USB;
        }
        SavePrefs();
        EmuSettings.autoloadGame = AutoloadGame(argv[1], argv[2]);
        autoboot = EmuSettings.autoloadGame;
    }
    while (!platform->shouldExit()) {
        appRequest = AppRequest::MENU;
        if (!autoboot) {
            platform->getAudio()->startMenuAudio();
            MainMenu(romLoaded ? MENU_GAME : MENU_GAMESELECTION);
        }
        platform->getAudio()->stopMenuAudio();
        if (platform->shouldExit()) break;
        WaitForBackgroundTasks(15000);
        autoboot = false;
        appRequest = AppRequest::NONE;
        SetControllers();
        SetPalette();
        platform->getVideo()->getEmulatorVideo()->resetVideo();
        platform->getAudio()->getEmulatorAudio()->resetAudio();
        SetVBAudioRate();
        platform->getAudio()->startEmulatorAudio();
        while (appRequest == AppRequest::NONE && !platform->shouldExit()) {
            SystemEvent event = platform->getSystemEvent();
            if (event == SystemEvent::ShutdownRequested) break;
            if (event == SystemEvent::ResetRequested) PowerNES();
            GetJoy();
            if (appRequest != AppRequest::NONE) break;
            RunVBFrame();
        }
        platform->getAudio()->stopEmulatorAudio();
        TakeScreenshot();
        platform->getVideo()->startMenuVideo();
    }
    ExitApp();
    return 0;
}
