#ifndef FALLOUT_GAME_OPTIONS_H_
#define FALLOUT_GAME_OPTIONS_H_

#include "plib/db/db.h"

namespace fallout {

int do_options();

// Just the preferences screen (no Save/Load/Exit around it), for the main menu's OPTIONS entry.
int do_preferences();
bool options_is_open();
int PauseWindow(bool is_world_map);
int init_options_menu();
int save_options(DB_FILE* stream);
int load_options(DB_FILE* stream);
void IncGamma();
void DecGamma();

} // namespace fallout

#endif /* FALLOUT_GAME_OPTIONS_H_ */
