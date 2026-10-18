#ifndef FALLOUT_GAME_AUTOTEST_H_
#define FALLOUT_GAME_AUTOTEST_H_

namespace fallout {

// THE GAME PLAYS A FIGHT BY ITSELF (2026-10-18). `fallout-ce.exe --autotest <scenario.txt>` skips
// the movies and the menu, loads the scenario's map, places the dude, spawns its enemies and lets a
// small bot play through a VIRTUAL GAMEPAD -- the same input path a player's pad takes -- in a hidden
// window, then quits and leaves the trace behind. .claude/tools/trace/airep.js judges that trace,
// so a change to the AI can be checked before and after without anyone playing.
// The scenario format is documented in autotest.cc; the scenarios live in .claude/tools/autotest/.

// Reads --autotest from the command line. Call first thing in gnw_main, before any SDL setup.
bool autotest_init(int argc, char** argv);
bool autotest_active();

char* autotest_map();
int autotest_seed();

// After the scenario's map has loaded: place, equip, spawn.
void autotest_setup_world();

// Every frame of the game loop: drives the virtual pad, ends the run.
void autotest_tick(float dt);

// The game loop has ended (the run is over, or the dude died and the game ended it): write the last
// line, close the trace and leave at once -- a self-played run needs no teardown, and the normal
// one expects the main menu to come next.
[[noreturn]] void autotest_finish();

// False in a self-played run unless its scenario says `mapscript on`: an encounter map's script
// (RndDesrt.int...) spawns its own critters into the arena.
bool autotest_map_script_allowed();

// The virtual pad's device index, or -1 (gamepad_ui opens this one instead of a real pad).
int autotest_controller_index();

} // namespace fallout

#endif /* FALLOUT_GAME_AUTOTEST_H_ */
