// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The mapping database: one JSON file per game build (mappings/<build>.json)
// naming every address, offset and constant Tome or a mod uses, and one per
// build of another module Tome patches (its "module", e.g. blizzard.ax). See
// docs/architecture.md, "Mapping database", for the file format.

#include "json.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tome::mappings {

constexpr int kSchema = 2;

enum class Kind {
    Function,  // code; has a calling convention and a signature
    Data,      // a global or a vtable
    Call,      // an `E8 rel32` instruction Tome may redirect; has a target function
    Offset,    // a byte offset inside a struct or vtable
    Constant,  // any other build-specific number (ids, tags)
    Import,    // an import address table slot of game.dll; has a convention, a signature and a target
};

enum class Convention { None, Cdecl, Stdcall, Fastcall, Thiscall };

struct Symbol {
    std::string name;
    Kind kind = Kind::Constant;
    uint32_t value = 0;  // the RVA for Function, Data and Call; the number itself otherwise
    Convention convention = Convention::None;
    std::string signature;
    // Call: the name of the function it calls. Import: the imported
    // function as "module!name", e.g. "kernel32.dll!GetTickCount".
    std::string target;
    // Function only: its first bytes, checked against the loaded game.dll
    // before anything is patched. -1 matches any byte (relocated addresses).
    std::vector<int16_t> bytes;
    std::string doc;
};

struct Build {
    std::string module = "game.dll";  // the file it describes, lowercase
    std::string name;          // community name, e.g. "1.27b"
    std::string file_version;  // e.g. "1.27.1.7085"
    uint32_t pe_timestamp = 0;
    uint32_t size_of_image = 0;
    std::string sha256;        // of the module on disk, lowercase hex
    std::vector<Symbol> symbols;

    const Symbol* find(std::string_view name) const;
};

struct ParseResult {
    bool ok = false;
    std::vector<std::string> errors;
};

// Validates one build file. Every error names the symbol it's about.
ParseResult parse_build(const json::Value& root, Build& out);

// Parses a "bytes" pattern: two lowercase hex digits or "??" per byte,
// separated by single spaces.
bool parse_bytes(const std::string& text, std::vector<int16_t>& out);

bool has_rva(Kind kind);
const char* to_string(Kind kind);
const char* to_string(Convention convention);

} // namespace tome::mappings

namespace tome {

// The symbols Tome itself needs, looked up once per build. RVAs and offsets
// as in the data file; see mappings/1.27b.json for what each one is.
struct BuildMappings {
    const mappings::Build* data = nullptr;

    struct {
        uint32_t bind_native;            // BindNative
        uint32_t init_natives;           // InitNatives
        uint32_t init_natives_calls[3];  // InitNatives.call.*; Tome redirects these
        uint32_t string_get_text;        // RCString::GetText (JASS string arguments)
        uint32_t create_string;          // CreateJassString (JASS string results)
        // Native.*: the game's own natives, for calling `code` values
        uint32_t create_trigger, destroy_trigger, condition, trigger_add_condition, trigger_evaluate,
            trigger_add_action, trigger_execute;
    } jass;

    struct {
        uint32_t load_framedef_toc;       // LoadFrameDefToc
        uint32_t load_toc_files;          // LoadTocFiles
        uint32_t fdf_table_a;             // FrameDefTableA
        uint32_t fdf_table_b;             // FrameDefTableB
        uint32_t fdf_default_status;      // FrameDefStatus
        uint32_t load_framedef_toc_call;  // LoadFrameDefToc.call.startup
        uint32_t create_frame;            // CreateFrame
        uint32_t layout_set_point;        // CLayoutFrame::SetPoint
        uint32_t layout_offset;           // CFrame.layout
        uint32_t sprite_get_instance;     // CSpriteFrame::GetInstance
        uint32_t skin_get_string;         // Skin::GetString
        uint32_t model_set_animation;     // SetAnimation
        uint32_t anim_tag_stand, anim_tag_morph, anim_tag_alternate, anim_tag_options;  // AnimTag.*
        uint32_t get_frame_by_name;       // GetFrameByName
        uint32_t text_frame_set_text;     // CTextFrame::SetText
        uint32_t frame_show_offset;       // CFrame.vtable.Show
        uint32_t frame_hide_offset;       // CFrame.vtable.Hide
        uint32_t glue_bind_control;       // CGlueMenu::BindControl
        uint32_t main_menu_vtable;        // CMainMenu.vtable
        uint32_t main_menu_event_offset;  // CMainMenu.vtable.OnEvent
        uint32_t main_menu_setup_offset;  // CMainMenu.vtable.Setup
        uint32_t main_menu_event_fn;      // CMainMenu::OnEvent, expected in that slot
        uint32_t main_menu_setup_fn;      // CMainMenu::Setup
        uint32_t observer_vtable;         // CObserver.vtable
        uint32_t observer_event_offset;   // CObserver.vtable.OnEvent
        uint32_t control_set_listener;    // CControl::SetListener
        uint32_t frame_set_focus;         // CFrame::SetFocus
        uint32_t focused_frame;           // FocusedFrame
        uint32_t layout_set_width;        // CLayoutFrame::SetWidth
        uint32_t control_click;           // CControl::Click
        uint32_t observer_has_event;      // CObserver::HasEvent
        uint32_t control_click_event;     // ControlEvent.click
        uint32_t control_click_data;      // ControlClickEvent
        uint32_t esc_menu_setup;          // CEscMenuMainPanel::Setup (hooked by name)
        uint32_t esc_menu_set_panel;      // CEscMenuMainPanel::SetPanel
        uint32_t esc_panel_main, esc_panel_help;  // EscMenuPanel.*
        uint32_t edit_get_text, edit_set_text;    // CEditBox::GetText, SetText
        uint32_t game_ui_set_mode;        // CGameUI::SetMode
        uint32_t game_ui_mode_offset;     // CGameUI.mode
        uint32_t game_ui_esc_menu_offset; // CGameUI.esc_menu
        uint32_t esc_menu_reset;          // CEscMenu::Reset
        uint32_t esc_menu_main_panel_offset;  // CEscMenu.main_panel
    } ui;

    // Glue screens, games and the in-game UI (events.cpp).
    struct {
        uint32_t glue_get;                // CGlueMgr::Get
        uint32_t glue_set_screen;         // CGlueMgr::SetScreen
        uint32_t glue_enter_game;         // CGlueMgr::EnterGame
        uint32_t glue_screen_id_offset;   // CGlueMgr.screen_id
        uint32_t glue_screen_offset;      // CGlueMgr.screen
        // GlueScreen.*
        uint32_t screen_loading, screen_main_menu, screen_options, screen_single_player, screen_custom_game,
            screen_custom_campaign, screen_campaign, screen_lan, screen_lan_create, screen_score_screen, screen_view_replay,
            screen_load_saved_game, screen_battle_net, screen_none;
        uint32_t game_reset;              // CGameWar3::Reset
        uint32_t game_instance;           // CGameWar3.instance
        uint32_t game_map_info_offset;    // CGameWar3.map_info
        uint32_t map_name_offset;         // MapInfo.name
        uint32_t map_path_offset;         // MapInfo.path
        uint32_t game_ui_get;             // CGameUI::Get
        uint32_t game_ui_slash_command;   // CGameUI::HandleSlashCommand
        uint32_t game_ui_display_text;    // CGameUI::DisplayText
        uint32_t present_frame;           // PresentFrame
        uint32_t game_tick;               // OnNetGameTick
        uint32_t game_tick_step_offset;   // CNetEventGameTick.step
        uint32_t game_shutdown;           // GameShutdown
        uint32_t expansion;               // Game.expansion (int: 1 The Frozen Throne, 0 Reign of Chaos)
    } game;

    // Players, units, the selection, chat and pause (events.cpp, units.cpp).
    struct {
        uint32_t map_description_offset;  // MapInfo.description
        uint32_t local_player_offset;     // CGameWar3.local_player
        uint32_t handles_offset;          // CGameWar3.handles
        uint32_t get_player;              // CGameWar3::GetPlayer
        uint32_t player_count_offset;     // CGameWar3.player_count (1.26a's GetPlayer doesn't check it)
        uint32_t player_name_offset;      // CPlayerWar3.name
        uint32_t player_slot_offset;      // CPlayerWar3.slot
        uint32_t player_selection_offset; // CPlayerWar3.selection
        uint32_t selection_leader_offset; // CSelectionWar3.local_leader
        uint32_t selection_units_offset;  // CSelectionWar3.local_units
        uint32_t group_first_offset;      // UnitGroup.first
        uint32_t group_count_offset;      // UnitGroup.count
        uint32_t agent_id_offset;         // CAgent.id
        uint32_t unit_type_offset;        // CUnit.type_id
        uint32_t unit_owner_offset;       // CUnit.owner
        uint32_t unit_flags_offset;       // CUnit.flags
        uint32_t unit_flag_dead, unit_flag_structure, unit_flag_not_hero;  // UnitFlag.*
        uint32_t unit_type_name;          // GetUnitTypeName
        uint32_t handle_buckets_offset;   // HandleManager.hash_buckets
        uint32_t handle_mask_offset;      // HandleManager.hash_mask
        uint32_t handle_entries_offset;   // HandleManager.entries
        uint32_t handle_first;            // HandleId.first
        uint32_t game_ui_chat_message;    // CGameUI::DisplayChatMessage
        uint32_t game_ui_set_paused;      // CGameUI::SetPaused
        uint32_t paused_by_player_offset; // CGameUI.paused_by_player
        uint32_t paused_by_trigger_offset;// CGameUI.paused_by_trigger
        uint32_t remove_player;           // CPlayerWar3::RemovePlayer
        uint32_t player_state_offset;     // CPlayerWar3.game_state
        uint32_t save_game;               // CGameWar3::SaveGame
        uint32_t load_path_offset;        // CGameWar3.load_path
        uint32_t load_is_save_offset;     // CGameWar3.load_is_save
        uint32_t fire_game_loaded;        // HandleManager::FireGameLoaded
        uint32_t unit_hero_level;         // CUnit::GetHeroLevel
        uint32_t item_type_offset;        // CItem.type_id
        uint32_t order_target;            // COrder::GetTarget
        uint32_t order_id_offset, order_x_offset, order_y_offset;  // COrder.*
        uint32_t unit_vtable, item_vtable;  // CUnit.vtable, CItem.vtable
        uint32_t player_controller_offset;  // CPlayerWar3.controller
        uint32_t destructable_vtable;       // CDestructable.vtable
        uint32_t destructable_type_offset;  // CDestructable.type_id
        uint32_t ability_type_offset;       // CAbility.type_id
        uint32_t spell_target_slot;         // CAbilitySpell.vtable.GetTargetWidget
        uint32_t spell_target_x_slot;       // CAbilitySpell.vtable.GetTargetX
        uint32_t spell_target_y_slot;       // CAbilitySpell.vtable.GetTargetY
        uint32_t net_event_sender_offset;   // CNetEvent.sender
        uint32_t ability_owner_offset;      // CAbility.owner
        uint32_t hero_xp_offset;            // CAbilityHero.xp
        uint32_t player_states_offset;      // CPlayerWar3.states
        uint32_t player_state_size;         // PlayerState.size
        uint32_t synced_int_get;            // SyncedInt::Get
        uint32_t get_float_game_state;      // GetFloatGameState
    } play;

    // The multiplayer handshake (handshake.cpp): the lobby's chat and Start
    // button, and the SyncStoredInteger action that carries the mod set in games.
    struct {
        uint32_t screen_game_chatroom;      // GlueScreen.game_chatroom
        uint32_t chatroom_chat_received;    // CGameChatroom::OnChatReceived
        uint32_t chatroom_start_game;       // CGameChatroom::StartGame
        uint32_t chatroom_net_index_offset; // CGameChatroom.net_index
        uint32_t chatroom_chat_area_offset; // CGameChatroom.chat_area
        uint32_t text_area_add_text;        // CTextArea::AddText
        uint32_t lobby_event_init;          // CNetEventLobby::Init
        uint32_t lobby_event_sender_offset; // CNetEventLobby.sender
        uint32_t lobby_event_text_offset;   // CNetEventLobby.text
        uint32_t lobby_event_size;          // CNetEventLobby.size
        uint32_t lobby_event_chat;          // LobbyEvent.chat
        uint32_t send_lobby_event;          // NetSendLobbyEvent
        uint32_t player_name;               // NetGetPlayerName
        uint32_t local_player;              // NetGetLocalPlayer
        uint32_t is_host;                   // NetIsHost
        uint32_t is_replay;                 // NetIsReplay
        uint32_t sync_init;                 // CNetEventSyncStoredInteger::Init
        uint32_t sync_sender_offset;        // CNetEventSyncStoredInteger.sender
        uint32_t sync_file_offset;          // CNetEventSyncStoredInteger.file
        uint32_t sync_mission_offset;       // CNetEventSyncStoredInteger.mission
        uint32_t sync_key_offset;           // CNetEventSyncStoredInteger.key
        uint32_t sync_value_object_offset;  // CNetEventSyncStoredInteger.value_object
        uint32_t sync_value_offset;         // CNetEventSyncStoredInteger.value
        uint32_t sync_size;                 // CNetEventSyncStoredInteger.size
        uint32_t sync_value_destroy;        // SyncValue::Destroy
        uint32_t send_action;               // NetSendAction
        uint32_t on_sync_integer;           // OnNetSyncStoredInteger (hooked by name)
    } net;

    // Optional: 0 when the build doesn't map them (blp-guard is then skipped).
    struct {
        uint32_t open;          // CBLPFile::Open
        uint32_t buffer_offset; // CBLPFile.buffer
        uint32_t size_offset;   // CBLPFile.size
        uint32_t header_copy;   // CBLPFile.header_copy (patched by blp-guard)
    } blp;

    // Optional: 0 when the build doesn't map them (d3d-reset-lights is then
    // skipped). The hooked ReleaseForReset is found by name, not stored here.
    struct {
        uint32_t device_offset;  // CGxDeviceD3d.d3d_device
    } d3d;

    // Optional: 0 when the build doesn't map them (gl-context-restore is then
    // skipped). The hooked CanDraw is found by name, not stored here.
    struct {
        uint32_t destroy_context;    // CGxDeviceOpenGl::DestroyContext
        uint32_t create_context;     // CGxDeviceOpenGl::CreateContext
        uint32_t fullscreen_offset;  // CGxDevice.fullscreen
    } gl;

    // Optional: 0 when the build doesn't map them (ambient-sound is then
    // skipped). The hooked CSound::InitMIDI is found by name, not stored here.
    struct {
        uint32_t create_midi;       // CreateMIDISnd
        uint32_t sound_offset;      // CSound.sound
        uint32_t midi_offset;       // CSound.midi
        uint32_t ambient_category;  // SoundCategory.ambient
    } sound;

    // Optional here (bind_optional), but widescreen needs it (with
    // CSpriteFrame::SetupCamera, its kSymbols): a build that doesn't map it
    // runs without the patch. The hooked functions are found by name, not
    // stored here.
    struct {
        uint32_t world_frame_offset;  // CGameUI.world_frame
    } widescreen;

    // Optional: movie-display-reset's (patches/movie_display_reset.cpp).
    struct {
        uint32_t cinematic_mode;  // Movie.cinematic_mode
        uint32_t gamma_call;      // Movie.gamma_call (patched by movie-display-reset)
        uint32_t gamma_restore;   // Movie.gamma_restore (patched by movie-display-reset)
    } movie;

    // Optional: game objects for mods (objects.cpp, API version 16). Each
    // property, handle lookup and jass_call works when its own symbols are
    // mapped (non-zero), and reports TOME_ERR_UNAVAILABLE otherwise.
    struct {
        uint32_t handle_count_offset;       // HandleManager.count
        uint32_t handle_free_count_offset;  // HandleManager.free_count
        uint32_t handle_entry_size;         // HandleEntry.size
        uint32_t handle_entry_object;       // HandleEntry.object
        uint32_t object_lookup;             // ObjectLookup
        uint32_t agent_tag_offset;          // AgentRecord.tag
        uint32_t agent_dying_offset;        // AgentRecord.dying
        uint32_t agent_tag;                 // AgentTag.agent
        uint32_t unit_get_state;            // CUnit::GetState
        uint32_t unit_set_state;            // CUnit::SetState
        uint32_t unit_get_move_speed;       // CUnit::GetMoveSpeed
        uint32_t unit_set_move_speed;       // CUnit::SetMoveSpeed
        uint32_t unit_get_level;            // CUnit::GetLevel
        uint32_t unit_position_slot;        // CUnit.vtable.GetPosition
        uint32_t position_get;              // CPosition::Get
        uint32_t position_set;              // CPosition::Set
        uint32_t widget_get_life_slot;      // CWidget.vtable.GetLife
        uint32_t widget_set_life_slot;      // CWidget.vtable.SetLife
        uint32_t item_charges_offset;       // CItem.charges
        uint32_t player_set_state;          // CPlayerWar3::SetState
        uint32_t jass_vm_get;               // JassVM::Get
        uint32_t vm_index_offset;           // CGameWar3.vm_index
        uint32_t vm_strings_offset;         // JassVM.strings
        uint32_t string_count_offset;       // JassStringTable.count
        uint32_t string_entries_offset;     // JassStringTable.entries
        uint32_t string_entry_size;         // JassString.size
        uint32_t float_multiply;            // FloatMultiply
        uint32_t position_facing_slot;      // CPosition.vtable.GetFacing
        uint32_t position_set_facing;       // CPosition::SetFacing
        uint32_t unit_fly_height_offset;    // CUnit.fly_height
        uint32_t unit_find_ability;         // CUnit::FindAbility
        uint32_t ability_level_offset;      // CAbility.level
        uint32_t ability_max_level;         // CAbility::GetMaxLevel
        uint32_t ability_level_up_slot;     // CAbility.vtable.LevelUp
        uint32_t ability_level_down_slot;   // CAbility.vtable.LevelDown
        uint32_t unit_get_acquire_range;    // CUnit::GetAcquireRange
        uint32_t unit_set_acquire_range;    // CUnit::SetAcquireRange
        uint32_t unit_get_hero_xp;          // CUnit::GetHeroXP
        uint32_t unit_get_hero_stat[3];     // CUnit::GetHeroStr, GetHeroAgi, GetHeroInt
        uint32_t unit_set_hero_stat[3];     // CUnit::SetHeroStr, SetHeroAgi, SetHeroInt
        uint32_t unit_armor_offset;         // CUnit.armor
        uint32_t unit_armor_type_offset;    // CUnit.armor_type
        uint32_t unit_attack_offset;        // CUnit.attack
        uint32_t attack_damage_base_offset; // CAbilityAttack.damage_base (int; the second weapon's 4 bytes on)
        uint32_t attack_damage_dice_offset; // CAbilityAttack.damage_dice (int, likewise)
        uint32_t attack_damage_sides_offset;// CAbilityAttack.damage_sides (int, likewise)
        uint32_t attack_cooldown_offset;    // CAbilityAttack.cooldown (float; the second weapon's 8 bytes on)
        uint32_t attack_range_offset;       // CAbilityAttack.range (float, likewise)
        uint32_t for_each_agent;            // ForEachAgent
        uint32_t agent_tags[3];             // AgentTag.unit, .item, .destructable
    } objects;
};

// The game's JASS event functions that events.cpp hooks by name (its
// kUnitHooks table, plus OnLeave, the orders and the other hooks in
// install); bind_build checks that
// each is a mapped function.
inline constexpr const char* kEventFunctions[] = {
    "CPlayerWar3::OnUnitDeath", "CPlayerWar3::OnTrainFinish", "CPlayerWar3::OnConstructFinish",
    "CPlayerWar3::OnUpgradeFinish", "CPlayerWar3::OnResearchFinish", "CPlayerWar3::OnHeroLevel",
    "CPlayerWar3::OnSummon", "CPlayerWar3::OnPickupItem", "CPlayerWar3::OnDropItem", "CPlayerWar3::OnUseItem",
    "CPlayerWar3::OnUnitAttacked", "CPlayerWar3::OnUnitRescued", "CPlayerWar3::OnConstructStart",
    "CPlayerWar3::OnConstructCancel", "CPlayerWar3::OnUpgradeStart", "CPlayerWar3::OnUpgradeCancel",
    "CPlayerWar3::OnTrainStart", "CPlayerWar3::OnTrainCancel", "CPlayerWar3::OnResearchStart",
    "CPlayerWar3::OnResearchCancel", "CPlayerWar3::OnHeroSkill", "CPlayerWar3::OnHeroRevivable",
    "CPlayerWar3::OnHeroReviveStart", "CPlayerWar3::OnHeroReviveCancel", "CPlayerWar3::OnHeroReviveFinish",
    "CPlayerWar3::OnUnitSell", "CPlayerWar3::OnItemSell", "CPlayerWar3::OnItemPawn",
    "CPlayerWar3::OnUnitChangeOwner", "CPlayerWar3::OnLeave", "CUnit::OnLoaded", "CUnit::OnOrder",
    "CUnit::OnPointOrder", "CUnit::OnTargetOrder", "CUnit::OnSpellChannel", "CUnit::OnSpellCast",
    "CUnit::OnSpellEffect", "CUnit::OnSpellFinish", "CUnit::OnSpellEndCast", "CUnit::OnDamaged",
    "CUnit::OnAcquireTarget", "CPlayerWar3::OnUnitDecay", "CPlayerWar3::OnUnitDetected",
    "CPlayerWar3::OnUnitHidden", "CPlayerWar3::OnEndCinematic", "CPlayerWar3::OnArrowKey", "CWidget::OnDeath",
    "CScriptDialog::OnClick", "OnNetShowSkillMenu", "OnNetShowBuildMenu", "HandleManager::FireEndLevel",
    "CreateUnitObject", "CUnit::Remove", "CAbilityHero::AddXP",
};

// Fills `out` from `build`. Fails if any symbol is missing (except the
// optional ones noted above) or has the wrong kind; each error names the
// symbol.
bool bind_build(const mappings::Build& build, BuildMappings& out, std::vector<std::string>& errors);

// blizzard.ax, the movies' BLZ0 decoder (module "blizzard.ax";
// patches/movie_frame_length.cpp). RVAs into blizzard.ax.
struct BlizzardAxMappings {
    const mappings::Build* data = nullptr;
    uint32_t transform;            // BlzFilter::Transform
    uint32_t transform_slot;       // BlzFilter.vtable.Transform_slot: its vtable slot
    uint32_t width_offset;         // BlzFilter.width: the movie's
    uint32_t height_offset;        // BlzFilter.height
    uint32_t format_offset;        // BlzFilter.format: movie_decoder::Format
    uint32_t output_width_offset;  // BlzFilter.output_width: the output type's biWidth
};

// Fills `out` from a blizzard.ax mapping; as bind_build.
bool bind_blizzard_ax(const mappings::Build& build, BlizzardAxMappings& out, std::vector<std::string>& errors);

} // namespace tome
