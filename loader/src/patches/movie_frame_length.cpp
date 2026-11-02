// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Movie frames under Wine (docs/known-bugs.md, "Under Wine";
 * docs/reverse-engineering.md, "blizzard.ax").
 *
 * The movies (the intro, the campaign cinematics) are AVIs of BLZ0 video,
 * which blizzard.ax, the game's own DirectShow decoder, turns into frames for
 * the video renderer. It asks for buffers of width * 4 * height bytes
 * whatever format it connected with, and its Transform never sets a sample's
 * length, so every frame claims the whole buffer. Windows' renderer copies a
 * frame by the surface's size and never notices. Wine's VMR7 copies the
 * sample's length: with YUY2 (2 bytes a pixel) that's twice the surface, and
 * the movie's first frame crashes the game (an access violation writing in
 * msvcrt's memcpy, called from quartz.dll). Wines whose splitter decodes
 * BLZ0 itself never use blizzard.ax for the picture (GE-Proton 10); those
 * with the winedmo splitter (GE-Proton 11) do.
 *
 * Mechanism: before each movie (a BEFORE handler on Movie::SetDisplayMode,
 * the first thing Movie::Play does, before it builds the graph), the first
 * time, Tome loads the install's blizzard.ax and keeps it loaded, so the
 * DirectShow graph gets this copy. It finds its mapping (mappings/blizzard.ax.json,
 * by PE timestamp and size), checks its bytes (verify_loaded_module) and
 * points the filter's Transform vtable slot at a
 * wrapper: the decoder's own, then the output sample's length set to one
 * frame in the format it's in now (core/movie_decoder.h). The buffers keep
 * the decoder's size, so it always has room, even after a format change.
 * The intro at startup plays before Tome loads (with `seenintromovie` 0), so
 * it isn't covered.
 *
 * Only under Wine: Windows ignores the length, and the decoder is left as it
 * was there. Elsewhere the patch is skipped (`needed`).
 */
#include "patches.h"

#include "core/hooks.h"
#include "core/movie_decoder.h"
#include "hooks.h"
#include "log.h"
#include "mappings.h"
#include "patch.h"
#include "tome/tome.h"

#include <windows.h>

#include <strmif.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// GCC warns that __thiscall is meant for class methods, but it does apply it
// to the wrapper below, which the decoder's vtable calls as one.
#pragma GCC diagnostic ignored "-Wattributes"

namespace tome::patches {
namespace {

// The loaded blizzard.ax's mapping (BlzFilter.* fields), set before the
// slot is patched.
const BlizzardAxMappings* g_ax = nullptr;

using TransformFn = HRESULT(__thiscall*)(void* filter, IMediaSample* in, IMediaSample* out);
TransformFn g_transform = nullptr;
std::atomic<uint32_t> g_logged_frame{0};

template <typename T>
T field(const void* object, size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const char*>(object) + offset, sizeof value);
    return value;
}

// In the decoder's vtable in place of Transform. Runs on the graph's
// streaming thread, once a frame.
HRESULT __thiscall transform(void* filter, IMediaSample* in, IMediaSample* out) {
    const HRESULT hr = g_transform(filter, in, out);
    if (hr != S_OK || !filter || !out) return hr;  // S_FALSE: the frame isn't delivered
    const uint32_t format = field<uint32_t>(filter, g_ax->format_offset);
    const int32_t width = field<int32_t>(filter, g_ax->width_offset);
    const int32_t height = field<int32_t>(filter, g_ax->height_offset);
    const int32_t row = movie_decoder::row_pixels(field<int32_t>(filter, g_ax->output_width_offset), width);
    const uint32_t frame = movie_decoder::frame_bytes(format, row, height);
    const long size = out->GetSize();
    if (frame == 0 || static_cast<long>(frame) >= size) return hr;
    out->SetActualDataLength(static_cast<long>(frame));
    if (g_logged_frame.exchange(frame) != frame) {
        TOME_INFO("movie-frame-length: %dx%d movie, format %u: frames of %u bytes (buffers of %ld)", width, height,
                  format, frame, size);
    }
    return hr;
}

// Loads (and keeps) the install's blizzard.ax and patches its vtable.
bool fix_decoder(std::string& error) {
    wchar_t path[MAX_PATH];
    HMODULE game = GetModuleHandleW(L"game.dll");
    DWORD n = game ? GetModuleFileNameW(game, path, MAX_PATH) : 0;
    if (n == 0 || n >= MAX_PATH) {
        error = "game.dll's folder is unknown";
        return false;
    }
    std::wstring ax(path, n);
    ax.resize(ax.find_last_of(L"\\/") + 1);
    ax += L"blizzard.ax";
    // Never freed: DirectShow loads this same file by its CLSID, and must get
    // the patched copy, for every movie.
    HMODULE module = LoadLibraryW(ax.c_str());
    if (!module) {
        error = "couldn't load blizzard.ax (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    const BlizzardAxMappings* mapping = find_blizzard_ax_by_header(module);
    if (!mapping) {
        error = "unknown blizzard.ax (no mappings/ file matches its PE header)";
        return false;
    }
    std::vector<std::string> mismatches;
    if (!verify_loaded_module(module, *mapping->data, mismatches)) {
        error = "blizzard.ax doesn't match its mapping: " + mismatches.front();
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    const uintptr_t transform_code = base + mapping->transform;
    g_ax = mapping;
    g_transform = reinterpret_cast<TransformFn>(transform_code);
    return patch_pointer(base + mapping->transform_slot, transform_code, reinterpret_cast<const void*>(&transform), error);
}

// BEFORE Movie::SetDisplayMode: the first movie. Main thread.
void TOME_CALL on_set_display_mode(tome_mod*, tome_hook_call*, void*) {
    static bool done = false;
    if (done) return;
    done = true;
    std::string error;
    if (fix_decoder(error)) {
        TOME_INFO("movie-frame-length: blizzard.ax patched");
    } else {
        TOME_WARN("movie-frame-length: %s; movies may crash under this Wine", error.c_str());
    }
}

bool install_movie_frame_length(uintptr_t, const BuildMappings&, std::string& report) {
    std::string error;
    hooks::Target* set_mode = hooks::find_or_install("Movie::SetDisplayMode", error);
    if (!set_mode) {
        report = "Movie::SetDisplayMode: " + error;
        return false;
    }
    hooks::Handler before;
    before.phase = TOME_HOOK_BEFORE;
    before.fn = on_set_display_mode;
    hooks::add(set_mode, before);
    report = "gives blizzard.ax's movie frames their real length, before the first movie";
    return true;
}

const char* const kSymbols[] = {"Movie::SetDisplayMode", nullptr};

} // namespace

extern const Patch kMovieFrameLength = {
    "movie-frame-length",
    "keeps movies (campaign cinematics) from crashing Wine's video renderer with frames longer than its surface",
    nullptr,  // any build that maps the symbols below
    kSymbols,
    nullptr,  // nothing to protect: a mod can't reopen a hole through it
    only_under_wine,
    install_movie_frame_length,
};

} // namespace tome::patches
