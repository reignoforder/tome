#include "popups.h"

#include "frames.h"
#include "guard.h"
#include "log.h"
#include "main_thread.h"
#include "mod_handle.h"
#include "restart.h"
#include "unwind.h"

#include <windows.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <vector>

namespace tome::popups {
namespace {

using popup::Style;

struct Entry {
    uint32_t id = 0;
    tome_mod* mod = nullptr;  // null for Tome's own
    tome_popup_fn fn = nullptr;
    void* user = nullptr;
    std::function<void(int)> done;  // Tome's own
    popup::Spec spec;
};

// The queue: any thread adds and removes, the main thread shows the front.
// Entries never change once queued, so readers share them instead of copying.
using EntryPtr = std::shared_ptr<const Entry>;
SRWLOCK g_lock = SRWLOCK_INIT;
std::deque<EntryPtr> g_queue;
uint32_t g_next_id = 1;

// What's on screen (main thread).
struct Shown {
    uint32_t id = 0;  // 0: nothing
    int page = 0;
    void* parent = nullptr;  // the screen's frame the set was made in
    Style style = Style::Glue;
    int context = 0;   // the set's: the popup frame's context; button slot i has context + 1 + i
    int slots = 0;     // buttons made in this set
    int used = 0;      // ...and shown for this page
};
Shown g_shown;
int g_next_context = 0;

// Every slot a layout can use: three in a row, or a page of the column with Previous and Next.
constexpr int kMaxSlots = popup::kColumnPage + 2;
constexpr int kContextsPerSet = kMaxSlots + 1;
// How often a showing popup's frames are shown again (frames::on_frame).
constexpr DWORD kKeepShownMs = 250;
DWORD g_last_keep = 0;
constexpr int kTopLeft = 0, kTop = 1, kBottomRight = 8;

void* frame(Style style, const char* part, int context) {
    return frames::get(popup::frame_name(style, part).c_str(), context);
}

EntryPtr front() {
    SharedLock hold(g_lock);
    return g_queue.empty() ? nullptr : g_queue.front();
}

// The entry with `id` in the queue, or end(). Hold g_lock.
std::deque<EntryPtr>::iterator find_locked(uint32_t id) {
    return std::find_if(g_queue.begin(), g_queue.end(), [id](const EntryPtr& e) { return e->id == id; });
}

// Removes the entry with `id`; null if it's gone already.
EntryPtr take(uint32_t id) {
    ExclusiveLock hold(g_lock);
    auto it = find_locked(id);
    if (it == g_queue.end()) return nullptr;
    EntryPtr entry = std::move(*it);
    g_queue.erase(it);
    return entry;
}

// Hides the current set's frames, if they still exist.
void hide_set() {
    if (g_shown.parent == nullptr) return;
    if (void* root = frame(g_shown.style, "Popup", g_shown.context)) frames::set_visible(root, false);
}

void hide() {
    if (g_shown.id == 0) return;
    g_shown.id = 0;
    hide_set();
}

void finish(uint32_t id, int choice);
void render();

void clicked(uint32_t id, int slot) {
    if (g_shown.id != id) return;
    EntryPtr entry = front();
    if (!entry || entry->id != id) return;
    popup::Layout l = popup::layout(entry->spec, g_shown.style, g_shown.page);
    if (slot >= static_cast<int>(l.buttons.size())) return;
    int option = l.buttons[slot].option;
    if (option == popup::kPrevious || option == popup::kNext) {
        g_shown.page += option == popup::kNext ? 1 : -1;
        render();
        return;
    }
    finish(id, option);
}

// Makes the popup frame and the button slots on this screen, as far as they're missing.
bool make_set(const popup::Layout& l) {
    const Style style = g_shown.style;
    void* root = frame(style, "Popup", g_shown.context);
    if (!root) {
        root = frames::create(popup::frame_name(style, "Popup").c_str(), g_shown.parent, 0, g_shown.context);
        g_shown.slots = 0;
        if (!root) {
            TOME_ERROR("Popup: couldn't create its frames");
            return false;
        }
    }
    const std::string button = popup::frame_name(style, "Button");
    while (g_shown.slots < static_cast<int>(l.buttons.size())) {
        const int slot = g_shown.slots;
        const int context = g_shown.context + 1 + slot;
        void* b = frames::create(button.c_str(), root, 0, context);
        if (!b) {
            TOME_ERROR("Popup: couldn't create a button");
            return false;
        }
        const int set = g_shown.context;
        frames::on_click(b, button.c_str(), context, [set, slot] {
            if (g_shown.context == set) clicked(g_shown.id, slot);
        });
        ++g_shown.slots;
    }
    return true;
}

// Shows the set, with the slots this page doesn't use hidden (showing a
// frame shows all its children).
void show_set() {
    frames::set_visible(frame(g_shown.style, "Popup", g_shown.context), true);
    for (int slot = g_shown.used; slot < g_shown.slots; ++slot) {
        frames::set_visible(frame(g_shown.style, "Button", g_shown.context + 1 + slot), false);
    }
}

// Shows the front of the queue on the current screen, or hides what's
// there if the queue is empty.
void render() {
    EntryPtr shown = front();
    if (!shown) {
        hide();
        return;
    }
    const Entry& entry = *shown;
    bool in_game = false;
    void* parent = frames::popup_parent(in_game);
    if (!parent) {
        if (g_shown.id != 0 || g_shown.parent) TOME_DEBUG("Popup %u: no screen to show it on yet", entry.id);
        // Usually the set went away with its screen; if its frames outlived
        // it (the main menu's do), they mustn't keep taking the clicks.
        hide_set();
        g_shown.id = 0;  // the set went away with its screen; on_screen brings it back
        g_shown.parent = nullptr;
        return;
    }
    const Style style = in_game ? Style::Esc : Style::Glue;
    if (parent != g_shown.parent || style != g_shown.style || !frame(style, "Popup", g_shown.context)) {
        // A new screen: a new set, with contexts no live frame uses. The old
        // set may outlive its screen (the main menu's frames do): hide it, or
        // its full-screen frame takes every click there with nothing to answer them.
        hide_set();
        g_shown.parent = parent;
        g_shown.style = style;
        g_next_context += kContextsPerSet;
        g_shown.context = g_next_context;
        g_shown.slots = 0;
    }
    if (g_shown.id != entry.id) {
        g_shown.page = 0;
        TOME_INFO("Popup %u shown%s: \"%s\", %zu option%s", entry.id, in_game ? " in game" : "",
                  entry.spec.title.c_str(), entry.spec.options.size(), entry.spec.options.size() == 1 ? "" : "s");
    }
    popup::Layout l = popup::layout(entry.spec, style, g_shown.page);
    g_shown.page = l.page;
    if (!make_set(l)) {
        g_shown.id = 0;
        return;
    }
    g_shown.id = entry.id;

    const int context = g_shown.context;
    void* root = frame(style, "Popup", context);
    void* backdrop = frame(style, "Backdrop", context);
    void* title = frame(style, "Title", context);
    void* text = frame(style, "Text", context);
    // Centered on the screen, a little above the middle.
    const float half = static_cast<float>(l.width / 2);
    const float top = static_cast<float>(-std::max(0.02, (0.6 - l.height) / 2 - 0.02));
    frames::set_point(backdrop, kTopLeft, root, kTop, -half, top);
    frames::set_point(backdrop, kBottomRight, root, kTop, half, top - static_cast<float>(l.height));
    auto place = [&](void* f, double y, double height) {
        const float left = static_cast<float>(l.content_left);
        frames::set_point(f, kTopLeft, backdrop, kTopLeft, left, static_cast<float>(-y));
        frames::set_point(f, kBottomRight, backdrop, kTopLeft, left + static_cast<float>(l.content_width),
                          static_cast<float>(-(y + height)));
    };
    place(title, l.title_top, 0.02);
    place(text, l.text_top, l.text_height > 0 ? l.text_height : 0.001);
    // An empty text frame keeps its old text; a space clears it.
    frames::set_text(title, entry.spec.title.empty() ? " " : entry.spec.title.c_str());
    frames::set_text(text, entry.spec.text.empty() ? " " : entry.spec.text.c_str());

    g_shown.used = static_cast<int>(l.buttons.size());
    show_set();
    g_last_keep = GetTickCount();
    for (int slot = 0; slot < g_shown.used; ++slot) {
        void* b = frame(style, "Button", context + 1 + slot);
        const popup::Layout::Button& button = l.buttons[slot];
        frames::set_point(b, kTopLeft, backdrop, kTopLeft, static_cast<float>(button.left),
                          static_cast<float>(-button.top));
        frames::set_width(b, static_cast<float>(button.width));
        const char* label = button.option == popup::kPrevious ? "Previous"
                            : button.option == popup::kNext   ? "Next"
                                                              : entry.spec.options[button.option].c_str();
        frames::set_text(frame(style, "ButtonText", context + 1 + slot), *label ? label : " ");
    }
    // Keys go to the popup, not a text box that had them.
    frames::clear_focus();
}

void schedule_render() {
    main_thread::post(render);
}

// Removes the popup and tells its owner; then the next one shows. Main thread.
void finish(uint32_t id, int choice) {
    EntryPtr entry = take(id);
    if (!entry) return;
    if (g_shown.id == id) hide();
    if (entry->mod) {
        entry->mod->drop_revokers({reinterpret_cast<const void*>(static_cast<uintptr_t>(id))});
        TOME_INFO("[%s] popup %u: %d", entry->mod->id.c_str(), id, choice);
        if (entry->fn) {
            guard::run(entry->mod, "popup callback", nullptr,
                       [&] { entry->fn(entry->mod, id, choice, entry->user); });
        }
    } else {
        TOME_INFO("Popup %u: %d", id, choice);
        if (entry->done) entry->done(choice);
    }
    render();
}

uint32_t new_id() {
    ExclusiveLock hold(g_lock);
    uint32_t id = g_next_id++;
    if (g_next_id == 0) g_next_id = 1;
    return id;
}

// Queues `entry` (with an id from new_id() already, or 0 for a new one).
uint32_t enqueue(Entry entry) {
    if (entry.id == 0) entry.id = new_id();
    const uint32_t id = entry.id;
    {
        ExclusiveLock hold(g_lock);
        g_queue.push_back(std::make_shared<const Entry>(std::move(entry)));
    }
    schedule_render();
    return id;
}

std::string text_or_empty(const char* s) {
    return s ? s : "";
}

tome_result TOME_CALL api_popup(tome_mod* mod, const tome_popup* p, tome_popup_fn fn, void* user, uint32_t* id) {
    if (id) *id = 0;
    if (!mod || !p || p->size < offsetof(tome_popup, cancel_option) + sizeof(p->cancel_option) ||
        p->option_count < 1 || !p->options) {
        TOME_ERROR("[%s] popup: needs a tome_popup with at least one option", mod ? mod->id.c_str() : "?");
        return TOME_ERR_INVALID_ARGUMENT;
    }
    if (!frames::ready()) return TOME_ERR_UNAVAILABLE;
    Entry entry;
    entry.mod = mod;
    entry.fn = fn;
    entry.user = user;
    entry.spec.title = text_or_empty(p->title);
    entry.spec.text = text_or_empty(p->text);
    for (int32_t i = 0; i < p->option_count; ++i) entry.spec.options.push_back(text_or_empty(p->options[i]));
    entry.spec.default_option = p->default_option;
    entry.spec.cancel_option = p->cancel_option;
    std::string error = popup::validate(entry.spec);
    if (!error.empty()) {
        TOME_ERROR("[%s] popup: %s", mod->id.c_str(), error.c_str());
        return TOME_ERR_INVALID_ARGUMENT;
    }
    TOME_DEBUG("[%s] popup queued", mod->id.c_str());
    // Disabling the mod takes its popups away, without calling back. The
    // revoker comes first: once queued, the main thread may finish the popup
    // (and drop its revoker) at any moment.
    const uint32_t assigned = new_id();
    entry.id = assigned;
    mod->add_revoker(
        [assigned] {
            if (take(assigned)) schedule_render();
        },
        reinterpret_cast<const void*>(static_cast<uintptr_t>(assigned)));
    enqueue(std::move(entry));
    if (id) *id = assigned;
    return TOME_OK;
}

tome_result TOME_CALL api_popup_close(tome_mod* mod, uint32_t id) {
    if (!mod) return TOME_ERR_INVALID_ARGUMENT;
    {
        SharedLock hold(g_lock);
        auto it = find_locked(id);
        if (it == g_queue.end() || (*it)->mod != mod) return TOME_ERR_INVALID_ARGUMENT;
    }
    main_thread::post([id] { finish(id, TOME_POPUP_CLOSED); });
    return TOME_OK;
}

tome_result TOME_CALL api_restart_game(tome_mod* mod) {
    if (!main_thread::installed()) return TOME_ERR_UNAVAILABLE;
    std::string who = mod ? mod->id : "?";
    main_thread::post([who] { restart::request(who.c_str()); });
    return TOME_OK;
}

} // namespace

void fill_api(tome_api& api) {
    api.popup = api_popup;
    api.popup_close = api_popup_close;
    api.restart_game = api_restart_game;
}

uint32_t show(popup::Spec spec, std::function<void(int)> done) {
    Entry entry;
    entry.spec = std::move(spec);
    entry.done = std::move(done);
    return enqueue(std::move(entry));
}

void on_screen() {
    schedule_render();
}

void on_frame() {
    if (g_shown.id == 0 || GetTickCount() - g_last_keep < kKeepShownMs) return;
    g_last_keep = GetTickCount();
    bool in_game = false;
    if (frames::popup_parent(in_game) == g_shown.parent) show_set();
}

bool on_key(keys::Key key, bool repeat) {
    if (g_shown.id == 0) return false;
    bool in_game = false;
    if (frames::popup_parent(in_game) != g_shown.parent) return false;  // its screen went away
    EntryPtr entry = front();
    if (!entry || entry->id != g_shown.id) return false;
    popup::KeyResult r = popup::key_pressed(entry->spec, g_shown.style, g_shown.page, key);
    if (r.kind == popup::KeyResult::Pass) return false;
    if (repeat) return true;
    if (r.kind == popup::KeyResult::Pick) {
        finish(entry->id, r.option);
    } else if (r.kind == popup::KeyResult::Page) {
        g_shown.page = r.page;
        render();
    }
    return true;
}

} // namespace tome::popups
