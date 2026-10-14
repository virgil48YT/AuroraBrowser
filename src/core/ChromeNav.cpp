#include "core/ChromeNav.hpp"

namespace aurora::core {

namespace {

// Iterator arithmetic without a sign-conversion warning under -Wconversion.
std::vector<ChromeNavEntry>::difference_type off(size_t i) {
    return static_cast<std::vector<ChromeNavEntry>::difference_type>(i);
}

bool same_target(const ChromeNavEntry& a, const ChromeNavEntry& b) {
    if (a.is_internal() != b.is_internal()) return false;
    if (a.is_internal()) return *a.page == *b.page;
    return a.url.normalized() == b.url.normalized();
}

} // namespace

void ChromeNavStack::record(const ChromeNavEntry& entry) {
    if (!entries_.empty() && same_target(entries_[index_], entry)) {
        // Same place: refresh what we know about it, keep the stack shape.
        entries_[index_].url = entry.url;
        if (!entry.title.empty()) entries_[index_].title = entry.title;
        return;
    }
    if (!entries_.empty() && index_ + 1 < entries_.size()) {
        entries_.erase(entries_.begin() + off(index_ + 1), entries_.end());
    }
    entries_.push_back(entry);
    index_ = entries_.size() - 1;
}

const ChromeNavEntry* ChromeNavStack::current() const {
    if (entries_.empty()) return nullptr;
    return &entries_[index_];
}

const ChromeNavEntry* ChromeNavStack::peek_back() const {
    if (!can_go_back()) return nullptr;
    return &entries_[index_ - 1];
}

const ChromeNavEntry* ChromeNavStack::peek_forward() const {
    if (!can_go_forward()) return nullptr;
    return &entries_[index_ + 1];
}

const ChromeNavEntry* ChromeNavStack::go_back() {
    if (!can_go_back()) return nullptr;
    --index_;
    return &entries_[index_];
}

const ChromeNavEntry* ChromeNavStack::go_forward() {
    if (!can_go_forward()) return nullptr;
    ++index_;
    return &entries_[index_];
}

void ChromeNavStack::note_web_url(const Url& url, const std::string& title) {
    if (entries_.empty()) {
        entries_.push_back(ChromeNavEntry{std::nullopt, url, title});
        index_ = 0;
        return;
    }
    auto& cur = entries_[index_];
    // While an internal page is the current entry the engine's address changes
    // belong to the (hidden) page behind it — never overwrite the page entry.
    // chrome-extension:// is also in-app: do not let a later web URL report
    // clobber it into something that could be dispatched externally.
    if (cur.is_internal() || cur.url.is_extension_url()) return;
    cur.url = url;
    if (!title.empty()) cur.title = title;
}

} // namespace aurora::core
