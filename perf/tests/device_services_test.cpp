// The 기기 page's services against fakes on this machine, never the real ones: the HTTP agent client and the Frame
// Sync worker against a fake agent on 127.0.0.1 (requests, headers, error kinds, timeouts, the cache file, a whole
// sync with the game question, a stop in the middle), Wake-on-LAN's bytes on a local UDP socket, the room-view
// mailbox against a fake vrserver WebSocket, and Camera Switch over a private dbus-daemon (a fake service owning the
// name, availability, Activate's arguments). The key in ~/.config/frame-sync is never read: configs are made up.
#include "camera_switch.h"
#include "frame_sync_agent.h"
#include "frame_sync_worker.h"
#include "pulse_audio.h"
#include "room_view.h"
#include "websocket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
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

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/** Waits (polling every 5 ms) until done() or timeout seconds. */
bool waitFor(const std::function<bool()>& done, double timeout) {
    const double until = now() + timeout;
    while (!done()) {
        if (now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

std::string tempDir(const char* name) {
    static int made = 0;
    const std::string dir = std::filesystem::temp_directory_path().string() + "/framedock-services-test-" +
                            std::to_string(::getpid()) + "-" + name + "-" + std::to_string(++made);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::string readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void writeAll(const std::string& path, const std::string& text) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

/** A listening TCP socket on 127.0.0.1 (a free port). */
int listenLocal(int& port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(fd, 16);
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    port = ntohs(addr.sin_port);
    return fd;
}

// ---- A fake agent: HTTP/1.1, one request per connection ----

class FakeAgent {
public:
    struct Request {
        std::string method, path, body;
        std::map<std::string, std::string> headers;  ///< lower-case names
    };
    struct Reply {
        int status = 200;
        std::string body = "{}";
        bool hang = false;  ///< accept and never answer
    };
    using Handler = std::function<Reply(const Request&)>;

    FakeAgent() {
        fd_ = listenLocal(port_);
        thread_ = std::thread([this] { run(); });
    }
    ~FakeAgent() {
        stop_ = true;
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        thread_.join();
    }
    int port() const { return port_; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }
    void setHandler(Handler h) {
        std::lock_guard<std::mutex> lock(mutex_);
        handler_ = std::move(h);
    }
    std::vector<Request> requests() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.clear();
    }

private:
    int fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_ {false};
    std::mutex mutex_;
    Handler handler_;
    std::vector<Request> requests_;
    std::thread thread_;
    std::vector<int> hung_;

    void run() {
        while (!stop_) {
            struct pollfd p = {fd_, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0) continue;
            const int c = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (c < 0) continue;
            serve(c);
        }
        for (const int c : hung_) ::close(c);
    }

    void serve(int c) {
        std::string in;
        char buf[4096];
        size_t headerEnd = std::string::npos;
        size_t want = 0;
        while (true) {
            if (headerEnd == std::string::npos) {
                headerEnd = in.find("\r\n\r\n");
                if (headerEnd != std::string::npos) {
                    const size_t cl = lower(in).find("content-length:");
                    want = headerEnd + 4 + (cl < headerEnd ? std::strtoul(in.c_str() + cl + 15, nullptr, 10) : 0);
                }
            }
            if (headerEnd != std::string::npos && in.size() >= want) break;
            struct pollfd p = {c, POLLIN, 0};
            if (::poll(&p, 1, 2000) <= 0) break;
            const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            in.append(buf, static_cast<size_t>(n));
        }
        if (headerEnd == std::string::npos) {
            ::close(c);
            return;
        }
        Request r;
        std::istringstream head(in.substr(0, headerEnd));
        std::string line;
        std::getline(head, line);
        std::istringstream first(line);
        first >> r.method >> r.path;
        while (std::getline(head, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            r.headers[lower(line.substr(0, colon))] = line.substr(line.find_first_not_of(' ', colon + 1));
        }
        r.body = in.substr(headerEnd + 4);
        Reply reply;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(r);
            if (handler_) reply = handler_(r);
        }
        if (reply.hang) {
            hung_.push_back(c);
            return;
        }
        const std::string out = "HTTP/1.1 " + std::to_string(reply.status) + " X\r\nContent-Type: application/json\r\n" +
                                "Content-Length: " + std::to_string(reply.body.size()) +
                                "\r\nConnection: close\r\n\r\n" + reply.body;
        (void)!::send(c, out.data(), out.size(), MSG_NOSIGNAL);
        ::close(c);
    }

    static std::string lower(std::string s) {
        for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    }
};

using framesync::AgentError;

void testHttpAgent() {
    FakeAgent fake;
    framesync::HttpAgent agent(fake.url() + "/", "fake-token\n", 2.0);
    fake.setHandler([](const FakeAgent::Request& r) {
        FakeAgent::Reply reply;
        if (r.path == "/status") {
            reply.body = R"({"steam": {"running": true, "steamId": "76561190000000001", "gameAppId": null},)"
                         R"( "streaming": true, "users": ["u"]})";
        } else if (r.path == "/claim" || r.path == "/shutdown") {
            reply.body = R"({"ok": true})";
        } else if (r.path == "/steam/switch") {
            if (r.body.find("\"force\": true") != std::string::npos) {
                reply.status = 202;
                reply.body = R"({"requestId": "abc"})";
            } else {
                reply.status = 409;
                reply.body = R"({"code": "game", "message": "a game runs"})";
            }
        } else if (r.path == "/steam/switch/abc") {
            reply.body = R"({"done": true, "ok": false, "message": "refused on the PC"})";
        } else if (r.path == "/steam/accounts") {
            reply.body = R"({"accounts": [{"steamId": "76561190000000001", "personaName": "Alpha",)"
                         R"( "accountName": "a", "hasAvatar": true}]})";
        } else if (r.path == "/steam/avatar/76561190000000001") {
            reply.body = "PNGDATA";
        } else if (r.path == "/steam/avatar/76561190000000002") {
            reply.status = 404;
        } else if (r.path == "/denied") {
            reply.status = 401;
            reply.body = R"({"message": "bad key"})";
        } else {
            reply.status = 500;
            reply.body = R"({"code": "boom", "message": "it broke"})";
        }
        return reply;
    });

    const framesync::PcStatus status = agent.status();
    CHECK(status.steamRunning && status.steamId == "76561190000000001" && status.gameAppId.empty() &&
          status.streaming && status.hasUsers);
    auto requests = fake.requests();
    CHECK(requests.size() == 1 && requests[0].method == "GET" && requests[0].path == "/status");
    CHECK(requests[0].headers["authorization"] == "Bearer fake-token");
    CHECK(requests[0].headers.count("content-type") == 0);

    agent.claim();
    requests = fake.requests();
    CHECK(requests.back().method == "POST" && requests.back().body == R"({"source": "frame"})");
    CHECK(requests.back().headers["content-type"] == "application/json");

    framesync::SwitchReply reply = agent.switchTo("76561190000000001", false);
    CHECK(reply.status == 409 && reply.code == "game" && reply.message == "a game runs");
    CHECK(fake.requests().back().body == R"({"steamId": "76561190000000001", "force": false})");
    reply = agent.switchTo("76561190000000001", true);
    CHECK(reply.status == 202 && reply.requestId == "abc");
    const framesync::SwitchResult result = agent.switchResult("abc");
    CHECK(result.done && !result.ok && result.message == "refused on the PC");

    JsonValue raw;
    const auto accounts = agent.accounts(&raw);
    CHECK(accounts.size() == 1 && accounts[0].personaName == "Alpha" && accounts[0].hasAvatar);
    CHECK(raw.type == JsonValue::Type::Array && raw.items.size() == 1);
    CHECK(agent.avatar("76561190000000001") == std::optional<std::string>("PNGDATA"));
    CHECK(!agent.avatar("76561190000000002"));  // 404: none
    agent.shutdown();
    CHECK(fake.requests().back().body == R"({"manual": true})");

    // Refusals: 401 is auth, anything else http with the body's code and message
    try {
        agent.request("GET", "/denied", nullptr);
        CHECK(false);
    } catch (const AgentError& e) {
        CHECK(e.kind == AgentError::Kind::Auth && e.status == 401 && e.message == "bad key");
    }
    try {
        agent.request("GET", "/other", nullptr);
        CHECK(false);
    } catch (const AgentError& e) {
        CHECK(e.kind == AgentError::Kind::Http && e.status == 500 && e.code == "boom" && e.message == "it broke");
    }

    // Nobody listening: unreachable at once
    int closedPort = 0;
    ::close(listenLocal(closedPort));
    framesync::HttpAgent nobody("http://127.0.0.1:" + std::to_string(closedPort), "k", 2.0);
    try {
        nobody.status();
        CHECK(false);
    } catch (const AgentError& e) {
        CHECK(e.kind == AgentError::Kind::Unreachable);
    }

    // An agent that never answers: unreachable after the timeout (1 s of silence), not hanging
    fake.setHandler([](const FakeAgent::Request&) {
        FakeAgent::Reply r;
        r.hang = true;
        return r;
    });
    framesync::HttpAgent slow(fake.url(), "k", 1.0);
    double t0 = now();
    try {
        slow.status();
        CHECK(false);
    } catch (const AgentError& e) {
        CHECK(e.kind == AgentError::Kind::Unreachable);
    }
    const double took = now() - t0;
    CHECK(took >= 0.9 && took < 4.0);

    // Cancelled (shutting down): ends within about a second
    std::atomic<bool> cancel {false};
    framesync::HttpAgent cancellable(fake.url(), "k", 12.0, &cancel);
    std::thread canceller([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancel = true;
    });
    t0 = now();
    try {
        cancellable.status();
        CHECK(false);
    } catch (const AgentError& e) {
        CHECK(e.kind == AgentError::Kind::Cancelled);  // not "off"
    }
    canceller.join();
    CHECK(now() - t0 < 2.5);
}

void testWol() {
    // To a local UDP socket instead of the broadcast address: three magic packets
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    std::string error;
    CHECK(framesync::sendWol("01:23:45:ab:cd:ef", "127.0.0.1", error, ntohs(addr.sin_port)));
    const std::vector<uint8_t> expected = framesync::magicPacket("01:23:45:ab:cd:ef");
    CHECK(expected.size() == 102);
    int packets = 0;
    uint8_t buf[256];
    while (true) {
        struct pollfd p = {fd, POLLIN, 0};
        if (::poll(&p, 1, 500) <= 0) break;
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        CHECK(n == 102 && std::memcmp(buf, expected.data(), 102) == 0);
        CHECK(buf[0] == 0xff && buf[5] == 0xff && buf[6] == 0x01 && buf[11] == 0xef && buf[101] == 0xef);
        ++packets;
    }
    CHECK(packets == 3);
    ::close(fd);
    CHECK(!framesync::sendWol("nonsense", "127.0.0.1", error));
    CHECK(!framesync::sendWol("01:23:45:ab:cd:ef", "not an address", error));
}

// ---- The worker against the fake agent ----

struct WorkerFixture {
    std::string dir;
    FrameSyncWorker::Paths paths;
    explicit WorkerFixture(const std::string& agentUrl) {
        dir = tempDir("worker");
        paths.configDir = dir + "/config";
        paths.cacheFile = dir + "/cache/pc-accounts.json";
        paths.steamDir = std::string(FIXTURES) + "/steam";
        paths.avatarDir = dir + "/avatars";
        writeAll(paths.configDir + "/config.json", "{\"agentUrl\": \"" + agentUrl +
                                                      "\", \"pcMac\": \"01:23:45:ab:cd:ef\", \"broadcast\": \"127.0.0.1\"}");
        writeAll(paths.configDir + "/token", "fake-token\n");
    }
    ~WorkerFixture() { std::filesystem::remove_all(dir); }
};

/** Takes the worker's events until one of kind comes (or the time is up). */
bool waitEvent(FrameSyncWorker& worker, std::vector<SyncEvent>& seen, SyncEvent::Kind kind, double timeout) {
    return waitFor(
        [&] {
            for (SyncEvent& e : worker.take()) seen.push_back(std::move(e));
            for (const SyncEvent& e : seen) {
                if (e.kind == kind) return true;
            }
            return false;
        },
        timeout);
}

const SyncEvent* last(const std::vector<SyncEvent>& seen, SyncEvent::Kind kind) {
    for (auto it = seen.rbegin(); it != seen.rend(); ++it) {
        if (it->kind == kind) return &*it;
    }
    return nullptr;
}

void testWorker() {
    FakeAgent fake;
    std::atomic<bool> gameRuns {true};
    std::atomic<int> switches {0};
    fake.setHandler([&](const FakeAgent::Request& r) {
        FakeAgent::Reply reply;
        if (r.path == "/status") {
            reply.body = R"({"steam": {"running": true, "steamId": "76561190000000005"}, "users": ["u"]})";
        } else if (r.path == "/steam/accounts") {
            reply.body = R"({"accounts": [{"steamId": "76561190000000002", "personaName": "Bravo",)"
                         R"( "accountName": "b", "hasAvatar": true}, {"steamId": "76561190000000001",)"
                         R"( "personaName": "Alpha", "accountName": "a", "hasAvatar": true}]})";
        } else if (r.path == "/steam/avatar/76561190000000002") {
            reply.body = "BRAVO-PNG";
        } else if (r.path == "/steam/switch") {
            ++switches;
            if (gameRuns && r.body.find("\"force\": false") != std::string::npos) {
                reply.status = 409;
                reply.body = R"({"code": "game"})";
            }
        } else {
            reply.status = 404;
        }
        return reply;
    });
    WorkerFixture f(fake.url());
    std::vector<std::string> wols;
    std::mutex wolMutex;
    FrameSyncWorker worker(f.paths, -1, [&](const std::string& mac, const std::string& broadcast) {
        std::lock_guard<std::mutex> lock(wolMutex);
        wols.push_back(mac + " " + broadcast);
    });
    std::vector<SyncEvent> seen;

    // Setup: the settings, the Frame's accounts and avatars, no cache yet
    worker.setup();
    CHECK(waitEvent(worker, seen, SyncEvent::Kind::Setup, 5));
    const SyncEvent* setup = last(seen, SyncEvent::Kind::Setup);
    CHECK(setup && setup->configured && setup->frameId == "76561190000000002" && setup->frameAccounts.size() == 3);
    CHECK(setup && setup->avatars.count("76561190000000001") == 1 && !setup->pcAccounts);

    // A poll: status, accounts (cached in Python's format), the PC's avatar for an account the Frame has none of
    worker.refresh();
    worker.refresh();  // a second while one is queued is dropped
    CHECK(waitEvent(worker, seen, SyncEvent::Kind::Refresh, 5));
    const SyncEvent* refresh = last(seen, SyncEvent::Kind::Refresh);
    CHECK(refresh && refresh->reach == framesync::Reach::On && refresh->error == framesync::ErrorKind::None);
    CHECK(refresh && refresh->status && refresh->status->steamId == "76561190000000005");
    CHECK(refresh && refresh->pcAccounts && refresh->pcAccounts->size() == 2);
    CHECK(refresh && refresh->avatars.size() == 1 && refresh->avatars.count("76561190000000002") == 1);
    CHECK(readAll(f.paths.avatarDir + "/76561190000000002.png") == "BRAVO-PNG");
    CHECK(readAll(f.paths.cacheFile).rfind(R"([{"steamId": "76561190000000002", "personaName": "Bravo", )", 0) == 0);
    int avatarRequests = 0;
    for (const auto& r : fake.requests()) avatarRequests += r.path.rfind("/steam/avatar/", 0) == 0;
    CHECK(avatarRequests == 1);  // Alpha's is on the Frame
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (SyncEvent& e : worker.take()) seen.push_back(std::move(e));
    int refreshes = 0;
    for (const SyncEvent& e : seen) refreshes += e.kind == SyncEvent::Kind::Refresh;
    CHECK(refreshes == 1);

    // A sync: the PC answers, a game runs, the question, 계속, forced, done
    seen.clear();
    worker.startSync("76561190000000002");
    CHECK(waitEvent(worker, seen, SyncEvent::Kind::AskGame, 5));
    worker.answerGame(true);
    CHECK(waitEvent(worker, seen, SyncEvent::Kind::Finished, 5));
    const SyncEvent* finished = last(seen, SyncEvent::Kind::Finished);
    CHECK(finished && finished->ok && finished->message.empty());
    CHECK(switches == 2);
    std::vector<framesync::Step> steps;
    for (const SyncEvent& e : seen) {
        if (e.kind == SyncEvent::Kind::Progress) steps.push_back(e.step);
    }
    CHECK(steps == std::vector<framesync::Step>({framesync::Step::WaitingLogin, framesync::Step::Switching}));
    CHECK(wols.empty());  // the PC was on

    // 취소 on the question
    seen.clear();
    worker.startSync("76561190000000002");
    CHECK(waitEvent(worker, seen, SyncEvent::Kind::AskGame, 5));
    worker.answerGame(false);
    CHECK(waitEvent(worker, seen, SyncEvent::Kind::Finished, 5));
    finished = last(seen, SyncEvent::Kind::Finished);
    CHECK(finished && !finished->ok && finished->message == tr(Tx::SyncCancelled));

    // The PC off: WOL to the configured broadcast, then waiting; a stop ends the wait at once
    int closedPort = 0;
    ::close(listenLocal(closedPort));
    WorkerFixture off("http://127.0.0.1:" + std::to_string(closedPort));
    FrameSyncWorker offWorker(off.paths, -1, [&](const std::string& mac, const std::string& broadcast) {
        std::lock_guard<std::mutex> lock(wolMutex);
        wols.push_back(mac + " " + broadcast);
    });
    seen.clear();
    offWorker.setup();
    offWorker.refresh();
    CHECK(waitEvent(offWorker, seen, SyncEvent::Kind::Refresh, 5));
    refresh = last(seen, SyncEvent::Kind::Refresh);
    CHECK(refresh && refresh->reach == framesync::Reach::Off && refresh->error == framesync::ErrorKind::None);
    offWorker.startSync("76561190000000002");
    CHECK(waitEvent(offWorker, seen, SyncEvent::Kind::Progress, 5));
    CHECK(waitFor(
        [&] {
            std::lock_guard<std::mutex> lock(wolMutex);
            return wols.size() == 1 && wols[0] == "01:23:45:ab:cd:ef 127.0.0.1";
        },
        2));
    const double t0 = now();
    offWorker.stop();
    CHECK(now() - t0 < 1.5);

    // Quitting while a sync's first status call waits for the PC: no WOL (the cancelled call isn't "off")
    FakeAgent silent;
    silent.setHandler([](const FakeAgent::Request&) {
        FakeAgent::Reply reply;
        reply.hang = true;
        return reply;
    });
    WorkerFixture slowPc(silent.url());
    std::atomic<int> quitWols {0};
    {
        FrameSyncWorker quitting(slowPc.paths, -1, [&](const std::string&, const std::string&) { ++quitWols; });
        seen.clear();
        quitting.setup();
        CHECK(waitEvent(quitting, seen, SyncEvent::Kind::Setup, 5));
        quitting.startSync("76561190000000002");
        CHECK(waitFor([&] { return !silent.requests().empty(); }, 5));
        const double t1 = now();
        quitting.stop();
        CHECK(now() - t1 < 2.5);
    }
    CHECK(quitWols == 0);

    // No settings: Setup says so
    WorkerFixture bare(fake.url());
    std::filesystem::remove(bare.paths.configDir + "/token");
    FrameSyncWorker bareWorker(bare.paths, -1, {});
    seen.clear();
    bareWorker.setup();
    CHECK(waitEvent(bareWorker, seen, SyncEvent::Kind::Setup, 5));
    CHECK(last(seen, SyncEvent::Kind::Setup) && !last(seen, SyncEvent::Kind::Setup)->configured);
}

// ---- The room view's mailbox against a fake vrserver WebSocket ----

void testRoomView() {
    int port = 0;
    const int listener = listenLocal(port);
    std::vector<std::string> texts;
    std::mutex mutex;
    std::atomic<int> connections {0};
    std::atomic<bool> stop {false};
    std::thread server([&] {
        while (!stop) {
            struct pollfd p = {listener, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0) continue;
            const int c = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
            if (c < 0) continue;
            ++connections;
            std::string in;
            bool upgraded = false;
            ws::FrameParser parser;
            char buf[4096];
            while (true) {
                struct pollfd q = {c, POLLIN, 0};
                if (::poll(&q, 1, 2000) <= 0) break;
                const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                if (!upgraded) {
                    in.append(buf, static_cast<size_t>(n));
                    size_t consumed = 0;
                    std::string path, response;
                    if (ws::parseHandshakeRequest(in, consumed, path, response) != ws::Handshake::Ok) continue;
                    upgraded = true;
                    (void)!::send(c, response.data(), response.size(), MSG_NOSIGNAL);
                    std::vector<ws::FrameParser::Message> messages;
                    parser.feed(in.data() + consumed, in.size() - consumed, messages);
                    for (auto& m : messages) {
                        std::lock_guard<std::mutex> lock(mutex);
                        texts.push_back(m.payload);
                    }
                    continue;
                }
                std::vector<ws::FrameParser::Message> messages;
                parser.feed(buf, static_cast<size_t>(n), messages);
                for (auto& m : messages) {
                    if (m.opcode != ws::Text) continue;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        texts.push_back(m.payload);
                    }
                    if (m.payload.rfind("mailbox_open ", 0) == 0) {
                        const std::string answer = ws::encodeFrame(ws::Text, R"({"type":"mailbox_registered"})", nullptr);
                        (void)!::send(c, answer.data(), answer.size(), MSG_NOSIGNAL);
                    }
                }
            }
            ::close(c);
        }
    });

    {
        RoomViewMailbox rv(port);
        rv.setEnabled(true);
        CHECK(waitFor([&] { return rv.finished() == 1; }, 5));
        CHECK(rv.sent() == 1);
        rv.setEnabled(false);
        CHECK(waitFor([&] { return rv.finished() == 2; }, 5));
        CHECK(rv.sent() == 2);
    }
    CHECK(waitFor(
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return texts.size() == 4;
        },
        3));
    {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(texts.size() == 4);
        if (texts.size() == 4) {
            CHECK(texts[0] == "mailbox_open framedock_roomview");
            CHECK(texts[1] == R"(mailbox_send vrcompositor_systemlayer {"type":"set_camera_roomview_enabled","enabled":true})");
            CHECK(texts[2] == "mailbox_open framedock_roomview");
            CHECK(texts[3] ==
                  R"(mailbox_send vrcompositor_systemlayer {"type":"set_camera_roomview_enabled","enabled":false})");
        }
    }
    CHECK(connections == 2);
    // --room-view probe: the server's answers come back
    std::vector<std::string> replies;
    std::string error;
    CHECK(roomview::exchange(port, roomview::sendCommand("vrcompositor_systemlayer", roomview::probeMessage()), 0.3, -1,
                             &replies, error));
    CHECK(replies.size() == 1 && replies[0] == R"({"type":"mailbox_registered"})");
    CHECK(waitFor(
        [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return texts.size() == 6;
        },
        3));
    {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(texts.size() == 6 &&
              texts[5] == R"(mailbox_send vrcompositor_systemlayer {"type":"get_debug_commands",)"
                          R"("returnAddress":"framedock_roomview"})");
    }
    stop = true;
    server.join();
    ::close(listener);

    // Nobody listening: the exchange fails quickly and says so; the destructor doesn't hang
    int closedPort = 0;
    ::close(listenLocal(closedPort));
    RoomViewMailbox none(closedPort);
    none.setEnabled(true);
    CHECK(waitFor([&] { return none.finished() == 1; }, 5));
    CHECK(none.sent() == 0);
    CHECK(roomview::enableMessage(true) == R"({"type":"set_camera_roomview_enabled","enabled":true})");
}

// ---- Sound without a server (the real one is never touched: PULSE_SERVER points at nothing) ----

void testPulseNoServer() {
    const std::string dir = tempDir("pulse");
    const char* old = std::getenv("PULSE_SERVER");
    const std::string saved = old != nullptr ? old : "";
    ::setenv("PULSE_SERVER", ("unix:" + dir + "/none").c_str(), 1);
    {
        PulseAudio pulse(-1);
        pulse.setWanted(true);
        pulse.setVolume(50);  // waits for a connection that never comes
        pulse.setMuted(true);
        pulse.selectOutput("x");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        AudioState state;
        state.known = true;
        CHECK(!pulse.take(state) || !state.known);  // nothing known, nothing made up
        CHECK(pulse.queries() == 0);
        pulse.setWanted(false);
        pulse.setWanted(true);
    }  // the destructor doesn't hang
    if (old != nullptr) ::setenv("PULSE_SERVER", saved.c_str(), 1);
    else ::unsetenv("PULSE_SERVER");
    std::filesystem::remove_all(dir);
}

// ---- The room view over a fake vrcmd (a shell script) ----

/** An executable script in dir. */
std::string script(const std::string& dir, const char* name, const std::string& body) {
    const std::string path = dir + "/" + name;
    std::ofstream(path) << "#!/bin/sh\n" << body << "\n";
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    return path;
}

bool readable(int fd) {
    struct pollfd p = {fd, POLLIN, 0};
    return ::poll(&p, 1, 0) == 1;
}

void testRoomViewVrcmd() {
    const std::string dir = tempDir("vrcmd");
    const std::string log = dir + "/args.txt";
    const std::string ok = script(dir, "vrcmd", "echo \"$@\" >> '" + log + "'\necho \"vrcmd: $2\"");
    const std::string refuses = script(dir, "refuses", "echo nope >&2\nexit 3");
    const std::string hangs = script(dir, "hangs", "sleep 30");

    // One run: the arguments as given (no shell between), its output, exit 0
    std::string output;
    std::string error;
    CHECK(roomview::runVrcmd(ok, "a?b=1 c", 2, -1, output, error));
    CHECK(output == "vrcmd: a?b=1 c\n");
    CHECK(readAll(log) == "--compositorcmd a?b=1 c\n");
    output.clear();
    CHECK(!roomview::runVrcmd(refuses, "x", 2, -1, output, error) && error == "vrcmd exited with 3" && output == "nope\n");
    CHECK(!roomview::runVrcmd(dir + "/missing", "x", 2, -1, output, error) && error.find("couldn't run") == 0);
    // Too slow: killed (with what it started) and reaped within the limit
    const double t0 = now();
    CHECK(!roomview::runVrcmd(hangs, "x", 0.3, -1, output, error) && error.find("no answer") == 0);
    CHECK(now() - t0 < 2);

    // The app's control: the wish goes out on its thread, the outcome comes back once and pokes the loop
    std::filesystem::remove(log);
    const int wake = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    {
        RoomViewVrcmd rv(ok, "on-cmd", "off-cmd", wake);
        bool result = false;
        CHECK(!rv.takeResult(result));
        rv.setEnabled(true);
        CHECK(waitFor([&] { return rv.finished() == 1; }, 5));
        CHECK(waitFor([&] { return readable(wake); }, 1));
        CHECK(rv.takeResult(result) && result);
        CHECK(!rv.takeResult(result));
        rv.setEnabled(false);
        CHECK(waitFor([&] { return rv.finished() == 2; }, 5));
        CHECK(waitFor([&] { bool r = false; return rv.takeResult(r) && r; }, 1));
        CHECK(readAll(log) == "--compositorcmd on-cmd\n--compositorcmd off-cmd\n");
    }
    {
        RoomViewVrcmd rv(refuses, "on", "off", -1);
        rv.setEnabled(true);
        CHECK(waitFor([&] { return rv.finished() == 1; }, 5));
        bool result = true;
        CHECK(waitFor([&] { return rv.takeResult(result); }, 1) && !result && rv.sent() == 0);
    }
    {
        // A wish that is overtaken: only the newer one's outcome is reported
        RoomViewVrcmd rv(hangs, "on", "off", -1, 0.3);
        rv.setEnabled(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // the first attempt runs
        rv.setEnabled(false);
        CHECK(waitFor([&] { return rv.finished() == 2; }, 5));
        bool result = true;
        CHECK(rv.takeResult(result) && !result);
        CHECK(!rv.takeResult(result));
    }
    {
        // Quitting during a run doesn't wait for vrcmd
        auto rv = std::make_unique<RoomViewVrcmd>(hangs, "on", "off", -1, 30);
        rv->setEnabled(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const double t1 = now();
        rv.reset();
        CHECK(now() - t1 < 1.5);
    }
    ::close(wake);

    // The route from the environment
    ::unsetenv("FRAMEDOCK_ROOMVIEW");
    ::unsetenv("FRAMEDOCK_ROOMVIEW_MAILBOX");
    ::unsetenv("FRAMEDOCK_ROOMVIEW_CMD_ON");
    ::unsetenv("FRAMEDOCK_ROOMVIEW_CMD_OFF");
    std::string warning;
    roomview::Route route = roomview::routeFromEnv(warning);
    CHECK(warning.empty() && route.kind == roomview::Route::Kind::Openvr && route.mailbox == "vrcompositor_systemlayer");
    CHECK(route.vrcmd == "/opt/steamvr/bin/linuxarm64/vrcmd");
    CHECK(route.cmdOn == "camera_room_view_toggle" && route.cmdOff == "camera_room_view_toggle");
    ::setenv("FRAMEDOCK_ROOMVIEW_CMD_ON", "set_camera_roomview_enabled?enabled=1", 1);
    ::setenv("FRAMEDOCK_ROOMVIEW_CMD_OFF", "set_camera_roomview_enabled?enabled=0", 1);
    route = roomview::routeFromEnv(warning);
    CHECK(route.cmdOn == "set_camera_roomview_enabled?enabled=1" && route.cmdOff == "set_camera_roomview_enabled?enabled=0");
    ::setenv("FRAMEDOCK_ROOMVIEW", "mailbox", 1);
    ::setenv("FRAMEDOCK_ROOMVIEW_MAILBOX", "vrcompositor_mailbox", 1);
    route = roomview::routeFromEnv(warning);
    CHECK(warning.empty() && route.kind == roomview::Route::Kind::Mailbox && route.mailbox == "vrcompositor_mailbox");
    ::setenv("FRAMEDOCK_ROOMVIEW", "vrcmd", 1);
    route = roomview::routeFromEnv(warning);
    CHECK(warning.empty() && route.kind == roomview::Route::Kind::Vrcmd);
    ::setenv("FRAMEDOCK_ROOMVIEW", "dbus", 1);
    route = roomview::routeFromEnv(warning);
    CHECK(!warning.empty() && route.kind == roomview::Route::Kind::Openvr);

    // The openvr route: the set message (not a toggle) at once, its outcome reported once
    CHECK(roomview::sceneAppsMessage(false) == R"({"type":"set_camera_roomview_enabled_forsceneapps","enabled":false})");
    {
        std::vector<std::string> sent;
        bool refuse = false;
        RoomViewDirect direct(
            [&](const std::string& mailbox, const std::string& message, std::string& error) {
                if (refuse) {
                    error = "refused";
                    return false;
                }
                sent.push_back(mailbox + " " + message);
                return true;
            },
            "vrcompositor_systemlayer");
        CHECK(direct.exact());
        bool ok = false;
        CHECK(!direct.takeResult(ok));
        direct.setEnabled(true);
        CHECK(sent.size() == 1 && sent[0] == R"(vrcompositor_systemlayer {"type":"set_camera_roomview_enabled","enabled":true})");
        CHECK(direct.takeResult(ok) && ok);
        CHECK(!direct.takeResult(ok));
        // Off: SteamVR's button's message, then the scene apps' one cleared
        direct.setEnabled(false);
        CHECK(sent.size() == 3 &&
              sent[1] == R"(vrcompositor_systemlayer {"type":"set_camera_roomview_enabled","enabled":false})" &&
              sent[2] == R"(vrcompositor_systemlayer {"type":"set_camera_roomview_enabled_forsceneapps","enabled":false})");
        CHECK(direct.takeResult(ok) && ok);
        refuse = true;
        direct.setEnabled(false);
        CHECK(direct.takeResult(ok) && !ok);
    }
    for (const char* name : {"FRAMEDOCK_ROOMVIEW", "FRAMEDOCK_ROOMVIEW_MAILBOX", "FRAMEDOCK_ROOMVIEW_CMD_ON",
                             "FRAMEDOCK_ROOMVIEW_CMD_OFF"}) {
        ::unsetenv(name);
    }
    std::filesystem::remove_all(dir);
}

// ---- Camera Switch over a private bus ----

struct FakeCameraSwitch {
    sd_bus* bus = nullptr;
    sd_bus_slot* slot = nullptr;
    std::vector<int> colors;  ///< Activate's booleans
    std::string action;
};

int onActivate(sd_bus_message* m, void* user, sd_bus_error*) {
    auto* fake = static_cast<FakeCameraSwitch*>(user);
    const char* name = nullptr;
    if (sd_bus_message_read(m, "s", &name) < 0) return -EINVAL;
    fake->action = name;
    if (sd_bus_message_enter_container(m, 'a', "v") < 0) return -EINVAL;
    int value = -1;
    if (sd_bus_message_enter_container(m, 'v', "b") >= 0) {
        sd_bus_message_read(m, "b", &value);
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
    fake->colors.push_back(value);
    return sd_bus_reply_method_return(m, "");
}

const sd_bus_vtable kActionsVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Activate", "sava{sv}", "", onActivate, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

/** Runs the fake's bus until done() (or the time is up). */
bool pump(FakeCameraSwitch& fake, const std::function<bool()>& done, double timeout) {
    const double until = now() + timeout;
    while (!done()) {
        if (now() > until) return false;
        while (sd_bus_process(fake.bus, nullptr) > 0) {
        }
        sd_bus_wait(fake.bus, 10000);
    }
    return true;
}

/** A private session bus (dbus-daemon), so the real Camera Switch is never touched. @return its address ("": none) */
std::string startBus(pid_t& daemon) {
    int out[2];
    if (::pipe(out) != 0) return {};
    daemon = ::fork();
    if (daemon == 0) {
        ::dup2(out[1], 1);
        ::close(out[0]);
        ::execlp("dbus-daemon", "dbus-daemon", "--session", "--nofork", "--nopidfile", "--print-address=1",
                 static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(out[1]);
    std::string address;
    char c;
    struct pollfd p = {out[0], POLLIN, 0};
    while (::poll(&p, 1, 3000) > 0 && ::read(out[0], &c, 1) == 1 && c != '\n') address += c;
    ::close(out[0]);
    if (address.empty()) {
        ::kill(daemon, SIGTERM);
        ::waitpid(daemon, nullptr, 0);
    }
    return address;
}

/** The fake Camera Switch on the bus DBUS_SESSION_BUS_ADDRESS names, owning its name. */
bool startFake(FakeCameraSwitch& fake) {
    return sd_bus_open_user(&fake.bus) >= 0 &&
           sd_bus_add_object_vtable(fake.bus, &fake.slot, CameraSwitchDbus::kPath, "org.gtk.Actions", kActionsVtable,
                                    &fake) >= 0 &&
           sd_bus_request_name(fake.bus, CameraSwitchDbus::kName, 0) >= 0;
}

void stopFake(FakeCameraSwitch& fake) {
    sd_bus_slot_unref(fake.slot);
    sd_bus_flush_close_unref(fake.bus);
    fake.slot = nullptr;
    fake.bus = nullptr;
}

void testCameraSwitch() {
    pid_t daemon = -1;
    const std::string address = startBus(daemon);
    if (address.empty()) {
        std::printf("SKIP camera switch: no dbus-daemon\n");
        return;
    }
    const char* oldAddress = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    const std::string saved = oldAddress != nullptr ? oldAddress : "";
    ::setenv("DBUS_SESSION_BUS_ADDRESS", address.c_str(), 1);

    const std::string dir = tempDir("camera");
    const std::string colorFile = dir + "/passthrough-color";
    {
        CameraSwitchDbus service(-1, colorFile);
        bool available = true;
        service.setWanted(true);
        CHECK(waitFor([&] { return service.takeAvailable(available); }, 5));
        CHECK(!available);  // nobody owns the name

        FakeCameraSwitch fake;
        CHECK(startFake(fake));
        CHECK(pump(fake, [&] { return service.takeAvailable(available); }, 5));
        CHECK(available);

        service.setColor(true);
        CHECK(pump(fake, [&] { return fake.colors.size() == 1; }, 5));
        CHECK(fake.action == "set-color-passthrough" && fake.colors == std::vector<int>({1}));
        CHECK(pump(fake, [&] { return service.callsAnswered() == 1; }, 5));
        CHECK(readRememberedColor(colorFile) == std::optional<bool>(true));
        service.setColor(false);
        CHECK(pump(fake, [&] { return service.callsAnswered() == 2; }, 5));
        CHECK(fake.colors == std::vector<int>({1, 0}));
        CHECK(readRememberedColor(colorFile) == std::optional<bool>(false));

        // Camera Switch quits
        sd_bus_release_name(fake.bus, CameraSwitchDbus::kName);
        CHECK(pump(fake, [&] { return service.takeAvailable(available); }, 5));
        CHECK(!available);
        CHECK(sd_bus_request_name(fake.bus, CameraSwitchDbus::kName, 0) >= 0);
        CHECK(pump(fake, [&] { return service.takeAvailable(available); }, 5));
        CHECK(available);
        stopFake(fake);
        CHECK(waitFor([&] { return service.takeAvailable(available); }, 5));
        CHECK(!available);

        // The bus itself goes away: the thread ends and says Camera Switch is gone...
        CHECK(startFake(fake));
        CHECK(pump(fake, [&] { return service.takeAvailable(available); }, 5));
        CHECK(available);
        ::kill(daemon, SIGTERM);
        ::waitpid(daemon, nullptr, 0);
        CHECK(waitFor([&] { return service.takeAvailable(available); }, 5));
        CHECK(!available);
        stopFake(fake);
        // ...and starts again the next time the page is shown (a new bus here)
        const std::string second = startBus(daemon);
        CHECK(!second.empty());
        ::setenv("DBUS_SESSION_BUS_ADDRESS", second.c_str(), 1);
        CHECK(startFake(fake));
        service.setWanted(false);
        service.setWanted(true);
        CHECK(pump(fake, [&] { return service.takeAvailable(available); }, 5));
        CHECK(available);
        fake.colors.clear();
        service.setColor(true);
        CHECK(pump(fake, [&] { return fake.colors.size() == 1; }, 5));
        stopFake(fake);
    }
    std::filesystem::remove_all(dir);
    ::kill(daemon, SIGTERM);
    ::waitpid(daemon, nullptr, 0);
    if (oldAddress != nullptr) ::setenv("DBUS_SESSION_BUS_ADDRESS", saved.c_str(), 1);
    else ::unsetenv("DBUS_SESSION_BUS_ADDRESS");
}

}  // namespace

int main() {
    ::signal(SIGPIPE, SIG_IGN);
    framesync::initHttp();
    testHttpAgent();
    testWol();
    testWorker();
    testPulseNoServer();
    testRoomView();
    testRoomViewVrcmd();
    testCameraSwitch();
    if (failures > 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("device-services: all passed\n");
    return 0;
}
