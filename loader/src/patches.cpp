// SPDX-License-Identifier: GPL-3.0-or-later
#include "patches.h"

#include "game_build.h"
#include "hooks.h"

#include <iterator>
#include <string>

namespace tome::patches {

// Each patch is defined in its own file under src/patches/ and listed here.
extern const Patch kMemhackGuard;
extern const Patch kPathSanitizer;
extern const Patch kBlpGuard;
extern const Patch kD3dResetLights;
extern const Patch kGlContextRestore;
extern const Patch kBackgroundMute;
extern const Patch kWidescreen;
extern const Patch kAmbientSound;
extern const Patch kMovieDisplayReset;
extern const Patch kMovieFrameLength;
extern const Patch kExecuteFuncGuard;
extern const Patch kGlobalStringGuard;
extern const Patch kLanMapGuard;
extern const Patch kJassTokenGuard;
extern const Patch kCommandCardGuard;

const Patch* const kAll[] = {
    &kMemhackGuard,
    &kPathSanitizer,
    &kBlpGuard,
    &kD3dResetLights,
    &kGlContextRestore,
    &kBackgroundMute,
    &kWidescreen,
    &kAmbientSound,
    &kMovieDisplayReset,
    &kMovieFrameLength,
    &kExecuteFuncGuard,
    &kGlobalStringGuard,
    &kLanMapGuard,
    &kJassTokenGuard,
    &kCommandCardGuard,
};

namespace {

bool build_listed(const char* const* builds, const std::string& name) {
    for (const char* const* b = builds; *b; ++b) {
        if (name == *b) return true;
    }
    return false;
}

// The first required symbol the build doesn't map, or null if it maps them all.
const char* first_missing(const char* const* symbols, const mappings::Build& build) {
    if (!symbols) return nullptr;
    for (const char* const* s = symbols; *s; ++s) {
        if (!build.find(*s)) return *s;
    }
    return nullptr;
}

// The variant of a patch that lists none: needs nothing more, changes nothing.
const Variant kPlain{};

// What install did with each patch of kAll, in its order.
Outcome g_outcomes[std::size(kAll)];

} // namespace

std::span<const Patch* const> all() {
    return kAll;
}

const Variant* variant_for(const Patch& patch, const std::string& build) {
    if (patch.variants.empty()) return &kPlain;
    const Variant* fallback = nullptr;
    for (const Variant& v : patch.variants) {
        if (!v.builds) {
            if (!fallback) fallback = &v;
        } else if (build_listed(v.builds, build)) {
            return &v;
        }
    }
    return fallback;
}

bool only_under_wine(std::string& report) {
    if (running_on_wine()) return true;
    report = "not needed outside Wine";
    return false;
}

Outcome outcome(std::string_view name) {
    for (size_t i = 0; i < std::size(kAll); ++i) {
        if (name == kAll[i]->name) return g_outcomes[i];
    }
    return {};
}

int count(State state) {
    int n = 0;
    for (const Outcome& o : g_outcomes) n += o.state == state;
    return n;
}

bool install(uintptr_t base, const BuildMappings& build, std::string& report, OffReason off) {
    // init is idempotent and events::install may have run it already; call it
    // so patches don't depend on events being installed first.
    hooks::init(base, build.data);
    const std::string& name = build.data->name;
    // Detail is collected into `report` rather than logged here: this runs in
    // DllMain, before the bootstrap thread opens tome.log, so a log line now
    // would be dropped. Bootstrap logs the report once the file is open.
    int applied = 0, skipped = 0, turned_off = 0, failed = 0;
    std::string detail;
    auto note = [&](const std::string& clause) { detail += (detail.empty() ? "" : "; ") + clause; };
    for (size_t i = 0; i < std::size(kAll); ++i) {
        const Patch* p = kAll[i];
        Outcome& out = g_outcomes[i];
        auto skip = [&](std::string why) {
            note(std::string(p->name) + " skipped (" + why + ")");
            out = {State::Skipped, std::move(why)};
            ++skipped;
        };
        // Before the other checks: the player's choice holds on every build.
        if (std::string why = off ? off(p->name) : std::string(); !why.empty()) {
            note(std::string(p->name) + " off (" + why + ")");
            out = {State::Off, std::move(why)};
            ++turned_off;
            continue;
        }
        const Variant* variant = variant_for(*p, name);
        if (!variant || variant->skip) {
            skip("not for " + name + (variant ? std::string(": ") + variant->skip : ""));
            continue;
        }
        const char* missing = first_missing(p->symbols, *build.data);
        if (!missing) missing = first_missing(variant->symbols, *build.data);
        if (missing) {
            skip(name + " has no mapping for " + missing);
            continue;
        }
        std::string r;
        if (p->needed && !p->needed(r)) {
            skip(r);
            continue;
        }
        if (p->install(base, build, *variant, r)) {
            // Lock the patch's functions against mod hooks, now that it's live.
            for (const char* const* list : {p->reserve, variant->reserve}) {
                for (const char* const* s = list; s && *s; ++s) hooks::reserve(*s);
            }
            note(std::string(p->name) + " applied" + (r.empty() ? "" : " (" + r + ")"));
            out = {State::Applied, r};
            ++applied;
        } else {
            note(std::string(p->name) + " FAILED (" + r + ")");
            out = {State::Failed, r};
            ++failed;
        }
    }
    report = std::to_string(applied) + " applied, " + std::to_string(skipped) + " skipped";
    if (turned_off) report += ", " + std::to_string(turned_off) + " turned off";
    if (failed) report += ", " + std::to_string(failed) + " failed";
    if (!detail.empty()) report += ": " + detail;
    return true;
}

} // namespace tome::patches
