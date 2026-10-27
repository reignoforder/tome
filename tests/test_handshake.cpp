#include "core/handshake.h"
#include "core/metadata.h"
#include "test.h"

using namespace tome::handshake;

namespace {

SyncMod mod(const char* id, const char* version = "1.0.0", const char* hash = "0123abcd") {
    return {id, version, hash};
}

// Every line sent to `to`, in order.
std::vector<std::string> lines_to(const std::vector<Outgoing>& out, int to) {
    std::vector<std::string> lines;
    for (const Outgoing& o : out) {
        if (o.to == to) lines.push_back(o.text);
    }
    return lines;
}

// Delivers what `from` sent to `to` (the id `to` knows it by).
void deliver(Lobby& to, int as, const std::vector<std::string>& lines) {
    for (const std::string& line : lines) to.receive(as, line);
}

bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST(handshake_describe_and_sort) {
    ModSet set = {mod("zeta"), mod("alpha", "2.0.0", "ff"), mod("zeta", "9.9.9")};
    sort(set);
    CHECK_EQ(set.size(), size_t(2));
    CHECK_EQ(set[0].id, std::string("alpha"));
    CHECK_EQ(set[1].version, std::string("1.0.0"));  // the first of a duplicated id stays
    CHECK_EQ(describe(set), std::string("alpha 2.0.0 #ff, zeta 1.0.0 #0123abcd"));
    CHECK_EQ(describe(ModSet{}), std::string("none"));
}

TEST(handshake_compare) {
    ModSet host = {mod("a"), mod("b"), mod("c", "1.0.0", "11")};
    ModSet mine = {mod("b"), mod("c", "1.1.0", "22"), mod("d")};
    Difference d = compare(host, mine);
    CHECK_EQ(d.missing.size(), size_t(1));
    CHECK_EQ(d.missing[0].id, std::string("a"));
    CHECK_EQ(d.extra.size(), size_t(1));
    CHECK_EQ(d.extra[0].id, std::string("d"));
    CHECK_EQ(d.changed.size(), size_t(1));
    CHECK_EQ(explain(d, "host"), std::string("missing a 1.0.0 #0123abcd; extra d 1.0.0 #0123abcd; "
                                             "different c 1.1.0 #22 (host: 1.0.0 #11)"));
    CHECK(compare(host, host).empty());
    CHECK(compare({}, {}).empty());
    // Only the hash differs: still a different mod.
    CHECK_EQ(compare({mod("a", "1.0.0", "1")}, {mod("a", "1.0.0", "2")}).changed.size(), size_t(1));
}

TEST(handshake_lines_round_trip) {
    ModSet set = {mod("hello", "1.2.3-beta.1+build", "deadbeef"), mod("other_mod", "2.0.0", "00ff00ff")};
    std::vector<std::string> lines = encode(LineKind::Required, set);
    CHECK_EQ(lines.size(), size_t(1));
    CHECK_EQ(lines[0], std::string("[Tome] Required mods 1/1: hello 1.2.3-beta.1+build #deadbeef, "
                                   "other_mod 2.0.0 #00ff00ff"));
    Line line;
    CHECK(decode(lines[0], line));
    CHECK(line.kind == LineKind::Required);
    CHECK_EQ(line.part, 1);
    CHECK_EQ(line.total, 1);
    CHECK(line.mods == set);

    std::vector<std::string> none = encode(LineKind::Reply, {});
    CHECK_EQ(none.size(), size_t(1));
    CHECK_EQ(none[0], std::string("[Tome] My mods 1/1: none"));
    CHECK(decode(none[0], line));
    CHECK(line.kind == LineKind::Reply);
    CHECK(line.mods.empty());
}

TEST(handshake_long_sets_split) {
    ModSet set;
    for (int i = 0; i < 30; ++i) set.push_back(mod(("mod-number-" + std::to_string(i)).c_str(), "10.20.30", "0123456789abcdef"));
    sort(set);
    std::vector<std::string> lines = encode(LineKind::Reply, set);
    CHECK(lines.size() > 1);
    Assembler assembler;
    ModSet out;
    for (size_t i = 0; i < lines.size(); ++i) {
        CHECK(lines[i].size() <= kMaxLine);
        Line line;
        CHECK(decode(lines[i], line));
        CHECK_EQ(line.total, int(lines.size()));
        CHECK_EQ(assembler.add(line, out), i + 1 == lines.size());
    }
    CHECK(out == set);

    // A missing part starts over: the next set still arrives whole.
    Line first, last;
    CHECK(decode(lines.front(), first));
    CHECK(decode(lines.back(), last));
    CHECK(!assembler.add(first, out));
    CHECK(!assembler.add(last, out));
    for (const std::string& text : lines) {
        Line line;
        decode(text, line);
        assembler.add(line, out);
    }
    CHECK(out == set);
}

TEST(handshake_decode_rejects) {
    Line line;
    CHECK(!decode("hello", line));
    CHECK(!decode("[Tome] Something else 1/1: none", line));
    CHECK(!decode("[Tome] Required mods 2/1: none", line));
    CHECK(!decode("[Tome] Required mods 1/2: none", line));  // "none" is a whole set
    CHECK(!decode("[Tome] Required mods 1/1: ", line));
    CHECK(!decode("[Tome] Required mods 1/1: a 1.0.0 0123", line));
    CHECK(!decode("[Tome] Required mods 1/1: a 1.0.0 #XYZ", line));
    CHECK(!decode("[Tome] Required mods x/1: a 1.0.0 #01", line));
    CHECK(!decode("[Tome] Required mods 1/1: a 1.0.0 #01,", line));
    CHECK(is_tome_line("[Tome] anything"));
    CHECK(!is_tome_line("Tome: anything"));
}

TEST(handshake_lobby_match) {
    ModSet set = {mod("hello")};
    Lobby host, client;
    // The host knows the client as 2, the client the host as 1.
    host.update(true, set, {{2, "Bob"}}, 0);
    client.update(false, set, {{1, "Alice"}}, 0);
    std::vector<std::string> problems;
    CHECK(!host.can_start(problems));
    CHECK_EQ(problems.size(), size_t(1));
    CHECK(contains(problems[0], "Bob hasn't sent"));

    deliver(client, 1, lines_to(host.take_outgoing(), 2));
    std::vector<std::string> notes = client.take_notes();
    CHECK_EQ(notes.size(), size_t(1));
    CHECK(contains(notes[0], "match the host's"));
    deliver(host, 2, lines_to(client.take_outgoing(), 1));
    notes = host.take_notes();
    CHECK_EQ(notes.size(), size_t(1));
    CHECK(contains(notes[0], "Bob has the same required mods"));
    CHECK(host.can_start(problems));

    // Nothing more is sent while nothing changes.
    host.update(true, set, {{2, "Bob"}}, 1);
    client.update(false, set, {{1, "Alice"}}, 1);
    CHECK(host.take_outgoing().empty());
    CHECK(client.take_outgoing().empty());
}

TEST(handshake_lobby_mismatch_and_change) {
    Lobby host, client;
    host.update(true, {mod("hello")}, {{2, "Bob"}}, 0);
    client.update(false, {mod("hello", "1.1.0", "99")}, {{1, "Alice"}}, 0);
    deliver(client, 1, lines_to(host.take_outgoing(), 2));
    std::vector<std::string> notes = client.take_notes();
    CHECK_EQ(notes.size(), size_t(1));
    CHECK(contains(notes[0], "differ from the host's: different hello 1.1.0 #99 (host: 1.0.0 #0123abcd)"));
    deliver(host, 2, lines_to(client.take_outgoing(), 1));
    std::vector<std::string> problems;
    CHECK(!host.can_start(problems));
    CHECK(contains(problems[0], "Bob: different hello 1.1.0"));
    CHECK(contains(host.take_notes()[0], "can't start"));

    // Bob updates the mod: his new set goes to the host on its own.
    client.update(false, {mod("hello")}, {{1, "Alice"}}, 1);
    deliver(host, 2, lines_to(client.take_outgoing(), 1));
    CHECK(host.can_start(problems));

    // The host turns its mod off: it tells Bob, who now has an extra one.
    host.update(true, {}, {{2, "Bob"}}, 2);
    std::vector<std::string> sent = lines_to(host.take_outgoing(), 2);
    CHECK_EQ(sent.size(), size_t(1));
    CHECK(contains(sent[0], ": none"));
    deliver(client, 1, sent);
    deliver(host, 2, lines_to(client.take_outgoing(), 1));
    CHECK(!host.can_start(problems));
    CHECK(contains(problems[0], "extra hello"));
}

TEST(handshake_lobby_quiet_host) {
    // A host without required mods sends nothing and lets anyone start.
    Lobby host;
    host.update(true, {}, {{2, "Bob"}, {3, "Carol"}}, 0);
    CHECK(host.take_outgoing().empty());
    std::vector<std::string> problems;
    CHECK(host.can_start(problems));

    // A client with required mods warns once when no set arrives.
    Lobby client;
    client.update(false, {mod("hello")}, {{1, "Alice"}}, 10);
    client.update(false, {mod("hello")}, {{1, "Alice"}}, 10 + Lobby::kHostWait - 0.1);
    CHECK(client.take_notes().empty());
    client.update(false, {mod("hello")}, {{1, "Alice"}}, 10 + Lobby::kHostWait);
    std::vector<std::string> notes = client.take_notes();
    CHECK_EQ(notes.size(), size_t(1));
    CHECK(contains(notes[0], "didn't send"));
    client.update(false, {mod("hello")}, {{1, "Alice"}}, 30);
    CHECK(client.take_notes().empty());
    CHECK(client.take_outgoing().empty());  // nothing goes to a host that never asked
}

TEST(handshake_lobby_players_come_and_go) {
    Lobby host;
    ModSet set = {mod("hello")};
    host.update(true, set, {{2, "Bob"}}, 0);
    CHECK_EQ(lines_to(host.take_outgoing(), 2).size(), size_t(1));
    // Carol joins: only she gets the set.
    host.update(true, set, {{2, "Bob"}, {3, "Carol"}}, 1);
    std::vector<Outgoing> out = host.take_outgoing();
    CHECK_EQ(out.size(), size_t(1));
    CHECK_EQ(out[0].to, 3);
    // Bob leaves without answering; Carol answers: the game may start.
    host.update(true, set, {{3, "Carol"}}, 2);
    deliver(host, 3, encode(LineKind::Reply, set));
    std::vector<std::string> problems;
    CHECK(host.can_start(problems));
    // Someone joins again under Bob's old id: a new player, asked again.
    host.update(true, set, {{2, "Dave"}, {3, "Carol"}}, 3);
    CHECK_EQ(lines_to(host.take_outgoing(), 2).size(), size_t(1));
    CHECK(!host.can_start(problems));
    CHECK(contains(problems[0], "Dave"));
}

TEST(handshake_game_keys) {
    SyncMod m = mod("hello", "1.0.0", "abc");
    CHECK_EQ(encode_key(m), std::string("hello 1.0.0 #abc"));
    SyncMod back;
    CHECK(decode_key(encode_key(m), back));
    CHECK(back == m);
    CHECK(!decode_key("hello 1.0.0", back));
    CHECK(!decode_key("", back));
}

TEST(handshake_game_record_and_report) {
    GameRecord record;
    record.add(0, mod("a"), 2);
    CHECK(record.sets().empty());
    record.add(0, mod("a"), 2);  // a repeat doesn't count twice
    CHECK(record.sets().empty());
    record.add(0, mod("b"), 2);
    CHECK_EQ(record.sets().size(), size_t(1));
    CHECK_EQ(record.sets().at(0).size(), size_t(2));
    record.add(1, mod("a"), 1);

    ModSet local = {mod("a"), mod("b")};
    std::vector<GamePlayer> players = {{0, "Alice"}, {1, "Bob"}, {2, "Carol"}};
    std::vector<std::string> lines = game_report(local, 0, players, record, false);
    CHECK_EQ(lines.size(), size_t(2));
    CHECK_EQ(lines[0], std::string("Bob: missing b 1.0.0 #0123abcd"));
    CHECK_EQ(lines[1], std::string("Carol: missing a 1.0.0 #0123abcd, b 1.0.0 #0123abcd"));

    // A set counts once it's whole; later actions from that player don't change it.
    record.add(1, mod("b"), 2);
    CHECK_EQ(record.sets().at(1).size(), size_t(1));

    // Everyone matching (Carol isn't in the game): nothing to say.
    GameRecord matching;
    matching.add(1, mod("a"), 3);
    matching.add(1, mod("b"), 2);  // a different total starts the player's set over
    matching.add(1, mod("a"), 2);
    matching.add(0, mod("b"), 2);
    matching.add(0, mod("a"), 2);
    CHECK(game_report(local, 0, {{0, "Alice"}, {1, "Bob"}}, matching, false).empty());

    // Without required mods, players who sent nothing match.
    GameRecord empty;
    CHECK(game_report({}, 0, players, empty, false).empty());

    // A replay compares every recorded player with the viewer's set.
    lines = game_report({}, 0, {{0, "Alice"}}, record, true);
    CHECK_EQ(lines.size(), size_t(1));
    CHECK(contains(lines[0], "In this replay, Alice: extra a"));
}

TEST(handshake_lobby_two_senders) {
    // Carol (3) pretends to host: Bob answers both her and the real host
    // (Alice, 1), so Alice's check still sees his mods.
    Lobby client;
    ModSet set = {mod("hello")};
    client.update(false, set, {{1, "Alice"}, {3, "Carol"}}, 0);
    deliver(client, 3, encode(LineKind::Required, {}));
    deliver(client, 1, encode(LineKind::Required, set));
    std::vector<std::string> notes = client.take_notes();
    CHECK_EQ(notes.size(), size_t(2));
    CHECK(contains(notes[1], "Alice also sent required mods, but only the host's count. Yours match Alice's."));
    std::vector<Outgoing> out = client.take_outgoing();
    CHECK_EQ(lines_to(out, 1).size(), size_t(1));
    CHECK_EQ(lines_to(out, 3).size(), size_t(1));

    // A change of mods goes to both; once Carol leaves, only to Alice.
    client.update(false, {}, {{1, "Alice"}, {3, "Carol"}}, 1);
    out = client.take_outgoing();
    CHECK_EQ(lines_to(out, 1).size(), size_t(1));
    CHECK_EQ(lines_to(out, 3).size(), size_t(1));
    client.update(false, set, {{1, "Alice"}}, 2);
    out = client.take_outgoing();
    CHECK_EQ(out.size(), size_t(1));
    CHECK_EQ(out[0].to, 1);
}

TEST(handshake_typed_lines_stay_visible) {
    // Lines a person types that only look like Tome's don't decode, so
    // they're shown as usual (the hook hides only decoded lines).
    Line line;
    CHECK(is_tome_line("[Tome] anyone have this mod?"));
    CHECK(!decode("[Tome] anyone have this mod?", line));
    CHECK(!decode("[Tome] My mods are great", line));
}

TEST(handshake_longest_entry_fits) {
    // The longest id and version tome.mod.json allows, with a full hash:
    // still one entry per line, within the game's limit.
    std::string id(tome::kMaxIdLength, 'a'), version = "1.0.0-" + std::string(tome::kMaxVersionLength - 6, 'b');
    ModSet set = {{id, version, "0123456789abcdef"}, {id + "x", version, "0123456789abcdef"}};
    for (LineKind kind : {LineKind::Required, LineKind::Reply}) {
        std::vector<std::string> lines = encode(kind, set);
        CHECK_EQ(lines.size(), size_t(2));
        for (const std::string& text : lines) {
            CHECK(text.size() <= kMaxLine);
            Line line;
            CHECK(decode(text, line));
        }
    }
}
