// The Frame's Steam accounts (frame_steam.py). See frame_steam.h.
#include "frame_steam.h"

#include <sys/stat.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace framesync {

namespace {

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

bool isWord(char c) {
    // Python's \w, for the ASCII keys Steam writes
    return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/**
 * A quoted string with backslash escapes ("(?:[^"\\]|\\.)*") starting at text[i] == '"'.
 * @return the index after the closing quote, or npos if it never closes
 */
size_t skipQuoted(const std::string& text, size_t i) {
    for (size_t k = i + 1; k < text.size(); ++k) {
        if (text[k] == '\\') {
            // \\. : a backslash and any character but a newline (no DOTALL in the Python regex)
            if (k + 1 >= text.size() || text[k + 1] == '\n') return std::string::npos;
            ++k;
        } else if (text[k] == '"') {
            return k + 1;
        }
    }
    return std::string::npos;
}

/** re.sub(r'\\(.)', r'\1', value): a backslash before any character but a newline drops out. */
std::string unescape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] != '\n') {
            out += value[++i];
        } else {
            out += value[i];
        }
    }
    return out;
}

/**
 * _BLOCK at i: "(\d{17})"\s*\{((?:[^{}"]|"(?:[^"\\]|\\.)*")*)\}
 * @return the index after the block, npos if no block starts here
 */
size_t matchBlock(const std::string& text, size_t i, std::string& id, std::string& body) {
    if (text[i] != '"' || i + 19 > text.size()) return std::string::npos;
    for (size_t k = 1; k <= 17; ++k) {
        if (!isDigit(text[i + k])) return std::string::npos;
    }
    if (text[i + 18] != '"') return std::string::npos;
    size_t k = i + 19;
    while (k < text.size() && isSpace(text[k])) ++k;
    if (k >= text.size() || text[k] != '{') return std::string::npos;
    const size_t bodyStart = ++k;
    while (k < text.size()) {
        const char c = text[k];
        if (c == '}') {
            id = text.substr(i + 1, 17);
            body = text.substr(bodyStart, k - bodyStart);
            return k + 1;
        }
        if (c == '{') return std::string::npos;
        if (c == '"') {
            // A quote that never closes can't be part of the body: the regex fails here
            k = skipQuoted(text, k);
            if (k == std::string::npos) return std::string::npos;
            continue;
        }
        ++k;
    }
    return std::string::npos;
}

/** _FIELD at i: "(\w+)"\s+"((?:[^"\\]|\\.)*)". @return the index after it, npos if none starts here */
size_t matchField(const std::string& body, size_t i, std::string& key, std::string& value) {
    if (body[i] != '"') return std::string::npos;
    size_t k = i + 1;
    while (k < body.size() && isWord(body[k])) ++k;
    if (k == i + 1 || k >= body.size() || body[k] != '"') return std::string::npos;
    const std::string name = body.substr(i + 1, k - i - 1);
    ++k;
    const size_t spaces = k;
    while (k < body.size() && isSpace(body[k])) ++k;
    if (k == spaces || k >= body.size() || body[k] != '"') return std::string::npos;
    const size_t end = skipQuoted(body, k);
    if (end == std::string::npos) return std::string::npos;
    key = name;
    value = body.substr(k + 1, end - k - 2);
    return end;
}

}  // namespace

std::string defaultSteamDir() {
    const char* home = std::getenv("HOME");
    return std::string(home != nullptr ? home : "") + "/.local/share/Steam";
}

std::vector<LoginUser> parseLoginUsers(const std::string& text) {
    std::vector<LoginUser> users;
    size_t i = 0;
    while (i < text.size()) {
        std::string id, body;
        const size_t end = matchBlock(text, i, id, body);
        if (end == std::string::npos) {
            ++i;
            continue;
        }
        LoginUser user;
        user.steamId = id;
        size_t k = 0;
        while (k < body.size()) {
            std::string key, value;
            const size_t next = matchField(body, k, key, value);
            if (next == std::string::npos) {
                ++k;
                continue;
            }
            user.fields[key] = unescape(value);  // a later duplicate wins, as in the dict comprehension
            k = next;
        }
        users.push_back(std::move(user));
        i = end;
    }
    return users;
}

std::vector<FrameAccount> frameAccounts(const std::vector<LoginUser>& users) {
    std::vector<FrameAccount> accounts;
    for (const LoginUser& u : users) {
        const auto account = u.fields.find("AccountName");
        if (account == u.fields.end() || account->second.empty()) continue;
        const auto persona = u.fields.find("PersonaName");
        FrameAccount a;
        a.steamId = u.steamId;
        a.accountName = account->second;
        a.personaName = persona != u.fields.end() && !persona->second.empty() ? persona->second : account->second;
        accounts.push_back(a);
    }
    return accounts;
}

std::string currentSteamId(const std::vector<LoginUser>& users) {
    for (const LoginUser& u : users) {
        const auto it = u.fields.find("AutoLogin");
        if (it != u.fields.end() && it->second == "1") return u.steamId;
    }
    return {};
}

bool validSteamId(const std::string& steamId) {
    if (steamId.size() != 17 || steamId.compare(0, 7, "7656119") != 0) return false;
    for (const char c : steamId) {
        if (!isDigit(c)) return false;
    }
    return true;
}

std::string avatarPath(const std::string& steamDir, const std::string& steamId) {
    if (!validSteamId(steamId)) return {};
    const std::string path = steamDir + "/config/avatarcache/" + steamId + ".png";
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? path : std::string();
}

std::string readLoginUsers(const std::string& steamDir) {
    std::ifstream in(steamDir + "/config/loginusers.vdf", std::ios::binary);
    if (!in) return {};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace framesync
