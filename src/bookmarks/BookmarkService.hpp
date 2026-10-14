// Aurora — bookmarks service.
//
// Bookmarks form a tree: folders contain bookmarks and other folders. The
// service is synchronous and stateless beyond its Database handle, mirroring
// the HistoryService design. Position is maintained explicitly so reordering
// in the UI is a cheap, local update.
#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>
#include "storage/Database.hpp"

namespace aurora::bookmarks {

struct BookmarkNode {
    int64_t id = 0;
    int64_t parent_id = 0;
    std::string title;
    std::string url;
    int position = 0;
    bool is_folder = false;
    std::chrono::system_clock::time_point added_at;
};

class BookmarkService {
public:
    explicit BookmarkService(storage::Database& db) : db_(db) {}

    // The root is parent_id = 0. Returns children of a folder, ordered by
    // position.
    std::vector<BookmarkNode> children(int64_t parent_id) const;

    // Add a bookmark or folder under an existing folder. Returns the new
    // node's id, or 0 on failure. Imports may preserve the original timestamp.
    int64_t add(int64_t parent_id, std::string_view title, std::string_view url,
                bool is_folder = false,
                std::chrono::system_clock::time_point added_at =
                    std::chrono::system_clock::now());

    // Update title/url.
    bool update(int64_t id, std::string_view title, std::string_view url);

    // Move a node to a new parent and/or position. Re-siblings are renumbered
    // so positions stay contiguous starting at 0.
    bool move(int64_t id, int64_t new_parent_id, int new_position);

    bool remove(int64_t id);

    // Flat list of every bookmark (not folders), for omnibox matching.
    std::vector<BookmarkNode> all_bookmarks() const;

private:
    storage::Database& db_;
    void renumber_siblings(int64_t parent_id);
};

} // namespace aurora::bookmarks
