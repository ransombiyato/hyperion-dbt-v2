// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Strict JSON reader/writer. See include/hyperion/json.hpp for the contract.
#include "hyperion/json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace hyperion::json {

namespace {

constexpr std::size_t kMaxDepth = 64;

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7FU) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFU) {
        out.push_back(static_cast<char>(0xC0U | (cp >> 6)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp <= 0xFFFFU) {
        out.push_back(static_cast<char>(0xE0U | (cp >> 12)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (cp >> 18)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

class Parser {
public:
    Parser(std::string_view text, ParseError& error) : text_(text), error_(error) {}

    bool run(Value& out) {
        skip_whitespace();
        if (!parse_value(out, 0)) {
            return false;
        }
        skip_whitespace();
        if (pos_ != text_.size()) {
            return fail("trailing characters after top-level value");
        }
        return true;
    }

private:
    bool fail(const char* message) {
        if (error_.message.empty()) {
            error_.message = message;
            error_.offset = pos_;
            // Recompute line/column from the start so the diagnostic points at
            // the actual byte rather than the end of the document.
            std::size_t line = 1;
            std::size_t column = 1;
            for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
                if (text_[i] == '\n') {
                    ++line;
                    column = 1;
                } else {
                    ++column;
                }
            }
            error_.line = line;
            error_.column = column;
        }
        return false;
    }

    bool at_end() const { return pos_ >= text_.size(); }
    char peek() const { return at_end() ? '\0' : text_[pos_]; }

    void skip_whitespace() {
        while (!at_end()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool literal(std::string_view word) {
        if (text_.compare(pos_, word.size(), word) != 0) {
            return fail("invalid literal");
        }
        pos_ += word.size();
        return true;
    }

    bool parse_value(Value& out, std::size_t depth) {
        if (depth > kMaxDepth) {
            return fail("nesting depth limit exceeded");
        }
        if (at_end()) {
            return fail("unexpected end of input");
        }
        switch (peek()) {
        case '{':
            return parse_object(out, depth);
        case '[':
            return parse_array(out, depth);
        case '"':
            out.type = Type::kString;
            return parse_string(out.string);
        case 't':
            out.type = Type::kBool;
            out.boolean = true;
            return literal("true");
        case 'f':
            out.type = Type::kBool;
            out.boolean = false;
            return literal("false");
        case 'n':
            out.type = Type::kNull;
            return literal("null");
        default:
            return parse_number(out);
        }
    }

    bool parse_object(Value& out, std::size_t depth) {
        out.type = Type::kObject;
        out.object.clear();
        ++pos_; // '{'
        skip_whitespace();
        if (peek() == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_whitespace();
            if (peek() != '"') {
                return fail("object key must be a string");
            }
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            skip_whitespace();
            if (peek() != ':') {
                return fail("expected ':' after object key");
            }
            ++pos_;
            skip_whitespace();
            Value member;
            if (!parse_value(member, depth + 1)) {
                return false;
            }
            // A duplicate key is rejected: it is ambiguous configuration and a
            // well-known way to smuggle a second value past a validator.
            if (out.find(key) != nullptr) {
                return fail("duplicate object key");
            }
            out.object.emplace_back(std::move(key), std::move(member));
            skip_whitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or '}' in object");
        }
    }

    bool parse_array(Value& out, std::size_t depth) {
        out.type = Type::kArray;
        out.array.clear();
        ++pos_; // '['
        skip_whitespace();
        if (peek() == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_whitespace();
            Value element;
            if (!parse_value(element, depth + 1)) {
                return false;
            }
            out.array.push_back(std::move(element));
            skip_whitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or ']' in array");
        }
    }

    bool parse_hex4(std::uint32_t& out) {
        if (pos_ + 4 > text_.size()) {
            return fail("truncated \\u escape");
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = hex_value(text_[pos_ + static_cast<std::size_t>(i)]);
            if (digit < 0) {
                return fail("invalid hex digit in \\u escape");
            }
            value = (value << 4) | static_cast<std::uint32_t>(digit);
        }
        pos_ += 4;
        out = value;
        return true;
    }

    bool parse_string(std::string& out) {
        out.clear();
        if (peek() != '"') {
            return fail("expected string");
        }
        ++pos_;
        while (true) {
            if (at_end()) {
                return fail("unterminated string");
            }
            const char c = text_[pos_];
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c == '\\') {
                ++pos_;
                if (at_end()) {
                    return fail("unterminated escape");
                }
                const char esc = text_[pos_++];
                switch (esc) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!parse_hex4(cp)) {
                        return false;
                    }
                    if (cp >= 0xD800U && cp <= 0xDBFFU) {
                        // High surrogate: a low surrogate must follow.
                        if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' ||
                            text_[pos_ + 1] != 'u') {
                            return fail("lone high surrogate in \\u escape");
                        }
                        pos_ += 2;
                        std::uint32_t low = 0;
                        if (!parse_hex4(low)) {
                            return false;
                        }
                        if (low < 0xDC00U || low > 0xDFFFU) {
                            return fail("invalid low surrogate in \\u escape");
                        }
                        cp = 0x10000U + ((cp - 0xD800U) << 10) + (low - 0xDC00U);
                    } else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
                        return fail("lone low surrogate in \\u escape");
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    return fail("invalid escape character");
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20U) {
                return fail("unescaped control character in string");
            }
            out.push_back(c);
            ++pos_;
        }
    }

    bool parse_number(Value& out) {
        const std::size_t start = pos_;
        bool integral = true;
        if (!at_end() && (peek() == '-' || peek() == '+')) {
            // A leading '+' is not valid JSON; reject it explicitly rather than
            // letting strtod accept a syntax the schema never promised.
            if (peek() == '+') {
                return fail("leading '+' is not valid JSON");
            }
            ++pos_;
        }
        while (!at_end()) {
            const char c = text_[pos_];
            if (c >= '0' && c <= '9') {
                ++pos_;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                integral = false;
                ++pos_;
            } else {
                break;
            }
        }
        if (pos_ == start) {
            return fail("unexpected character");
        }
        const std::string literal(text_.substr(start, pos_ - start));
        out.type = Type::kNumber;
        out.is_integer = false;
        out.integer = 0;
        out.number = 0.0;
        if (integral) {
            errno = 0;
            char* end = nullptr;
            const long long v = std::strtoll(literal.c_str(), &end, 10);
            if (errno == 0 && end != nullptr && *end == '\0') {
                out.is_integer = true;
                out.integer = static_cast<std::int64_t>(v);
                out.number = static_cast<double>(v);
                return true;
            }
            // Out of int64 range: fall through to the double representation so
            // the caller can still report a meaningful type error.
        }
        errno = 0;
        char* end = nullptr;
        const double d = std::strtod(literal.c_str(), &end);
        if (end == nullptr || *end != '\0') {
            return fail("malformed number");
        }
        if (!std::isfinite(d)) {
            return fail("number is not finite");
        }
        out.number = d;
        return true;
    }

    std::string_view text_;
    ParseError& error_;
    std::size_t pos_ = 0;
};

void serialize_into(const Value& value, std::string& out, int indent_width, int depth) {
    const auto newline_indent = [&](int d) {
        out.push_back('\n');
        out.append(static_cast<std::size_t>(indent_width) * static_cast<std::size_t>(d), ' ');
    };
    switch (value.type) {
    case Type::kNull:
        out.append("null");
        break;
    case Type::kBool:
        out.append(value.boolean ? "true" : "false");
        break;
    case Type::kNumber:
        if (value.is_integer) {
            out.append(std::to_string(value.integer));
        } else {
            char buf[64];
            const int n = std::snprintf(buf, sizeof(buf), "%.17g", value.number);
            if (n > 0 && static_cast<std::size_t>(n) < sizeof(buf)) {
                out.append(buf, static_cast<std::size_t>(n));
            } else {
                out.append("0");
            }
        }
        break;
    case Type::kString:
        out.append(quote(value.string));
        break;
    case Type::kArray:
        if (value.array.empty()) {
            out.append("[]");
            break;
        }
        out.push_back('[');
        for (std::size_t i = 0; i < value.array.size(); ++i) {
            if (i != 0) {
                out.push_back(',');
            }
            newline_indent(depth + 1);
            serialize_into(value.array[i], out, indent_width, depth + 1);
        }
        newline_indent(depth);
        out.push_back(']');
        break;
    case Type::kObject:
        if (value.object.empty()) {
            out.append("{}");
            break;
        }
        out.push_back('{');
        for (std::size_t i = 0; i < value.object.size(); ++i) {
            if (i != 0) {
                out.push_back(',');
            }
            newline_indent(depth + 1);
            out.append(quote(value.object[i].first));
            out.append(": ");
            serialize_into(value.object[i].second, out, indent_width, depth + 1);
        }
        newline_indent(depth);
        out.push_back('}');
        break;
    }
}

} // namespace

const char* to_string(Type type) {
    switch (type) {
    case Type::kNull:
        return "null";
    case Type::kBool:
        return "boolean";
    case Type::kNumber:
        return "number";
    case Type::kString:
        return "string";
    case Type::kArray:
        return "array";
    case Type::kObject:
        return "object";
    }
    return "unknown";
}

const Value* Value::find(std::string_view key) const {
    if (type != Type::kObject) {
        return nullptr;
    }
    for (const auto& member : object) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

const Value* Value::find(std::size_t index) const {
    if (type != Type::kArray || index >= array.size()) {
        return nullptr;
    }
    return &array[index];
}

Value Value::make_object() {
    Value v;
    v.type = Type::kObject;
    return v;
}

Value Value::make_array() {
    Value v;
    v.type = Type::kArray;
    return v;
}

Value Value::make_string(std::string s) {
    Value v;
    v.type = Type::kString;
    v.string = std::move(s);
    return v;
}

Value Value::make_int(std::int64_t n) {
    Value v;
    v.type = Type::kNumber;
    v.is_integer = true;
    v.integer = n;
    v.number = static_cast<double>(n);
    return v;
}

Value Value::make_bool(bool b) {
    Value v;
    v.type = Type::kBool;
    v.boolean = b;
    return v;
}

Value Value::make_null() {
    return Value{};
}

void Value::set(std::string key, Value value) {
    for (auto& member : object) {
        if (member.first == key) {
            member.second = std::move(value);
            return;
        }
    }
    object.emplace_back(std::move(key), std::move(value));
}

void Value::push(Value value) {
    array.push_back(std::move(value));
}

std::string ParseError::format(std::string_view source_name) const {
    std::string out(source_name);
    out.append(":");
    out.append(std::to_string(line));
    out.append(":");
    out.append(std::to_string(column));
    out.append(": ");
    out.append(message);
    return out;
}

bool parse(std::string_view text, Value& out, ParseError& error, std::size_t max_bytes) {
    error = ParseError{};
    if (text.size() > max_bytes) {
        error.message = "input exceeds the maximum accepted size";
        error.offset = max_bytes;
        error.line = 1;
        error.column = max_bytes + 1;
        return false;
    }
    Value parsed;
    Parser parser(text, error);
    if (!parser.run(parsed)) {
        return false;
    }
    out = std::move(parsed);
    return true;
}

std::string quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\b':
            out.append("\\b");
            break;
        case '\f':
            out.append("\\f");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20U) {
                char buf[7];
                const int n = std::snprintf(buf, sizeof(buf), "\\u%04x",
                                            static_cast<unsigned>(c) & 0xFFU);
                if (n > 0) {
                    out.append(buf, static_cast<std::size_t>(n));
                }
            } else {
                out.push_back(c);
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

std::string serialize(const Value& value, int indent_width) {
    std::string out;
    serialize_into(value, out, indent_width, 0);
    out.push_back('\n');
    return out;
}

} // namespace hyperion::json
