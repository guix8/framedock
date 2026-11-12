// headset/sensor_math tests: parsing and derivations for the system sensors, fed from fixture files.
// Usage: sensor-math-test <fixture dir>   (CTest passes perf/tests/fixtures/sensors)
// Ported from upstream PR #5 (066a5c8): the CPU and General parts (Frame Dock has no power tree / battery details).
#include "sensor_math.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
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

using namespace sensor_math;

std::string fixtureDir;

std::string readFixture(const char* name) {
    std::ifstream in(fixtureDir + "/" + name, std::ios::binary);
    if (!in) {
        std::printf("FAIL cannot open fixture %s/%s\n", fixtureDir.c_str(), name);
        ++failures;
        return "";
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool near(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

bool isNan(double v) {
    return std::isnan(v);
}

CpuTicks parsed(const char* fixture) {
    CpuTicks t;
    CHECK(parseProcStat(readFixture(fixture).c_str(), t));
    return t;
}

void testProcStat() {
    const CpuTicks a = parsed("proc_stat_a.txt");
    CHECK(a.total.size() == 3);  // total + cpu0 + cpu1; intr/ctxt lines ignored
    CHECK(a.present[0] && a.present[1] && a.present[2]);
    // busy = user+nice+system+irq+softirq+steal; total = busy + idle + iowait
    CHECK(a.busy[0] == 367360u + 15068u + 256399u + 89570u + 79807u);
    CHECK(a.total[0] == a.busy[0] + 6245980u + 687u);
    CHECK(a.busy[1] == 46199u + 1714u + 33845u + 30759u + 5426u);
    CHECK(a.total[1] == a.busy[1] + 761685u + 19u);

    const CpuTicks b = parsed("proc_stat_b.txt");
    const std::vector<double> pct = cpuPercents(a, b);
    CHECK(pct.size() == 3);
    CHECK(near(pct[0], 25.0));  // +100 busy of +400
    CHECK(near(pct[1], 50.0));  // +50 of +100
    CHECK(near(pct[2], 0.0));   // idle only

    // Offline core: cpu1 has no line, so cpu2 must still land on index 3
    const CpuTicks oa = parsed("proc_stat_offline_a.txt");
    CHECK(oa.total.size() == 4);
    CHECK(oa.present[0] && oa.present[1] && !oa.present[2] && oa.present[3]);
    const std::vector<double> op = cpuPercents(oa, parsed("proc_stat_offline_b.txt"));
    CHECK(op.size() == 4);
    CHECK(near(op[0], 40.0));
    CHECK(near(op[1], 50.0));
    CHECK(isNan(op[2]));
    CHECK(near(op[3], 50.0));  // +100 busy of +200

    // Counter going backwards (b -> a) is NaN, not a negative or huge value
    const std::vector<double> back = cpuPercents(b, a);
    CHECK(back.size() == 3);
    for (double v : back) CHECK(isNan(v));

    // No time passed
    const std::vector<double> same = cpuPercents(a, a);
    for (double v : same) CHECK(isNan(v));

    // First read (empty prev) and size mismatch (core appeared / prev shorter)
    const std::vector<double> first = cpuPercents(CpuTicks(), a);
    CHECK(first.size() == 3);
    for (double v : first) CHECK(isNan(v));
    CpuTicks shortPrev;
    CHECK(parseProcStat("cpu  100 0 100 800 0 0 0 0 0 0\n", shortPrev));
    CpuTicks longCur = a;
    longCur.total[0] += 1000;
    longCur.busy[0] += 500;
    const std::vector<double> mism = cpuPercents(shortPrev, longCur);
    CHECK(mism.size() == 3);
    CHECK(!isNan(mism[0]));
    CHECK(isNan(mism[1]));
    CHECK(isNan(mism[2]));

    // Garbage
    CpuTicks g;
    CHECK(!parseProcStat("", g));
    CHECK(!parseProcStat(nullptr, g));
    CHECK(!parseProcStat("intr 1 2 3\n", g));
    CHECK(!parseProcStat("cpu0 1 2 3 4 5\n", g));  // a core line without the total line
}

void testGeneral() {
    double up = -1;
    CHECK(parseUptime(readFixture("uptime.txt").c_str(), up));
    CHECK(near(up, 8914.95));
    CHECK(!parseUptime("", up));
    CHECK(!parseUptime("abc", up));
    CHECK(!parseUptime(nullptr, up));
    CHECK(near(up, 8914.95));  // failures leave the result alone

    long long pages = -1;
    CHECK(parseStatmResident(readFixture("statm.txt").c_str(), pages));
    CHECK(pages == 292);
    CHECK(!parseStatmResident("", pages));
    CHECK(!parseStatmResident("617", pages));
    CHECK(!parseStatmResident("x y", pages));
    CHECK(!parseStatmResident(nullptr, pages));
    CHECK(pages == 292);

    CHECK(near(selfWatts(0.5, 2.0, 4.0), 1.0));
    CHECK(near(selfWatts(5.0, 2.0, 4.0), 4.0));  // share capped at 1
    CHECK(near(selfWatts(0.0, 2.0, 4.0), 0.0));
    CHECK(isNan(selfWatts(0.5, 0.0, 4.0)));  // idle system
    CHECK(isNan(selfWatts(kNaN, 2.0, 4.0)));
    CHECK(isNan(selfWatts(0.5, kNaN, 4.0)));
    CHECK(isNan(selfWatts(0.5, 2.0, kNaN)));
    CHECK(isNan(selfWatts(-0.1, 2.0, 4.0)));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <fixture dir>\n", argv[0]);
        return 2;
    }
    fixtureDir = argv[1];
    testProcStat();
    testGeneral();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("sensor-math: all checks passed\n");
    return 0;
}
