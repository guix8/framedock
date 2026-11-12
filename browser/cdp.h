// The Chrome DevTools Protocol messages the browser panel uses: building commands (JSON text with the id and, for a
// page, the flattened session id) and reading what comes back (responses and events). Pure: no sockets, no Chrome.
// JSON goes through Perf's parser (perf/core/json.*).
#pragma once

#include "json.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cdp {

/** A message from Chrome: the answer to a command, or an event. */
struct Message {
    enum class Kind { Response, Event, Invalid };
    Kind kind = Kind::Invalid;
    int id = 0;                 ///< Response
    bool failed = false;        ///< Response: Chrome returned "error"
    std::string errorMessage;   ///< Response: error.message
    std::string method;         ///< Event
    std::string sessionId;      ///< the page session it belongs to ("" = the browser)
    JsonValue body;             ///< Response: "result"; Event: "params"
};

/** @return the message, Kind::Invalid if it isn't JSON or isn't a response or event */
Message parse(const std::string& text);

/** Numbers commands and writes them as JSON. */
class Writer {
public:
    /**
     * @param method e.g. "Page.navigate"
     * @param params a JSON object ("{}" when there are none)
     * @param sessionId the page session (flattened, Target.attachToTarget), "" for the browser itself
     * @param id set to the command's id (the response carries it)
     * @return the message to send
     */
    std::string command(const std::string& method, const std::string& params, const std::string& sessionId, int& id);
    int lastId() const { return nextId_ - 1; }

private:
    int nextId_ = 1;
};

// ---- Parameters (JSON objects) ----
/** A number for JSON: finite, at most 3 decimals ("0" for NaN / infinity) */
std::string number(double value);

std::string navigateParams(const std::string& url);
/** Page.startScreencast: JPEG at this quality, scaled to fit maxWidth × maxHeight, every frame (we pace the acks) */
std::string screencastParams(int maxWidth, int maxHeight, int quality);
std::string screencastAckParams(int frameSessionId);
/** Emulation.setDeviceMetricsOverride: a desktop page of width × height CSS px at scale 1 */
std::string metricsParams(int width, int height, bool mobile = false);
/**
 * Emulation.setUserAgentOverride: a phone's (Android) user agent and client hints for the mobile version of sites, or
 * the desktop one back ("" lets Chrome use its own)
 */
std::string userAgentParams(const std::string& userAgent, bool mobile);
std::string createTargetParams(const std::string& url);
std::string attachParams(const std::string& targetId);
std::string evaluateParams(const std::string& expression, bool userGesture = false);
std::string historyEntryParams(int entryId);
std::string detachParams(const std::string& sessionId);
std::string closeTargetParams(const std::string& targetId);
/** Page.setWebLifecycleState: "frozen" or "active" */
std::string lifecycleParams(bool frozen);
/** Browser.getWindowForTarget */
std::string windowForTargetParams(const std::string& targetId);
/** Browser.setWindowBounds: the window minimized, or back to normal */
std::string windowStateParams(int windowId, bool minimized);
/** Page.handleJavaScriptDialog: accept (with the prompt's text) or dismiss */
std::string dialogParams(bool accept, const std::string& promptText);

/** Mouse buttons as Input.dispatchMouseEvent names them */
enum class Button { None, Left, Right, Middle };
/**
 * Input.dispatchMouseEvent.
 * @param type "mouseMoved", "mousePressed", "mouseReleased"
 * @param x,y CSS px in the page's viewport
 * @param buttons the buttons held (bit 1 = left), as the event's "buttons"
 */
std::string mouseParams(const char* type, double x, double y, Button button, int clickCount, int buttons);
/** A wheel turn at x,y (deltas in CSS px, positive y scrolls down) */
std::string wheelParams(double x, double y, double deltaX, double deltaY);
std::string insertTextParams(const std::string& text);

/** The keys the keyboard sends as key events (everything printable goes as insertText). */
enum class Key { Backspace, Enter, Tab, Escape, Left, Right, Up, Down, Delete, Home, End, SelectAll, Copy, Paste };
/**
 * Input.dispatchKeyEvent for one key going down or up ("rawKeyDown" / "keyDown" with text for Enter, "keyUp").
 * SelectAll, Copy and Paste are Ctrl+A / C / V (modifiers 2) with their editing command ("selectAll", "copy",
 * "paste"): the page's selection to Chrome's own clipboard and back (headless Chrome keeps its clipboard to itself).
 */
std::string keyParams(Key key, bool down);

// ---- Reading results ----
/** Chrome's DevToolsActivePort file: the port on the first line, the browser endpoint's path on the second. */
bool parseActivePort(const std::string& content, int& port, std::string& path);

/** One screencast frame (Page.screencastFrame). */
struct ScreencastFrame {
    std::string jpeg;        ///< decoded from base64
    int sessionId = 0;       ///< what screencastFrameAck needs
    double deviceWidth = 0;  ///< metadata: the viewport in CSS px
    double deviceHeight = 0;
    double offsetTop = 0;
    double pageScale = 1;
};
/** @return false if params isn't a frame (or its data isn't base64) */
bool readScreencastFrame(const JsonValue& params, ScreencastFrame& frame);

/** What readScreencastMessage made of a message */
enum class FastFrame {
    NotAFrame,  ///< some other message: parse it as usual
    Frame,      ///< a frame: frame and sessionId are set
    Fallback,   ///< a frame in a shape this reader doesn't take (escapes in the data, odd order): parse it as usual
};
/**
 * Page.screencastFrame straight from the message text, without parsing it into a JsonValue (that copies the large
 * base64 string, then decodes the copy): the data is decoded from the text in place, the small metadata parsed alone.
 * Takes Chrome's own shape ({"method":"Page.screencastFrame","params":{...},"sessionId":"..."}); anything else is
 * NotAFrame or Fallback, so the full parser still handles it.
 * @param sessionId the page session the frame belongs to
 */
FastFrame readScreencastMessage(const std::string& text, ScreencastFrame& frame, std::string& sessionId);

/** Page.getNavigationHistory's answer: where we are and what back / forward lead to. */
struct History {
    int currentIndex = -1;
    std::vector<int> entryIds;
    std::string url;    ///< the current entry's
    std::string title;
    bool canBack() const { return currentIndex > 0; }
    bool canForward() const { return currentIndex >= 0 && currentIndex + 1 < static_cast<int>(entryIds.size()); }
    /** @return the entry id to go to (back: −1, forward: +1), −1 when there is none */
    int entryAt(int step) const;
};
bool readHistory(const JsonValue& result, History& history);

/** Runtime.evaluate's boolean result (returnByValue). @return false if it isn't one */
bool readBoolResult(const JsonValue& result, bool& value);
/** Runtime.evaluate's string result (returnByValue). @return false if it isn't one */
bool readStringResult(const JsonValue& result, std::string& value);
/** @return a script whose value is the address of the link at the page point x,y (CSS px), "" when there is none */
std::string linkAtScript(double x, double y);

// ---- The address bar ----
/**
 * What the address bar's text means: a URL as it is (http, https, about, chrome, file schemes, or a host that looks
 * like one, given https://), otherwise a Google search for it.
 */
std::string addressToUrl(const std::string& text);
/** @return text percent-encoded for a URL query (UTF-8 bytes; unreserved characters kept, space as %20) */
std::string urlEncode(const std::string& text);

/** Where a point on the panel's page area lands in the page. */
struct ViewMap {
    double areaX = 0, areaY = 0, areaW = 1, areaH = 1;  ///< the page area on the texture (px)
    int viewportW = 1280, viewportH = 720;              ///< the page's viewport (CSS px)
    /**
     * @param x,y texture px
     * @param cssX,cssY the page point (clamped into the viewport)
     * @return false if the point is outside the page area
     */
    bool toPage(double x, double y, double& cssX, double& cssY) const;
};

}  // namespace cdp
