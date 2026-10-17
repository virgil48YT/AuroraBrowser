// Aurora — Qt host for the in-app filter-list updater.
//
// adblock::ListUpdater (aurora_core) owns the policy: validate, atomically
// swap adblock-rules.txt, keep a backup, roll back. It is Qt-free, so it
// cannot download anything itself; this object supplies the transport
// (QNetworkAccessManager, configured like WeatherService's requests) and the
// threading:
//
//   * the GET runs on the UI thread's event loop (asynchronous, never blocks);
//   * validation + the file swap + re-reading the combined rule text run on a
//     private worker pool, because validation lets the Rust engine parse the
//     whole list (EasyList takes a noticeable fraction of a second);
//   * the result, and the prefs bookkeeping (last-success time, source URL),
//     come back to the UI thread, which owns the Settings object.
//
// One instance per process, owned by main.cpp and lent to every normal
// window through ServiceBundle::filter_lists (private windows get null). The
// rules it holds are the CURRENT combined list (starter rules + the profile
// file), so windows opened after an update build their engine from the new
// list instead of the text main.cpp loaded at startup.
//
// Nothing here runs on a timer: an update starts only from start(), which the
// settings page's "Update now" button triggers.
#pragma once

#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "adblock/ListUpdater.hpp"

class QNetworkAccessManager;
class QThreadPool;

namespace aurora::settings { class Settings; }

namespace aurora::ui {

class FilterListUpdater : public QObject {
    Q_OBJECT
public:
    // `profile_dir` is where adblock-rules.txt lives; `rule_paths` are the
    // load_rules() candidates main.cpp used at startup (re-read after an
    // update so the legacy fallback keeps working); `initial_rules` is what
    // main.cpp already loaded. `prefs` may be null (no bookkeeping then).
    FilterListUpdater(const QString& profile_dir, settings::Settings* prefs,
                      std::vector<std::string> rule_paths,
                      std::string initial_rules, QObject* parent = nullptr);
    ~FilterListUpdater() override;

    bool busy() const { return busy_; }
    QString source_url() const;
    // Last successful in-app update (0 = never), from the prefs store.
    adblock::UpdateRecord last_update() const;

    // The combined rule text the engine should run. Bumped by every
    // successful update; engine_current() tells a window whether the shared
    // engine was already rebuilt from it (the CEF engine is process-wide, so
    // one rebuild serves every window).
    const std::string& rules() const { return rules_; }
    uint64_t rules_generation() const { return generation_; }
    bool engine_current() const { return engine_generation_ == generation_; }
    void mark_engine_current() { engine_generation_ = generation_; }

public slots:
    // Download, validate and install the list. Ignored (with a finished()
    // failure) while an update is already running.
    void start();

signals:
    void busy_changed(bool busy);
    // `message` is user-facing ("Filter list updated: 71234 rules").
    void finished(bool ok, const QString& message);

private:
    void fetch(const std::string& url, adblock::ListUpdater::FetchDone done);
    void complete(const adblock::ListUpdater::Result& result,
                  std::string new_rules);

    adblock::ListUpdater updater_;
    settings::Settings* prefs_ = nullptr;
    std::vector<std::string> rule_paths_;
    std::string rules_;
    uint64_t generation_ = 0;
    uint64_t engine_generation_ = 0;
    bool busy_ = false;
    QNetworkAccessManager* net_ = nullptr;
    // Declared last so it is destroyed first: its destructor waits for a
    // running install, which still uses updater_ and rule_paths_.
    std::unique_ptr<QThreadPool> pool_;
};

} // namespace aurora::ui
