#include "version_notice.h"

#include "atomic_file.h"
#include "json.h"

#include <curl/curl.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <utility>

namespace version_notice {

namespace {

constexpr size_t kMaxBody = 256 * 1024;  // the release's JSON (its notes, the assets) is a few KB

/** Digits only, at most 9 (no overflow) */
bool parseNumber(const std::string& text, size_t& i, long& value) {
    const size_t start = i;
    value = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9' && i - start < 9) value = value * 10 + (text[i++] - '0');
    return i > start && (i == text.size() || text[i] < '0' || text[i] > '9');
}

struct Fetch {
    std::string body;
    const std::atomic<bool>* cancel;
};

size_t collect(char* data, size_t size, size_t count, void* user) {
    std::string& out = static_cast<Fetch*>(user)->body;
    if (out.size() + size * count > kMaxBody) return 0;
    out.append(data, size * count);
    return size * count;
}

int progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<Fetch*>(user)->cancel->load() ? 1 : 0;  // non-zero aborts the transfer
}

/** @return the latest tag, "" with error set */
std::string fetchLatestTag(const std::string& userAgent, const std::atomic<bool>& cancel, std::string& error) {
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        error = "curl_easy_init failed";
        return "";
    }
    Fetch fetch {std::string(), &cancel};
    curl_easy_setopt(curl, CURLOPT_URL, kLatestUrl);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // GitHub wants a User-Agent; this one says the app and its version, nothing else
    curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fetch);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &fetch);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) {
        error = curl_easy_strerror(code);
        return "";
    }
    if (status != 200) {  // 403 / 429: the rate limit; 404: no release yet
        error = "HTTP " + std::to_string(status);
        return "";
    }
    const std::string tag = parseLatestTag(fetch.body);
    if (tag.empty()) error = "no tag_name in the answer";
    return tag;
}

}  // namespace

bool parseVersion(const std::string& text, Version& version) {
    size_t i = !text.empty() && (text[0] == 'v' || text[0] == 'V') ? 1 : 0;
    Version v;
    if (!parseNumber(text, i, v.major) || i >= text.size() || text[i++] != '.') return false;
    if (!parseNumber(text, i, v.minor) || i >= text.size() || text[i++] != '.') return false;
    if (!parseNumber(text, i, v.patch) || i != text.size()) return false;
    version = v;
    return true;
}

int compareVersions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

bool isNewer(const std::string& tag, const std::string& running) {
    Version latest, current;
    return parseVersion(tag, latest) && parseVersion(running, current) && compareVersions(latest, current) > 0;
}

std::string parseLatestTag(const std::string& body) {
    JsonValue root;
    std::string error;
    if (!parseJson(body, root, error) || !root.isObject()) return "";
    const JsonValue* tag = root.get("tag_name");
    return tag != nullptr && tag->isString() ? tag->text : "";
}

bool checkDue(int64_t nowUnix, int64_t lastUnix) {
    return lastUnix <= 0 || nowUnix < lastUnix || nowUnix - lastUnix >= kIntervalSec;
}

bool parseState(const std::string& text, State& state) {
    JsonValue root;
    std::string error;
    if (!parseJson(text, root, error) || !root.isObject()) return false;
    const JsonValue* checked = root.get("checked");
    const JsonValue* latest = root.get("latest");
    if (checked == nullptr || !checked->isNumber() || checked->number < 0) return false;
    State parsed;
    parsed.checkedUnix = static_cast<int64_t>(checked->number);
    if (latest != nullptr && latest->isString()) parsed.latestTag = latest->text;
    state = parsed;
    return true;
}

std::string stateToJson(const State& state) {
    return "{\n  \"checked\": " + std::to_string(state.checkedUnix) + ",\n  \"latest\": " + jsonString(state.latestTag) +
           "\n}\n";
}

std::string statePath(const std::string& configPath) {
    const size_t slash = configPath.find_last_of('/');
    return (slash == std::string::npos ? std::string(".") : configPath.substr(0, slash)) + "/version_check.json";
}

UpdateChecker::UpdateChecker(std::string statePath, std::string runningVersion)
    : path_(std::move(statePath)), running_(std::move(runningVersion)) {
    // libcurl's global setup on the main thread, like render_size's and Frame Sync's (it counts its callers)
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    std::ifstream in(path_, std::ios::binary);
    if (in) parseState(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()), state_);
}

UpdateChecker::~UpdateChecker() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

bool UpdateChecker::poll(bool tabVisible, bool enabled, int64_t nowUnix) {
    if (worker_.joinable() && !busy_.load()) {
        worker_.join();
        if (!fetchedTag_.empty()) {
            state_.latestTag = fetchedTag_;
            save();
        } else {
            std::fprintf(stderr, "[update] Couldn't check for a new version: %s\n", fetchError_.c_str());
        }
    }
    if (!worker_.joinable() && enabled && tabVisible && checkDue(nowUnix, state_.checkedUnix)) {
        state_.checkedUnix = nowUnix;  // counted when asked, so a failure isn't retried within the day either
        save();
        busy_ = true;
        worker_ = std::thread([this] {
            fetchedTag_ = fetchLatestTag(std::string("framedock/") + running_, cancel_, fetchError_);
            busy_ = false;
        });
    }
    const std::string newer = enabled && isNewer(state_.latestTag, running_) ? state_.latestTag : std::string();
    if (newer == newer_) return false;
    newer_ = newer;
    return true;
}

void UpdateChecker::save() {
    std::string error;
    if (!frame_perf_overlay::atomicWriteFile(path_, stateToJson(state_), error)) {
        std::fprintf(stderr, "[update] Can't write %s: %s\n", path_.c_str(), error.c_str());
    }
}

}  // namespace version_notice
