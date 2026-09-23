#pragma once
// A deliberately small, self-contained JSON reader/writer. mousiki has
// no JSON dependency anywhere else in the codebase (the two spots that
// touch yt-dlp's JSON lines use a one-key string grep, see
// online_source.cpp) and pulling in a whole library just for one
// flat-ish snapshot file isn't worth it. This supports exactly what
// snapshot.cpp needs: objects, arrays, strings (with the common escapes),
// numbers, and booleans -- nothing fancier (no unicode \uXXXX escapes,
// no comments, no trailing commas). Malformed input is handled by
// returning a Null-ish/empty result rather than throwing; a corrupt or
// half-written snapshot.json should never crash the app, it should just
// fail to restore.
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace muisc {
namespace tinyjson {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj; // insertion-ordered, small N -- linear find() is fine

    static Value make_null() { return Value{}; }
    static Value make_bool(bool v) { Value x; x.type = Type::Bool; x.b = v; return x; }
    static Value make_num(double v) { Value x; x.type = Type::Number; x.num = v; return x; }
    static Value make_str(std::string v) { Value x; x.type = Type::String; x.str = std::move(v); return x; }
    static Value make_arr() { Value x; x.type = Type::Array; return x; }
    static Value make_obj() { Value x; x.type = Type::Object; return x; }

    void set(const std::string& key, Value v) {
        for (auto& kv : obj) if (kv.first == key) { kv.second = std::move(v); return; }
        obj.emplace_back(key, std::move(v));
    }
    const Value* find(const std::string& key) const {
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }

    std::string as_string(const std::string& def = "") const { return type == Type::String ? str : def; }
    double as_number(double def = 0.0) const { return type == Type::Number ? num : def; }
    bool as_bool(bool def = false) const { return type == Type::Bool ? b : def; }
};

// ---------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------

inline std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) { /* skip other control chars */ }
                else out += c;
        }
    }
    return out;
}

inline void dump(const Value& v, std::string& out, int indent) {
    auto pad = [&](int n) { out.append(static_cast<size_t>(n) * 2, ' '); };
    switch (v.type) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += v.b ? "true" : "false"; break;
        case Type::Number: {
            // Integral values print without a trailing ".0" so ints round-trip cleanly.
            if (v.num == static_cast<long long>(v.num)) out += std::to_string(static_cast<long long>(v.num));
            else out += std::to_string(v.num);
            break;
        }
        case Type::String: out += "\""; out += escape(v.str); out += "\""; break;
        case Type::Array: {
            if (v.arr.empty()) { out += "[]"; break; }
            out += "[\n";
            for (size_t i = 0; i < v.arr.size(); ++i) {
                pad(indent + 1);
                dump(v.arr[i], out, indent + 1);
                if (i + 1 < v.arr.size()) out += ",";
                out += "\n";
            }
            pad(indent); out += "]";
            break;
        }
        case Type::Object: {
            if (v.obj.empty()) { out += "{}"; break; }
            out += "{\n";
            for (size_t i = 0; i < v.obj.size(); ++i) {
                pad(indent + 1);
                out += "\""; out += escape(v.obj[i].first); out += "\": ";
                dump(v.obj[i].second, out, indent + 1);
                if (i + 1 < v.obj.size()) out += ",";
                out += "\n";
            }
            pad(indent); out += "}";
            break;
        }
    }
}

inline std::string write(const Value& v) {
    std::string out;
    dump(v, out, 0);
    out += "\n";
    return out;
}

// ---------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    bool parse(Value& out) {
        skip_ws();
        if (!parse_value(out)) return false;
        return true; // trailing garbage is tolerated
    }

private:
    const std::string& s_;
    size_t i_ = 0;
    bool ok_ = true;

    void skip_ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_; }
    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }

    bool parse_value(Value& out) {
        skip_ws();
        char c = peek();
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == '"') return parse_string_value(out);
        if (c == 't' || c == 'f') return parse_bool(out);
        if (c == 'n') { if (s_.compare(i_, 4, "null") == 0) { i_ += 4; out = Value::make_null(); return true; } return false; }
        return parse_number(out);
    }

    bool parse_object(Value& out) {
        out = Value::make_obj();
        ++i_; // '{'
        skip_ws();
        if (peek() == '}') { ++i_; return true; }
        while (true) {
            skip_ws();
            std::string key;
            if (peek() != '"' || !parse_raw_string(key)) return false;
            skip_ws();
            if (peek() != ':') return false;
            ++i_;
            Value v;
            if (!parse_value(v)) return false;
            out.obj.emplace_back(key, std::move(v));
            skip_ws();
            if (peek() == ',') { ++i_; continue; }
            if (peek() == '}') { ++i_; break; }
            return false;
        }
        return true;
    }

    bool parse_array(Value& out) {
        out = Value::make_arr();
        ++i_; // '['
        skip_ws();
        if (peek() == ']') { ++i_; return true; }
        while (true) {
            Value v;
            if (!parse_value(v)) return false;
            out.arr.push_back(std::move(v));
            skip_ws();
            if (peek() == ',') { ++i_; continue; }
            if (peek() == ']') { ++i_; break; }
            return false;
        }
        return true;
    }

    bool parse_raw_string(std::string& out) {
        if (peek() != '"') return false;
        ++i_;
        out.clear();
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_];
            if (c == '\\' && i_ + 1 < s_.size()) {
                char n = s_[i_ + 1];
                switch (n) {
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    default: out += n; break;
                }
                i_ += 2;
                continue;
            }
            out += c;
            ++i_;
        }
        if (i_ >= s_.size()) return false; // unterminated
        ++i_; // closing quote
        return true;
    }

    bool parse_string_value(Value& out) {
        std::string raw;
        if (!parse_raw_string(raw)) return false;
        out = Value::make_str(std::move(raw));
        return true;
    }

    bool parse_bool(Value& out) {
        if (s_.compare(i_, 4, "true") == 0) { i_ += 4; out = Value::make_bool(true); return true; }
        if (s_.compare(i_, 5, "false") == 0) { i_ += 5; out = Value::make_bool(false); return true; }
        return false;
    }

    bool parse_number(Value& out) {
        size_t start = i_;
        if (peek() == '-' || peek() == '+') ++i_;
        while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' ||
                                   s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '-' || s_[i_] == '+')) ++i_;
        if (i_ == start) return false;
        try {
            out = Value::make_num(std::stod(s_.substr(start, i_ - start)));
        } catch (...) {
            return false;
        }
        return true;
    }
};

inline bool parse(const std::string& text, Value& out) {
    Parser p(text);
    return p.parse(out);
}

} // namespace tinyjson
} // namespace muisc
