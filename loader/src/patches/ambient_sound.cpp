/*
 * Ambient sound (docs/known-bugs.md, "Ambient sound"; docs/reverse-engineering.md,
 * "Sound").
 *
 * 1.27a shipped with "Disabled ambient sound while a MIDI issue is being
 * resolved", and 1.27b still has it: a map's day and night ambience (birds,
 * crickets, wind) never plays. Those sounds are MIDI sequences played with a
 * DLS bank (UI\SoundInfo\MIDISounds.slk); every melee map and most custom
 * ones make them with Blizzard.j's SetAmbientDaySound/SetAmbientNightSound,
 * that is CreateMIDISound. The native calls CSound::InitMIDI, which 1.27a
 * changed to make a plain looping sound from the label instead. The WAV label
 * tables don't have the MIDI labels, so the sound is empty and StartSound
 * does nothing. The rest of the MIDI path is intact: the Miles MIDI driver
 * and DLS synthesizer are opened at startup, StartSound plays CSound.midi,
 * the release frees it and saved games restore it with the game's own MIDI
 * constructor (CreateMIDISnd).
 *
 * Mechanism: the innermost REPLACE handler on CSound::InitMIDI (so mods'
 * BEFORE and REPLACE handlers still run first and see the call) makes the
 * MIDI sound with CreateMIDISnd into CSound.midi in place of the 1.27b body.
 * The sound gets the ambient category, so the game's Ambient sounds option
 * still turns it off. If the label isn't a MIDI one (or there's no MIDI
 * driver: the nomidi option) it hands on to the game's own body, as without
 * the patch.
 *
 * Only what each player hears changes: CreateMIDISound still returns the
 * same handle, and sound was already local (each player's sound options).
 */
#include "patches.h"

#include "core/hooks.h"
#include "hooks.h"
#include "log.h"
#include "tome/tome.h"

#include <cstdint>
#include <string>

namespace tome::patches {
namespace {

using CreateMidiFn = void*(__fastcall*)(const char* label, int open_arg, int fade_in, int fade_out, uint32_t category);

CreateMidiFn g_create_midi = nullptr;
uint32_t g_sound_offset = 0;
uint32_t g_midi_offset = 0;
uint32_t g_ambient_category = 0;

// REPLACE CSound::InitMIDI(this, label, fade_in, fade_out), innermost: in
// place of the game's body only. Runs on the main thread (the native's).
void TOME_CALL on_init_midi(tome_mod*, tome_hook_call* call, void*) {
    char* self = call->arg_count >= 4 ? reinterpret_cast<char*>(call->args[0]) : nullptr;
    const char* label = self ? reinterpret_cast<const char*>(call->args[1]) : nullptr;
    // The native's CSound is new; one that already holds a sound is left to
    // the game.
    if (!label || !*label || *reinterpret_cast<void**>(self + g_midi_offset) ||
        *reinterpret_cast<void**>(self + g_sound_offset)) {
        hooks::call_next(call);
        return;
    }
    // edx is passed on to the sequence open; the saved-game loader passes 0.
    void* midi = g_create_midi(label, 0, static_cast<int>(call->args[2]), static_cast<int>(call->args[3]),
                               g_ambient_category);
    if (!midi) {
        TOME_DEBUG("ambient-sound: '%s' isn't a MIDI sound here; the game makes it as before", label);
        hooks::call_next(call);
        return;
    }
    *reinterpret_cast<void**>(self + g_midi_offset) = midi;
    TOME_DEBUG("ambient-sound: made '%s' as a MIDI sound", label);
}

bool install_ambient_sound(uintptr_t base, const BuildMappings& build, std::string& report) {
    g_create_midi = reinterpret_cast<CreateMidiFn>(base + build.sound.create_midi);
    g_sound_offset = build.sound.sound_offset;
    g_midi_offset = build.sound.midi_offset;
    g_ambient_category = build.sound.ambient_category;
    std::string error;
    hooks::Target* init = hooks::find_or_install("CSound::InitMIDI", error);
    if (!init) {
        report = "CSound::InitMIDI: " + error;
        return false;
    }
    hooks::Handler replace;
    replace.phase = TOME_HOOK_REPLACE;
    replace.priority = INT32_MIN;  // innermost: mods' handlers run first, only the game's body is replaced
    replace.fn = on_init_midi;
    hooks::add(init, replace);
    report = "plays maps' MIDI ambient sounds again";
    return true;
}

const char* const kSymbols[] = {"CSound::InitMIDI", "CreateMIDISnd", "CSound.sound", "CSound.midi",
                                "SoundCategory.ambient", nullptr};

} // namespace

extern const Patch kAmbientSound = {
    "ambient-sound",
    "plays the day and night ambient sounds that 1.27a turned off (CreateMIDISound made nothing)",
    nullptr,  // any build that maps the symbols below
    kSymbols,
    nullptr,  // nothing to protect: a mod may change the game's sounds
    nullptr,  // needed everywhere
    install_ambient_sound,
};

} // namespace tome::patches
