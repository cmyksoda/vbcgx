#include "fceugx.h"
#include "fceusupport.h"
#include "filebrowser.h"
#include "menu.h"
#include "drivers/Platform.h"
#include <cstring>
bool romLoaded = false;
int GCMemROM(int size)
{
    CloseVBGame();
    if (size < 1024 || size > 16 * 1024 * 1024) {
        ErrorPrompt("Invalid Virtual Boy ROM size");
        return 0;
    }
    char path[MAXPATHLEN];
    snprintf(path, sizeof(path), "%s/%s", browser.dir, browserList[browser.selIndex].filename);
    retro_game_info game{path, nesrom, static_cast<size_t>(size), nullptr};
    romLoaded = retro_load_game(&game);
    if (!romLoaded) ErrorPrompt("Invalid Virtual Boy game");
    if (romLoaded) platform->getAudio()->getEmulatorAudio()->resetAudio();
    return romLoaded;
}
