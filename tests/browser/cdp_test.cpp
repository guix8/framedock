// The browser panel's protocol pieces: WebSocket framing and handshake, SHA-1 / base64, DevTools messages (building
// and reading), the address bar, mapping laser points onto the page, and decoding screencast JPEGs.
#include "cdp.h"
#include "devtools_pipe.h"
#include "jpeg_frame.h"
#include "websocket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
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

std::string hex(const std::string& s) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (const char c : s) {
        out += d[(static_cast<unsigned char>(c) >> 4) & 15];
        out += d[static_cast<unsigned char>(c) & 15];
    }
    return out;
}

// A 16×8 JPEG made with libjpeg-turbo (quality 95, 4:4:4): red on the left half, blue on the right
const unsigned char kJpeg[] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02,
    0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04, 0x03, 0x02, 0x02, 0x02, 0x02, 0x05, 0x04,
    0x04, 0x03, 0x04, 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06, 0x07, 0x09, 0x08, 0x06,
    0x07, 0x09, 0x07, 0x06, 0x06, 0x08, 0x0b, 0x08, 0x09, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x06, 0x08,
    0x0b, 0x0c, 0x0b, 0x0a, 0x0c, 0x09, 0x0a, 0x0a, 0x0a, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x02, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x05, 0x03, 0x03, 0x05, 0x0a, 0x07, 0x06, 0x07, 0x0a, 0x0a, 0x0a, 0x0a,
    0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
    0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
    0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0xff, 0xc0,
    0x00, 0x11, 0x08, 0x00, 0x08, 0x00, 0x10, 0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11,
    0x01, 0xff, 0xc4, 0x00, 0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05,
    0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
    0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23,
    0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a,
    0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5,
    0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1,
    0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xc4, 0x00, 0x1f, 0x01, 0x00, 0x03,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x11, 0x00,
    0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77, 0x00,
    0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13,
    0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15,
    0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6,
    0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4,
    0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
    0xfa, 0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0xf9,
    0xde, 0xbf, 0x07, 0x3f, 0xd6, 0x83, 0xc9, 0xeb, 0xfd, 0xc4, 0x3f, 0xc3, 0xb3, 0xff, 0xd9,
};

void testHashes() {
    CHECK(hex(ws::sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hex(ws::sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex(ws::sha1(std::string(1000, 'a'))) == "291e9a6c66994949b57ba5e650361e98fc36b1ba");
    CHECK(ws::base64Encode("") == "");
    CHECK(ws::base64Encode("f") == "Zg==");
    CHECK(ws::base64Encode("fo") == "Zm8=");
    CHECK(ws::base64Encode("foo") == "Zm9v");
    CHECK(ws::base64Encode("foobar") == "Zm9vYmFy");
    std::string out;
    CHECK(ws::base64Decode("Zm9vYmFy", out) && out == "foobar");
    CHECK(ws::base64Decode("Zm8=", out) && out == "fo");
    CHECK(ws::base64Decode("Zg", out) && out == "f");  // padding is optional
    CHECK(!ws::base64Decode("Zm9v!", out));
    CHECK(!ws::base64Decode("Z", out));
    const std::string binary("\x00\xff\x10\x80\x7f", 5);
    CHECK(ws::base64Decode(ws::base64Encode(binary), out) && out == binary);
    // RFC 6455's example
    CHECK(ws::acceptFor("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

void testHandshake() {
    const std::string request = ws::handshakeRequest("127.0.0.1", 9222, "/devtools/browser/abc", "dGhlIHNhbXBsZSBub25jZQ==");
    CHECK(request.find("GET /devtools/browser/abc HTTP/1.1\r\n") == 0);
    CHECK(request.find("Host: 127.0.0.1:9222\r\n") != std::string::npos);
    CHECK(request.find("Sec-WebSocket-Version: 13\r\n") != std::string::npos);
    CHECK(request.find("Origin") == std::string::npos);  // no Origin: Chrome's DevTools accepts origin-less clients
    size_t consumed = 0;
    std::string path, response, error;
    CHECK(ws::parseHandshakeRequest(request, consumed, path, response) == ws::Handshake::Ok);
    CHECK(path == "/devtools/browser/abc");
    CHECK(consumed == request.size());
    CHECK(ws::parseHandshakeResponse(response + "rest", "dGhlIHNhbXBsZSBub25jZQ==", consumed, error) == ws::Handshake::Ok);
    CHECK(consumed == response.size());
    CHECK(ws::parseHandshakeResponse(response.substr(0, 20), "dGhlIHNhbXBsZSBub25jZQ==", consumed, error) ==
          ws::Handshake::Incomplete);
    CHECK(ws::parseHandshakeResponse(response, "another key", consumed, error) == ws::Handshake::Failed);
    CHECK(ws::parseHandshakeResponse("HTTP/1.1 404 Not Found\r\n\r\n", "k", consumed, error) == ws::Handshake::Failed);
    CHECK(error.find("404") != std::string::npos);
    // Header names in any case
    const std::string mixed = "HTTP/1.1 101 Switching Protocols\r\nupgrade: websocket\r\nconnection: upgrade\r\n"
                              "sec-websocket-accept:   s3pPLMBiTxaQ9kYGzzhZRbK+xOo=  \r\n\r\n";
    CHECK(ws::parseHandshakeResponse(mixed, "dGhlIHNhbXBsZSBub25jZQ==", consumed, error) == ws::Handshake::Ok);
}

void testFrames() {
    const uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    // RFC 6455 5.7: a masked "Hello"
    const std::string masked = ws::encodeFrame(ws::Text, "Hello", mask);
    CHECK(hex(masked) == "818537fa213d7f9f4d5158");
    CHECK(hex(ws::encodeFrame(ws::Text, "Hello", nullptr)) == "810548656c6c6f");
    std::vector<ws::FrameParser::Message> out;
    ws::FrameParser p;
    CHECK(p.feed(masked.data(), masked.size(), out));
    CHECK(out.size() == 1 && out[0].opcode == ws::Text && out[0].payload == "Hello");
    // Byte by byte, and the 16-bit and 64-bit lengths
    for (const size_t n : {size_t(125), size_t(126), size_t(65535), size_t(65536), size_t(200000)}) {
        const std::string payload(n, 'x');
        const std::string frame = ws::encodeFrame(ws::Text, payload, n % 2 ? mask : nullptr);
        CHECK(frame.size() == n + (n < 126 ? 2 : n < 65536 ? 4 : 10) + (n % 2 ? 4 : 0));
        ws::FrameParser q;
        std::vector<ws::FrameParser::Message> got;
        size_t at = 0;
        while (at < frame.size()) {
            const size_t step = std::min<size_t>(frame.size() - at, at < 20 ? 1 : 7000);
            CHECK(q.feed(frame.data() + at, step, got));
            at += step;
        }
        CHECK(got.size() == 1 && got[0].payload == payload);
    }
    // Fragments: "Hel" + "lo" with a ping between them
    std::string fragmented;
    fragmented += std::string("\x01\x03", 2) + "Hel";
    fragmented += ws::encodeFrame(ws::Ping, "p", nullptr);
    fragmented += std::string("\x80\x02", 2) + "lo";
    ws::FrameParser f;
    out.clear();
    CHECK(f.feed(fragmented.data(), fragmented.size(), out));
    CHECK(out.size() == 2);
    if (out.size() == 2) {
        CHECK(out[0].opcode == ws::Ping && out[0].payload == "p");
        CHECK(out[1].opcode == ws::Text && out[1].payload == "Hello");
    }
    // Two messages in one read
    const std::string two = ws::encodeFrame(ws::Text, "a", nullptr) + ws::encodeFrame(ws::Text, "bc", nullptr);
    ws::FrameParser t;
    out.clear();
    CHECK(t.feed(two.data(), two.size(), out) && out.size() == 2 && out[1].payload == "bc");
    // Errors
    ws::FrameParser bad;
    const std::string reserved("\xC1\x00", 2);
    CHECK(!bad.feed(reserved.data(), reserved.size(), out));
    ws::FrameParser cont;
    const std::string orphan("\x80\x01x", 3);
    CHECK(!cont.feed(orphan.data(), orphan.size(), out));
    ws::FrameParser huge;
    std::string tooBig("\x81\x7f", 2);
    for (int i = 0; i < 8; ++i) tooBig += static_cast<char>(i == 3 ? 0x10 : 0);  // 2^40 bytes
    CHECK(!huge.feed(tooBig.data(), tooBig.size(), out));
    ws::FrameParser longPing;
    const std::string ping = ws::encodeFrame(ws::Ping, std::string(126, 'p'), nullptr);
    CHECK(!longPing.feed(ping.data(), ping.size(), out));
}

void testMessages() {
    cdp::Writer w;
    int id = 0;
    CHECK(w.command("Page.navigate", cdp::navigateParams("https://a.example/?q=\"x\""), "S1", id) ==
          "{\"id\":1,\"method\":\"Page.navigate\",\"params\":{\"url\":\"https://a.example/?q=\\\"x\\\"\"},\"sessionId\":\"S1\"}");
    CHECK(id == 1);
    CHECK(w.command("Target.getTargets", "", "", id) == "{\"id\":2,\"method\":\"Target.getTargets\",\"params\":{}}");
    CHECK(id == 2 && w.lastId() == 2);

    cdp::Message r = cdp::parse("{\"id\":7,\"result\":{\"sessionId\":\"AB\"}}");
    CHECK(r.kind == cdp::Message::Kind::Response && r.id == 7 && !r.failed);
    CHECK(r.body.get("sessionId") != nullptr && r.body.get("sessionId")->text == "AB");
    r = cdp::parse("{\"id\":8,\"error\":{\"code\":-32000,\"message\":\"No target\"}}");
    CHECK(r.kind == cdp::Message::Kind::Response && r.failed && r.errorMessage == "No target");
    cdp::Message e = cdp::parse("{\"method\":\"Page.loadEventFired\",\"params\":{\"timestamp\":1.5},\"sessionId\":\"S9\"}");
    CHECK(e.kind == cdp::Message::Kind::Event && e.method == "Page.loadEventFired" && e.sessionId == "S9");
    CHECK(cdp::parse("not json").kind == cdp::Message::Kind::Invalid);
    CHECK(cdp::parse("[1,2]").kind == cdp::Message::Kind::Invalid);
    CHECK(cdp::parse("{\"x\":1}").kind == cdp::Message::Kind::Invalid);

    CHECK(cdp::number(1) == "1");
    CHECK(cdp::number(12.5) == "12.5");
    CHECK(cdp::number(-0.0001) == "0");
    CHECK(cdp::number(1.23456) == "1.235");
    CHECK(cdp::number(1.0 / 0.0) == "0");
    CHECK(cdp::mouseParams("mousePressed", 10.25, 20, cdp::Button::Left, 1, 1) ==
          "{\"type\":\"mousePressed\",\"x\":10.25,\"y\":20,\"button\":\"left\",\"buttons\":1,\"clickCount\":1}");
    CHECK(cdp::wheelParams(1, 2, 0, -120) == "{\"type\":\"mouseWheel\",\"x\":1,\"y\":2,\"deltaX\":0,\"deltaY\":-120}");
    CHECK(cdp::insertTextParams("한글 \"x\"") == "{\"text\":\"한글 \\\"x\\\"\"}");
    CHECK(cdp::screencastParams(1280, 720, 70) ==
          "{\"format\":\"jpeg\",\"quality\":70,\"maxWidth\":1280,\"maxHeight\":720,\"everyNthFrame\":1}");
    CHECK(cdp::screencastAckParams(42) == "{\"sessionId\":42}");
    CHECK(cdp::metricsParams(1280, 720) == "{\"width\":1280,\"height\":720,\"deviceScaleFactor\":1,\"mobile\":false}");
    CHECK(cdp::attachParams("T1") == "{\"targetId\":\"T1\",\"flatten\":true}");
    // Keys: Enter types "\r" (a keypress, so forms submit); Backspace is a raw key; Ctrl+A selects all
    const std::string enter = cdp::keyParams(cdp::Key::Enter, true);
    CHECK(enter.find("\"type\":\"keyDown\"") != std::string::npos);
    CHECK(enter.find("\"windowsVirtualKeyCode\":13") != std::string::npos);
    CHECK(enter.find("\"text\":\"\\u000d\"") != std::string::npos);  // "\r", escaped as JSON
    CHECK(cdp::keyParams(cdp::Key::Enter, false).find("\"type\":\"keyUp\"") != std::string::npos);
    CHECK(cdp::keyParams(cdp::Key::Enter, false).find("\"text\"") == std::string::npos);
    const std::string back = cdp::keyParams(cdp::Key::Backspace, true);
    CHECK(back.find("\"type\":\"rawKeyDown\",\"key\":\"Backspace\",\"code\":\"Backspace\",\"windowsVirtualKeyCode\":8") !=
          std::string::npos);
    const std::string all = cdp::keyParams(cdp::Key::SelectAll, true);
    CHECK(all.find("\"modifiers\":2") != std::string::npos && all.find("\"commands\":[\"selectAll\"]") != std::string::npos);
    CHECK(cdp::keyParams(cdp::Key::Left, true).find("\"key\":\"ArrowLeft\"") != std::string::npos);
    // The context menu's 복사 / 붙여넣기: Ctrl+C / Ctrl+V with their editing commands (on the key down only)
    const std::string copy = cdp::keyParams(cdp::Key::Copy, true);
    CHECK(copy.find("\"key\":\"c\",\"code\":\"KeyC\",\"windowsVirtualKeyCode\":67") != std::string::npos);
    CHECK(copy.find("\"modifiers\":2") != std::string::npos && copy.find("\"commands\":[\"copy\"]") != std::string::npos);
    const std::string paste = cdp::keyParams(cdp::Key::Paste, true);
    CHECK(paste.find("\"code\":\"KeyV\"") != std::string::npos && paste.find("\"commands\":[\"paste\"]") != std::string::npos);
    CHECK(cdp::keyParams(cdp::Key::Paste, false).find("commands") == std::string::npos);
}

void testResults() {
    int port = 0;
    std::string path;
    CHECK(cdp::parseActivePort("38451\n/devtools/browser/9b1f-aa\n", port, path));
    CHECK(port == 38451 && path == "/devtools/browser/9b1f-aa");
    CHECK(!cdp::parseActivePort("", port, path));
    CHECK(!cdp::parseActivePort("38451", port, path));          // being written: no second line yet
    CHECK(!cdp::parseActivePort("0\n/devtools/browser/x\n", port, path));
    CHECK(!cdp::parseActivePort("abc\n/devtools/browser/x\n", port, path));
    CHECK(!cdp::parseActivePort("9222\n/json\n", port, path));

    JsonValue v;
    std::string error;
    CHECK(parseJson("{\"currentIndex\":1,\"entries\":[{\"id\":3,\"url\":\"a\",\"title\":\"A\"},"
                    "{\"id\":5,\"url\":\"b\",\"title\":\"B\"},{\"id\":9,\"url\":\"c\",\"title\":\"C\"}]}",
                    v, error));
    cdp::History h;
    CHECK(cdp::readHistory(v, h));
    CHECK(h.url == "b" && h.title == "B" && h.canBack() && h.canForward());
    CHECK(h.entryAt(-1) == 3 && h.entryAt(1) == 9 && h.entryAt(2) == -1);
    CHECK(parseJson("{\"currentIndex\":0,\"entries\":[{\"id\":3,\"url\":\"a\",\"title\":\"A\"}]}", v, error));
    CHECK(cdp::readHistory(v, h) && !h.canBack() && !h.canForward() && h.entryAt(-1) == -1);
    CHECK(parseJson("{\"result\":{\"type\":\"boolean\",\"value\":true}}", v, error));
    bool b = false;
    CHECK(cdp::readBoolResult(v, b) && b);
    CHECK(parseJson("{\"result\":{\"type\":\"undefined\"}}", v, error));
    CHECK(!cdp::readBoolResult(v, b));
    std::string str = "x";
    CHECK(!cdp::readStringResult(v, str));
    CHECK(parseJson("{\"result\":{\"type\":\"string\",\"value\":\"https://a.example/\"}}", v, error));
    CHECK(cdp::readStringResult(v, str) && str == "https://a.example/" && !cdp::readBoolResult(v, b));
    CHECK(cdp::linkAtScript(12.5, 300) ==
          "(() => { const e = document.elementFromPoint(12.5, 300); const a = e && e.closest('a[href]'); "
          "return a ? a.href : ''; })()");

    const std::string jpeg(reinterpret_cast<const char*>(kJpeg), sizeof(kJpeg));
    CHECK(parseJson("{\"data\":\"" + ws::base64Encode(jpeg) +
                        "\",\"metadata\":{\"offsetTop\":0,\"pageScaleFactor\":1,\"deviceWidth\":1280,\"deviceHeight\":720},"
                        "\"sessionId\":17}",
                    v, error));
    cdp::ScreencastFrame f;
    CHECK(cdp::readScreencastFrame(v, f));
    CHECK(f.sessionId == 17 && f.jpeg == jpeg && f.deviceWidth == 1280 && f.deviceHeight == 720);
    CHECK(parseJson("{\"data\":\"***\",\"sessionId\":1}", v, error));
    CHECK(!cdp::readScreencastFrame(v, f));
}

void testAddress() {
    CHECK(cdp::addressToUrl("https://www.youtube.com/watch?v=x") == "https://www.youtube.com/watch?v=x");
    CHECK(cdp::addressToUrl("  youtube.com  ") == "https://youtube.com");
    CHECK(cdp::addressToUrl("m.naver.com/news") == "https://m.naver.com/news");
    CHECK(cdp::addressToUrl("localhost:8080") == "https://localhost:8080");
    CHECK(cdp::addressToUrl("192.168.0.10") == "https://192.168.0.10");
    CHECK(cdp::addressToUrl("about:blank") == "about:blank");
    CHECK(cdp::addressToUrl("chrome://gpu") == "chrome://gpu");
    CHECK(cdp::addressToUrl("고양이") == "https://www.google.com/search?q=%EA%B3%A0%EC%96%91%EC%9D%B4");
    CHECK(cdp::addressToUrl("steam frame review") == "https://www.google.com/search?q=steam%20frame%20review");
    CHECK(cdp::addressToUrl("node.js tutorial") == "https://www.google.com/search?q=node.js%20tutorial");
    CHECK(cdp::addressToUrl("v1.2") == "https://www.google.com/search?q=v1.2");  // a version, not a host
    CHECK(cdp::addressToUrl("a&b=c?") == "https://www.google.com/search?q=a%26b%3Dc%3F");
    CHECK(cdp::addressToUrl("   ").empty());
    CHECK(cdp::urlEncode("a b-_.~") == "a%20b-_.~");
}

void testViewMap() {
    cdp::ViewMap m;
    m.areaX = 0;
    m.areaY = 88;
    m.areaW = 1280;
    m.areaH = 720;
    m.viewportW = 1280;
    m.viewportH = 720;
    double x = 0, y = 0;
    CHECK(m.toPage(640, 448, x, y) && x == 640 && y == 360);
    CHECK(m.toPage(0, 88, x, y) && x == 0 && y == 0);
    CHECK(m.toPage(1280, 808, x, y) && x == 1279 && y == 719);  // the far edge stays inside the page
    CHECK(!m.toPage(640, 80, x, y));                             // the bar
    CHECK(!m.toPage(-1, 300, x, y));
    // A page area drawn at another size than the viewport (a smaller texture): scaled
    m.areaW = 960;
    m.areaH = 540;
    m.areaX = 20;
    m.areaY = 100;
    CHECK(m.toPage(20 + 480, 100 + 270, x, y) && std::abs(x - 640) < 1e-9 && std::abs(y - 360) < 1e-9);
    CHECK(m.toPage(20 + 96, 100 + 54, x, y) && std::abs(x - 128) < 1e-9 && std::abs(y - 72) < 1e-9);
}

void testJpeg() {
    JpegDecoder decoder;
    BgraFrame frame;
    std::string error;
    CHECK(decoder.decode(kJpeg, sizeof(kJpeg), frame, error));
    CHECK(frame.width == 16 && frame.height == 8 && frame.pixels.size() == 16 * 8 * 4);
    CHECK(frame.sequence == 1);
    if (frame.width == 16) {
        const uint8_t* red = &frame.pixels[(4 * 16 + 2) * 4];    // BGRA
        const uint8_t* blue = &frame.pixels[(4 * 16 + 13) * 4];
        CHECK(red[2] > 190 && red[1] < 80 && red[0] < 80 && red[3] == 255);
        CHECK(blue[0] > 190 && blue[2] < 80 && blue[3] == 255);
    }
    // The same buffer again, the sequence counts on
    const uint8_t* before = frame.pixels.data();
    CHECK(decoder.decode(kJpeg, sizeof(kJpeg), frame, error));
    CHECK(frame.pixels.data() == before && frame.sequence == 2);
    // Round trip through the encoder (the fake Chrome's frames)
    BgraFrame big;
    big.width = 320;
    big.height = 180;
    big.pixels.assign(320 * 180 * 4, 0);
    for (size_t i = 0; i < big.pixels.size(); i += 4) {
        big.pixels[i] = 30;
        big.pixels[i + 1] = 160;
        big.pixels[i + 2] = 90;
        big.pixels[i + 3] = 255;
    }
    std::string jpeg;
    CHECK(JpegDecoder::encode(big, 80, jpeg));
    BgraFrame back;
    CHECK(decoder.decode(reinterpret_cast<const uint8_t*>(jpeg.data()), jpeg.size(), back, error));
    CHECK(back.width == 320 && back.height == 180);
    if (back.width == 320) CHECK(std::abs(back.pixels[(90 * 320 + 160) * 4 + 1] - 160) < 6);
    // Too large for the limit, cut short, not a JPEG
    CHECK(!decoder.decode(reinterpret_cast<const uint8_t*>(jpeg.data()), jpeg.size(), back, error, 100, 100));
    CHECK(error.find("out of range") != std::string::npos);
    const std::string garbage = "not a jpeg at all";
    CHECK(!decoder.decode(reinterpret_cast<const uint8_t*>(garbage.data()), garbage.size(), back, error));
    CHECK(!decoder.decode(kJpeg, 100, back, error));  // cut short: dropped, not shown half drawn
    CHECK(!decoder.decode(kJpeg + 2, sizeof(kJpeg) - 2, back, error));
}


/** The server side of one loopback connection (a listening socket, then the accepted client). */
struct LoopbackServer {
    int listener = -1;
    int conn = -1;
    int port = 0;
    LoopbackServer() {
        listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listener, 1) != 0) return;
        socklen_t len = sizeof(addr);
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
    }
    ~LoopbackServer() {
        if (conn >= 0) ::close(conn);
        if (listener >= 0) ::close(listener);
    }
    /** Accepts the client and reads its upgrade request. @return the 101 answer to send, "" on failure */
    std::string acceptHandshake(ws::Client& client) {
        pollfd p {listener, POLLIN, 0};
        for (int i = 0; i < 100 && conn < 0; ++i) {
            client.onWritable();  // finishes the connect, sends the request
            if (::poll(&p, 1, 20) > 0) conn = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        }
        if (conn < 0) return {};
        std::string in, path, response;
        size_t consumed = 0;
        for (int i = 0; i < 100; ++i) {
            client.onWritable();
            pollfd c {conn, POLLIN, 0};
            if (::poll(&c, 1, 20) <= 0) continue;
            char buf[4096];
            const ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) return {};
            in.append(buf, static_cast<size_t>(n));
            const ws::Handshake h = ws::parseHandshakeRequest(in, consumed, path, response);
            if (h == ws::Handshake::Ok) return response;
            if (h == ws::Handshake::Failed) return {};
        }
        return {};
    }
    /** @return the frames the client sent within timeoutMs */
    std::vector<ws::FrameParser::Message> readFrames(int timeoutMs) {
        std::vector<ws::FrameParser::Message> out;
        ws::FrameParser parser;
        pollfd c {conn, POLLIN, 0};
        while (::poll(&c, 1, timeoutMs) > 0) {
            char buf[4096];
            const ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) break;
            parser.feed(buf, static_cast<size_t>(n), out);
            timeoutMs = 50;
        }
        return out;
    }
};

/** Waits until the client's socket is readable, then reads. */
void readClient(ws::Client& client, std::vector<std::string>& messages) {
    pollfd p {client.fd(), POLLIN, 0};
    if (::poll(&p, 1, 1000) > 0) client.onReadable(messages);
}

void testClientControlFramesWithHandshake() {
    // A ping and a text frame in the same segment as the 101 answer: the text comes out, the ping is answered
    {
        LoopbackServer server;
        CHECK(server.port > 0);
        ws::Client client;
        CHECK(client.connect(server.port, "/devtools/browser/x"));
        const std::string response = server.acceptHandshake(client);
        CHECK(!response.empty());
        const std::string burst = response + ws::encodeFrame(ws::Ping, "hello", nullptr) +
                                  ws::encodeFrame(ws::Text, "{\"method\":\"A\"}", nullptr);
        CHECK(::send(server.conn, burst.data(), burst.size(), 0) == static_cast<ssize_t>(burst.size()));
        std::vector<std::string> messages;
        readClient(client, messages);
        CHECK(client.open());
        CHECK(messages.size() == 1 && messages[0] == "{\"method\":\"A\"}");
        const std::vector<ws::FrameParser::Message> back = server.readFrames(1000);
        CHECK(back.size() == 1 && back[0].opcode == ws::Pong && back[0].payload == "hello");
    }
    // A close right behind the answer: the connection ends (and the close is echoed), not left open
    {
        LoopbackServer server;
        ws::Client client;
        CHECK(client.connect(server.port, "/x"));
        const std::string response = server.acceptHandshake(client);
        CHECK(!response.empty());
        const std::string burst = response + ws::encodeFrame(ws::Close, std::string("\x03\xe8", 2), nullptr);
        CHECK(::send(server.conn, burst.data(), burst.size(), 0) == static_cast<ssize_t>(burst.size()));
        std::vector<std::string> messages;
        readClient(client, messages);
        CHECK(client.state() == ws::Client::State::Failed);
        CHECK(client.error() == "the other side sent close");
        const std::vector<ws::FrameParser::Message> back = server.readFrames(1000);
        CHECK(back.size() == 1 && back[0].opcode == ws::Close);
    }
}

}  // namespace

double clockNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** The old decoder, bit by bit: the reference for the 4→3 block decoder */
bool referenceBase64(const std::string& data, std::string& out) {
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t size = data.size();
    while (size > 0 && data[size - 1] == '=') --size;
    if (size % 4 == 1) return false;
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < size; ++i) {
        const size_t v = alphabet.find(data[i]);
        if (v == std::string::npos) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return true;
}

void testFastDecoders() {
    // base64: the block decoder against the reference on every length and byte, and its errors
    std::string binary;
    for (int i = 0; i < 1000; ++i) binary += static_cast<char>((i * 7919 + 13) & 0xFF);
    for (size_t n = 0; n <= 64; ++n) {
        const std::string in = binary.substr(0, n);
        const std::string enc = ws::base64Encode(in);
        std::string a, b;
        CHECK(ws::base64Decode(enc, a) && a == in);
        CHECK(referenceBase64(enc, b) && a == b);
        std::string unpadded = enc;
        while (!unpadded.empty() && unpadded.back() == '=') unpadded.pop_back();
        CHECK(ws::base64Decode(unpadded, a) && a == in);
    }
    std::string out = "kept?";
    CHECK(!ws::base64Decode("Zm9v\"Zm9v", out));
    CHECK(!ws::base64Decode("Zm9vYmF\x80", out));
    CHECK(!ws::base64Decode("Zm9vY", out));
    CHECK(!ws::base64Decode("Zm9-", out));  // URL-safe characters aren't standard base64
    // A frame-sized buffer, decoded fast enough (a few ms at most even in a Debug + sanitizer build)
    std::string big;
    while (big.size() < (900u << 10)) big += binary;
    const std::string bigEncoded = ws::base64Encode(big);
    const double t0 = clockNow();
    for (int i = 0; i < 20; ++i) CHECK(ws::base64Decode(bigEncoded, out));
    const double perFrameMs = (clockNow() - t0) / 20 * 1e3;
    std::printf("base64: %.2f ms per 900 KB frame\n", perFrameMs);
    CHECK(out == big);
    CHECK(perFrameMs < 50);

    // The screencast message without the full JSON parse: the same frame as readScreencastFrame
    const std::string jpeg(reinterpret_cast<const char*>(kJpeg), sizeof(kJpeg));
    const std::string message = "{\"method\":\"Page.screencastFrame\",\"params\":{\"data\":\"" + ws::base64Encode(jpeg) +
                                "\",\"metadata\":{\"offsetTop\":12,\"pageScaleFactor\":1.5,\"deviceWidth\":1280,"
                                "\"deviceHeight\":720,\"scrollOffsetX\":0,\"scrollOffsetY\":0},\"sessionId\":17},"
                                "\"sessionId\":\"S-T1.1\"}";
    cdp::ScreencastFrame fast;
    std::string session;
    CHECK(cdp::readScreencastMessage(message, fast, session) == cdp::FastFrame::Frame);
    CHECK(session == "S-T1.1" && fast.sessionId == 17 && fast.jpeg == jpeg);
    CHECK(fast.deviceWidth == 1280 && fast.deviceHeight == 720 && fast.offsetTop == 12 && fast.pageScale == 1.5);
    const cdp::Message parsed = cdp::parse(message);
    cdp::ScreencastFrame slow;
    CHECK(cdp::readScreencastFrame(parsed.body, slow) && slow.jpeg == fast.jpeg && slow.sessionId == fast.sessionId &&
          parsed.sessionId == session);
    // Params in another order, spaces, no session at the top: still read
    const std::string other = "{\"method\":\"Page.screencastFrame\",\"params\": { \"sessionId\" : 3 , \"metadata\":{},"
                              "\"data\" : \"" + ws::base64Encode(jpeg) + "\"}}";
    CHECK(cdp::readScreencastMessage(other, fast, session) == cdp::FastFrame::Frame);
    CHECK(fast.sessionId == 3 && session.empty() && fast.jpeg == jpeg && fast.deviceWidth == 0);
    // Not a frame, or a frame this reader leaves to the full parser
    CHECK(cdp::readScreencastMessage("{\"id\":3,\"result\":{}}", fast, session) == cdp::FastFrame::NotAFrame);
    CHECK(cdp::readScreencastMessage("{\"method\":\"Page.frameNavigated\",\"params\":{}}", fast, session) ==
          cdp::FastFrame::NotAFrame);
    CHECK(cdp::readScreencastMessage("{\"params\":{\"data\":\"AAAA\",\"sessionId\":1},\"method\":\"Page.screencastFrame\"}",
                                     fast, session) == cdp::FastFrame::Fallback);
    CHECK(cdp::readScreencastMessage("{\"method\":\"Page.screencastFrame\",\"params\":{\"data\":\"AA\\/A\",\"sessionId\":1}}",
                                     fast, session) == cdp::FastFrame::Fallback);  // an escape in the data
    CHECK(cdp::readScreencastMessage("{\"method\":\"Page.screencastFrame\",\"params\":{\"data\":\"***\",\"sessionId\":1}}",
                                     fast, session) == cdp::FastFrame::Fallback);  // not base64
    CHECK(cdp::readScreencastMessage("{\"method\":\"Page.screencastFrame\",\"params\":{\"data\":\"AAAA", fast, session) ==
          cdp::FastFrame::Fallback);  // cut short
    // Fast and full paths side by side on a frame-sized message
    const std::string bigMessage = "{\"method\":\"Page.screencastFrame\",\"params\":{\"data\":\"" + bigEncoded +
                                   "\",\"metadata\":{\"deviceWidth\":1280},\"sessionId\":5},\"sessionId\":\"S\"}";
    // The point of the fast path: it must be clearly faster. Best of 10 runs each (noise only ever adds time); it
    // measured about 7 times faster, so asking for twice is a generous margin
    double bestFast = 1e9, bestFull = 1e9;
    for (int i = 0; i < 10; ++i) {
        const double f0 = clockNow();
        CHECK(cdp::readScreencastMessage(bigMessage, fast, session) == cdp::FastFrame::Frame);
        const double f1 = clockNow();
        const cdp::Message m = cdp::parse(bigMessage);
        CHECK(cdp::readScreencastFrame(m.body, slow));
        const double f2 = clockNow();
        bestFast = std::min(bestFast, f1 - f0);
        bestFull = std::min(bestFull, f2 - f1);
    }
    std::printf("screencast message: %.2f ms targeted, %.2f ms full parse (best of 10)\n", bestFast * 1e3,
                bestFull * 1e3);
    CHECK(bestFast * 2 < bestFull);
    CHECK(fast.jpeg == big && slow.jpeg == big);
    // A frame session id no int holds (a cast would be undefined): the fast path leaves it to the full parser, which
    // drops the frame
    for (const char* id : {"1e20", "-1e20", "1e999", "2147483648", "17x"}) {
        const std::string odd = "{\"method\":\"Page.screencastFrame\",\"params\":{\"data\":\"" +
                                ws::base64Encode(jpeg) + "\",\"sessionId\":" + id + "}}";
        CHECK(cdp::readScreencastMessage(odd, fast, session) == cdp::FastFrame::Fallback);
        const cdp::Message m = cdp::parse(odd);
        CHECK(!cdp::readScreencastFrame(m.body, slow));
    }
    {
        const std::string edge = "{\"method\":\"Page.screencastFrame\",\"params\":{\"data\":\"" +
                                 ws::base64Encode(jpeg) + "\",\"sessionId\":2147483647}}";
        CHECK(cdp::readScreencastMessage(edge, fast, session) == cdp::FastFrame::Frame && fast.sessionId == 2147483647);
    }

    // The pipe's parser: a large message in many small reads is searched once, not from its start each read
    devtools::PipeParser parser;
    std::vector<std::string> messages;
    const std::string payload(16u << 20, 'x');
    const double p0 = clockNow();
    for (size_t at = 0; at < payload.size(); at += 4096) {
        CHECK(parser.feed(payload.data() + at, std::min<size_t>(4096, payload.size() - at), messages));
    }
    CHECK(parser.feed("\0{\"a\":1}\0{\"b\"", 13, messages));
    CHECK(parser.feed(":2}\0", 4, messages));
    const double pipeSec = clockNow() - p0;
    std::printf("pipe: 16 MB message in 4 KB reads: %.0f ms\n", pipeSec * 1e3);
    CHECK(pipeSec < 2.0);
    CHECK(messages.size() == 3 && messages[0].size() == payload.size() && messages[1] == "{\"a\":1}" &&
          messages[2] == "{\"b\":2}");
}

int main() {
    testHashes();
    testHandshake();
    testFrames();
    testMessages();
    testResults();
    testAddress();
    testViewMap();
    testJpeg();
    testClientControlFramesWithHandshake();
    testFastDecoders();
    if (failures == 0) std::printf("cdp: all passed\n");
    return failures == 0 ? 0 : 1;
}
