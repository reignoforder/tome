// SPDX-License-Identifier: GPL-3.0-or-later
#include "handshake.h"

#include <algorithm>

namespace tome::handshake {
namespace {

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
    return text;
}

// A positive decimal number of at most 4 digits, or -1.
int small_number(std::string_view text) {
    if (text.empty() || text.size() > 4) return -1;
    int value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return -1;
        value = value * 10 + (c - '0');
    }
    return value;
}

bool valid_word(std::string_view word) {
    if (word.empty()) return false;
    for (char c : word) {
        if (c == ' ' || c == ',' || c == '#' || static_cast<unsigned char>(c) < 0x20) return false;
    }
    return true;
}

bool valid_hash(std::string_view hash) {
    if (hash.empty() || hash.size() > 16) return false;
    for (char c : hash) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// "id version #hash"
bool parse_entry(std::string_view text, SyncMod& out) {
    text = trim(text);
    size_t first = text.find(' ');
    if (first == std::string_view::npos) return false;
    size_t second = text.find(' ', first + 1);
    if (second == std::string_view::npos) return false;
    std::string_view id = text.substr(0, first), version = text.substr(first + 1, second - first - 1),
                     hash = text.substr(second + 1);
    if (!valid_word(id) || !valid_word(version) || hash.size() < 2 || hash[0] != '#' || !valid_hash(hash.substr(1))) {
        return false;
    }
    out = {std::string(id), std::string(version), std::string(hash.substr(1))};
    return true;
}

std::string_view kind_label(LineKind kind) {
    return kind == LineKind::Required ? "Required mods" : "My mods";
}

} // namespace

void sort(ModSet& set) {
    std::stable_sort(set.begin(), set.end(), [](const SyncMod& a, const SyncMod& b) { return a.id < b.id; });
    set.erase(std::unique(set.begin(), set.end(), [](const SyncMod& a, const SyncMod& b) { return a.id == b.id; }),
              set.end());
}

std::string describe(const SyncMod& mod) {
    return mod.id + " " + mod.version + " #" + mod.hash;
}

std::string describe(const ModSet& set) {
    if (set.empty()) return "none";
    std::string out;
    for (const SyncMod& mod : set) out += (out.empty() ? "" : ", ") + describe(mod);
    return out;
}

Difference compare(const ModSet& expected, const ModSet& actual) {
    Difference d;
    for (const SyncMod& want : expected) {
        auto have = std::find_if(actual.begin(), actual.end(), [&](const SyncMod& m) { return m.id == want.id; });
        if (have == actual.end()) {
            d.missing.push_back(want);
        } else if (!(*have == want)) {
            d.changed.emplace_back(want, *have);
        }
    }
    for (const SyncMod& have : actual) {
        bool known = std::any_of(expected.begin(), expected.end(), [&](const SyncMod& m) { return m.id == have.id; });
        if (!known) d.extra.push_back(have);
    }
    return d;
}

std::string explain(const Difference& difference, std::string_view other) {
    std::string out;
    auto part = [&](const std::string& text) { out += (out.empty() ? "" : "; ") + text; };
    if (!difference.missing.empty()) part("missing " + describe(difference.missing));
    if (!difference.extra.empty()) part("extra " + describe(difference.extra));
    for (const auto& [want, have] : difference.changed) {
        part("different " + describe(have) + " (" + std::string(other) + ": " + want.version + " #" + want.hash + ")");
    }
    return out;
}

// --- lobby chat lines ----------------------------------------------------------------

std::vector<std::string> encode(LineKind kind, const ModSet& set) {
    // Entries per line first, then the "n/total" headers.
    std::vector<std::string> bodies;
    const size_t header = kPrefix.size() + kind_label(kind).size() + sizeof(" 99/99: ") - 1;
    std::string body;
    for (const SyncMod& mod : set) {
        std::string entry = describe(mod);
        if (!body.empty() && header + body.size() + 2 + entry.size() > kMaxLine) {
            bodies.push_back(std::move(body));
            body.clear();
        }
        body += (body.empty() ? "" : ", ") + entry;
    }
    if (!body.empty() || bodies.empty()) bodies.push_back(body.empty() ? "none" : std::move(body));
    std::vector<std::string> lines;
    for (size_t i = 0; i < bodies.size(); ++i) {
        lines.push_back(std::string(kPrefix) + std::string(kind_label(kind)) + " " + std::to_string(i + 1) + "/" +
                        std::to_string(bodies.size()) + ": " + bodies[i]);
    }
    return lines;
}

bool is_tome_line(std::string_view text) {
    return starts_with(text, kPrefix);
}

bool decode(std::string_view text, Line& out) {
    if (!is_tome_line(text)) return false;
    text.remove_prefix(kPrefix.size());
    if (starts_with(text, kind_label(LineKind::Required))) {
        out.kind = LineKind::Required;
    } else if (starts_with(text, kind_label(LineKind::Reply))) {
        out.kind = LineKind::Reply;
    } else {
        return false;
    }
    text.remove_prefix(kind_label(out.kind).size());
    if (!starts_with(text, " ")) return false;
    text.remove_prefix(1);
    size_t colon = text.find(": ");
    size_t slash = text.find('/');
    if (colon == std::string_view::npos || slash == std::string_view::npos || slash > colon) return false;
    out.part = small_number(text.substr(0, slash));
    out.total = small_number(text.substr(slash + 1, colon - slash - 1));
    if (out.part < 1 || out.total < out.part) return false;
    std::string_view list = text.substr(colon + 2);
    out.mods.clear();
    if (trim(list) == "none") return out.total == 1;
    for (;;) {
        size_t comma = list.find(',');
        SyncMod mod;
        if (!parse_entry(list.substr(0, comma), mod)) return false;
        out.mods.push_back(std::move(mod));
        if (comma == std::string_view::npos) return true;
        list = list.substr(comma + 1);
    }
}

bool Assembler::add(const Line& line, ModSet& out) {
    if (line.part == 1 || line.kind != kind_ || line.part != next_) {
        mods_.clear();
        next_ = 1;
        kind_ = line.kind;
        if (line.part != 1) return false;
    }
    mods_.insert(mods_.end(), line.mods.begin(), line.mods.end());
    if (mods_.size() > static_cast<size_t>(kMaxMods)) {  // too big to be real: dropped
        mods_.clear();
        next_ = 1;
        return false;
    }
    if (line.part < line.total) {
        next_ = line.part + 1;
        return false;
    }
    out = std::move(mods_);
    sort(out);
    mods_.clear();
    next_ = 1;
    return true;
}

// --- the lobby -----------------------------------------------------------------------

void Lobby::send(int to, LineKind kind, const ModSet& set) {
    for (std::string& line : encode(kind, set)) outgoing_.push_back({to, std::move(line)});
}

std::string Lobby::name_of(int id) const {
    auto peer = peers_.find(id);
    return peer != peers_.end() && !peer->second.name.empty() ? peer->second.name : "player " + std::to_string(id);
}

void Lobby::update(bool host, const ModSet& local, const std::vector<LobbyPlayer>& others, double now) {
    if (!started_) {
        started_ = true;
        joined_at_ = now;
    }
    bool changed = local != local_;
    local_ = local;
    host_ = host;

    // Players who left are forgotten; new ones are added.
    auto present = [&](int id) {
        return std::any_of(others.begin(), others.end(), [&](const LobbyPlayer& p) { return p.id == id; });
    };
    for (auto it = peers_.begin(); it != peers_.end();) it = present(it->first) ? std::next(it) : peers_.erase(it);
    for (auto it = claims_.begin(); it != claims_.end();) it = present(it->first) ? std::next(it) : claims_.erase(it);
    for (const LobbyPlayer& p : others) peers_[p.id].name = p.name;

    if (host_) {
        if (changed) {
            for (auto& [id, peer] : peers_) peer.announced = false;
        }
        // A host without required mods stays quiet, so players without Tome
        // see nothing, unless it told someone about a set before.
        for (auto& [id, peer] : peers_) {
            if (peer.announced || (local_.empty() && !peer.answered)) continue;
            send(id, LineKind::Required, local_);
            peer.announced = true;
        }
        return;
    }

    if (changed) {
        for (const auto& [id, set] : claims_) send(id, LineKind::Reply, local_);
    }
    if (claims_.empty() && !local_.empty() && !warned_no_host_ && now - joined_at_ >= kHostWait) {
        warned_no_host_ = true;
        note("The host didn't send its required mods (it may not run Tome). Yours (" + describe(local_) +
             ") run only for you, and the game may desync.");
    }
}

void Lobby::receive(int from, std::string_view text) {
    Line line;
    if (!decode(text, line)) return;
    Peer& peer = peers_[from];
    ModSet set;
    if (!peer.assembler.add(line, set)) return;

    if (host_) {
        if (line.kind != LineKind::Reply) return;
        peer.answered = true;
        peer.set = set;
        Difference d = compare(local_, set);
        if (d.empty()) {
            if (!local_.empty()) note(name_of(from) + " has the same required mods.");
        } else {
            note(name_of(from) + ": " + explain(d, "host") + ". The game can't start until they match.");
        }
        return;
    }

    if (line.kind != LineKind::Required) return;
    bool first = claims_.empty() || (claims_.size() == 1 && claims_.count(from));
    claims_[from] = set;
    Difference d = compare(set, local_);
    if (!first) {
        // Only the host's set counts; this client can't tell which one that is.
        note(name_of(from) + " also sent required mods, but only the host's count. " +
             (d.empty() ? "Yours match " + name_of(from) + "'s."
                        : "Yours differ from " + name_of(from) + "'s: " + explain(d, name_of(from)) + "."));
    } else if (d.empty()) {
        note(set.empty() ? "The host has no required mods, like you."
                         : "Your required mods match the host's (" + describe(set) + ").");
    } else {
        note("Your required mods differ from the host's: " + explain(d, "host") +
             ". The host can't start the game until they match.");
    }
    send(from, LineKind::Reply, local_);
}

bool Lobby::can_start(std::vector<std::string>& problems) const {
    problems.clear();
    for (const auto& [id, peer] : peers_) {
        if (peer.answered) {
            Difference d = compare(local_, peer.set);
            if (!d.empty()) problems.push_back(name_of(id) + ": " + explain(d, "host"));
        } else if (!local_.empty()) {
            problems.push_back(name_of(id) + " hasn't sent their mods (no Tome, or not yet)");
        }
    }
    return problems.empty();
}

std::vector<Outgoing> Lobby::take_outgoing() {
    std::vector<Outgoing> out;
    out.swap(outgoing_);
    return out;
}

std::vector<std::string> Lobby::take_notes() {
    std::vector<std::string> out;
    out.swap(notes_);
    return out;
}

// --- in games ------------------------------------------------------------------------

std::string encode_key(const SyncMod& mod) {
    return mod.id + " " + mod.version + " #" + mod.hash;
}

bool decode_key(std::string_view key, SyncMod& out) {
    return parse_entry(key, out);
}

LogBudget::Verdict LogBudget::take(int player) {
    int& used = used_[player];
    if (used > limit_) return Verdict::Quiet;
    ++used;
    return used > limit_ ? Verdict::Exhausted : Verdict::Log;
}

void GameRecord::add(int sender, const SyncMod& mod, int total) {
    if (total < 1 || total > kMaxMods || complete_.count(sender)) return;
    auto& [expected, mods] = pending_[sender];
    if (expected != total) {
        expected = total;
        mods.clear();
    }
    if (std::none_of(mods.begin(), mods.end(), [&](const SyncMod& m) { return m.id == mod.id; })) mods.push_back(mod);
    if (static_cast<int>(mods.size()) < total) return;
    sort(mods);
    complete_[sender] = std::move(mods);
    pending_.erase(sender);
}

std::vector<std::string> game_report(const ModSet& local, int local_slot, const std::vector<GamePlayer>& players,
                                     const GameRecord& record, bool replay) {
    std::vector<std::string> lines;
    static const ModSet kNone;
    for (const GamePlayer& p : players) {
        if (!replay && p.slot == local_slot) continue;
        auto sent = record.sets().find(p.slot);
        const ModSet& theirs = sent != record.sets().end() ? sent->second : kNone;
        Difference d = compare(local, theirs);
        if (d.empty()) continue;
        lines.push_back((replay ? "In this replay, " : "") + p.name + ": " + explain(d, "you"));
    }
    return lines;
}

} // namespace tome::handshake
