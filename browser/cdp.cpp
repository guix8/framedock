// DevTools protocol messages. See cdp.h.
#include "cdp.h"

#include "websocket.h"  // base64

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cdp {
namespace {

std::string str(const JsonValue* v) { return v != nullptr && v->isString() ? v->text : std::string(); }
double num(const JsonValue* v, double fallback = 0) { return v != nullptr && v->isNumber() ? v->number : fallback; }

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

/** @return true if v is a usable frame session id (finite, in int's range): then out = it; a cast of anything else
 *          (inf, nan, 1e20) to int is undefined behaviour */
bool toFrameSession(double v, int& out) {
    if (!(v >= static_cast<double>(INT_MIN) && v <= static_cast<double>(INT_MAX))) return false;
    out = static_cast<int>(v);
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/** @return true if text (no spaces) looks like a host name, maybe with a port and a path: "youtube.com", "localhost:8080/x" */
bool looksLikeHost(const std::string& text) {
    if (text.empty() || text.find(' ') != std::string::npos) return false;
    const size_t end = text.find_first_of("/?#");
    std::string host = text.substr(0, end);
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) {
        const std::string port = host.substr(colon + 1);
        if (port.empty() || !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return false;
        }
        host = host.substr(0, colon);
    }
    if (lower(host) == "localhost") return true;
    const size_t dot = host.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= host.size() || host.front() == '.') return false;
    // The last label: letters only (a TLD) or all digits (an IPv4 address's last part)
    const std::string tld = host.substr(dot + 1);
    const bool letters = std::all_of(tld.begin(), tld.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    });
    // An IPv4 address: four dotted numbers
    const bool ipv4 = std::count(host.begin(), host.end(), '.') == 3 &&
                      std::all_of(host.begin(), host.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
    if (!(letters && tld.size() >= 2) && !ipv4) return false;
    return std::all_of(host.begin(), host.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
               static_cast<unsigned char>(c) >= 0x80;
    });
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

}  // namespace

Message parse(const std::string& text) {
    Message m;
    JsonValue root;
    std::string error;
    if (!parseJson(text, root, error) || !root.isObject()) return m;
    m.sessionId = str(root.get("sessionId"));
    if (const JsonValue* id = root.get("id"); id != nullptr && id->isNumber()) {
        m.kind = Message::Kind::Response;
        m.id = static_cast<int>(id->number);
        if (const JsonValue* err = root.get("error")) {
            m.failed = true;
            m.errorMessage = str(err->get("message"));
            if (m.errorMessage.empty()) m.errorMessage = "error";
        } else if (const JsonValue* result = root.get("result")) {
            m.body = *result;
        }
        return m;
    }
    if (const JsonValue* method = root.get("method"); method != nullptr && method->isString()) {
        m.kind = Message::Kind::Event;
        m.method = method->text;
        if (const JsonValue* params = root.get("params")) m.body = *params;
    }
    return m;
}

std::string Writer::command(const std::string& method, const std::string& params, const std::string& sessionId,
                            int& id) {
    id = nextId_++;
    std::string out = "{\"id\":" + std::to_string(id) + ",\"method\":" + jsonString(method) + ",\"params\":" +
                      (params.empty() ? std::string("{}") : params);
    if (!sessionId.empty()) out += ",\"sessionId\":" + jsonString(sessionId);
    out += '}';
    return out;
}

std::string number(double value) {
    if (!std::isfinite(value)) return "0";
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    std::string s = buffer;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

std::string navigateParams(const std::string& url) { return "{\"url\":" + jsonString(url) + "}"; }

std::string screencastParams(int maxWidth, int maxHeight, int quality) {
    return "{\"format\":\"jpeg\",\"quality\":" + std::to_string(std::clamp(quality, 10, 100)) +
           ",\"maxWidth\":" + std::to_string(maxWidth) + ",\"maxHeight\":" + std::to_string(maxHeight) +
           ",\"everyNthFrame\":1}";
}

std::string screencastAckParams(int frameSessionId) { return "{\"sessionId\":" + std::to_string(frameSessionId) + "}"; }

std::string metricsParams(int width, int height, bool mobile) {
    return "{\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) +
           ",\"deviceScaleFactor\":1,\"mobile\":" + (mobile ? "true" : "false") + "}";
}

std::string userAgentParams(const std::string& userAgent, bool mobile) {
    if (!mobile) return "{\"userAgent\":" + jsonString(userAgent) + "}";
    return "{\"userAgent\":" + jsonString(userAgent) +
           ",\"userAgentMetadata\":{\"platform\":\"Android\",\"platformVersion\":\"14\",\"architecture\":\"\","
           "\"model\":\"Pixel 8\",\"mobile\":true}}";
}

std::string createTargetParams(const std::string& url) { return "{\"url\":" + jsonString(url) + "}"; }
std::string detachParams(const std::string& sessionId) { return "{\"sessionId\":" + jsonString(sessionId) + "}"; }
std::string closeTargetParams(const std::string& targetId) { return "{\"targetId\":" + jsonString(targetId) + "}"; }
std::string lifecycleParams(bool frozen) {
    return std::string("{\"state\":\"") + (frozen ? "frozen" : "active") + "\"}";
}
std::string windowForTargetParams(const std::string& targetId) { return "{\"targetId\":" + jsonString(targetId) + "}"; }
std::string windowStateParams(int windowId, bool minimized) {
    return "{\"windowId\":" + std::to_string(windowId) + ",\"bounds\":{\"windowState\":\"" +
           (minimized ? "minimized" : "normal") + "\"}}";
}
std::string dialogParams(bool accept, const std::string& promptText) {
    return std::string("{\"accept\":") + (accept ? "true" : "false") +
           (accept && !promptText.empty() ? ",\"promptText\":" + jsonString(promptText) : std::string()) + "}";
}
std::string attachParams(const std::string& targetId) {
    return "{\"targetId\":" + jsonString(targetId) + ",\"flatten\":true}";
}
std::string evaluateParams(const std::string& expression, bool userGesture) {
    return "{\"expression\":" + jsonString(expression) + ",\"returnByValue\":true" +
           (userGesture ? ",\"userGesture\":true}" : "}");
}
std::string historyEntryParams(int entryId) { return "{\"entryId\":" + std::to_string(entryId) + "}"; }

std::string mouseParams(const char* type, double x, double y, Button button, int clickCount, int buttons) {
    const char* name = button == Button::Left ? "left" : button == Button::Right ? "right"
                       : button == Button::Middle ? "middle" : "none";
    return std::string("{\"type\":\"") + type + "\",\"x\":" + number(x) + ",\"y\":" + number(y) + ",\"button\":\"" +
           name + "\",\"buttons\":" + std::to_string(buttons) + ",\"clickCount\":" + std::to_string(clickCount) + "}";
}

std::string wheelParams(double x, double y, double deltaX, double deltaY) {
    return "{\"type\":\"mouseWheel\",\"x\":" + number(x) + ",\"y\":" + number(y) + ",\"deltaX\":" + number(deltaX) +
           ",\"deltaY\":" + number(deltaY) + "}";
}

std::string insertTextParams(const std::string& text) { return "{\"text\":" + jsonString(text) + "}"; }

std::string keyParams(Key key, bool down) {
    struct Info {
        const char* key;
        const char* code;
        int vk;
        const char* text;  // sent with the key down (a keypress), or nullptr
    };
    Info info {"", "", 0, nullptr};
    int modifiers = 0;
    const char* command = nullptr;
    switch (key) {
        case Key::Backspace: info = {"Backspace", "Backspace", 8, nullptr}; break;
        case Key::Enter: info = {"Enter", "Enter", 13, "\r"}; break;
        case Key::Tab: info = {"Tab", "Tab", 9, nullptr}; break;
        case Key::Escape: info = {"Escape", "Escape", 27, nullptr}; break;
        case Key::Left: info = {"ArrowLeft", "ArrowLeft", 37, nullptr}; break;
        case Key::Up: info = {"ArrowUp", "ArrowUp", 38, nullptr}; break;
        case Key::Right: info = {"ArrowRight", "ArrowRight", 39, nullptr}; break;
        case Key::Down: info = {"ArrowDown", "ArrowDown", 40, nullptr}; break;
        case Key::Delete: info = {"Delete", "Delete", 46, nullptr}; break;
        case Key::Home: info = {"Home", "Home", 36, nullptr}; break;
        case Key::End: info = {"End", "End", 35, nullptr}; break;
        case Key::SelectAll:
            info = {"a", "KeyA", 65, nullptr};
            modifiers = 2;  // Ctrl
            command = "selectAll";
            break;
        case Key::Copy:
            info = {"c", "KeyC", 67, nullptr};
            modifiers = 2;
            command = "copy";
            break;
        case Key::Paste:
            info = {"v", "KeyV", 86, nullptr};
            modifiers = 2;
            command = "paste";
            break;
    }
    std::string type = !down ? "keyUp" : info.text != nullptr ? "keyDown" : "rawKeyDown";
    std::string out = "{\"type\":\"" + type + "\",\"key\":" + jsonString(info.key) + ",\"code\":" + jsonString(info.code) +
                      ",\"windowsVirtualKeyCode\":" + std::to_string(info.vk) +
                      ",\"nativeVirtualKeyCode\":" + std::to_string(info.vk) + ",\"modifiers\":" + std::to_string(modifiers);
    if (down && info.text != nullptr) out += ",\"text\":" + jsonString(info.text) + ",\"unmodifiedText\":" + jsonString(info.text);
    if (down && command != nullptr) out += ",\"commands\":[" + jsonString(command) + "]";
    out += '}';
    return out;
}

bool parseActivePort(const std::string& content, int& port, std::string& path) {
    const size_t nl = content.find('\n');
    if (nl == std::string::npos) return false;
    const std::string first = trim(content.substr(0, nl));
    char* end = nullptr;
    const long value = std::strtol(first.c_str(), &end, 10);
    if (first.empty() || end == nullptr || *end != '\0' || value <= 0 || value > 65535) return false;
    const size_t nl2 = content.find('\n', nl + 1);
    path = trim(content.substr(nl + 1, nl2 == std::string::npos ? std::string::npos : nl2 - nl - 1));
    if (!startsWith(path, "/devtools/browser/")) return false;
    port = static_cast<int>(value);
    return true;
}

bool readScreencastFrame(const JsonValue& params, ScreencastFrame& frame) {
    const JsonValue* data = params.get("data");
    const JsonValue* session = params.get("sessionId");
    if (data == nullptr || !data->isString() || session == nullptr || !session->isNumber()) return false;
    int frameSession = 0;
    if (!toFrameSession(session->number, frameSession)) return false;
    if (!ws::base64Decode(data->text, frame.jpeg) || frame.jpeg.size() < 4) return false;
    frame.sessionId = frameSession;
    if (const JsonValue* meta = params.get("metadata")) {
        frame.deviceWidth = num(meta->get("deviceWidth"));
        frame.deviceHeight = num(meta->get("deviceHeight"));
        frame.offsetTop = num(meta->get("offsetTop"));
        frame.pageScale = num(meta->get("pageScaleFactor"), 1);
    }
    return true;
}

namespace {

/** A small scanner over one JSON text: enough to walk Chrome's screencast message without building values */
struct Scan {
    const std::string& t;
    size_t i = 0;
    void ws() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i;
    }
    bool eat(char c) {
        ws();
        if (i < t.size() && t[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    /** A string without escapes: [begin, end) of its content. @return false if it isn't one (or has escapes) */
    bool plainString(size_t& begin, size_t& end) {
        ws();
        if (i >= t.size() || t[i] != '"') return false;
        begin = i + 1;
        const char* data = t.data();
        const void* q = std::memchr(data + begin, '"', t.size() - begin);
        if (q == nullptr) return false;
        end = static_cast<size_t>(static_cast<const char*>(q) - data);
        if (std::memchr(data + begin, '\\', end - begin) != nullptr) return false;
        i = end + 1;
        return true;
    }
    bool key(std::string& name) {
        size_t b = 0, e = 0;
        if (!plainString(b, e) || !eat(':')) return false;
        name.assign(t, b, e - b);
        return true;
    }
    /** Skips one value (object, array, string, number, literal). @return its [begin, end) */
    bool skipValue(size_t& begin, size_t& end) {
        ws();
        begin = i;
        if (i >= t.size()) return false;
        const char c = t[i];
        if (c == '"') {
            ++i;
            for (; i < t.size(); ++i) {
                if (t[i] == '\\') {
                    ++i;
                } else if (t[i] == '"') {
                    ++i;
                    end = i;
                    return true;
                }
            }
            return false;
        }
        if (c == '{' || c == '[') {
            int depth = 0;
            for (; i < t.size(); ++i) {
                const char d = t[i];
                if (d == '"') {
                    size_t b = 0, e = 0;
                    if (!skipValue(b, e)) return false;
                    --i;  // the loop's ++i
                } else if (d == '{' || d == '[') {
                    ++depth;
                } else if (d == '}' || d == ']') {
                    if (--depth == 0) {
                        ++i;
                        end = i;
                        return true;
                    }
                }
            }
            return false;
        }
        while (i < t.size() && t[i] != ',' && t[i] != '}' && t[i] != ']') ++i;
        end = i;
        return end > begin;
    }
};

}  // namespace

FastFrame readScreencastMessage(const std::string& text, ScreencastFrame& frame, std::string& sessionId) {
    static const char kPrefix[] = "{\"method\":\"Page.screencastFrame\",\"params\":";
    if (text.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) {
        // Not Chrome's frame shape: a frame in another key order is still read by the full parser
        return text.find("\"Page.screencastFrame\"") == std::string::npos ? FastFrame::NotAFrame : FastFrame::Fallback;
    }
    Scan scan {text, sizeof(kPrefix) - 1};
    if (!scan.eat('{')) return FastFrame::Fallback;
    size_t dataBegin = 0, dataEnd = 0, metaBegin = 0, metaEnd = 0;
    bool haveData = false, haveSession = false, haveMeta = false;
    int frameSession = 0;
    std::string name;
    if (!scan.eat('}')) {
        do {
            if (!scan.key(name)) return FastFrame::Fallback;
            if (name == "data") {
                if (!scan.plainString(dataBegin, dataEnd)) return FastFrame::Fallback;
                haveData = true;
            } else {
                size_t b = 0, e = 0;
                if (!scan.skipValue(b, e)) return FastFrame::Fallback;
                if (name == "sessionId") {
                    char* stop = nullptr;
                    const std::string number(text, b, e - b);
                    const double value = std::strtod(number.c_str(), &stop);
                    while (*stop == ' ' || *stop == '\t' || *stop == '\r' || *stop == '\n') ++stop;
                    // Not all a number, or none an int holds (inf, 1e20): the full parser decides
                    if (stop == number.c_str() || *stop != '\0' || !toFrameSession(value, frameSession)) {
                        return FastFrame::Fallback;
                    }
                    haveSession = true;
                } else if (name == "metadata") {
                    metaBegin = b;
                    metaEnd = e;
                    haveMeta = true;
                }
            }
        } while (scan.eat(','));
        if (!scan.eat('}')) return FastFrame::Fallback;
    }
    sessionId.clear();
    while (scan.eat(',')) {
        if (!scan.key(name)) return FastFrame::Fallback;
        if (name == "sessionId") {
            size_t b = 0, e = 0;
            if (!scan.plainString(b, e)) return FastFrame::Fallback;
            sessionId.assign(text, b, e - b);
        } else {
            size_t b = 0, e = 0;
            if (!scan.skipValue(b, e)) return FastFrame::Fallback;
        }
    }
    if (!scan.eat('}') || !haveData || !haveSession) return FastFrame::Fallback;
    if (!ws::base64Decode(text.data() + dataBegin, dataEnd - dataBegin, frame.jpeg) || frame.jpeg.size() < 4) {
        return FastFrame::Fallback;
    }
    frame.sessionId = frameSession;
    frame.deviceWidth = frame.deviceHeight = frame.offsetTop = 0;
    frame.pageScale = 1;
    if (haveMeta) {
        JsonValue meta;
        std::string error;
        if (parseJson(text.substr(metaBegin, metaEnd - metaBegin), meta, error)) {
            frame.deviceWidth = num(meta.get("deviceWidth"));
            frame.deviceHeight = num(meta.get("deviceHeight"));
            frame.offsetTop = num(meta.get("offsetTop"));
            frame.pageScale = num(meta.get("pageScaleFactor"), 1);
        }
    }
    return FastFrame::Frame;
}

int History::entryAt(int step) const {
    const int i = currentIndex + step;
    if (currentIndex < 0 || i < 0 || i >= static_cast<int>(entryIds.size())) return -1;
    return entryIds[static_cast<size_t>(i)];
}

bool readHistory(const JsonValue& result, History& history) {
    const JsonValue* index = result.get("currentIndex");
    const JsonValue* entries = result.get("entries");
    if (index == nullptr || !index->isNumber() || entries == nullptr || entries->type != JsonValue::Type::Array) {
        return false;
    }
    history = History {};
    history.currentIndex = static_cast<int>(index->number);
    for (const JsonValue& e : entries->items) history.entryIds.push_back(static_cast<int>(num(e.get("id"), -1)));
    if (history.currentIndex >= 0 && history.currentIndex < static_cast<int>(entries->items.size())) {
        const JsonValue& current = entries->items[static_cast<size_t>(history.currentIndex)];
        history.url = str(current.get("url"));
        history.title = str(current.get("title"));
    }
    return true;
}

bool readBoolResult(const JsonValue& result, bool& value) {
    const JsonValue* inner = result.get("result");
    if (inner == nullptr) return false;
    const JsonValue* v = inner->get("value");
    if (v == nullptr || !v->isBool()) return false;
    value = v->boolean;
    return true;
}

bool readStringResult(const JsonValue& result, std::string& value) {
    const JsonValue* inner = result.get("result");
    if (inner == nullptr) return false;
    const JsonValue* v = inner->get("value");
    if (v == nullptr || !v->isString()) return false;
    value = v->text;
    return true;
}

std::string linkAtScript(double x, double y) {
    return "(() => { const e = document.elementFromPoint(" + number(x) + ", " + number(y) +
           "); const a = e && e.closest('a[href]'); return a ? a.href : ''; })()";
}

std::string urlEncode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string addressToUrl(const std::string& input) {
    const std::string text = trim(input);
    if (text.empty()) return {};
    const std::string low = lower(text);
    if (text.find(' ') == std::string::npos &&
        (low.find("://") != std::string::npos || startsWith(low, "about:") || startsWith(low, "chrome:") ||
         startsWith(low, "data:") || startsWith(low, "file:") || startsWith(low, "view-source:"))) {
        return text;
    }
    if (looksLikeHost(text)) return "https://" + text;
    return "https://www.google.com/search?q=" + urlEncode(text);
}

bool ViewMap::toPage(double x, double y, double& cssX, double& cssY) const {
    if (areaW <= 0 || areaH <= 0) return false;
    if (x < areaX || y < areaY || x > areaX + areaW || y > areaY + areaH) return false;
    cssX = std::clamp((x - areaX) * viewportW / areaW, 0.0, static_cast<double>(viewportW - 1));
    cssY = std::clamp((y - areaY) * viewportH / areaH, 0.0, static_cast<double>(viewportH - 1));
    return true;
}

}  // namespace cdp
