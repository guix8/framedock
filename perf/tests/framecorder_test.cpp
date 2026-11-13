// 녹화 (framecorder.h): ui.conf to the recorder's options, telling the recorder from framecorder's other programs, the
// search in a made-up /proc, and Recorder against fake recorders (shell scripts in a temporary folder: one that saves
// on SIGINT, one that ends by itself, one that ignores SIGINT), and Recorder with framecorder's tab up: a fake tab
// (status.json written here, command.json read back) instead of the real one. framecorder itself isn't needed.
#include "framecorder.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                             \
    do {                                                        \
        if (!(expr)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr); \
            ++failures;                                         \
        }                                                       \
    } while (false)

using framecorder::Recorder;
using framecorder::State;
using Args = std::vector<std::string>;

void write(const std::string& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
}

bool exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

std::string read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void testUiConf() {
    // The settings on the Frame this was written for (framecorder 0.1.2's tab)
    const std::string frame =
        "shape=square\neye=right\nquality=standard\nfps=auto\ngame_audio=true\nmic=false\nclips=false\nclip=60\n"
        "onboarded=true\n";
    CHECK(framecorder::argsFromUiConf(frame) == (Args {"--aspect", "1:1", "--eye", "right"}));
    CHECK(framecorder::argsFromUiConf("") == Args {});
    CHECK(framecorder::argsFromUiConf("shape=wide\n") == (Args {"--aspect", "16:9"}));
    CHECK(framecorder::argsFromUiConf("shape = tall \r\n") == (Args {"--aspect", "9:16"}));
    CHECK(framecorder::argsFromUiConf("game_audio=false\nmic=true\nfps=60\neye=left\n") ==
          (Args {"--eye", "left", "--fps", "60", "--no-audio", "--mic"}));
    // Unknown values, quality and comments: framecorder's defaults
    CHECK(framecorder::argsFromUiConf("# c\nshape=round\neye=both\nfps=fast\nfps2=30\nquality=high\nmic=yes\n") ==
          Args {});
    CHECK(framecorder::argsFromUiConf("fps=0\n") == Args {});
}

void testProcessName() {
    using framecorder::isRecorderProcess;
    CHECK(isRecorderProcess("framecorder", {}));
    CHECK(isRecorderProcess("x", {"/home/deck/.local/bin/framecorder", "--control"}));
    CHECK(isRecorderProcess("python3", {"/usr/bin/python3", "/home/deck/.local/bin/framecorder"}));
    CHECK(!isRecorderProcess("framecorder-ui", {"/home/deck/.local/bin/framecorder-ui"}));
    CHECK(!isRecorderProcess("framecorder-syn", {"/home/deck/.local/bin/framecorder-sync"}));
    CHECK(!isRecorderProcess("framecorder-gra", {"framecorder-grab"}));
    CHECK(!isRecorderProcess("python3", {"python3", "/home/deck/.local/bin/framecorder-ui"}));
    CHECK(!isRecorderProcess("vim", {"vim", "framecorder"}));  // a file of that name open: not the recorder
    CHECK(!isRecorderProcess("framecorderx", {"framecorders"}));
}

void testFindRecorder(const std::string& dir) {
    const std::string proc = dir + "/proc";
    ::mkdir(proc.c_str(), 0700);
    const auto add = [&](const char* pid, const std::string& comm, const std::string& cmdline) {
        const std::string p = proc + "/" + pid;
        ::mkdir(p.c_str(), 0700);
        write(p + "/comm", comm + "\n");
        write(p + "/cmdline", cmdline);
    };
    add("1", "systemd", std::string("/sbin/init\0", 11));
    add("100", "framecorder-ui", std::string("/home/deck/.local/bin/framecorder-ui\0", 37));
    add("101", "python3", std::string("python3\0/home/deck/.local/bin/framecorder-sync\0", 47));
    ::mkdir((proc + "/self").c_str(), 0700);
    CHECK(framecorder::findRecorder(proc, 0) == 0);
    add("102", "framecorder", std::string("/home/deck/.local/bin/framecorder\0--control\0", 44));
    CHECK(framecorder::findRecorder(proc, 0) == 102);
    CHECK(framecorder::findRecorder(proc, 102) == 0);  // Frame Dock's own child
    CHECK(framecorder::findRecorder(dir + "/none", 0) == 0);
}

/** Calls tick at the made-up time now until the state isn't from any more (real waits, 5 s at most). */
bool waitWhile(Recorder& r, State from, double now) {
    for (int i = 0; i < 250; ++i) {
        r.tick(now);
        if (r.state() != from) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

std::string script(const std::string& dir, const char* name, const std::string& body) {
    const std::string path = dir + "/" + name;
    write(path, "#!/bin/sh\n" + body);
    ::chmod(path.c_str(), 0755);
    return path;
}

void testRecorder(const std::string& dir) {
    const std::string emptyProc = dir + "/emptyproc";
    ::mkdir(emptyProc.c_str(), 0700);
    // Saves on SIGINT (framecorder's "ctrl+c to stop"), writes down its options
    const std::string saver = script(dir, "framecorder",
                                     "trap 'echo saved > \"$0.saved\"; exit 0' INT\n"
                                     "echo \"$@\" > \"$0.args\"\n"  // after the trap: its file says it is set
                                     "while :; do sleep 0.05; done\n");
    const std::string conf = dir + "/ui.conf";
    write(conf, "shape=square\neye=right\nfps=auto\ngame_audio=false\n");

    // Not installed: nothing to press
    {
        Recorder none(dir + "/missing", conf, emptyProc);
        CHECK(!none.available() && !none.toggle(0) && none.state() == State::Idle);
        CHECK(std::isinf(none.nextWake(0)));
    }
    {
        Recorder r(saver, conf, emptyProc);
        CHECK(r.available() && r.state() == State::Idle && r.pid() <= 0);
        CHECK(r.toggle(100) && r.state() == State::Recording && r.pid() > 0);
        CHECK(r.elapsedSec(142.5) == 42 && r.nextWake(142.5) == 143);
        CHECK(r.tick(142.5));   // a new second on the card
        CHECK(!r.tick(142.7));  // the same one
        for (int i = 0; i < 100 && !exists(saver + ".args"); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(read(saver + ".args") == "--aspect 1:1 --eye right --no-audio\n");
        CHECK(r.toggle(150) && r.state() == State::Saving);
        CHECK(!r.toggle(150.5));  // nothing while it saves
        CHECK(waitWhile(r, State::Saving, 151) && r.state() == State::Idle && r.pid() <= 0);
        CHECK(exists(saver + ".saved"));
        CHECK(std::isinf(r.nextWake(152)));
    }
    // Another recorder runs (framecorder's own tab started it): refused, said for kNoteSec
    {
        const std::string busyProc = dir + "/busyproc";
        ::mkdir(busyProc.c_str(), 0700);
        ::mkdir((busyProc + "/4242").c_str(), 0700);
        write(busyProc + "/4242/comm", "framecorder\n");
        Recorder r(saver, conf, busyProc);
        CHECK(r.toggle(10) && r.state() == State::Busy && r.pid() <= 0);
        CHECK(r.nextWake(10) == 10 + Recorder::kNoteSec);
        CHECK(!r.tick(12) && r.state() == State::Busy);
        CHECK(r.tick(10 + Recorder::kNoteSec) && r.state() == State::Idle);
    }
    // Ends by itself: Failed for kNoteSec, then idle; a press then starts again
    {
        const std::string quitter = script(dir, "quitter", "exit 3\n");
        Recorder r(quitter, "", emptyProc);
        CHECK(r.toggle(0) && r.state() == State::Recording);
        CHECK(waitWhile(r, State::Recording, 1) && r.state() == State::Failed && r.pid() <= 0);
        CHECK(r.tick(1 + Recorder::kNoteSec) && r.state() == State::Idle);
        CHECK(r.toggle(20) && r.state() == State::Recording);
        CHECK(waitWhile(r, State::Recording, 21) && r.state() == State::Failed);
    }
    // Doesn't end on SIGINT (its trap only says so): SIGKILL kStopSec after the stop, then reaped
    {
        const std::string stubborn = script(dir, "stubborn", "trap 'echo ignored' INT\nwhile :; do sleep 0.05; done\n");
        Recorder r(stubborn, "", emptyProc);
        CHECK(r.toggle(0));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // the trap is set
        CHECK(r.toggle(1) && r.state() == State::Saving);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        r.tick(2);
        CHECK(r.state() == State::Saving && r.pid() > 0);  // still there: no SIGKILL yet
        CHECK(waitWhile(r, State::Saving, 1 + Recorder::kStopSec) && r.state() == State::Idle && r.pid() <= 0);
    }
    // Frame Dock exits while recording: the destructor stops it, and it saves
    {
        ::unlink((saver + ".saved").c_str());
        {
            Recorder r(saver, conf, emptyProc);
            CHECK(r.toggle(0));
            std::this_thread::sleep_for(std::chrono::milliseconds(300));  // the trap is set
        }
        CHECK(exists(saver + ".saved"));
    }
    // Couldn't run at all (not executable any more): Failed
    {
        Recorder r(saver, conf, emptyProc);
        ::chmod(saver.c_str(), 0644);
        CHECK(r.toggle(0));
        // posix_spawn says so itself, or (where it can't tell, as under qemu-user) the child's exit 127 does
        if (r.state() == State::Recording) waitWhile(r, State::Recording, 1);
        CHECK(r.state() == State::Failed && r.pid() <= 0);
    }
}

/** The fake tab's status.json: the tab as this process (its pid in the made-up /proc) */
void tabStatus(const std::string& syncDir, const std::string& fields) {
    const long wall = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count());
    write(syncDir + "/status.json", "{\"pid\":" + std::to_string(::getpid()) + ",\"ready\":true,\"at\":" +
                                        std::to_string(wall) + "," + fields + "}");
}

void testParseTabStatus() {
    // What framecorder's dev build (e7c7bcc) wrote on a Frame, clips on
    const framecorder::TabStatus s = framecorder::parseTabStatus(
        "{\"at\":1794579879558,\"clip_ready\":true,\"clips\":60,\"done\":3,\"live_capture\":false,"
        "\"live_viewers\":0,\"panels\":true,\"pid\":49054,\"ready\":true,\"recorded_ms\":0,\"recording\":false,"
        "\"result\":\"ok\",\"running\":false}");
    CHECK(s.read && s.pid == 49054 && s.ready && s.clips && s.clipReady && !s.recording && s.done == 3);
    CHECK(s.result == "ok" && s.at == 1794579879558ull);
    // Clips off: null
    CHECK(!framecorder::parseTabStatus("{\"pid\":7,\"clips\":null}").clips);
    CHECK(!framecorder::parseTabStatus("").read && !framecorder::parseTabStatus("{\"ready\":true}").read);
    CHECK(!framecorder::parseTabStatus("{\"pid\":7,").read);  // cut short
    CHECK(framecorder::tabDir("/home/u/.config/framecorder/ui.conf") == "/home/u/.config/framecorder/sync");
}

void testTab(const std::string& dir) {
    const std::string home = dir + "/tabhome", sync = home + "/sync", proc = dir + "/tabproc";
    for (const std::string& d : {home, sync, proc, proc + "/" + std::to_string(::getpid())}) ::mkdir(d.c_str(), 0700);
    const std::string conf = home + "/ui.conf";
    write(conf, "clips=true\n");
    // Run only when the tab isn't up
    const std::string spawned = script(dir, "framecorder-tabless", "echo ran > \"$0.ran\"\nexec sleep 30\n");
    const std::string command = sync + "/command.json";

    tabStatus(sync, "\"recording\":false,\"running\":false,\"clips\":null,\"clip_ready\":false,\"done\":4,"
                    "\"result\":\"ok\",\"recorded_ms\":0");
    Recorder r(spawned, conf, proc);
    CHECK(r.available() && r.state() == State::Idle && !r.clipsOffered() && !r.clip(0));

    // 녹화 asks the tab (one more than its done), nothing is run here; its answer makes it the tab's recording
    CHECK(r.toggle(10) && r.pid() <= 0);
    const std::string asked = read(command);
    CHECK(asked.find("\"id\":5,\"do\":\"record\",\"from\":\"Frame Dock\"") != std::string::npos);
    CHECK(!r.toggle(10.1));                   // waiting for the answer
    CHECK(r.nextWake(10.1) <= 10.1 + 0.05);   // looked for soon
    CHECK(!r.tick(10.2) && r.state() == State::Idle);
    tabStatus(sync, "\"recording\":true,\"running\":true,\"clips\":null,\"clip_ready\":false,\"done\":5,"
                    "\"result\":\"ok\",\"recorded_ms\":42000");
    CHECK(r.tick(10.3) && r.state() == State::Recording && r.elapsedSec(10.3) == 42);
    CHECK(std::isinf(framecorder::Recorder(dir + "/missing", conf, proc).nextWake(0)));

    // 정지: asked, Saving until the tab says it stopped
    CHECK(r.toggle(20) && r.state() == State::Saving);
    CHECK(read(command).find("\"id\":6,\"do\":\"stop\"") != std::string::npos);
    tabStatus(sync, "\"recording\":false,\"running\":false,\"clips\":null,\"clip_ready\":false,\"done\":6,"
                    "\"result\":\"ok\",\"recorded_ms\":0");
    CHECK(r.tick(20.2) && r.state() == State::Idle);

    // A recording started from the tab or a phone shows here too, and its stop there ends it here
    tabStatus(sync, "\"recording\":true,\"running\":false,\"clips\":null,\"clip_ready\":false,\"done\":6,"
                    "\"result\":\"ok\",\"recorded_ms\":3000");
    CHECK(r.tick(30) && r.state() == State::Recording && r.elapsedSec(31) == 4);
    tabStatus(sync, "\"recording\":false,\"running\":false,\"clips\":null,\"clip_ready\":false,\"done\":6,"
                    "\"result\":\"ok\",\"recorded_ms\":0");
    CHECK(r.tick(32) && r.state() == State::Idle);

    // Clips on: 클립 offered; saved, then said for kNoteSec
    tabStatus(sync, "\"recording\":false,\"running\":false,\"clips\":60,\"clip_ready\":true,\"done\":6,"
                    "\"result\":\"ok\",\"recorded_ms\":0");
    CHECK(r.tick(40) && r.clipsOffered() && r.clipUsable());
    CHECK(r.clip(41) && !r.clipUsable() && read(command).find("\"id\":7,\"do\":\"clip\"") != std::string::npos);
    tabStatus(sync, "\"recording\":false,\"running\":false,\"clips\":60,\"clip_ready\":true,\"done\":7,"
                    "\"result\":\"ok\",\"recorded_ms\":0");
    CHECK(r.tick(41.1) && r.state() == State::ClipSaved && r.clipsOffered());
    CHECK(!r.tick(41 + Recorder::kNoteSec) && r.tick(41.1 + Recorder::kNoteSec) && r.state() == State::Idle);
    // The tab says no: Refused
    CHECK(r.clip(50));
    tabStatus(sync, "\"recording\":false,\"running\":false,\"clips\":60,\"clip_ready\":true,\"done\":8,"
                    "\"result\":\"just saved one\",\"recorded_ms\":0");
    CHECK(r.tick(50.1) && r.state() == State::Refused);
    // No answer in kAnswerSec: Refused too, and the next press is free again
    CHECK(r.tick(60) && r.state() == State::Idle && r.clip(60));
    CHECK(!r.tick(60 + Recorder::kAnswerSec - 0.1) && r.state() == State::Idle);
    CHECK(r.tick(60 + Recorder::kAnswerSec) && r.state() == State::Refused && r.clipUsable());

    // The tab gone (its pid not running, its status.json left behind): 녹화 runs the recorder itself again
    ::rmdir((proc + "/" + std::to_string(::getpid())).c_str());
    CHECK(r.tick(80) && !r.clipsOffered());
    CHECK(r.toggle(81) && r.state() == State::Recording && r.pid() > 0);
    for (int i = 0; i < 100 && !exists(spawned + ".ran"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(exists(spawned + ".ran"));
}

}  // namespace

int main() {
    testUiConf();
    testProcessName();
    char tmpl[] = "/tmp/framecorder-test-XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (dir == nullptr) {
        std::printf("FAIL: mkdtemp\n");
        return 1;
    }
    testFindRecorder(dir);
    testRecorder(dir);
    testParseTabStatus();
    testTab(dir);
    const std::string cleanup = std::string("rm -rf '") + dir + "'";
    if (std::system(cleanup.c_str()) != 0) std::printf("(couldn't remove %s)\n", dir);
    if (failures > 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("framecorder OK\n");
    return 0;
}
