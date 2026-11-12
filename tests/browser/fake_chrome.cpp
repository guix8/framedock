// A stand-in for Chrome in the browser session's tests: takes Chrome's switches and answers the DevTools protocol, over
// --remote-debugging-pipe (NUL-terminated JSON read from fd 3, written to fd 4; it exits when the pipe closes, as
// Chrome does) or else over a real WebSocket on 127.0.0.1 (writing DevToolsActivePort into --user-data-dir), with
// just enough behaviour:
// one page target, navigation history, screencast frames (a small JPEG, the next one only after the ack), popups,
// a window per page (Browser.getWindowForTarget),
// Browser.close. Every command it receives goes to <profile>/fake-log.txt ("method sessionId params").
//
// FAKE_CHROME_MODE (environment):
//   normal       as above
//   no-port      never writes DevToolsActivePort (a Chrome that hangs at start)
//   ignore-term  ignores SIGTERM and Browser.close (only SIGKILL ends it)
//   spawn-child  also starts a child that sleeps in the same process group (its PID in <profile>/fake-child.pid)
//   no-page      Target.getTargets lists no page (the session creates one)
// Markers in the profile, read at start:
//   fake-nondumpable       PR_SET_DUMPABLE 0, as Chrome's sandboxed helpers (/proc/N then belongs to root)
//   fake-detached-helper   also starts a helper outside its process group and session (as a flatpak's zypak child
//                          spawned through flatpak-portal), named by --database=<profile>/Crash Reports only (its PID
//                          in <profile>/fake-helper.pid)
//   fake-pipe-deaf         with --remote-debugging-pipe: never reads or answers it (the fds went somewhere else)
//   fake-pipe-closed       with --remote-debugging-pipe: closes fds 3 and 4 and stays up (a wrapper closed them)
//   fake-pipe-exit         with --remote-debugging-pipe: exits at once (as Chrome when its pipe fds aren't open)
// With --fake-helper it is such a helper: it only sleeps.
// The context menu's link lookup (elementFromPoint) answers https://link.example/, or "" at x = 5.
// The audio check answers "playing" (audible) while <profile>/fake-audible exists, "paused" while
// <profile>/fake-media-paused does, else "no media", and not at all while <profile>/fake-audible-hang does (a stuck
// page). The video controls' play / pause and pause-all move between the first two.
// Sessions are numbered ("S-T1.1", "S-T1.2": attaching again gives a new one); Target.detachFromTarget ends one and
// says so (Target.detachedFromTarget). A URL with "alert", "confirm", "prompt" or "beforeunload" in it opens that
// dialog; Page.handleJavaScriptDialog closes it (Page.javascriptDialogClosed).
// Navigating to a URL with "crash" in it exits with 3 (once per profile: <profile>/fake-crashed marks it; every time
// while <profile>/fake-crash-always exists), with "popup" opens a new page from this one, with "close" closes the
// current page. While <profile>/fake-crash-any exists any navigation exits with 3 (a Chrome that can't stay up).
// Popups nest ("popup" again from a popup opens P2 with P1 as its opener, ...); Target.closeTarget closes any of them.
// A URL with "renderfail" crashes the page's renderer (Inspector.targetCrashed and Target.targetCrashed) when it
// loads and on every Page.reload. More markers: fake-attach-fail-<target> (Target.attachToTarget on it fails),
// fake-input-hang (Input.* commands are never answered), fake-log-flood (6 MB to stderr at start), fake-reload-asks
// (Page.reload opens a "beforeunload" dialog instead of reading the page again).
#include "jpeg_frame.h"
#include "json.h"
#include "websocket.h"

#include <algorithm>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string profile;
std::string mode = "normal";
FILE* logFile = nullptr;
int client = -1;      // where answers go: the WebSocket, or fd 4
bool pipeMode = false;
std::string out;

struct Entry {
    int id;
    std::string url;
};
std::vector<Entry> history;
int current = -1;
int nextEntry = 1;
std::string pageTarget = "T1";
std::vector<std::string> popups;  // the popup chain, the newest last (shown)
int nextPopup = 1;
std::string screencastSession;  // "" = off
int nextSession = 1;
std::vector<std::pair<std::string, std::string>> sessions;  // session id → its target
int frameNumber = 0;
std::string frameData;  // base64 JPEG

void sendText(const std::string& text) {
    if (pipeMode) {
        out += text;
        out += '\0';
    } else {
        out += ws::encodeFrame(ws::Text, text, nullptr);
    }
}

void flushOut() {
    while (!out.empty() && client >= 0) {
        const ssize_t n = ::write(client, out.data(), out.size());
        if (n <= 0) {
            if (errno == EAGAIN || errno == EINTR) {
                pollfd p {client, POLLOUT, 0};
                ::poll(&p, 1, 100);
                continue;
            }
            out.clear();
            return;
        }
        out.erase(0, static_cast<size_t>(n));
    }
}

void respond(int id, const std::string& result, const std::string& session) {
    std::string msg = "{\"id\":" + std::to_string(id) + ",\"result\":" + result;
    if (!session.empty()) msg += ",\"sessionId\":" + jsonString(session);
    sendText(msg + "}");
}

void event(const std::string& method, const std::string& params, const std::string& session) {
    std::string msg = "{\"method\":" + jsonString(method) + ",\"params\":" + params;
    if (!session.empty()) msg += ",\"sessionId\":" + jsonString(session);
    sendText(msg + "}");
}

std::string currentTarget() { return popups.empty() ? pageTarget : popups.back(); }

bool marked(const std::string& name) { return ::access((profile + "/" + name).c_str(), F_OK) == 0; }

/** A target goes away: its sessions detach, then it is destroyed */
void destroyTarget(const std::string& gone) {
    for (size_t i = 0; i < sessions.size();) {
        if (sessions[i].second != gone) {
            ++i;
            continue;
        }
        if (screencastSession == sessions[i].first) screencastSession.clear();
        event("Target.detachedFromTarget", "{\"sessionId\":" + jsonString(sessions[i].first) + ",\"targetId\":" + jsonString(gone) + "}", "");
        sessions.erase(sessions.begin() + static_cast<long>(i));
    }
    event("Target.targetDestroyed", "{\"targetId\":" + jsonString(gone) + "}", "");
}

void rendererCrash(const std::string& session) {
    std::fprintf(logFile, "RENDERCRASH\n");
    std::fflush(logFile);
    event("Inspector.targetCrashed", "{}", session);
    event("Target.targetCrashed", "{\"targetId\":" + jsonString(currentTarget()) + ",\"status\":\"crashed\",\"errorCode\":139}", "");
}

void sendFrame() {
    if (screencastSession.empty()) return;
    ++frameNumber;
    event("Page.screencastFrame",
          "{\"data\":\"" + frameData + "\",\"sessionId\":" + std::to_string(frameNumber) +
              ",\"metadata\":{\"offsetTop\":0,\"pageScaleFactor\":1,\"deviceWidth\":1280,\"deviceHeight\":720,"
              "\"scrollOffsetX\":0,\"scrollOffsetY\":0}}",
          screencastSession);
}

void navigated(const std::string& session) {
    const std::string& url = history[static_cast<size_t>(current)].url;
    event("Page.frameStartedLoading", "{\"frameId\":\"F1\"}", session);
    event("Page.frameNavigated", "{\"frame\":{\"id\":\"F1\",\"loaderId\":\"L1\",\"url\":" + jsonString(url) +
                                     ",\"securityOrigin\":\"\",\"mimeType\":\"text/html\"},\"type\":\"Navigation\"}",
          session);
    event("Page.loadEventFired", "{\"timestamp\":1}", session);
    event("Page.frameStoppedLoading", "{\"frameId\":\"F1\"}", session);
    event("Target.targetInfoChanged",
          "{\"targetInfo\":{\"targetId\":" + jsonString(currentTarget()) + ",\"type\":\"page\",\"url\":" + jsonString(url) +
              ",\"title\":" + jsonString("Title of " + url) + ",\"attached\":true}}",
          "");
}

void handle(const std::string& text) {
    JsonValue root;
    std::string error;
    if (!parseJson(text, root, error)) return;
    const JsonValue* idValue = root.get("id");
    const JsonValue* methodValue = root.get("method");
    if (idValue == nullptr || methodValue == nullptr) return;
    const int id = static_cast<int>(idValue->number);
    const std::string method = methodValue->text;
    const JsonValue* sessionValue = root.get("sessionId");
    const std::string session = sessionValue != nullptr ? sessionValue->text : "";
    const JsonValue* params = root.get("params");
    const size_t paramsAt = text.find("\"params\":");
    std::string paramsText = paramsAt == std::string::npos ? "{}" : text.substr(paramsAt + 9);
    std::fprintf(logFile, "%s %s %s\n", method.c_str(), session.empty() ? "-" : session.c_str(), paramsText.c_str());
    std::fflush(logFile);

    if (method == "Target.getTargets") {
        if (mode == "no-page") {
            respond(id, "{\"targetInfos\":[]}", session);
        } else {
            respond(id, "{\"targetInfos\":[{\"targetId\":\"T1\",\"type\":\"page\",\"title\":\"\",\"url\":\"about:blank\","
                        "\"attached\":false}]}",
                    session);
        }
    } else if (method == "Target.createTarget") {
        pageTarget = "T2";
        respond(id, "{\"targetId\":\"T2\"}", session);
    } else if (method == "Target.attachToTarget") {
        const std::string target = params != nullptr && params->get("targetId") != nullptr ? params->get("targetId")->text : "";
        if (marked("fake-attach-fail-" + target)) {
            sendText("{\"id\":" + std::to_string(id) + ",\"error\":{\"code\":-32000,\"message\":\"No target with given id found\"}}");
            return;
        }
        const std::string made = "S-" + target + "." + std::to_string(nextSession++);
        sessions.push_back({made, target});
        respond(id, "{\"sessionId\":" + jsonString(made) + "}", session);
    } else if (method == "Target.detachFromTarget") {
        const std::string gone = params != nullptr && params->get("sessionId") != nullptr ? params->get("sessionId")->text : "";
        bool found = false;
        for (size_t i = 0; i < sessions.size(); ++i) {
            if (sessions[i].first != gone) continue;
            const std::string target = sessions[i].second;
            sessions.erase(sessions.begin() + static_cast<long>(i));
            found = true;
            if (screencastSession == gone) screencastSession.clear();
            respond(id, "{}", session);
            event("Target.detachedFromTarget", "{\"sessionId\":" + jsonString(gone) + ",\"targetId\":" + jsonString(target) + "}", "");
            break;
        }
        if (!found) sendText("{\"id\":" + std::to_string(id) + ",\"error\":{\"code\":-32602,\"message\":\"No session with given id\"}}");
    } else if (method == "Page.handleJavaScriptDialog") {
        const bool accept = params != nullptr && params->get("accept") != nullptr && params->get("accept")->type == JsonValue::Type::Bool && params->get("accept")->boolean;
        respond(id, "{}", session);
        event("Page.javascriptDialogClosed", std::string("{\"result\":") + (accept ? "true" : "false") + ",\"userInput\":\"\"}", session);
    } else if (method == "Page.navigate") {
        const std::string url = params != nullptr && params->get("url") != nullptr ? params->get("url")->text : "";
        respond(id, "{\"frameId\":\"F1\",\"loaderId\":\"L1\"}", session);
        if (marked("fake-crash-any") ||
            (url.find("crash") != std::string::npos && (marked("fake-crash-always") || !marked("fake-crashed")))) {
            std::ofstream(profile + "/fake-crashed") << "1";
            flushOut();
            std::fprintf(logFile, "CRASH\n");
            std::fflush(logFile);
            std::_Exit(3);
        }
        if (url.find("popup") != std::string::npos) {
            const std::string opener = currentTarget();
            const std::string made = "P" + std::to_string(nextPopup++);
            popups.push_back(made);
            event("Target.targetCreated",
                  "{\"targetInfo\":{\"targetId\":" + jsonString(made) + ",\"type\":\"page\",\"url\":\"about:blank\",\"title\":\"\","
                  "\"attached\":false,\"openerId\":" + jsonString(opener) + "}}",
                  "");
            return;
        }
        if (url.find("close") != std::string::npos && !popups.empty()) {
            const std::string gone = popups.back();
            popups.pop_back();
            destroyTarget(gone);
            return;
        }
        for (const char* type : {"alert", "confirm", "prompt", "beforeunload"}) {
            if (url.find(type) == std::string::npos) continue;
            event("Page.javascriptDialogOpening",
                  "{\"url\":" + jsonString(url) + ",\"message\":\"Sure?\",\"type\":\"" + type +
                      "\",\"hasBrowserHandler\":false,\"defaultPrompt\":\"fill\"}",
                  session);
            return;
        }
        history.resize(static_cast<size_t>(current + 1));
        history.push_back({nextEntry++, url});
        current = static_cast<int>(history.size()) - 1;
        navigated(session);
        if (url.find("renderfail") != std::string::npos) rendererCrash(session);
    } else if (method == "Page.reload") {
        respond(id, "{}", session);
        if (marked("fake-reload-asks")) {
            // The page asks "leave this page?" before it is read again; nothing loads (the answer only closes it)
            event("Page.javascriptDialogOpening",
                  "{\"url\":\"\",\"message\":\"\",\"type\":\"beforeunload\",\"hasBrowserHandler\":false,"
                  "\"defaultPrompt\":\"\"}",
                  session);
        } else if (current >= 0 && history[static_cast<size_t>(current)].url.find("renderfail") != std::string::npos) {
            rendererCrash(session);
        } else if (current >= 0) {
            navigated(session);
        }
    } else if (method == "Target.closeTarget") {
        const std::string gone = params != nullptr && params->get("targetId") != nullptr ? params->get("targetId")->text : "";
        respond(id, "{\"success\":true}", session);
        const auto it = std::find(popups.begin(), popups.end(), gone);
        if (it != popups.end()) {
            popups.erase(it);
            destroyTarget(gone);
        }
    } else if (method.rfind("Input.", 0) == 0 && marked("fake-input-hang")) {
        return;  // never answered (a renderer that is stuck)
    } else if (method == "Page.getNavigationHistory") {
        std::string entries;
        for (const Entry& e : history) {
            if (!entries.empty()) entries += ",";
            entries += "{\"id\":" + std::to_string(e.id) + ",\"url\":" + jsonString(e.url) + ",\"userTypedURL\":" +
                       jsonString(e.url) + ",\"title\":" + jsonString("Title of " + e.url) + ",\"transitionType\":\"typed\"}";
        }
        respond(id, "{\"currentIndex\":" + std::to_string(current) + ",\"entries\":[" + entries + "]}", session);
    } else if (method == "Page.navigateToHistoryEntry") {
        const int entry = params != nullptr && params->get("entryId") != nullptr ? static_cast<int>(params->get("entryId")->number) : -1;
        for (size_t i = 0; i < history.size(); ++i) {
            if (history[i].id == entry) current = static_cast<int>(i);
        }
        respond(id, "{}", session);
        if (current >= 0) navigated(session);
    } else if (method == "Page.startScreencast") {
        respond(id, "{}", session);
        screencastSession = session;
        sendFrame();
    } else if (method == "Page.stopScreencast") {
        respond(id, "{}", session);
        screencastSession.clear();
    } else if (method == "Page.screencastFrameAck") {
        respond(id, "{}", session);
        sendFrame();
    } else if (method == "Runtime.evaluate") {
        const std::string expression = params != nullptr && params->get("expression") != nullptr ? params->get("expression")->text : "";
        if (expression.find("elementFromPoint") != std::string::npos) {
            // The context menu's link lookup: a link everywhere but at the left edge (x < 10)
            const bool edge = expression.find("elementFromPoint(5,") != std::string::npos;
            respond(id, std::string("{\"result\":{\"type\":\"string\",\"value\":\"") +
                            (edge ? "" : "https://link.example/") + "\"}}",
                    session);
        } else if (expression.find(".some(") != std::string::npos) {
            if (::access((profile + "/fake-audible-hang").c_str(), F_OK) == 0) return;  // never answered
            const std::string audibleFile = profile + "/fake-audible", pausedFile = profile + "/fake-media-paused";
            // The video controls act on the files: play / pause swaps them, pausing everything pauses
            const bool playing = ::access(audibleFile.c_str(), F_OK) == 0;
            const bool paused = ::access(pausedFile.c_str(), F_OK) == 0;
            const bool toggle = expression.find("m.play()") != std::string::npos;
            const bool pauseAll = expression.find("x.pause()") != std::string::npos;
            if ((toggle || pauseAll) && playing) {
                ::unlink(audibleFile.c_str());
                std::ofstream(pausedFile) << "1";
            } else if (toggle && paused) {
                ::unlink(pausedFile.c_str());
                std::ofstream(audibleFile) << "1";
            }
            const bool nowPlaying = ::access(audibleFile.c_str(), F_OK) == 0;
            const bool nowPaused = ::access(pausedFile.c_str(), F_OK) == 0;
            // "<audible><n / r / p><muted> <position> <duration>" (the session's media script)
            std::string state = nowPlaying ? "1r- 42.5 300" : nowPaused ? "0p- 42.5 300" : "0n- -1 NaN";
            if (pauseAll) state += playing ? " w1" : " w0";  // pause everything says whether something played
            respond(id, "{\"result\":{\"type\":\"string\",\"value\":\"" + state + "\"}}", session);
        } else {
            respond(id, "{\"result\":{\"type\":\"undefined\"}}", session);
        }
    } else if (method == "Browser.getWindowForTarget") {
        // A window per page: T1 → 1, T2 → 2, P1 → 101, ...
        const std::string target = params != nullptr && params->get("targetId") != nullptr ? params->get("targetId")->text : "";
        const int window = target.size() > 1 ? (target[0] == 'P' ? 100 : 0) + std::atoi(target.c_str() + 1) : 0;
        respond(id, "{\"windowId\":" + std::to_string(window) +
                        ",\"bounds\":{\"left\":0,\"top\":0,\"width\":1280,\"height\":720,\"windowState\":\"normal\"}}",
                session);
    } else if (method == "Browser.getVersion") {
        respond(id, "{\"protocolVersion\":\"1.3\",\"product\":\"FakeChrome/1.0\",\"userAgent\":\"fake\"}", session);
    } else if (method == "Browser.close") {
        respond(id, "{}", session);
        flushOut();
        if (mode != "ignore-term") std::exit(0);
    } else {
        respond(id, "{}", session);
    }
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--user-data-dir=", 0) == 0) profile = a.substr(16);
        if (a == "--fake-helper") {
            for (;;) ::pause();
        }
    }
    if (profile.empty()) return 2;
    ::signal(SIGPIPE, SIG_IGN);
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--remote-debugging-pipe") pipeMode = true;
    }
    if (::access((profile + "/fake-nondumpable").c_str(), F_OK) == 0) ::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    if (::access((profile + "/fake-detached-helper").c_str(), F_OK) == 0) {
        const pid_t helper = ::fork();
        if (helper == 0) {
            ::setsid();  // its own session and process group: no signal to ours reaches it
            const std::string database = "--database=" + profile + "/Crash Reports";
            const char* args[] = {"fake-chrome-helper", "--fake-helper", "--type=crashpad-handler", database.c_str(), nullptr};
            ::execv("/proc/self/exe", const_cast<char* const*>(args));
            ::_exit(127);
        }
        std::ofstream(profile + "/fake-helper.pid") << helper;
    }
    if (const char* m = std::getenv("FAKE_CHROME_MODE")) mode = m;
    logFile = std::fopen((profile + "/fake-log.txt").c_str(), "a");
    if (logFile == nullptr) return 2;
    std::fprintf(logFile, "START %d\n", static_cast<int>(::getpid()));
    std::fflush(logFile);
    if (mode == "ignore-term") ::signal(SIGTERM, SIG_IGN);
    if (mode == "spawn-child") {
        const pid_t child = ::fork();
        if (child == 0) {
            for (;;) ::pause();
        }
        std::ofstream(profile + "/fake-child.pid") << child;
    }
    if (mode == "no-port") {
        for (;;) ::pause();
    }
    if (marked("fake-log-flood")) {
        const std::string line(1023, 'x');
        for (int i = 0; i < 6 * 1024; ++i) std::fprintf(stderr, "%s\n", line.c_str());
        std::fflush(stderr);
    }
    // The frame: a 64×36 gradient, red on the left
    BgraFrame picture;
    picture.width = 64;
    picture.height = 36;
    picture.pixels.resize(64 * 36 * 4);
    for (int y = 0; y < 36; ++y) {
        for (int x = 0; x < 64; ++x) {
            uint8_t* p = &picture.pixels[(static_cast<size_t>(y) * 64 + x) * 4];
            p[0] = static_cast<uint8_t>(x * 4);        // B
            p[1] = 40;                                 // G
            p[2] = static_cast<uint8_t>(255 - x * 4);  // R
            p[3] = 255;
        }
    }
    std::string jpeg;
    if (!JpegDecoder::encode(picture, 90, jpeg)) return 2;
    frameData = ws::base64Encode(jpeg);

    if (pipeMode) {
        std::fprintf(logFile, "PIPE\n");
        std::fflush(logFile);
        if (marked("fake-pipe-exit")) return 0;
        if (marked("fake-pipe-closed")) {
            ::close(3);
            ::close(4);
        }
        if (marked("fake-pipe-deaf") || marked("fake-pipe-closed")) {
            for (;;) ::pause();
        }
        client = 4;
        std::string in;
        char buffer[65536];
        for (;;) {
            const ssize_t n = ::read(3, buffer, sizeof(buffer));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return 0;  // the pipe closed: Chrome quits
            in.append(buffer, static_cast<size_t>(n));
            size_t zero;
            while ((zero = in.find('\0')) != std::string::npos) {
                const std::string message = in.substr(0, zero);
                in.erase(0, zero + 1);
                handle(message);
            }
            flushOut();
        }
    }
    const int server = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(server, 4) != 0) return 2;
    socklen_t len = sizeof(addr);
    ::getsockname(server, reinterpret_cast<sockaddr*>(&addr), &len);
    {
        const std::string temp = profile + "/DevToolsActivePort.tmp";
        std::ofstream(temp) << ntohs(addr.sin_port) << "\n/devtools/browser/fake-browser-id\n";
        std::rename(temp.c_str(), (profile + "/DevToolsActivePort").c_str());
    }
    for (;;) {
        client = ::accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;
        std::string in;
        bool open = false;
        ws::FrameParser parser;
        char buffer[65536];
        for (;;) {
            const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            if (!open) {
                in.append(buffer, static_cast<size_t>(n));
                size_t consumed = 0;
                std::string path, response;
                const ws::Handshake h = ws::parseHandshakeRequest(in, consumed, path, response);
                if (h == ws::Handshake::Incomplete) continue;
                if (h == ws::Handshake::Failed) break;
                out += response;
                flushOut();
                open = true;
                std::vector<ws::FrameParser::Message> messages;
                parser.feed(in.data() + consumed, in.size() - consumed, messages);
                for (auto& m : messages) {
                    if (m.opcode == ws::Text) handle(m.payload);
                }
                flushOut();
                continue;
            }
            std::vector<ws::FrameParser::Message> messages;
            if (!parser.feed(buffer, static_cast<size_t>(n), messages)) break;
            for (auto& m : messages) {
                if (m.opcode == ws::Text) handle(m.payload);
                if (m.opcode == ws::Close) {
                    ::close(client);
                    client = -1;
                    break;
                }
            }
            if (client < 0) break;
            flushOut();
        }
        if (client >= 0) ::close(client);
        client = -1;
        screencastSession.clear();
    }
}
