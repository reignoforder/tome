// SPDX-License-Identifier: GPL-3.0-or-later
#include "edition.h"

namespace tome {

Edition edition_from_game(int32_t value) {
    if (value == 0) return Edition::ReignOfChaos;
    if (value == 1) return Edition::FrozenThrone;
    return Edition::Unknown;
}

const char* edition_name(Edition edition) {
    switch (edition) {
    case Edition::ReignOfChaos: return "Reign of Chaos";
    case Edition::FrozenThrone: return "The Frozen Throne";
    case Edition::Unknown: break;
    }
    return "an unknown edition";
}

bool parse_edition(std::string_view text, Edition& out) {
    if (text == "roc") {
        out = Edition::ReignOfChaos;
        return true;
    }
    if (text == "tft") {
        out = Edition::FrozenThrone;
        return true;
    }
    return false;
}

bool edition_matches(Edition wanted, Edition running) {
    return wanted == Edition::Unknown || running == Edition::Unknown || wanted == running;
}

} // namespace tome
