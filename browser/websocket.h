// A minimal WebSocket (RFC 6455) client for Chrome's DevTools endpoint on 127.0.0.1, and the few server-side pieces
// the tests' fake Chrome needs. No TLS, no extensions, no compression: Chrome's DevTools socket needs none of them.
//
// The framing (encodeFrame, FrameParser) and the handshake helpers are pure, so the tests check them byte by byte.
// Client is a non-blocking TCP socket the owner polls: fd(), wantsWrite(), onReadable(), onWritable(). Nothing here
// blocks or spins; a caller with nothing to read sleeps in poll().
#pragma once

#include "devtools_channel.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ws {

/** @return the 20-byte SHA-1 digest (for Sec-WebSocket-Accept) */
std::string sha1(const std::string& data);
std::string base64Encode(const std::string& data);
/**
 * Decodes standard base64 (padding optional, no whitespace).
 * @return false on a character outside the alphabet or a bad length
 */
bool base64Decode(const char* data, size_t size, std::string& out);
inline bool base64Decode(const std::string& data, std::string& out) { return base64Decode(data.data(), data.size(), out); }

/** @return the Sec-WebSocket-Accept value for a Sec-WebSocket-Key */
std::string acceptFor(const std::string& key);
/** @return a GET request upgrading to a WebSocket */
std::string handshakeRequest(const std::string& host, int port, const std::string& path, const std::string& key);

/** Where the server's answer to the handshake stands. */
enum class Handshake { Incomplete, Ok, Failed };
/**
 * Reads the server's answer from the start of buffer.
 * @param consumed the bytes the answer took (Ok); what follows is already frames
 * @param error why it failed (Failed)
 */
Handshake parseHandshakeResponse(const std::string& buffer, const std::string& key, size_t& consumed,
                                 std::string& error);
/**
 * Server side (the tests' fake Chrome): reads a client's upgrade request.
 * @param path the request's path
 * @param response the 101 answer to send (Ok)
 */
Handshake parseHandshakeRequest(const std::string& buffer, size_t& consumed, std::string& path, std::string& response);

enum Opcode : uint8_t { Continuation = 0x0, Text = 0x1, Binary = 0x2, Close = 0x8, Ping = 0x9, Pong = 0xA };

/**
 * One frame (FIN set). A client masks every frame it sends (RFC 6455 5.3); a server never does.
 * @param mask the masking key, or nullptr for an unmasked (server) frame
 */
std::string encodeFrame(Opcode opcode, const std::string& payload, const uint8_t* mask);

/** Turns received bytes into messages: joins fragments, unmasks, answers control frames through the caller. */
class FrameParser {
public:
    struct Message {
        Opcode opcode;  ///< Text, Binary, Close, Ping or Pong (a fragmented message comes whole, with its first opcode)
        std::string payload;
    };
    /** Messages larger than this are an error (a screencast frame is a few hundred KiB) */
    static constexpr size_t kMaxMessage = 64u << 20;

    /** Appends bytes and takes every complete message out of them. @return false on a protocol error (error()) */
    bool feed(const char* data, size_t size, std::vector<Message>& out);
    const std::string& error() const { return error_; }

private:
    std::string buffer_;
    size_t start_ = 0;  ///< parsed up to here (the buffer is compacted now and then, not on every frame)
    std::string partial_;
    Opcode partialOpcode_ = Text;
    bool inFragment_ = false;
    std::string error_;
};

/** A client connection to ws://127.0.0.1:port/path. */
class Client : public DevToolsChannel {
public:
    enum class State { Closed, Connecting, Handshaking, Open, Failed };

    Client() = default;
    ~Client() override;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /**
     * Starts a non-blocking connect to 127.0.0.1 (always loopback: the DevTools port must not reach the network).
     * @return false if the socket couldn't even be made (error())
     */
    bool connect(int port, const std::string& path);
    /** Closes the socket (no close frame: the other side may already be gone). */
    void close() override;

    State state() const { return state_; }
    bool open() const override { return state_ == State::Open; }
    bool failed() const override { return state_ == State::Failed; }
    int fd() const override { return fd_; }
    /** @return true while bytes wait to go out (poll for POLLOUT), or the connect is in progress */
    bool wantsWrite() const override { return fd_ >= 0 && (state_ == State::Connecting || out_.size() > outStart_); }
    const std::string& error() const override { return error_; }

    /** Queues a text message (sent as the socket takes it). False when not open */
    bool sendText(const std::string& text) override;
    /** The socket is readable. @param messages text messages that came in (ping / pong / close handled here) */
    void onReadable(std::vector<std::string>& messages) override;
    /** The socket is writable: finishes the connect, sends the handshake or queued frames. */
    void onWritable() override;
    /** @return bytes queued and not yet sent (back pressure: the caller may drop mouse moves while it grows) */
    size_t queued() const override { return out_.size() - outStart_; }

private:
    int fd_ = -1;
    State state_ = State::Closed;
    std::string path_;
    int port_ = 0;
    std::string key_;
    std::string in_;      ///< handshake bytes until the answer is complete
    std::string out_;
    size_t outStart_ = 0;
    FrameParser parser_;
    std::string error_;
    uint32_t maskState_ = 0;

    void fail(const std::string& why);
    /** Parses received bytes: text messages out, a ping answered, a close ends it. @return false if it ended */
    bool takeFrames(const char* data, size_t size, std::vector<std::string>& messages);
    void queueFrame(Opcode opcode, const std::string& payload);
    void flush();
    void nextMask(uint8_t mask[4]);
};

}  // namespace ws
