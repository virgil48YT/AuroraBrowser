#include "ui/OmniboxPopup.hpp"

#include <QApplication>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QIcon>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QWindow>

#include "ui/FaviconCache.hpp"
#include "ui/Icons.hpp"
#include "ui/ThemeState.hpp"

namespace aurora::ui {

namespace {

constexpr int kRowHeight = 36;
constexpr int kPadX = 12;
constexpr int kIconSize = 16;
constexpr int kIconGap = 12;
constexpr int kVPad = 6;
constexpr int kRadius = 10;    // same corner radius as the address field
constexpr int kSideInset = 2;   // matches the field's painted inset (Omnibox::paintEvent)

using omnibox::Kind;

icons::Name icon_for(Kind k) {
    switch (k) {
        case Kind::Bookmark: return icons::Name::StarFilled;
        case Kind::History: return icons::Name::History;
        case Kind::OpenTab: return icons::Name::NewTab;
        case Kind::Navigate: return icons::Name::Globe;
        case Kind::Search:
        case Kind::RemoteSearch:
        default: return icons::Name::Search;
    }
}

} // namespace

OmniboxPopup::OmniboxPopup(QWidget* anchor)
    : QWidget(anchor, Qt::ToolTip | Qt::FramelessWindowHint |
                          Qt::NoDropShadowWindowHint | Qt::WindowDoesNotAcceptFocus),
      anchor_(anchor) {
    setObjectName(QStringLiteral("OmniboxPopup"));
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);

    // A favicon that arrives while the list is open should appear in place.
    connect(&FaviconCache::instance(), &FaviconCache::icon_ready, this,
            [this](const QString&) { if (isVisible()) update(); });
}

int OmniboxPopup::content_height() const {
    return static_cast<int>(rows_.size()) * kRowHeight + 2 * kVPad;
}

QSize OmniboxPopup::sizeHint() const {
    const int w = anchor_ ? anchor_->width() - 2 * kSideInset : 480;
    return {w, content_height()};
}

void OmniboxPopup::set_rows(std::vector<omnibox::Suggestion> rows,
                            const QString& engine_name) {
    rows_ = std::move(rows);
    engine_name_ = engine_name;
    if (selected_ >= static_cast<int>(rows_.size())) selected_ = -1;
    hovered_ = -1;
    if (rows_.empty()) {
        hide();
        return;
    }
    if (isVisible()) present();
    update();
}

void OmniboxPopup::clear_rows() {
    rows_.clear();
    selected_ = -1;
    hovered_ = -1;
    hide();
}

void OmniboxPopup::set_selected(int index) {
    if (index < -1 || index >= static_cast<int>(rows_.size())) index = -1;
    if (index == selected_) return;
    selected_ = index;
    update();
}

int OmniboxPopup::move_selection(int delta) {
    const int n = static_cast<int>(rows_.size());
    if (n == 0) return -1;
    // Positions -1 .. n-1 form a ring of n + 1 ring.
    const int ring = n + 1;
    int pos = (selected_ + 1 + delta) % ring;
    if (pos < 0) pos += ring;
    set_selected(pos - 1);
    return selected_;
}

const omnibox::Suggestion* OmniboxPopup::row(int index) const {
    if (index < 0 || index >= static_cast<int>(rows_.size())) return nullptr;
    return &rows_[static_cast<size_t>(index)];
}

void OmniboxPopup::present() {
    if (!anchor_ || rows_.empty()) {
        hide();
        return;
    }
    const QSize sz = sizeHint();
    // The field is painted 2px inside its widget; hang the list a hair
    // below its bottom edge, flush with its left and right edges.
    QPoint below = anchor_->mapToGlobal(QPoint(kSideInset, anchor_->height() + 2));
    if (const QScreen* screen = anchor_->screen()) {
        const QRect avail = screen->availableGeometry();
        if (below.x() + sz.width() > avail.right()) below.setX(avail.right() - sz.width());
        if (below.x() < avail.left()) below.setX(avail.left());
    }
    setGeometry(QRect(below, sz));
    if (!isVisible()) show();
    update();
}

int OmniboxPopup::row_at(const QPoint& pos) const {
    const int y = pos.y() - kVPad;
    if (y < 0) return -1;
    const int i = y / kRowHeight;
    return i < static_cast<int>(rows_.size()) ? i : -1;
}

void OmniboxPopup::mouseMoveEvent(QMouseEvent* e) {
    const int i = row_at(e->position().toPoint());
    if (i != hovered_) {
        hovered_ = i;
        update();
    }
}

void OmniboxPopup::leaveEvent(QEvent* e) {
    hovered_ = -1;
    update();
    QWidget::leaveEvent(e);
}

void OmniboxPopup::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) {
        e->ignore();
        return;
    }
    const int i = row_at(e->position().toPoint());
    if (i >= 0) emit row_activated(i);
}

QString OmniboxPopup::kind_hint(const omnibox::Suggestion& s) const {
    switch (s.kind) {
        case Kind::OpenTab: return QStringLiteral("Switch to tab");
        case Kind::Bookmark: return QStringLiteral("Bookmark");
        case Kind::Navigate: return QStringLiteral("Open");
        case Kind::Search:
            return engine_name_.isEmpty()
                       ? QStringLiteral("Search")
                       : QStringLiteral("Search %1").arg(engine_name_);
        default: return {};
    }
}

void OmniboxPopup::paintEvent(QPaintEvent*) {
    if (rows_.empty()) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const auto& c = current_theme().colors;

    // Plate: raised surface with a hairline, softened by a faint accent edge
    // so it reads as part of the omnibox above it.
    const QRectF plate = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(plate, kRadius, kRadius);
    QColor bg = c.surface_raised;
    bg.setAlpha(255);
    p.setPen(QPen(c.border_subtle, 1.0));
    p.setBrush(bg);
    p.drawPath(path);

    const QFont base = font();
    QFont title_font = base;
    title_font.setPixelSize(13);
    QFont url_font = base;
    url_font.setPixelSize(12);
    QFont hint_font = base;
    hint_font.setPixelSize(11);
    const QFontMetrics tfm(title_font), ufm(url_font), hfm(hint_font);

    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        const auto& r = rows_[static_cast<size_t>(i)];
        const QRect row_rect(kSideInset / 2, kVPad + i * kRowHeight,
                             width() - kSideInset, kRowHeight);

        const bool selected = (i == selected_);
        const bool hovered = (i == hovered_) && !selected;
        if (selected || hovered) {
            QPainterPath hl;
            hl.addRoundedRect(QRectF(row_rect), 8, 8);
            p.fillPath(hl, selected ? c.surface_selected : c.surface_hover);
        }

        // Leading icon: a page's favicon when we have one, else a glyph.
        const QRect icon_rect(row_rect.left() + kPadX - 2,
                              row_rect.top() + (kRowHeight - kIconSize) / 2,
                              kIconSize, kIconSize);
        const bool page_row = r.kind == Kind::History || r.kind == Kind::Bookmark ||
                              r.kind == Kind::OpenTab ||
                              (r.kind == Kind::Navigate && !r.url.empty());
        bool drew_favicon = false;
        if (page_row && !r.url.empty()) {
            const QIcon fav = FaviconCache::instance().icon_for_url(
                QString::fromStdString(r.url));
            if (!fav.isNull()) {
                fav.paint(&p, icon_rect);
                drew_favicon = true;
            }
        }
        if (!drew_favicon) {
            icons::paint(&p, icon_for(r.kind), QRectF(icon_rect),
                         selected ? c.text : c.text_muted);
        }

        // Right-aligned hint.
        const QString hint = kind_hint(r);
        int right = row_rect.right() - kPadX;
        if (!hint.isEmpty() && (selected || hovered || r.kind == Kind::OpenTab)) {
            p.setFont(hint_font);
            p.setPen(c.text_faint);
            const int hw = hfm.horizontalAdvance(hint);
            p.drawText(QRect(right - hw, row_rect.top(), hw, kRowHeight),
                       Qt::AlignRight | Qt::AlignVCenter, hint);
            right -= hw + 12;
        }

        const int text_left = icon_rect.right() + kIconGap;
        const int avail = std::max(0, right - text_left);
        const QString title = QString::fromStdString(r.title.empty() ? r.fill : r.title);

        if (r.url.empty()) {
            // Search-style row: just the terms.
            p.setFont(title_font);
            p.setPen(c.text);
            p.drawText(QRect(text_left, row_rect.top(), avail, kRowHeight),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       tfm.elidedText(title, Qt::ElideRight, avail));
        } else {
            // Page row: title, then the address dimmed after it.
            QString url = QString::fromStdString(r.url);
            if (url.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) url.remove(0, 8);
            else if (url.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)) url.remove(0, 7);
            if (url.startsWith(QStringLiteral("www."), Qt::CaseInsensitive)) url.remove(0, 4);
            if (url.endsWith(QLatin1Char('/'))) url.chop(1);

            const bool has_title = !r.title.empty() && title != url;
            const QString shown_title = has_title ? title : url;
            const int title_w = std::min(tfm.horizontalAdvance(shown_title),
                                         has_title ? static_cast<int>(avail * 0.62) : avail);
            p.setFont(title_font);
            p.setPen(c.text);
            p.drawText(QRect(text_left, row_rect.top(), title_w, kRowHeight),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       tfm.elidedText(shown_title, Qt::ElideRight, title_w));
            if (has_title) {
                const int url_left = text_left + title_w + 10;
                const int url_avail = right - url_left;
                if (url_avail > 40) {
                    p.setFont(url_font);
                    p.setPen(c.accent);
                    p.drawText(QRect(url_left, row_rect.top(), url_avail, kRowHeight),
                               Qt::AlignLeft | Qt::AlignVCenter,
                               ufm.elidedText(url, Qt::ElideMiddle, url_avail));
                }
            }
        }
    }
}

} // namespace aurora::ui
