/// @file PtpSettings.cpp

#include "PtpSettings.h"
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <pwd.h>
#include <sstream>
#include <unistd.h>
#include <variant>

namespace AES67 {
namespace Ptp {

namespace {

using Value = std::variant<std::string, long long, bool>;

/// A flat JSON object of strings, integers and booleans: all ptp.json holds.
class FlatObjectParser {
public:
    explicit FlatObjectParser(const std::string& text) : text_(text) {}

    std::optional<std::map<std::string, Value>> parse(std::string& error) {
        std::map<std::string, Value> fields;
        skipSpace();
        if (!take('{')) return fail(error, "expected a JSON object");
        skipSpace();
        if (take('}')) return finish(fields, error);
        for (;;) {
            skipSpace();
            std::string key;
            if (!readString(key)) return fail(error, "expected a quoted key");
            skipSpace();
            if (!take(':')) return fail(error, "expected ':' after \"" + key + "\"");
            skipSpace();
            Value value;
            if (!readValue(value)) return fail(error, "unsupported value for \"" + key + "\"");
            fields[key] = value;
            skipSpace();
            if (take(',')) continue;
            if (take('}')) return finish(fields, error);
            return fail(error, "expected ',' or '}'");
        }
    }

private:
    std::optional<std::map<std::string, Value>> finish(std::map<std::string, Value>& fields, std::string& error) {
        skipSpace();
        if (pos_ != text_.size()) return fail(error, "text after the object");
        return fields;
    }

    static std::optional<std::map<std::string, Value>> fail(std::string& error, const std::string& reason) {
        error = reason;
        return std::nullopt;
    }

    void skipSpace() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    bool take(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool readString(std::string& out) {
        if (!take('"')) return false;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= text_.size()) return false;
                const char escaped = text_[pos_++];
                if (escaped != '"' && escaped != '\\' && escaped != '/') return false;  // no others needed
                out.push_back(escaped);
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool readValue(Value& out) {
        if (pos_ >= text_.size()) return false;
        if (text_[pos_] == '"') {
            std::string s;
            if (!readString(s)) return false;
            out = s;
            return true;
        }
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            out = true;
            return true;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            out = false;
            return true;
        }
        const size_t start = pos_;
        if (text_[pos_] == '-') ++pos_;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        if (pos_ == start || (pos_ == start + 1 && text_[start] == '-') || pos_ - start > 12) return false;
        out = std::strtoll(text_.substr(start, pos_ - start).c_str(), nullptr, 10);
        return true;
    }

    const std::string& text_;
    size_t pos_{0};
};

std::string homeDirectory() {
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') return home;
    if (const passwd* pw = getpwuid(getuid())) return pw->pw_dir;
    return "";
}

} // namespace

Settings Settings::parse(const std::string& json, std::string* error) {
    std::string reason;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return Settings{};
    };
    const auto fields = FlatObjectParser(json).parse(reason);
    if (!fields) return fail(reason);

    Settings s;
    for (const auto& [key, value] : *fields) {
        if (key == "enabled") {
            if (!std::holds_alternative<bool>(value)) return fail("\"enabled\" must be true or false");
            s.enabled = std::get<bool>(value);
        } else if (key == "interface") {
            if (!std::holds_alternative<std::string>(value)) return fail("\"interface\" must be a string");
            s.networkInterface = std::get<std::string>(value);
        } else if (key == "domain") {
            if (!std::holds_alternative<long long>(value)) return fail("\"domain\" must be a number");
            const long long domain = std::get<long long>(value);
            if (domain < 0 || domain > 127) return fail("\"domain\" must be 0 to 127");
            s.domain = static_cast<uint8_t>(domain);
        } else if (key == "hybrid") {
            if (!std::holds_alternative<bool>(value)) return fail("\"hybrid\" must be true or false");
            s.unicastDelayRequests = std::get<bool>(value);
        }
        // Other keys are ignored, so a newer file still works here
    }
    if (error) error->clear();
    return s;
}

std::vector<std::string> Settings::searchPaths() {
    std::vector<std::string> paths;
    if (const char* override = std::getenv("AES67_PTP_CONFIG_PATH"); override && override[0] != '\0') {
        return {override};  // only this file: tests must never pick up a real one
    }
    const std::string home = homeDirectory();
    if (!home.empty()) {
        paths.push_back(home + "/Library/Application Support/AES67Driver/ptp.json");
    }
    paths.push_back("/Library/Application Support/AES67Driver/ptp.json");
    return paths;
}

Settings Settings::load(std::string* foundAt, std::string* error) {
    if (foundAt) foundAt->clear();
    if (error) error->clear();
    for (const auto& path : searchPaths()) {
        std::ifstream file(path);
        if (!file) continue;
        std::stringstream contents;
        contents << file.rdbuf();
        if (foundAt) *foundAt = path;
        return parse(contents.str(), error);
    }
    return Settings{};
}

} // namespace Ptp
} // namespace AES67
