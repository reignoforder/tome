/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * e2e fixture (tools/e2e-multiplayer.sh): the mod every player of the LAN
 * game needs. Its code tries messages between players: "/synctest <text>"
 * sends the text, "/synctest big" 1000 bytes with NULs in them; every
 * message that arrives is logged with its sender, size and a checksum.
 */
#include <tome/tome.h>

#include <string.h>

static const tome_api* api;

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

TOME_EXPORT int TOME_CALL tome_mod_load(const tome_api* tome, tome_mod* mod) {
    (void)mod;
    api = tome;
    return tome_has_net_messages(api) ? 0 : 1;
}

TOME_EXPORT void TOME_CALL tome_mod_enable(tome_mod* mod) {
    api->register_command(mod, "synctest", on_command, NULL);
    api->on_net_message(mod, on_message, NULL);
}

TOME_EXPORT void TOME_CALL tome_mod_disable(tome_mod* mod) {
    (void)mod;
}

TOME_EXPORT void TOME_CALL tome_mod_unload(tome_mod* mod) {
    (void)mod;
}
