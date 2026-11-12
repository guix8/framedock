// Frame Sync's network side. See frame_sync_agent.h.
#include "frame_sync_agent.h"

#include "atomic_file.h"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace framesync {

namespace {

std::string home() {
    const char* h = std::getenv("HOME");
    return h != nullptr ? h : "";
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream text;
    text << in.rdbuf();
    out = text.str();
    return !in.bad();
}

std::string strip(const std::string& s) {
    const char* space = " \t\r\n\f\v";
    const size_t a = s.find_first_not_of(space);
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(space) - a + 1);
}

/** Python's bool(value) for JSON values. */
bool truthy(const JsonValue* v) {
    if (v == nullptr) return false;
    switch (v->type) {
        case JsonValue::Type::Null: return false;
        case JsonValue::Type::Bool: return v->boolean;
        case JsonValue::Type::Number: return v->number != 0;
        case JsonValue::Type::String: return !v->text.empty();
        case JsonValue::Type::Array: return !v->items.empty();
        case JsonValue::Type::Object: return !v->members.empty();
    }
    return false;
}

std::string numberText(double n) {
    char buffer[64];
    if (std::isfinite(n) && n == std::floor(n) && std::fabs(n) < 1e17) {
        std::snprintf(buffer, sizeof(buffer), "%.0f", n);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.17g", n);
    }
    return buffer;
}

/** A field as text: a string as it is, a number written out (gameAppId may be either), else "". */
std::string textOf(const JsonValue* v) {
    if (v == nullptr) return {};
    if (v->isString()) return v->text;
    if (v->isNumber()) return v->number != 0 ? numberText(v->number) : std::string();
    return {};
}

/** agent_client._json: an object, or {} for anything else (bad JSON, a list). */
JsonValue objectOf(const std::string& raw) {
    JsonValue value;
    std::string error;
    if (!parseJson(raw.empty() ? std::string("{}") : raw, value, error) || !value.isObject()) {
        value = JsonValue();
        value.type = JsonValue::Type::Object;
    }
    return value;
}

std::string stringField(const JsonValue& object, const char* key) {
    const JsonValue* v = object.get(key);
    return v != nullptr && v->isString() ? v->text : std::string();
}

size_t collect(char* data, size_t size, size_t count, void* user) {
    std::string& out = *static_cast<std::string*>(user);
    constexpr size_t kMaxBody = 8u << 20;  // an avatar is a few KiB; anything this big is not our agent
    if (out.size() + size * count > kMaxBody) return 0;
    out.append(data, size * count);
    return size * count;
}

int progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* cancel = static_cast<const std::atomic<bool>*>(user);
    return cancel != nullptr && cancel->load() ? 1 : 0;  // non-zero aborts the transfer
}

}  // namespace

std::string defaultConfigDir() {
    return home() + "/.config/frame-sync";
}

std::string defaultCacheFile() {
    return home() + "/.cache/frame-sync/pc-accounts.json";
}

bool settingsPresent(const std::string& configDir) {
    struct stat config {}, token {};
    return ::stat((configDir + "/config.json").c_str(), &config) == 0 && S_ISREG(config.st_mode) &&
           ::stat((configDir + "/token").c_str(), &token) == 0 && S_ISREG(token.st_mode) && token.st_size > 0;
}

std::optional<Settings> loadSettings(const std::string& configDir) {
    std::string text;
    if (!readFile(configDir + "/config.json", text)) return std::nullopt;
    JsonValue config;
    std::string error;
    if (!parseJson(text, config, error) || !config.isObject()) return std::nullopt;
    Settings s;
    s.agentUrl = stringField(config, "agentUrl");
    s.pcMac = stringField(config, "pcMac");
    s.broadcast = stringField(config, "broadcast");
    std::string token;
    if (!readFile(configDir + "/token", token)) return std::nullopt;
    s.token = strip(token);
    if (s.agentUrl.empty() || s.pcMac.empty() || s.broadcast.empty() || s.token.empty()) return std::nullopt;
    return s;
}

PcStatus statusFrom(const JsonValue& json) {
    PcStatus status;
    const JsonValue* steam = json.get("steam");
    if (steam != nullptr && steam->isObject()) {
        status.steamRunning = truthy(steam->get("running"));
        status.steamId = textOf(steam->get("steamId"));
        status.gameAppId = textOf(steam->get("gameAppId"));
    }
    status.streaming = truthy(json.get("streaming"));
    status.hasUsers = truthy(json.get("users"));
    return status;
}

std::vector<PcAccount> pcAccountsFrom(const JsonValue& accounts) {
    std::vector<PcAccount> list;
    if (accounts.type != JsonValue::Type::Array) return list;
    for (const JsonValue& item : accounts.items) {
        if (!item.isObject()) continue;
        PcAccount a;
        a.steamId = textOf(item.get("steamId"));
        if (a.steamId.empty()) continue;
        a.personaName = stringField(item, "personaName");
        a.accountName = stringField(item, "accountName");
        a.hasAvatar = truthy(item.get("hasAvatar"));
        list.push_back(a);
    }
    return list;
}

std::string pythonJson(const JsonValue& value) {
    switch (value.type) {
        case JsonValue::Type::Null: return "null";
        case JsonValue::Type::Bool: return value.boolean ? "true" : "false";
        case JsonValue::Type::Number: return numberText(value.number);
        case JsonValue::Type::String: return jsonString(value.text);
        case JsonValue::Type::Array: {
            std::string out = "[";
            for (size_t i = 0; i < value.items.size(); ++i) {
                if (i > 0) out += ", ";
                out += pythonJson(value.items[i]);
            }
            return out + "]";
        }
        case JsonValue::Type::Object: {
            std::string out = "{";
            for (size_t i = 0; i < value.members.size(); ++i) {
                if (i > 0) out += ", ";
                out += jsonString(value.members[i].first) + ": " + pythonJson(value.members[i].second);
            }
            return out + "}";
        }
    }
    return "null";
}

std::optional<std::vector<PcAccount>> loadCachedPcAccounts(const std::string& file) {
    std::string text;
    if (!readFile(file, text)) return std::nullopt;
    JsonValue value;
    std::string error;
    if (!parseJson(text, value, error) || value.type != JsonValue::Type::Array) return std::nullopt;
    return pcAccountsFrom(value);
}

bool saveCachedPcAccounts(const std::string& file, const JsonValue& accounts, std::string& error, bool* written) {
    const std::string text = pythonJson(accounts);
    std::string old;
    if (written != nullptr) *written = false;
    if (readFile(file, old) && old == text) return true;
    if (!frame_perf_overlay::atomicWriteFile(file, text, error)) return false;
    if (written != nullptr) *written = true;
    return true;
}

bool sendWol(const std::string& mac, const std::string& broadcast, std::string& error, int port) {
    const std::vector<uint8_t> packet = magicPacket(mac);
    if (packet.empty()) {
        error = "bad MAC address";
        return false;
    }
    sockaddr_in to {};
    to.sin_family = AF_INET;
    to.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, broadcast.c_str(), &to.sin_addr) != 1) {
        error = "bad broadcast address";
        return false;
    }
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = "socket failed";
        return false;
    }
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    int sent = 0;
    for (int i = 0; i < 3; ++i) {
        if (::sendto(fd, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) ==
            static_cast<ssize_t>(packet.size())) {
            ++sent;
        }
    }
    ::close(fd);
    if (sent == 0) error = "sendto failed";
    return sent > 0;
}

void initHttp() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

HttpAgent::HttpAgent(std::string baseUrl, std::string token, double timeoutSec, const std::atomic<bool>* cancel)
    : base_(std::move(baseUrl)), token_(strip(token)), timeoutSec_(timeoutSec), cancel_(cancel) {
    while (!base_.empty() && base_.back() == '/') base_.pop_back();
}

HttpAgent::Response HttpAgent::request(const char* method, const std::string& path, const std::string* body,
                                       std::initializer_list<int> ok) {
    initHttp();
    CURL* curl = curl_easy_init();
    if (curl == nullptr) throw AgentError(AgentError::Kind::Unreachable, 0, {}, "curl_easy_init failed");
    Response response;
    const std::string url = base_ + path;
    const std::string auth = "Authorization: Bearer " + token_;
    curl_slist* headers = curl_slist_append(nullptr, auth.c_str());
    if (body != nullptr) headers = curl_slist_append(headers, "Content-Type: application/json");
    const long timeoutMs = static_cast<long>(timeoutSec_ * 1000);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");  // the agent is on the LAN
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (body != nullptr) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
    }
    // urllib's timeout: the connect, and each wait for data. A hard cap of three times that ends a trickle too
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeoutMs);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, std::max(1L, (timeoutMs + 999) / 1000));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3 * timeoutMs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(cancel_));
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) {
        // Ended by us (quitting): not "off", so a sync doesn't go on to wake the PC
        if (cancel_ != nullptr && cancel_->load()) throw AgentError(AgentError::Kind::Cancelled, 0, {}, "cancelled");
        // URLError / OSError / a timeout: the PC counts as off
        throw AgentError(AgentError::Kind::Unreachable, 0, {}, curl_easy_strerror(code));
    }
    response.status = static_cast<int>(status);
    for (const int accepted : ok) {
        if (response.status == accepted) return response;
    }
    const JsonValue payload = objectOf(response.body);
    if (response.status == 401) {
        throw AgentError(AgentError::Kind::Auth, response.status, {}, stringField(payload, "message"));
    }
    throw AgentError(AgentError::Kind::Http, response.status, stringField(payload, "code"),
                     stringField(payload, "message"));
}

PcStatus HttpAgent::status() {
    return statusFrom(objectOf(request("GET", "/status", nullptr).body));
}

void HttpAgent::claim() {
    const std::string body = R"({"source": "frame"})";
    request("POST", "/claim", &body);
}

SwitchReply HttpAgent::switchTo(const std::string& steamId, bool force) {
    const std::string body =
        "{\"steamId\": " + jsonString(steamId) + ", \"force\": " + (force ? "true" : "false") + "}";
    const Response r = request("POST", "/steam/switch", &body, {200, 202, 409});
    const JsonValue payload = objectOf(r.body);
    SwitchReply reply;
    reply.status = r.status;
    reply.code = stringField(payload, "code");
    reply.message = stringField(payload, "message");
    reply.requestId = textOf(payload.get("requestId"));
    // flow.py reads body['requestId'] for anything but 200 / 409: missing, that's its KeyError
    if (reply.status == 202 && payload.get("requestId") == nullptr) throw std::runtime_error("'requestId'");
    return reply;
}

SwitchResult HttpAgent::switchResult(const std::string& requestId) {
    const JsonValue payload = objectOf(request("GET", "/steam/switch/" + requestId, nullptr).body);
    SwitchResult result;
    result.done = truthy(payload.get("done"));
    result.ok = truthy(payload.get("ok"));
    result.message = stringField(payload, "message");
    return result;
}

std::vector<PcAccount> HttpAgent::accounts(JsonValue* raw) {
    const JsonValue payload = objectOf(request("GET", "/steam/accounts", nullptr).body);
    const JsonValue* list = payload.get("accounts");
    if (list == nullptr) throw std::runtime_error("'accounts'");  // app.py's KeyError
    if (raw != nullptr) *raw = *list;
    return pcAccountsFrom(*list);
}

std::optional<std::string> HttpAgent::avatar(const std::string& steamId) {
    try {
        return request("GET", "/steam/avatar/" + steamId, nullptr).body;
    } catch (const AgentError& error) {
        if (error.status == 400 || error.status == 403 || error.status == 404) return std::nullopt;
        throw;
    }
}

void HttpAgent::shutdown() {
    const std::string body = R"({"manual": true})";
    request("POST", "/shutdown", &body);
}

}  // namespace framesync
