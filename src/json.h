#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>
#include <initializer_list>
#include <utility>
#include <algorithm>

namespace mini {

class json {
public:
    enum type_t { t_null, t_string, t_number, t_object, t_array };

private:
    type_t m_type;
    // m_str holds string content for t_string, numeric text for t_number
    std::string m_str;
    std::vector<std::pair<std::string, json>> m_obj;
    std::vector<json> m_arr;

    static json make_num(const std::string& s) {
        json j;
        j.m_type = t_number;
        j.m_str = s;
        return j;
    }

    // Format integer to string, avoiding int64_t ABI issues on cross-compile
    template<typename T>
    static std::string fmt_int(T v) {
        char buf[32];
        // Build string manually to avoid any 64-bit argument passing
        if (v == 0) return "0";
        bool neg = false;
        // Use unsigned type for the actual formatting
        unsigned long uv;
        if (v < 0) {
            neg = true;
            // Handle negative: negate using unsigned to avoid UB on INT_MIN
            uv = (unsigned long)(-(v + 1)) + 1u;
        } else {
            uv = (unsigned long)v;
        }
        char* p = buf + sizeof(buf) - 1;
        *p = '\0';
        while (uv > 0) {
            *--p = '0' + (char)(uv % 10);
            uv /= 10;
        }
        if (neg) *--p = '-';
        return std::string(p);
    }

    static std::string fmt_uint(unsigned long v) {
        if (v == 0) return "0";
        char buf[32];
        char* p = buf + sizeof(buf) - 1;
        *p = '\0';
        while (v > 0) {
            *--p = '0' + (char)(v % 10);
            v /= 10;
        }
        return std::string(p);
    }

    // ── Parser internals ────────────────────────────────────────────────
    struct parser {
        const char* p;
        const char* end;

        void skip_ws() {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
        }

        char peek() { skip_ws(); return p < end ? *p : 0; }
        char take() { skip_ws(); return p < end ? *p++ : 0; }

        void expect(char c) {
            if (take() != c) throw std::runtime_error("json parse error");
        }

        std::string parse_string_val() {
            expect('"');
            std::string s;
            while (p < end && *p != '"') {
                if (*p == '\\') {
                    ++p;
                    if (p >= end) throw std::runtime_error("json parse error");
                    switch (*p) {
                        case '"': s += '"'; break;
                        case '\\': s += '\\'; break;
                        case '/': s += '/'; break;
                        case 'b': s += '\b'; break;
                        case 'f': s += '\f'; break;
                        case 'n': s += '\n'; break;
                        case 'r': s += '\r'; break;
                        case 't': s += '\t'; break;
                        case 'u': {
                            if (p + 4 >= end) throw std::runtime_error("json parse error");
                            unsigned cp = 0;
                            for (int i = 1; i <= 4; ++i) {
                                char h = p[i];
                                cp <<= 4;
                                if (h >= '0' && h <= '9') cp |= h - '0';
                                else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                                else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                                else throw std::runtime_error("json parse error");
                            }
                            p += 4;
                            if (cp < 0x80) {
                                s += (char)cp;
                            } else if (cp < 0x800) {
                                s += (char)(0xC0 | (cp >> 6));
                                s += (char)(0x80 | (cp & 0x3F));
                            } else {
                                s += (char)(0xE0 | (cp >> 12));
                                s += (char)(0x80 | ((cp >> 6) & 0x3F));
                                s += (char)(0x80 | (cp & 0x3F));
                            }
                            break;
                        }
                        default: s += *p; break;
                    }
                } else {
                    s += *p;
                }
                ++p;
            }
            if (p >= end) throw std::runtime_error("json parse error");
            ++p;
            return s;
        }

        json parse_value() {
            char c = peek();
            if (c == '"') {
                json j;
                j.m_type = t_string;
                j.m_str = parse_string_val();
                return j;
            }
            if (c == '{') return parse_object();
            if (c == '[') return parse_array();
            if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
            if (c == 't') { p += 4; return make_num("1"); }
            if (c == 'f') { p += 5; return make_num("0"); }
            if (c == 'n') { p += 4; return json(); }
            throw std::runtime_error("json parse error");
        }

        json parse_object() {
            expect('{');
            json j;
            j.m_type = t_object;
            if (peek() == '}') { ++p; return j; }
            for (;;) {
                std::string key = parse_string_val();
                expect(':');
                j.m_obj.push_back({std::move(key), parse_value()});
                char c = take();
                if (c == '}') break;
                if (c != ',') throw std::runtime_error("json parse error");
            }
            return j;
        }

        json parse_array() {
            expect('[');
            json j;
            j.m_type = t_array;
            if (peek() == ']') { ++p; return j; }
            for (;;) {
                j.m_arr.push_back(parse_value());
                char c = take();
                if (c == ']') break;
                if (c != ',') throw std::runtime_error("json parse error");
            }
            return j;
        }

        json parse_number() {
            skip_ws();
            const char* start = p;
            if (*p == '-') ++p;
            while (p < end && *p >= '0' && *p <= '9') ++p;
            if (p < end && *p == '.') {
                ++p;
                while (p < end && *p >= '0' && *p <= '9') ++p;
            }
            if (p < end && (*p == 'e' || *p == 'E')) {
                ++p;
                if (p < end && (*p == '+' || *p == '-')) ++p;
                while (p < end && *p >= '0' && *p <= '9') ++p;
            }
            // Store the raw numeric text
            return make_num(std::string(start, p));
        }
    };

    // ── dump helpers ────────────────────────────────────────────────────
    static void escape_string(std::string& out, const std::string& s) {
        out += '"';
        for (char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if ((unsigned char)c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                        out += buf;
                    } else {
                        out += c;
                    }
                    break;
            }
        }
        out += '"';
    }

    void dump_to(std::string& out) const {
        switch (m_type) {
            case t_null: out += "null"; break;
            case t_string: escape_string(out, m_str); break;
            case t_number: out += m_str; break;
            case t_object: {
                out += '{';
                for (size_t i = 0; i < m_obj.size(); ++i) {
                    if (i > 0) out += ',';
                    escape_string(out, m_obj[i].first);
                    out += ':';
                    m_obj[i].second.dump_to(out);
                }
                out += '}';
                break;
            }
            case t_array: {
                out += '[';
                for (size_t i = 0; i < m_arr.size(); ++i) {
                    if (i > 0) out += ',';
                    m_arr[i].dump_to(out);
                }
                out += ']';
                break;
            }
        }
    }

    int find_key(const std::string& key) const {
        for (size_t i = 0; i < m_obj.size(); ++i) {
            if (m_obj[i].first == key) return (int)i;
        }
        return -1;
    }

    long to_long() const {
        if (m_str.empty()) return 0;
        return strtol(m_str.c_str(), nullptr, 10);
    }

    unsigned long to_ulong() const {
        if (m_str.empty()) return 0;
        return strtoul(m_str.c_str(), nullptr, 10);
    }

public:
    // ── Constructors ────────────────────────────────────────────────────
    json() : m_type(t_null) {}
    json(std::nullptr_t) : m_type(t_null) {}
    json(const char* s) : m_type(t_string), m_str(s) {}
    json(const std::string& s) : m_type(t_string), m_str(s) {}
    json(std::string&& s) : m_type(t_string), m_str(std::move(s)) {}

    // Number constructors: format to string immediately to avoid int64 ABI issues
    json(int v) : m_type(t_number), m_str(fmt_int(v)) {}
    json(unsigned int v) : m_type(t_number), m_str(fmt_uint(v)) {}
    json(long v) : m_type(t_number), m_str(fmt_int(v)) {}
    json(unsigned long v) : m_type(t_number), m_str(fmt_uint(v)) {}
    json(bool v) : m_type(t_number), m_str(v ? "1" : "0") {}

    json(const std::vector<json>& arr) : m_type(t_array), m_arr(arr) {}
    json(std::vector<json>&& arr) : m_type(t_array), m_arr(std::move(arr)) {}

    json(std::initializer_list<std::pair<std::string, json>> il)
        : m_type(t_object), m_obj(il.begin(), il.end()) {}

    json(const json&) = default;
    json(json&&) = default;
    json& operator=(const json&) = default;
    json& operator=(json&&) = default;

    // Assignment from values
    json& operator=(const char* s) { *this = json(s); return *this; }
    json& operator=(const std::string& s) { *this = json(s); return *this; }
    json& operator=(int v) { *this = json(v); return *this; }
    json& operator=(unsigned int v) { *this = json(v); return *this; }
    json& operator=(long v) { *this = json(v); return *this; }
    json& operator=(unsigned long v) { *this = json(v); return *this; }

    // ── Static factories ────────────────────────────────────────────────
    static json object() {
        json j;
        j.m_type = t_object;
        return j;
    }

    static json parse(const std::string& s) {
        parser p{s.data(), s.data() + s.size()};
        return p.parse_value();
    }

    // ── Serialization ───────────────────────────────────────────────────
    std::string dump() const {
        std::string out;
        dump_to(out);
        return out;
    }

    // ── Access ──────────────────────────────────────────────────────────
    json& operator[](const char* key) { return (*this)[std::string(key)]; }
    const json& operator[](const char* key) const { return (*this)[std::string(key)]; }

    json& operator[](const std::string& key) {
        if (m_type == t_null) m_type = t_object;
        int idx = find_key(key);
        if (idx >= 0) return m_obj[idx].second;
        m_obj.push_back({key, json()});
        return m_obj.back().second;
    }

    const json& operator[](const std::string& key) const {
        int idx = find_key(key);
        if (idx >= 0) return m_obj[idx].second;
        static const json null_json;
        return null_json;
    }

    // ── Conversions ─────────────────────────────────────────────────────
    operator std::string() const { return m_str; }
    explicit operator int() const { return (int)to_long(); }
    explicit operator unsigned int() const { return (unsigned int)to_ulong(); }
    explicit operator long() const { return to_long(); }
    explicit operator unsigned long() const { return to_ulong(); }

    // ── Comparison ──────────────────────────────────────────────────────
    bool operator==(const json& other) const {
        if (m_type != other.m_type) return false;
        switch (m_type) {
            case t_null: return true;
            case t_string: return m_str == other.m_str;
            case t_number: return to_long() == other.to_long();
            case t_object: return m_obj == other.m_obj;
            case t_array: return m_arr == other.m_arr;
        }
        return false;
    }
    bool operator==(int v) const { return m_type == t_number && to_long() == v; }
    bool operator==(unsigned int v) const { return m_type == t_number && to_ulong() == v; }
    bool operator==(long v) const { return m_type == t_number && to_long() == v; }
    bool operator==(unsigned long v) const { return m_type == t_number && to_ulong() == v; }
    bool operator==(const char* s) const { return m_type == t_string && m_str == s; }
    bool operator==(const std::string& s) const { return m_type == t_string && m_str == s; }

    // ── Mutation ────────────────────────────────────────────────────────
    void erase(const std::string& key) {
        m_obj.erase(
            std::remove_if(m_obj.begin(), m_obj.end(),
                [&key](const std::pair<std::string, json>& p) { return p.first == key; }),
            m_obj.end());
    }

    void push_back(const json& val) {
        if (m_type == t_null) m_type = t_array;
        m_arr.push_back(val);
    }

    // ── Query ───────────────────────────────────────────────────────────
    bool empty() const {
        switch (m_type) {
            case t_null: return true;
            case t_string: return m_str.empty();
            case t_object: return m_obj.empty();
            case t_array: return m_arr.empty();
            default: return false;
        }
    }

    type_t type() const { return m_type; }
};

} // namespace mini

using json = mini::json;
