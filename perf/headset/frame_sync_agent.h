// Frame Sync's network side: the home PC's agent over HTTP (agent_client.py's AgentClient), Wake-on-LAN
// (send_wol), the settings (app.py's load_settings) and the PC's account list cached for when the PC is off
// (app.py's ~/.cache/frame-sync/pc-accounts.json, the same file and format so both apps agree while both exist).
//
// Calls block for up to their timeout (4 s for the status poll, 12 s during a sync, as app.py): only the Frame Sync
// worker thread makes them. The key (token) is sent as a Bearer header and never logged.
#pragma once

#include "frame_sync_flow.h"
#include "json.h"

#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace framesync {

/** ~/.config/frame-sync's config.json and token. */
struct Settings {
    std::string agentUrl;
    std::string pcMac;
    std::string broadcast;
    std::string token;
};

/** ~/.config/frame-sync (XDG_CONFIG_HOME is not used: Frame Sync's install writes there) */
std::string defaultConfigDir();
/** ~/.cache/frame-sync/pc-accounts.json */
std::string defaultCacheFile();

/**
 * Whether Frame Sync is set up here at all: config.json and a non-empty token file in configDir (only looked at with
 * stat, nothing is read). Without them the 기기 page leaves PC 계정 맞추기 out.
 */
bool settingsPresent(const std::string& configDir);

/**
 * app.py's load_settings: config.json with agentUrl, pcMac and broadcast all set and a non-empty token.
 * @return nullopt when anything is missing or unreadable (the page says the settings are missing)
 */
std::optional<Settings> loadSettings(const std::string& configDir);

/** The PC's /status as the page needs it (flow.py reads steam.running / steamId / gameAppId, streaming, users). */
PcStatus statusFrom(const JsonValue& json);
/** /steam/accounts' "accounts" (and the cache file): entries without a steamId are skipped. */
std::vector<PcAccount> pcAccountsFrom(const JsonValue& accounts);

/** @return value as Python's json.dumps(value, ensure_ascii=False) writes it (", " and ": ", UTF-8 as it is) */
std::string pythonJson(const JsonValue& value);

/** app.py's load_cached_pc_accounts. nullopt if there is no usable file. */
std::optional<std::vector<PcAccount>> loadCachedPcAccounts(const std::string& file);
/**
 * app.py's save_cached_pc_accounts: the list as the agent sent it (atomically replaced). A file that already says
 * exactly that is left alone (the poll runs every 5 s; no write, no fsync).
 * @param written set to whether the file was written (may be null)
 */
bool saveCachedPcAccounts(const std::string& file, const JsonValue& accounts, std::string& error,
                          bool* written = nullptr);

/**
 * agent_client.py's send_wol: the magic packet to broadcast:port, three times, from a SO_BROADCAST UDP socket.
 * @return false (and why) if the address is bad or nothing could be sent
 */
bool sendWol(const std::string& mac, const std::string& broadcast, std::string& error, int port = 9);

/** agent_client.py's AgentClient on libcurl. Every call may throw AgentError (and std::runtime_error, as KeyError). */
class HttpAgent : public Agent {
public:
    /**
     * @param baseUrl the agent's URL (a trailing / is dropped)
     * @param timeoutSec how long the connect, and any silence while reading, may take (4 s; 12 s during a sync)
     * @param cancel when set, a call in progress ends at once with AgentError::Kind::Cancelled (shutting down)
     */
    HttpAgent(std::string baseUrl, std::string token, double timeoutSec, const std::atomic<bool>* cancel = nullptr);

    PcStatus status() override;
    void claim() override;
    SwitchReply switchTo(const std::string& steamId, bool force) override;
    SwitchResult switchResult(const std::string& requestId) override;

    /** GET /steam/accounts. @param raw the "accounts" array as sent (for the cache) */
    std::vector<PcAccount> accounts(JsonValue* raw = nullptr);
    /** GET /steam/avatar/<id>: the PNG, or nullopt if the agent has none (400, 403, 404) */
    std::optional<std::string> avatar(const std::string& steamId);
    /** POST /shutdown {"manual": true} */
    void shutdown();

    /** What came back. */
    struct Response {
        int status = 0;
        std::string body;
    };
    /**
     * One request: the key as a Bearer header, Content-Type application/json with a body.
     * @param body JSON text, or nullptr for none
     * @return the status and body when status is in ok; otherwise throws AgentError
     */
    Response request(const char* method, const std::string& path, const std::string* body,
                     std::initializer_list<int> ok = {200});

private:
    std::string base_;
    std::string token_;
    double timeoutSec_;
    const std::atomic<bool>* cancel_;
};

/** libcurl's global init, once per process (FrameSyncWorker calls it before its thread starts). */
void initHttp();

}  // namespace framesync
