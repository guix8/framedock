// Switching the Frame's own Steam to another of its accounts, after the home PC was switched to it from the list.
// It does what Steam's own "switch account" does: the account Steam logs in as next is the one with AutoLogin "1" in
// config/loginusers.vdf and AutoLoginUser in ~/.steam/registry.vdf. Steam rewrites both when it quits, so they are
// changed while steam.service is stopped. Only an account whose login Steam remembers comes up without a password.
//
// Checked on the headset 2026-11-14 (SteamOS with deckard's steam.service, Restart=always): `systemctl --user stop
// steam.service` leaves steamvr.service and framedock.service running (nothing requires steam.service), and after
// `start` Steam logged on as the new account in about 6 s ("[Logged On, ...] [U:1:<account id>]" in
// logs/connection_log.txt). Not a documented interface: a Steam update can change it.
#pragma once

#include <functional>
#include <optional>
#include <string>

namespace framesync {

/** loginusers.vdf with AutoLogin "1" in steamId's block and "0" in the others. nullopt: no such block or key. */
std::optional<std::string> withAutoLogin(const std::string& loginUsers, const std::string& steamId);
/** registry.vdf with AutoLoginUser set to accountName (letters, digits, _). nullopt: not exactly one key, a bad name. */
std::optional<std::string> withAutoLoginUser(const std::string& registry, const std::string& accountName);
/** Whether a "[Logged On" line of connection_log.txt names steamId's account ([U:1:<id - 76561197960265728>]). */
bool loggedOnAs(const std::string& logText, const std::string& steamId);
/** Whether Steam runs a game on the Frame (a process started as "reaper SteamLaunch ..."), from procDir. */
bool frameGameRunning(const std::string& procDir = "/proc");
/** Runs `systemctl --user <verb> steam.service` (verb "stop" or "start"). @return true if it exited with 0 */
bool steamServiceCommand(const std::string& verb, double timeoutSec);

struct FrameSwitchPaths {
    std::string steamDir;      ///< ~/.local/share/Steam (config/loginusers.vdf, logs/connection_log.txt)
    std::string registryFile;  ///< ~/.steam/registry.vdf
    std::string backupDir;     ///< the files as they were before the last switch: ~/.local/state/framedock/steam-switch
    static FrameSwitchPaths defaults();
};

struct FrameSwitchHooks {
    std::function<bool(const std::string& verb)> service;  ///< steam.service "stop" / "start"; false = it failed
    std::function<bool(double seconds)> sleep;             ///< false: Frame Dock is quitting
    std::function<bool()> gameRunning;
};

constexpr double kFrameLogonLimitSec = 60;  ///< Steam's logon after the start (it took 6 s)

/**
 * Switches the Frame's Steam to steamId: stop Steam, back the two files up, change them, start Steam, wait for the
 * logon. Blocks (the worker thread runs it). Refused while a game runs on the Frame. Already that account: done.
 * @param message why it failed (app_text's words)
 * @return true once Steam logged on as steamId
 */
bool switchFrameSteam(const FrameSwitchPaths& paths, const std::string& steamId, const FrameSwitchHooks& hooks,
                      std::string& message);

}  // namespace framesync
