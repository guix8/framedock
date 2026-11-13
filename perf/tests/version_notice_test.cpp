// headset/version_notice tests: the version comparison, GitHub's tag_name, the once-a-day rule and version_check.json.
// No network: the checker is only built on a state file and polled with the setting off or the tab hidden.
#include "version_notice.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using namespace version_notice;

void testVersions() {
    Version v;
    CHECK(parseVersion("v0.1.1", v) && v.major == 0 && v.minor == 1 && v.patch == 1);
    CHECK(parseVersion("1.20.3", v) && v.major == 1 && v.minor == 20 && v.patch == 3);
    CHECK(parseVersion("V2.0.0", v) && v.major == 2);
    // Not a release version: v untouched
    for (const char* bad : {"", "v", "0.1", "0.1.1.1", "0.1.x", "v0.2.0-rc1", " 0.1.1", "0.1.1 ", "0..1", "-1.0.0",
                            "1234567890.0.0", "unknown", "vv0.1.1"}) {
        Version before;
        before.major = 9;
        CHECK(!parseVersion(bad, before) && before.major == 9);
    }

    Version a, b;
    parseVersion("0.9.0", a);
    parseVersion("0.10.0", b);
    CHECK(compareVersions(a, b) < 0 && compareVersions(b, a) > 0 && compareVersions(a, a) == 0);  // numeric
    CHECK(isNewer("v0.1.1", "0.1.0"));
    CHECK(isNewer("v1.0.0", "0.9.9"));
    CHECK(isNewer("v0.1.10", "0.1.9"));
    CHECK(!isNewer("v0.1.0", "0.1.0"));
    CHECK(!isNewer("v0.0.9", "0.1.0"));  // an older latest (a release pulled back): nothing to say
    CHECK(!isNewer("v0.2.0", "unknown"));
    CHECK(!isNewer("", "0.1.0"));
    CHECK(!isNewer("nightly", "0.1.0"));
}

void testLatestTag() {
    CHECK(parseLatestTag(R"({"url":"x","tag_name":"v0.1.1","name":"Frame Dock 0.1.1","assets":[{"id":1}]})") ==
          "v0.1.1");
    CHECK(parseLatestTag(R"({"tag_name":"v0.1.1"})") == "v0.1.1");
    // GitHub's errors (rate limit, no release) and garbage
    CHECK(parseLatestTag(R"({"message":"API rate limit exceeded","documentation_url":"x"})").empty());
    CHECK(parseLatestTag(R"({"message":"Not Found"})").empty());
    CHECK(parseLatestTag(R"({"tag_name":11})").empty());
    CHECK(parseLatestTag(R"({"tag_name":null})").empty());
    CHECK(parseLatestTag(R"(["v0.1.1"])").empty());
    CHECK(parseLatestTag("<html>").empty());
    CHECK(parseLatestTag(R"({"tag_name":"v0.1.1")").empty());  // cut off
    CHECK(parseLatestTag("").empty());
}

void testOnceADay() {
    const int64_t t = 1'800'000'000;
    CHECK(checkDue(t, 0));  // never checked
    CHECK(!checkDue(t, t));
    CHECK(!checkDue(t + kIntervalSec - 1, t));
    CHECK(checkDue(t + kIntervalSec, t));
    CHECK(checkDue(t - 60, t));  // the clock set back: don't wait for it to catch up
}

void testState() {
    State s;
    s.checkedUnix = 1'800'000'000;
    s.latestTag = "v0.1.1";
    State back;
    CHECK(parseState(stateToJson(s), back) && back.checkedUnix == s.checkedUnix && back.latestTag == "v0.1.1");
    State empty;
    CHECK(parseState(stateToJson(empty), back) && back.checkedUnix == 0 && back.latestTag.empty());
    back.latestTag = "keep";
    CHECK(!parseState("", back) && !parseState("{}", back) && !parseState(R"({"checked":"x"})", back) &&
          !parseState(R"({"checked":-5})", back) && back.latestTag == "keep");
    CHECK(statePath("/home/u/.config/frame-perf-overlay/config.json") ==
          "/home/u/.config/frame-perf-overlay/version_check.json");
    CHECK(statePath("config.json") == "./version_check.json");
}

void testChecker() {
    // Built on a state file: shows what the last check found, without asking (the setting off, or the tab hidden)
    char base[] = "/tmp/version-notice-test-XXXXXX";
    if (mkdtemp(base) == nullptr) {
        std::printf("FAIL: mkdtemp\n");
        ++failures;
        return;
    }
    const std::string path = std::string(base) + "/version_check.json";
    const int64_t now = 1'800'000'000;
    {
        std::ofstream(path) << R"({"checked": )" << now - 60 << R"(, "latest": "v0.1.1"})";
        UpdateChecker checker(path, "0.1.0");
        CHECK(checker.newerTag().empty());
        CHECK(checker.poll(false, true, now) && checker.newerTag() == "v0.1.1");  // checked a minute ago: no request
        CHECK(!checker.poll(true, true, now + 10));  // still within the day: no request either
        CHECK(checker.poll(true, false, now + 20) && checker.newerTag().empty());  // off: no notice
    }
    {
        UpdateChecker checker(path, "0.1.1");  // updated since
        CHECK(!checker.poll(false, true, now) && checker.newerTag().empty());
    }
    {
        std::ofstream(path) << "not json";
        UpdateChecker checker(path, "0.1.0");
        CHECK(!checker.poll(false, true, now) && checker.newerTag().empty());
    }
    if (std::system(("rm -rf '" + std::string(base) + "'").c_str()) != 0) std::printf("could not remove %s\n", base);
}

}  // namespace

int main() {
    testVersions();
    testLatestTag();
    testOnceADay();
    testState();
    testChecker();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("version-notice: all checks passed\n");
    return 0;
}
