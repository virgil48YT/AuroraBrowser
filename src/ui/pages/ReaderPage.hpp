// Aurora — Reader mode (aurora://reader).
//
// A clean-text view of the article on the current page. The extraction runs
// in the page itself (kExtractionScript, a small Readability-style heuristic:
// prefer <article>/<main>, else the block whose paragraphs score highest by
// text length and comma count, minus link-heavy and boilerplate blocks) and
// returns JSON with title, byline, site name and SANITIZED HTML — the script
// rebuilds the article from a whitelist of text tags with every text node
// escaped, keeps only http(s) links, and turns images into their alt text,
// so nothing from the page can run here and no remote resource is fetched by
// the reader view (it is a QTextBrowser, which also never runs scripts).
//
// The page is one widget per window, like the other internal pages, but it
// keeps one article per reader tab: MainWindow calls show_article() with the
// tab's article every time that tab is shown. A reader tab restored from a
// previous session has no article and shows the fallback message.
#pragma once

#include <QString>

#include "ui/pages/InternalPageWidget.hpp"

class QTextBrowser;
class QPushButton;

namespace aurora::ui {

struct ReaderArticle {
    bool ok = false;
    QString title;
    QString byline;
    QString site;
    QString source_url;
    QString html;          // sanitized by the extraction script
    QString message;       // why there is no article (ok == false)
    int words = 0;
};

class ReaderPage : public InternalPageWidget {
    Q_OBJECT
public:
    explicit ReaderPage(QWidget* parent = nullptr);

    // The JavaScript evaluated in the source page. Evaluates to a JSON string.
    static const char* extraction_script();
    // Parse the script's JSON result; ok == false with a message when the
    // page has no readable article.
    static ReaderArticle parse_result(bool eval_ok, const QString& json,
                                      const QString& source_url);

    void show_article(const ReaderArticle& article);
    void refresh() override;

signals:
    // "Open original" — the window navigates back to the source page.
    void open_original_requested(const QString& url);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void render();
    void update_measure();

    ReaderArticle article_;
    QTextBrowser* view_ = nullptr;
    QPushButton* original_ = nullptr;
    QPushButton* smaller_ = nullptr;
    QPushButton* larger_ = nullptr;
    int font_px_ = 19;
};

} // namespace aurora::ui
