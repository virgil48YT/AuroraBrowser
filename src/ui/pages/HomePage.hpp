// Aurora — home page (aurora://home, the new-tab page).
//
// The search-centered new-tab surface from the aurora-glass design: a dark
// nebula background painted here, the Aurora wordmark above one big centered
// search pill, and the circular search orb that doubles as the quiet-state
// affordance. Qt widget so the page lives natively in the content stack like
// every other aurora:// page. The page paints its own backdrop in
// paintEvent(), so it deliberately does NOT use the InternalPageWidget
// skeleton — it only re-states that base's signal contract so MainWindow can
// register it exactly like HistoryPage & co.
#pragma once

#include <QString>
#include <QWidget>

#include <cstdint>
#include <functional>

class QLineEdit;
class QPaintEvent;
class QResizeEvent;
class QToolButton;

namespace aurora::ui {

class HomePage : public QWidget {
    Q_OBJECT
public:
    // MainWindow wires the page with a projector that turns the raw query
    // into a resolved target URL (the active search engine's results page,
    // or the typed address as-is when it already parses as a URL).
    using SearchUrlFor = std::function<QString(const QString&)>;

    explicit HomePage(QWidget* parent = nullptr);
    ~HomePage() override;

    void set_query_url_resolver(SearchUrlFor fn);

signals:
    // Same contract as InternalPageWidget: typing a query (Enter or the orb)
    // asks the window to open it in the active tab; the window owns the
    // search-engine routing.
    void open_url_requested(const QString& url);
    void status_message(const QString& text);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void submit();
    void on_query_changed(const QString& text);

private:
    void position_orb();

    QLineEdit* search_ = nullptr;
    QToolButton* search_btn_ = nullptr;  // orb inside the pill's trailing margin
    QToolButton* orb_ = nullptr;         // floating bottom-right orb (rest state)
    SearchUrlFor query_url_for_;         // window-provided query → URL projector
    std::uint64_t theme_observer_ = 0;   // ThemeState subscription (orb icons)
};

} // namespace aurora::ui
