#include "render_size.h"

#include "json.h"
#include "metric_registry.h"

#include <curl/curl.h>
#include <unistd.h>

#include <algorithm>
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
constexpr size_t kMaxBody = 4096;        // the helper's answer: under 100 bytes (/render), about 400 (/v2/state)
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

/**
 * @param connectMs the TCP connect time (each request is a new connection), NaN if libcurl doesn't say
 * @return the HTTP status with the answer's body, 0 when there was no answer
 */
long httpGet(const std::string& url, long timeoutMs, std::string& body, double& connectMs) {
    connectMs = std::nan("");
    CURL* curl = curl_easy_init();
    if (curl == nullptr) return 0;
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
    curl_off_t connectUs = 0;
    if (curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME_T, &connectUs) == CURLE_OK && connectUs > 0) {
        connectMs = static_cast<double>(connectUs) / 1000.0;
    }
    curl_easy_cleanup(curl);
    return code == CURLE_OK ? status : 0;
}

/** The /render body, already parsed (also /v2/state's "render") */
HelperReply parseRender(const JsonValue& root, int& width, int& height) {
    if (!root.isObject()) return HelperReply::Invalid;
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

/** @return the member as a number within [min, max], else NaN (null, missing, another type, out of range) */
double numberIn(const JsonValue& root, const char* key, double min, double max) {
    const JsonValue* v = root.get(key);
    if (v == nullptr || !v->isNumber() || !(v->number >= min && v->number <= max)) return std::nan("");
    return v->number;
}

}  // namespace

HelperReply parseHelperReply(const std::string& body, int& width, int& height) {
    JsonValue root;
    std::string error;
    if (!parseJson(body, root, error)) return HelperReply::Invalid;
    return parseRender(root, width, height);
}

HelperReply parseHelperState(const std::string& body, int& width, int& height, PcState& pc) {
    JsonValue root;
    std::string error;
    if (!parseJson(body, root, error) || !root.isObject()) return HelperReply::Invalid;
    const JsonValue* v = root.get("v");
    const JsonValue* render = root.get("render");
    if (v == nullptr || !v->isNumber() || v->number != 2 || render == nullptr) return HelperReply::Invalid;
    const HelperReply reply = parseRender(*render, width, height);
    if (reply == HelperReply::Invalid) return reply;
    pc = PcState();
    const JsonValue* vr = root.get("vr");
    pc.vr = vr != nullptr && vr->isBool() && vr->boolean;
    pc.hz = numberIn(root, "hz", 1, 1000);
    // The frame values: only from a helper attached to SteamVR, and only if read lately (no "ageMs": not known, so no)
    const double ageMs = numberIn(root, "ageMs", 0, 1e9);
    if (pc.vr && ageMs <= kMaxFrameAgeMs) {
        pc.fps = numberIn(root, "fps", 0, 1000);
        pc.missedPct = numberIn(root, "missedPct", 0, 100);
        pc.gpuMs = numberIn(root, "gpuMs", 0, 10000);
        pc.gpuMaxMs = numberIn(root, "gpuMaxMs", 0, 10000);
        pc.cpuMs = numberIn(root, "cpuMs", 0, 10000);
        pc.cpuMaxMs = numberIn(root, "cpuMaxMs", 0, 10000);
    }
    pc.cpuPct = numberIn(root, "pcCpuPct", 0, 100);
    pc.gpuPct = numberIn(root, "pcGpuPct", 0, 100);
    pc.gpuTempC = numberIn(root, "pcGpuTempC", -50, 200);
    pc.vramUsedMb = numberIn(root, "pcVramUsedMb", 0, 1e7);
    pc.vramTotalMb = numberIn(root, "pcVramTotalMb", 1, 1e7);
    pc.linkMbit = numberIn(root, "linkMbit", 0, 1e5);
    return reply;
}

bool HelperVersion::stateAnswered(const std::string& peer, long status, double now) {
    if (status == 404) {
        oldPeer_ = peer;
        probeAt_ = now + kStateProbeSec;
        return true;
    }
    if (status == 200 && peer == oldPeer_) oldPeer_.clear();  // updated since
    return false;
}

void PingMedian::add(double ms) {
    if (std::isnan(ms)) return;
    if (samples_.size() >= kSamples) samples_.erase(samples_.begin());
    samples_.push_back(ms);
}

double PingMedian::median() const {
    if (samples_.empty()) return std::nan("");
    std::vector<double> sorted = samples_;
    std::sort(sorted.begin(), sorted.end());
    const size_t mid = sorted.size() / 2;
    return sorted.size() % 2 != 0 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2.0;
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

RenderSizeReader::RenderSizeReader(std::string home, std::string runtimeDir, std::string procRoot, int port)
    : vrlinkLog_(home + "/.local/share/Steam/logs/vrlink.txt"),
      layerFile_(runtimeDir + "/framecolor/render-size"),
      procRoot_(procRoot),
      port_(port),
      streaming_(procRoot) {
    // libcurl's global setup, on the constructing (main) thread like Frame Sync's initHttp; it counts its callers
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

Reading RenderSizeReader::read(double now, int scenePid) {
    // The cards change with this, so one odd look (vrlink restarting) doesn't flip them: the same answer twice
    const bool seen = streaming_.streaming(now);
    if (seen == lastSeen_) stable_ = seen;
    lastSeen_ = seen;
    if (stable_) return readPc(now);
    // Standalone: the next stream starts with nothing of the last one's (the address stays as a first guess)
    ping_.clear();
    haveGood_ = false;
    return readLayer(scenePid);
}

bool RenderSizeReader::askPeer(const std::string& peer, double now, Reading& r) {
    using frame_perf_overlay::StreamState;
    const std::string base = "http://" + peer + ":" + std::to_string(port_);
    std::string body;
    double connectMs = std::nan("");
    int w = 0, h = 0;
    HelperReply reply = HelperReply::Invalid;
    StreamState stream = StreamState::OldHelper;
    PcState pc;
    bool old = !version_.askState(peer, now);
    if (!old) {
        const long status = httpGet(base + "/v2/state", kTimeoutMs, body, connectMs);
        old = version_.stateAnswered(peer, status, now);
        if (!old) {
            if (status != 200) return false;
            reply = parseHelperState(body, w, h, pc);
            stream = StreamState::Helper;
            // 200 with something this build can't read (a later helper's format): the render size at least
            if (reply == HelperReply::Invalid) {
                old = true;
                stream = StreamState::OldHelper;
                pc = PcState();
            }
        }
    }
    if (old) {
        if (httpGet(base + "/render", kTimeoutMs, body, connectMs) != 200) return false;
        reply = parseHelperReply(body, w, h);
    }
    if (reply == HelperReply::Invalid) return false;
    if (peer != peer_) ping_.clear();  // another PC's
    peer_ = peer;
    ping_.add(connectMs);
    r.stream = static_cast<uint32_t>(stream);
    r.pc = pc;
    r.pingMs = ping_.median();
    if (reply == HelperReply::Game) {
        r.width = w;
        r.height = h;
        r.source = static_cast<uint32_t>(frame_perf_overlay::RenderSource::Pc);
    }
    return true;
}

Reading RenderSizeReader::readPc(double now) {
    Reading r;
    r.stream = static_cast<uint32_t>(frame_perf_overlay::StreamState::NoHelper);
    if (now < nextPcTry_) return r;
    if (!peer_.empty()) {
        if (askPeer(peer_, now, r)) {
            good_ = r;
            goodAt_ = now;
            haveGood_ = true;
            return r;
        }
        // One late answer on a Wi-Fi busy with the stream isn't "no helper": the last values stay for kHoldSec
        if (haveGood_ && now - goodAt_ <= kHoldSec) return good_;
    }
    haveGood_ = false;
    const std::string last = peer_;
    peer_.clear();
    ping_.clear();
    int tried = 0;
    for (const std::string& peer : vrlinkPeers(readTail(vrlinkLog_, kLogTail))) {
        if (peer == last) continue;
        if (++tried > kMaxPeers) break;
        if (askPeer(peer, now, r)) {
            good_ = r;
            goodAt_ = now;
            haveGood_ = true;
            return r;
        }
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
