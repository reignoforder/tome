#include "mod_menu.h"

#include "core/frames.h"
#include "core/hash.h"
#include "core/main_menu_fdf.h"
#include "core/mod_menu_model.h"
#include "core/popup.h"
#include "core/tome_settings.h"
#include "frames.h"
#include "hooks.h"
#include "keybinds.h"
#include "log.h"
#include "main_thread.h"
#include "mod_manager.h"
#include "overlay.h"
#include "patch.h"
#include "popups.h"
#include "restart.h"
#include "tome_settings.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <set>
#include <utility>
#include <vector>

// GCC warns that __thiscall is meant for class methods, but it does apply it
// to free functions and function pointer types (ecx = this, callee pops the
// stack), which is what calling into and hooking the game's methods needs.
#pragma GCC diagnostic ignored "-Wattributes"

namespace tome::mod_menu {
namespace {

using LoadTocFn = uint32_t(__fastcall*)(const char* toc, void* context);
using BindControlFn = void*(__thiscall*)(void* menu, const char* name, int command, int enable);
using MenuEventFn = int(__thiscall*)(void* menu, void* event);
using MenuSetupFn = int(__thiscall*)(void* menu, int a, int b);
using SetPanelFn = void(__thiscall*)(void* esc_panel, int panel);
using GameUiGetFn = void*(__fastcall*)(int create, int forget);
using SetModeFn = void(__thiscall*)(void* game_ui, void* mode, int push);
using EscMenuResetFn = void(__thiscall*)(void* esc_menu);

using ui::MenuStyle;

// The main menu's Mods button reports to CMainMenu's event handler with this
// command id; CMainMenu's own handler ignores ids above 0x21. Everything in
// the panels reports to Tome's observer instead (frames::on_click).
constexpr int kCmdOpen = 0x7100;

LoadTocFn g_load_toc = nullptr;
const BuildMappings* g_build = nullptr;
BindControlFn g_bind_control = nullptr;
MenuEventFn g_menu_event = nullptr;
MenuSetupFn g_menu_setup = nullptr;
SetPanelFn g_set_panel = nullptr;
GameUiGetFn g_game_ui_get = nullptr;
SetModeFn g_set_mode = nullptr;
EscMenuResetFn g_esc_menu_reset = nullptr;

bool g_overlay_ready = false;

// One menu: the main menu's panel or the Esc menu's.
struct Panel {
    MenuStyle style;
    bool open = false;  // its content is showing
    menu::State state;
    // Icon frames made on this screen, as (definition, context); new screens start empty.
    std::vector<std::pair<std::string, int>> icons;
    std::string shown;  // the page last logged ("list", "settings hello", ...)
};
Panel g_glue{MenuStyle::Glue, false, {}, {}, {}};
Panel g_esc{MenuStyle::Esc, false, {}, {}, {}};

void* g_esc_menu = nullptr;  // the CEscMenuMainPanel from the last Setup
bool g_esc_opening = false;  // Tome's own SetPanel(help) call is running

// --- frames ------------------------------------------------------------------

void* frame(const std::string& name) {
    return frames::get(name.c_str());
}

void show(const std::string& name, bool visible) {
    frames::set_visible(frame(name), visible);
}

void set_text(const std::string& name, const std::string& text) {
    void* f = frame(name);
    // An empty text frame keeps its old text in the game; a space clears it.
    if (f && !frames::set_text(f, text.empty() ? " " : text.c_str())) {
        TOME_WARN("Mod menu: %s isn't a text frame", name.c_str());
    }
}

enum FramePoint { kTop = 1, kTopRight = 2, kBottom = 7, kBottomRight = 8 };

bool set_point(const char* name, int point, const char* relative, int relative_point, float x, float y) {
    return frames::set_point(frame(name), point, frame(relative), relative_point, x, y);
}

// --- pages -------------------------------------------------------------------

menu::Data gather() {
    menu::Data data;
    data.mods = mod_manager().status();
    data.keybinds = keybinds::list();
    data.tome.version = TOME_VERSION;
    data.tome.game_build = g_build->data->name + " (" + g_build->data->file_version + ")";
    data.tome.mods_dir = narrow(mod_manager().paths().mods_dir);
    data.tome.safe_mode = mod_manager().safe_mode();
    data.tome.settings = tome_settings::schema();
    data.tome.settings_values = tome_settings::values();
    return data;
}

// --- icons -------------------------------------------------------------------
// A mod's icon (tome.mod.json "icon", a .blp or .tga) goes into the generated
// overlay with a BACKDROP definition of its own, named after the file's
// contents: definitions can't be replaced, so a changed icon gets a new name.

struct IconFile {
    FileStamp stamp;
    std::string definition;  // "" if it can't be shown
};
std::map<std::string, IconFile> g_icon_files;  // by mod folder
std::set<std::string> g_icon_definitions;     // loaded this session

// The icon's frame definition, loading it the first time; "" if the mod has
// no icon Tome can show (logged once per file version).
std::string icon_definition(const ModStatus& m) {
    std::wstring icon_path = widen(m.icon);
    std::replace(icon_path.begin(), icon_path.end(), L'/', L'\\');
    std::wstring path = widen(m.dir) + L"\\" + icon_path;
    FileStamp stamp;
    if (!file_stamp(path, stamp)) return {};
    IconFile& cached = g_icon_files[m.folder];
    if (cached.stamp == stamp) return cached.definition;
    cached = {stamp, ""};

    std::string ext = m.icon.size() > 4 ? m.icon.substr(m.icon.size() - 4) : "";
    for (char& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (ext != ".blp" && ext != ".tga") {
        TOME_WARN("%s: icon %s: the game shows .blp and .tga images only", m.folder.c_str(), m.icon.c_str());
        return {};
    }
    std::string data;
    if (!read_file(path, data) || data.empty() || data.size() >= (4u << 20)) {
        TOME_WARN("%s: can't read icon %s", m.folder.c_str(), m.icon.c_str());
        return {};
    }
    char id[20];
    snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(fnv1a64(data)));
    std::string definition = std::string("TomeIcon") + id;
    if (!g_icon_definitions.count(definition)) {
        std::string texture = std::string("UI\\Tome\\Icons\\") + id + ext;
        std::string fdf = std::string("UI\\FrameDef\\Tome\\Icons\\") + id + ".fdf";
        std::string toc = fdf + ".toc";
        if (!overlay::put_generated({{texture, data}, {fdf, ui::icon_fdf(definition, texture)}, {toc, ui::toc_for(fdf)}}) ||
            !frames::load_toc(toc.c_str())) {
            TOME_WARN("%s: icon %s couldn't be loaded", m.folder.c_str(), m.icon.c_str());
            return {};
        }
        g_icon_definitions.insert(definition);
    }
    TOME_INFO("%s: icon %s loaded", m.folder.c_str(), m.icon.c_str());
    cached.definition = definition;
    return definition;
}

// The icon frame for `row` showing `definition`, made on first use (each
// row has its own context, so two rows can show the same icon).
void* icon_frame(Panel& p, const std::string& definition, int row) {
    int context = 1 + row;
    void* f = frames::get(definition.c_str(), context);
    if (f) return f;
    void* parent = frame(ui::menu_frame(p.style, p.style == MenuStyle::Glue ? "Content" : "Panel"));
    if (!parent) return nullptr;
    f = frames::create(definition.c_str(), parent, 0, context);
    if (f) p.icons.emplace_back(definition, context);
    return f;
}

// --- text boxes ----------------------------------------------------------------

// Carries out a SetSetting or SetTomeSetting; false for any other action.
bool store_setting(const menu::Action& action) {
    std::string error;
    bool ok;
    if (action.kind == menu::Action::SetSetting) {
        ok = mod_manager().set_setting(action.mod, action.key, action.value, error) == TOME_OK;
    } else if (action.kind == menu::Action::SetTomeSetting) {
        ok = tome_settings::set(action.key, action.value, error);
    } else {
        return false;
    }
    if (!ok) TOME_WARN("Mod menu: %s", error.c_str());
    return true;
}

// Saves what the player typed into the page's text boxes.
void save_edits(Panel& p) {
    if (!p.open || !menu::is_settings_page(p.state.page)) return;
    const int rows = ui::menu_rows(p.style);
    menu::Data data = gather();
    menu::View view = menu::build(p.state, data, rows);
    for (int row = 0; row < static_cast<int>(view.rows.size()); ++row) {
        if (!view.rows[row].editable) continue;
        std::string text;
        if (!frames::edit_text(frame(ui::menu_row_frame(p.style, row, "Edit")), text)) continue;
        store_setting(menu::edit_row(p.state, data, rows, row, text));
    }
}

const char* page_name(menu::Page page) {
    switch (page) {
    case menu::Page::List: return "list";
    case menu::Page::Details: return "details";
    case menu::Page::Settings: return "settings";
    case menu::Page::Tome: return "Tome";
    case menu::Page::TomeSettings: return "Tome settings";
    case menu::Page::Keybinds: return "keybinds";
    }
    return "?";
}

// "Mod menu page: settings hello", once per change: what a click or a key
// did, for the log (and the e2e test, which waits for it before the next).
void log_page(Panel& p) {
    std::string shown = page_name(p.state.page);
    if (!p.state.mod.empty() && (p.state.page == menu::Page::Details || p.state.page == menu::Page::Settings)) {
        shown += " " + p.state.mod;
    }
    if (p.state.capture >= 0) shown += " (waiting for a key for " + p.state.capture_mod + "/" + p.state.capture_id + ")";
    if (shown == p.shown) return;
    p.shown = shown;
    TOME_INFO("Mod menu page: %s", shown.c_str());
}

// Fills the panel's frames from the current page.
void render(Panel& p) {
    const MenuStyle style = p.style;
    if (!frame(ui::menu_frame(style, "Panel"))) return;
    const int rows = ui::menu_rows(style);
    menu::Data data = gather();
    menu::clamp(p.state, data, rows);
    log_page(p);  // after clamp: the page it shows (a mod without settings shows its details)
    menu::View view = menu::build(p.state, data, rows);

    set_text(ui::menu_frame(style, "Title"), view.title);
    set_text(ui::menu_frame(style, "Summary"), view.summary);
    bool body = view.rows.empty();
    show(ui::menu_frame(style, "Body"), body);
    if (body) set_text(ui::menu_frame(style, "Body"), view.body);
    for (const auto& [definition, context] : p.icons) frames::set_visible(frames::get(definition.c_str(), context), false);
    const ui::RowGeometry geometry = ui::row_geometry(style);
    void* panel = frame(ui::menu_frame(style, "Panel"));
    void* focused = frames::focused();
    for (int row = 0; row < rows; ++row) {
        const menu::Row* r = !body && row < static_cast<int>(view.rows.size()) ? &view.rows[row] : nullptr;
        const std::string name = ui::menu_row_frame(style, row, "Name");
        show(name, r != nullptr);
        show(ui::menu_row_frame(style, row, "Info"), r != nullptr);
        show(ui::menu_row_frame(style, row, "Small"), r && !r->small.empty());
        show(ui::menu_row_frame(style, row, "Button"), r && !r->button.empty());
        const std::string edit = ui::menu_row_frame(style, row, "Edit");
        show(edit, r && r->editable);
        if (!r) continue;

        // An icon left of the name, which moves right to make room.
        const float y = static_cast<float>(geometry.top - row * geometry.step);
        std::string icon;
        if (!r->icon.empty()) {
            for (const ModStatus& m : data.mods) {
                if (m.folder == r->icon) icon = icon_definition(m);
            }
        }
        void* icon_f = icon.empty() ? nullptr : icon_frame(p, icon, row);
        float shift = icon_f ? static_cast<float>(ui::kIconSize + ui::kIconGap) : 0.0f;
        if (icon_f) {
            frames::set_point(icon_f, 0, panel, 0, static_cast<float>(geometry.left), y + 0.002f);
            frames::set_visible(icon_f, true);
        }
        frames::set_point(frame(name), 0, panel, 0, static_cast<float>(geometry.left) + shift, y);
        frames::set_width(frame(name), static_cast<float>(geometry.name_width) - shift);
        frames::set_width(frame(ui::menu_row_frame(style, row, "Info")), static_cast<float>(geometry.name_width) - shift);
        if (r->editable) {
            // Leave the box alone while the player types in it.
            std::string current;
            void* box = frame(edit);
            if (box != focused && frames::edit_text(box, current) && current != r->edit) {
                frames::set_edit_text(box, r->edit.c_str());
            }
        }
        set_text(ui::menu_row_frame(style, row, "Name"), r->name);
        set_text(ui::menu_row_frame(style, row, "Info"), r->info);
        if (!r->small.empty()) set_text(ui::menu_row_frame(style, row, "SmallText"), r->small);
        if (!r->button.empty()) set_text(ui::menu_row_frame(style, row, "ButtonText"), r->button);
    }
    for (int i = 0; i < menu::kFooterButtons; ++i) {
        show(ui::menu_footer_frame(style, i), !view.footer[i].empty());
        if (!view.footer[i].empty()) set_text(ui::menu_footer_frame(style, i, true), view.footer[i]);
    }
}

void close_panel(Panel& p);

void open_mods_folder() {
    std::wstring dir = mod_manager().paths().mods_dir;
    auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr,
                                                          SW_SHOWNORMAL));
    if (result <= 32) TOME_WARN("Mod menu: couldn't open the mods folder (%ld)", static_cast<long>(result));
}

// A toggle left changes waiting for a restart: offer one.
void offer_restart(const std::vector<ModStatus>& before, bool in_game) {
    std::vector<std::string> names = menu::newly_restart_pending(before, mod_manager().status());
    if (names.empty()) return;
    popups::show(menu::restart_prompt(names, in_game), [](int choice) {
        if (choice == menu::kRestartNow) restart::request("the mod menu");
    });
}

// Carries out what a click or key asked for, then shows the result.
void apply(Panel& p, const menu::Action& action) {
    switch (action.kind) {
    case menu::Action::None: break;
    case menu::Action::Close:
        close_panel(p);  // saves the text boxes
        return;
    case menu::Action::SetEnabled: {
        TOME_INFO("Mod menu: %s %s", action.flag ? "enabling" : "disabling", action.mod.c_str());
        std::vector<ModStatus> before = mod_manager().status();
        mod_manager().set_enabled(action.mod, action.flag);
        offer_restart(before, p.style == MenuStyle::Esc);
        break;
    }
    case menu::Action::SetSetting:
    case menu::Action::SetTomeSetting: store_setting(action); break;
    case menu::Action::ResetSettings: mod_manager().reset_settings(action.mod); break;
    case menu::Action::SetKey: keybinds::set(action.mod, action.key, action.key_value); break;
    case menu::Action::ResetKeys: keybinds::reset_all(); break;
    case menu::Action::SetSafeMode: mod_manager().set_safe_mode_now(action.flag); break;
    case menu::Action::OpenModsFolder: open_mods_folder(); break;
    }
    if (p.open) render(p);
}

// Registers the clicks of a panel's buttons (each time its frames are new).
void bind_panel(Panel& p) {
    const MenuStyle style = p.style;
    Panel* panel = &p;
    auto bind = [](const std::string& name, std::function<void()> fn) {
        if (!frames::on_click(frame(name), name.c_str(), 0, std::move(fn))) {
            TOME_WARN("Mod menu: button %s not found", name.c_str());
        }
    };
    const int rows = ui::menu_rows(style);
    for (int row = 0; row < rows; ++row) {
        for (bool small : {true, false}) {
            bind(ui::menu_row_frame(style, row, small ? "Small" : "Button"), [panel, row, small, rows] {
                save_edits(*panel);
                menu::Data data = gather();
                apply(*panel, menu::click_row(panel->state, data, rows, row, small));
            });
        }
    }
    for (int i = 0; i < menu::kFooterButtons; ++i) {
        bind(ui::menu_footer_frame(style, i), [panel, i, rows] {
            save_edits(*panel);
            menu::Data data = gather();
            apply(*panel, menu::click_footer(panel->state, data, rows, i));
        });
    }
}

// Starts each opening on the mod list, where it left off.
void reset_page(Panel& p) {
    int list_first = p.state.list_first;
    p.state = {};
    p.shown.clear();
    p.state.list_first = list_first;
}

// --- main menu ---------------------------------------------------------------

// Poses the chained panel model behind Tome's panel.
void animate_chains(std::initializer_list<int> tags) {
    std::vector<int> list(tags);
    if (!frames::set_animation(frame("TomeModsChains"), list.data(), static_cast<int>(list.size()))) {
        TOME_WARN("Mod menu: chains model not loaded");
    }
}

// Timings of the panel model's "Options Morph" and "Options Morph Alternate"
// sequences (TopLeftPanel-Expansion.mdx: 1000 ms and 667 ms).
constexpr unsigned kLowerMs = 1000;
constexpr unsigned kRaiseMs = 700;
unsigned g_sequence = 0;  // invalidates pending animation steps on every click

int tag(uint32_t id) {
    return static_cast<int>(id);
}

void open_glue_panel() {
    const auto& ui = g_build->ui;
    unsigned sequence = ++g_sequence;
    g_glue.open = true;
    reset_page(g_glue);
    show("WarCraftIIILogo", false);  // the Options screen hides it too
    // Show() also shows every child, so show the panel first, then hide the content.
    show("TomeModsPanel", true);
    show("TomeModsContent", false);
    animate_chains({tag(ui.anim_tag_morph), tag(ui.anim_tag_options)});
    main_thread::post_delayed(kLowerMs, [sequence] {
        if (sequence != g_sequence || !g_glue.open) return;
        const auto& ui = g_build->ui;
        animate_chains({tag(ui.anim_tag_stand), tag(ui.anim_tag_options), tag(ui.anim_tag_alternate)});
        show("TomeModsContent", true);
        render(g_glue);
    });
    TOME_INFO("Mod menu opened");
}

void close_glue_panel() {
    const auto& ui = g_build->ui;
    unsigned sequence = ++g_sequence;
    save_edits(g_glue);
    g_glue.open = false;
    g_glue.state.capture = -1;
    show("TomeModsContent", false);
    animate_chains({tag(ui.anim_tag_morph), tag(ui.anim_tag_options), tag(ui.anim_tag_alternate)});
    main_thread::post_delayed(kRaiseMs, [sequence] {
        if (sequence != g_sequence || g_glue.open) return;
        show("TomeModsPanel", false);
        show("WarCraftIIILogo", true);
    });
    TOME_INFO("Mod menu closed");
}

// --- Esc menu ----------------------------------------------------------------

// The Mods button: the Esc menu's own Help panel is switched to (which sizes
// the backdrop), and Tome's panel shown in its place.
void open_esc_panel() {
    if (!g_esc_menu) return;
    reset_page(g_esc);
    g_esc_opening = true;
    g_set_panel(g_esc_menu, static_cast<int>(g_build->ui.esc_panel_help));
    g_esc_opening = false;
}

// CEscMenuMainPanel::SetPanel, after the game's code: shows Tome's panel if
// Tome asked for it, and hides it whenever the game switches panels itself
// (Back, Esc, reopening the menu).
void TOME_CALL after_set_panel(tome_mod*, tome_hook_call*, void*) {
    if (g_esc_opening) {
        show("HelpPanel", false);
        show(ui::menu_frame(MenuStyle::Esc, "Panel"), true);
        g_esc.open = true;
        render(g_esc);
        TOME_INFO("Mod menu opened in game");
    } else if (g_esc.open) {
        save_edits(g_esc);
        show(ui::menu_frame(MenuStyle::Esc, "Panel"), false);
        g_esc.open = false;
        g_esc.state.capture = -1;
        TOME_INFO("Mod menu closed in game");
    }
}

// CEscMenuMainPanel::Setup, after the game's code: the Esc menu was built
// (once per game). Adds the Mods button beside Options and Tome's panel.
void TOME_CALL after_esc_setup(tome_mod*, tome_hook_call* call, void*) {
    if (!g_overlay_ready) return;
    void* menu = reinterpret_cast<void*>(static_cast<uintptr_t>(call->args[0]));
    void* inside = frame("InsideMainPanel");
    void* options = frame("OptionsButton");
    if (!inside || !options || !frame("LoadGameButton") || !frame("TipsButton") || !frame("EndGameButton") ||
        !frame("HelpPanel")) {
        TOME_WARN("Mod menu: the Esc menu doesn't have the frames Tome expects; no Mods button in games");
        return;
    }
    const std::string panel_name = ui::menu_frame(MenuStyle::Esc, "Panel");
    // Two frames with one name crash the game, so never create one twice.
    if (!frame(ui::kEscModsButtonFrame)) frames::create(ui::kEscModsButtonFrame, inside, 0, 0);
    if (!frame(panel_name)) frames::create(panel_name.c_str(), menu, 0, 0);
    if (!frame(ui::kEscModsButtonFrame) || !frame(panel_name)) {
        TOME_WARN("Mod menu: couldn't create the Esc menu's frames");
        return;
    }
    g_esc_menu = menu;
    g_esc.open = false;
    g_esc.icons.clear();
    show(panel_name, false);

    // Options and Mods share the row like Help and Tips do below them; Tips
    // keeps its right edge under Mods.
    constexpr float kHalf = 0.059f;  // (0.228 - 0.11) / 2: from the row's center to each half's
    frames::set_width(options, 0.11f);
    set_point("OptionsButton", kTop, "LoadGameButton", kBottom, -kHalf, -0.002f);
    set_point(ui::kEscModsButtonFrame, kTop, "LoadGameButton", kBottom, kHalf, -0.002f);
    set_point("TipsButton", kTopRight, ui::kEscModsButtonFrame, kBottomRight, 0.0f, -0.002f);
    // End Game hung under Options' center: keep it centered, as far below.
    set_point("EndGameButton", kTop, "LoadGameButton", kBottom, 0.0f, -(0.002f + 0.035f + 0.039f));

    frames::on_click(frame(ui::kEscModsButtonFrame), ui::kEscModsButtonFrame, 0, open_esc_panel);
    bind_panel(g_esc);
    TOME_INFO("Esc menu set up; Mods button added");
}

// The game's Esc menu, if its main panel is the one Tome set up (the game in
// progress), else null; `game_ui` gets the CGameUI.
void* current_esc_menu(void** game_ui = nullptr) {
    if (!g_esc_menu) return nullptr;
    void* ui_root = g_game_ui_get(0, 0);
    if (!ui_root) return nullptr;
    const auto& ui = g_build->ui;
    void* esc_menu = *reinterpret_cast<void**>(static_cast<char*>(ui_root) + ui.game_ui_esc_menu_offset);
    if (!esc_menu || *reinterpret_cast<void**>(static_cast<char*>(esc_menu) + ui.esc_menu_main_panel_offset) !=
                         g_esc_menu) {
        return nullptr;
    }
    if (game_ui) *game_ui = ui_root;
    return esc_menu;
}

// Whether the panel's screen is still there. A panel left open when its
// screen went away (a game ended with the Esc menu up, the main menu was
// left) is marked closed, so it doesn't take keys or touch freed frames.
bool panel_live(Panel& p) {
    bool live = p.style == MenuStyle::Glue ? frames::main_menu_showing() && frame("TomeModsPanel")
                                           : current_esc_menu() != nullptr;
    if (!live && p.open) {
        p.open = false;
        p.state.capture = -1;
    }
    return live;
}

void close_panel(Panel& p) {
    if (p.style == MenuStyle::Glue) {
        close_glue_panel();
    } else if (current_esc_menu()) {
        g_set_panel(g_esc_menu, static_cast<int>(g_build->ui.esc_panel_main));
    }
}

// --- hooks -------------------------------------------------------------------

void build_overlay() {
    g_overlay_ready = overlay::put_generated({
        {ui::kModMenuFdfPath, ui::mod_menu_fdf()},
        {popup::kFdfPath, popup::popup_fdf()},
        {"UI\\FrameDef\\Tome.toc",
         std::string(ui::kModMenuFdfPath) + "\r\n" + popup::kFdfPath + "\r\n\r\n"},
    });
    if (!g_overlay_ready) TOME_ERROR("Mod menu: the overlay archive couldn't be mounted");
}

bool g_overlay_attempted = false;
bool g_definitions_loaded = false;

// Parses Tome's FDF on top of the game's definitions, without clearing them.
void load_tome_definitions() {
    if (!g_overlay_ready) return;
    g_definitions_loaded = frames::load_toc("UI\\FrameDef\\Tome.toc");
    if (!g_definitions_loaded) TOME_ERROR("Mod menu: the game couldn't load Tome.toc");
}

// Only reached if Tome was loaded before the game read FrameDef.toc.
uint32_t __fastcall hooked_load_toc(const char* toc, void* context) {
    if (!g_overlay_attempted) {
        g_overlay_attempted = true;
        build_overlay();
    }
    uint32_t result = g_load_toc(toc, context);
    load_tome_definitions();
    return result;
}

// Developer aid for screenshots and tests (TOME_DEBUG_OPEN_MOD_MENU=1): opens
// the panel once the main menu has finished appearing.
DWORD WINAPI debug_open_later(LPVOID) {
    Sleep(3000);
    main_thread::post([] {
        if (!g_glue.open) open_glue_panel();
    });
    return 0;
}

void add_mod_menu(void* menu) {
    g_glue.open = false;
    g_glue.icons.clear();
    ++g_sequence;
    if (!g_definitions_loaded) {
        return;
    }
    void* layer = frame("ControlLayer");
    if (!layer) {
        TOME_WARN("Mod menu: the main menu has no ControlLayer frame");
        return;
    }
    // The menu owns its children; Tome's frames go away with it.
    frames::create(ui::kModsButtonFrame, layer, 0, 0);
    frames::create(ui::menu_frame(MenuStyle::Glue, "Panel").c_str(), layer, 0, 0);
    if (!frame("TomeModsButton")) {
        TOME_WARN("Mod menu: couldn't create Tome's frames");
        return;
    }
    TOME_INFO("Main menu set up; Mods button added (thread %lu)", GetCurrentThreadId());
    g_bind_control(menu, "TomeModsButton", kCmdOpen, 1);
    bind_panel(g_glue);
    show("TomeModsPanel", false);

    // Share the bottom box with Quit like OK and Cancel do on the Options
    // screen: Mods takes the upper slot (in the FDF), Quit moves to the lower.
    if (set_point("ExitBackdrop", kBottomRight, "MainMenuFrame", kBottomRight, -0.015f, 0.0225f)) {
        TOME_INFO("Main menu: Quit moved to the lower slot of its box");
    }

    // Developer aid for screenshots and tests: open the panel right away.
    char debug[8];
    // Waits until the menu has finished appearing, which overrides visibility set during setup.
    if (TOME_DEV && GetEnvironmentVariableA("TOME_DEBUG_OPEN_MOD_MENU", debug, sizeof(debug)) && debug[0] == '1') {
        HANDLE t = CreateThread(nullptr, 0, debug_open_later, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
}

int __thiscall hooked_menu_setup(void* menu, int a, int b) {
    int result = g_menu_setup(menu, a, b);
    add_mod_menu(menu);
    frames::on_main_menu_setup(menu);
    // No map is loaded now, so asset changes that waited for one to end apply.
    mod_manager().on_main_menu();
    return result;
}

int __thiscall hooked_menu_event(void* menu, void* event) {
    int command = *reinterpret_cast<int*>(static_cast<char*>(event) + 8);
    if (command != kCmdOpen) return g_menu_event(menu, event);
    g_glue.open ? close_glue_panel() : open_glue_panel();
    return 1;
}

} // namespace

void on_mods_changed() {
    if (g_glue.open && panel_live(g_glue)) render(g_glue);
    if (g_esc.open && panel_live(g_esc)) render(g_esc);
}

bool on_key(keys::Key key, bool repeat) {
    constexpr uint8_t kVkEscape = 0x1B, kVkReturn = 0x0D;
    for (Panel* p : {&g_glue, &g_esc}) {
        if (!p->open || !panel_live(*p)) continue;
        if (p->state.capture >= 0) {
            if (repeat) return true;
            menu::Action action;
            if (!menu::key_pressed(p->state, gather(), key, action)) continue;
            apply(*p, action);  // renders the result
            return true;
        }
        if (key.mods != 0) continue;
        if (key.vk == kVkEscape) {
            if (repeat) return true;
            save_edits(*p);
            frames::clear_focus();
            menu::Action action = menu::back(p->state);
            if (action.kind == menu::Action::Close) {
                close_panel(*p);
            } else {
                render(*p);
            }
            return true;
        }
        if (key.vk == kVkReturn) {
            void* focused = frames::focused();
            for (int row = 0; focused && row < ui::menu_rows(p->style); ++row) {
                if (focused != frame(ui::menu_row_frame(p->style, row, "Edit"))) continue;
                save_edits(*p);
                frames::clear_focus();
                render(*p);
                return true;
            }
        }
    }
    return false;
}

bool open_from_key() {
    if (frames::main_menu_showing() && frame("TomeModsPanel")) {
        g_glue.open ? close_glue_panel() : open_glue_panel();
        return true;
    }
    void* game_ui = nullptr;
    void* esc_menu = current_esc_menu(&game_ui);
    if (!esc_menu) return false;
    if (g_esc.open) return true;
    // What F10 does, then Mods.
    if (*reinterpret_cast<void**>(static_cast<char*>(game_ui) + g_build->ui.game_ui_mode_offset) != esc_menu) {
        g_esc_menu_reset(esc_menu);
        g_set_mode(game_ui, esc_menu, 1);
    }
    open_esc_panel();
    return true;
}

void reload_frame_definitions(std::string& report) {
    if (g_overlay_attempted) {
        report = "FrameDef.toc not loaded yet; the overlay will mount when it is";
        return;
    }
    g_overlay_attempted = true;
    build_overlay();
    if (!g_overlay_ready) {
        report = "overlay not mounted; see errors above";
        return;
    }
    // Reloading all of FrameDef.toc instead would clear tables that frames the
    // game already created still point into (crashes on the next map load).
    load_tome_definitions();
    report = g_definitions_loaded ? "frame definitions loaded" : "frame definitions failed to load";
}

bool install_hooks(uintptr_t base, const BuildMappings& build, std::string& report) {
    const auto& ui = build.ui;
    g_load_toc = reinterpret_cast<LoadTocFn>(base + ui.load_framedef_toc);
    g_build = &build;
    g_bind_control = reinterpret_cast<BindControlFn>(base + ui.glue_bind_control);
    g_set_panel = reinterpret_cast<SetPanelFn>(base + ui.esc_menu_set_panel);
    g_game_ui_get = reinterpret_cast<GameUiGetFn>(base + build.game.game_ui_get);
    g_set_mode = reinterpret_cast<SetModeFn>(base + ui.game_ui_set_mode);
    g_esc_menu_reset = reinterpret_cast<EscMenuResetFn>(base + ui.esc_menu_reset);

    auto vtable = reinterpret_cast<uintptr_t*>(base + ui.main_menu_vtable);
    const uint32_t event_index = ui.main_menu_event_offset / sizeof(uintptr_t);
    const uint32_t setup_index = ui.main_menu_setup_offset / sizeof(uintptr_t);
    uintptr_t event_slot = reinterpret_cast<uintptr_t>(&vtable[event_index]);
    uintptr_t setup_slot = reinterpret_cast<uintptr_t>(&vtable[setup_index]);
    g_menu_event = reinterpret_cast<MenuEventFn>(base + ui.main_menu_event_fn);
    g_menu_setup = reinterpret_cast<MenuSetupFn>(base + ui.main_menu_setup_fn);

    // Check every target before patching anything, so a mismatch leaves the game untouched.
    if (vtable[event_index] != base + ui.main_menu_event_fn ||
        vtable[setup_index] != base + ui.main_menu_setup_fn) {
        report = "CMainMenu vtable doesn't match the mappings";
        return false;
    }
    std::string error;
    if (!patch_call(base + ui.load_framedef_toc_call, base + ui.load_framedef_toc,
                    reinterpret_cast<const void*>(&hooked_load_toc), error)) {
        report = "FrameDef.toc hook: " + error;
        return false;
    }
    if (!patch_pointer(event_slot, base + ui.main_menu_event_fn, reinterpret_cast<const void*>(&hooked_menu_event),
                       error) ||
        !patch_pointer(setup_slot, base + ui.main_menu_setup_fn, reinterpret_cast<const void*>(&hooked_menu_setup),
                       error)) {
        report = "CMainMenu vtable: " + error;
        return false;
    }
    report = "hooked FrameDef.toc loading and CMainMenu";
    return true;
}

bool install_game_hooks(std::string& report) {
    struct Spec {
        const char* name;
        tome_hook_fn fn;
    };
    const Spec specs[] = {
        {"CEscMenuMainPanel::Setup", after_esc_setup},
        {"CEscMenuMainPanel::SetPanel", after_set_panel},
    };
    hooks::Target* targets[std::size(specs)];
    for (size_t i = 0; i < std::size(specs); ++i) {
        std::string error;
        targets[i] = hooks::find_or_install(specs[i].name, error);
        if (!targets[i]) {
            report = std::string(specs[i].name) + ": " + error;
            return false;
        }
    }
    for (size_t i = 0; i < std::size(specs); ++i) {
        hooks::Handler handler;
        handler.phase = TOME_HOOK_AFTER;
        handler.priority = INT32_MIN;  // after mods' handlers
        handler.fn = specs[i].fn;
        hooks::add(targets[i], handler);
    }
    report = "hooked the Esc menu";
    return true;
}

} // namespace tome::mod_menu
