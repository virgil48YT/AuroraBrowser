// Aurora — home page (aurora://home, the new-tab page).
//
// The neon new-tab surface from the aurora reference: a deep cosmic navy
// gradient with cyan (left) and magenta (right) aurora glows painted in
// paintEvent(); a live clock + date in the top-right corner and a weather
// chip in the top-left; the gradient-ribbon "A" logo and widely-tracked
// "A U R O R A" wordmark above one glowing gradient-outlined search pill; the
// user's own shortcut tiles (none by default, added with the "Add shortcut"
// tile); and three bottom cards (resume rows, recently visited, quick
// actions). Real history feeds the two list cards; with no history they show
// an empty state rather than made-up rows. The weather chip shows live
// conditions (see WeatherService) and stays hidden until they arrive.
//
// Qt widget so the page lives natively in the content stack like every other
// aurora:// page. The page paints its own backdrop in paintEvent(), so it
// deliberately does NOT use the InternalPageWidget skeleton — it only
// re-states that base's signal contract so MainWindow can register it
// exactly like HistoryPage & co.
#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

#include "ui/ShortcutList.hpp"

#include <cstdint>
#include <functional>

class QEvent;
class QLabel;
class QLineEdit;
class QPaintEvent;
class QResizeEvent;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace aurora::history { class HistoryService; }
namespace aurora::settings { class Settings; }

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
    // Optional live history: the "Continue where you left off" and
    // "Recently visited" cards re-render from the newest visits. Safe to
    // call with nullptr to fall back to the built-in mock rows.
    void set_history_service(history::HistoryService* history);
    // Profile preferences: where the user's shortcut tiles and pinned
    // weather city are stored. Safe to call with nullptr (nothing persists).
    void set_prefs(settings::Settings* prefs);

signals:
    // Same contract as InternalPageWidget: typing a query (Enter or the
    // sparkle) asks the window to open it in the active tab; shortcut tiles
    // and card rows emit fully-resolved targets. The window owns routing.
    void open_url_requested(const QString& url);
    void status_message(const QString& text);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void submit();

private:
    // One row of either history card: icon + title/subtitle (+ an optional
    // progress bar) + a quiet time label.
    struct HistoryEntry {
        QString url;
        QString title;
        std::int64_t when_epoch = 0;    // seconds; color strip is derived
    };

    void build_ui();
    QWidget* build_shortcut_tile(QWidget* parent, const QString& label,
                                 const QString& target_url, bool is_add,
                                 int index);
    void rebuild_shortcuts();                  // tiles from shortcuts_
    void add_shortcut();
    void edit_shortcut(int index);
    void remove_shortcut(int index);
    void update_weather();                     // chip from WeatherService
    void show_weather_menu(const QPoint& global_pos);
    void set_weather_city();
    QWidget* build_history_card(QWidget* parent, const QString& heading);
    QWidget* build_actions_card(QWidget* parent);
    QWidget* build_weather_chip();
    QWidget* build_clock_block();
    void render_history_rows();
    void render_entry(QVBoxLayout* rows, const HistoryEntry& entry);
    QString time_ago(std::int64_t epoch_seconds) const;
    void apply_icons();   // re-stamp every glyph from the live theme

    QWidget* cards_row_ = nullptr;            // the three bottom cards
    QVBoxLayout* resume_rows_ = nullptr;
    QVBoxLayout* recent_rows_ = nullptr;

    QLineEdit* search_ = nullptr;
    QToolButton* search_btn_ = nullptr;       // leading glyph inside the pill
    QToolButton* sparkle_btn_ = nullptr;      // trailing sparkle (submit)
    QLabel* clock_label_ = nullptr;
    QLabel* date_label_ = nullptr;
    QLabel* weather_icon_ = nullptr;
    QWidget* weather_chip_ = nullptr;
    QLabel* weather_temp_ = nullptr;
    QLabel* weather_sub_ = nullptr;
    QWidget* tiles_host_ = nullptr;           // holds the shortcut rows
    QVBoxLayout* tiles_layout_ = nullptr;
    settings::Settings* prefs_ = nullptr;
    QVector<Shortcut> shortcuts_;
    QTimer* clock_timer_ = nullptr;

    QVector<QToolButton*> shortcut_buttons_;  // tiles (icon re-tint helper)
    QVector<QToolButton*> action_buttons_;    // quick-action rows

    history::HistoryService* history_ = nullptr;

    // The aurora wallpaper is scaled-once-painted-many (see paintEvent).
    QPixmap bg_;
    QPixmap bg_source_;

    SearchUrlFor query_url_for_;              // window-provided query → URL
    std::uint64_t theme_observer_ = 0;        // ThemeState subscription
};

} // namespace aurora::ui
