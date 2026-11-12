// 즐겨찾기: the pages saved with the browser panel's ☆, in a small file of their own
// (~/.config/framedock/favorites.json: {"favorites": [{"title": "...", "url": "..."}, ...]}), written atomically after
// each change. At most kMax, in the order they were saved. A file that can't be read is left as it is until the first
// change, which moves it aside (favorites.json.bad) before writing a new one, so nothing is overwritten unseen.
#pragma once

#include "browser_view.h"

#include <string>
#include <vector>

class FavoriteStore {
public:
    static constexpr size_t kMax = 10;  ///< the list under 즐겨찾기 shows them all

    explicit FavoriteStore(std::string path) : path_(std::move(path)) {}

    /** Reads the file (missing = none saved yet). @return false if it exists but can't be read (error() says why) */
    bool load();
    const std::vector<Bookmark>& items() const { return items_; }
    /** @return the index of the saved page with this URL, −1 if it isn't saved */
    int find(const std::string& url) const;

    enum class Change {
        Done,     ///< changed and saved
        Full,     ///< kMax already: nothing added
        Unsaved,  ///< changed here, but the file couldn't be written (error() says why; the next change tries again)
        None,     ///< nothing to do (already saved / not there)
    };
    /** Saves a page (its title, or the URL when it has none) at the end of the list. */
    Change add(const std::string& title, const std::string& url);
    /** Removes the saved page at index. */
    Change remove(size_t index);

    /** @return why the last load or save failed ("" when it didn't), for the log */
    const std::string& error() const { return error_; }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::vector<Bookmark> items_;
    std::string error_;
    bool unreadable_ = false;  ///< the file on disk couldn't be read: moved aside before the first write

    Change save();
};
