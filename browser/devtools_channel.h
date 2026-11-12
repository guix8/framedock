// The DevTools connection as the browser session sees it: the WebSocket (ws::Client) or the pipe
// (devtools::PipeClient). See devtools_pipe.h for why there are two.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

/** What the session needs from either connection (ws::Client and PipeClient implement it). */
class DevToolsChannel {
public:
    virtual ~DevToolsChannel() = default;
    /** @return true while messages can be sent */
    virtual bool open() const = 0;
    /** @return true once it broke (error() says why); close() clears it */
    virtual bool failed() const = 0;
    /** @return the fd to poll (−1 when there is none) */
    virtual int fd() const = 0;
    /** @return true while bytes wait to go out (poll for POLLOUT) */
    virtual bool wantsWrite() const = 0;
    virtual const std::string& error() const = 0;
    /** Queues one message (a JSON text). @return false when not open */
    virtual bool sendText(const std::string& text) = 0;
    /** The fd is readable: the messages that came in */
    virtual void onReadable(std::vector<std::string>& messages) = 0;
    /** The fd is writable: sends what is queued */
    virtual void onWritable() = 0;
    /** @return bytes queued and not sent yet */
    virtual size_t queued() const = 0;
    virtual void close() = 0;
};
