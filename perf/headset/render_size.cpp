#include "render_size.h"

#include "json.h"
#include "metric_registry.h"

#include <curl/curl.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>

namespace render_size {

namespace {

constexpr int kMaxSide = 16384;
constexpr size_t kMaxBody = 4096;        // the helper's answer is under 100 bytes
constexpr size_t kLogTail = 256 * 1024;  // vrlink.txt grows over a session; its last SetPeer lines are near the end

bool validSide(double v) {
    return v >= 1 && v <= kMaxSide && v == std::floor(v);
}

/** a.b.c.d, each 0-255 */
bool isIpv4(const std::string& s) {
    int parts = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = i;
        int value = 0;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9' && j - i < 3) value = value * 10 + (s[j++] - '0');
        if (j == i || value > 255) return false;
        ++parts;
        if (j == s.size()) break;
        if (s[j] != '.') return false;
        i = j + 1;
        if (i == s.size()) return false;
    }
    return parts == 4;
}

std::string readTail(const std::string& path, size_t maxBytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0) return {};
    const std::streamoff start = size > static_cast<std::streamoff>(maxBytes) ? size - maxBytes : 0;
    in.seekg(start);
    std::string text(static_cast<size_t>(size - start), '\0');
    in.read(&text[0], static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(in.gcount()));
    return text;
}

std::string readSmall(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string text(512, '\0');
    in.read(&text[0], static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(in.gcount()));
    return text;
}

size_t collect(char* data, size_t size, size_t count, void* user) {
    std::string& out = *static_cast<std::string*>(user);
    if (out.size() + size * count > kMaxBody) return 0;  // not our helper
    out.append(data, size * count);
    return size * count;
}

/** @return true with the body of a 200 answer */
bool httpGet(const std::string& url, long timeoutMs, std::string& body) {
    CURL* curl = curl_easy_init();
    if (curl == nullptr) return false;
    body.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");  // the PC is on the LAN
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    return code == CURLE_OK && status == 200;
}

}  // namespace

HelperReply parseHelperReply(const std::string& body, int& width, int& height) {
    JsonValue root;
    std::string error;
    if (!parseJson(body, root, error) || !root.isObject()) return HelperReply::Invalid;
    const JsonValue* v = root.get("v");
    const JsonValue* app = root.get("app");
    if (v == nullptr || !v->isNumber() || v->number != 1 || app == nullptr) return HelperReply::Invalid;
    if (app->type == JsonValue::Type::Null) return HelperReply::NoGame;
    if (!app->isString()) return HelperReply::Invalid;
    const JsonValue* w = root.get("width");
    const JsonValue* h = root.get("height");
    if (w == nullptr || h == nullptr || !w->isNumber() || !h->isNumber() || !validSide(w->number) ||
        !validSide(h->number)) {
        return HelperReply::Invalid;
    }
    width = static_cast<int>(w->number);
    height = static_cast<int>(h->number);
    return HelperReply::Game;
}

std::vector<std::string> vrlinkPeers(const std::string& logText) {
    static const std::string kTag = "SVLDataLinkTransferUDP::SetPeer ";
    std::vector<std::string> peers;
    size_t end = logText.size();
    while (end > 0) {
        const size_t begin = logText.rfind('\n', end - 1);
        const size_t from = begin == std::string::npos ? 0 : begin + 1;
        const std::string line = logText.substr(from, end - from);
        end = begin == std::string::npos ? 0 : begin;
        const size_t tag = line.find(kTag);
        if (tag == std::string::npos) continue;
        const size_t arrow = line.find("->", tag + kTag.size());
        if (arrow == std::string::npos) continue;
        size_t i = arrow + 2;
        while (i < line.size() && line[i] == ' ') ++i;
        size_t j = i;
        while (j < line.size() && ((line[j] >= '0' && line[j] <= '9') || line[j] == '.')) ++j;
        const std::string peer = line.substr(i, j - i);
        if (!isIpv4(peer)) continue;
        bool seen = false;
        for (const std::string& p : peers) seen = seen || p == peer;
        if (!seen) peers.push_back(peer);
    }
    return peers;
}

bool parseLayerFile(const std::string& text, int& width, int& height, int64_t& writtenUnix) {
    std::istringstream in(text);
    long long w = 0, h = 0, t = 0;
    if (!(in >> w >> h >> t) || w < 1 || h < 1 || w > kMaxSide || h > kMaxSide || t <= 0) return false;
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    writtenUnix = t;
    return true;
}

double processStartUnix(const std::string& statText, double uptimeS, double nowUnix, long ticksPerSec) {
    // "pid (comm) state ...": comm may hold spaces and parentheses, so count from the last ')'. Field 3 (state) is
    // the first after it, so field 22 is the 20th
    const size_t close = statText.rfind(')');
    if (close == std::string::npos || ticksPerSec <= 0 || std::isnan(uptimeS)) return std::nan("");
    std::istringstream in(statText.substr(close + 1));
    std::string field;
    for (int k = 3; k <= 22; ++k) {
        if (!(in >> field)) return std::nan("");
    }
    char* endp = nullptr;
    const double ticks = std::strtod(field.c_str(), &endp);
    if (endp == field.c_str() || *endp != '\0' || ticks < 0) return std::nan("");
    return nowUnix - (uptimeS - ticks / static_cast<double>(ticksPerSec));
}

bool layerSizeIsCurrent(int64_t writtenUnix, double startUnix) {
    return !std::isnan(startUnix) && static_cast<double>(writtenUnix) >= startUnix - kStartSlackS;
}

RenderSizeReader::RenderSizeReader(std::string home, std::string runtimeDir, std::string procRoot)
    : vrlinkLog_(home + "/.local/share/Steam/logs/vrlink.txt"),
      layerFile_(runtimeDir + "/framecolor/render-size"),
      procRoot_(procRoot),
      streaming_(procRoot) {
    // libcurl's global setup, on the constructing (main) thread like Frame Sync's initHttp; it counts its callers
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

Reading RenderSizeReader::read(double now, int scenePid) {
    if (streaming_.streaming(now)) return readPc(now);
    return readLayer(scenePid);
}

Reading RenderSizeReader::readPc(double now) {
    Reading r;
    if (now < nextPcTry_) return r;
    const auto ask = [&](const std::string& peer) {
        std::string body;
        if (!httpGet("http://" + peer + ":" + std::to_string(kHelperPort) + "/render", kTimeoutMs, body)) return false;
        int w = 0, h = 0;
        const HelperReply reply = parseHelperReply(body, w, h);
        if (reply == HelperReply::Invalid) return false;
        peer_ = peer;
        if (reply == HelperReply::Game) {
            r.width = w;
            r.height = h;
            r.source = static_cast<uint32_t>(frame_perf_overlay::RenderSource::Pc);
        }
        return true;
    };
    if (!peer_.empty() && ask(peer_)) return r;
    const std::string last = peer_;
    peer_.clear();
    int tried = 0;
    for (const std::string& peer : vrlinkPeers(readTail(vrlinkLog_, kLogTail))) {
        if (peer == last) continue;
        if (++tried > kMaxPeers) break;
        if (ask(peer)) return r;
    }
    nextPcTry_ = now + kRetrySec;  // no helper there (not installed, or the firewall drops)
    return r;
}

Reading RenderSizeReader::readLayer(int scenePid) const {
    Reading r;
    if (scenePid <= 0) return r;
    int w = 0, h = 0;
    int64_t written = 0;
    if (!parseLayerFile(readSmall(layerFile_), w, h, written)) return r;
    double uptime = std::nan("");
    std::istringstream(readSmall(procRoot_ + "/uptime")) >> uptime;
    const double start = processStartUnix(readSmall(procRoot_ + "/" + std::to_string(scenePid) + "/stat"), uptime,
                                          static_cast<double>(std::time(nullptr)), sysconf(_SC_CLK_TCK));
    if (!layerSizeIsCurrent(written, start)) return r;  // a previous game's
    r.width = w;
    r.height = h;
    r.source = static_cast<uint32_t>(frame_perf_overlay::RenderSource::Headset);
    return r;
}

}  // namespace render_size
