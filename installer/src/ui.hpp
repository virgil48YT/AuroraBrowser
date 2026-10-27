// Aurora Browser Setup — wizard window.
#pragma once

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "install.hpp"
#include "payload.hpp"
#include "strings.hpp"

namespace aurora::setup {

struct SetupArgs {
    bool verify_payload = false;
    bool update_mode = false;
    bool relaunch = false;
    bool elevated = false;
    bool has_components = false;
    bool desktop_shortcut = true;
    bool start_menu_shortcut = true;
    bool has_language = false;
    Language language = Language::En;
    std::wstring install_dir;
    std::wstring log_path;
};

class SetupWindow {
public:
    explicit SetupWindow(SetupArgs args);
    ~SetupWindow();
    SetupWindow(const SetupWindow&) = delete;
    SetupWindow& operator=(const SetupWindow&) = delete;

    bool create(HINSTANCE instance);
    int run();

private:
    enum class Page { Language, Welcome, Location, Components, Installing,
                      Complete, Failed, ThankYou };
    enum class HitId {
        None,
        TitleMin, TitleMax, TitleClose,
        Back, Next, Cancel, Finish, Close, Launch, Browse,
        PathField, Combo,
        Lang0, Lang1, Lang2, Lang3, Lang4,
        CheckRequired, CheckDesktop, CheckStart,
        CheckLaunch, CheckCompleteDesktop,
    };
    struct Hit {
        HitId id = HitId::None;
        D2D1_RECT_F rect{};
        bool enabled = true;
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                       LPARAM lparam);
    static LRESULT CALLBACK EditProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam, UINT_PTR subclass,
                                     DWORD_PTR ref);
    LRESULT handle_message(HWND hwnd, UINT message, WPARAM wparam,
                           LPARAM lparam);

    void create_edit_control();
    void update_edit_font();
    void position_edit();
    bool create_factories();
    bool create_text_formats();
    bool ensure_render_target();
    void discard_render_target();
    bool load_brand_images();
    bool load_bitmap_from_resource(int resource_id,
                                   ID2D1Bitmap** bitmap_out);
    void update_window_region();

    void set_page(Page page);
    void layout();
    void add_hit(HitId id, D2D1_RECT_F rect, bool enabled = true);
    D2D1_RECT_F hit_rect(HitId id) const;
    HitId hit_test(float x, float y) const;
    void update_space();
    void update_window_title();
    void focus_next(bool backward);
    void set_focus(HitId id);
    void activate(HitId id);
    void choose_language(int index);
    void open_language_dropdown();
    void close_language_dropdown();
    void open_folder_dialog();
    void read_path_from_edit();
    bool validate_path(bool show_error);
    bool ensure_aurora_stopped();
    void start_install();
    void on_install_done();
    void finish_install();
    void launch_now(bool final_page);
    bool confirm_stop_install();
    void request_close();
    void show_invalid_path();
    void show_win32_error(const wchar_t* title, DWORD error,
                          const std::wstring& detail);
    void maybe_elevate_and_retry();

    void paint();

    void draw_background();
    void draw_titlebar();
    void draw_rail();
    void draw_footer();
    void draw_language_page();
    void draw_welcome_page();
    void draw_location_page();
    void draw_components_page();
    void draw_installing_page();
    void draw_complete_page();
    void draw_failed_page();
    void draw_thankyou_page();
    void draw_language_dropdown();
    void draw_window_border();

    void draw_logo_block(float center_x, float top, float width,
                         bool show_tagline);
    void draw_button(HitId id, const wchar_t* label, bool primary,
                     bool chevron_right, bool chevron_left);
    void draw_checkbox(HitId id, bool checked, const wchar_t* label,
                       bool enabled);
    void draw_folder_icon(D2D1_RECT_F box);
    void draw_components_icon(D2D1_RECT_F box);
    void draw_globe_icon(D2D1_RECT_F box);
    void draw_chevron(D2D1_POINT_2F center, float size, bool down,
                      bool left, D2D1_COLOR_F color);
    void draw_check_glyph(D2D1_POINT_2F center, float size);
    void draw_success_icon(D2D1_RECT_F box);
    void draw_warning_icon(D2D1_RECT_F box);

    void draw_text(const std::wstring& text, IDWriteTextFormat* format,
                   D2D1_RECT_F box, D2D1_COLOR_F color,
                   DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                   DWRITE_PARAGRAPH_ALIGNMENT paragraph =
                       DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    float measure_text(const std::wstring& text, IDWriteTextFormat* format);
    void fill_rect(D2D1_RECT_F rect, D2D1_COLOR_F color);
    void fill_round(D2D1_RECT_F rect, float radius, D2D1_COLOR_F color);
    void stroke_round(D2D1_RECT_F rect, float radius, D2D1_COLOR_F color,
                      float width);
    void fill_round_gradient(D2D1_RECT_F rect, float radius, D2D1_COLOR_F a,
                             D2D1_COLOR_F b);
    void glow_round(D2D1_RECT_F rect, float radius, D2D1_COLOR_F color);
    void draw_line(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_COLOR_F color,
                   float width);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> make_brush(
        D2D1_COLOR_F color);
    float dip(int pixels) const { return pixels * 96.0f / dpi_; }
    int pixels(float dip_value) const {
        return static_cast<int>(dip_value * dpi_ / 96.0f + 0.5f);
    }
    float width_dip() const;
    float height_dip() const;
    std::wstring elide_left(const std::wstring& text,
                            IDWriteTextFormat* format, float max_width);
    std::wstring elide_right(const std::wstring& text,
                             IDWriteTextFormat* format, float max_width);
    float primary_button_width(const wchar_t* label) const;
    float secondary_button_width(const wchar_t* label) const;
    std::wstring components_argument() const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    HFONT edit_font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    UINT_PTR timer_ = 0;
    float dpi_ = 96.0f;
    bool edit_syncing_ = false;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> render_target_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> brand_mark_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> brand_logo_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> brand_background_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> title_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> page_title_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> body_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> small_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> button_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> tagline_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> code_format_;

    SetupArgs args_;
    const Strings* text_ = nullptr;
    Language language_ = Language::En;
    Page page_ = Page::Language;
    std::wstring install_dir_;
    std::wstring inline_message_;

    bool language_open_ = false;
    int language_hover_ = -1;
    bool desktop_choice_ = true;
    bool start_menu_choice_ = true;
    bool launch_choice_ = true;
    bool complete_desktop_choice_ = true;

    HitId hover_ = HitId::None;
    HitId pressed_ = HitId::None;
    HitId focus_ = HitId::None;
    bool tracking_leave_ = false;
    std::vector<Hit> hits_;
    std::vector<HitId> focus_order_;

    D2D1_RECT_F content_{};
    D2D1_RECT_F rail_{};
    D2D1_RECT_F footer_{};
    D2D1_RECT_F edit_field_{};
    D2D1_RECT_F edit_text_{};
    D2D1_RECT_F combo_{};
    D2D1_RECT_F progress_bar_{};
    D2D1_RECT_F dropdown_{};
    float rail_width_ = 300.0f;
    float title_height_ = 38.0f;

    PayloadArchive archive_;
    bool archive_open_ = false;
    DWORD archive_error_ = ERROR_SUCCESS;
    std::wstring archive_detail_;
    InstallProgress progress_;
    InstallOptions options_;
    InstallResult result_;
    std::thread worker_;
    std::atomic<bool> installing_{false};
    bool close_when_done_ = false;
    bool cancel_pending_ = false;
    bool start_when_shown_ = false;
    bool update_started_ = false;
};

} // namespace aurora::setup
