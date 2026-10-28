#include "patches.h"

#include "hooks.h"

#include <string>

namespace tome::patches {

// Each patch is defined in its own file under src/patches/ and listed here.
extern const Patch kMemhackGuard;
extern const Patch kPathSanitizer;
extern const Patch kBlpGuard;
extern const Patch kD3dResetLights;
extern const Patch kGlContextRestore;

const Patch* const kAll[] = {
    &kMemhackGuard,
    &kPathSanitizer,
    &kBlpGuard,
    &kD3dResetLights,
    &kGlContextRestore,
};

namespace {

bool build_listed(const char* const* builds, const std::string& name) {
    if (!builds) return true;  // any build
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

} // namespace

bool install(uintptr_t base, const BuildMappings& build, std::string& report) {
    // init is idempotent and events::install may have run it already; call it
    // so patches don't depend on events being installed first.
    hooks::init(base, build.data);
    const std::string& name = build.data->name;
    // Detail is collected into `report` rather than logged here: this runs in
    // DllMain, before the bootstrap thread opens tome.log, so a log line now
    // would be dropped. Bootstrap logs the report once the file is open.
    int applied = 0, skipped = 0, failed = 0;
    std::string detail;
    auto note = [&](const std::string& clause) { detail += (detail.empty() ? "" : "; ") + clause; };
    for (const Patch* p : kAll) {
        if (!build_listed(p->builds, name)) {
            note(std::string(p->name) + " skipped (not for " + name + ")");
            ++skipped;
            continue;
        }
        if (const char* missing = first_missing(p->symbols, *build.data)) {
            note(std::string(p->name) + " skipped (" + name + " has no mapping for " + missing + ")");
            ++skipped;
            continue;
        }
        std::string r;
        if (p->needed && !p->needed(r)) {
            note(std::string(p->name) + " skipped (" + r + ")");
            ++skipped;
            continue;
        }
        if (p->install(base, build, r)) {
            // Lock the patch's functions against mod hooks, now that it's live.
            if (p->reserve) {
                for (const char* const* s = p->reserve; *s; ++s) hooks::reserve(*s);
            }
            note(std::string(p->name) + " applied" + (r.empty() ? "" : " (" + r + ")"));
            ++applied;
        } else {
            note(std::string(p->name) + " FAILED (" + r + ")");
            ++failed;
        }
    }
    report = std::to_string(applied) + " applied, " + std::to_string(skipped) + " skipped";
    if (failed) report += ", " + std::to_string(failed) + " failed";
    if (!detail.empty()) report += ": " + detail;
    return true;
}

} // namespace tome::patches
