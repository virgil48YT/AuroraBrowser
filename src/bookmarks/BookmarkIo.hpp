// Aurora — Netscape bookmark HTML interchange.
//
// The portable bookmark tree format used by Chrome, Firefox and Safari.
// No browser engine or HTML/Qt dependency is required; file selection and
// reading/writing belong to the UI. Import appends without replacing or
// deduplicating existing nodes, preserving sibling order and ADD_DATE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace aurora::bookmarks {

class BookmarkService;

struct BookmarkImportResult {
    std::size_t folders_imported = 0;
    std::size_t bookmarks_imported = 0;
    // Entries rejected for unsupported/missing URLs or failed insertion.
    std::size_t skipped = 0;
    // Malformed entries (bad dates, truncated tags, missing closing A/H3).
    // Recovered entries can also be imported, so this is not a separate total.
    std::size_t malformed = 0;
};

std::string export_netscape_html(const BookmarkService& service);

// Only absolute HTTP(S) bookmarks are imported; javascript:, file:, data:
// and browser-internal URLs are deliberately rejected. Descriptions are
// ignored. An invalid parent folder rejects all entries without relocation.
BookmarkImportResult import_netscape_html(BookmarkService& service,
                                         std::string_view html,
                                         int64_t parent_id = 0);

} // namespace aurora::bookmarks
