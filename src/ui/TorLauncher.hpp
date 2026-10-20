// Aurora — runs the built-in (bundled) tor daemon for Tor windows.
//
// Brave-style Tor: when the package ships the Tor Expert Bundle (copied next
// to the browser as tor/ by scripts/fetch_tor.sh + the build), "New Tor
// Incognito Window" starts that tor on demand — no system Tor, Orbot or Tor
// Browser needed. The engine-independent pieces (binary lookup, torrc, log
// parsing) live in tor/TorBundle; this object owns the QProcess:
//
//   * torrc + state live in <AppLocalData>/tor (the consensus cache makes
//     later starts fast; no browsing data is stored there);
//   * tor picks a free loopback SOCKS port, read back from its log;
//   * bootstrap progress is parsed from tor's stdout and re-emitted;
//   * __OwningControllerProcess ties tor to the browser's PID, so it exits
//     with Aurora even if the browser crashes; aboutToQuit stops it cleanly.
//
// One instance per process (instance()), parented to the application.
// Tor windows only ever get the proxy once tor reports 100% bootstrap; if tor
// dies afterwards, their requests fail (the proxy is gone) rather than
// falling back to a direct connection.
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <string>

class QProcess;

namespace aurora::ui {

class TorLauncher : public QObject {
    Q_OBJECT
public:
    enum class State { Stopped, Starting, Ready, Failed };

    static TorLauncher& instance();

    // Path of the bundled tor executable, or empty when this build ships none.
    QString bundled_tor_path() const { return tor_path_; }
    bool has_bundled_tor() const { return !tor_path_.isEmpty(); }

    State state() const { return state_; }
    int progress() const { return progress_; }
    QString summary() const { return summary_; }
    QString last_error() const { return error_; }
    // "socks5://127.0.0.1:PORT" once Ready, else empty.
    std::string proxy() const;

    // Starts tor if it is not already starting/running. Idempotent.
    void start();
    // Stops tor (Tor windows lose connectivity; they never fall back).
    void stop();

signals:
    void progress_changed(int percent, const QString& summary);
    void ready(const QString& proxy);
    void failed(const QString& why);

private:
    explicit TorLauncher(QObject* parent);
    void on_output();
    void handle_line(const QString& line);
    void fail(const QString& why);

    QString tor_path_;
    QProcess* process_ = nullptr;
    QTimer timeout_;
    QByteArray pending_;
    State state_ = State::Stopped;
    int progress_ = 0;
    uint16_t socks_port_ = 0;
    QString summary_;
    QString error_;
};

} // namespace aurora::ui
