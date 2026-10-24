// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Tests for the dependency-free JSON reader/writer used by the syscall registry.
//
// The registry is configuration written by hand and by tools, so the parser's
// job is to reject anything ambiguous rather than to guess: duplicate keys,
// trailing commas, NaN and unescaped control characters are errors, and the
// diagnostics carry a line/column so a user can find the mistake.
#include <cstdint>
#include <string>

#include "harness/test_harness.hpp"
#include "hyperion/json.hpp"

using hyperion::json::ParseError;
using hyperion::json::Type;
using hyperion::json::Value;

namespace {

bool parses(const std::string& text, Value& out) {
    ParseError error;
    return hyperion::json::parse(text, out, error);
}

std::string parse_error(const std::string& text) {
    Value out;
    ParseError error;
    if (hyperion::json::parse(text, out, error)) {
        return std::string();
    }
    return error.message;
}

} // namespace

HYPERION_TEST("json", "parses_scalars_and_nesting") {
    Value root;
    HYPERION_CHECK(parses(R"({"a": 1, "b": [true, false, null], "c": "x"})", root));
    HYPERION_CHECK(root.is_object());
    HYPERION_CHECK_EQ(root.object.size(), std::size_t{3});

    const Value* a = root.find("a");
    HYPERION_CHECK(a != nullptr);
    HYPERION_CHECK(a->is_number());
    HYPERION_CHECK(a->is_integer);
    HYPERION_CHECK_EQ(a->integer, std::int64_t{1});

    const Value* b = root.find("b");
    HYPERION_CHECK(b != nullptr);
    HYPERION_CHECK_EQ(b->array.size(), std::size_t{3});
    HYPERION_CHECK(b->array[0].type == Type::kBool);
    HYPERION_CHECK(b->array[0].boolean);
    HYPERION_CHECK(!b->array[1].boolean);
    HYPERION_CHECK(b->array[2].type == Type::kNull);

    const Value* c = root.find("c");
    HYPERION_CHECK(c != nullptr);
    HYPERION_CHECK_EQ(c->string, std::string("x"));
}

HYPERION_TEST("json", "preserves_object_order") {
    Value root;
    HYPERION_CHECK(parses(R"({"z": 1, "a": 2, "m": 3})", root));
    HYPERION_CHECK_EQ(root.object[0].first, std::string("z"));
    HYPERION_CHECK_EQ(root.object[1].first, std::string("a"));
    HYPERION_CHECK_EQ(root.object[2].first, std::string("m"));
}

HYPERION_TEST("json", "negative_and_large_integers_stay_exact") {
    Value root;
    HYPERION_CHECK(parses(R"({"neg": -100, "big": 9007199254740993})", root));
    HYPERION_CHECK_EQ(root.find("neg")->integer, std::int64_t{-100});
    // 2^53 + 1 is not representable as a double; the integer form must keep it.
    HYPERION_CHECK_EQ(root.find("big")->integer, std::int64_t{9007199254740993LL});
    HYPERION_CHECK(root.find("big")->is_integer);
}

HYPERION_TEST("json", "floats_are_marked_non_integer") {
    Value root;
    HYPERION_CHECK(parses(R"({"x": 1.5})", root));
    HYPERION_CHECK(root.find("x")->is_number());
    HYPERION_CHECK(!root.find("x")->is_integer);
    HYPERION_CHECK(root.find("x")->number > 1.4 && root.find("x")->number < 1.6);
}

HYPERION_TEST("json", "rejects_duplicate_keys") {
    HYPERION_CHECK_EQ(parse_error(R"({"a": 1, "a": 2})"), std::string("duplicate object key"));
}

HYPERION_TEST("json", "rejects_trailing_comma") {
    HYPERION_CHECK(parse_error(R"({"a": 1,})").empty() == false);
    HYPERION_CHECK(parse_error(R"([1, 2,])").empty() == false);
}

HYPERION_TEST("json", "rejects_trailing_garbage") {
    HYPERION_CHECK_EQ(parse_error(R"({"a": 1} extra)"),
                      std::string("trailing characters after top-level value"));
}

HYPERION_TEST("json", "rejects_nan_and_infinity") {
    HYPERION_CHECK(parse_error("NaN").empty() == false);
    HYPERION_CHECK(parse_error("Infinity").empty() == false);
}

HYPERION_TEST("json", "rejects_unterminated_string_and_bad_escape") {
    HYPERION_CHECK(parse_error("\"abc").empty() == false);
    HYPERION_CHECK(parse_error(R"("a\q")").empty() == false);
}

HYPERION_TEST("json", "rejects_unescaped_control_character") {
    HYPERION_CHECK_EQ(parse_error("\"a\nb\""),
                      std::string("unescaped control character in string"));
}

HYPERION_TEST("json", "decodes_escapes_and_surrogate_pairs") {
    Value root;
    HYPERION_CHECK(parses("\"a\\tb\\u0041\\uD83D\\uDE00\"", root));
    // tab, 'A', then U+1F600 as a four-byte UTF-8 sequence.
    HYPERION_CHECK_EQ(root.string, std::string("a\tbA\xF0\x9F\x98\x80"));
}

HYPERION_TEST("json", "rejects_lone_surrogate") {
    HYPERION_CHECK(parse_error("\"\\uD83D\"").empty() == false);
    HYPERION_CHECK(parse_error("\"\\uDE00\"").empty() == false);
}

HYPERION_TEST("json", "rejects_excessive_nesting") {
    std::string deep;
    for (int i = 0; i < 100; ++i) {
        deep.push_back('[');
    }
    for (int i = 0; i < 100; ++i) {
        deep.push_back(']');
    }
    HYPERION_CHECK_EQ(parse_error(deep), std::string("nesting depth limit exceeded"));
}

HYPERION_TEST("json", "reports_line_and_column") {
    Value out;
    ParseError error;
    HYPERION_CHECK(!hyperion::json::parse("{\n  \"a\": ,\n}", out, error));
    HYPERION_CHECK_EQ(error.line, std::size_t{2});
    HYPERION_CHECK(error.column > 1);
    HYPERION_CHECK(error.format("registry.json").rfind("registry.json:2:", 0) == 0);
}

HYPERION_TEST("json", "enforces_size_limit") {
    Value out;
    ParseError error;
    HYPERION_CHECK(!hyperion::json::parse("\"aaaaaaaaaa\"", out, error, 4));
    HYPERION_CHECK(error.message.find("maximum accepted size") != std::string::npos);
}

HYPERION_TEST("json", "serialize_round_trips") {
    Value root;
    HYPERION_CHECK(parses(R"({"schema": 2, "rules": [{"id": "a", "on": true}]})", root));
    const std::string text = hyperion::json::serialize(root);
    Value reparsed;
    HYPERION_CHECK(parses(text, reparsed));
    HYPERION_CHECK_EQ(reparsed.find("schema")->integer, std::int64_t{2});
    HYPERION_CHECK_EQ(reparsed.find("rules")->array.size(), std::size_t{1});
    HYPERION_CHECK(reparsed.find("rules")->array[0].find("on")->boolean);
}

HYPERION_TEST("json", "quote_escapes_control_characters") {
    HYPERION_CHECK_EQ(hyperion::json::quote("a\"b\\c"), std::string("\"a\\\"b\\\\c\""));
    HYPERION_CHECK_EQ(hyperion::json::quote(std::string("\x01")), std::string("\"\\u0001\""));
}
