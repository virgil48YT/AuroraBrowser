#include "ui/ExtensionsButton.hpp"

#include <algorithm>

#include <QByteArray>
#include <QColor>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSizePolicy>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include "extensions/ExtensionService.hpp"
#include "ui/Icons.hpp"
#include "ui/PopupPlacement.hpp"
#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"

namespace aurora::ui {

namespace {

constexpr int kContentWidth = 420;
// Extension rows are TWO-line rows (name + "Version x • Enabled"). Their
// height must fit both text line boxes plus the vertical padding, or the
// row layout compresses the two labels into each other and the name/version
// text renders on top of itself.
constexpr int kRowVPad = Spacing::sm;        // 8px above/below the text block
constexpr int kTextLineGap = Spacing::xxs;   // 2px between name and subtitle
constexpr int kRowMinHeight = 60;
// The footer is also two lines ("Manage extensions" + "Changes apply after
// restart"), so it needs at least as much room as a row plus its own padding;
// with too little the second line was clipped by the panel's bottom edge.
constexpr int kFooterVPad = Spacing::sm + Spacing::xxs;  // 10px
constexpr int kFooterMinHeight = 64;
constexpr int kNameWidth = 300;
constexpr QMargins kShadowMargins(9, 7, 13, 15);

QString extension_id_from_bytes(const QByteArray& bytes) {
    if (bytes.isEmpty()) return {};
    const QByteArray digest =
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).left(16);
    QString id;
    id.reserve(digest.size() * 2);
    for (const unsigned char byte : digest) {
        id.append(QChar('a' + ((byte >> 4) & 0x0f)));
        id.append(QChar('a' + (byte & 0x0f)));
    }
    return id;
}

QString extension_id(const QString& path, const QJsonObject& manifest) {
    // A manifest key is the stable public key Chromium uses for packed
    // extensions. Chrome accepts base64 DER (and tolerates PEM wrappers).
    QByteArray encoded = manifest.value(QStringLiteral("key"))
                             .toString()
                             .toLatin1();
    if (!encoded.isEmpty()) {
        encoded.replace("-----BEGIN PUBLIC KEY-----", "");
        encoded.replace("-----END PUBLIC KEY-----", "");
        encoded.replace("\r", "");
        encoded.replace("\n", "");
        encoded.replace(" ", "");
        const QByteArray key = QByteArray::fromBase64(encoded);
        if (!key.isEmpty()) return extension_id_from_bytes(key);
    }

    // Development/unpacked extensions without a key are identified by their
    // normalized absolute path. Chromium hashes FilePath::value() bytes: UTF-16
    // on Windows, UTF-8 on the other supported desktop platforms.
    QString normalized = QDir::toNativeSeparators(path);
#if defined(Q_OS_WIN)
    if (normalized.size() >= 2 && normalized.at(1) == QLatin1Char(':')) {
        normalized[0] = normalized.at(0).toUpper();
    }
    const QByteArray path_bytes(
        reinterpret_cast<const char*>(normalized.utf16()),
        normalized.size() * static_cast<qsizetype>(sizeof(char16_t)));
#else
    const QByteArray path_bytes = normalized.toUtf8();
#endif
    return extension_id_from_bytes(path_bytes);
}

// Distinguishes an extension's UI surface: a small popup (action/
// browser_action/page_action default_popup) from a full options page
// (options_ui/options_page). The rebuild() loop uses this to decide
// whether to emit popup_requested (popup) or open_extension_requested
// (options page).
struct ManifestPage {
    QString path;       // cleaned relative path within the extension dir
    bool is_popup = false;  // true = default_popup, false = options page
    // Some manifests spell their surface as a full chrome-extension:// URL
    // instead of a relative path. Then `path` IS the final URL and must not be
    // resolved against the computed extension ID.
    bool is_absolute = false;
};

ManifestPage manifest_page(const QJsonObject& manifest) {
    ManifestPage result;
    // Chrome's toolbar icon opens the default_popup when one exists, even if
    // the extension also declares an options page. Check popups FIRST so an
    // extension with both surfaces behaves like Chrome (popup on click,
    // options page accessible via the extensions management page).
    //
    // MV3 puts it under "action", MV2 under "browser_action" (and, for
    // tab-scoped extensions, "page_action"). Assign into `page` only on a hit
    // so a later empty section can never clobber an earlier match.
    QString page;
    for (const char* key : {"action", "browser_action", "page_action"}) {
        const QString popup = manifest.value(QLatin1String(key))
                                  .toObject()
                                  .value(QStringLiteral("default_popup"))
                                  .toString()
                                  .trimmed();
        if (!popup.isEmpty()) {
            page = popup;
            result.is_popup = true;
            break;
        }
    }
    // Only fall back to the options page when there is no popup.
    if (page.isEmpty()) {
        page = manifest.value(QStringLiteral("options_ui"))
                  .toObject()
                  .value(QStringLiteral("page"))
                  .toString()
                  .trimmed();
    }
    if (page.isEmpty()) {
        page = manifest.value(QStringLiteral("options_page"))
                  .toString()
                  .trimmed();
    }
    if (page.isEmpty()) return result;
    page.replace(QLatin1Char('\\'), QLatin1Char('/'));
    // A manifest may point at its own packaged page absolutely. Accept that
    // (it is the same origin) instead of dropping the surface entirely, which
    // downgraded such extensions to the aurora://extensions fallback.
    if (page.startsWith(QLatin1String("chrome-extension://"),
                        Qt::CaseInsensitive)) {
        result.path = page;
        result.is_absolute = true;
        return result;
    }
    if (page.contains(QLatin1String("://"))) {
        return result;  // some other scheme: not an in-extension surface
    }
    while (page.startsWith(QLatin1Char('/'))) page.remove(0, 1);
    // Split any query/fragment off before the traversal check: "popup.html
    // ?ctx=toolbar" and "popup.html#/dashboard" are both valid default_popup
    // values, and QDir::cleanPath() would mangle them.
    QString suffix;
    for (qsizetype i = 0; i < page.size(); ++i) {
        const QChar ch = page.at(i);
        if (ch == QLatin1Char('?') || ch == QLatin1Char('#')) {
            suffix = page.mid(i);
            page.truncate(i);
            break;
        }
    }
    const QString clean = QDir::cleanPath(page);
    if (clean.isEmpty() || clean == QLatin1String(".") ||
        clean == QLatin1String("..") ||
        clean.startsWith(QLatin1String("../"))) {
        return result;  // path stays empty → caller falls back to details
    }
    result.path = clean + suffix;
    return result;
}

QByteArray without_json_comments(const QByteArray& input) {
    QByteArray out;
    out.reserve(input.size());
    bool in_string = false;
    bool escaped = false;
    bool line_comment = false;
    bool block_comment = false;
    for (qsizetype i = 0; i < input.size(); ++i) {
        const char ch = input.at(i);
        const char next = i + 1 < input.size() ? input.at(i + 1) : '\0';
        if (line_comment) {
            if (ch == '\n' || ch == '\r') {
                line_comment = false;
                out.append(ch);
            }
            continue;
        }
        if (block_comment) {
            if (ch == '*' && next == '/') {
                block_comment = false;
                ++i;
            } else if (ch == '\n' || ch == '\r') {
                out.append(ch);
            }
            continue;
        }
        if (in_string) {
            out.append(ch);
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
            out.append(ch);
        } else if (ch == '/' && next == '/') {
            line_comment = true;
            ++i;
        } else if (ch == '/' && next == '*') {
            block_comment = true;
            ++i;
        } else {
            out.append(ch);
        }
    }
    return out;
}

// Resolves the extension's UI surface URL and whether it is a popup.
// Returns an empty path when the extension has neither an options page nor a
// default_popup — the caller falls back to aurora://extensions details.
struct ExtensionTarget {
    QString url;           // fully-resolved chrome-extension:// URL
    QString extension_id;  // 32-char lower-case hex ID
    bool is_popup = false; // true = default_popup, false = options page
};

ExtensionTarget extension_target_uncached(const extensions::Extension& ext) {
    ExtensionTarget result;
    QFile file(QString::fromStdString(ext.path) +
               QStringLiteral("/manifest.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return result;
    }
    QJsonParseError error;
    const QJsonDocument doc =
        QJsonDocument::fromJson(without_json_comments(file.readAll()), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return result;
    }
    const QJsonObject manifest = doc.object();
    const ManifestPage page = manifest_page(manifest);
    if (page.path.isEmpty()) return result;
    if (page.is_absolute) {
        const QUrl absolute(page.path);
        if (!absolute.isValid() || absolute.host().isEmpty()) return result;
        result.url = absolute.toString(QUrl::FullyEncoded);
        result.extension_id = absolute.host();
        result.is_popup = page.is_popup;
        return result;
    }
    const QString id =
        extension_id(QString::fromStdString(ext.path), manifest);
    if (id.isEmpty()) {
        return result;
    }
    const QUrl base(QStringLiteral("chrome-extension://%1/").arg(id));
    result.url = base.resolved(QUrl(page.path)).toString(QUrl::FullyEncoded);
    result.extension_id = id;
    result.is_popup = page.is_popup;
    return result;
}

// Resolving a target opens, de-comments and JSON-parses a manifest.json — per
// extension, on the click that opens the dropdown, before a single pixel can
// be shown. With a cold file cache that is most of the "the extensions popup
// is slow" complaint, and it is pure repeated work: the answer only changes
// when the manifest itself changes.
//
// Keyed on the manifest path and invalidated by its last-modified time and
// size, so editing an unpacked extension in place is still picked up on the
// next open.
struct TargetCacheEntry {
    QDateTime mtime;
    qint64 size = -1;
    ExtensionTarget target;
};

ExtensionTarget extension_target(const extensions::Extension& ext) {
    static QHash<QString, TargetCacheEntry> cache;
    const QString path = QString::fromStdString(ext.path);
    const QFileInfo manifest(path + QStringLiteral("/manifest.json"));
    if (!manifest.isFile()) {
        cache.remove(path);
        return {};
    }
    const QDateTime mtime = manifest.lastModified();
    const qint64 size = manifest.size();
    const auto it = cache.constFind(path);
    if (it != cache.constEnd() && it->mtime == mtime && it->size == size) {
        return it->target;
    }
    TargetCacheEntry entry;
    entry.mtime = mtime;
    entry.size = size;
    entry.target = extension_target_uncached(ext);
    cache.insert(path, entry);
    return entry.target;
}

// Decoding an extension's PNG per row on every open is the other half of the
// same problem, and the artwork of an installed extension does not change
// while the window is up. Keyed on the file plus the exact size and device
// pixel ratio it is rendered at, so moving the window to another monitor still
// re-renders at the right density.
QPixmap row_artwork(const QString& icon_file, const QColor& fallback_tint,
                    int px, qreal dpr) {
    static QHash<QString, QPixmap> cache;
    const QString key = icon_file + QLatin1Char('|') + QString::number(px) +
                        QLatin1Char('|') + QString::number(dpr) +
                        QLatin1Char('|') + fallback_tint.name(QColor::HexArgb);
    const auto it = cache.constFind(key);
    if (it != cache.constEnd()) return *it;

    QIcon artwork;
    if (!icon_file.isEmpty()) artwork = QIcon(icon_file);
    if (artwork.isNull()) {
        artwork = icons::icon(icons::Name::Puzzle, fallback_tint, 22, dpr);
    }
    const QPixmap pixmap = artwork.pixmap(QSize(px, px), dpr);
    // Bounded: a profile with a pathological number of extensions must not
    // turn a speed fix into an unbounded pixmap cache.
    if (cache.size() > 128) cache.clear();
    cache.insert(key, pixmap);
    return pixmap;
}

} // namespace

ExtensionsButton::ExtensionsButton(extensions::ExtensionService* service,
                                   QWidget* parent)
    : IconButton(icons::Name::Puzzle, parent), service_(service) {
    setObjectName(QStringLiteral("ExtensionsButton"));
    setToolTip(QStringLiteral("Extensions"));
    setPopupMode(QToolButton::DelayedPopup);
    setFixedSize(30, 30);
    setIconPx(17);
    build_popup();

    // Warm the manifest/artwork caches off the critical path. Doing it from a
    // zero-timer means the first click on the puzzle piece finds the same warm
    // caches every later click does, instead of paying for every extension's
    // manifest parse and icon decode while the user waits for the dropdown.
    QTimer::singleShot(0, this, [this]() { prewarm_caches(); });
}

void ExtensionsButton::prewarm_caches() {
    if (!service_) return;
    const qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    const QColor tint = current_theme().colors.text_muted;
    for (const auto& ext : service_->all_detailed()) {
        extension_target(ext);
        row_artwork(QString::fromStdString(ext.icon_file), tint, 24, dpr);
    }
}

void ExtensionsButton::build_popup() {
    // Mirror DownloadsPopover exactly where it matters: a Qt::Popup owned by
    // the browser window. A button-owned Qt::Tool was visible above the native
    // CEF child but did not capture its mouse input, so the page showed an
    // I-beam through the panel and row clicks never arrived here.
    popup_ = new QWidget(window());
    popup_->setObjectName(QStringLiteral("ExtensionsPopup"));
    popup_->setAttribute(Qt::WA_TranslucentBackground);
    popup_->setAttribute(Qt::WA_NoSystemBackground);
    popup_->setAttribute(Qt::WA_StyledBackground, true);
    popup_->setAutoFillBackground(false);
    popup_->setFocusPolicy(Qt::StrongFocus);
    popup_->setFixedWidth(kContentWidth + kShadowMargins.left() +
                          kShadowMargins.right());

    PopupPlacement placement;
    placement.mode = Qt::Popup;
    placement.clickOutsideToClose = true;
    placement.anchor = this;
    placement.apply(popup_);

    auto* root = new QVBoxLayout(popup_);
    root->setContentsMargins(kShadowMargins);
    root->setSpacing(0);

    panel_ = new QWidget(popup_);
    panel_->setObjectName(QStringLiteral("ExtensionsPopupPanel"));
    panel_->setAttribute(Qt::WA_StyledBackground, true);
    root->addWidget(panel_);
    installShadowBorder(panel_, current_theme().colors.shadow_color);

    auto* panel_layout = new QVBoxLayout(panel_);
    panel_layout->setContentsMargins(0, 0, 0, 0);
    panel_layout->setSpacing(0);

    header_ = new QWidget(panel_);
    header_->setObjectName(QStringLiteral("ExtensionsPopupHeader"));
    header_->setAttribute(Qt::WA_StyledBackground, true);
    auto* header_layout = new QHBoxLayout(header_);
    header_layout->setContentsMargins(Spacing::md, Spacing::sm,
                                      Spacing::sm, Spacing::sm);
    header_layout->setSpacing(Spacing::sm);
    auto* title = new QLabel(QStringLiteral("Extensions"), header_);
    title->setObjectName(QStringLiteral("ExtensionsPopupTitle"));
    header_layout->addWidget(title, 1);
    auto* close_button = new IconButton(icons::Name::Close, header_);
    close_button->setObjectName(QStringLiteral("PopoverCloseButton"));
    close_button->setToolTip(QStringLiteral("Close"));
    close_button->setFixedSize(22, 22);
    close_button->setIconPx(12);
    connect(close_button, &IconButton::clicked, popup_, &QWidget::close);
    header_layout->addWidget(close_button);
    panel_layout->addWidget(header_);

    // Rows live in a scroll area so clamping the popup to the screen never
    // clips the two-line footer. When every row fits, the scroll bar stays
    // hidden and the panel sizes to its content.
    list_scroll_ = new QScrollArea(panel_);
    list_scroll_->setObjectName(QStringLiteral("ExtensionsPopupScroll"));
    list_scroll_->setWidgetResizable(true);
    list_scroll_->setFrameShape(QFrame::NoFrame);
    list_scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    list_scroll_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    if (list_scroll_->viewport()) {
        list_scroll_->viewport()->setAutoFillBackground(false);
        list_scroll_->viewport()->setAttribute(Qt::WA_StyledBackground, false);
    }

    list_host_ = new QWidget(list_scroll_);
    list_host_->setObjectName(QStringLiteral("ExtensionsPopupList"));
    list_layout_ = new QVBoxLayout(list_host_);
    list_layout_->setContentsMargins(Spacing::xs, Spacing::xs,
                                     Spacing::xs, Spacing::xs);
    // A small gap keeps two-line rows from butting into each other so each
    // name/subtitle pair reads as one block.
    list_layout_->setSpacing(Spacing::xxs);
    list_scroll_->setWidget(list_host_);
    panel_layout->addWidget(list_scroll_, 1);

    auto* footer = new QPushButton(panel_);
    footer_ = footer;
    footer_->setObjectName(QStringLiteral("ExtensionsPopupFooter"));
    footer_->setCursor(Qt::PointingHandCursor);
    footer_->setAccessibleName(QStringLiteral("Manage extensions"));
    footer_->setMinimumHeight(kFooterMinHeight);
    // Minimum (not Fixed) so the footer can never be squeezed below the height
    // its two text lines need.
    footer_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    auto* footer_layout = new QHBoxLayout(footer_);
    footer_layout->setContentsMargins(Spacing::md, kFooterVPad,
                                      Spacing::md, kFooterVPad);
    footer_layout->setSpacing(Spacing::sm);
    auto* footer_text = new QVBoxLayout();
    footer_text->setContentsMargins(0, 0, 0, 0);
    footer_text->setSpacing(kTextLineGap);
    auto* manage = new QLabel(QStringLiteral("Manage extensions"), footer);
    manage->setObjectName(QStringLiteral("ExtensionsPopupFooterText"));
    manage->setCursor(Qt::PointingHandCursor);
    manage->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    footer_text->addWidget(manage);
    auto* note = new QLabel(QStringLiteral("Changes apply after restart"), footer);
    note->setObjectName(QStringLiteral("ExtensionsPopupFooterSub"));
    note->setCursor(Qt::PointingHandCursor);
    note->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    footer_text->addWidget(note);
    footer_layout->addLayout(footer_text, 1);
    footer_layout->setAlignment(footer_text, Qt::AlignVCenter);
    auto* arrow = new QLabel(footer);
    arrow->setObjectName(QStringLiteral("ExtensionsPopupFooterArrow"));
    arrow->setCursor(Qt::PointingHandCursor);
    arrow->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    footer_layout->addWidget(arrow, 0, Qt::AlignVCenter);
    connect(footer, &QPushButton::clicked, this, [this] {
        popup_->close();
        emit manage_requested();
    });
    panel_layout->addWidget(footer_);
}

void ExtensionsButton::update_popup_style() {
    if (!popup_ || !panel_) return;

    const Theme& theme = current_theme();
    const ThemeColors& c = theme.colors;
    const QString hover = popup_detail::css_color(c.accent_soft);
    QColor pressed_color(c.accent_soft);
    pressed_color.setAlpha(qMin(255, pressed_color.alpha() + 42));
    const QString pressed = popup_detail::css_color(pressed_color);

    // min-height on the row/footer text labels reserves a full line box for
    // each one. Without it the two stacked labels collapsed into whatever
    // height was left over, so the name printed on top of the version line and
    // the footer's subtext was cut off at the panel's bottom edge.
    QString style = QStringLiteral(
        "QWidget#ExtensionsPopup { background: transparent; border: none; }"
        "QWidget#ExtensionsPopupHeader { background: transparent; border: none;"
        " border-bottom: 1px solid %1; }"
        "QLabel#ExtensionsPopupTitle { color: %2; font-size: %3px;"
        " font-weight: 600; }"
        "QToolButton#PopoverCloseButton { background: transparent; border: none;"
        " border-radius: %4px; padding: 0px; }"
        "QToolButton#PopoverCloseButton:hover { background: %5; }"
        "QToolButton#PopoverCloseButton:pressed { background: %6; }"
        "QScrollArea#ExtensionsPopupScroll { background: transparent;"
        " border: none; }"
        "QWidget#ExtensionsPopupList {"
        " background: transparent; border: none; }"
        "QPushButton#ExtensionRow { background: transparent; border: none;"
        " border-radius: %4px; padding: 0px; min-height: %16px;"
        " text-align: left; }"
        "QPushButton#ExtensionRow:hover { background: %5; }"
        "QPushButton#ExtensionRow:pressed { background: %6; }"
        "QLabel#ExtensionRowName, QLabel#ExtensionsPopupFooterText {"
        " color: %2; font-size: %7px; min-height: %13px; padding: 0px;"
        " margin: 0px; }"
        "QLabel#ExtensionRowSub, QLabel#ExtensionsPopupEmpty,"
        " QLabel#ExtensionsPopupFooterSub { color: %8; font-size: %9px; }"
        "QLabel#ExtensionRowSub, QLabel#ExtensionsPopupFooterSub {"
        " min-height: %14px; padding: 0px; margin: 0px; }"
        "QLabel#ExtensionsPopupEmpty { padding: %10px %11px; }"
        "QPushButton#ExtensionsPopupFooter { background: transparent;"
        " border: none; border-top: 1px solid %1; border-radius: 0px;"
        " padding: 0px; min-height: %15px; text-align: left; }"
        "QPushButton#ExtensionsPopupFooter:hover { background: %5;"
        " border-bottom-left-radius: %12px; border-bottom-right-radius: %12px; }"
        "QPushButton#ExtensionsPopupFooter:pressed { background: %6; }")
        .arg(popup_detail::css_color(c.divider),
             popup_detail::css_color(c.text))
        .arg(theme.title_font_px)
        .arg(Radius::sm)
        .arg(hover, pressed)
        .arg(theme.body_font_px)
        .arg(popup_detail::css_color(c.text_muted))
        .arg(theme.small_font_px + 1)
        .arg(Spacing::xl)
        .arg(Spacing::md)
        .arg(Radius::popover - Metrics::hairline)
        // %13/%14: line boxes = font size plus leading, so descenders and the
        // gap between the two lines are always drawn.
        .arg(theme.body_font_px + 7)
        .arg(theme.small_font_px + 6)
        .arg(kFooterMinHeight)
        .arg(kRowMinHeight);
    style += roundedPanelStyle(QStringLiteral("ExtensionsPopupPanel"), c);
    popup_->setStyleSheet(style);

    if (auto* shadow =
            qobject_cast<QGraphicsDropShadowEffect*>(panel_->graphicsEffect())) {
        QColor shadow_color(c.shadow_color);
        shadow_color.setAlpha(qMax(38, shadow_color.alpha()));
        shadow->setColor(shadow_color);
    } else {
        installShadowBorder(panel_, c.shadow_color);
    }

    const qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    if (auto* arrow = panel_->findChild<QLabel*>(
            QStringLiteral("ExtensionsPopupFooterArrow"))) {
        arrow->setPixmap(icons::pixmap(icons::Name::OpenExternal, 15,
                                       c.text_muted, dpr));
    }
}

void ExtensionsButton::rebuild() {
    while (auto* item = list_layout_->takeAt(0)) {
        // The popup is hidden during rebuild. Delete synchronously so stale
        // detached labels cannot survive through show() and overlap the freshly
        // built name/version rows until the next event-loop turn.
        if (auto* widget = item->widget()) delete widget;
        delete item;
    }

    const auto add_message = [this](const QString& message) {
        auto* label = new QLabel(message, list_host_);
        label->setObjectName(QStringLiteral("ExtensionsPopupEmpty"));
        label->setAlignment(Qt::AlignCenter);
        label->setWordWrap(true);
        list_layout_->addWidget(label);
    };

    if (!service_) {
        add_message(QStringLiteral("Extensions are unavailable for this window."));
        return;
    }

    // Preserve the registry's name order inside each group while putting the
    // extensions that are actually loaded first, like Chrome's list.
    auto list = service_->all_detailed();
    std::stable_sort(list.begin(), list.end(),
                     [](const auto& left, const auto& right) {
                         return left.enabled && !right.enabled;
                     });
    if (list.empty()) {
        add_message(QStringLiteral("No extensions installed"));
        return;
    }

    const ThemeColors& colors = current_theme().colors;
    const qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    for (const auto& ext : list) {
        const ExtensionTarget target = extension_target(ext);
        const bool has_surface = !target.url.isEmpty();

        auto* row = new QPushButton(list_host_);
        row->setObjectName(QStringLiteral("ExtensionRow"));
        row->setCursor(Qt::PointingHandCursor);
        // Vertically Minimum (not Fixed): the row must be free to grow to the
        // height its two stacked labels need instead of being clamped.
        row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        row->setMinimumHeight(kRowMinHeight);
        const QString name = QString::fromStdString(ext.name);
        row->setAccessibleName(name);

        QString tooltip = !ext.enabled
                              ? QStringLiteral("Open extension details")
                              : !has_surface
                                    ? QStringLiteral("Open extension details")
                                    : target.is_popup
                                          ? QStringLiteral("Open extension popup")
                                          : QStringLiteral("Open extension options");
        if (!ext.description.empty()) {
            tooltip += QStringLiteral("\n") +
                       QString::fromStdString(ext.description);
        }
        if (!ext.enabled) {
            tooltip += QStringLiteral("\nDisabled (changes apply after restart)");
        }
        row->setToolTip(tooltip);

        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(Spacing::sm, kRowVPad,
                                       Spacing::md, kRowVPad);
        row_layout->setSpacing(Spacing::sm);
        auto* icon = new QLabel(row);
        icon->setFixedSize(28, 28);
        icon->setAlignment(Qt::AlignCenter);
        icon->setCursor(Qt::PointingHandCursor);
        icon->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        icon->setPixmap(row_artwork(QString::fromStdString(ext.icon_file),
                                    colors.text_muted, 24, dpr));
        row_layout->addWidget(icon, 0, Qt::AlignVCenter);

        auto* text_layout = new QVBoxLayout();
        text_layout->setContentsMargins(0, 0, 0, 0);
        text_layout->setSpacing(kTextLineGap);
        auto* name_label = new QLabel(name, row);
        name_label->setObjectName(QStringLiteral("ExtensionRowName"));
        name_label->setCursor(Qt::PointingHandCursor);
        name_label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        name_label->setWordWrap(false);
        name_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        // Keep the un-elided name so elide_row_names() can measure against the
        // stylesheet font after ensurePolished() — measuring here used the
        // wrong metrics and either overflowed or collapsed the row.
        name_label->setProperty("fullName", name);
        name_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        text_layout->addWidget(name_label);

        // Spec format: "Version x • Enabled/Disabled/No popup".
        const QString status = !ext.enabled
                                   ? QStringLiteral("Disabled")
                                   : !has_surface
                                         ? QStringLiteral("No popup")
                                         : QStringLiteral("Enabled");
        const QString subtitle =
            ext.version.empty()
                ? status
                : QStringLiteral("Version %1 • %2")
                      .arg(QString::fromStdString(ext.version), status);
        auto* sub_label = new QLabel(subtitle, row);
        sub_label->setObjectName(QStringLiteral("ExtensionRowSub"));
        sub_label->setCursor(Qt::PointingHandCursor);
        sub_label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        sub_label->setWordWrap(false);
        sub_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        text_layout->addWidget(sub_label);
        row_layout->addLayout(text_layout, 1);
        row_layout->setAlignment(text_layout, Qt::AlignVCenter);

        auto* arrow = new QLabel(row);
        arrow->setCursor(Qt::PointingHandCursor);
        arrow->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        arrow->setPixmap(icons::pixmap(icons::Name::ChevronRight, 14,
                                       colors.text_muted, dpr));
        row_layout->addWidget(arrow, 0, Qt::AlignVCenter);

        const bool usable = ext.enabled && has_surface;
        const bool wants_popup = usable && target.is_popup;
        const QString row_url =
            usable ? target.url : QStringLiteral("aurora://extensions");
        const QString row_id = target.extension_id;
        connect(row, &QPushButton::clicked, this,
                [this, wants_popup, row_url, row_id] {
            popup_->close();
            // Emit on the NEXT event-loop turn. This dropdown is a Qt::Popup:
            // closing it releases the native mouse grab and hands window
            // activation back to the browser window. Creating the extension
            // popup window inside the same turn raced that transition — the
            // activation change that followed reached the host's event filter
            // after the new popup had been shown and closed it again, so the
            // click appeared to do nothing.
            QTimer::singleShot(0, this, [this, wants_popup, row_url, row_id] {
                if (wants_popup) {
                    emit popup_requested(row_id, row_url);
                } else {
                    emit open_extension_requested(row_url);
                }
            });
        });
        list_layout_->addWidget(row);
    }
}

void ExtensionsButton::elide_row_names() {
    if (!list_host_) return;
    const auto names =
        list_host_->findChildren<QLabel*>(QStringLiteral("ExtensionRowName"));
    for (auto* name_label : names) {
        const QString full = name_label->property("fullName").toString();
        if (full.isEmpty()) continue;
        name_label->ensurePolished();
        int width = name_label->width();
        if (width < 8) width = kNameWidth;
        name_label->setText(
            QFontMetrics(name_label->font()).elidedText(full, Qt::ElideRight, width));
    }
}

void ExtensionsButton::fit_and_place_popup() {
    if (!popup_ || !panel_ || !list_scroll_ || !list_host_ || !footer_ ||
        !header_) {
        return;
    }

    // Drop the previous open's floors so a shorter list can shrink again.
    list_scroll_->setMinimumHeight(0);
    list_scroll_->setMaximumHeight(QWIDGETSIZE_MAX);
    popup_->setMinimumHeight(0);
    popup_->setMaximumHeight(QWIDGETSIZE_MAX);

    popup_->ensurePolished();
    panel_->ensurePolished();
    header_->ensurePolished();
    footer_->ensurePolished();
    list_host_->ensurePolished();

    // The footer's second line ("Changes apply after restart") must never be
    // clipped: floor at whichever is larger, the constant or the hint after
    // the live stylesheet has been applied.
    footer_->setMinimumHeight(
        qMax(kFooterMinHeight, footer_->minimumSizeHint().height()));

    if (list_layout_) list_layout_->activate();
    list_host_->adjustSize();
    const int list_h = qMax(list_host_->height(),
                            qMax(list_host_->sizeHint().height(),
                                 list_host_->minimumSizeHint().height()));
    // Temporarily pin the scroll area to the full list so the popup's
    // sizeHint includes every row (a QScrollArea otherwise reports a small
    // default viewport and the panel looks truncated).
    list_scroll_->setMinimumHeight(list_h);

    if (auto* panel_layout = panel_->layout()) panel_layout->activate();
    panel_->adjustSize();
    if (auto* root_layout = popup_->layout()) root_layout->activate();
    popup_->adjustSize();

    int popup_h = qMax(popup_->sizeHint().height(),
                       popup_->minimumSizeHint().height());

    const int available_panel =
        PopupPlacement::availablePanelHeightFromAnchor(this, Spacing::xs);
    const int max_popup =
        available_panel + kShadowMargins.top() + kShadowMargins.bottom();

    if (popup_h > max_popup) {
        const int overflow = popup_h - max_popup;
        const int scroll_h = qMax(kRowMinHeight, list_h - overflow);
        list_scroll_->setMinimumHeight(0);
        list_scroll_->setMaximumHeight(scroll_h);
        popup_h = max_popup;
        if (auto* panel_layout = panel_->layout()) panel_layout->activate();
        if (auto* root_layout = popup_->layout()) root_layout->activate();
    }

    popup_->setMinimumHeight(popup_h);
    popup_->resize(popup_->width(), popup_h);
    popup_->setGeometry(PopupPlacement::popupGeometryFromAnchor(
        this, QSize(popup_->width(), popup_h), /*alignRight=*/true, Spacing::xs,
        kShadowMargins));
}

void ExtensionsButton::close_popup() {
    if (popup_ && popup_->isVisible()) popup_->close();
}

void ExtensionsButton::toggle_popup() {
    if (!popup_) return;
    if (popup_->isVisible()) {
        popup_->close();
        return;
    }

    // Re-parent to the current browser window if the widget tree moved.
    // setParent() resets window flags, so restore the Qt::Popup capture that
    // keeps clicks from falling through to the CEF child. Do NOT call
    // PopupPlacement::apply() again — that would install a second dismiss
    // filter. The original filter is parented to popup_ and survives.
    QWidget* win = window();
    if (win && popup_->parentWidget() != win) {
        popup_->setParent(win);
        popup_->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint |
                               Qt::NoDropShadowWindowHint);
        popup_->setAttribute(Qt::WA_TranslucentBackground);
        popup_->setAttribute(Qt::WA_NoSystemBackground);
        popup_->setAttribute(Qt::WA_StyledBackground, true);
    }

    QScreen* target_screen = screen();
    if (target_screen) {
        popup_->setFixedWidth(qMin(
            kContentWidth + kShadowMargins.left() + kShadowMargins.right(),
            qMax(1, target_screen->availableGeometry().width() -
                        2 * Spacing::sm)));
    }

    // Always restyle from the live theme + DPR. Caching skipped the rebuild
    // after a theme switch and left the panel looking like the previous theme.
    update_popup_style();
    rebuild();
    popup_->ensurePolished();
    elide_row_names();
    fit_and_place_popup();
    // Qt::Popup supplies the native input capture that DownloadsPopover relies
    // on; show/raise is sufficient and avoids focus races with the CEF child.
    popup_->show();
    popup_->raise();
}

} // namespace aurora::ui
