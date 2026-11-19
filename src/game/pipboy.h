#ifndef FALLOUT_GAME_PIPBOY_H_
#define FALLOUT_GAME_PIPBOY_H_

#include <stddef.h>

#include "game/art.h"
#include "game/message.h"
#include "plib/db/db.h"
#include "plib/gnw/rect.h"

namespace fallout {

typedef enum PipboyOpenIntent {
    PIPBOY_OPEN_INTENT_UNSPECIFIED = 0,
    PIPBOY_OPEN_INTENT_REST = 1,
} PipboyOpenIntent;

typedef void(PipboyRenderProc)(int a1);

int pipboy(int intent);
void pip_init();
// Hash of all quest global-var values; changes on any quest progress (for autosave).
unsigned int pipboy_quest_state_hash();
int save_pipboy(DB_FILE* stream);
int load_pipboy(DB_FILE* stream);

// The CONTROLS list for the pause menu's own window (options.cc): its title, its row count, and row `index` as the
// Pip-Boy page prints it (false for a blank spacer; a heading has `button` empty).
const char* pipboy_controls_title();
int pipboy_controls_count();
bool pipboy_controls_row(int index, char* button, size_t buttonSize, char* action, size_t actionSize);

} // namespace fallout

#endif /* FALLOUT_GAME_PIPBOY_H_ */
