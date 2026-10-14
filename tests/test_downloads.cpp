#include <doctest.h>

#include "downloads/DownloadService.hpp"
#include "storage/Database.hpp"
#include "storage/Migration.hpp"

using namespace aurora::storage;
using namespace aurora::downloads;

static Database open_downloads() {
    Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(Migrator::migrate(db) == DbError::Ok);
    return db;
}

TEST_CASE("begin inserts an in-progress download") {
    auto db = open_downloads();
    DownloadService s(db);
    int64_t id = s.begin("https://a.test/file.zip", "/tmp/file.zip",
                         "application/zip", 2048);
    CHECK(id > 0);

    auto item = s.find(id);
    REQUIRE(item.has_value());
    CHECK(item->url == "https://a.test/file.zip");
    CHECK(item->file_name() == "file.zip");
    CHECK(item->total_bytes == 2048);
    CHECK(item->received == 0);
    CHECK(item->state == DownloadState::InProgress);
    CHECK_FALSE(item->is_finished());
    CHECK_FALSE(item->finished_at.has_value());
    CHECK(item->percent() == 0);
}

TEST_CASE("empty url is rejected") {
    auto db = open_downloads();
    DownloadService s(db);
    CHECK(s.begin("", "/tmp/x", "", 1) == 0);
    CHECK(s.recent(10).empty());
}

TEST_CASE("progress and completion update the row") {
    auto db = open_downloads();
    DownloadService s(db);
    int64_t id = s.begin("https://a.test/f.bin", "/tmp/f.bin", "", -1);
    REQUIRE(id > 0);

    CHECK(s.update_progress(id, 512, 1024));
    auto mid = s.find(id);
    REQUIRE(mid.has_value());
    CHECK(mid->received == 512);
    CHECK(mid->percent() == 50);

    CHECK(s.finish(id, DownloadState::Complete));
    auto done = s.find(id);
    REQUIRE(done.has_value());
    CHECK(done->state == DownloadState::Complete);
    CHECK(done->is_finished());
    CHECK(done->finished_at.has_value());
}

TEST_CASE("percent is -1 when the total size is unknown") {
    DownloadItem d;
    d.total_bytes = -1;
    d.received = 900;
    CHECK(d.percent() == -1);
}

TEST_CASE("percent is capped at 100 when received overshoots") {
    DownloadItem d;
    d.total_bytes = 100;
    d.received = 250;
    CHECK(d.percent() == 100);
}

TEST_CASE("recent returns newest first and respects the limit") {
    auto db = open_downloads();
    DownloadService s(db);
    for (int i = 0; i < 5; ++i) {
        s.begin("https://a.test/" + std::to_string(i), "/tmp/" + std::to_string(i),
                "", 10);
    }
    auto all = s.recent(10);
    REQUIRE(all.size() == 5);
    // Same second for every row, so the id tiebreaker decides: newest first.
    CHECK(all.front().target_path == "/tmp/4");
    CHECK(s.recent(2).size() == 2);
}

TEST_CASE("remove, clear_finished and clear delete the expected rows") {
    auto db = open_downloads();
    DownloadService s(db);
    int64_t a = s.begin("https://a.test/a", "/tmp/a", "", 1);
    int64_t b = s.begin("https://a.test/b", "/tmp/b", "", 1);
    int64_t c = s.begin("https://a.test/c", "/tmp/c", "", 1);
    s.finish(b, DownloadState::Complete);
    s.finish(c, DownloadState::Cancelled);

    CHECK(s.remove(a));
    CHECK_FALSE(s.remove(a));  // already gone
    CHECK(s.recent(10).size() == 2);

    CHECK(s.clear_finished() == 2);
    CHECK(s.recent(10).empty());

    s.begin("https://a.test/d", "/tmp/d", "", 1);
    CHECK(s.clear() == 1);
    CHECK(s.recent(10).empty());
}

TEST_CASE("state strings round-trip and unknown text falls back") {
    for (auto st : {DownloadState::InProgress, DownloadState::Complete,
                    DownloadState::Cancelled, DownloadState::Interrupted}) {
        CHECK(state_from_string(to_string(st)) == st);
    }
    CHECK(state_from_string("garbage") == DownloadState::InProgress);
}

TEST_CASE("file_name handles windows separators and bare names") {
    DownloadItem win;
    win.target_path = R"(C:\Users\me\Downloads\report.pdf)";
    CHECK(win.file_name() == "report.pdf");
    DownloadItem bare;
    bare.target_path = "notes.txt";
    CHECK(bare.file_name() == "notes.txt");
}

TEST_CASE("an interrupted download can still be corrected to complete") {
    // Chromium interrupts a download for recoverable reasons (network hiccup,
    // server drop) and resumes it itself, so "interrupted" must not be a
    // one-way door: the same row has to be able to end up complete.
    auto db = open_downloads();
    DownloadService svc(db);
    const int64_t id = svc.begin("https://host.test/big.zip", "", "application/zip", 100);
    REQUIRE(id > 0);

    // The destination is only known once the engine reports it.
    CHECK(svc.set_target_path(id, "/tmp/big.zip"));
    CHECK(svc.find(id)->target_path == "/tmp/big.zip");
    CHECK_FALSE(svc.set_target_path(id, ""));

    CHECK(svc.finish(id, DownloadState::Interrupted));
    CHECK(svc.find(id)->state == DownloadState::Interrupted);

    CHECK(svc.update_progress(id, 100, 100));
    CHECK(svc.finish(id, DownloadState::Complete));
    CHECK(svc.find(id)->state == DownloadState::Complete);
}

// --- Pause / resume ----------------------------------------------------------
// Paused is a live-but-frozen state, not a terminal one: everything that
// treats downloads as "done" must exclude it, and both transitions are
// strictly in_progress <-> paused.

TEST_CASE("pause and resume move the row between in_progress and paused") {
    auto db = open_downloads();
    DownloadService s(db);
    int64_t id = s.begin("https://a.test/f.zip", "/tmp/f.zip", "", 100);
    REQUIRE(id > 0);

    CHECK(s.update_progress(id, 40, 100));
    CHECK(s.pause(id));
    auto paused = s.find(id);
    REQUIRE(paused.has_value());
    CHECK(paused->state == DownloadState::Paused);
    CHECK_FALSE(paused->is_finished());
    // Paused is still an engine-owned transfer, just frozen: active (kept by
    // "clear finished") but not retryable and not finished.
    CHECK(paused->is_active());
    CHECK_FALSE(paused->is_retryable());
    CHECK_FALSE(paused->finished_at.has_value());

    // Pausing twice is a no-op (already paused).
    CHECK_FALSE(s.pause(id));
    // Progress still lands while paused: Chromium can report one last update
    // after the pause takes effect, and it must not resurrect the row.
    CHECK(s.update_progress(id, 50, 100));
    auto still = s.find(id);
    REQUIRE(still.has_value());
    CHECK(still->state == DownloadState::Paused);
    CHECK(still->received == 50);

    CHECK(s.resume(id));
    auto running = s.find(id);
    REQUIRE(running.has_value());
    CHECK(running->state == DownloadState::InProgress);
    CHECK(running->is_active());
    // Resuming twice is a no-op.
    CHECK_FALSE(s.resume(id));
}

TEST_CASE("a paused download can still reach a terminal state") {
    auto db = open_downloads();
    DownloadService s(db);
    int64_t id = s.begin("https://a.test/g.zip", "/tmp/g.zip", "", 10);
    REQUIRE(id > 0);
    REQUIRE(s.pause(id));

    // The engine can cancel (or complete a nearly done transfer) while it is
    // paused; nothing else would ever move the row out of 'paused', so a
    // terminal engine event must still land.
    CHECK(s.finish(id, DownloadState::Cancelled));
    auto item = s.find(id);
    REQUIRE(item.has_value());
    CHECK(item->state == DownloadState::Cancelled);
    CHECK(item->is_finished());
    CHECK(item->is_retryable());

    // Complete stays monotonic: a late event cannot resurrect or downgrade it.
    CHECK_FALSE(s.finish(id, DownloadState::Complete));
    CHECK_FALSE(s.finish(id, DownloadState::Interrupted));
}

TEST_CASE("finish and pause are inert on terminal rows") {
    auto db = open_downloads();
    DownloadService s(db);
    int64_t id = s.begin("https://a.test/h.zip", "/tmp/h.zip", "", 10);
    REQUIRE(id > 0);
    REQUIRE(s.finish(id, DownloadState::Complete));
    CHECK_FALSE(s.pause(id));
    CHECK_FALSE(s.resume(id));
    // finish(InProgress) is a caller bug and is rejected.
    CHECK_FALSE(s.finish(id, DownloadState::InProgress));
    CHECK_FALSE(s.finish(id, DownloadState::Paused));
}

TEST_CASE("clear_finished keeps paused rows") {
    auto db = open_downloads();
    DownloadService s(db);
    int64_t done = s.begin("https://a.test/1", "/tmp/1", "", 1);
    int64_t paused = s.begin("https://a.test/2", "/tmp/2", "", 1);
    REQUIRE(done > 0);
    REQUIRE(paused > 0);
    REQUIRE(s.finish(done, DownloadState::Complete));
    REQUIRE(s.pause(paused));

    CHECK(s.clear_finished() == 1);
    auto items = s.recent(10);
    REQUIRE(items.size() == 1);
    CHECK(items[0].id == paused);
    CHECK(items[0].state == DownloadState::Paused);
}

TEST_CASE("cancelled and interrupted rows are retryable, others are not") {
    auto db = open_downloads();
    DownloadService s(db);
    int64_t ok = s.begin("https://a.test/ok", "/tmp/ok", "", 1);
    int64_t stopped = s.begin("https://a.test/stop", "/tmp/stop", "", 1);
    int64_t dead = s.begin("https://a.test/dead", "/tmp/dead", "", 1);
    REQUIRE(ok > 0);
    REQUIRE(stopped > 0);
    REQUIRE(dead > 0);
    REQUIRE(s.finish(ok, DownloadState::Complete));
    REQUIRE(s.finish(stopped, DownloadState::Cancelled));
    REQUIRE(s.finish(dead, DownloadState::Interrupted));

    auto item = s.find(ok);
    REQUIRE(item.has_value());
    CHECK_FALSE(item->is_retryable());
    item = s.find(stopped);
    REQUIRE(item.has_value());
    CHECK(item->is_retryable());
    item = s.find(dead);
    REQUIRE(item.has_value());
    CHECK(item->is_retryable());
}
