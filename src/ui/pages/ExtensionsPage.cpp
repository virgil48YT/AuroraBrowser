#include "ui/pages/ExtensionsPage.hpp"

#include <QCheckBox>
#include <QEvent>
#include <QFileDialog>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QTimer>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <functional>
#include <utility>

#include "extensions/ExtensionService.hpp"
#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"
#include "ui/ToggleSwitch.hpp"

namespace aurora::ui {

namespace {

// Card artwork size in logical pixels.
constexpr int kIconPx = 36;

// The extension name doubles as the enable/disable label. QLabel is not
// interactive by default, so keep the small behavior here instead of making
// every card install an event filter. The color is refreshed on StyleChange:
// MainWindow reapplies the application stylesheet when the theme changes.
class ExtensionToggleLabel final : public QLabel {
public:
    explicit ExtensionToggleLabel(const QString& text, QWidget* parent)
        : QLabel(text, parent) {
        setCursor(Qt::PointingHandCursor);
    }

    void set_checked(bool checked) {
        checked_ = checked;
        apply_color();
    }

    void set_toggle_callback(std::function<void()> callback) {
        toggle_ = std::move(callback);
    }

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton &&
            rect().contains(event->position().toPoint())) {
            if (toggle_) toggle_();
            event->accept();
            return;
        }
        QLabel::mouseReleaseEvent(event);
    }

    void changeEvent(QEvent* event) override {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::StyleChange ||
            event->type() == QEvent::PaletteChange) {
            apply_color();
        }
    }

private:
    void apply_color() {
        if (applying_color_) return;
        applying_color_ = true;
        const QColor color = checked_ ? current_theme().colors.text
                                      : current_theme().colors.text_faint;
        setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
        applying_color_ = false;
    }

    std::function<void()> toggle_;
    bool checked_ = true;
    bool applying_color_ = false;
};

// The extension's folder path, re-elided every time the card is resized.
//
// The old code elided once against a hard-coded 440px width. That width is
// wrong at every other window width and device pixel ratio: on a narrow card
// the pre-elided string still did not fit, so the label reported a size hint
// wider than the viewport, the card grew past it and — because the list has
// horizontal scrolling disabled — the trailing controls (Enabled / Remove)
// were pushed out of view instead of rendering.
//
// Eliding against the label's real width fixes the sizing, and an Ignored
// horizontal size policy keeps a long path from dictating the card's width in
// the first place. The untruncated path stays available as the tooltip.
class ElidedPathLabel final : public QLabel {
public:
    ElidedPathLabel(const QString& path, QWidget* parent)
        : QLabel(parent), full_(path) {
        setToolTip(path);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(80);
        apply_elide();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        apply_elide();
    }

    // The card's font size comes from the application stylesheet, which is
    // applied (and reapplied on a theme change) after construction. Re-elide
    // whenever it lands, otherwise the truncation is measured with the wrong
    // metrics and the path either overflows or is cut too short.
    void changeEvent(QEvent* event) override {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::FontChange ||
            event->type() == QEvent::StyleChange) {
            apply_elide();
        }
    }

private:
    void apply_elide() {
        const int budget = qMax(40, width());
        setText(fontMetrics().elidedText(full_, Qt::ElideMiddle, budget));
    }

    QString full_;
};

// Deterministic stand-in artwork for an extension whose manifest declares no
// icon (or whose icon file is missing): the initial of its real name on a
// rounded tile, tinted by a stable hash of the extension's folder path. The
// same extension therefore always gets the same tile — this is the one place
// a placeholder is allowed, and it is never random.
QPixmap monogram_icon(const QString& name, const QString& seed, qreal dpr) {
    const QChar initial =
        name.isEmpty() ? QChar('?') : name.at(0).toUpper();
    const auto hash = static_cast<uint>(qHash(seed.isEmpty() ? name : seed));
    QColor tile = QColor::fromHsv(static_cast<int>(hash % 360u), 90, 150);

    QPixmap pm(QSize(kIconPx, kIconPx) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    QPainterPath path;
    path.addRoundedRect(QRectF(0.5, 0.5, kIconPx - 1.0, kIconPx - 1.0),
                        Radius::lg, Radius::lg);
    p.fillPath(path, tile);
    QFont f = QGuiApplication::font();
    f.setPixelSize(kIconPx / 2);
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.setPen(QColor(255, 255, 255, 235));
    p.drawText(QRectF(0, 0, kIconPx, kIconPx), Qt::AlignCenter,
               QString(initial));
    return pm;
}

// The extension's own icon file, scaled to the card size. Returns a null
// pixmap when the file cannot be decoded, so the caller can fall back.
QPixmap manifest_icon(const QString& file, qreal dpr) {
    if (file.isEmpty()) return {};
    QPixmap source(file);
    if (source.isNull()) return {};
    QPixmap scaled = source.scaled(QSize(kIconPx, kIconPx) * dpr,
                                   Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(dpr);
    return scaled;
}

} // namespace

ExtensionsPage::ExtensionsPage(extensions::ExtensionService* service,
                              QWidget* parent)
    : InternalPageWidget(QStringLiteral("Extensions"), parent),
      service_(service) {
    set_subtitle(QStringLiteral(
        "Aurora runs Chromium, so real Chrome extensions work. Changes apply "
        "the next time Aurora starts."));

    toolbar_row()->addStretch(1);

    auto* load_btn = new QPushButton(QStringLiteral("Load unpacked…"), this);
    load_btn->setObjectName("PageButton");
    load_btn->setToolTip(
        QStringLiteral("Choose a folder that contains a manifest.json"));
    toolbar_row()->addWidget(load_btn);

    // Packaged installs route through the same extraction/registration path
    // used by Chrome Web Store downloads.
    auto* crx_btn = new QPushButton(QStringLiteral("Install .crx…"), this);
    crx_btn->setObjectName("PageButton");
    crx_btn->setToolTip(
        QStringLiteral("Choose a packaged .crx extension file"));
    toolbar_row()->addWidget(crx_btn);

    // Opens in an in-app tab via open_url_requested (wired by MainWindow to
    // open_url_in_new_tab). Never QDesktopServices::openUrl — that is what
    // dumped the store into external Chromium.
    auto* store_btn = new QPushButton(QStringLiteral("Chrome Web Store"), this);
    store_btn->setObjectName("PageButton");
    store_btn->setToolTip(
        QStringLiteral("Browse the Chrome Web Store in Aurora"));
    toolbar_row()->addWidget(store_btn);

    // The card list and the policy hint share the page body. Cards replaced a
    // table: an extension is an identity (artwork + name + version), not a row
    // of cells, and per-card controls remove the "select a row, then press the
    // toolbar button" indirection.
    auto* body = new QWidget(this);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(Spacing::sm);

    scroll_ = new QScrollArea(body);
    scroll_->setObjectName("ExtensionsScroll");
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    if (scroll_->viewport()) scroll_->viewport()->setAutoFillBackground(false);

    card_host_ = new QWidget(scroll_);
    card_host_->setObjectName("ExtensionsList");
    cards_ = new QVBoxLayout(card_host_);
    cards_->setContentsMargins(0, 0, 0, 0);
    cards_->setSpacing(Spacing::sm);
    scroll_->setWidget(card_host_);
    body_layout->addWidget(scroll_, 1);

    auto* hint = new QLabel(
        QStringLiteral("Install a packaged .crx or load a folder containing "
                       "manifest.json. Changes apply after restart."),
        body);
    hint->setObjectName("PageHint");
    hint->setWordWrap(true);
    body_layout->addWidget(hint);

    set_body(body);

    connect(load_btn, &QPushButton::clicked, this,
            &ExtensionsPage::load_unpacked);
    connect(crx_btn, &QPushButton::clicked, this,
            &ExtensionsPage::install_crx);
    connect(store_btn, &QPushButton::clicked, this,
            &ExtensionsPage::open_web_store);
}

void ExtensionsPage::clear_cards() {
    while (auto* item = cards_->takeAt(0)) {
        if (auto* w = item->widget()) {
            // Detach and hide NOW, delete later. refresh() can run from a
            // card's own signal handler (the Remove button), so the widget
            // cannot be destroyed synchronously — but a deleteLater()'d card
            // that is still parented to card_host_ keeps painting until the
            // next event-loop turn and overlaps the cards rebuilt right after
            // this loop, which is what made the list render as doubled or
            // garbled rows. setParent(nullptr) takes it out of the host and
            // hides it immediately.
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete item;
    }
}

void ExtensionsPage::refresh() {
    clear_cards();
    if (!service_) {
        set_empty_state(true,
                        QStringLiteral("No profile is attached to this window, "
                                       "so extensions are unavailable."));
        return;
    }

    // all_detailed() re-reads each manifest.json, so a card shows the name,
    // version and artwork the extension itself declares today.
    const auto items = service_->all_detailed();
    for (const auto& e : items) add_card(e);
    cards_->addStretch(1);

    if (items.empty()) {
        set_empty_state(true,
                        QStringLiteral("No extensions yet. Use “Load "
                                       "unpacked…” to add a folder that "
                                       "contains a manifest.json."));
    } else {
        set_empty_state(false);
    }
}

void ExtensionsPage::add_card(const extensions::Extension& e) {
    const qreal dpr = screen() ? screen()->devicePixelRatio()
                               : qApp->devicePixelRatio();
    const QString name = QString::fromStdString(e.name);
    const QString path = QString::fromStdString(e.path);

    auto* card = new QWidget(card_host_);
    card->setObjectName("ExtensionCard");
    card->setAttribute(Qt::WA_StyledBackground, true);
    // Fill the viewport width, take only the height the content needs. Without
    // an explicit policy the word-wrapped description could be laid out for a
    // width the card never gets, and its wrapped lines were clipped.
    card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    auto* row = new QHBoxLayout(card);
    row->setContentsMargins(Spacing::md, Spacing::md, Spacing::md, Spacing::md);
    row->setSpacing(Spacing::md);

    // --- Artwork: the manifest's icon, else a deterministic monogram --------
    auto* icon = new QLabel(card);
    icon->setObjectName("ExtensionCardIcon");
    icon->setFixedSize(kIconPx, kIconPx);
    icon->setAlignment(Qt::AlignCenter);
    QPixmap art = manifest_icon(QString::fromStdString(e.icon_file), dpr);
    if (art.isNull()) art = monogram_icon(name, path, dpr);
    icon->setPixmap(art);
    row->addWidget(icon, 0, Qt::AlignTop);

    // --- Identity: name + version, description, folder ---------------------
    auto* text = new QVBoxLayout();
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(Spacing::xxs);

    auto* title_row = new QHBoxLayout();
    title_row->setContentsMargins(0, 0, 0, 0);
    title_row->setSpacing(Spacing::sm);
    auto* name_label = new ExtensionToggleLabel(name, card);
    name_label->setObjectName("ExtensionCardName");
    title_row->addWidget(name_label);
    if (!e.version.empty()) {
        auto* version = new QLabel(QString::fromStdString(e.version), card);
        version->setObjectName("ExtensionCardVersion");
        title_row->addWidget(version);
    }
    title_row->addStretch(1);
    text->addLayout(title_row);

    if (!e.description.empty()) {
        auto* desc = new QLabel(QString::fromStdString(e.description), card);
        desc->setObjectName("ExtensionCardMeta");
        desc->setWordWrap(true);
        text->addWidget(desc);
    }
    // Elide in the middle: the interesting parts of an extension path are its
    // drive/root and its folder name, not the middle of the tree. The label
    // re-elides against its own width, so the path never forces the card wider
    // than the list viewport.
    auto* folder = new ElidedPathLabel(path, card);
    folder->setObjectName("ExtensionCardPath");
    text->addWidget(folder);
    row->addLayout(text, 1);

    // --- Controls: enable switch + remove ----------------------------------
    auto* controls = new QVBoxLayout();
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setSpacing(Spacing::sm);
    auto* enabled = new ToggleSwitch(QStringLiteral("Enabled"), card);
    enabled->setObjectName("PageCheck");
    enabled->setChecked(e.enabled);
    name_label->set_checked(e.enabled);
    name_label->setToolTip(QStringLiteral("Click to enable or disable %1").arg(name));
    name_label->set_toggle_callback([enabled] { enabled->toggle(); });
    controls->addWidget(enabled, 0, Qt::AlignRight);
    auto* options = new QPushButton(QStringLiteral("Options"), card);
    options->setObjectName("PageButton");
    options->setToolTip(QStringLiteral("Open options in Aurora"));
    controls->addWidget(options, 0, Qt::AlignRight);
    auto* remove = new QPushButton(QStringLiteral("Remove"), card);
    remove->setObjectName("PageDangerButton");
    controls->addWidget(remove, 0, Qt::AlignRight);
    controls->addStretch(1);
    row->addLayout(controls, 0);

    const int64_t id = e.id;
    connect(options, &QPushButton::clicked, this, [this, id]() {
        if (!service_) return;
        // chrome-extension:// URL, opened in-app through open_url_requested.
        const std::string url = service_->extension_options_url(id);
        emit open_url_requested(url.empty()
                                    ? QStringLiteral("aurora://extensions")
                                    : QString::fromStdString(url));
    });
    connect(enabled, &QCheckBox::toggled, this,
            [this, id, name, name_label, enabled](bool on) {
        if (!service_ || !service_->set_enabled(id, on)) {
            const QSignalBlocker blocker(enabled);
            enabled->setChecked(!on);
            name_label->set_checked(!on);
            return;
        }
        name_label->set_checked(on);
        emit status_message((on ? QStringLiteral("Enabled %1")
                                : QStringLiteral("Disabled %1"))
                                   .arg(name) +
                           QStringLiteral(" — takes effect after a restart"));
    });
    connect(remove, &QPushButton::clicked, this, [this, id, name, remove]() {
        if (!service_) return;
        // A second click while the first uninstall is still tearing the card
        // down would run against a row that no longer exists and report a
        // failure the user did not cause.
        remove->setEnabled(false);
        const auto result = service_->uninstall(id);
        if (!result.ok) {
            remove->setEnabled(true);
            emit status_message(
                QStringLiteral("Could not remove %1 — the extension registry "
                               "could not be updated")
                    .arg(name));
            return;
        }
        // Say what actually happened. A packaged extension Aurora unpacked
        // into the profile is gone from disk as well, so nothing can load it
        // again; a folder the user loaded themselves is still theirs, and the
        // honest statement is that Aurora simply stopped loading it.
        emit status_message(
            result.files_deleted
                ? QStringLiteral("Removed %1 — its files were deleted; restart "
                                 "Aurora to unload it from the engine")
                      .arg(name)
                : QStringLiteral("Removed %1 — Aurora no longer loads it; your "
                                 "folder was left in place. Restart Aurora to "
                                 "unload it from the engine")
                      .arg(name));
        // status_message is what tells the window the registry changed (it
        // closes any open extension popup), so the rebuild is queued behind
        // it and behind this button's own click handler.
        QTimer::singleShot(0, this, [this] { refresh(); });
    });

    cards_->addWidget(card);
}

void ExtensionsPage::install_crx() {
    if (!service_) return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select a packaged extension"), QString(),
        QStringLiteral("Chrome extensions (*.crx *.crx3)"));
    if (path.isEmpty()) return;  // Cancelled.

    emit status_message(QStringLiteral("Installing extension…"));

    // install_from_file() is synchronous: it extracts the ZIP, validates the
    // manifest, and registers the extension in the database before returning.
    // The refresh below therefore runs after the install is fully complete,
    // not before — this avoids showing a stale list that doesn't include the
    // extension the user just installed.
    const auto result = service_->install_from_file(path.toStdString());

    switch (result.result) {
        case extensions::CrxInstallResult::Installed:
            // Tell the user explicitly that a restart is required: Chromium
            // loads extensions at engine startup, so the newly installed
            // extension will not appear in the toolbar until Aurora restarts.
            emit status_message(
                QString::fromStdString(result.detail) +
                QStringLiteral(" Restart Aurora to activate it."));
            refresh();
            break;
        case extensions::CrxInstallResult::Unsupported:
            // The package was recognized as a .crx but could not be unpacked
            // safely. Give the user a clear, actionable message.
            emit status_message(
                QStringLiteral("Could not install this extension: ") +
                QString::fromStdString(result.detail));
            break;
        case extensions::CrxInstallResult::NotACrx:
            // The file is not a valid .crx package at all.
            emit status_message(
                QStringLiteral("This file is not a valid .crx extension: ") +
                QString::fromStdString(result.detail));
            break;
    }
}

void ExtensionsPage::open_web_store() {
    // Always an in-app tab. Do not use QDesktopServices::openUrl here — that
    // hands the store to the OS default browser (Chromium) instead of Aurora.
    emit open_url_requested(
        QStringLiteral("https://chromewebstore.google.com/"));
}

void ExtensionsPage::load_unpacked() {
    if (!service_) return;
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select an unpacked extension folder"));
    if (dir.isEmpty()) return;  // Cancelled.

    if (service_->add_unpacked(dir.toStdString()) == 0) {
        emit status_message(
            QStringLiteral("That folder has no readable manifest.json, so it "
                           "is not a Chrome extension"));
        return;
    }
    emit status_message(
        QStringLiteral("Extension added — restart Aurora to load it"));
    refresh();
}

} // namespace aurora::ui
