#pragma once

// The first mod load runs on the bootstrap thread while the game's main
// thread keeps starting up. Two of the game's steps wait for it (at most
// 10 s), so mods are loaded and enabled before them:
//   - InitNatives, so a map that loads at once (-loadfile) sees every
//     mod's natives (natives.cpp);
//   - the game opening its sound device (AIL_open_digital_driver, through
//     game.dll's import), so a mod can choose the device: Miles is what
//     loads tome.mix, during AIL_startup, just before.

#include <string>

namespace tome::mods_ready {

// Makes the event. DllMain, before anything that waits on it.
void init();

// The first mod load finished (or won't happen). Idempotent.
void set();

// Waits for set(), at most 10 s; on a timeout logs `what` and stops every
// later wait too. Main thread.
void wait(const char* what);

// Holds the game's sound start-up until mods have loaded. DllMain, after
// hooks::init (patches::install).
bool install_sound_wait(std::string& report);

} // namespace tome::mods_ready
