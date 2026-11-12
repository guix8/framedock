// 즐겨찾기 (the browser panel's saved pages). See favorites.h.
#include "favorites.h"

#include "atomic_file.h"
#include "json.h"

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

bool FavoriteStore::load() {
    items_.clear();
    error_.clear();
    unreadable_ = false;
    struct stat st {};
    if (::stat(path_.c_str(), &st) != 0 && errno == ENOENT) return true;  // nothing saved yet
    std::ifstream file(path_);
    if (!file) {
        error_ = path_ + ": can't be opened";
        unreadable_ = true;
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    JsonValue root;
    std::string why;
    if (!parseJson(ss.str(), root, why)) {
        error_ = path_ + ": " + why;
        unreadable_ = true;
        return false;
    }
    const JsonValue* list = root.get("favorites");
    if (list == nullptr || list->type != JsonValue::Type::Array) {
        error_ = path_ + ": no \"favorites\" list";
        unreadable_ = true;
        return false;
    }
    for (const JsonValue& item : list->items) {
        const JsonValue* url = item.get("url");
        const JsonValue* title = item.get("title");
        if (url == nullptr || !url->isString() || url->text.empty() || find(url->text) >= 0) continue;
        items_.push_back({title != nullptr && title->isString() ? title->text : std::string(), url->text});
        if (items_.size() == kMax) break;
    }
    return true;
}

int FavoriteStore::find(const std::string& url) const {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].url == url) return static_cast<int>(i);
    }
    return -1;
}

FavoriteStore::Change FavoriteStore::add(const std::string& title, const std::string& url) {
    if (url.empty() || find(url) >= 0) return Change::None;
    if (items_.size() >= kMax) return Change::Full;
    items_.push_back({title, url});
    return save();
}

FavoriteStore::Change FavoriteStore::remove(size_t index) {
    if (index >= items_.size()) return Change::None;
    items_.erase(items_.begin() + static_cast<long>(index));
    return save();
}

FavoriteStore::Change FavoriteStore::save() {
    if (unreadable_) {
        // The file we couldn't read keeps its content under another name
        const std::string aside = path_ + ".bad";
        if (std::rename(path_.c_str(), aside.c_str()) != 0 && errno != ENOENT) {
            error_ = "can't move " + path_ + " aside: " + std::strerror(errno);
            return Change::Unsaved;
        }
        unreadable_ = false;
    }
    std::string text = "{\n  \"favorites\": [";
    for (size_t i = 0; i < items_.size(); ++i) {
        text += i == 0 ? "\n" : ",\n";
        text += "    {\"title\": " + jsonString(items_[i].name) + ", \"url\": " + jsonString(items_[i].url) + "}";
    }
    text += items_.empty() ? "]\n}\n" : "\n  ]\n}\n";
    std::string why;
    if (!frame_perf_overlay::atomicWriteFile(path_, text, why)) {
        error_ = why;
        return Change::Unsaved;
    }
    error_.clear();
    return Change::Done;
}
