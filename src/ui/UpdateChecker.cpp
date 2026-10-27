#include "ui/UpdateChecker.hpp"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

#include "settings/Settings.hpp"
#include "updates/ReleaseVersion.hpp"

namespace aurora::ui {
namespace {

constexpr char kApiUrl[] =
    "https://api.github.com/repos/virgil48YT/AuroraBrowser/releases/latest";
constexpr char kReleasesUrl[] =
    "https://github.com/virgil48YT/AuroraBrowser/releases/latest";
constexpr int kApiTimeoutMs = 30000;
constexpr qint64 kMaxJsonBytes = 1024 * 1024;
constexpr qint64 kMaxShaBytes = 64 * 1024;
constexpr qint64 kMaxInstallerBytes = 2ll * 1024ll * 1024ll * 1024ll;
constexpr qint64 kProgressIntervalMs = 100;
constexpr qint64 kAutoCheckIntervalSeconds = 24 * 60 * 60;

bool allowed_download_host(const QUrl& url) {
    if (!url.isValid() || url.scheme().compare(QStringLiteral("https"),
                                               Qt::CaseInsensitive) != 0) {
        return false;
    }
    const QString host = url.host().toLower();
    return host == QStringLiteral("github.com") ||
           host == QStringLiteral("objects.githubusercontent.com") ||
           host == QStringLiteral("release-assets.githubusercontent.com");
}

bool is_github_url(const QUrl& url) {
    if (!url.isValid() || url.scheme().compare(QStringLiteral("https"),
                                               Qt::CaseInsensitive) != 0) {
        return false;
    }
    const QString host = url.host().toLower();
    return host == QStringLiteral("github.com") ||
           host == QStringLiteral("www.github.com");
}

QString user_agent(const QString& version) {
    return QStringLiteral("AuroraBrowser/%1").arg(version);
}

void apply_headers(QNetworkRequest& request, const QString& version,
                   const char* accept) {
    request.setRawHeader("User-Agent", user_agent(version).toUtf8());
    request.setRawHeader("Accept", accept);
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kApiTimeoutMs);
}

bool valid_sha(const QByteArray& value) {
    if (value.size() != 64) return false;
    for (const char c : value) {
        const bool digit = c >= '0' && c <= '9';
        const bool alpha = (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!digit && !alpha) return false;
    }
    return true;
}

quint64 little_endian_u64(const QByteArray& bytes, int offset) {
    quint64 value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8u) |
                static_cast<unsigned char>(bytes.at(offset + i));
    }
    return value;
}

// The release's installer must be the Aurora setup wizard, not an unrelated
// or legacy self-extractor. The wizard writes a 32-byte footer after the ZIP:
// magic, version, reserved, payload offset, payload size.
bool is_custom_setup(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const qint64 file_size = file.size();
    if (file_size < 32 || !file.seek(file_size - 32)) return false;
    const QByteArray footer = file.read(32);
    if (footer.size() != 32 || footer.left(8) != QByteArray("AURSFX01")) {
        return false;
    }
    const quint64 offset = little_endian_u64(footer, 16);
    const quint64 size = little_endian_u64(footer, 24);
    return offset > 0 && size >= 22 &&
           offset + size + 32 == static_cast<quint64>(file_size);
}

QString platform_asset_suffix() {
#if defined(Q_OS_WIN)
    return QStringLiteral("-windows-x64-setup.exe");
#elif defined(Q_OS_MACOS)
    return QStringLiteral(".dmg");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("-linux-x86_64.AppImage");
#else
    return QString();
#endif
}

QString human_size(qint64 bytes) {
    if (bytes <= 0) return QString();
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb >= 1024.0) {
        return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 MB").arg(mb, 0, 'f', 0);
}

} // namespace

UpdateChecker::UpdateChecker(settings::Settings* prefs, QObject* parent)
    : QObject(parent),
      prefs_(prefs),
      net_(new QNetworkAccessManager(this)) {
#ifdef AURORA_VERSION_STRING
    current_version_ = QString::fromLatin1(AURORA_VERSION_STRING);
#else
    current_version_ = QStringLiteral("0.0.0");
#endif
    if (prefs_ && !prefs_->is_read_only()) {
        const qint64 last = prefs_->get_int(
            settings::PrefKeys::kUpdateLastCheck, 0);
        if (last > 0) {
            const QString when = QLocale().toString(
                QDateTime::fromSecsSinceEpoch(last), QLocale::ShortFormat);
            status_ = QStringLiteral("Last checked %1.").arg(when);
        } else {
            status_ = QStringLiteral("Not checked yet.");
        }
    } else {
        status_ = QStringLiteral("Update checks are unavailable in this window.");
    }
}

UpdateChecker::~UpdateChecker() {
    if (reply_) {
        reply_->abort();
        reply_.clear();
    }
    if (output_) {
        output_->cancelWriting();
        output_.reset();
    }
}

bool UpdateChecker::auto_check_enabled() const {
    return prefs_ && !prefs_->is_read_only() &&
           prefs_->get_bool(settings::PrefKeys::kAutoCheckUpdates,
                            settings::PrefDefaults::kAutoCheckUpdates);
}

bool UpdateChecker::installable() const {
#if defined(Q_OS_WIN)
    return update_available() && !asset_url_.isEmpty() && !sha_url_.isEmpty();
#else
    return false;
#endif
}

void UpdateChecker::set_status(const QString& status, bool notify) {
    status_ = status;
    if (notify) emit state_changed();
}

void UpdateChecker::record_check_time() {
    if (!prefs_ || prefs_->is_read_only()) return;
    prefs_->set_int(settings::PrefKeys::kUpdateLastCheck,
                    QDateTime::currentSecsSinceEpoch());
}

void UpdateChecker::maybe_auto_check() {
    if (!prefs_ || prefs_->is_read_only() || checking_ || downloading_) return;
    if (!auto_check_enabled()) return;
    if (update_available()) return;
    const qint64 last = prefs_->get_int(settings::PrefKeys::kUpdateLastCheck, 0);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (last > 0 && now - last < kAutoCheckIntervalSeconds) return;
    check_now();
}

void UpdateChecker::check_now() {
    if (checking_ || downloading_) return;
    if (prefs_ && prefs_->is_read_only()) {
        set_status(QStringLiteral("Update checks are unavailable in this window."));
        return;
    }
    checking_ = true;
    download_failed_ = false;
    download_canceled_ = false;
    set_status(QStringLiteral("Checking for updates\u2026"));
    request_release();
}

void UpdateChecker::request_release() {
    QNetworkRequest request{QUrl(QString::fromLatin1(kApiUrl))};
    apply_headers(request, current_version_, "application/vnd.github+json");
    QNetworkReply* reply = net_->get(request);
    reply_ = reply;
    connect(reply, &QNetworkReply::downloadProgress, this,
            [reply](qint64 received, qint64) {
                if (received > kMaxJsonBytes) reply->abort();
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply] { on_release_finished(reply); });
}

void UpdateChecker::on_release_finished(QNetworkReply* reply) {
    if (reply_.data() != reply) {
        reply->deleteLater();
        return;
    }
    reply_.clear();
    checking_ = false;
    record_check_time();

    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError network_error = reply->error();
    const QString error_text = reply->errorString();
    const QByteArray body = reply->readAll();
    const QByteArray rate_limit = reply->rawHeader("X-RateLimit-Remaining");
    reply->deleteLater();

    if (network_error != QNetworkReply::NoError || status != 200) {
        if (status == 403 && rate_limit == "0") {
            fail(QStringLiteral("GitHub's API rate limit was reached. Try again later."));
        } else if (status != 0) {
            fail(QStringLiteral("Update check failed (HTTP %1).").arg(status));
        } else {
            fail(QStringLiteral("Update check failed: %1").arg(error_text));
        }
        return;
    }
    if (body.size() > kMaxJsonBytes) {
        fail(QStringLiteral("GitHub returned an oversized response."));
        return;
    }
    apply_release(body);
}

void UpdateChecker::apply_release(const QByteArray& body) {
    QJsonParseError parse_error{};
    const QJsonDocument document = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        fail(QStringLiteral("GitHub returned an invalid release description."));
        return;
    }
    const QJsonObject release = document.object();
    if (release.value(QStringLiteral("draft")).toBool() ||
        release.value(QStringLiteral("prerelease")).toBool()) {
        fail(QStringLiteral("GitHub returned an unexpected release."));
        return;
    }
    const QString tag = release.value(QStringLiteral("tag_name"))
                            .toString()
                            .trimmed();
    if (tag.isEmpty()) {
        fail(QStringLiteral("The latest release has no tag."));
        return;
    }
    const QString html = release.value(QStringLiteral("html_url")).toString();
    const QUrl html_url(html);
    release_url_ = is_github_url(html_url) ? html_url.toString()
                                           : QString::fromLatin1(kReleasesUrl);

    if (!aurora::updates::is_newer_version(tag.toStdString(),
                                           current_version_.toStdString())) {
        available_version_.clear();
        asset_url_.clear();
        sha_url_.clear();
        asset_name_.clear();
        installer_path_.clear();
        installer_version_.clear();
        set_status(QStringLiteral("Aurora %1 is up to date.").arg(current_version_));
        return;
    }

    if (available_version_ != tag) {
        // A previously downloaded installer belongs to the previous version;
        // never offer it as if it were this one.
        installer_path_.clear();
        installer_version_.clear();
    }
    available_version_ = tag;
    asset_url_.clear();
    sha_url_.clear();
    asset_name_.clear();
    asset_size_ = 0;

    const QJsonArray assets = release.value(QStringLiteral("assets")).toArray();
    const QString suffix = platform_asset_suffix();
    for (const QJsonValue& value : assets) {
        if (!value.isObject()) continue;
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        const QString url = asset.value(QStringLiteral("browser_download_url"))
                                .toString();
        if (name.isEmpty() || url.isEmpty()) continue;
        if (!suffix.isEmpty() &&
            name.toLower().endsWith(suffix.toLower()) &&
            allowed_download_host(QUrl(url))) {
            asset_name_ = name;
            asset_url_ = url;
            asset_size_ = static_cast<qint64>(
                asset.value(QStringLiteral("size")).toDouble());
            break;
        }
    }
    if (!asset_url_.isEmpty()) {
        const QString sha_name = asset_name_ + QStringLiteral(".sha256");
        for (const QJsonValue& value : assets) {
            if (!value.isObject()) continue;
            const QJsonObject asset = value.toObject();
            const QString name = asset.value(QStringLiteral("name")).toString();
            const QString url = asset.value(QStringLiteral("browser_download_url"))
                                    .toString();
            if (name.compare(sha_name, Qt::CaseInsensitive) == 0 &&
                allowed_download_host(QUrl(url))) {
                sha_url_ = url;
                break;
            }
        }
    }

    QString status = QStringLiteral("Update %1 is available.").arg(tag);
    if (installable()) {
        const QString size = human_size(asset_size_);
        status = size.isEmpty()
                     ? QStringLiteral("Update %1 is available (%2).")
                           .arg(tag, asset_name_)
                     : QStringLiteral("Update %1 is available (%2).")
                           .arg(tag, size);
    } else if (!asset_url_.isEmpty()) {
        status = QStringLiteral(
            "Update %1 is available, but its SHA-256 file is missing.").arg(tag);
    } else {
        status = QStringLiteral(
            "Update %1 is available. Open the releases page to download it.")
            .arg(tag);
    }
    set_status(status);

    if (notified_version_ != tag) {
        notified_version_ = tag;
        emit update_available(tag, release_url_);
    }
}

void UpdateChecker::download_and_install() {
    if (downloading_ || checking_) return;
    if (prefs_ && prefs_->is_read_only()) {
        set_status(QStringLiteral("Updates cannot be installed from this window."));
        return;
    }
    if (installer_ready() && QFile::exists(installer_path_)) {
        emit ready_to_install(installer_path_, available_version_);
        return;
    }
    if (!installable()) {
        emit failed(QStringLiteral(
            "This release has no verified Windows installer to download."));
        return;
    }
    downloading_ = true;
    download_failed_ = false;
    download_canceled_ = false;
    download_received_ = 0;
    download_total_ = asset_size_;
    expected_sha_.clear();
    hash_.reset();
    set_status(QStringLiteral("Fetching the update checksum\u2026"));
    start_sha_download();
}

void UpdateChecker::start_sha_download() {
    QNetworkRequest request{QUrl(sha_url_)};
    apply_headers(request, current_version_, "text/plain");
    QNetworkReply* reply = net_->get(request);
    reply_ = reply;
    connect(reply, &QNetworkReply::downloadProgress, this,
            [reply](qint64 received, qint64) {
                if (received > kMaxShaBytes) reply->abort();
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply] { on_sha_finished(reply); });
}

void UpdateChecker::on_sha_finished(QNetworkReply* reply) {
    if (reply_.data() != reply) {
        reply->deleteLater();
        return;
    }
    reply_.clear();
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError network_error = reply->error();
    const QByteArray body = reply->readAll();
    reply->deleteLater();
    if (download_canceled_) {
        downloading_ = false;
        set_status(QStringLiteral("Update download canceled."));
        return;
    }
    if (network_error != QNetworkReply::NoError || status != 200 ||
        body.size() > kMaxShaBytes) {
        abort_download(QStringLiteral("The update checksum could not be downloaded."));
        return;
    }
    const QByteArray first_line = body.left(200).trimmed();
    const QByteArray token = first_line.split(' ').value(0).trimmed();
    if (!valid_sha(token)) {
        abort_download(QStringLiteral("The release's SHA-256 file is malformed."));
        return;
    }
    expected_sha_ = token.toLower();
    start_installer_download();
}

void UpdateChecker::start_installer_download() {
    QString directory =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (directory.isEmpty()) directory = QDir::tempPath();
    const QString target_dir = QDir(directory).filePath(QStringLiteral("AuroraUpdate"));
    if (!QDir().mkpath(target_dir)) {
        abort_download(QStringLiteral("The update folder could not be created."));
        return;
    }
    QString file_name = QFileInfo(asset_name_).fileName();
    for (int i = 0; i < file_name.size(); ++i) {
        const QChar character = file_name.at(i);
        if (!character.isLetterOrNumber() && character != QLatin1Char('.') &&
            character != QLatin1Char('-') && character != QLatin1Char('_')) {
            file_name[i] = QLatin1Char('_');
        }
    }
    if (file_name.isEmpty()) file_name = QStringLiteral("Aurora-setup.exe");
    const QString path = QDir(target_dir).filePath(file_name);

    output_ = std::make_unique<QSaveFile>(path);
    if (!output_->open(QIODevice::WriteOnly)) {
        abort_download(QStringLiteral("The update file could not be created: %1")
                           .arg(output_->errorString()));
        return;
    }
    hash_.reset();
    download_received_ = 0;
    if (download_total_ <= 0) download_total_ = asset_size_;
    set_status(QStringLiteral("Downloading update %1\u2026").arg(available_version_));

    QNetworkRequest request{QUrl(asset_url_)};
    apply_headers(request, current_version_, "application/octet-stream");
    QNetworkReply* reply = net_->get(request);
    reply_ = reply;
    connect(reply, &QNetworkReply::readyRead, this,
            [this, reply] { on_installer_ready_read(reply); });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                on_installer_progress(received, total);
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply] { on_installer_finished(reply); });
}

void UpdateChecker::on_installer_ready_read(QNetworkReply* reply) {
    if (download_failed_ || download_canceled_ || !output_) return;
    while (reply->bytesAvailable() > 0) {
        const QByteArray chunk = reply->read(256 * 1024);
        if (chunk.isEmpty()) break;
        if (download_received_ + chunk.size() > kMaxInstallerBytes) {
            abort_download(QStringLiteral("The update is larger than expected."));
            return;
        }
        if (asset_size_ > 0 &&
            download_received_ + chunk.size() > asset_size_ + 1024 * 1024) {
            abort_download(QStringLiteral("The update is larger than the release asset."));
            return;
        }
        if (output_->write(chunk) != chunk.size()) {
            abort_download(QStringLiteral("The update file could not be written."));
            return;
        }
        hash_.addData(chunk);
        download_received_ += chunk.size();
    }
    emit_progress(false);
}

void UpdateChecker::on_installer_progress(qint64 received, qint64 total) {
    if (total > 0) download_total_ = total;
    if (download_total_ <= 0) download_total_ = asset_size_;
    if (received > download_received_) download_received_ = received;
    emit_progress(false);
}

void UpdateChecker::on_installer_finished(QNetworkReply* reply) {
    if (reply_.data() != reply) {
        reply->deleteLater();
        return;
    }
    on_installer_ready_read(reply);
    reply_.clear();
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError network_error = reply->error();
    reply->deleteLater();

    if (download_failed_) {
        download_failed_ = false;
        return;
    }
    if (download_canceled_) {
        download_canceled_ = false;
        downloading_ = false;
        if (output_) {
            output_->cancelWriting();
            output_.reset();
        }
        set_status(QStringLiteral("Update download canceled."));
        return;
    }
    if (network_error != QNetworkReply::NoError || status != 200) {
        abort_download(QStringLiteral("The update download failed (HTTP %1).")
                           .arg(status));
        return;
    }
    if (!output_) {
        downloading_ = false;
        fail(QStringLiteral("The update file was lost."));
        return;
    }
    if (!output_->commit()) {
        const QString message = output_->errorString();
        output_.reset();
        downloading_ = false;
        fail(QStringLiteral("The update file could not be saved: %1").arg(message));
        return;
    }
    const QString path = output_->fileName();
    output_.reset();
    downloading_ = false;

    const QByteArray actual = hash_.result().toHex();
    if (actual != expected_sha_) {
        QFile::remove(path);
        fail(QStringLiteral(
            "The downloaded update did not match its published SHA-256."));
        return;
    }
    if (!is_custom_setup(path)) {
        QFile::remove(path);
        fail(QStringLiteral(
            "The release's installer is not an Aurora setup wizard."));
        return;
    }
    installer_path_ = path;
    installer_version_ = available_version_;
    emit_progress(true);
    set_status(QStringLiteral("Update %1 was downloaded and verified.")
                   .arg(available_version_));
    emit ready_to_install(installer_path_, available_version_);
}

void UpdateChecker::emit_progress(bool force) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && now - last_progress_ms_ < kProgressIntervalMs) return;
    last_progress_ms_ = now;
    emit download_progress(download_received_, download_total_);
}

void UpdateChecker::abort_download(const QString& message) {
    downloading_ = false;
    download_failed_ = true;
    if (output_) {
        output_->cancelWriting();
        output_.reset();
    }
    if (reply_) {
        QNetworkReply* reply = reply_.data();
        reply_.clear();
        reply->abort();
        reply->deleteLater();
    }
    status_ = message;
    emit failed(message);
    emit state_changed();
}

void UpdateChecker::cancel_download() {
    if (!downloading_) return;
    download_canceled_ = true;
    downloading_ = false;
    if (output_) {
        output_->cancelWriting();
        output_.reset();
    }
    if (reply_) {
        QNetworkReply* reply = reply_.data();
        reply_.clear();
        reply->abort();
        reply->deleteLater();
    }
    set_status(QStringLiteral("Update download canceled."));
}

void UpdateChecker::open_release_page() {
    QUrl url = release_url_.isEmpty() ? QUrl(QString::fromLatin1(kReleasesUrl))
                                      : QUrl(release_url_);
    if (!is_github_url(url)) url = QUrl(QString::fromLatin1(kReleasesUrl));
    QDesktopServices::openUrl(url);
}

void UpdateChecker::fail(const QString& message) {
    checking_ = false;
    downloading_ = false;
    if (output_) {
        output_->cancelWriting();
        output_.reset();
    }
    if (reply_) {
        QNetworkReply* reply = reply_.data();
        reply_.clear();
        reply->abort();
        reply->deleteLater();
    }
    status_ = message;
    emit failed(message);
    emit state_changed();
}

} // namespace aurora::ui
