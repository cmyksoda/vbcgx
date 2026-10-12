/****************************************************************************
 * FCE Ultra GX
 *
 * Daryl Borth 2008-2026
 *
 * fceustate.cpp
 *
 * Memory Based Load/Save RAM Manager
 *
 * These are the battery-backed RAM (save data) routines, brought together
 * as GCxxxxx
 * The original file I/O is replaced with Memory Read/Writes to the
 * savebuffer below
 ****************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <string.h>

#include "fceugx.h"
#include "fceusupport.h"
#include "menu.h"
#include "filebrowser.h"
#include "fileop.h"
#include "fceuram.h"


static int GetGameRAM(unsigned char *buffer)
{
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    void *ram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!ram || size > SAVEBUFFERSIZE) return 0;
    if (buffer) memcpy(buffer, ram, size);
    return size;
}
static bool WriteRAMFromSaveBuffer(char *path, int size, bool silent)
{
    bool ok = size > 0 && SaveFile((char *)savebuffer, path, size, silent) == size;
    if (ok && !silent) InfoPrompt("Save successful");
    return ok;
}

bool SaveRAM (char * filepath, bool silent)
{
	bool retval = false;
	int datasize = 0;
	int device;

	if(!FindDevice(filepath, &device))
		return 0;


	AllocSaveBuffer ();

	// save game save to savebuffer
	datasize = GetGameRAM(savebuffer);

	retval = WriteRAMFromSaveBuffer(filepath, datasize, silent);

	FreeSaveBuffer ();
	return retval;
}

/****************************************************************************
 * Deferred auto-save
 *
 * SnapshotRAMAuto() copies everything that is needed (the RAM and where it
 * goes) so it can be called from the main thread at the moment the game is
 * left. WriteRAMSnapshot() then does the slow part - the device I/O - and
 * can run whenever, on any thread, even after another game has been loaded.
 ***************************************************************************/
struct RAMSnapshot
{
	char path[MAXPATHLEN];
	unsigned char * data;
	int size;
};

RAMSnapshot * SnapshotRAMAuto ()
{

	int size = GetGameRAM(nullptr);

	if(size <= 0)
		return nullptr;

	RAMSnapshot * snapshot = (RAMSnapshot *)calloc(1, sizeof(RAMSnapshot));

	if(!snapshot)
		return nullptr;

	snapshot->data = (unsigned char *)malloc(size);

	if(!snapshot->data || !MakeFilePath(snapshot->path, FILE_RAM, romFilename, 0))
	{
		FreeRAMSnapshot(snapshot);
		return nullptr;
	}

	GetGameRAM(snapshot->data);
	snapshot->size = size;
	return snapshot;
}

bool WriteRAMSnapshot (RAMSnapshot * snapshot, bool silent)
{
	int device;

	if(!snapshot || !snapshot->data || !FindDevice(snapshot->path, &device))
		return false;

	AllocSaveBuffer ();
	memcpy(savebuffer, snapshot->data, snapshot->size);
	bool retval = WriteRAMFromSaveBuffer(snapshot->path, snapshot->size, silent);
	FreeSaveBuffer ();
	return retval;
}

void FreeRAMSnapshot (RAMSnapshot * snapshot)
{
	if(!snapshot)
		return;

	free(snapshot->data);
	free(snapshot);
}

bool
SaveRAMAuto (bool silent)
{
	char filepath[1024];

	if(!MakeFilePath(filepath, FILE_RAM, romFilename, 0))
		return false;

	return SaveRAM(filepath, silent);
}

bool LoadRAM (char * filepath, bool silent)
{
	int offset = 0;
	bool retval = false;
	int device;

	if(!FindDevice(filepath, &device))
		return 0;

    AllocSaveBuffer();
    offset = LoadFile(filepath, silent);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    void *ram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (ram && size && offset == static_cast<int>(size)) {
        memcpy(ram, savebuffer, size);
        retval = true;
    } else if (!silent) {
        ErrorPrompt("Missing or invalid battery save");
    }

	FreeSaveBuffer ();
	return retval;
}

bool
LoadRAMAuto (bool silent)
{
	char filepath[MAXPATHLEN];
	char filepath2[MAXPATHLEN];

	// look for Auto save file
	if(!MakeFilePath(filepath, FILE_RAM, romFilename, 0))
		return false;

	if (LoadRAM(filepath, silent))
		return true;

	if (!EmuSettings.appendAuto)
		return false;

	// look for file with no number or Auto appended
	if(!MakeFilePath(filepath2, FILE_RAM, romFilename, -1))
		return false;

	if(LoadRAM(filepath2, silent))
	{
		// rename this file - append Auto
		rename(filepath2, filepath); // rename file (to avoid duplicates)
		return true;
	}
	return false;
}
