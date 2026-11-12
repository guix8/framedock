// The WebSocket client. See websocket.h.
#include "websocket.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cctype>
#include <cstring>
#include <random>

namespace ws {
namespace {

constexpr const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

/** @return the header's value (case-insensitive name), "" if it isn't there */
std::string header(const std::string& head, const char* name) {
    const size_t n = std::strlen(name);
    size_t pos = 0;
    while ((pos = head.find("\r\n", pos)) != std::string::npos) {
        pos += 2;
        if (head.size() - pos < n + 1) break;
        bool same = true;
        for (size_t i = 0; i < n && same; ++i) {
            same = std::tolower(static_cast<unsigned char>(head[pos + i])) == std::tolower(static_cast<unsigned char>(name[i]));
        }
        if (!same || head[pos + n] != ':') continue;
        size_t start = pos + n + 1;
        const size_t end = head.find("\r\n", start);
        while (start < end && (head[start] == ' ' || head[start] == '\t')) ++start;
        size_t stop = end;
        while (stop > start && (head[stop - 1] == ' ' || head[stop - 1] == '\t')) --stop;
        return head.substr(start, stop - start);
    }
    return {};
}

bool containsToken(const std::string& value, const char* token) {
    std::string lower;
    for (const char c : value) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.find(token) != std::string::npos;
}

}  // namespace

std::string sha1(const std::string& data) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string msg = data;
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    msg += static_cast<char>(0x80);
    while (msg.size() % 64 != 56) msg += '\0';
    for (int i = 7; i >= 0; --i) msg += static_cast<char>((bits >> (i * 8)) & 0xFF);
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const auto* p = reinterpret_cast<const unsigned char*>(msg.data() + chunk + i * 4);
            w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::string out;
    for (const uint32_t v : h) {
        for (int i = 3; i >= 0; --i) out += static_cast<char>((v >> (i * 8)) & 0xFF);
    }
    return out;
}

std::string base64Encode(const std::string& data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        const uint32_t v = (uint32_t(uint8_t(data[i])) << 16) | (uint32_t(uint8_t(data[i + 1])) << 8) | uint8_t(data[i + 2]);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
        i += 3;
    }
    if (i < data.size()) {
        uint32_t v = uint32_t(uint8_t(data[i])) << 16;
        if (i + 1 < data.size()) v |= uint32_t(uint8_t(data[i + 1])) << 8;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += i + 1 < data.size() ? kAlphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

namespace {

/** The decoding table: 0..63, 0xFF for anything else. Built at compile time (no first-use race between threads) */
struct Base64Table {
    uint8_t v[256];
    constexpr Base64Table() : v() {
        for (int i = 0; i < 256; ++i) v[i] = 0xFF;
        for (int i = 0; i < 26; ++i) {
            v['A' + i] = static_cast<uint8_t>(i);
            v['a' + i] = static_cast<uint8_t>(26 + i);
        }
        for (int i = 0; i < 10; ++i) v['0' + i] = static_cast<uint8_t>(52 + i);
        v[static_cast<unsigned char>('+')] = 62;
        v[static_cast<unsigned char>('/')] = 63;
    }
};
constexpr Base64Table kBase64;

}  // namespace

bool base64Decode(const char* data, size_t size, std::string& out) {
    while (size > 0 && data[size - 1] == '=') --size;
    if (size % 4 == 1) return false;
    // Four characters to three bytes at a time, into a buffer sized once (a screencast frame is ~100 KB-1 MB)
    const size_t full = size / 4;
    const size_t rest = size % 4;
    out.resize(full * 3 + (rest == 0 ? 0 : rest - 1));
    char* o = &out[0];
    const auto* in = reinterpret_cast<const unsigned char*>(data);
    const uint8_t* v = kBase64.v;
    for (size_t q = 0; q < full; ++q, in += 4, o += 3) {
        const uint32_t a = v[in[0]], b = v[in[1]], c = v[in[2]], d = v[in[3]];
        if ((a | b | c | d) & 0x80) {
            out.clear();
            return false;
        }
        const uint32_t n = (a << 18) | (b << 12) | (c << 6) | d;
        o[0] = static_cast<char>(n >> 16);
        o[1] = static_cast<char>((n >> 8) & 0xFF);
        o[2] = static_cast<char>(n & 0xFF);
    }
    if (rest > 0) {
        uint32_t n = 0;
        for (size_t j = 0; j < rest; ++j) {
            const uint32_t x = v[in[j]];
            if (x & 0x80) {
                out.clear();
                return false;
            }
            n |= x << (18 - 6 * j);
        }
        o[0] = static_cast<char>(n >> 16);
        if (rest == 3) o[1] = static_cast<char>((n >> 8) & 0xFF);
    }
    return true;
}

std::string acceptFor(const std::string& key) { return base64Encode(sha1(key + kGuid)); }

std::string handshakeRequest(const std::string& host, int port, const std::string& path, const std::string& key) {
    return "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
           "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
           "\r\nSec-WebSocket-Version: 13\r\n\r\n";
}

Handshake parseHandshakeResponse(const std::string& buffer, const std::string& key, size_t& consumed,
                                 std::string& error) {
    const size_t end = buffer.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (buffer.size() > 16384) {
            error = "the handshake answer is too long";
            return Handshake::Failed;
        }
        return Handshake::Incomplete;
    }
    const std::string head = buffer.substr(0, end + 2);
    const size_t lineEnd = head.find("\r\n");
    const std::string status = head.substr(0, lineEnd);
    if (status.compare(0, 9, "HTTP/1.1 ") != 0 || status.compare(9, 3, "101") != 0) {
        error = "not upgraded: " + status;
        return Handshake::Failed;
    }
    if (!containsToken(header(head, "Upgrade"), "websocket")) {
        error = "no Upgrade: websocket";
        return Handshake::Failed;
    }
    if (header(head, "Sec-WebSocket-Accept") != acceptFor(key)) {
        error = "wrong Sec-WebSocket-Accept";
        return Handshake::Failed;
    }
    consumed = end + 4;
    return Handshake::Ok;
}

Handshake parseHandshakeRequest(const std::string& buffer, size_t& consumed, std::string& path, std::string& response) {
    const size_t end = buffer.find("\r\n\r\n");
    if (end == std::string::npos) return buffer.size() > 16384 ? Handshake::Failed : Handshake::Incomplete;
    const std::string head = buffer.substr(0, end + 2);
    if (head.compare(0, 4, "GET ") != 0) return Handshake::Failed;
    const size_t pathEnd = head.find(' ', 4);
    if (pathEnd == std::string::npos) return Handshake::Failed;
    path = head.substr(4, pathEnd - 4);
    const std::string key = header(head, "Sec-WebSocket-Key");
    if (key.empty() || !containsToken(header(head, "Upgrade"), "websocket")) return Handshake::Failed;
    response = "HTTP/1.1 101 WebSocket Protocol Handshake\r\nUpgrade: WebSocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Accept: " + acceptFor(key) + "\r\n\r\n";
    consumed = end + 4;
    return Handshake::Ok;
}

std::string encodeFrame(Opcode opcode, const std::string& payload, const uint8_t* mask) {
    std::string out;
    out.reserve(payload.size() + 14);
    out += static_cast<char>(0x80 | opcode);
    const uint8_t maskBit = mask != nullptr ? 0x80 : 0;
    const uint64_t n = payload.size();
    if (n < 126) {
        out += static_cast<char>(maskBit | n);
    } else if (n < 65536) {
        out += static_cast<char>(maskBit | 126);
        out += static_cast<char>((n >> 8) & 0xFF);
        out += static_cast<char>(n & 0xFF);
    } else {
        out += static_cast<char>(maskBit | 127);
        for (int i = 7; i >= 0; --i) out += static_cast<char>((n >> (i * 8)) & 0xFF);
    }
    if (mask == nullptr) return out + payload;
    out.append(reinterpret_cast<const char*>(mask), 4);
    const size_t head = out.size();
    out += payload;
    for (size_t i = 0; i < payload.size(); ++i) out[head + i] = static_cast<char>(out[head + i] ^ mask[i & 3]);
    return out;
}

bool FrameParser::feed(const char* data, size_t size, std::vector<Message>& out) {
    if (!error_.empty()) return false;
    buffer_.append(data, size);
    for (;;) {
        const size_t avail = buffer_.size() - start_;
        if (avail < 2) break;
        const auto* p = reinterpret_cast<const unsigned char*>(buffer_.data() + start_);
        const bool fin = (p[0] & 0x80) != 0;
        if ((p[0] & 0x70) != 0) {
            error_ = "reserved bits set (no extension was agreed)";
            return false;
        }
        const Opcode opcode = static_cast<Opcode>(p[0] & 0x0F);
        const bool masked = (p[1] & 0x80) != 0;
        uint64_t len = p[1] & 0x7F;
        size_t head = 2;
        if (len == 126) {
            if (avail < 4) break;
            len = (uint64_t(p[2]) << 8) | p[3];
            head = 4;
        } else if (len == 127) {
            if (avail < 10) break;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
            head = 10;
        }
        if (len > kMaxMessage) {
            error_ = "a frame larger than " + std::to_string(kMaxMessage) + " bytes";
            return false;
        }
        const size_t maskAt = head;
        if (masked) head += 4;
        if (avail < head + len) break;
        std::string payload(buffer_.data() + start_ + head, static_cast<size_t>(len));
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ p[maskAt + (i & 3)]);
        }
        start_ += head + static_cast<size_t>(len);
        const bool control = (opcode & 0x8) != 0;
        if (control) {
            if (!fin || len > 125) {
                error_ = "a fragmented or long control frame";
                return false;
            }
            out.push_back({opcode, std::move(payload)});
            continue;
        }
        if (opcode == Continuation) {
            if (!inFragment_) {
                error_ = "a continuation without a start";
                return false;
            }
            if (partial_.size() + payload.size() > kMaxMessage) {
                error_ = "a message larger than " + std::to_string(kMaxMessage) + " bytes";
                return false;
            }
            partial_ += payload;
            if (fin) {
                out.push_back({partialOpcode_, std::move(partial_)});
                partial_.clear();
                inFragment_ = false;
            }
            continue;
        }
        if (opcode != Text && opcode != Binary) {
            error_ = "unknown opcode " + std::to_string(opcode);
            return false;
        }
        if (inFragment_) {
            error_ = "a new message inside a fragmented one";
            return false;
        }
        if (fin) {
            out.push_back({opcode, std::move(payload)});
        } else {
            inFragment_ = true;
            partialOpcode_ = opcode;
            partial_ = std::move(payload);
        }
    }
    // Compact once the parsed part is large (not on every frame: no quadratic copying)
    if (start_ > 0 && (start_ == buffer_.size() || start_ > (1u << 20))) {
        buffer_.erase(0, start_);
        start_ = 0;
    }
    return true;
}

Client::~Client() { close(); }

void Client::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    if (state_ != State::Failed) state_ = State::Closed;
    out_.clear();
    outStart_ = 0;
    in_.clear();
    parser_ = FrameParser {};
}

void Client::fail(const std::string& why) {
    if (error_.empty() || state_ != State::Failed) error_ = why;
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    state_ = State::Failed;
    out_.clear();
    outStart_ = 0;
}

void Client::nextMask(uint8_t mask[4]) {
    // Masking only stops proxies from misreading frames; loopback has none. A cheap xorshift is plenty
    if (maskState_ == 0) {
        maskState_ = static_cast<uint32_t>(std::random_device {}()) | 1u;
    }
    maskState_ ^= maskState_ << 13;
    maskState_ ^= maskState_ >> 17;
    maskState_ ^= maskState_ << 5;
    std::memcpy(mask, &maskState_, 4);
}

bool Client::connect(int port, const std::string& path) {
    close();
    error_.clear();
    port_ = port;
    path_ = path;
    fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        fail(std::string("socket: ") + std::strerror(errno));
        return false;
    }
    const int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));  // small input events go out at once
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::random_device rd;
    std::string raw(16, '\0');
    for (char& c : raw) c = static_cast<char>(rd() & 0xFF);
    key_ = base64Encode(raw);
    out_ = handshakeRequest("127.0.0.1", port, path, key_);
    outStart_ = 0;
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        state_ = State::Handshaking;
        flush();
        return true;
    }
    if (errno != EINPROGRESS) {
        fail(std::string("connect: ") + std::strerror(errno));
        return false;
    }
    state_ = State::Connecting;
    return true;
}

void Client::onWritable() {
    if (fd_ < 0) return;
    if (state_ == State::Connecting) {
        int err = 0;
        socklen_t len = sizeof(err);
        ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            fail(std::string("connect: ") + std::strerror(err));
            return;
        }
        state_ = State::Handshaking;
    }
    flush();
}

void Client::flush() {
    while (fd_ >= 0 && outStart_ < out_.size()) {
        const ssize_t n = ::send(fd_, out_.data() + outStart_, out_.size() - outStart_, MSG_NOSIGNAL);
        if (n > 0) {
            outStart_ += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0 && errno == EINTR) continue;
        fail(std::string("send: ") + std::strerror(errno));
        return;
    }
    if (outStart_ == out_.size()) {
        out_.clear();
        outStart_ = 0;
    } else if (outStart_ > (1u << 20)) {
        out_.erase(0, outStart_);
        outStart_ = 0;
    }
}

void Client::queueFrame(Opcode opcode, const std::string& payload) {
    uint8_t mask[4];
    nextMask(mask);
    out_ += encodeFrame(opcode, payload, mask);
}

bool Client::sendText(const std::string& text) {
    if (state_ != State::Open) return false;
    queueFrame(Text, text);
    flush();
    return state_ == State::Open;
}

void Client::onReadable(std::vector<std::string>& messages) {
    if (fd_ < 0) return;
    char chunk[65536];
    for (;;) {
        const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (n == 0) {
            fail("the other side closed the connection");
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            fail(std::string("recv: ") + std::strerror(errno));
            return;
        }
        const char* data = chunk;
        size_t size = static_cast<size_t>(n);
        if (state_ == State::Handshaking || state_ == State::Connecting) {
            in_.append(chunk, size);
            size_t consumed = 0;
            std::string why;
            const Handshake h = parseHandshakeResponse(in_, key_, consumed, why);
            if (h == Handshake::Incomplete) continue;
            if (h == Handshake::Failed) {
                fail("handshake: " + why);
                return;
            }
            state_ = State::Open;
            const std::string rest = in_.substr(consumed);
            in_.clear();
            // Frames that came in the same read as the answer (a ping, a close, the first events) count like any
            if (!takeFrames(rest.data(), rest.size(), messages)) return;
            flush();  // anything queued while handshaking, and a pong
            continue;
        }
        if (!takeFrames(data, size, messages)) return;
        flush();
    }
}

bool Client::takeFrames(const char* data, size_t size, std::vector<std::string>& messages) {
    std::vector<FrameParser::Message> frames;
    if (!parser_.feed(data, size, frames)) {
        fail("frames: " + parser_.error());
        return false;
    }
    for (auto& f : frames) {
        switch (f.opcode) {
            case Text: messages.push_back(std::move(f.payload)); break;
            case Ping: queueFrame(Pong, f.payload); break;
            case Close:
                queueFrame(Close, f.payload.substr(0, 2));
                flush();
                fail("the other side sent close");
                return false;
            default: break;  // binary and pongs: nothing for DevTools
        }
    }
    return true;
}

}  // namespace ws
