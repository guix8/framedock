// Switching the Frame's Steam account. See frame_steam_switch.h.
#include "frame_steam_switch.h"

#include "app_text.h"
#include "atomic_file.h"
#include "frame_steam.h"

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

namespace framesync {

namespace {

/** One token of a VDF text: a quoted string (with its quotes) or a brace. */
struct Token {
    size_t begin = 0;
    size_t end = 0;  ///< one past the token
    char kind = '"';  ///< '"', '{' or '}'
};

/** The text's tokens; nullopt if something else than whitespace, strings and braces is in it (or a string never ends). */
std::optional<std::vector<Token>> tokenize(const std::string& text) {
    std::vector<Token> tokens;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
        } else if (c == '{' || c == '}') {
            tokens.push_back({i, i + 1, c});
            ++i;
        } else if (c == '"') {
            size_t k = i + 1;
            while (k < text.size() && text[k] != '"') k += text[k] == '\\' ? 2 : 1;
            if (k >= text.size()) return std::nullopt;
            tokens.push_back({i, k + 1, '"'});
            i = k + 1;
        } else {
            return std::nullopt;
        }
    }
    return tokens;
}

std::string inner(const std::string& text, const Token& t) {
    return text.substr(t.begin + 1, t.end - t.begin - 2);
}

bool accountNameOk(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (const char c : name) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!ok) return false;
    }
    return true;
}

std::string readFile(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    ok = static_cast<bool>(in);
    if (!ok) return {};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/** Where the log ended before Steam started: its inode and size. */
struct LogMark {
    ino_t inode = 0;
    off_t size = 0;
};

LogMark markOf(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return {};
    return {st.st_ino, st.st_size};
}

/** What Steam added after mark: all of it if the file is another one (rotated) or shorter. */
std::string readFrom(const std::string& path, const LogMark& mark) {
    const LogMark now = markOf(path);
    const off_t offset = now.inode == mark.inode && now.size >= mark.size ? mark.size : 0;
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    in.seekg(offset);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

std::optional<std::string> withAutoLogin(const std::string& loginUsers, const std::string& steamId) {
    const auto tokens = tokenize(loginUsers);
    if (!tokens || !validSteamId(steamId)) return std::nullopt;
    // "users" { "<id>" { "AutoLogin" "1" ... } ... }: the values to change, back to front later
    std::vector<std::pair<Token, char>> changes;
    bool found = false;
    int depth = 0;
    std::string block;  // the id of the block at depth 2 ("" = not an account block)
    const std::vector<Token>& t = *tokens;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i].kind == '{') {
            ++depth;
            continue;
        }
        if (t[i].kind == '}') {
            if (--depth < 0) return std::nullopt;
            if (depth < 2) block.clear();
            continue;
        }
        const bool opens = i + 1 < t.size() && t[i + 1].kind == '{';
        if (opens) {
            if (depth == 1) block = inner(loginUsers, t[i]);
            continue;
        }
        // A key and its value
        if (i + 1 >= t.size() || t[i + 1].kind != '"') return std::nullopt;
        if (depth == 2 && validSteamId(block) && inner(loginUsers, t[i]) == "AutoLogin") {
            const bool target = block == steamId;
            found = found || target;
            changes.emplace_back(t[i + 1], target ? '1' : '0');
        }
        ++i;
    }
    if (depth != 0 || !found) return std::nullopt;
    std::string out = loginUsers;
    for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
        out.replace(it->first.begin, it->first.end - it->first.begin, std::string("\"") + it->second + "\"");
    }
    return out;
}

std::optional<std::string> withAutoLoginUser(const std::string& registry, const std::string& accountName) {
    const auto tokens = tokenize(registry);
    if (!tokens || !accountNameOk(accountName)) return std::nullopt;
    const std::vector<Token>& t = *tokens;
    std::optional<Token> value;
    for (size_t i = 0; i + 1 < t.size(); ++i) {
        if (t[i].kind != '"' || t[i + 1].kind != '"') continue;
        // A key and its value (the value is skipped, so a value that reads "AutoLoginUser" isn't taken for the key)
        if (inner(registry, t[i]) == "AutoLoginUser") {
            if (value) return std::nullopt;  // twice: not the file we know
            value = t[i + 1];
        }
        ++i;
    }
    if (!value) return std::nullopt;
    std::string out = registry;
    out.replace(value->begin, value->end - value->begin, "\"" + accountName + "\"");
    return out;
}

bool loggedOnAs(const std::string& logText, const std::string& steamId) {
    if (!validSteamId(steamId)) return false;
    const unsigned long long id = std::strtoull(steamId.c_str(), nullptr, 10) - 76561197960265728ULL;
    const std::string tag = "[U:1:" + std::to_string(id) + "]";
    std::istringstream lines(logText);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find("[Logged On") != std::string::npos && line.find(tag) != std::string::npos) return true;
    }
    return false;
}

bool frameGameRunning(const std::string& procDir) {
    DIR* dir = ::opendir(procDir.c_str());
    if (dir == nullptr) return false;
    bool running = false;
    while (const dirent* entry = ::readdir(dir)) {
        const char* name = entry->d_name;
        if (name[0] < '0' || name[0] > '9') continue;
        bool ok = false;
        const std::string cmdline = readFile(procDir + "/" + name + "/cmdline", ok);
        // argv is NUL-separated: "…/reaper\0SteamLaunch\0AppId=…"
        const size_t first = cmdline.find('\0');
        if (first == std::string::npos) continue;
        const std::string arg0 = cmdline.substr(0, first);
        if (arg0.size() >= 6 && arg0.compare(arg0.size() - 6, 6, "reaper") == 0 &&
            cmdline.compare(first + 1, 12, std::string("SteamLaunch") + '\0') == 0) {
            running = true;
            break;
        }
    }
    ::closedir(dir);
    return running;
}

bool steamServiceCommand(const std::string& verb, double timeoutSec) {
    const char* systemctl = ::access("/usr/bin/systemctl", X_OK) == 0 ? "/usr/bin/systemctl" : "/bin/systemctl";
    // The arguments are made before the fork (only async-signal-safe calls in the child)
    std::string a0 = systemctl, a1 = "--user", a2 = verb, a3 = "steam.service";
    char* argv[] = {a0.data(), a1.data(), a2.data(), a3.data(), nullptr};
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        ::execv(argv[0], argv);
        ::_exit(127);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSec);
    int status = 0;
    while (true) {
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (done < 0) return false;
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

FrameSwitchPaths FrameSwitchPaths::defaults() {
    const char* home = std::getenv("HOME");
    const std::string h = home != nullptr ? home : "";
    FrameSwitchPaths p;
    p.steamDir = defaultSteamDir();
    p.registryFile = h + "/.steam/registry.vdf";
    const char* state = std::getenv("XDG_STATE_HOME");
    p.backupDir = (state != nullptr && state[0] == '/' ? std::string(state) : h + "/.local/state") +
                  "/framedock/steam-switch";
    return p;
}

bool switchFrameSteam(const FrameSwitchPaths& paths, const std::string& steamId, const FrameSwitchHooks& hooks,
                      std::string& message) {
    const std::string usersFile = paths.steamDir + "/config/loginusers.vdf";
    const std::string logFile = paths.steamDir + "/logs/connection_log.txt";
    const auto users = parseLoginUsers(readLoginUsers(paths.steamDir));
    std::string accountName;
    for (const FrameAccount& a : frameAccounts(users)) {
        if (a.steamId == steamId) accountName = a.accountName;
    }
    if (accountName.empty()) {
        message = tr(Tx::SyncFrameNoAccount);
        return false;
    }
    // Steam writes AutoLogin at the logon too (seen 2026-11-14: the file changed a second after "Logged On")
    if (currentSteamId(users) == steamId) return true;
    for (const LoginUser& u : users) {
        const auto remember = u.fields.find("RememberPassword");
        if (u.steamId == steamId && (remember == u.fields.end() || remember->second != "1")) {
            message = tr(Tx::SyncFrameNoPassword);  // Steam would stop at its password screen
            return false;
        }
    }
    if (hooks.gameRunning && hooks.gameRunning()) {
        message = tr(Tx::SyncFrameGame);
        return false;
    }
    // Checked before Steam is stopped, so a file we can't change leaves Steam as it is
    bool ok = false;
    const std::string usersBefore = readFile(usersFile, ok);
    bool registryOk = false;
    const std::string registryBefore = readFile(paths.registryFile, registryOk);
    if (!ok || !registryOk || !withAutoLogin(usersBefore, steamId) || !withAutoLoginUser(registryBefore, accountName)) {
        message = tr(Tx::SyncFrameFiles);
        return false;
    }
    std::fprintf(stderr, "[frame-sync] Switching the Frame's Steam: stopping steam.service\n");
    if (!hooks.service("stop")) {
        message = tr(Tx::SyncFrameStop);
        hooks.service("start");  // a stop that failed half way: Steam back on
        return false;
    }
    // Steam wrote both files as it quit: change what it wrote
    const std::string usersNow = readFile(usersFile, ok);
    const std::string registryNow = readFile(paths.registryFile, registryOk);
    const auto usersNew = ok ? withAutoLogin(usersNow, steamId) : std::nullopt;
    const auto registryNew = registryOk ? withAutoLoginUser(registryNow, accountName) : std::nullopt;
    std::string error;
    bool written = false;
    bool half = false;  // loginusers.vdf changed, registry.vdf not, and putting it back failed too
    if (usersNew && registryNew && frame_perf_overlay::atomicWriteFile(paths.backupDir + "/loginusers.vdf", usersNow, error) &&
        frame_perf_overlay::atomicWriteFile(paths.backupDir + "/registry.vdf", registryNow, error)) {
        if (frame_perf_overlay::atomicWriteFile(usersFile, *usersNew, error)) {
            written = frame_perf_overlay::atomicWriteFile(paths.registryFile, *registryNew, error);
            std::string undoError;
            if (!written && !frame_perf_overlay::atomicWriteFile(usersFile, usersNow, undoError)) {  // both or neither
                half = true;
                std::fprintf(stderr, "[frame-sync] Couldn't put loginusers.vdf back: %s\n", undoError.c_str());
            }
        }
    }
    if (!written) std::fprintf(stderr, "[frame-sync] Couldn't change Steam's files: %s\n", error.c_str());
    const LogMark logStart = markOf(logFile);
    std::fprintf(stderr, "[frame-sync] Starting steam.service\n");
    if (!hooks.service("start")) {
        message = tr(Tx::SyncFrameStart);
        return false;
    }
    if (!written) {
        message = half ? trf(Tx::SyncFrameFilesHalf, {paths.backupDir}) : std::string(tr(Tx::SyncFrameFiles));
        return false;
    }
    for (double waited = 0; waited < kFrameLogonLimitSec; waited += 1) {
        if (!hooks.sleep(1)) {
            message = tr(Tx::SyncQuitting);
            return false;
        }
        if (loggedOnAs(readFrom(logFile, logStart), steamId)) {
            std::fprintf(stderr, "[frame-sync] The Frame's Steam logged on as the new account\n");
            return true;
        }
    }
    message = tr(Tx::SyncFrameNoLogon);
    return false;
}

}  // namespace framesync
