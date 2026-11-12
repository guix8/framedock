// The Frame's own Steam accounts, read-only: a port of Frame Sync's frame_steam.py. The current account is the one
// with AutoLogin "1" in config/loginusers.vdf (the same rule the PC's agent uses; Frame Sync part 1, checked
// 2026-10-25). Avatars come from config/avatarcache/<SteamID>.png.
#pragma once

#include "frame_sync_flow.h"

#include <map>
#include <string>
#include <vector>

namespace framesync {

/** ~/.local/share/Steam (frame_steam.STEAM_DIR) */
std::string defaultSteamDir();

/** One "<17 digits>" { "Key" "Value" ... } block of loginusers.vdf. */
struct LoginUser {
    std::string steamId;
    std::map<std::string, std::string> fields;  ///< values with their \x escapes undone
};

/**
 * frame_steam._blocks: every block whose body has no nested braces (quoted text may hold braces), in file order.
 * Same matching as the Python regexes, so both apps see the same accounts.
 */
std::vector<LoginUser> parseLoginUsers(const std::string& text);

/** frame_steam.accounts: blocks with an AccountName; the persona name falls back to it. */
std::vector<FrameAccount> frameAccounts(const std::vector<LoginUser>& users);
/** frame_steam.current_steam_id: the first block with AutoLogin "1" ("" = none). */
std::string currentSteamId(const std::vector<LoginUser>& users);

/** @return true for a 64-bit SteamID of an individual account (7656119 and ten digits) */
bool validSteamId(const std::string& steamId);
/** frame_steam.avatar_path: the avatar file if steamId is valid and the file exists, else "". */
std::string avatarPath(const std::string& steamDir, const std::string& steamId);

/** @return <steamDir>/config/loginusers.vdf's text ("" if it can't be read) */
std::string readLoginUsers(const std::string& steamDir);

}  // namespace framesync
