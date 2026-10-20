#include "ui/TorLauncher.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

#include <filesystem>

#include "tor/TorBundle.hpp"

namespace aurora::ui {

namespace {

// Bootstrapping on a slow or censored network can take a while; after this
// long without reaching 100% the attempt is reported as failed.
constexpr int kBootstrapTimeoutMs = 180 * 1000;

std::filesystem::path to_path(const QString& s) {
#if defined(_WIN32)
    return std::filesystem::path(s.toStdWString());
#else
    return std::filesystem::path(s.toStdString());
#endif
}

QString from_path(const std::filesystem::path& p) {
#if defined(_WIN32)
    return QString::fromStdWString(p.wstring());
#else
    return QString::fromStdString(p.string());
#endif
}

} // namespace

TorLauncher& TorLauncher::instance() {
    // Parented to the application so it is torn down with it (a plain
    // function-local static would outlive QCoreApplication).
    static TorLauncher* self = new TorLauncher(QCoreApplication::instance());
    return *self;
}

TorLauncher::TorLauncher(QObject* parent) : QObject(parent) {
    if (auto found = tor::find_bundled_tor(to_path(QCoreApplication::applicationDirPath()))) {
        tor_path_ = from_path(*found);
    }
    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, [this] {
        if (state_ == State::Starting) {
            fail(tr("Tor did not finish connecting within %1 minutes. "
                    "Check your network connection, or that Tor is not blocked on it.")
                     .arg(kBootstrapTimeoutMs / 60000));
        }
    });
    if (QCoreApplication::instance()) {
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &TorLauncher::stop);
    }
}

std::string TorLauncher::proxy() const {
    if (state_ != State::Ready || socks_port_ == 0) return {};
    return tor::socks_proxy_url(socks_port_);
}

void TorLauncher::start() {
    if (state_ == State::Starting || state_ == State::Ready) return;
    if (tor_path_.isEmpty()) {
        fail(tr("This build of Aurora does not include the built-in Tor."));
        return;
    }

    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString dir = QDir(base.isEmpty() ? QDir::tempPath() : base).filePath(QStringLiteral("tor"));
    const QString data_dir = QDir(dir).filePath(QStringLiteral("data"));
    if (!QDir().mkpath(data_dir)) {
        fail(tr("Could not create the Tor data folder %1.").arg(QDir::toNativeSeparators(data_dir)));
        return;
    }
#if !defined(_WIN32)
    // tor refuses a DataDirectory that other users can read.
    QFile::setPermissions(data_dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif

    tor::TorrcOptions opts;
    opts.socks_port = 0; // tor picks a free loopback port; read back from its log
    opts.data_directory = to_path(data_dir);
    const std::filesystem::path geo = tor::bundled_data_dir(to_path(tor_path_));
    std::error_code ec;
    if (std::filesystem::is_regular_file(geo / "geoip", ec)) opts.geoip_file = geo / "geoip";
    if (std::filesystem::is_regular_file(geo / "geoip6", ec)) opts.geoip6_file = geo / "geoip6";
    opts.owning_pid = QCoreApplication::applicationPid();

    const QString torrc = QDir(dir).filePath(QStringLiteral("torrc"));
    const QString defaults = QDir(dir).filePath(QStringLiteral("torrc-defaults"));
    {
        QFile f(torrc);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            fail(tr("Could not write %1.").arg(QDir::toNativeSeparators(torrc)));
            return;
        }
        f.write(QByteArray::fromStdString(tor::build_torrc(opts)));
    }
    {
        // An empty defaults file, so a system-wide torrc-defaults (or the
        // Expert Bundle's Tor Browser one) can never change our config.
        QFile f(defaults);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            f.write("# Intentionally empty: Aurora's built-in Tor uses only its torrc.\n");
        }
    }

    delete process_;
    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::MergedChannels);
    const QString tor_dir = QFileInfo(tor_path_).absolutePath();
    process_->setWorkingDirectory(tor_dir);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
#if defined(__linux__)
    // The Expert Bundle ships libevent/OpenSSL next to tor without an RPATH.
    const QString ld = env.value(QStringLiteral("LD_LIBRARY_PATH"));
    env.insert(QStringLiteral("LD_LIBRARY_PATH"), ld.isEmpty() ? tor_dir : tor_dir + QLatin1Char(':') + ld);
#elif defined(__APPLE__)
    const QString dyld = env.value(QStringLiteral("DYLD_LIBRARY_PATH"));
    env.insert(QStringLiteral("DYLD_LIBRARY_PATH"), dyld.isEmpty() ? tor_dir : tor_dir + QLatin1Char(':') + dyld);
#endif
    process_->setProcessEnvironment(env);

    connect(process_, &QProcess::readyReadStandardOutput, this, &TorLauncher::on_output);
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
        if (err == QProcess::FailedToStart) {
            fail(tr("The built-in Tor could not be started (%1).").arg(process_ ? process_->errorString() : QString()));
        }
    });
    connect(process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        // Stopped: we asked it to exit. Failed: fail() already reported why.
        if (state_ == State::Stopped || state_ == State::Failed) return;
        fail(error_.isEmpty() ? tr("Tor exited unexpectedly (code %1). %2").arg(code).arg(summary_) : error_);
    });

    state_ = State::Starting;
    progress_ = 0;
    socks_port_ = 0;
    summary_ = tr("Starting Tor");
    error_.clear();
    pending_.clear();
    emit progress_changed(progress_, summary_);
    timeout_.start(kBootstrapTimeoutMs);
    process_->start(tor_path_, {QStringLiteral("-f"), torrc, QStringLiteral("--defaults-torrc"), defaults});
}

void TorLauncher::stop() {
    timeout_.stop();
    const bool was_running = state_ == State::Starting || state_ == State::Ready;
    state_ = State::Stopped;
    socks_port_ = 0;
    progress_ = 0;
    if (process_ && process_->state() != QProcess::NotRunning) {
        process_->terminate(); // SIGTERM: tor shuts down cleanly
        if (!process_->waitForFinished(3000)) {
            process_->kill();
            process_->waitForFinished(1000);
        }
    }
    if (was_running) emit progress_changed(0, tr("Tor stopped"));
}

void TorLauncher::on_output() {
    if (!process_) return;
    pending_ += process_->readAllStandardOutput();
    qsizetype nl = 0;
    while ((nl = pending_.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(pending_.left(nl)).trimmed();
        pending_.remove(0, nl + 1);
        if (!line.isEmpty()) handle_line(line);
    }
    if (pending_.size() > 64 * 1024) pending_.clear(); // runaway line guard
}

void TorLauncher::handle_line(const QString& line) {
    const std::string raw = line.toStdString();
    if (auto port = tor::parse_socks_listener_line(raw)) socks_port_ = *port;
    if (line.contains(QLatin1String("[err]"))) error_ = line.section(QLatin1String("[err]"), 1).trimmed();
    auto boot = tor::parse_bootstrap_line(raw);
    if (!boot || state_ != State::Starting) return;
    progress_ = boot->percent;
    summary_ = QString::fromStdString(boot->summary);
    emit progress_changed(progress_, summary_);
    if (progress_ >= 100) {
        if (socks_port_ == 0) {
            fail(tr("Tor connected but did not report its SOCKS port."));
            return;
        }
        timeout_.stop();
        state_ = State::Ready;
        emit ready(QString::fromStdString(proxy()));
    }
}

void TorLauncher::fail(const QString& why) {
    timeout_.stop();
    const bool running = process_ && process_->state() != QProcess::NotRunning;
    state_ = State::Failed;
    socks_port_ = 0;
    error_ = why;
    if (running) {
        process_->kill();
        process_->waitForFinished(1000);
    }
    emit failed(why);
}

} // namespace aurora::ui
