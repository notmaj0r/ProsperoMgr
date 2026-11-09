#pragma once
// Small JSON builder for API responses — no third-party dependency to
// cross-compile. Request bodies are small/flat, so parsing stays minimal too.

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "raw_name.hpp"

namespace prospero {

class JsonWriter {
  public:
    JsonWriter() { buf_ << '{'; }

    JsonWriter &Str(const std::string &key, const std::string &value) {
        Sep();
        buf_ << Quote(key) << ':' << Quote(value);
        return *this;
    }

    JsonWriter &Int(const std::string &key, long long value) {
        Sep();
        buf_ << Quote(key) << ':' << value;
        return *this;
    }

    JsonWriter &Bool(const std::string &key, bool value) {
        Sep();
        buf_ << Quote(key) << ':' << (value ? "true" : "false");
        return *this;
    }

    // Embeds a pre-built JSON fragment (object/array/literal) verbatim,
    // e.g. Raw("payloads", "[" + joined_elements + "]").
    JsonWriter &Raw(const std::string &key, const std::string &json_fragment) {
        Sep();
        buf_ << Quote(key) << ':' << json_fragment;
        return *this;
    }

    std::string Done() {
        buf_ << '}';
        return buf_.str();
    }

    // Escapes a string for embedding as a JSON string literal, including
    // the surrounding quotes.
    static std::string Quote(const std::string &raw) {
        std::string out = "\"";
        out.reserve(raw.size() + 2);
        for (unsigned char c : NameToUtf8(raw)) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char hex[8];
                        std::snprintf(hex, sizeof(hex), "\\u%04x", c);
                        out += hex;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
        out += '"';
        return out;
    }

    // Builds a JSON array of already-quoted/serialized elements, e.g.
    // JsonWriter::Array({obj1.Done(), obj2.Done()}).
    static std::string Array(const std::vector<std::string> &elements) {
        std::string out = "[";
        for (size_t i = 0; i < elements.size(); i++) {
            if (i) out += ',';
            out += elements[i];
        }
        out += ']';
        return out;
    }

  private:
    void Sep() {
        if (!first_) buf_ << ',';
        first_ = false;
    }

    std::ostringstream buf_;
    bool first_ = true;
};

// Extracts a top-level "key":"value" field — good enough for this API's small POST bodies.
std::string JsonExtractString(const std::string &json, const std::string &key);

// Extracts a top-level "key":true/false literal.
bool JsonExtractBool(const std::string &json, const std::string &key, bool default_value);

// Extracts a top-level "key":123 numeric literal.
int JsonExtractInt(const std::string &json, const std::string &key, int default_value);

// Splits "key":[{...},{...}] into each object's raw text — walks repository index JSON
// without a full parser. Empty `key` treats `json` itself as a top-level array (GitHub/Gitea releases).
std::vector<std::string> JsonExtractArrayItems(const std::string &json, const std::string &key);

// Same idea for "key":["a","b"] — a plain string array (e.g. a bulk file-selection
// path list), decoded the same way JsonExtractString decodes one string.
std::vector<std::string> JsonExtractStringArray(const std::string &json, const std::string &key);

} // namespace prospero
