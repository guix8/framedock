// Chrome's --remote-debugging-pipe. See devtools_pipe.h.
#include "devtools_pipe.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace devtools {

bool PipeParser::feed(const char* data, size_t size, std::vector<std::string>& out) {
    if (!error_.empty()) return false;
    buffer_.append(data, size);
    for (;;) {
        // From where the last search stopped, not from the message's start: a large message arriving in many reads
        // is searched once (was O(n²) over its reads)
        const size_t zero = buffer_.find('\0', std::max(start_, scanned_));
        if (zero == std::string::npos) {
            scanned_ = buffer_.size();
            break;
        }
        out.emplace_back(buffer_, start_, zero - start_);
        start_ = zero + 1;
    }
    if (buffer_.size() - start_ > kMaxMessage) {
        error_ = "a message larger than " + std::to_string(kMaxMessage) + " bytes";
        return false;
    }
    // Compact once the parsed part is large (not on every message)
    if (start_ > 0 && (start_ == buffer_.size() || start_ > (1u << 20))) {
        buffer_.erase(0, start_);
        scanned_ -= std::min(scanned_, start_);
        start_ = 0;
    }
    return true;
}

PipeClient::~PipeClient() { close(); }

void PipeClient::attach(int fd) {
    close();
    error_.clear();
    fd_ = fd;
    ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL) | O_NONBLOCK);
    ::fcntl(fd_, F_SETFD, FD_CLOEXEC);
    state_ = State::Open;
}

void PipeClient::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    state_ = State::Closed;
    out_.clear();
    outStart_ = 0;
    parser_ = PipeParser {};
    heard_ = false;
}

void PipeClient::fail(const std::string& why) {
    if (state_ != State::Failed) error_ = why;
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    state_ = State::Failed;
    out_.clear();
    outStart_ = 0;
}

void PipeClient::flush() {
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

bool PipeClient::sendText(const std::string& text) {
    if (state_ != State::Open) return false;
    out_ += text;
    out_ += '\0';
    flush();
    return state_ == State::Open;
}

void PipeClient::onReadable(std::vector<std::string>& messages) {
    if (fd_ < 0) return;
    char chunk[65536];
    for (;;) {
        const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (n == 0) {
            fail("the other side closed the pipe");
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            fail(std::string("recv: ") + std::strerror(errno));
            return;
        }
        heard_ = true;
        if (!parser_.feed(chunk, static_cast<size_t>(n), messages)) {
            fail("pipe: " + parser_.error());
            return;
        }
    }
    flush();
}

}  // namespace devtools
