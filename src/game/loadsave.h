#ifndef FALLOUT_GAME_LOADSAVE_H_
#define FALLOUT_GAME_LOADSAVE_H_

#include "game/art.h"
#include "game/message.h"
#include "plib/db/db.h"
#include "plib/gnw/rect.h"

namespace fallout {

typedef enum LoadSaveMode {
    // Special case - loading game from main menu.
    LOAD_SAVE_MODE_FROM_MAIN_MENU,

    // Normal (full-screen) save/load screen.
    LOAD_SAVE_MODE_NORMAL,

    // Quick load/save.
    LOAD_SAVE_MODE_QUICK,
} LoadSaveMode;

void InitLoadSave();
void ResetLoadSave();
int SaveGame(int mode);
int LoadGame(int mode);
// Headless saves for the auto/quick save scheme (slots 0,1=autosave ping-pong, 2=quick).
int SaveGameSlot(int slot, const char* description);
// Resume the most recent save with no load screen (main menu CONTINUE). 1 = loaded,
// 0 = nothing to resume, -1 = failed.
int LoadGameNewest();
int AutoSaveLocation();
int AutoSaveQuest();
int QuickSaveGame();
bool LoadSaveConsumeJustLoaded();
int isLoadingGame();
void KillOldMaps();
int MapDirErase(const char* path, const char* a2);
int MapDirEraseFile(const char* a1, const char* a2);

// Counter of successful loads. Sample it on entry to any screen that runs its own modal loop and
// bail out when it changes -- the game under you has been swapped. See LoadSlot.
unsigned int LoadGameSerial();
} // namespace fallout

#endif /* FALLOUT_GAME_LOADSAVE_H_ */
