#include "json_writer.hpp"

#include <cctype>
#include <cstdlib>

namespace prospero {

std::string JsonExtractString(const std::string &json, const std::string &key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos++;

    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;

    std::string out;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;
            switch (json[pos]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                default: out += json[pos];
            }
        } else {
            out += json[pos];
        }
        pos++;
    }
    return NameFromUtf8(out);
}

bool JsonExtractBool(const std::string &json, const std::string &key, bool default_value) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return default_value;

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return default_value;
    pos++;

    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) pos++;
    if (json.compare(pos, 4, "true") == 0) return true;
    if (json.compare(pos, 5, "false") == 0) return false;
    return default_value;
}

int JsonExtractInt(const std::string &json, const std::string &key, int default_value) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return default_value;

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return default_value;
    pos++;

    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) pos++;
    if (pos >= json.size() || (!std::isdigit(static_cast<unsigned char>(json[pos])) && json[pos] != '-')) {
        return default_value;
    }
    return std::atoi(json.c_str() + pos);
}

std::vector<std::string> JsonExtractArrayItems(const std::string &json, const std::string &key) {
    std::vector<std::string> out;

    size_t pos = 0;
    if (!key.empty()) {
        const std::string needle = "\"" + key + "\"";
        pos = json.find(needle);
        if (pos == std::string::npos) return out;

        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos) return out;
        pos++;
    }
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) pos++;
    if (pos >= json.size() || json[pos] != '[') return out;
    pos++;

    bool in_string = false;
    bool escape = false;
    int obj_depth = 0;
    size_t obj_start = 0;

    for (; pos < json.size(); pos++) {
        char c = json[pos];

        if (in_string) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') in_string = false;
            continue;
        }

        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            if (obj_depth == 0) obj_start = pos;
            obj_depth++;
        } else if (c == '}') {
            obj_depth--;
            if (obj_depth == 0) out.push_back(json.substr(obj_start, pos - obj_start + 1));
        } else if (c == ']' && obj_depth == 0) {
            break;
        }
    }

    return out;
}

std::vector<std::string> JsonExtractStringArray(const std::string &json, const std::string &key) {
    std::vector<std::string> out;

    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return out;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return out;
    pos++;

    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) pos++;
    if (pos >= json.size() || json[pos] != '[') return out;
    pos++;

    while (pos < json.size()) {
        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) pos++;
        if (pos >= json.size() || json[pos] == ']') break;
        if (json[pos] != '"') break; // not a well-formed string array — stop rather than misparse

        pos++;
        std::string item;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                pos++;
                switch (json[pos]) {
                    case 'n': item += '\n'; break;
                    case 't': item += '\t'; break;
                    case 'r': item += '\r'; break;
                    case '"': item += '"'; break;
                    case '\\': item += '\\'; break;
                    default: item += json[pos];
                }
            } else {
                item += json[pos];
            }
            pos++;
        }
        out.push_back(NameFromUtf8(item));
        pos++; // skip closing quote

        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) pos++;
        if (pos < json.size() && json[pos] == ',') pos++;
    }

    return out;
}

} // namespace prospero
