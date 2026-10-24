// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Minimal, dependency-free JSON reader/writer.
//
// Why a hand-written parser rather than a library: HYPERION must build on
// constrained Android/Termux-style hosts with no package manager and no
// vendored third-party tree (see DECISIONS.md D-006, the same reasoning that
// gives the test harness no dependencies). The syscall registry needs strict
// RFC 8259 parsing of a small, trusted-by-schema file — nothing more.
//
// What this parser deliberately does *not* do:
//   - It does not accept duplicate object keys. A duplicated key is a
//     configuration bug (and a classic way to smuggle a second value past a
//     validator), so it is rejected rather than silently resolved.
//   - It does not accept comments, trailing commas, NaN/Infinity, or unquoted
//     keys. The registry is machine-written; anything non-standard is a bug.
//   - It does not recurse without bound (kMaxDepth) and it does not allocate
//     without bound (the caller passes a size limit).
//
// Representation: one flat struct holding every possible payload. A tagged
// union would be smaller, but the registry is a handful of kilobytes read once
// at startup, and a flat struct has no lifetime or aliasing hazards to get
// wrong. Nothing here is on the syscall hot path — see syscall_dispatch.hpp.
#ifndef HYPERION_JSON_HPP
#define HYPERION_JSON_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hyperion::json {

enum class Type : std::uint8_t { kNull, kBool, kNumber, kString, kArray, kObject };
const char* to_string(Type type);

// A parsed JSON value.
struct Value {
    Type type = Type::kNull;
    bool boolean = false;
    // Numbers keep both a double and an exact integer when the literal is
    // integral and fits in int64. The registry addresses syscall numbers and
    // file modes, so losing exactness to a double round-trip is not acceptable.
    double number = 0.0;
    std::int64_t integer = 0;
    bool is_integer = false;
    std::string string;
    std::vector<Value> array;
    // Insertion order is preserved so a rewritten file stays diffable.
    std::vector<std::pair<std::string, Value>> object;

    bool is_null() const noexcept { return type == Type::kNull; }
    bool is_bool() const noexcept { return type == Type::kBool; }
    bool is_number() const noexcept { return type == Type::kNumber; }
    bool is_string() const noexcept { return type == Type::kString; }
    bool is_array() const noexcept { return type == Type::kArray; }
    bool is_object() const noexcept { return type == Type::kObject; }

    // Object member lookup; nullptr when absent or when this is not an object.
    const Value* find(std::string_view key) const;
    const Value* find(std::size_t index) const; // array element access

    static Value make_object();
    static Value make_array();
    static Value make_string(std::string s);
    static Value make_int(std::int64_t v);
    static Value make_bool(bool v);
    static Value make_null();

    void set(std::string key, Value value); // object insert or replace
    void push(Value value);                 // array append
};

struct ParseError {
    std::string message;
    std::size_t offset = 0; // byte offset into the parsed text
    std::size_t line = 0;   // 1-based
    std::size_t column = 0; // 1-based

    std::string format(std::string_view source_name) const;
};

// Parse `text` into `out`. On failure returns false, leaves `out` untouched and
// fills `error` with a located diagnostic. `max_bytes` bounds the input size so
// a hostile or corrupt file cannot drive an allocation storm.
bool parse(std::string_view text, Value& out, ParseError& error,
           std::size_t max_bytes = 16U * 1024U * 1024U);

// Render `value` as indented JSON with a trailing newline. Object member order
// is preserved verbatim.
std::string serialize(const Value& value, int indent_width = 2);

// Escape `s` as a JSON string literal, including the surrounding quotes.
std::string quote(std::string_view s);

} // namespace hyperion::json

#endif // HYPERION_JSON_HPP
