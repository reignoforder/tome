// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The multiplayer handshake for "sync": "required" mods (docs/research.md
// §4): every player of a game needs the same set, or the game desyncs. This
// is the portable part; loader/src/handshake.cpp reads the game and calls it.
//
// In the lobby, the host sends its set as chat lines to each player who
// joins, and Tome clients answer the host with theirs. Tome hides those lines
// on both sides; a player without Tome sees the host's (readable) lines. The
// host refuses to start while a player's set differs or hasn't arrived.
//
// A client knows the host by its net id where the build maps it (the game's
// router gives a game's creator player number 1: set_host), and counts only
// the host's set. It answers every player who sends a set all the same, so a
// player pretending to host can't divert the replies the real host checks;
// it only gets a note naming it. Without the mapping, a second sender is
// noted, since the client can't tell which one hosts.
//
// In a game, every player with required mods sends one SyncStoredInteger
// action per mod right after the start. Actions reach every player and are
// recorded in the replay, so each client (and a replay viewer) compares the
// sets and says who differs.

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace tome::handshake {

// A mod's identity: id, version and a hash of its files (tome.mod.json, the
// DLL and its assets).
struct SyncMod {
    std::string id;
    std::string version;
    std::string hash;  // lowercase hex

    bool operator==(const SyncMod&) const = default;
};

// Sorted by id, each id once.
using ModSet = std::vector<SyncMod>;

void sort(ModSet& set);

// "hello 1.0.0 #1a2b3c4d"
std::string describe(const SyncMod& mod);
// "hello 1.0.0 #1a2b3c4d, other 2.0.0 #00ff00ff", or "none".
std::string describe(const ModSet& set);

// --- comparing sets ------------------------------------------------------------------

struct Difference {
    std::vector<SyncMod> missing;  // in the expected set only
    std::vector<SyncMod> extra;    // in the actual set only
    std::vector<std::pair<SyncMod, SyncMod>> changed;  // same id: expected, actual

    bool empty() const { return missing.empty() && extra.empty() && changed.empty(); }
};

Difference compare(const ModSet& expected, const ModSet& actual);

// "missing a 1.0.0 #..; extra b 2.0.0 #..; different c 1.1.0 #.. (<other>: 1.0.0 #..)",
// where <other> names the owner of the expected set ("host", "you").
std::string explain(const Difference& difference, std::string_view other);

// The most mods one player's set may have. A bigger one (from a modified
// client: nobody runs that many required mods) is dropped, so every set Tome
// keeps and compares stays small.
constexpr int kMaxMods = 256;

// How many log lines each player's handshake messages get (per lobby, or
// per game), so a modified client can't fill tome.log.
class LogBudget {
public:
    explicit LogBudget(int per_player) : limit_(per_player) {}
    enum class Verdict {
        Log,        // log it
        Exhausted,  // the first one over the limit: say that the rest aren't logged
        Quiet,      // don't log it
    };
    Verdict take(int player);
    void reset() { used_.clear(); }

private:
    int limit_;
    std::map<int, int> used_;
};

// --- lobby chat lines ----------------------------------------------------------------

constexpr std::string_view kPrefix = "[Tome] ";
// The game copies at most 0xFF bytes of a lobby chat line.
constexpr size_t kMaxLine = 250;

enum class LineKind {
    Required,  // the host's set: "[Tome] Required mods 1/2: ..."
    Reply,     // a client's answer to the host: "[Tome] My mods 1/1: ..."
};

// The set as one or more lines of at most kMaxLine bytes, "none" if it's empty.
std::vector<std::string> encode(LineKind kind, const ModSet& set);

// Whether a chat line starts like Tome's. Only lines that decode are
// hidden from the chat, so a person typing "[Tome] ..." is still read.
bool is_tome_line(std::string_view text);

struct Line {
    LineKind kind = LineKind::Required;
    int part = 0;   // 1-based
    int total = 0;
    ModSet mods;    // this line's part of the set
};

bool decode(std::string_view text, Line& out);

// Puts a set sent in several lines back together, per sender.
class Assembler {
public:
    // True when `line` completes a set (`out`). A line out of order, or of
    // another kind, starts over.
    bool add(const Line& line, ModSet& out);

private:
    LineKind kind_ = LineKind::Required;
    int next_ = 1;
    ModSet mods_;
};

// --- the lobby -----------------------------------------------------------------------

struct LobbyPlayer {
    int id = 0;  // net player id
    std::string name;
};

struct Outgoing {
    int to = 0;  // net player id
    std::string text;
};

// What one client does in one lobby. Feed it with update() (every half
// second or so) and receive(); then send take_outgoing() and show
// take_notes() in the lobby's chat.
class Lobby {
public:
    // `host`: this client hosts the game. `others`: every other player in
    // the lobby (people, not computers). `now`: seconds, any clock.
    void update(bool host, const ModSet& local, const std::vector<LobbyPlayer>& others, double now);

    // Client: the host's net id (the player whose number is 1, which a
    // game's creator always is: NetGetPlayerNumber), or -1 if unknown.
    // Known, only the host's set counts: another player who sends one is
    // named in a note and ignored (it still gets a reply, so a wrong id
    // can't keep the real host from starting). Unknown, a second sender is
    // noted, since a client can't tell which one hosts.
    void set_host(int id) { host_id_ = id; }

    // A Tome line (is_tome_line) from player `from`.
    void receive(int from, std::string_view text);

    // Host: whether the game may start. If not, `problems` says why, one
    // line per player.
    bool can_start(std::vector<std::string>& problems) const;

    std::vector<Outgoing> take_outgoing();
    std::vector<std::string> take_notes();

    // A client gives up waiting for the host's set after this many seconds.
    static constexpr double kHostWait = 5.0;

private:
    struct Peer {
        std::string name;
        bool announced = false;  // host: our current set went to this player
        bool answered = false;   // host: their reply arrived
        ModSet set;              // host: their reply
        Assembler assembler;
    };

    void send(int to, LineKind kind, const ModSet& set);
    void note(std::string text) { notes_.push_back(std::move(text)); }
    // Client: how our set compares with the one `from` sent. `first`: the
    // only sender so far (normally the host).
    void note_claim(int from, const ModSet& set, bool first);
    std::string name_of(int id) const;

    bool host_ = false;
    bool started_ = false;  // update() ran
    ModSet local_;
    std::map<int, Peer> peers_;
    double joined_at_ = 0;
    // Client side: the sets players sent us, by sender. Normally only the
    // host's; each sender gets our replies.
    std::map<int, ModSet> claims_;
    bool warned_no_host_ = false;
    int host_id_ = -1;
    std::set<int> ignored_;  // client: non-hosts whose sets were ignored (noted once each)
    std::vector<Outgoing> outgoing_;
    std::vector<std::string> notes_;
};

// --- in games: SyncStoredInteger actions ---------------------------------------------

// The game cache name and mission key of Tome's actions (the mission is the
// protocol version). The key is "id version hash", the value the number of
// mods in the sender's set.
constexpr std::string_view kCacheFile = "TomeMods";
constexpr std::string_view kMission = "1";

std::string encode_key(const SyncMod& mod);
bool decode_key(std::string_view key, SyncMod& out);

// The sets that arrived in one game, by player slot.
class GameRecord {
public:
    void reset() { pending_.clear(); complete_.clear(); }

    // One action: a mod of `sender`'s set of `total`. A total over kMaxMods
    // is ignored.
    void add(int sender, const SyncMod& mod, int total);

    // Every set that arrived whole.
    const std::map<int, ModSet>& sets() const { return complete_; }

private:
    std::map<int, std::pair<int, ModSet>> pending_;  // total, mods so far
    std::map<int, ModSet> complete_;
};

struct GamePlayer {
    int slot = 0;
    std::string name;
};

// After the start: one line per player (`players`: the people in the game,
// the local one included) whose set differs from `local`. A player who sent
// nothing counts as having no required mods. Empty when everything matches.
std::vector<std::string> game_report(const ModSet& local, int local_slot, const std::vector<GamePlayer>& players,
                                     const GameRecord& record, bool replay);

} // namespace tome::handshake
