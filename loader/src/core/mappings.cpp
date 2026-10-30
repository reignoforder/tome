#include "mappings.h"

#include <cmath>

namespace tome::mappings {
namespace {

// At least room for an inline hook's `jmp rel32`; at most what's worth checking.
constexpr size_t kMinBytes = 5;
constexpr size_t kMaxBytes = 64;

struct KindName {
    Kind kind;
    const char* name;
};
constexpr KindName kKinds[] = {
    {Kind::Function, "function"}, {Kind::Data, "data"},         {Kind::Call, "call"},
    {Kind::Offset, "offset"},     {Kind::Constant, "constant"}, {Kind::Import, "import"},
};

struct ConventionName {
    Convention convention;
    const char* name;
};
constexpr ConventionName kConventions[] = {
    {Convention::Cdecl, "cdecl"},
    {Convention::Stdcall, "stdcall"},
    {Convention::Fastcall, "fastcall"},
    {Convention::Thiscall, "thiscall"},
};

// A JSON integer, or a string "0x..." (JSON has no hex literals).
bool parse_u32(const json::Value& v, uint32_t& out) {
    if (v.is_number()) {
        double n = v.as_number();
        if (n < 0 || n > 0xFFFFFFFFu || std::floor(n) != n) return false;
        out = static_cast<uint32_t>(n);
        return true;
    }
    if (!v.is_string()) return false;
    const std::string& s = v.as_string();
    if (s.size() < 3 || s.size() > 10 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
    uint32_t value = 0;
    for (size_t i = 2; i < s.size(); ++i) {
        char c = s[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        value = value << 4 | digit;
    }
    out = value;
    return true;
}

bool is_lower_hex(const std::string& s) {
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Letters, digits and _ . : (e.g. "CLayoutFrame::SetPoint", "AnimTag.stand").
bool is_valid_name(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    for (char c : s) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                  c == '.' || c == ':';
        if (!ok) return false;
    }
    return true;
}

class Parser {
public:
    explicit Parser(Build& out) : out_(out) {}

    void error(std::string message) { errors_.push_back(std::move(message)); }

    const std::string* string_field(const json::Value& obj, const char* key, const std::string& where,
                                    bool required) {
        const json::Value* v = obj.find(key);
        if (!v) {
            if (required) error(where + ": missing \"" + key + "\"");
            return nullptr;
        }
        if (!v->is_string()) {
            error(where + ": \"" + key + "\" must be a string");
            return nullptr;
        }
        return &v->as_string();
    }

    bool u32_field(const json::Value& obj, const char* key, const std::string& where, uint32_t& out) {
        const json::Value* v = obj.find(key);
        if (!v) {
            error(where + ": missing \"" + key + "\"");
            return false;
        }
        if (!parse_u32(*v, out)) {
            error(where + ": \"" + key + "\" must be a 32-bit unsigned integer or a \"0x...\" string");
            return false;
        }
        return true;
    }

    void symbol(const std::string& name, const json::Value& entry) {
        const std::string where = "symbol \"" + name + "\"";
        if (!is_valid_name(name)) {
            error(where + ": names use letters, digits, '_', '.' and ':'");
            return;
        }
        if (out_.find(name)) {
            error(where + ": defined twice");
            return;
        }
        if (!entry.is_object()) {
            error(where + ": must be an object");
            return;
        }

        Symbol sym;
        sym.name = name;
        const std::string* kind = string_field(entry, "kind", where, true);
        if (!kind) return;
        bool known_kind = false;
        for (const KindName& k : kKinds) {
            if (*kind == k.name) {
                sym.kind = k.kind;
                known_kind = true;
            }
        }
        if (!known_kind) {
            error(where + ": unknown kind \"" + *kind + "\"");
            return;
        }

        static const char* const kAllowed[] = {"kind", "rva", "value", "conv", "sig", "bytes", "target", "doc"};
        for (const auto& [key, value] : entry.members()) {
            bool allowed = false;
            for (const char* a : kAllowed) allowed = allowed || key == a;
            if (!allowed) error(where + ": unknown field \"" + key + "\"");
        }

        if (has_rva(sym.kind)) {
            if (entry.find("value")) error(where + ": a " + *kind + " has an \"rva\", not a \"value\"");
            if (!u32_field(entry, "rva", where, sym.value)) return;
            if (sym.value == 0 || (out_.size_of_image && sym.value >= out_.size_of_image)) {
                error(where + ": rva is outside the image");
            }
        } else {
            if (entry.find("rva")) error(where + ": a " + *kind + " has a \"value\", not an \"rva\"");
            if (!u32_field(entry, "value", where, sym.value)) return;
        }

        if (sym.kind == Kind::Function || sym.kind == Kind::Import) {
            if (const std::string* conv = string_field(entry, "conv", where, true)) {
                for (const ConventionName& c : kConventions) {
                    if (*conv == c.name) sym.convention = c.convention;
                }
                if (sym.convention == Convention::None) error(where + ": unknown calling convention \"" + *conv + "\"");
            }
            if (const std::string* sig = string_field(entry, "sig", where, true)) sym.signature = *sig;
        }
        if (sym.kind == Kind::Import) {
            if (entry.find("bytes")) error(where + ": only functions have \"bytes\"");
        } else if (sym.kind == Kind::Function) {
            if (const std::string* bytes = string_field(entry, "bytes", where, true)) {
                if (!parse_bytes(*bytes, sym.bytes)) {
                    error(where + ": \"bytes\" must be hex bytes or \"??\" separated by spaces, e.g. \"55 8b ec ??\"");
                } else if (sym.bytes.size() < kMinBytes || sym.bytes.size() > kMaxBytes) {
                    error(where + ": \"bytes\" must have " + std::to_string(kMinBytes) + " to " +
                          std::to_string(kMaxBytes) + " bytes");
                } else if (sym.bytes[0] < 0) {
                    error(where + ": \"bytes\" must not start with \"??\"");
                } else if (out_.size_of_image && sym.value + sym.bytes.size() > out_.size_of_image) {
                    error(where + ": \"bytes\" run past the end of the image");
                }
            }
        } else {
            if (entry.find("conv")) error(where + ": only functions and imports have a calling convention");
            if (entry.find("bytes")) error(where + ": only functions have \"bytes\"");
            if (const std::string* sig = string_field(entry, "sig", where, false)) sym.signature = *sig;
        }

        if (sym.kind == Kind::Call) {
            if (const std::string* target = string_field(entry, "target", where, true)) sym.target = *target;
        } else if (sym.kind == Kind::Import) {
            if (const std::string* target = string_field(entry, "target", where, true)) {
                const size_t bang = target->find('!');
                if (bang == 0 || bang == std::string::npos || bang + 1 == target->size()) {
                    error(where + ": an import's \"target\" is \"module!function\", e.g. \"kernel32.dll!Sleep\"");
                }
                sym.target = *target;
            }
        } else if (entry.find("target")) {
            error(where + ": only calls and imports have a target");
        }
        if (const std::string* doc = string_field(entry, "doc", where, false)) sym.doc = *doc;

        out_.symbols.push_back(std::move(sym));
    }

    // Runs once every symbol is known.
    void check_targets() {
        for (const Symbol& sym : out_.symbols) {
            if (sym.kind != Kind::Call) continue;
            const Symbol* target = out_.find(sym.target);
            if (!target) {
                error("symbol \"" + sym.name + "\": target \"" + sym.target + "\" isn't defined");
            } else if (target->kind != Kind::Function) {
                error("symbol \"" + sym.name + "\": target \"" + sym.target + "\" isn't a function");
            }
        }
    }

    ParseResult run(const json::Value& root) {
        if (!root.is_object()) {
            error("the file must contain a JSON object");
            return finish();
        }
        const json::Value* schema = root.find("schema");
        if (!schema || !schema->is_number()) {
            error("missing \"schema\"");
        } else if (schema->as_number() != kSchema) {
            error("unsupported schema " + std::to_string(static_cast<long long>(schema->as_number())) +
                  " (this Tome reads schema " + std::to_string(kSchema) + ")");
            return finish();
        }

        if (const std::string* name = string_field(root, "name", "build", true)) out_.name = *name;
        if (const std::string* version = string_field(root, "file_version", "build", true)) {
            out_.file_version = *version;
        }
        u32_field(root, "pe_timestamp", "build", out_.pe_timestamp);
        u32_field(root, "size_of_image", "build", out_.size_of_image);
        if (const std::string* sha = string_field(root, "sha256", "build", true)) {
            if (sha->size() != 64 || !is_lower_hex(*sha)) {
                error("build: \"sha256\" must be 64 lowercase hex digits");
            }
            out_.sha256 = *sha;
        }

        const json::Value* symbols = root.find("symbols");
        if (!symbols || !symbols->is_object()) {
            error("missing \"symbols\" object");
        } else {
            for (const auto& [name, entry] : symbols->members()) symbol(name, entry);
            check_targets();
        }
        return finish();
    }

private:
    ParseResult finish() {
        ParseResult r;
        r.ok = errors_.empty();
        r.errors = std::move(errors_);
        return r;
    }

    Build& out_;
    std::vector<std::string> errors_;
};

} // namespace

bool parse_bytes(const std::string& text, std::vector<int16_t>& out) {
    out.clear();
    if (text.size() % 3 != 2) return false;  // "xx" plus " xx" per further byte
    for (size_t i = 0; i < text.size(); i += 3) {
        if (i + 2 < text.size() && text[i + 2] != ' ') return false;
        if (text[i] == '?' && text[i + 1] == '?') {
            out.push_back(-1);
            continue;
        }
        int hi = hex_digit(text[i]), lo = hex_digit(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<int16_t>(hi << 4 | lo));
    }
    return !out.empty();
}

const Symbol* Build::find(std::string_view name) const {
    for (const Symbol& sym : symbols) {
        if (sym.name == name) return &sym;
    }
    return nullptr;
}

ParseResult parse_build(const json::Value& root, Build& out) {
    out = Build{};
    return Parser(out).run(root);
}

bool has_rva(Kind kind) {
    return kind == Kind::Function || kind == Kind::Data || kind == Kind::Call || kind == Kind::Import;
}

const char* to_string(Kind kind) {
    for (const KindName& k : kKinds) {
        if (k.kind == kind) return k.name;
    }
    return "?";
}

const char* to_string(Convention convention) {
    for (const ConventionName& c : kConventions) {
        if (c.convention == convention) return c.name;
    }
    return "";
}

} // namespace tome::mappings

namespace tome {

bool bind_build(const mappings::Build& build, BuildMappings& out, std::vector<std::string>& errors) {
    using mappings::Kind;
    const size_t errors_before = errors.size();
    out = BuildMappings{};
    out.data = &build;
    auto bind = [&](const char* name, Kind kind, uint32_t& field) {
        const mappings::Symbol* sym = build.find(name);
        if (!sym) {
            errors.push_back(std::string("\"") + name + "\" isn't mapped");
        } else if (sym->kind != kind) {
            errors.push_back(std::string("\"") + name + "\" is a " + to_string(sym->kind) + ", expected a " +
                             to_string(kind));
        } else {
            field = sym->value;
        }
    };
    // For symbols only a built-in patch needs: a build that doesn't map them
    // still binds (the field stays 0 and patches::install skips that patch),
    // but one mapped with the wrong kind is still an error.
    auto bind_optional = [&](const char* name, Kind kind, uint32_t& field) {
        if (build.find(name)) bind(name, kind, field);
    };

    auto& jass = out.jass;
    bind("BindNative", Kind::Function, jass.bind_native);
    bind("InitNatives", Kind::Function, jass.init_natives);
    bind("InitNatives.call.startup", Kind::Call, jass.init_natives_calls[0]);
    bind("InitNatives.call.map_load_1", Kind::Call, jass.init_natives_calls[1]);
    bind("InitNatives.call.map_load_2", Kind::Call, jass.init_natives_calls[2]);
    bind("RCString::GetText", Kind::Function, jass.string_get_text);
    bind("CreateJassString", Kind::Function, jass.create_string);
    bind("Native.CreateTrigger", Kind::Function, jass.create_trigger);
    bind("Native.DestroyTrigger", Kind::Function, jass.destroy_trigger);
    bind("Native.Condition", Kind::Function, jass.condition);
    bind("Native.TriggerAddCondition", Kind::Function, jass.trigger_add_condition);
    bind("Native.TriggerEvaluate", Kind::Function, jass.trigger_evaluate);
    bind("Native.TriggerAddAction", Kind::Function, jass.trigger_add_action);
    bind("Native.TriggerExecute", Kind::Function, jass.trigger_execute);

    auto& ui = out.ui;
    bind("LoadFrameDefToc", Kind::Function, ui.load_framedef_toc);
    bind("LoadTocFiles", Kind::Function, ui.load_toc_files);
    bind("FrameDefTableA", Kind::Data, ui.fdf_table_a);
    bind("FrameDefTableB", Kind::Data, ui.fdf_table_b);
    bind("FrameDefStatus", Kind::Data, ui.fdf_default_status);
    bind("LoadFrameDefToc.call.startup", Kind::Call, ui.load_framedef_toc_call);
    bind("CreateFrame", Kind::Function, ui.create_frame);
    bind("CLayoutFrame::SetPoint", Kind::Function, ui.layout_set_point);
    bind("CFrame.layout", Kind::Offset, ui.layout_offset);
    bind("CSpriteFrame::GetInstance", Kind::Function, ui.sprite_get_instance);
    bind("SetAnimation", Kind::Function, ui.model_set_animation);
    bind("AnimTag.stand", Kind::Constant, ui.anim_tag_stand);
    bind("AnimTag.morph", Kind::Constant, ui.anim_tag_morph);
    bind("AnimTag.alternate", Kind::Constant, ui.anim_tag_alternate);
    bind("AnimTag.options", Kind::Constant, ui.anim_tag_options);
    bind("GetFrameByName", Kind::Function, ui.get_frame_by_name);
    bind("CTextFrame::SetText", Kind::Function, ui.text_frame_set_text);
    bind("CFrame.vtable.Show", Kind::Offset, ui.frame_show_offset);
    bind("CFrame.vtable.Hide", Kind::Offset, ui.frame_hide_offset);
    bind("CGlueMenu::BindControl", Kind::Function, ui.glue_bind_control);
    bind("CMainMenu.vtable", Kind::Data, ui.main_menu_vtable);
    bind("CMainMenu.vtable.OnEvent", Kind::Offset, ui.main_menu_event_offset);
    bind("CMainMenu.vtable.Setup", Kind::Offset, ui.main_menu_setup_offset);
    bind("CMainMenu::OnEvent", Kind::Function, ui.main_menu_event_fn);
    bind("CMainMenu::Setup", Kind::Function, ui.main_menu_setup_fn);
    bind("CObserver.vtable", Kind::Data, ui.observer_vtable);
    bind("CObserver.vtable.OnEvent", Kind::Offset, ui.observer_event_offset);
    bind("CControl::SetListener", Kind::Function, ui.control_set_listener);
    bind("CFrame::SetFocus", Kind::Function, ui.frame_set_focus);
    bind("FocusedFrame", Kind::Data, ui.focused_frame);
    bind("CLayoutFrame::SetWidth", Kind::Function, ui.layout_set_width);
    bind("CControl::Click", Kind::Function, ui.control_click);
    bind("CObserver::HasEvent", Kind::Function, ui.observer_has_event);
    bind("ControlEvent.click", Kind::Constant, ui.control_click_event);
    bind("CEscMenuMainPanel::Setup", Kind::Function, ui.esc_menu_setup);
    bind("CEscMenuMainPanel::SetPanel", Kind::Function, ui.esc_menu_set_panel);
    bind("EscMenuPanel.main", Kind::Constant, ui.esc_panel_main);
    bind("EscMenuPanel.help", Kind::Constant, ui.esc_panel_help);
    bind("CEditBox::GetText", Kind::Function, ui.edit_get_text);
    bind("CEditBox::SetText", Kind::Function, ui.edit_set_text);
    bind("CGameUI::SetMode", Kind::Function, ui.game_ui_set_mode);
    bind("CGameUI.mode", Kind::Offset, ui.game_ui_mode_offset);
    bind("CGameUI.esc_menu", Kind::Offset, ui.game_ui_esc_menu_offset);
    bind("CEscMenu::Reset", Kind::Function, ui.esc_menu_reset);
    bind("CEscMenu.main_panel", Kind::Offset, ui.esc_menu_main_panel_offset);

    auto& game = out.game;
    bind("CGlueMgr::Get", Kind::Function, game.glue_get);
    bind("CGlueMgr::SetScreen", Kind::Function, game.glue_set_screen);
    bind("CGlueMgr::EnterGame", Kind::Function, game.glue_enter_game);
    bind("CGlueMgr.screen_id", Kind::Offset, game.glue_screen_id_offset);
    bind("CGlueMgr.screen", Kind::Offset, game.glue_screen_offset);
    bind("GlueScreen.loading", Kind::Constant, game.screen_loading);
    bind("GlueScreen.main_menu", Kind::Constant, game.screen_main_menu);
    bind("GlueScreen.options", Kind::Constant, game.screen_options);
    bind("GlueScreen.single_player", Kind::Constant, game.screen_single_player);
    bind("GlueScreen.custom_game", Kind::Constant, game.screen_custom_game);
    bind("GlueScreen.custom_campaign", Kind::Constant, game.screen_custom_campaign);
    bind("GlueScreen.campaign", Kind::Constant, game.screen_campaign);
    bind("GlueScreen.lan", Kind::Constant, game.screen_lan);
    bind("GlueScreen.lan_create", Kind::Constant, game.screen_lan_create);
    bind("GlueScreen.score_screen", Kind::Constant, game.screen_score_screen);
    bind("GlueScreen.view_replay", Kind::Constant, game.screen_view_replay);
    bind("GlueScreen.load_saved_game", Kind::Constant, game.screen_load_saved_game);
    bind("GlueScreen.battle_net", Kind::Constant, game.screen_battle_net);
    bind("GlueScreen.none", Kind::Constant, game.screen_none);
    bind("CGameWar3::Reset", Kind::Function, game.game_reset);
    bind("CGameWar3.instance", Kind::Data, game.game_instance);
    bind("CGameWar3.map_info", Kind::Offset, game.game_map_info_offset);
    bind("MapInfo.name", Kind::Offset, game.map_name_offset);
    bind("MapInfo.path", Kind::Offset, game.map_path_offset);
    bind("CGameUI::Get", Kind::Function, game.game_ui_get);
    bind("CGameUI::HandleSlashCommand", Kind::Function, game.game_ui_slash_command);
    bind("CGameUI::DisplayText", Kind::Function, game.game_ui_display_text);
    bind("PresentFrame", Kind::Function, game.present_frame);
    bind("OnNetGameTick", Kind::Function, game.game_tick);
    bind("CNetEventGameTick.step", Kind::Offset, game.game_tick_step_offset);
    bind("GameShutdown", Kind::Function, game.game_shutdown);

    auto& play = out.play;
    bind("MapInfo.description", Kind::Offset, play.map_description_offset);
    bind("CGameWar3.local_player", Kind::Offset, play.local_player_offset);
    bind("CGameWar3.handles", Kind::Offset, play.handles_offset);
    bind("CGameWar3::GetPlayer", Kind::Function, play.get_player);
    bind("CPlayerWar3.name", Kind::Offset, play.player_name_offset);
    bind("CPlayerWar3.slot", Kind::Offset, play.player_slot_offset);
    bind("CPlayerWar3.selection", Kind::Offset, play.player_selection_offset);
    bind("CSelectionWar3.local_leader", Kind::Offset, play.selection_leader_offset);
    bind("CSelectionWar3.local_units", Kind::Offset, play.selection_units_offset);
    bind("UnitGroup.first", Kind::Offset, play.group_first_offset);
    bind("UnitGroup.count", Kind::Offset, play.group_count_offset);
    bind("CAgent.id", Kind::Offset, play.agent_id_offset);
    bind("CUnit.type_id", Kind::Offset, play.unit_type_offset);
    bind("CUnit.owner", Kind::Offset, play.unit_owner_offset);
    bind("CUnit.flags", Kind::Offset, play.unit_flags_offset);
    bind("UnitFlag.dead", Kind::Constant, play.unit_flag_dead);
    bind("UnitFlag.structure", Kind::Constant, play.unit_flag_structure);
    bind("UnitFlag.not_hero", Kind::Constant, play.unit_flag_not_hero);
    bind("GetUnitTypeName", Kind::Function, play.unit_type_name);
    bind("HandleManager.hash_buckets", Kind::Offset, play.handle_buckets_offset);
    bind("HandleManager.hash_mask", Kind::Offset, play.handle_mask_offset);
    bind("HandleManager.entries", Kind::Offset, play.handle_entries_offset);
    bind("HandleId.first", Kind::Constant, play.handle_first);
    bind("CGameUI::DisplayChatMessage", Kind::Function, play.game_ui_chat_message);
    bind("CGameUI::SetPaused", Kind::Function, play.game_ui_set_paused);
    bind("CGameUI.paused_by_player", Kind::Offset, play.paused_by_player_offset);
    bind("CGameUI.paused_by_trigger", Kind::Offset, play.paused_by_trigger_offset);
    // The game's JASS event functions (CPlayerWar3::On*, CUnit::On*) that
    // events.cpp hooks by name (kUnitHooks): only checked here.
    uint32_t unused;
    for (const char* name : kEventFunctions) bind(name, Kind::Function, unused);
    bind("CPlayerWar3::RemovePlayer", Kind::Function, play.remove_player);
    bind("CPlayerWar3.game_state", Kind::Offset, play.player_state_offset);
    bind("CGameWar3::SaveGame", Kind::Function, play.save_game);
    bind("CGameWar3.load_path", Kind::Offset, play.load_path_offset);
    bind("CGameWar3.load_is_save", Kind::Offset, play.load_is_save_offset);
    bind("HandleManager::FireGameLoaded", Kind::Function, play.fire_game_loaded);
    bind("CUnit::GetHeroLevel", Kind::Function, play.unit_hero_level);
    bind("CItem.type_id", Kind::Offset, play.item_type_offset);
    bind("COrder::GetTarget", Kind::Function, play.order_target);
    bind("COrder.id", Kind::Offset, play.order_id_offset);
    bind("COrder.x", Kind::Offset, play.order_x_offset);
    bind("COrder.y", Kind::Offset, play.order_y_offset);
    bind("CUnit.vtable", Kind::Data, play.unit_vtable);
    bind("CItem.vtable", Kind::Data, play.item_vtable);
    bind("CPlayerWar3.controller", Kind::Offset, play.player_controller_offset);
    bind("CDestructable.vtable", Kind::Data, play.destructable_vtable);
    bind("CDestructable.type_id", Kind::Offset, play.destructable_type_offset);
    bind("CAbility.type_id", Kind::Offset, play.ability_type_offset);
    bind("CAbilitySpell.vtable.GetTargetWidget", Kind::Offset, play.spell_target_slot);
    bind("CAbilitySpell.vtable.GetTargetX", Kind::Offset, play.spell_target_x_slot);
    bind("CAbilitySpell.vtable.GetTargetY", Kind::Offset, play.spell_target_y_slot);
    bind("CNetEvent.sender", Kind::Offset, play.net_event_sender_offset);
    bind("CAbility.owner", Kind::Offset, play.ability_owner_offset);
    bind("CAbilityHero.xp", Kind::Offset, play.hero_xp_offset);
    bind("CPlayerWar3.states", Kind::Offset, play.player_states_offset);
    bind("PlayerState.size", Kind::Constant, play.player_state_size);
    bind("SyncedInt::Get", Kind::Function, play.synced_int_get);
    bind("GetFloatGameState", Kind::Function, play.get_float_game_state);

    auto& net = out.net;
    bind("GlueScreen.game_chatroom", Kind::Constant, net.screen_game_chatroom);
    bind("CGameChatroom::OnChatReceived", Kind::Function, net.chatroom_chat_received);
    bind("CGameChatroom::StartGame", Kind::Function, net.chatroom_start_game);
    bind("CGameChatroom.net_index", Kind::Offset, net.chatroom_net_index_offset);
    bind("CGameChatroom.chat_area", Kind::Offset, net.chatroom_chat_area_offset);
    bind("CTextArea::AddText", Kind::Function, net.text_area_add_text);
    bind("CNetEventLobby::Init", Kind::Function, net.lobby_event_init);
    bind("CNetEventLobby.sender", Kind::Offset, net.lobby_event_sender_offset);
    bind("CNetEventLobby.text", Kind::Offset, net.lobby_event_text_offset);
    bind("CNetEventLobby.size", Kind::Constant, net.lobby_event_size);
    bind("LobbyEvent.chat", Kind::Constant, net.lobby_event_chat);
    bind("NetSendLobbyEvent", Kind::Function, net.send_lobby_event);
    bind("NetGetPlayerName", Kind::Function, net.player_name);
    bind("NetGetLocalPlayer", Kind::Function, net.local_player);
    bind("NetIsHost", Kind::Function, net.is_host);
    bind("NetIsReplay", Kind::Function, net.is_replay);
    bind("CNetEventSyncStoredInteger::Init", Kind::Function, net.sync_init);
    bind("CNetEventSyncStoredInteger.sender", Kind::Offset, net.sync_sender_offset);
    bind("CNetEventSyncStoredInteger.file", Kind::Offset, net.sync_file_offset);
    bind("CNetEventSyncStoredInteger.mission", Kind::Offset, net.sync_mission_offset);
    bind("CNetEventSyncStoredInteger.key", Kind::Offset, net.sync_key_offset);
    bind("CNetEventSyncStoredInteger.value_object", Kind::Offset, net.sync_value_object_offset);
    bind("CNetEventSyncStoredInteger.value", Kind::Offset, net.sync_value_offset);
    bind("CNetEventSyncStoredInteger.size", Kind::Constant, net.sync_size);
    bind("SyncValue::Destroy", Kind::Function, net.sync_value_destroy);
    bind("NetSendAction", Kind::Function, net.send_action);
    bind("OnNetSyncStoredInteger", Kind::Function, net.on_sync_integer);

    auto& blp = out.blp;  // blp-guard's symbols (patches/blp_guard.cpp)
    bind_optional("CBLPFile::Open", Kind::Function, blp.open);
    bind_optional("CBLPFile.buffer", Kind::Offset, blp.buffer_offset);
    bind_optional("CBLPFile.size", Kind::Offset, blp.size_offset);
    bind_optional("CBLPFile.header_copy", Kind::Data, blp.header_copy);

    auto& d3d = out.d3d;  // d3d-reset-lights' symbols (patches/d3d_reset_lights.cpp)
    bind_optional("CGxDeviceD3d.d3d_device", Kind::Offset, d3d.device_offset);

    auto& gl = out.gl;  // gl-context-restore's symbols (patches/gl_context_restore.cpp)
    bind_optional("CGxDeviceOpenGl::DestroyContext", Kind::Function, gl.destroy_context);
    bind_optional("CGxDeviceOpenGl::CreateContext", Kind::Function, gl.create_context);
    bind_optional("CGxDevice.fullscreen", Kind::Offset, gl.fullscreen_offset);

    auto& sound = out.sound;  // ambient-sound's symbols (patches/ambient_sound.cpp)
    bind_optional("CreateMIDISnd", Kind::Function, sound.create_midi);
    bind_optional("CSound.sound", Kind::Offset, sound.sound_offset);
    bind_optional("CSound.midi", Kind::Offset, sound.midi_offset);
    bind_optional("SoundCategory.ambient", Kind::Constant, sound.ambient_category);
    // The handshake writes the names into the action and reads the lobby's
    // text; each must leave room for its 0x100 bytes. (Only if they're all
    // mapped: a missing one is reported above.)
    if (errors.size() == errors_before && (net.sync_mission_offset < net.sync_file_offset + 0x100 || net.sync_key_offset < net.sync_mission_offset + 0x100 ||
        net.sync_value_object_offset < net.sync_key_offset + 0x100 || net.sync_value_offset + 4 > net.sync_size ||
        net.lobby_event_text_offset + 0x100 > net.lobby_event_size)) {
        errors.push_back("the handshake's event layouts overlap");
    }

    for (uint32_t offset : {ui.frame_show_offset, ui.frame_hide_offset, ui.main_menu_event_offset,
                            ui.main_menu_setup_offset, ui.observer_event_offset}) {
        if (offset % 4 != 0) errors.push_back("vtable offsets must be multiples of 4");
    }
    return errors.size() == errors_before;
}

} // namespace tome
