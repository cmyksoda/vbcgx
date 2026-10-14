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
#include "drivers/ogc/wii/WiiPlatform.h"
#include "drivers/ogc/wii/mem2.h"
static WiiPlatform platformInstance;
Platform *platform = &platformInstance;
AppRequest appRequest = AppRequest::NONE;
char appPath[MAXPATHLEN] = {};
unsigned char *nesrom = nullptr;
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
    platform->requestExit(EmuSettings.exitAction, false);
}
int main(int argc, char **argv)
{
    PlatformConfig config;
    config.canvasWidth = 640;
    config.canvasHeight = 480;
    platform->init(config);
    platform->setSaveHandler(SaveAppData);
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
    InitMem2Manager();
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
    while (!platform->shouldExit()) {
        appRequest = AppRequest::MENU;
        platform->getAudio()->startMenuAudio();
        MainMenu(romLoaded ? MENU_GAME : MENU_GAMESELECTION);
        platform->getAudio()->stopMenuAudio();
        if (platform->shouldExit()) break;
        WaitForBackgroundTasks(15000);
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
