// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/chat.h"
#include "test.h"

using namespace tome::chat;

TEST(chat_parse_command) {
    Command c;
    CHECK(parse("/tome list", c));
    CHECK_EQ(c.name, std::string("tome"));
    CHECK_EQ(c.args, std::string("list"));

    CHECK(parse("/Hello", c));
    CHECK_EQ(c.name, std::string("hello"));
    CHECK(c.args.empty());

    CHECK(parse("/my-mod_2   some  args  ", c));
    CHECK_EQ(c.name, std::string("my-mod_2"));
    CHECK_EQ(c.args, std::string("some  args"));

    CHECK(parse("/x\targ", c));
    CHECK_EQ(c.args, std::string("arg"));

    CHECK(!parse("", c));
    CHECK(!parse("hello", c));
    CHECK(!parse("/", c));
    CHECK(!parse("/ tome", c));
    CHECK(!parse("//tome", c));
    CHECK(!parse("/to.me", c));
    CHECK(!parse("/name!", c));
}

TEST(chat_names) {
    CHECK(valid_name("a"));
    CHECK(valid_name("Mod_1-x"));
    CHECK(valid_name(std::string(32, 'a')));
    CHECK(!valid_name(std::string(33, 'a')));
    CHECK(!valid_name(""));
    CHECK(!valid_name("a b"));
    CHECK(!valid_name("\xc3\xa9"));

    CHECK(reserved("tome"));
    CHECK(reserved("TOME"));
    CHECK(reserved("fps"));
    CHECK(reserved("MapName"));
    CHECK(!reserved("hello"));
}

TEST(chat_parse_tome) {
    CHECK(parse_tome("").action == TomeAction::Help);
    CHECK(parse_tome("help").action == TomeAction::Help);
    CHECK(parse_tome("LIST").action == TomeAction::List);

    TomeCommand enable = parse_tome("enable hello");
    CHECK(enable.action == TomeAction::Enable);
    CHECK_EQ(enable.id, std::string("hello"));
    CHECK(parse_tome("disable  hello ").action == TomeAction::Disable);

    TomeCommand reload_all = parse_tome("reload");
    CHECK(reload_all.action == TomeAction::Reload);
    CHECK(reload_all.id.empty());
    CHECK_EQ(parse_tome("reload hello").id, std::string("hello"));

    TomeCommand missing = parse_tome("enable");
    CHECK(missing.action == TomeAction::Invalid);
    CHECK_EQ(missing.error, std::string("usage: /tome enable <mod id>"));
    CHECK(parse_tome("frobnicate").action == TomeAction::Invalid);
    CHECK(parse_tome("enable a b").action == TomeAction::Invalid);
    CHECK(parse_tome("list everything").action == TomeAction::Invalid);
}

TEST(chat_parse_tome_logs) {
    TomeCommand logs = parse_tome("logs");
    CHECK(logs.action == TomeAction::Logs);
    CHECK(logs.filter == LogFilter::All);
    CHECK_EQ(logs.count, kLogsDefault);

    logs = parse_tome("LOGS warn 20");
    CHECK(logs.action == TomeAction::Logs);
    CHECK(logs.filter == LogFilter::Warn);
    CHECK_EQ(logs.count, size_t(20));

    CHECK(parse_tome("logs warning").filter == LogFilter::Warn);

    logs = parse_tome("log 5 errors");
    CHECK(logs.filter == LogFilter::Error);
    CHECK_EQ(logs.count, size_t(5));

    CHECK(parse_tome("logs 0").action == TomeAction::Invalid);
    CHECK(parse_tome("logs 31").action == TomeAction::Invalid);
    CHECK(parse_tome("logs -1").action == TomeAction::Invalid);
    CHECK(parse_tome("logs warn error").action == TomeAction::Invalid);
    CHECK(parse_tome("logs 5 6").action == TomeAction::Invalid);
    CHECK(parse_tome("logs everything").action == TomeAction::Invalid);
    CHECK(parse_tome("logs warn 5 x").action == TomeAction::Invalid);
}
