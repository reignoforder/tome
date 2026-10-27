#include "core/units.h"
#include "test.h"

#include <cstring>

using namespace tome::units;

namespace {

const FlagBits kBits = {0x100, 0x10000, 0x40000000};

// id("hpea") without GCC's multi-character constant warning.
constexpr uint32_t id(const char (&code)[5]) {
    return uint32_t(code[0]) << 24 | uint32_t(code[1]) << 16 | uint32_t(code[2]) << 8 | uint32_t(code[3]);
}

} // namespace

TEST(units_classify) {
    CHECK_EQ(classify(id("hpea"), 0x201, kBits), 0u);
    CHECK_EQ(classify(id("htow"), 0x10000, kBits), uint32_t(TOME_UNIT_STRUCTURE));
    CHECK_EQ(classify(id("Hpal"), 0, kBits), uint32_t(TOME_UNIT_HERO));
    CHECK_EQ(classify(id("Hpal"), 0x40000000, kBits), 0u);
    CHECK_EQ(classify(id("Hpal"), 0x100, kBits), uint32_t(TOME_UNIT_HERO | TOME_UNIT_DEAD));
    CHECK_EQ(classify(id("nfoo"), 0x10100, kBits), uint32_t(TOME_UNIT_STRUCTURE | TOME_UNIT_DEAD));
}

TEST(units_type_code) {
    char code[5];
    type_code(id("hpea"), code);
    CHECK_EQ(std::string(code), std::string("hpea"));
    CHECK_EQ(std::string(tome_type_code(id("Hpal"), code)), std::string("Hpal"));
}

TEST(units_chat_recipient) {
    CHECK_EQ(chat_recipient(0), int32_t(TOME_CHAT_ALL));
    CHECK_EQ(chat_recipient(1), int32_t(TOME_CHAT_ALLIES));
    CHECK_EQ(chat_recipient(2), int32_t(TOME_CHAT_OBSERVERS));
    CHECK_EQ(chat_recipient(3), int32_t(TOME_CHAT_PRIVATE));
    CHECK_EQ(chat_recipient(7), int32_t(TOME_CHAT_PRIVATE));
}

TEST(units_pause_player) {
    CHECK_EQ(pause_player(3, 0), 3);
    CHECK_EQ(pause_player(0xFF, 0), -1);
    CHECK_EQ(pause_player(0, 1), -1);  // PauseGame passes the local player
}

TEST(units_selection_compare) {
    Selection a, b;
    CHECK(a == b);
    a.units.push_back({0x1000, {1, 2}});
    CHECK(!(a == b));
    b.units.push_back({0x1000, {1, 2}});
    CHECK(a == b);
    b.leader = 0x1000;
    CHECK(!(a == b));
    a.leader = 0x1000;
    // Another unit at a freed unit's address.
    b.units[0].id[1] = 3;
    CHECK(!(a == b));
}

// A handle manager laid out like the game's (HandleLayout below): a hash of
// nodes {key, next (link offset 0), ..., handle id at +0x18} in 4 buckets,
// and a handle table of 12-byte entries with the object at +4.
TEST(units_find_handle) {
    static_assert(sizeof(void*) == 4, "the game's structures hold 32-bit pointers");
    const HandleLayout layout = {0x1c0, 0x1c8, 0x19c, 0x100000};
    uint32_t object_a = 0, object_b = 0, object_c = 0;  // stand-ins for units

    struct Node {
        uint32_t key, next, pad[4], id;
    };
    static_assert(offsetof(Node, id) == 0x18);
    Node nodes[3] = {};
    uint32_t buckets[4 * 3] = {};
    uint32_t entries[4 * 3] = {};
    alignas(4) unsigned char manager[0x200] = {};
    auto put = [&](uint32_t offset, uint32_t value) { std::memcpy(manager + offset, &value, 4); };
    auto addr = [](const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); };

    // Keys 5 and 9 share bucket 1 (mask 3); key 6 is in bucket 2 with no handle entry pointing back.
    nodes[0] = {9, 0, {}, 0x100001};
    nodes[1] = {5, addr(&nodes[0]), {}, 0x100002};
    nodes[2] = {6, 0, {}, 0x100003};
    buckets[1 * 3 + 2] = addr(&nodes[1]);
    buckets[2 * 3 + 2] = addr(&nodes[2]);
    entries[1 * 3 + 1] = addr(&object_b);
    entries[2 * 3 + 1] = addr(&object_a);
    entries[3 * 3 + 1] = addr(&object_b);  // the id was reused for another object
    put(0x1c0, addr(buckets));
    put(0x19c, addr(entries));

    put(0x1c8, 0xFFFFFFFF);  // empty hash
    CHECK_EQ(find_handle(manager, &object_a, 5, layout), 0u);

    put(0x1c8, 3);
    CHECK_EQ(find_handle(manager, &object_a, 5, layout), 0x100002u);
    CHECK_EQ(find_handle(manager, &object_b, 9, layout), 0x100001u);  // second in its bucket
    CHECK_EQ(find_handle(manager, &object_c, 6, layout), 0u);         // the entry names another object
    CHECK_EQ(find_handle(manager, &object_c, 13, layout), 0u);        // bucket 1, no such key
    CHECK_EQ(find_handle(manager, &object_c, 7, layout), 0u);         // empty bucket
    CHECK_EQ(find_handle(nullptr, &object_a, 5, layout), 0u);

    // A negative value ends a list too.
    nodes[0].next = 0x80000001u;
    buckets[3 * 3 + 2] = 0xFFFFFFFFu;
    CHECK_EQ(find_handle(manager, &object_c, 11, layout), 0u);
}

TEST(units_results_once_per_player) {
    Results r;
    CHECK(r.first(TOME_EVENT_PLAYER_VICTORY, 0));
    CHECK(!r.first(TOME_EVENT_PLAYER_VICTORY, 0));
    CHECK(r.first(TOME_EVENT_PLAYER_VICTORY, 1));
    CHECK(r.first(TOME_EVENT_PLAYER_LEFT, 0));
    r.reset();
    CHECK(r.first(TOME_EVENT_PLAYER_VICTORY, 0));
}
