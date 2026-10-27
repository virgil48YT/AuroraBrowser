// Aurora — in-app update checker.
//
// Policy:
//   * Ask GitHub for the latest non-draft, non-prerelease release.
//   * Compare its tag with the version compiled into this build; offer it only
//     when it is strictly newer.
//   * Automatic checks are a preference and run at most once a day. A manual
//     check always works.
//   * Nothing downloads or installs without an explicit user action.
//   * The downloaded installer is verified against the release's .sha256
//     asset before it is handed to the setup wizard. A missing checksum is a
//     failed update, not a prompt to skip verification.
#pragma once

#include <QCryptographicHash>
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;

namespace aurora::settings { class Settings; }

namespace aurora::ui {

class UpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(settings::Settings* prefs, QObject* parent = nullptr);
    ~UpdateChecker() override;

    bool auto_check_enabled() const;
    bool checking() const { return checking_; }
    bool downloading() const { return downloading_; }
    bool update_available() const { return !available_version_.isEmpty(); }
    // Windows only: a setup asset and its .sha256 were published.
    bool installable() const;
    bool installer_ready() const {
        return !installer_path_.isEmpty() &&
               installer_version_ == available_version_;
    }

    QString current_version() const { return current_version_; }
    QString available_version() const { return available_version_; }
    QString release_url() const { return release_url_; }
    QString installer_path() const { return installer_path_; }
    QString status_text() const { return status_; }
    qint64 download_received() const { return download_received_; }
    qint64 download_total() const { return download_total_; }

    // Called once after startup; respects the preference and the 24-hour
    // throttle. Never touches the network when automatic checks are off.
    void maybe_auto_check();
    void check_now();
    void download_and_install();
    void cancel_download();
    void open_release_page();

signals:
    void state_changed();
    // Emitted once per version per process so the window can flash a notice
    // without repeating it every check.
    void update_available(const QString& version, const QString& release_url);
    void download_progress(qint64 received, qint64 total);
    void ready_to_install(const QString& installer_path,
                          const QString& version);
    void failed(const QString& message);

private:
    void request_release();
    void on_release_finished(QNetworkReply* reply);
    void apply_release(const QByteArray& body);
    void start_sha_download();
    void on_sha_finished(QNetworkReply* reply);
    void start_installer_download();
    void on_installer_ready_read(QNetworkReply* reply);
    void on_installer_progress(qint64 received, qint64 total);
    void on_installer_finished(QNetworkReply* reply);
    void emit_progress(bool force);
    void record_check_time();
    void set_status(const QString& status, bool notify = true);
    void fail(const QString& message);
    void abort_download(const QString& message);

    settings::Settings* prefs_ = nullptr;
    QNetworkAccessManager* net_ = nullptr;
    QPointer<QNetworkReply> reply_;
    std::unique_ptr<QSaveFile> output_;

    QString current_version_;
    QString status_;
    QString available_version_;
    QString release_url_;
    QString asset_url_;
    QString sha_url_;
    QString asset_name_;
    QString installer_path_;
    QString installer_version_;
    QString notified_version_;
    QByteArray expected_sha_;
    QCryptographicHash hash_{QCryptographicHash::Sha256};

    qint64 asset_size_ = 0;
    qint64 download_received_ = 0;
    qint64 download_total_ = 0;
    qint64 last_progress_ms_ = 0;

    bool checking_ = false;
    bool downloading_ = false;
    bool download_failed_ = false;
    bool download_canceled_ = false;
};

} // namespace aurora::ui
