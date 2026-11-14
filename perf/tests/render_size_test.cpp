// headset/render_size tests: the PC helper's JSON, vrlink.txt's SetPeer lines, the layer's file and its staleness
// rule, and the standalone read on a fake /proc and runtime directory (no stream there, so no network). The stream's
// read (2026-11-15) runs against a fake helper on 127.0.0.1: /v2/state, an old helper's 404, no helper.
#include "metric_registry.h"
#include "render_size.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using namespace render_size;

void testHelperReply() {
    int w = 0, h = 0;
    CHECK(parseHelperReply(R"({"v":1,"app":"steam.app.1172380","width":3048,"height":3048})", w, h) ==
          HelperReply::Game);
    CHECK(w == 3048 && h == 3048);
    CHECK(parseHelperReply(R"({"v":1,"app":null})", w, h) == HelperReply::NoGame);
    // A game without a size, a newer version, garbage: not trusted
    CHECK(parseHelperReply(R"({"v":1,"app":"x"})", w, h) == HelperReply::Invalid);
    CHECK(parseHelperReply(R"({"v":2,"app":"x","width":100,"height":100})", w, h) == HelperReply::Invalid);
    CHECK(parseHelperReply(R"({"app":null})", w, h) == HelperReply::Invalid);
    CHECK(parseHelperReply(R"({"v":1,"app":"x","width":0,"height":100})", w, h) == HelperReply::Invalid);
    CHECK(parseHelperReply(R"({"v":1,"app":"x","width":100.5,"height":100})", w, h) == HelperReply::Invalid);
    CHECK(parseHelperReply(R"({"v":1,"app":"x","width":99999,"height":100})", w, h) == HelperReply::Invalid);
    CHECK(parseHelperReply(R"({"v":1,"app":"x","width":"100","height":100})", w, h) == HelperReply::Invalid);
    CHECK(parseHelperReply(R"({"v":1,"app":7,"width":100,"height":100})", w, h) == HelperReply::Invalid);
    CHECK(parseHelperReply("not found", w, h) == HelperReply::Invalid);
    CHECK(parseHelperReply("", w, h) == HelperReply::Invalid);
    CHECK(parseHelperReply("[1]", w, h) == HelperReply::Invalid);
    CHECK(w == 3048 && h == 3048);  // untouched by the failures
}

// The helper's own example bodies (pc-helper/framedock-pc.cs's self test)
const char* const kStateFull =
    R"({"v":2,"render":{"v":1,"app":"steam.app.1172380","width":3048,"height":3048},"vr":true,"hz":90.0,"fps":45.0,)"
    R"("missedPct":50.0,"gpuMs":6.79,"gpuMaxMs":9.10,"cpuMs":8.00,"cpuMaxMs":21.70,"dropped":3,"ageMs":120,)"
    R"("pcCpuPct":12.3,"pcGpuPct":56,"pcGpuTempC":61,"pcVramUsedMb":4096,"pcVramTotalMb":16384,"linkMbit":350.0})";
const char* const kStateNulls =
    R"({"v":2,"render":{"v":1,"app":null},"vr":false,"hz":null,"fps":null,"missedPct":null,"gpuMs":null,)"
    R"("gpuMaxMs":null,"cpuMs":null,"cpuMaxMs":null,"dropped":null,"ageMs":null,"pcCpuPct":null,"pcGpuPct":null,)"
    R"("pcGpuTempC":null,"pcVramUsedMb":null,"pcVramTotalMb":null,"linkMbit":null})";

bool near(double a, double b) {
    return std::fabs(a - b) < 1e-9;
}

void testHelperState() {
    int w = 0, h = 0;
    PcState pc;
    CHECK(parseHelperState(kStateFull, w, h, pc) == HelperReply::Game);
    CHECK(w == 3048 && h == 3048 && pc.vr);
    CHECK(near(pc.hz, 90) && near(pc.fps, 45) && near(pc.missedPct, 50) && near(pc.gpuMs, 6.79) &&
          near(pc.gpuMaxMs, 9.1) && near(pc.cpuMs, 8) && near(pc.cpuMaxMs, 21.7));
    CHECK(near(pc.cpuPct, 12.3) && near(pc.gpuPct, 56) && near(pc.gpuTempC, 61) && near(pc.vramUsedMb, 4096) &&
          near(pc.vramTotalMb, 16384) && near(pc.linkMbit, 350));

    // Every value null (the first answer after a pause): still our helper, nothing known
    w = h = 7;
    CHECK(parseHelperState(kStateNulls, w, h, pc) == HelperReply::NoGame);
    CHECK(w == 7 && h == 7 && !pc.vr);
    for (const double v : {pc.hz, pc.fps, pc.missedPct, pc.gpuMs, pc.gpuMaxMs, pc.cpuMs, pc.cpuMaxMs, pc.cpuPct,
                           pc.gpuPct, pc.gpuTempC, pc.vramUsedMb, pc.vramTotalMb, pc.linkMbit}) {
        CHECK(std::isnan(v));
    }
    // The headset idle: 0 fps, no times, the PC's own values still there
    CHECK(parseHelperState(R"({"v":2,"render":{"v":1,"app":null},"vr":true,"hz":90.0,"fps":0.0,"missedPct":null,)"
                           R"("gpuMs":null,"ageMs":40,"pcGpuPct":3})", w, h, pc) == HelperReply::NoGame);
    CHECK(pc.vr && near(pc.fps, 0) && std::isnan(pc.missedPct) && std::isnan(pc.gpuMs) && near(pc.gpuPct, 3));
    CHECK(std::isnan(pc.cpuPct));  // missing, like null

    // Wrong types and values out of range: that value alone is unknown
    CHECK(parseHelperState(R"({"v":2,"render":{"v":1,"app":"x","width":100,"height":200},"vr":true,"hz":"90",)"
                           R"("fps":-1,"missedPct":101,"gpuMs":"6.8","gpuMaxMs":1e9,"cpuMs":true,"cpuMaxMs":[1],)"
                           R"("ageMs":10,"pcCpuPct":250,"pcGpuPct":{"a":1},"pcGpuTempC":-300,"pcVramUsedMb":-1,)"
                           R"("pcVramTotalMb":0,"linkMbit":1e12})", w, h, pc) == HelperReply::Game);
    CHECK(w == 100 && h == 200 && pc.vr);
    for (const double v : {pc.hz, pc.fps, pc.missedPct, pc.gpuMs, pc.gpuMaxMs, pc.cpuMs, pc.cpuMaxMs, pc.cpuPct,
                           pc.gpuPct, pc.gpuTempC, pc.vramUsedMb, pc.vramTotalMb, pc.linkMbit}) {
        CHECK(std::isnan(v));
    }
    // "vr" not a bool, or false: no frame values, whatever is there
    CHECK(parseHelperState(R"({"v":2,"render":{"v":1,"app":null},"vr":1,"fps":60,"gpuMs":5,"ageMs":10,"pcCpuPct":9})",
                           w, h, pc) == HelperReply::NoGame);
    CHECK(!pc.vr && std::isnan(pc.fps) && std::isnan(pc.gpuMs) && near(pc.cpuPct, 9));
    // Frame values read too long ago, or of unknown age: dropped
    CHECK(parseHelperState(R"({"v":2,"render":{"v":1,"app":null},"vr":true,"fps":60,"gpuMs":5,"ageMs":3001})", w, h,
                           pc) == HelperReply::NoGame);
    CHECK(std::isnan(pc.fps) && std::isnan(pc.gpuMs));
    CHECK(parseHelperState(R"({"v":2,"render":{"v":1,"app":null},"vr":true,"fps":60,"gpuMs":5,"ageMs":3000})", w, h,
                           pc) == HelperReply::NoGame);
    CHECK(near(pc.fps, 60) && near(pc.gpuMs, 5));
    CHECK(parseHelperState(R"({"v":2,"render":{"v":1,"app":null},"vr":true,"fps":60,"gpuMs":5})", w, h, pc) ==
          HelperReply::NoGame);
    CHECK(std::isnan(pc.fps));

    // Not a /v2/state body: a v1 body, another version, a bad "render", garbage. pc and the size stay as they were
    pc = PcState();
    pc.fps = 77;
    w = h = 5;
    CHECK(parseHelperState(R"({"v":1,"app":"steam.app.1172380","width":3048,"height":3048})", w, h, pc) ==
          HelperReply::Invalid);
    CHECK(parseHelperState(R"({"v":3,"render":{"v":1,"app":null},"vr":true})", w, h, pc) == HelperReply::Invalid);
    CHECK(parseHelperState(R"({"v":"2","render":{"v":1,"app":null}})", w, h, pc) == HelperReply::Invalid);
    CHECK(parseHelperState(R"({"v":2,"vr":true,"fps":60})", w, h, pc) == HelperReply::Invalid);
    CHECK(parseHelperState(R"({"v":2,"render":null})", w, h, pc) == HelperReply::Invalid);
    CHECK(parseHelperState(R"({"v":2,"render":{"v":2,"app":null}})", w, h, pc) == HelperReply::Invalid);
    CHECK(parseHelperState(R"({"v":2,"render":{"v":1,"app":"x","width":99999,"height":100}})", w, h, pc) ==
          HelperReply::Invalid);
    CHECK(parseHelperState("not found", w, h, pc) == HelperReply::Invalid);
    CHECK(parseHelperState("", w, h, pc) == HelperReply::Invalid);
    CHECK(parseHelperState("[2]", w, h, pc) == HelperReply::Invalid);
    CHECK(parseHelperState(R"({"v":2,"render":{"v":1,"app":null},"vr":true)", w, h, pc) == HelperReply::Invalid);
    CHECK(w == 5 && h == 5 && near(pc.fps, 77));
    // ...and /render's parser doesn't take a /v2/state body
    CHECK(parseHelperReply(kStateFull, w, h) == HelperReply::Invalid);
}

void testHelperVersion() {
    HelperVersion v;
    // Unknown peers are asked /v2/state
    CHECK(v.askState("10.0.0.2", 100.0));
    // 200: the new helper; no answer or another error: nothing learnt (not "/render instead")
    CHECK(!v.stateAnswered("10.0.0.2", 200, 100.0) && v.askState("10.0.0.2", 101.0));
    CHECK(!v.stateAnswered("10.0.0.2", 0, 101.0) && v.askState("10.0.0.2", 102.0));
    CHECK(!v.stateAnswered("10.0.0.2", 500, 102.0) && v.askState("10.0.0.2", 103.0));
    // 404: an old helper. /render for kStateProbeSec, then /v2/state once more
    CHECK(v.stateAnswered("10.0.0.2", 404, 103.0));
    CHECK(!v.askState("10.0.0.2", 103.0) && !v.askState("10.0.0.2", 104.0) && !v.askState("10.0.0.2", 162.9));
    CHECK(v.askState("10.0.0.2", 163.0));
    CHECK(v.stateAnswered("10.0.0.2", 404, 163.0));  // still old: another minute
    CHECK(!v.askState("10.0.0.2", 200.0) && v.askState("10.0.0.2", 223.0));
    // Another PC isn't taken for the old one
    CHECK(v.askState("10.0.0.3", 164.0));
    // Updated meanwhile: /v2/state from then on
    CHECK(!v.stateAnswered("10.0.0.2", 200, 223.0));
    CHECK(v.askState("10.0.0.2", 224.0));
}

void testPingMedian() {
    PingMedian p;
    CHECK(std::isnan(p.median()));
    p.add(4.0);
    CHECK(near(p.median(), 4.0));
    p.add(2.0);
    CHECK(near(p.median(), 3.0));  // two: their mean
    p.add(std::nan(""));           // libcurl gave no time: skipped
    CHECK(near(p.median(), 3.0));
    p.add(90.0);  // one slow connect doesn't show
    CHECK(near(p.median(), 4.0));
    p.add(3.0);
    p.add(5.0);
    CHECK(near(p.median(), 4.0));  // 4 2 90 3 5
    p.add(6.0);                    // the oldest (4) leaves: 2 90 3 5 6
    CHECK(near(p.median(), 5.0));
    p.add(7.0);
    p.add(8.0);
    p.add(9.0);  // 5 6 7 8 9: the 90 is gone
    CHECK(near(p.median(), 7.0));
    p.clear();
    CHECK(std::isnan(p.median()));
}

void testVrlinkPeers() {
    const std::string log =
        "Thu Nov 12 2026 20:01:00.000 - SVLDataLinkTransferUDP::SetPeer 192.168.50.48 -> 192.168.50.237 (port 1)\n"
        "Thu Nov 12 2026 20:02:00.000 - something else -> 1.2.3.4\n"
        "Thu Nov 12 2026 20:03:00.000 - SVLDataLinkTransferUDP::SetPeer 10.35.78.1 -> 10.35.78.22 (dongle)\n"
        "Thu Nov 12 2026 20:04:00.000 - SVLDataLinkTransferUDP::SetPeer 192.168.50.48 -> 192.168.50.237 (port 2)\n"
        "Thu Nov 12 2026 20:05:00.000 - SVLDataLinkTransferUDP::SetPeer 192.168.50.48 -> 999.1.1.1 (bad)\n"
        "Thu Nov 12 2026 20:06:00.000 - SVLDataLinkTransferUDP::SetPeer 192.168.50.48 -> (none)\n"
        "SVLDataLinkTransferUDP::SetPeer 1.1.1.1 -> 10.0.0";  // cut off (the tail of a log being written)
    const std::vector<std::string> peers = vrlinkPeers(log);
    CHECK((peers == std::vector<std::string> {"192.168.50.237", "10.35.78.22"}));
    CHECK(vrlinkPeers("").empty());
    CHECK(vrlinkPeers("\n\n").empty());
    CHECK((vrlinkPeers("SVLDataLinkTransferUDP::SetPeer a -> 192.168.0.9") ==
           std::vector<std::string> {"192.168.0.9"}));
}

void testLayerFile() {
    int w = 0, h = 0;
    int64_t t = 0;
    CHECK(parseLayerFile("2400 2400 1763038000\n", w, h, t) && w == 2400 && h == 2400 && t == 1763038000);
    CHECK(!parseLayerFile("", w, h, t));
    CHECK(!parseLayerFile("2400 2400\n", w, h, t));
    CHECK(!parseLayerFile("0 2400 1763038000\n", w, h, t));
    CHECK(!parseLayerFile("abc", w, h, t));
}

void testStaleness() {
    // stat: pid (comm with ") (" inside) state, then fields 4.. ; field 22 (start time) = 12345 ticks
    std::string stat = "4242 (my game) (x)) S";
    for (int k = 4; k <= 21; ++k) stat += " " + std::to_string(k);
    stat += " 12345 23 24\n";
    // Started at uptime 123.45 s; now uptime 1123.45 s and wall clock 2000000 → started at 1999000
    const double start = processStartUnix(stat, 1123.45, 2000000.0, 100);
    CHECK(std::fabs(start - 1999000.0) < 1e-6);
    CHECK(std::isnan(processStartUnix("4242 (short) S 1 2", 1.0, 2.0, 100)));
    CHECK(std::isnan(processStartUnix("garbage", 1.0, 2.0, 100)));
    CHECK(std::isnan(processStartUnix(stat, std::nan(""), 2.0, 100)));
    // Written after the start (or within the slack: whole seconds, an estimated start): this game's
    CHECK(layerSizeIsCurrent(1999005, start));
    CHECK(layerSizeIsCurrent(1999000, start));
    CHECK(layerSizeIsCurrent(1998999, start));
    // Written well before: a previous game's
    CHECK(!layerSizeIsCurrent(1998990, start));
    CHECK(!layerSizeIsCurrent(1999005, std::nan("")));
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

void testStandaloneRead() {
    char base[] = "/tmp/render-size-test-XXXXXX";
    if (mkdtemp(base) == nullptr) {
        CHECK(!"mkdtemp");
        return;
    }
    const std::string root = base;
    const std::string proc = root + "/proc";
    const std::string runtime = root + "/run";
    mkdir(proc.c_str(), 0700);
    mkdir((proc + "/77").c_str(), 0700);
    mkdir(runtime.c_str(), 0700);
    mkdir((runtime + "/framecolor").c_str(), 0700);
    // The game started 100 s ago: uptime 1000 s, started 900 s after boot (in this host's clock ticks)
    const long tck = sysconf(_SC_CLK_TCK);
    std::string stat = "77 (game) S";
    for (int k = 4; k <= 21; ++k) stat += " 0";
    stat += " " + std::to_string(900L * tck) + " 0\n";
    writeFile(proc + "/77/stat", stat);
    writeFile(proc + "/uptime", "1000.00 4000.00\n");
    const long long nowUnix = static_cast<long long>(std::time(nullptr));
    RenderSizeReader reader(root + "/home", runtime, proc);

    // No file yet; no scene app
    CHECK(reader.read(1.0, 77).source == 0);
    writeFile(runtime + "/framecolor/render-size", "2400 2400 " + std::to_string(nowUnix - 50) + "\n");
    CHECK(reader.read(2.0, 0).source == 0);
    const Reading r = reader.read(3.0, 77);
    CHECK(r.source == static_cast<uint32_t>(frame_perf_overlay::RenderSource::Headset) && r.width == 2400 &&
          r.height == 2400);
    // Written before this game started (a previous game's): N/A
    writeFile(runtime + "/framecolor/render-size", "1728 1728 " + std::to_string(nowUnix - 200) + "\n");
    CHECK(reader.read(4.0, 77).source == 0);
    // The scene process gone
    CHECK(reader.read(5.0, 78).source == 0);

    if (std::system(("rm -rf '" + root + "'").c_str()) != 0) std::printf("could not remove %s\n", base);
}

/** A PC helper on 127.0.0.1 (an ephemeral port): the new one, an old one (404 to /v2/state), or one that's broken. */
class FakeHelper {
public:
    enum class Mode { New, Old, Garbage };

    FakeHelper() {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(addr);
        if (fd_ < 0 || bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(fd_, 8) != 0 ||
            getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return;
        }
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeHelper() { stop(); }

    /** Stops answering: the port refuses connections from then on. */
    void stop() {
        if (!thread_.joinable()) return;
        stopping_ = true;
        shutdown(fd_, SHUT_RDWR);  // wakes accept()
        thread_.join();
        close(fd_);
    }
    int port() const { return port_; }
    void setMode(Mode mode) { mode_ = mode; }
    /** The paths asked so far, in order ("/v2/state", "/render"), and forgets them. */
    std::vector<std::string> takeAsked() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        out.swap(asked_);
        return out;
    }

private:
    int fd_ = -1;
    int port_ = 0;
    std::thread thread_;
    std::atomic<bool> stopping_ {false};
    std::atomic<Mode> mode_ {Mode::New};
    std::mutex mutex_;
    std::vector<std::string> asked_;

    void serve() {
        while (!stopping_) {
            const int client = accept(fd_, nullptr, nullptr);
            if (client < 0) break;
            std::string request;
            char buffer[512];
            while (request.find("\r\n\r\n") == std::string::npos) {
                const ssize_t n = read(client, buffer, sizeof(buffer));
                if (n <= 0) break;
                request.append(buffer, static_cast<size_t>(n));
            }
            const size_t a = request.find(' ');
            const size_t b = a == std::string::npos ? a : request.find(' ', a + 1);
            const std::string path = b == std::string::npos ? "" : request.substr(a + 1, b - a - 1);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                asked_.push_back(path);
            }
            const Mode mode = mode_;
            std::string status = "200 OK";
            std::string body;
            if (mode == Mode::Garbage) {
                body = "<html>a router's page</html>";
            } else if (path == "/render") {
                body = R"({"v":1,"app":"steam.app.1172380","width":3048,"height":3048})";
            } else if (path == "/v2/state" && mode == Mode::New) {
                body = kStateFull;
            } else {
                status = "404 Not Found";
                body = "not found";
            }
            const std::string answer = "HTTP/1.0 " + status + "\r\nContent-Length: " + std::to_string(body.size()) +
                                       "\r\nConnection: close\r\n\r\n" + body;
            if (write(client, answer.data(), answer.size()) < 0) std::printf("fake helper: write failed\n");
            close(client);
        }
    }
};

// The stream's read end to end: nothing asked while not streaming; /v2/state; an old helper's 404 and /render, with
// /v2/state asked again only a minute later; a helper that goes away (and the 15 s before the next try)
void testStreamRead() {
    using frame_perf_overlay::RenderSource;
    using frame_perf_overlay::StreamState;
    using Asked = std::vector<std::string>;
    char base[] = "/tmp/render-size-test-XXXXXX";
    if (mkdtemp(base) == nullptr) {
        CHECK(!"mkdtemp");
        return;
    }
    const std::string root = base;
    const std::string proc = root + "/proc";
    const std::string logs = root + "/home/.local/share/Steam/logs";
    CHECK(std::system(("mkdir -p '" + proc + "/4321' '" + logs + "' '" + root + "/run'").c_str()) == 0);
    writeFile(logs + "/vrlink.txt",
              "Sun Nov 15 2026 03:00:00.000 - SVLDataLinkTransferUDP::SetPeer 127.0.0.1 -> 127.0.0.1 (port 1)\n");
    FakeHelper helper;
    CHECK(helper.port() > 0);
    RenderSizeReader reader(root + "/home", root + "/run", proc, helper.port());

    // Not streaming (no vrlink process): the standalone read, and not one request
    Reading r = reader.read(1.0, 0);
    CHECK(r.stream == static_cast<uint32_t>(StreamState::None) && r.source == 0 && std::isnan(r.pingMs));
    CHECK(helper.takeAsked().empty());

    // vrlink runs (StreamingProbe looks again 5 s after its last scan). Seen once: not yet (one odd look doesn't flip
    // the cards); seen twice: the new helper's values
    writeFile(proc + "/4321/cmdline", std::string("/home/steam/vrlink\0--arg\0", 25));
    r = reader.read(9.0, 0);
    CHECK(r.stream == static_cast<uint32_t>(StreamState::None));
    CHECK(helper.takeAsked().empty());
    r = reader.read(10.0, 0);
    CHECK(r.stream == static_cast<uint32_t>(StreamState::Helper));
    CHECK(r.source == static_cast<uint32_t>(RenderSource::Pc) && r.width == 3048 && r.height == 3048);
    CHECK(r.pc.vr && near(r.pc.fps, 45) && near(r.pc.gpuMs, 6.79) && near(r.pc.gpuPct, 56) && near(r.pc.linkMbit, 350));
    CHECK(r.pingMs > 0 && r.pingMs < RenderSizeReader::kTimeoutMs);  // the connect time, here to this machine
    CHECK((helper.takeAsked() == Asked {"/v2/state"}));
    r = reader.read(11.0, 0);
    CHECK(r.stream == static_cast<uint32_t>(StreamState::Helper) && r.pingMs > 0);
    CHECK((helper.takeAsked() == Asked {"/v2/state"}));  // one request a read

    // An old helper: 404 to /v2/state, then /render in the same read; the render size and the ping only
    helper.setMode(FakeHelper::Mode::Old);
    r = reader.read(12.0, 0);
    CHECK(r.stream == static_cast<uint32_t>(StreamState::OldHelper));
    CHECK(r.source == static_cast<uint32_t>(RenderSource::Pc) && r.width == 3048);
    CHECK(std::isnan(r.pc.fps) && std::isnan(r.pc.gpuPct) && r.pingMs > 0);
    CHECK((helper.takeAsked() == Asked {"/v2/state", "/render"}));
    // ...and for the next minute /render alone
    for (const double now : {13.0, 14.0, 40.0, 71.9}) {
        r = reader.read(now, 0);
        CHECK(r.stream == static_cast<uint32_t>(StreamState::OldHelper) && r.width == 3048);
        CHECK((helper.takeAsked() == Asked {"/render"}));
    }
    r = reader.read(72.0, 0);  // 60 s after the 404: asked again, still old
    CHECK(r.stream == static_cast<uint32_t>(StreamState::OldHelper));
    CHECK((helper.takeAsked() == Asked {"/v2/state", "/render"}));
    r = reader.read(73.0, 0);
    CHECK((helper.takeAsked() == Asked {"/render"}));
    // The helper was updated: found at the next probe
    helper.setMode(FakeHelper::Mode::New);
    r = reader.read(100.0, 0);
    CHECK(r.stream == static_cast<uint32_t>(StreamState::OldHelper));
    CHECK((helper.takeAsked() == Asked {"/render"}));
    r = reader.read(132.0, 0);
    CHECK(r.stream == static_cast<uint32_t>(StreamState::Helper) && near(r.pc.fps, 45));
    CHECK((helper.takeAsked() == Asked {"/v2/state"}));

    // Something else on that port (200 with a page): /render is tried too (a later helper's format would still give
    // the size), and it isn't a helper. The last values stay for kHoldSec (a late answer isn't "no helper"), then
    // the address is left alone for kRetrySec
    helper.setMode(FakeHelper::Mode::Garbage);
    r = reader.read(133.0, 0);
    CHECK(r.stream == static_cast<uint32_t>(StreamState::Helper) && near(r.pc.fps, 45) && r.width == 3048);  // held
    CHECK((helper.takeAsked() == Asked {"/v2/state", "/render"}));
    r = reader.read(132.0 + RenderSizeReader::kHoldSec, 0);
    CHECK(r.stream == static_cast<uint32_t>(StreamState::Helper));  // still held
    helper.takeAsked();
    const double gaveUp = 132.5 + RenderSizeReader::kHoldSec;
    r = reader.read(gaveUp, 0);
    CHECK(r.stream == static_cast<uint32_t>(StreamState::NoHelper) && r.source == 0 && std::isnan(r.pingMs) &&
          std::isnan(r.pc.fps));
    CHECK((helper.takeAsked() == Asked {"/v2/state", "/render"}));
    helper.setMode(FakeHelper::Mode::New);
    r = reader.read(gaveUp + 5, 0);
    CHECK(r.stream == static_cast<uint32_t>(StreamState::NoHelper));
    CHECK(helper.takeAsked().empty());  // within the 15 s
    r = reader.read(gaveUp + RenderSizeReader::kRetrySec, 0);
    CHECK(r.stream == static_cast<uint32_t>(StreamState::Helper) && r.pingMs > 0);
    CHECK((helper.takeAsked() == Asked {"/v2/state"}));

    // The helper is gone (the port refuses): streaming without a helper
    helper.stop();
    r = reader.read(200.0, 0);
    CHECK(r.stream == static_cast<uint32_t>(StreamState::NoHelper) && r.source == 0 && std::isnan(r.pingMs));

    // The stream ends: standalone again, once that was seen twice
    CHECK(std::remove((proc + "/4321/cmdline").c_str()) == 0);
    r = reader.read(300.0, 0);
    CHECK(r.stream == static_cast<uint32_t>(StreamState::NoHelper));
    r = reader.read(301.0, 0);
    CHECK(r.stream == static_cast<uint32_t>(StreamState::None));

    if (std::system(("rm -rf '" + root + "'").c_str()) != 0) std::printf("could not remove %s\n", base);
}

}  // namespace

int main() {
    testHelperReply();
    testHelperState();
    testHelperVersion();
    testPingMedian();
    testStreamRead();
    testVrlinkPeers();
    testLayerFile();
    testStaleness();
    testStandaloneRead();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("render-size: all checks passed\n");
    return 0;
}
