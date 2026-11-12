// The DevTools connection to Chrome, two ways behind one interface:
//   - the pipe (--remote-debugging-pipe): Chrome reads commands from its fd 3 and writes answers to its fd 4, each
//     message a JSON text ending in a NUL byte. Nothing listens anywhere: only the process holding the other end can
//     drive Chrome. We give Chrome one end of a socketpair as both fd 3 and 4 (a socket, so our writes can use
//     MSG_NOSIGNAL), and keep the other end here (PipeClient).
//   - the WebSocket on 127.0.0.1:<port> (--remote-debugging-port=0, ws::Client): any local process of any user, and
//     any sandbox sharing the host's network, can connect to that port while Chrome runs.
// The pipe is the default; the port is the fallback (config "transport": "port", or automatically for the rest of the
// run when the pipe gets no answer: fd passing through flatpak run → bwrap → zypak is not verified).
#pragma once

#include "devtools_channel.h"

#include <cstddef>
#include <string>
#include <vector>

namespace devtools {

/** Splits the pipe's byte stream into its NUL-terminated messages. */
class PipeParser {
public:
    static constexpr size_t kMaxMessage = 64u << 20;  ///< as the WebSocket's limit (a screencast frame is far less)
    /** Appends bytes and takes every complete message out. @return false if one grew past kMaxMessage */
    bool feed(const char* data, size_t size, std::vector<std::string>& out);
    const std::string& error() const { return error_; }

private:
    std::string buffer_;
    size_t start_ = 0;
    size_t scanned_ = 0;  ///< buffer_ before this has no NUL after start_ (the next search starts here)
    std::string error_;
};

/** Our end of the pipe (a socketpair's end). Non-blocking; the owner polls fd(). */
class PipeClient : public DevToolsChannel {
public:
    PipeClient() = default;
    ~PipeClient() override;
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    /** Takes over fd (our end of the socketpair whose other end is Chrome's fd 3 and 4). */
    void attach(int fd);

    bool open() const override { return state_ == State::Open; }
    bool failed() const override { return state_ == State::Failed; }
    int fd() const override { return fd_; }
    bool wantsWrite() const override { return fd_ >= 0 && out_.size() > outStart_; }
    const std::string& error() const override { return error_; }
    bool sendText(const std::string& text) override;
    void onReadable(std::vector<std::string>& messages) override;
    void onWritable() override { flush(); }
    size_t queued() const override { return out_.size() - outStart_; }
    void close() override;
    /** @return true once anything came back (Chrome is on the other end) */
    bool heardFrom() const { return heard_; }

private:
    enum class State { Closed, Open, Failed };
    int fd_ = -1;
    State state_ = State::Closed;
    std::string out_;
    size_t outStart_ = 0;
    PipeParser parser_;
    std::string error_;
    bool heard_ = false;

    void fail(const std::string& why);
    void flush();
};

}  // namespace devtools
