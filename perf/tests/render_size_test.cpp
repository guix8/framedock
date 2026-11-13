// headset/render_size tests: the PC helper's JSON, vrlink.txt's SetPeer lines, the layer's file and its staleness
// rule, and the standalone read on a fake /proc and runtime directory (no stream there, so no network).
#include "metric_registry.h"
#include "render_size.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>
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

}  // namespace

int main() {
    testHelperReply();
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
