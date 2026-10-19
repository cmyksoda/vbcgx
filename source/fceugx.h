/****************************************************************************
 * FCE Ultra GX
 *
 * Daryl Borth 2008-2026
 *
 * fceugx.h
 *
 * This file controls overall program flow. Most things start and end here!
 ****************************************************************************/

#ifndef _FCEUGX_H_
#define _FCEUGX_H_

#include <unistd.h>

#include "drivers/InputDriver.h"
#include "drivers/FileSystemDriver.h"
#include <gccore.h>

#define APPNAME			"VBoy Color GX"
#define APPVERSION		"1.0.0"
#define APPFOLDER 		"vbcgx"
#define PREF_FILE_NAME	"settings.xml"

#define MAXPATHLEN 1024
#define NOTSILENT 0
#define SILENT 1

enum {
    SAVEFOLDER_SAVES = 0,
    SAVEFOLDER_LENGTH
};

enum {
    LOADFOLDER_ROMS = 0,
    LOADFOLDER_SCREENSHOTS,
    LOADFOLDER_COVERS,
    LOADFOLDER_ARTWORK,
    LOADFOLDER_LENGTH
};

typedef struct {
    int id;
    const char *name;
} FolderDef;

const FolderDef saveFolder[] = {
    { SAVEFOLDER_SAVES,  "saves" }
};

const FolderDef loadFolder[] = {
    { LOADFOLDER_ROMS,        "roms" },
    { LOADFOLDER_SCREENSHOTS, "screenshots" },
    { LOADFOLDER_COVERS,      "covers" },
    { LOADFOLDER_ARTWORK,     "artwork" }
};

enum
{
	FILE_RAM,
	FILE_STATE,
	FILE_ROM
};

enum {
	AUTOLOAD_OFF = 0,
	AUTOLOAD_RAM,
	AUTOLOAD_STATE
};

enum {
	AUTOSAVE_OFF = 0,
	AUTOSAVE_RAM,
	AUTOSAVE_STATE,
	AUTOSAVE_BOTH
};

enum {
	PREVIEWIMAGE_SCREENSHOT = 0,
	PREVIEWIMAGE_COVER,
	PREVIEWIMAGE_ARTWORK,
	PREVIEWIMAGE_LENGTH
};

enum {
	SCALING_MAINTAIN_ASPECT = 0,
	SCALING_PARTIAL_STRETCH,
	SCALING_STRETCH_TO_FIT,
	SCALING_LENGTH
};

#define VIDEO_FIXED_RATIO_MAX 2 // (3x of 224 rows won't fit any output mode)

enum {
	VIDEOMODE_AUTO = 0,
	VIDEOMODE_NTSC,
	VIDEOMODE_PROGRESSIVE,
	VIDEOMODE_PAL,
	VIDEOMODE_PAL60,
	VIDEOMODE_ORIGINAL_240P,
    VIDEOMODE_240P_PAL,
	VIDEOMODE_LENGTH
};

enum
{
	CTRL_PAD
};

#define CTRL_BTN_MAPPINGS 1

const char ctrlName[1][20] =
{ "Virtual Boy Pad" };

enum
{
	LANG_JAPANESE = 0,
	LANG_ENGLISH,
	LANG_GERMAN,
	LANG_FRENCH,
	LANG_SPANISH,
	LANG_ITALIAN,
	LANG_DUTCH,
	LANG_SIMP_CHINESE,
	LANG_TRAD_CHINESE,
	LANG_KOREAN,
	LANG_PORTUGUESE,
	LANG_BRAZILIAN_PORTUGUESE,
	LANG_CATALAN,
	LANG_TURKISH,
	LANG_SWEDISH,
	LANG_LENGTH
};

struct SEmuSettings
{
	int		autoLoad;
    int		autoSave;
    int		loadDevice; // For ROMS: Auto, SD, DVD, USB, Network Share
	int		saveDevice; // For SRAM, Freeze, Prefs: Auto, SD, USB, Network Share
	bool	appendAuto;
	char	loadFolder[MAXPATHLEN]; // Path to game files
	char	lastFileLoaded[MAXPATHLEN]; //Last file loaded filename
	char	saveFolder[MAXPATHLEN]; // Path to save files
	char	screenshotsFolder[MAXPATHLEN]; // Path to screenshot files
	char	coverFolder[MAXPATHLEN]; 	// Path to cover files
	char	artworkFolder[MAXPATHLEN]; 	// Path to artwork files
	bool	autoloadGame;

	SmbShareInfo smbShare; // host/share/user/password for DEVICE_SMB

	int		videoMode;
	int		videoAspectRatioCorrection;
	int		videoFixedRatio; // 0 off, else rows per Virtual Boy row
	bool	videoBilinearFilter;
	float	videoZoomHor;
	float	videoZoomVert;
	int		videoXshift;
	int		videoYshift;
	int		currentPalette;
    int colorMode, gradient, tint;
    int tintCustom;                  // Tint Color: 0 Default (Red), 1 Custom
    int tintRed, tintGreen, tintBlue; // the custom color, hundredths (0-100, steps of 5)
    bool colorPacks;

	int		wiimoteOrientation;
	int		exitAction;
	int		musicVolume;
	int		sfxVolume;
	int 	language;
	int		previewImage;
	bool	hideSramSaving;
};

void ExitApp();
extern struct SEmuSettings EmuSettings;
enum class AppRequest { NONE, MENU };
extern AppRequest appRequest;
extern char appPath[];

#endif
