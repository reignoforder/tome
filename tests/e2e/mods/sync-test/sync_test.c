/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * e2e fixture (tools/e2e-multiplayer.sh): the mod every player of the LAN
 * game needs. Its code tries messages between players: "/synctest <text>"
 * sends the text, "/synctest big" 1000 bytes with NULs in them; every
 * message that arrives is logged with its sender, size and a checksum.
 * Its state for desync checks (on_checksum) is what it received, the same
 * on every computer, until "/synctest skew" changes this computer's alone.
 *
 * In the lobby (API version 21): a joined player sends the host "hello"
 * once the host's Tome answered, the host answers "welcome", and both log
 * what arrives. The host's first Start that the handshake allows is held
 * once ("holding the start once"); the next frame the mod presses Start
 * itself (lobby_start), which goes through.
 */
#include <tome/tome.h>

#include <string.h>

static const tome_api* api;
static uint64_t g_state = TOME_HASH_INIT;  /* every message received, in order */
static uint32_t g_skew = 0;                /* "/synctest skew": this computer only */
static int g_in_lobby = 0;                 /* lobby_players saw a lobby last frame */
static int g_said_hello = 0;               /* client: hello went to this lobby's host */
static int g_held = 0;                     /* host: the start was held once in this lobby */
static int g_press = 0;                    /* host: press Start on the next frame */

static uint32_t fnv1a(const unsigned char* data, uint32_t size) {
    uint32_t h = 2166136261u;
    uint32_t i;
    for (i = 0; i < size; ++i) h = (h ^ data[i]) * 16777619u;
    return h;
}

static void TOME_CALL on_message(tome_mod* mod, const tome_net_message* m, void* user) {
    char text[41] = {0};
    uint32_t n = m->data_size < 40 ? m->data_size : 40;
    uint32_t i;
    (void)user;
    g_state = tome_hash_u32(tome_hash_bytes(g_state, m->data, m->data_size), (uint32_t)m->player);
    for (i = 0; i < n; ++i) {
        const char c = ((const char*)m->data)[i];
        text[i] = c >= 32 && c < 127 ? c : '.';
    }
    tome_logf(api, mod, TOME_LOG_INFO, "message from player %d (%u bytes, fnv %08x, replay %d): %s", (int)m->player,
              (unsigned)m->data_size, (unsigned)fnv1a((const unsigned char*)m->data, m->data_size), (int)m->replay,
              text);
}

static void TOME_CALL on_command(tome_mod* mod, const char* args, void* user) {
    tome_result r;
    (void)user;
    if (strcmp(args, "skew") == 0) {
        ++g_skew;
        tome_logf(api, mod, TOME_LOG_INFO, "skewed this computer's state");
        return;
    }
    if (strcmp(args, "big") == 0) {
        unsigned char big[1000];
        int i;
        for (i = 0; i < 1000; ++i) big[i] = (unsigned char)(i * 7);
        r = api->net_send(mod, big, sizeof(big));
        tome_logf(api, mod, TOME_LOG_INFO, "sent 1000 bytes (fnv %08x): %d", (unsigned)fnv1a(big, sizeof(big)), (int)r);
    } else {
        r = api->net_send(mod, args, (uint32_t)strlen(args));
        tome_logf(api, mod, TOME_LOG_INFO, "sent \"%s\": %d", args, (int)r);
    }
}

static void TOME_CALL on_lobby_message(tome_mod* mod, const tome_lobby_message* m, void* user) {
    char text[41] = {0};
    uint32_t n = m->data_size < 40 ? m->data_size : 40;
    (void)user;
    memcpy(text, m->data, n);
    tome_logf(api, mod, TOME_LOG_INFO, "lobby message from player %d (from host %d, %u bytes): %s", (int)m->player,
              (int)m->from_host, (unsigned)m->data_size, text);
    if (strcmp(text, "hello") == 0) {
        tome_result r = api->lobby_send(mod, m->player, "welcome", 7);
        tome_logf(api, mod, TOME_LOG_INFO, "lobby: sent welcome to player %d: %d", (int)m->player, (int)r);
    }
}

static void TOME_CALL on_tick(tome_mod* mod, const tome_event_info* info, void* user) {
    tome_lobby_player players[12];
    int32_t count, i;
    (void)info;
    (void)user;
    count = api->lobby_players(players, 12);
    if (count < 0) {
        g_in_lobby = g_said_hello = g_held = g_press = 0;
        return;
    }
    g_in_lobby = 1;
    if (g_press) {
        g_press = 0;
        tome_logf(api, mod, TOME_LOG_INFO, "lobby: pressing Start: %d", (int)api->lobby_start(mod));
        return;
    }
    if (g_said_hello) return;
    for (i = 0; i < count && i < 12; ++i) {
        if (players[i].is_host && !players[i].is_local && players[i].runs_tome) {
            tome_result r = api->lobby_send(mod, players[i].id, "hello", 5);
            tome_logf(api, mod, TOME_LOG_INFO, "lobby: sent hello to the host (player %d, %s): %d", (int)players[i].id,
                      players[i].name, (int)r);
            g_said_hello = 1;
        }
    }
}

static int32_t TOME_CALL on_lobby_start(tome_mod* mod, char* reason, uint32_t reason_size, void* user) {
    (void)user;
    if (g_held) {
        tome_logf(api, mod, TOME_LOG_INFO, "lobby: letting the start through");
        return TOME_START_ALLOW;
    }
    g_held = 1;
    g_press = 1;
    strncpy(reason, "sync-test: holding the start once", reason_size - 1);
    tome_logf(api, mod, TOME_LOG_INFO, "lobby: holding the start once");
    return TOME_START_REFUSE;
}

static uint64_t TOME_CALL checksum(tome_mod* mod, void* user) {
    (void)mod;
    (void)user;
    return tome_hash_u32(g_state, g_skew);
}

TOME_EXPORT int TOME_CALL tome_mod_load(const tome_api* tome, tome_mod* mod) {
    (void)mod;
    api = tome;
    return tome_has_net_messages(api) && tome_has_timers(api) && tome_has_lobby(api) ? 0 : 1;
}

TOME_EXPORT void TOME_CALL tome_mod_enable(tome_mod* mod) {
    api->register_command(mod, "synctest", on_command, NULL);
    api->on_net_message(mod, on_message, NULL);
    api->on_checksum(mod, checksum, NULL);
    api->on_lobby_message(mod, on_lobby_message, NULL);
    api->on_lobby_start(mod, on_lobby_start, NULL);
    api->on_event(mod, TOME_EVENT_TICK, on_tick, NULL);
}

TOME_EXPORT void TOME_CALL tome_mod_disable(tome_mod* mod) {
    (void)mod;
}

TOME_EXPORT void TOME_CALL tome_mod_unload(tome_mod* mod) {
    (void)mod;
}
