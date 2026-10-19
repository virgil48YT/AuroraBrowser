// Aurora — Reader mode. See ReaderPage.hpp.
#include "ui/pages/ReaderPage.hpp"

#include <QHBoxLayout>
#include <QResizeEvent>

#include <algorithm>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QTextBrowser>
#include <QTextFrame>
#include <QTextFrameFormat>
#include <QUrl>

#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"

namespace aurora::ui {

namespace {

// Runs in the page's main world via Runtime.evaluate and must evaluate to a
// string. Kept dependency-free and defensive: any exception is reported as
// "no article" by the caller.
const char kExtraction[] = R"JS((function () {
  var MIN_CHARS = 400;
  var BAD = /(comment|sidebar|footer|foot|masthead|nav|menu|share|social|promo|advert|sponsor|related|recommend|cookie|consent|subscribe|newsletter|popup|modal|breadcrumb|banner|outbrain|taboola)/i;
  var SKIP = {SCRIPT:1, STYLE:1, NOSCRIPT:1, TEMPLATE:1, IFRAME:1, FORM:1, BUTTON:1,
              INPUT:1, SELECT:1, TEXTAREA:1, NAV:1, ASIDE:1, FOOTER:1, HEADER:1,
              SVG:1, CANVAS:1, VIDEO:1, AUDIO:1, OBJECT:1, EMBED:1, DIALOG:1};
  function hidden(el) {
    if (el.hidden || el.getAttribute('aria-hidden') === 'true') return true;
    var s = window.getComputedStyle(el);
    return s && (s.display === 'none' || s.visibility === 'hidden');
  }
  function boilerplate(el) {
    var tag = el.tagName;
    if (SKIP[tag]) return true;
    var role = el.getAttribute('role') || '';
    if (/^(navigation|banner|complementary|contentinfo|dialog|menu)$/.test(role)) return true;
    var sig = (el.id || '') + ' ' + (typeof el.className === 'string' ? el.className : '');
    return sig.length > 1 && BAD.test(sig) && !/(article|content|body|post|entry|story)/i.test(sig);
  }
  function textLen(el) { return (el.textContent || '').replace(/\s+/g, ' ').trim().length; }
  function linkDensity(el) {
    var t = textLen(el) || 1, l = 0;
    el.querySelectorAll('a').forEach(function (a) { l += textLen(a); });
    return l / t;
  }
  function score() {
    var scores = new Map();
    document.querySelectorAll('p, pre, blockquote, li').forEach(function (p) {
      var len = textLen(p);
      if (len < 25) return;
      var s = 1 + (p.textContent.split(',').length - 1) + Math.min(Math.floor(len / 100), 3);
      var parent = p.parentElement, gp = parent && parent.parentElement;
      if (parent) scores.set(parent, (scores.get(parent) || 0) + s);
      if (gp) scores.set(gp, (scores.get(gp) || 0) + s / 2);
    });
    var best = null, bestScore = 0;
    scores.forEach(function (s, el) {
      if (boilerplate(el)) return;
      var adjusted = s * (1 - linkDensity(el));
      if (adjusted > bestScore) { bestScore = adjusted; best = el; }
    });
    return best;
  }
  function pick() {
    var articles = Array.prototype.filter.call(document.querySelectorAll('article'),
        function (a) { return textLen(a) >= MIN_CHARS; });
    if (articles.length === 1) return articles[0];
    var main = document.querySelector('main, [role=main]');
    var scored = score();
    if (scored && (!main || main.contains(scored))) return scored;
    return main && textLen(main) >= MIN_CHARS ? main : scored;
  }
  function esc(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  }
  var BLOCK = {P:'p', H1:'h2', H2:'h2', H3:'h3', H4:'h4', H5:'h5', H6:'h6', UL:'ul', OL:'ol',
               LI:'li', BLOCKQUOTE:'blockquote', PRE:'pre', FIGCAPTION:'p', DL:'dl', DT:'dt', DD:'dd',
               TABLE:'table', TR:'tr', TD:'td', TH:'th', THEAD:'thead', TBODY:'tbody'};
  var INLINE = {EM:'em', I:'em', STRONG:'strong', B:'strong', CODE:'code', SUB:'sub', SUP:'sup',
                MARK:'strong', Q:'em', CITE:'em', SMALL:'small'};
  function out(node, depth) {
    if (depth > 40) return '';
    if (node.nodeType === 3) return esc(node.nodeValue.replace(/\s+/g, ' '));
    if (node.nodeType !== 1) return '';
    var el = node, tag = el.tagName;
    if (boilerplate(el) || hidden(el)) return '';
    if (tag === 'BR') return '<br>';
    if (tag === 'HR') return '<hr>';
    if (tag === 'IMG') {
      var alt = (el.getAttribute('alt') || '').trim();
      return alt ? '<p class="img">[' + esc(alt) + ']</p>' : '';
    }
    var inner = '';
    for (var c = el.firstChild; c; c = c.nextSibling) inner += out(c, depth + 1);
    if (tag === 'A') {
      var href = el.href || '';
      return /^https?:/i.test(href) ? '<a href="' + esc(href) + '">' + inner + '</a>' : inner;
    }
    if (tag === 'PRE') return '<pre>' + esc(el.textContent || '') + '</pre>';
    if (BLOCK[tag]) return inner.trim() ? '<' + BLOCK[tag] + '>' + inner + '</' + BLOCK[tag] + '>' : '';
    if (INLINE[tag]) return '<' + INLINE[tag] + '>' + inner + '</' + INLINE[tag] + '>';
    return inner;
  }
  function meta(names) {
    for (var i = 0; i < names.length; i++) {
      var m = document.querySelector('meta[name="' + names[i] + '"], meta[property="' + names[i] + '"]');
      if (m && m.content) return m.content.trim();
    }
    return '';
  }
  try {
    var root = pick();
    if (!root || textLen(root) < MIN_CHARS) return JSON.stringify({ok: false});
    var html = out(root, 0);
    var plain = (root.textContent || '').replace(/\s+/g, ' ').trim();
    var h1 = document.querySelector('h1');
    return JSON.stringify({
      ok: true,
      title: meta(['og:title', 'twitter:title']) || (h1 && h1.textContent.trim()) || document.title,
      byline: meta(['author', 'article:author', 'byl', 'parsely-author']),
      site: meta(['og:site_name', 'application-name']) || location.hostname,
      html: html,
      words: plain.split(' ').length
    });
  } catch (e) {
    return JSON.stringify({ok: false, error: String(e)});
  }
})())JS";

QString esc(const QString& s) { return s.toHtmlEscaped(); }

} // namespace

const char* ReaderPage::extraction_script() { return kExtraction; }

ReaderArticle ReaderPage::parse_result(bool eval_ok, const QString& json,
                                       const QString& source_url) {
    ReaderArticle a;
    a.source_url = source_url;
    a.message = QStringLiteral(
        "Reader mode could not find an article on this page. It works best on "
        "news stories, blog posts and documentation with a main block of text.");
    if (!eval_ok) return a;
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    if (!o.value(QStringLiteral("ok")).toBool()) return a;
    a.html = o.value(QStringLiteral("html")).toString();
    if (a.html.trimmed().isEmpty()) return a;
    a.ok = true;
    a.title = o.value(QStringLiteral("title")).toString().trimmed();
    a.byline = o.value(QStringLiteral("byline")).toString().trimmed();
    a.site = o.value(QStringLiteral("site")).toString().trimmed();
    a.words = o.value(QStringLiteral("words")).toInt();
    return a;
}

ReaderPage::ReaderPage(QWidget* parent)
    : InternalPageWidget(QStringLiteral("Reader"), parent) {
    original_ = new QPushButton(QStringLiteral("Open original"), this);
    smaller_ = new QPushButton(QStringLiteral("A−"), this);
    larger_ = new QPushButton(QStringLiteral("A+"), this);
    smaller_->setToolTip(QStringLiteral("Smaller text"));
    larger_->setToolTip(QStringLiteral("Larger text"));
    toolbar_row()->addWidget(original_);
    toolbar_row()->addStretch(1);
    toolbar_row()->addWidget(smaller_);
    toolbar_row()->addWidget(larger_);

    view_ = new QTextBrowser(this);
    view_->setObjectName(QStringLiteral("ReaderView"));
    view_->setOpenLinks(false);  // links open as tabs through the window
    view_->setFrameShape(QFrame::NoFrame);
    // QTextBrowser would try to resolve <img>/relative resources against the
    // local filesystem; the sanitized article has none, and this keeps it so.
    view_->setSearchPaths({});
    set_body(view_);

    connect(view_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
            emit open_url_requested(url.toString());
    });
    connect(original_, &QPushButton::clicked, this, [this] {
        if (!article_.source_url.isEmpty()) emit open_original_requested(article_.source_url);
    });
    connect(smaller_, &QPushButton::clicked, this, [this] {
        font_px_ = std::max(14, font_px_ - 2);
        render();
    });
    connect(larger_, &QPushButton::clicked, this, [this] {
        font_px_ = std::min(30, font_px_ + 2);
        render();
    });
}

void ReaderPage::show_article(const ReaderArticle& article) {
    article_ = article;
    render();
}

void ReaderPage::refresh() { render(); }

void ReaderPage::resizeEvent(QResizeEvent* event) {
    InternalPageWidget::resizeEvent(event);
    update_measure();
}

// The measure: centre a ~36em column so lines stay readable on wide windows.
void ReaderPage::update_measure() {
    if (!view_) return;
    const int measure = font_px_ * 36;
    const int side = std::max(0, (view_->viewport()->width() - measure) / 2);
    // Root-frame margins, not setViewportMargins (protected on the scroll
    // area); setHtml() resets the frame format, so render() calls this last.
    QTextFrame* root = view_->document()->rootFrame();
    QTextFrameFormat fmt = root->frameFormat();
    fmt.setLeftMargin(side);
    fmt.setRightMargin(side);
    root->setFrameFormat(fmt);
}

void ReaderPage::render() {
    const Theme& theme = current_theme();
    // Reader typography is deliberately its own: a serif text face, a
    // ~70-character measure and generous leading, on a paper-like surface in
    // the light theme and a soft (not pure black) one in the dark theme.
    const bool dark = theme.is_dark;
    const QString bg = dark ? QStringLiteral("#15171c") : QStringLiteral("#fbf8f1");
    const QString fg = dark ? QStringLiteral("#d9dbe0") : QStringLiteral("#22201c");
    const QString muted = dark ? QStringLiteral("#9097a3") : QStringLiteral("#6b665c");
    const QString link = dark ? QStringLiteral("#8ab4f8") : QStringLiteral("#1a5fb4");
    const QString code_bg = dark ? QStringLiteral("#1f2229") : QStringLiteral("#f0ebe0");
    view_->setStyleSheet(QStringLiteral("QTextBrowser#ReaderView { background: %1; border: none; }")
                             .arg(bg));
    view_->document()->setDefaultStyleSheet(
        QStringLiteral(
            "body { color: %1; font-family: Georgia, 'Iowan Old Style', 'Noto Serif', serif; }"
            "a { color: %2; text-decoration: none; }"
            "h1 { font-size: %3px; font-weight: 700; margin-bottom: 4px; }"
            "h2 { font-size: %4px; margin-top: 18px; }"
            "h3, h4, h5, h6 { font-size: %5px; margin-top: 14px; }"
            "p, li, dd { font-size: %6px; line-height: 160%; }"
            "blockquote { color: %7; font-style: italic; margin-left: 18px; }"
            "pre, code { font-family: 'JetBrains Mono', Consolas, monospace; font-size: %8px;"
            "  background: %9; }"
            ".meta { color: %7; font-size: %8px; font-family: sans-serif; }"
            ".img { color: %7; font-style: italic; }")
            .arg(fg, link)
            .arg(font_px_ + 13)
            .arg(font_px_ + 6)
            .arg(font_px_ + 2)
            .arg(font_px_)
            .arg(muted)
            .arg(std::max(12, font_px_ - 4))
            .arg(code_bg));
    view_->document()->setDocumentMargin(24);

    QString html;
    if (!article_.ok) {
        html = QStringLiteral("<h2>No readable article</h2><p>%1</p>")
                   .arg(esc(article_.message.isEmpty()
                                ? QStringLiteral("Open an article and choose Reader Mode "
                                                 "(Ctrl+Alt+R).")
                                : article_.message));
        original_->setEnabled(!article_.source_url.isEmpty());
    } else {
        QString meta = esc(article_.site);
        if (!article_.byline.isEmpty()) meta += QStringLiteral(" · ") + esc(article_.byline);
        if (article_.words > 0) {
            meta += QStringLiteral(" · %1 min read").arg(std::max(1, article_.words / 230));
        }
        html = QStringLiteral("<h1>%1</h1><p class=\"meta\">%2</p>%3")
                   .arg(esc(article_.title), meta, article_.html);
        original_->setEnabled(true);
    }
    view_->setHtml(html);
    update_measure();
}

} // namespace aurora::ui
