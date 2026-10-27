#include "ui.hpp"

#include <commctrl.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <iterator>
#include <new>
#include <utility>

#include "win_util.hpp"

namespace aurora::setup {
namespace {

constexpr wchar_t kWindowClass[] = L"AuroraSetupWindow";
constexpr int kEditId = 1001;
constexpr UINT_PTR kProgressTimer = 1;
constexpr UINT kMessageInstallDone = WM_APP + 1;
constexpr UINT kMessageStartInstall = WM_APP + 2;

constexpr int kResMark = 101;
constexpr int kResLogo = 102;
constexpr int kResBackground = 103;

constexpr uint32_t kText = 0xEDF4FF;
constexpr uint32_t kTextSoft = 0xC6D4E8;
constexpr uint32_t kMuted = 0x97A9C6;
constexpr uint32_t kFaint = 0x667792;
constexpr uint32_t kAccent = 0x2FDCFF;
constexpr uint32_t kAccent2 = 0xA855F7;
constexpr uint32_t kDanger = 0xFF3D6E;
constexpr uint32_t kField = 0x08101F;
constexpr uint32_t kLine = 0x253758;
constexpr uint32_t kPanel = 0x070D1A;

#ifdef AURORA_SETUP_VERSION
std::wstring setup_version() {
    std::wstring result;
    for (const char* p = AURORA_SETUP_VERSION; p && *p; ++p) {
        result.push_back(static_cast<wchar_t>(*p));
    }
    return result;
}
#else
std::wstring setup_version() { return L"0.0.0"; }
#endif
const std::wstring kSetupVersion = setup_version();

D2D1_COLOR_F color(uint32_t rgb, float alpha = 1.0f) {
    return D2D1::ColorF(rgb, alpha);
}

bool contains(D2D1_RECT_F rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top &&
           y < rect.bottom;
}

D2D1_RECT_F inset(D2D1_RECT_F rect, float amount) {
    return D2D1::RectF(rect.left + amount, rect.top + amount,
                       rect.right - amount, rect.bottom - amount);
}

D2D1_RECT_F offset(D2D1_RECT_F rect, float dx, float dy) {
    return D2D1::RectF(rect.left + dx, rect.top + dy, rect.right + dx,
                       rect.bottom + dy);
}

float rect_width(D2D1_RECT_F rect) { return rect.right - rect.left; }
float rect_height(D2D1_RECT_F rect) { return rect.bottom - rect.top; }

std::wstring hresult_message(HRESULT hr) {
    const DWORD code = HRESULT_FACILITY(hr) == FACILITY_WIN32
                           ? static_cast<DWORD>(HRESULT_CODE(hr))
                           : ERROR_GEN_FAILURE;
    return format_win32_message(code);
}

} // namespace

SetupWindow::SetupWindow(SetupArgs args)
    : args_(std::move(args)) {
    language_ = args_.has_language ? args_.language : system_language();
    text_ = &strings(language_);
    install_dir_ = args_.install_dir.empty()
                       ? default_install_dir()
                       : normalize_path(trim_copy(args_.install_dir));
    if (args_.has_components) {
        desktop_choice_ = args_.desktop_shortcut;
        start_menu_choice_ = args_.start_menu_shortcut;
    } else if (args_.update_mode) {
        desktop_choice_ = shortcut_exists_for(true);
        start_menu_choice_ = shortcut_exists_for(false);
    }
    complete_desktop_choice_ = desktop_choice_;
}

SetupWindow::~SetupWindow() {
    if (worker_.joinable()) {
        progress_.cancel.store(true, std::memory_order_relaxed);
        worker_.join();
    }
    if (timer_ && hwnd_) {
        KillTimer(hwnd_, timer_);
        timer_ = 0;
    }
    if (edit_font_) DeleteObject(edit_font_);
    if (edit_brush_) DeleteObject(edit_brush_);
}

bool SetupWindow::create(HINSTANCE instance) {
    instance_ = instance;

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = &SetupWindow::WindowProc;
    window_class.hInstance = instance;
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    window_class.hIconSm = window_class.hIcon;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    HDC screen = GetDC(nullptr);
    dpi_ = screen ? static_cast<float>(GetDeviceCaps(screen, LOGPIXELSX)) : 96.0f;
    if (screen) ReleaseDC(nullptr, screen);
    if (dpi_ < 96.0f) dpi_ = 96.0f;

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int work_width = std::max(1, static_cast<int>(work.right - work.left));
    const int work_height = std::max(1, static_cast<int>(work.bottom - work.top));
    int window_width = std::min(pixels(900.0f), work_width - 24);
    int window_height = std::min(pixels(560.0f), work_height - 24);
    window_width = std::max(window_width, pixels(600.0f));
    window_height = std::max(window_height, pixels(420.0f));
    const int x = work.left + (work_width - window_width) / 2;
    const int y = work.top + (work_height - window_height) / 2;

    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW, kWindowClass, text_->app_title,
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
            WS_SYSMENU,
        x, y, window_width, window_height, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    SendMessageW(hwnd_, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(
                     LoadIconW(instance_, MAKEINTRESOURCEW(1))));
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(
                     LoadIconW(instance_, MAKEINTRESOURCEW(1))));

    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(hwnd_, 19, &dark, sizeof(dark));
    }
    int corners = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33, &corners, sizeof(corners));

    create_edit_control();
    if (!create_factories() || !create_text_formats()) {
        MessageBoxW(hwnd_,
                    L"The installer could not initialize its graphics.",
                    text_->app_title, MB_OK | MB_ICONERROR);
        return false;
    }

    archive_open_ = archive_.open(module_path(), archive_detail_, archive_error_);
    if (!archive_open_) {
        result_.ok = false;
        result_.error = archive_error_;
        result_.hr = HRESULT_FROM_WIN32(archive_error_);
        result_.detail = archive_detail_;
        set_page(Page::Failed);
    } else if (args_.update_mode || args_.elevated) {
        start_when_shown_ = true;
        set_page(Page::Installing);
    } else {
        set_page(Page::Language);
    }

    log_line(L"Aurora Browser Setup " + std::wstring(kSetupVersion));
    log_line(L"mode: " + std::wstring(args_.update_mode ? L"update" : L"install"));
    log_line(L"install dir: " + install_dir_);
    if (!archive_open_) {
        log_line(L"payload open failed: " + format_hresult(
                     HRESULT_FROM_WIN32(archive_error_)) + L" " +
                 archive_detail_);
    } else {
        log_line(L"payload: " + std::to_wstring(archive_.entries().size()) +
                 L" entries, " +
                 std::to_wstring(archive_.total_uncompressed()) + L" bytes");
    }

    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    if (start_when_shown_ && archive_open_) {
        PostMessageW(hwnd_, kMessageStartInstall, 0, 0);
    }
    return true;
}

int SetupWindow::run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK SetupWindow::WindowProc(HWND hwnd, UINT message,
                                         WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        auto* self = static_cast<SetupWindow*>(create->lpCreateParams);
        if (self) {
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
    }
    auto* self =
        reinterpret_cast<SetupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->handle_message(hwnd, message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK SetupWindow::EditProc(HWND hwnd, UINT message,
                                       WPARAM wparam, LPARAM lparam,
                                       UINT_PTR subclass, DWORD_PTR ref) {
    auto* self = reinterpret_cast<SetupWindow*>(ref);
    if (self) {
        if (message == WM_GETDLGCODE) {
            return DLGC_WANTTAB | DLGC_WANTARROWS;
        }
        if (message == WM_KEYDOWN) {
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (wparam == VK_TAB) {
                self->focus_next(shift);
                return 0;
            }
            if (wparam == VK_RETURN) {
                self->activate(HitId::Next);
                return 0;
            }
            if (wparam == VK_ESCAPE) {
                self->request_close();
                return 0;
            }
        }
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(hwnd, &SetupWindow::EditProc, subclass);
        }
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

void SetupWindow::create_edit_control() {
    edit_brush_ = CreateSolidBrush(RGB(8, 16, 31));
    edit_ = CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | ES_LEFT | ES_AUTOHSCROLL, 0, 0, 10, 10,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)),
        instance_, nullptr);
    if (!edit_) return;
    SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELONG(0, 0));
    SetWindowSubclass(edit_, &SetupWindow::EditProc, 1,
                      reinterpret_cast<DWORD_PTR>(this));
    update_edit_font();
    edit_syncing_ = true;
    SetWindowTextW(edit_, install_dir_.c_str());
    edit_syncing_ = false;
    ShowWindow(edit_, SW_HIDE);
}

void SetupWindow::update_edit_font() {
    if (!edit_) return;
    HFONT font = CreateFontW(
        -MulDiv(13, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL, FALSE,
        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!font) return;
    if (edit_font_) DeleteObject(edit_font_);
    edit_font_ = font;
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_),
                 TRUE);
}

void SetupWindow::position_edit() {
    if (!edit_) return;
    if (page_ != Page::Location) {
        if (IsWindowVisible(edit_)) {
            if (GetFocus() == edit_) SetFocus(hwnd_);
            ShowWindow(edit_, SW_HIDE);
        }
        return;
    }
    const int x = pixels(edit_text_.left);
    const int y = pixels(edit_text_.top);
    const int width = std::max(10, pixels(rect_width(edit_text_)));
    const int height = std::max(10, pixels(rect_height(edit_text_)));
    SetWindowPos(edit_, HWND_TOP, x, y, width, height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (!IsWindowVisible(edit_)) ShowWindow(edit_, SW_SHOWNA);
}

bool SetupWindow::create_factories() {
    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
        reinterpret_cast<void**>(d2d_factory_.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return false;
    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwrite_factory_.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return false;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                          CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                          reinterpret_cast<void**>(
                              wic_factory_.ReleaseAndGetAddressOf()));
    return SUCCEEDED(hr);
}

bool SetupWindow::create_text_formats() {
    const auto make = [this](float size, DWRITE_FONT_WEIGHT weight,
                             const wchar_t* family,
                             Microsoft::WRL::ComPtr<IDWriteTextFormat>& out) {
        HRESULT hr = dwrite_factory_->CreateTextFormat(
            family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"en-US",
            out.ReleaseAndGetAddressOf());
        if (FAILED(hr)) return false;
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        return true;
    };
    return make(24.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, L"Segoe UI",
                title_format_) &&
           make(21.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, L"Segoe UI",
                page_title_format_) &&
           make(13.0f, DWRITE_FONT_WEIGHT_NORMAL, L"Segoe UI", body_format_) &&
           make(11.5f, DWRITE_FONT_WEIGHT_NORMAL, L"Segoe UI", small_format_) &&
           make(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, L"Segoe UI",
                button_format_) &&
           make(13.0f, DWRITE_FONT_WEIGHT_NORMAL, L"Segoe UI",
                tagline_format_) &&
           make(12.0f, DWRITE_FONT_WEIGHT_NORMAL, L"Consolas", code_format_);
}

bool SetupWindow::ensure_render_target() {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const UINT width = static_cast<UINT>(std::max(1, static_cast<int>(client.right)));
    const UINT height = static_cast<UINT>(std::max(1, static_cast<int>(client.bottom)));
    if (render_target_) {
        const D2D1_SIZE_U current = render_target_->GetPixelSize();
        if (current.width == width && current.height == height) return true;
        if (SUCCEEDED(render_target_->Resize(D2D1::SizeU(width, height)))) {
            return true;
        }
        discard_render_target();
    }
    const D2D1_RENDER_TARGET_PROPERTIES properties =
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN),
            dpi_, dpi_);
    const HRESULT hr = d2d_factory_->CreateHwndRenderTarget(
        properties,
        D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(width, height)),
        render_target_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;
    render_target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    render_target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    load_brand_images();
    return true;
}

void SetupWindow::discard_render_target() {
    brand_background_.Reset();
    brand_logo_.Reset();
    brand_mark_.Reset();
    render_target_.Reset();
}

bool SetupWindow::load_bitmap_from_resource(int resource_id,
                                            ID2D1Bitmap** bitmap_out) {
    if (!bitmap_out) return false;
    *bitmap_out = nullptr;
    HRSRC resource =
        FindResourceW(instance_, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (!resource) return false;
    const DWORD size = SizeofResource(instance_, resource);
    HGLOBAL loaded = LoadResource(instance_, resource);
    void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data || size == 0) return false;

    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic_factory_->CreateStream(stream.GetAddressOf())) ||
        FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(data), size)) ||
        FAILED(wic_factory_->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
            decoder.GetAddressOf())) ||
        FAILED(decoder->GetFrame(0, frame.GetAddressOf())) ||
        FAILED(wic_factory_->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(
            frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom))) {
        return false;
    }
    return SUCCEEDED(render_target_->CreateBitmapFromWicBitmap(
        converter.Get(), nullptr, bitmap_out));
}

bool SetupWindow::load_brand_images() {
    if (!render_target_ || !wic_factory_) return false;
    if (!brand_mark_ && !load_bitmap_from_resource(
                             kResMark, brand_mark_.ReleaseAndGetAddressOf())) {
        log_line(L"Brand mark image could not be loaded.");
    }
    if (!brand_logo_ && !load_bitmap_from_resource(
                             kResLogo, brand_logo_.ReleaseAndGetAddressOf())) {
        log_line(L"Brand logo image could not be loaded.");
    }
    if (!brand_background_ &&
        !load_bitmap_from_resource(kResBackground,
                                   brand_background_.ReleaseAndGetAddressOf())) {
        log_line(L"Background image could not be loaded.");
    }
    return true;
}

void SetupWindow::update_window_region() {
    if (!hwnd_) return;
    if (IsZoomed(hwnd_)) {
        SetWindowRgn(hwnd_, nullptr, TRUE);
        return;
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;
    const int radius = std::max(2, pixels(11.0f));
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius * 2,
                                     radius * 2);
    if (region) SetWindowRgn(hwnd_, region, TRUE);
}

void SetupWindow::update_window_title() {
    if (hwnd_) SetWindowTextW(hwnd_, text_->app_title);
}

float SetupWindow::width_dip() const {
    if (!hwnd_) return 900.0f;
    RECT rect{};
    GetClientRect(hwnd_, &rect);
    return static_cast<float>(rect.right - rect.left) * 96.0f / dpi_;
}

float SetupWindow::height_dip() const {
    if (!hwnd_) return 560.0f;
    RECT rect{};
    GetClientRect(hwnd_, &rect);
    return static_cast<float>(rect.bottom - rect.top) * 96.0f / dpi_;
}

void SetupWindow::add_hit(HitId id, D2D1_RECT_F rect, bool enabled) {
    hits_.push_back(Hit{id, rect, enabled});
}

D2D1_RECT_F SetupWindow::hit_rect(HitId id) const {
    for (const Hit& hit : hits_) {
        if (hit.id == id) return hit.rect;
    }
    return D2D1::RectF(0, 0, 0, 0);
}

SetupWindow::HitId SetupWindow::hit_test(float x, float y) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (it->enabled && contains(it->rect, x, y)) return it->id;
    }
    return HitId::None;
}

float SetupWindow::primary_button_width(const wchar_t* label) const {
    if (!button_format_) return 104.0f;
    return std::max(104.0f, measure_text(label, button_format_.Get()) + 44.0f);
}

float SetupWindow::secondary_button_width(const wchar_t* label) const {
    if (!button_format_) return 100.0f;
    return std::max(96.0f, measure_text(label, button_format_.Get()) + 38.0f);
}

void SetupWindow::layout() {
    const float width = width_dip();
    const float height = height_dip();
    hits_.clear();
    focus_order_.clear();
    if (width < 1.0f || height < 1.0f) return;

    // The mockup uses a narrower logo rail on the Installing and Complete
    // pages (and a wider content area for the progress/details), while the
    // wizard pages keep the reference wizard's proportion.
    if (page_ == Page::Installing || page_ == Page::Complete) {
        rail_width_ = 220.0f;
    } else {
        rail_width_ = width >= 820.0f ? 300.0f : 264.0f;
    }
    title_height_ = 38.0f;
    rail_ = D2D1::RectF(0, title_height_, rail_width_, height);
    content_ = D2D1::RectF(rail_width_ + 38.0f, title_height_ + 26.0f,
                           width - 38.0f, height - 96.0f);
    footer_ = D2D1::RectF(rail_width_ + 30.0f, height - 64.0f, width - 30.0f,
                          height - 28.0f);
    if (page_ == Page::ThankYou) {
        content_ = D2D1::RectF(40.0f, title_height_ + 20.0f, width - 40.0f,
                               height - 96.0f);
        footer_ = D2D1::RectF(30.0f, height - 72.0f, width - 30.0f,
                              height - 30.0f);
    }

    // Title-bar buttons.
    const float title_button_width = 46.0f;
    const float title_button_top = 0.0f;
    add_hit(HitId::TitleMin,
            D2D1::RectF(width - 3 * title_button_width, title_button_top,
                        width - 2 * title_button_width,
                        title_button_top + title_height_));
    add_hit(HitId::TitleMax,
            D2D1::RectF(width - 2 * title_button_width, title_button_top,
                        width - title_button_width,
                        title_button_top + title_height_));
    add_hit(HitId::TitleClose,
            D2D1::RectF(width - title_button_width, title_button_top, width,
                        title_button_top + title_height_));

    dropdown_ = D2D1::RectF(0, 0, 0, 0);
    language_open_ = language_open_ && page_ == Page::Language;
    if (page_ == Page::Language) {
        const float combo_width = std::min(470.0f, rect_width(content_));
        combo_ = D2D1::RectF(content_.left, content_.top + 100.0f,
                             content_.left + combo_width,
                             content_.top + 142.0f);
        add_hit(HitId::Combo, combo_);
        if (language_open_) {
            const float item_height = 36.0f;
            const float list_height =
                8.0f + item_height * static_cast<float>(language_count());
            dropdown_ = D2D1::RectF(combo_.left, combo_.bottom + 6.0f,
                                    combo_.right,
                                    combo_.bottom + 6.0f + list_height);
            for (int i = 0; i < language_count() && i < 5; ++i) {
                add_hit(static_cast<HitId>(
                            static_cast<int>(HitId::Lang0) + i),
                        D2D1::RectF(dropdown_.left + 4.0f,
                                    dropdown_.top + 4.0f +
                                        item_height * static_cast<float>(i),
                                    dropdown_.right - 4.0f,
                                    dropdown_.top + 4.0f +
                                        item_height * static_cast<float>(i + 1)));
            }
        }
    } else {
        combo_ = D2D1::RectF(0, 0, 0, 0);
    }

    if (page_ == Page::Location) {
        const float field_top = content_.top + 104.0f;
        edit_field_ = D2D1::RectF(content_.left, field_top,
                                  content_.right - 120.0f, field_top + 42.0f);
        edit_text_ = D2D1::RectF(edit_field_.left + 14.0f,
                                 edit_field_.top + 9.0f,
                                 edit_field_.right - 14.0f,
                                 edit_field_.bottom - 9.0f);
        add_hit(HitId::PathField, edit_field_);
        const D2D1_RECT_F browse =
            D2D1::RectF(content_.right - 108.0f, field_top, content_.right,
                        field_top + 42.0f);
        add_hit(HitId::Browse, browse);
    }

    if (page_ == Page::Components) {
        const float row_height = 52.0f;
        const float row_top = content_.top + 112.0f;
        const float group_width = std::min(rect_width(content_) * 0.60f, 350.0f);
        const HitId ids[3] = {HitId::CheckRequired, HitId::CheckDesktop,
                              HitId::CheckStart};
        for (int i = 0; i < 3; ++i) {
            add_hit(ids[i],
                    D2D1::RectF(content_.left + 8.0f,
                                row_top + row_height * static_cast<float>(i),
                                content_.left + group_width - 8.0f,
                                row_top + row_height * static_cast<float>(i) +
                                    row_height - 4.0f),
                    i != 0);
        }
    }

    if (page_ == Page::Complete) {
        add_hit(HitId::CheckLaunch,
                D2D1::RectF(content_.left + 2.0f, content_.top + 124.0f,
                            content_.left + rect_width(content_) - 8.0f,
                            content_.top + 168.0f));
        add_hit(HitId::CheckCompleteDesktop,
                D2D1::RectF(content_.left + 2.0f, content_.top + 172.0f,
                            content_.left + rect_width(content_) - 8.0f,
                            content_.top + 216.0f));
    }

    if (page_ == Page::Installing) {
        progress_bar_ = D2D1::RectF(content_.left, content_.top + 116.0f,
                                    content_.right - 66.0f,
                                    content_.top + 128.0f);
    } else {
        progress_bar_ = D2D1::RectF(0, 0, 0, 0);
    }

    // Footer buttons, right aligned in the mockup's Back / Next / Cancel order.
    struct ButtonSpec {
        HitId id;
        float width;
        bool enabled;
    };
    std::vector<ButtonSpec> buttons;
    const float gap = 12.0f;
    if (page_ == Page::Language) {
        buttons.push_back({HitId::Back, secondary_button_width(text_->btn_back),
                           false});
        buttons.push_back({HitId::Next, primary_button_width(text_->btn_next),
                           true});
        buttons.push_back(
            {HitId::Cancel, secondary_button_width(text_->btn_cancel), true});
    } else if (page_ == Page::Welcome) {
        buttons.push_back({HitId::Next, primary_button_width(text_->btn_next),
                           true});
        buttons.push_back(
            {HitId::Cancel, secondary_button_width(text_->btn_cancel), true});
    } else if (page_ == Page::Location || page_ == Page::Components) {
        buttons.push_back({HitId::Back, secondary_button_width(text_->btn_back),
                           true});
        buttons.push_back({HitId::Next, primary_button_width(text_->btn_next),
                           true});
        buttons.push_back(
            {HitId::Cancel, secondary_button_width(text_->btn_cancel), true});
    } else if (page_ == Page::Installing) {
        buttons.push_back(
            {HitId::Cancel, secondary_button_width(text_->btn_cancel), true});
    } else if (page_ == Page::Complete) {
        buttons.push_back(
            {HitId::Finish, primary_button_width(text_->btn_finish), true});
    } else if (page_ == Page::Failed) {
        buttons.push_back(
            {HitId::Close, secondary_button_width(text_->btn_close), true});
    } else if (page_ == Page::ThankYou) {
        const D2D1_RECT_F launch =
            D2D1::RectF(width / 2.0f - 105.0f, height - 76.0f,
                        width / 2.0f + 105.0f, height - 34.0f);
        add_hit(HitId::Launch, launch);
    }

    if (!buttons.empty()) {
        float total = 0;
        for (const ButtonSpec& button : buttons) total += button.width;
        total += gap * static_cast<float>(buttons.size() - 1);
        float x = width - 30.0f - total;
        const float y = height - 64.0f;
        const float button_height = 36.0f;
        for (const ButtonSpec& button : buttons) {
            add_hit(button.id,
                    D2D1::RectF(x, y, x + button.width, y + button_height),
                    button.enabled);
            x += button.width + gap;
        }
    }

    // Explicit keyboard order, independent of hit-testing order.
    const auto focus = [this](HitId id, bool enabled) {
        if (enabled) focus_order_.push_back(id);
    };
    switch (page_) {
        case Page::Language:
            focus(HitId::Combo, true);
            focus(HitId::Next, true);
            focus(HitId::Cancel, true);
            break;
        case Page::Welcome:
            focus(HitId::Next, true);
            focus(HitId::Cancel, true);
            break;
        case Page::Location:
            focus(HitId::PathField, true);
            focus(HitId::Browse, true);
            focus(HitId::Back, true);
            focus(HitId::Next, true);
            focus(HitId::Cancel, true);
            break;
        case Page::Components:
            focus(HitId::CheckDesktop, true);
            focus(HitId::CheckStart, true);
            focus(HitId::Back, true);
            focus(HitId::Next, true);
            focus(HitId::Cancel, true);
            break;
        case Page::Installing:
            focus(HitId::Cancel, true);
            break;
        case Page::Complete:
            focus(HitId::CheckLaunch, true);
            focus(HitId::CheckCompleteDesktop, true);
            focus(HitId::Finish, true);
            break;
        case Page::Failed:
            focus(HitId::Close, true);
            break;
        case Page::ThankYou:
            focus(HitId::Launch, true);
            break;
    }
    if (std::find(focus_order_.begin(), focus_order_.end(), focus_) ==
        focus_order_.end()) {
        focus_ = focus_order_.empty() ? HitId::None : focus_order_.front();
    }

    position_edit();
}

void SetupWindow::set_page(Page page) {
    if (timer_) {
        KillTimer(hwnd_, timer_);
        timer_ = 0;
    }
    page_ = page;
    language_open_ = false;
    language_hover_ = -1;
    pressed_ = HitId::None;
    hover_ = HitId::None;
    inline_message_.clear();

    switch (page) {
        case Page::Language:
        case Page::Welcome:
        case Page::Location:
        case Page::Components:
            focus_ = HitId::Next;
            break;
        case Page::Installing:
            focus_ = HitId::Cancel;
            break;
        case Page::Complete:
            focus_ = HitId::Finish;
            launch_choice_ = true;
            complete_desktop_choice_ = desktop_choice_;
            break;
        case Page::Failed:
            focus_ = HitId::Close;
            break;
        case Page::ThankYou:
            focus_ = HitId::Launch;
            break;
    }

    if (page == Page::Location && edit_) {
        edit_syncing_ = true;
        SetWindowTextW(edit_, install_dir_.c_str());
        edit_syncing_ = false;
    }
    if (page == Page::Installing && hwnd_) {
        timer_ = SetTimer(hwnd_, kProgressTimer, 80, nullptr);
    }
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SetupWindow::update_space() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SetupWindow::read_path_from_edit() {
    if (!edit_) return;
    const int length = GetWindowTextLengthW(edit_);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(edit_, text.data(), length + 1);
    if (copied < 0) return;
    text.resize(static_cast<std::size_t>(copied));
    install_dir_ = trim_copy(text);
    update_space();
}

bool SetupWindow::validate_path(bool show_error) {
    read_path_from_edit();
    if (!is_absolute_path(install_dir_)) {
        if (show_error) show_invalid_path();
        return false;
    }
    std::wstring normalized = normalize_path(install_dir_);
    std::wstring why;
    if (!is_safe_install_dir(normalized, why)) {
        if (show_error) show_invalid_path();
        return false;
    }
    install_dir_ = normalized;
    if (edit_) {
        edit_syncing_ = true;
        SetWindowTextW(edit_, install_dir_.c_str());
        edit_syncing_ = false;
    }
    return true;
}

void SetupWindow::show_invalid_path() {
    std::wstring why;
    is_safe_install_dir(normalize_path(install_dir_), why);
    std::wstring body = text_->invalid_dir_text;
    if (!why.empty()) body += L"\n\n" + why;
    MessageBoxW(hwnd_, body.c_str(), text_->invalid_dir_title,
                MB_OK | MB_ICONWARNING);
}

bool SetupWindow::ensure_aurora_stopped() {
    if (args_.update_mode && wait_for_aurora_exit(install_dir_, 30)) {
        return true;
    }
    for (;;) {
        const std::vector<std::wstring> processes =
            running_aurora_processes(install_dir_);
        if (processes.empty()) return true;
        std::wstring body = text_->running_text;
        body += L"\n\n";
        for (std::size_t i = 0; i < processes.size() && i < 8; ++i) {
            body += processes[i];
            body.push_back(L'\n');
        }
        if (processes.size() > 8) body += L"...\n";
        body += L"\n" + install_dir_;
        const int result = MessageBoxW(hwnd_, body.c_str(),
                                       text_->running_title,
                                       MB_RETRYCANCEL | MB_ICONWARNING);
        if (result != IDRETRY) return false;
        Sleep(300);
    }
}

void SetupWindow::start_install() {
    if (installing_) return;
    if (!archive_open_) {
        result_.ok = false;
        result_.error = archive_error_;
        result_.hr = HRESULT_FROM_WIN32(archive_error_);
        result_.detail = archive_detail_;
        set_page(Page::Failed);
        return;
    }
    const bool interactive = !(args_.update_mode || args_.elevated);
    if (!validate_path(interactive)) {
        if (interactive) {
            set_page(Page::Location);
            return;
        }
        result_.ok = false;
        result_.error = ERROR_BAD_PATHNAME;
        result_.hr = HRESULT_FROM_WIN32(result_.error);
        result_.detail = L"Invalid install folder.";
        set_page(Page::Failed);
        return;
    }
    if (!ensure_aurora_stopped()) {
        if (args_.update_mode || args_.elevated) {
            DestroyWindow(hwnd_);
        } else {
            set_page(Page::Components);
        }
        return;
    }

    options_.install_dir = install_dir_;
    options_.version = kSetupVersion;
    options_.language = language_;
    options_.desktop_shortcut = desktop_choice_;
    options_.start_menu_shortcut = start_menu_choice_;
    options_.update_mode = args_.update_mode;
    options_.relaunch = args_.relaunch;
    options_.elevated = args_.elevated;

    progress_.bytes_done.store(0, std::memory_order_relaxed);
    progress_.bytes_total.store(archive_.total_uncompressed(),
                                std::memory_order_relaxed);
    progress_.cancel.store(false, std::memory_order_relaxed);
    progress_.set_current(L"");
    result_ = InstallResult{};
    cancel_pending_ = false;
    close_when_done_ = false;
    installing_.store(true);
    set_page(Page::Installing);
    log_line(L"install start: " + install_dir_);

    worker_ = std::thread([this] {
        try {
            result_ = install_from_payload(archive_, options_, progress_);
        } catch (const std::bad_alloc&) {
            result_ = InstallResult{};
            result_.error = ERROR_NOT_ENOUGH_MEMORY;
            result_.hr = HRESULT_FROM_WIN32(result_.error);
            result_.detail = L"Out of memory while installing.";
        } catch (const std::exception&) {
            result_ = InstallResult{};
            result_.error = ERROR_INTERNAL_ERROR;
            result_.hr = HRESULT_FROM_WIN32(result_.error);
            result_.detail = L"An unexpected error stopped the installation.";
        } catch (...) {
            result_ = InstallResult{};
            result_.error = ERROR_INTERNAL_ERROR;
            result_.hr = HRESULT_FROM_WIN32(result_.error);
            result_.detail = L"An unexpected error stopped the installation.";
        }
        PostMessageW(hwnd_, kMessageInstallDone, 0, 0);
    });
}

void SetupWindow::on_install_done() {
    if (worker_.joinable()) worker_.join();
    if (timer_) {
        KillTimer(hwnd_, timer_);
        timer_ = 0;
    }
    const bool was_installing = installing_.exchange(false);
    (void)was_installing;
    if (close_when_done_) {
        DestroyWindow(hwnd_);
        return;
    }
    if (result_.canceled) {
        log_line(L"install canceled");
        if (args_.update_mode || args_.elevated) {
            DestroyWindow(hwnd_);
            return;
        }
        set_page(Page::Components);
        inline_message_ = text_->install_canceled;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (!result_.ok) {
        log_line(L"install failed: " + format_hresult(result_.hr) + L" " +
                 result_.detail + L" " + result_.failed_path);
        if (result_.error == ERROR_ACCESS_DENIED && !args_.elevated) {
            const int answer = MessageBoxW(
                hwnd_, text_->elevate_text, text_->elevate_title,
                MB_YESNO | MB_ICONWARNING);
            if (answer == IDYES) {
                maybe_elevate_and_retry();
                return;
            }
        }
        set_page(Page::Failed);
        return;
    }
    log_line(L"install complete: " + install_dir_);
    launch_choice_ = true;
    complete_desktop_choice_ = desktop_choice_;
    set_page(Page::Complete);
}

void SetupWindow::maybe_elevate_and_retry() {
    std::vector<std::wstring> args{
        L"--elevated", L"--dir", install_dir_,
        L"--lang", language_tag(language_),
        L"--components", components_argument()};
    if (args_.update_mode) args.push_back(L"--update");
    if (args_.relaunch) args.push_back(L"--relaunch");
    if (!log_path().empty()) {
        args.push_back(L"--log");
        args.push_back(log_path());
    }
    std::wstring detail;
    DWORD error = ERROR_SUCCESS;
    if (relaunch_elevated(args, detail, error)) {
        DestroyWindow(hwnd_);
        return;
    }
    log_line(L"elevation failed: " + format_win32_message(error) + L" " +
             detail);
    show_win32_error(text_->elevate_title, error, detail);
    set_page(Page::Failed);
}

void SetupWindow::finish_install() {
    if (installing_) return;
    if (complete_desktop_choice_) {
        std::wstring detail;
        HRESULT error = S_OK;
        if (!create_shortcut(true, install_dir_, detail, error)) {
            log_line(L"desktop shortcut failed: " + format_hresult(error) +
                     L" " + detail);
            const std::wstring body =
                format_hresult(error) + L"\n" + hresult_message(error) +
                (detail.empty() ? L"" : L"\n\n" + detail);
            MessageBoxW(hwnd_, body.c_str(), text_->shortcut_error_title,
                        MB_OK | MB_ICONWARNING);
        }
    }
    if (start_menu_choice_) {
        std::wstring detail;
        HRESULT error = S_OK;
        if (!create_shortcut(false, install_dir_, detail, error)) {
            log_line(L"start menu shortcut failed: " + format_hresult(error) +
                     L" " + detail);
            const std::wstring body =
                format_hresult(error) + L"\n" + hresult_message(error) +
                (detail.empty() ? L"" : L"\n\n" + detail);
            MessageBoxW(hwnd_, body.c_str(), text_->shortcut_error_title,
                        MB_OK | MB_ICONWARNING);
        }
    }
    if (launch_choice_) {
        launch_now(false);
    } else {
        set_page(Page::ThankYou);
    }
}

void SetupWindow::launch_now(bool final_page) {
    (void)final_page;
    const bool pass_language = !args_.update_mode;
    std::wstring detail;
    DWORD error = ERROR_SUCCESS;
    if (!launch_aurora(install_dir_, language_tag(language_), pass_language,
                       detail, error)) {
        log_line(L"launch failed: " + format_win32_message(error) + L" " +
                 detail);
        show_win32_error(text_->launch_error_title, error, detail);
        return;
    }
    log_line(L"launched Aurora");
    DestroyWindow(hwnd_);
}

bool SetupWindow::confirm_stop_install() {
    return MessageBoxW(hwnd_, text_->cancel_text, text_->cancel_title,
                       MB_YESNO | MB_ICONWARNING) == IDYES;
}

void SetupWindow::request_close() {
    if (installing_) {
        if (!confirm_stop_install()) return;
        close_when_done_ = true;
        progress_.cancel.store(true, std::memory_order_relaxed);
        cancel_pending_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    DestroyWindow(hwnd_);
}

void SetupWindow::show_win32_error(const wchar_t* title, DWORD error,
                                   const std::wstring& detail) {
    std::wstring body = format_hresult(HRESULT_FROM_WIN32(error)) + L"\n" +
                        format_win32_message(error);
    if (!detail.empty()) body += L"\n\n" + detail;
    MessageBoxW(hwnd_, body.c_str(), title, MB_OK | MB_ICONERROR);
}

void SetupWindow::open_language_dropdown() {
    if (page_ != Page::Language) return;
    language_open_ = true;
    language_hover_ = language_index(language_);
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SetupWindow::close_language_dropdown() {
    if (!language_open_) return;
    language_open_ = false;
    language_hover_ = -1;
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SetupWindow::choose_language(int index) {
    if (index < 0 || index >= language_count()) return;
    language_ = language_list()[index].id;
    text_ = &strings(language_);
    args_.language = language_;
    args_.has_language = true;
    language_open_ = false;
    language_hover_ = -1;
    update_window_title();
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SetupWindow::open_folder_dialog() {
    read_path_from_edit();
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(dialog.GetAddressOf()));
    if (FAILED(hr)) {
        show_win32_error(text_->invalid_dir_title, ERROR_GEN_FAILURE,
                         format_hresult(hr));
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                       FOS_PATHMUSTEXIST);
    dialog->SetTitle(text_->location_choose);
    if (directory_exists(install_dir_)) {
        Microsoft::WRL::ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                install_dir_.c_str(), nullptr,
                IID_PPV_ARGS(folder.GetAddressOf())))) {
            dialog->SetFolder(folder.Get());
        }
    }
    hr = dialog->Show(hwnd_);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    if (FAILED(hr)) {
        show_win32_error(text_->invalid_dir_title,
                         HRESULT_FACILITY(hr) == FACILITY_WIN32
                             ? static_cast<DWORD>(HRESULT_CODE(hr))
                             : ERROR_GEN_FAILURE,
                         format_hresult(hr));
        return;
    }
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.GetAddressOf())) || !item) return;
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        install_dir_ = normalize_path(path);
        CoTaskMemFree(path);
        if (edit_) {
            edit_syncing_ = true;
            SetWindowTextW(edit_, install_dir_.c_str());
            edit_syncing_ = false;
        }
        update_space();
    }
}

void SetupWindow::focus_next(bool backward) {
    if (focus_order_.empty()) return;
    auto it = std::find(focus_order_.begin(), focus_order_.end(), focus_);
    int index = it == focus_order_.end()
                    ? 0
                    : static_cast<int>(std::distance(focus_order_.begin(), it));
    const int count = static_cast<int>(focus_order_.size());
    index = (index + (backward ? -1 : 1) + count) % count;
    set_focus(focus_order_[static_cast<std::size_t>(index)]);
}

void SetupWindow::set_focus(HitId id) {
    focus_ = id;
    if (id == HitId::PathField && edit_) {
        SetFocus(edit_);
    } else if (edit_ && GetFocus() == edit_) {
        SetFocus(hwnd_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SetupWindow::activate(HitId id) {
    switch (id) {
        case HitId::TitleMin:
            ShowWindow(hwnd_, SW_MINIMIZE);
            return;
        case HitId::TitleMax:
            ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
            return;
        case HitId::TitleClose:
            request_close();
            return;
        case HitId::Next:
            if (page_ == Page::Language) {
                set_page(Page::Welcome);
            } else if (page_ == Page::Welcome) {
                if (edit_) {
                    edit_syncing_ = true;
                    SetWindowTextW(edit_, install_dir_.c_str());
                    edit_syncing_ = false;
                }
                set_page(Page::Location);
            } else if (page_ == Page::Location) {
                if (validate_path(true)) set_page(Page::Components);
            } else if (page_ == Page::Components) {
                start_install();
            }
            return;
        case HitId::Back:
            if (page_ == Page::Location) {
                set_page(Page::Welcome);
            } else if (page_ == Page::Components) {
                set_page(Page::Location);
            }
            return;
        case HitId::Cancel:
            if (page_ == Page::Installing) {
                if (confirm_stop_install()) {
                    progress_.cancel.store(true, std::memory_order_relaxed);
                    cancel_pending_ = true;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            } else {
                request_close();
            }
            return;
        case HitId::Finish:
            finish_install();
            return;
        case HitId::Close:
            request_close();
            return;
        case HitId::Launch:
            launch_now(true);
            return;
        case HitId::Browse:
            open_folder_dialog();
            return;
        case HitId::Combo:
            if (language_open_) {
                close_language_dropdown();
            } else {
                open_language_dropdown();
            }
            return;
        case HitId::PathField:
            set_focus(HitId::PathField);
            return;
        case HitId::CheckRequired:
            return;
        case HitId::CheckDesktop:
            desktop_choice_ = !desktop_choice_;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case HitId::CheckStart:
            start_menu_choice_ = !start_menu_choice_;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case HitId::CheckLaunch:
            launch_choice_ = !launch_choice_;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case HitId::CheckCompleteDesktop:
            complete_desktop_choice_ = !complete_desktop_choice_;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        case HitId::None:
            return;
        default:
            break;
    }
    if (id >= HitId::Lang0 && id <= HitId::Lang4) {
        const int index = static_cast<int>(id) - static_cast<int>(HitId::Lang0);
        if (index >= 0 && index < language_count()) choose_language(index);
        return;
    }
}

std::wstring SetupWindow::components_argument() const {
    std::wstring result;
    if (desktop_choice_) result = L"desktop";
    if (start_menu_choice_) {
        if (!result.empty()) result.push_back(L',');
        result += L"startmenu";
    }
    if (result.empty()) result = L"none";
    return result;
}

LRESULT SetupWindow::handle_message(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
    switch (message) {
        case WM_NCCALCSIZE:
            if (wparam) return 0;
            break;
        case WM_NCACTIVATE:
            return TRUE;
        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &point);
            RECT client{};
            GetClientRect(hwnd, &client);
            const int border = std::max(2, pixels(5.0f));
            const int corner = std::max(border, pixels(14.0f));
            const int x = point.x;
            const int y = point.y;
            const float dip_x = static_cast<float>(x) * 96.0f / dpi_;
            const float dip_y = static_cast<float>(y) * 96.0f / dpi_;
            for (HitId id : {HitId::TitleMin, HitId::TitleMax,
                             HitId::TitleClose}) {
                if (contains(hit_rect(id), dip_x, dip_y)) return HTCLIENT;
            }
            if (dip_y >= 0 && dip_y < title_height_) return HTCAPTION;
            if (IsZoomed(hwnd)) return HTCLIENT;
            const bool left = x < border;
            const bool right = x >= client.right - border;
            const bool top = y < border;
            const bool bottom = y >= client.bottom - border;
            const bool near_left = x < corner;
            const bool near_right = x >= client.right - corner;
            const bool near_top = y < corner;
            const bool near_bottom = y >= client.bottom - corner;
            if (near_top && near_left) return HTTOPLEFT;
            if (near_top && near_right) return HTTOPRIGHT;
            if (near_bottom && near_left) return HTBOTTOMLEFT;
            if (near_bottom && near_right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            return HTCLIENT;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                                &monitor)) {
                info->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
                info->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
                info->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
                info->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
                info->ptMinTrackSize.x =
                    std::min(pixels(760.0f),
                             std::max(500, static_cast<int>(
                                               monitor.rcWork.right -
                                               monitor.rcWork.left)));
                info->ptMinTrackSize.y =
                    std::min(pixels(470.0f),
                             std::max(400, static_cast<int>(
                                               monitor.rcWork.bottom -
                                               monitor.rcWork.top)));
            }
            return 0;
        }
        case WM_SIZE: {
            if (wparam == SIZE_MINIMIZED) return 0;
            if (render_target_) {
                RECT client{};
                GetClientRect(hwnd, &client);
                render_target_->Resize(D2D1::SizeU(
                    static_cast<UINT>(std::max(1L, client.right)),
                    static_cast<UINT>(std::max(1L, client.bottom))));
            }
            update_window_region();
            layout();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = static_cast<float>(HIWORD(wparam));
            if (dpi_ < 96.0f) dpi_ = 96.0f;
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (render_target_) {
                render_target_->SetDpi(dpi_, dpi_);
            }
            update_edit_font();
            update_window_region();
            layout();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint_struct{};
            BeginPaint(hwnd, &paint_struct);
            paint();
            EndPaint(hwnd, &paint_struct);
            return 0;
        }
        case WM_TIMER:
            if (wparam == kProgressTimer) {
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_MOUSEMOVE: {
            if (!tracking_leave_) {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                TrackMouseEvent(&track);
                tracking_leave_ = true;
            }
            const float x = static_cast<float>(GET_X_LPARAM(lparam)) * 96.0f / dpi_;
            const float y = static_cast<float>(GET_Y_LPARAM(lparam)) * 96.0f / dpi_;
            const HitId hovered = hit_test(x, y);
            int language_hover = -1;
            if (language_open_) {
                for (int i = 0; i < language_count() && i < 5; ++i) {
                    if (contains(hit_rect(static_cast<HitId>(
                                     static_cast<int>(HitId::Lang0) + i)),
                                 x, y)) {
                        language_hover = i;
                        break;
                    }
                }
            }
            if (hovered != hover_ || language_hover != language_hover_) {
                hover_ = hovered;
                language_hover_ = language_hover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_leave_ = false;
            hover_ = HitId::None;
            language_hover_ = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            const float x = static_cast<float>(GET_X_LPARAM(lparam)) * 96.0f / dpi_;
            const float y = static_cast<float>(GET_Y_LPARAM(lparam)) * 96.0f / dpi_;
            const HitId hit = hit_test(x, y);
            if (language_open_) {
                if (hit >= HitId::Lang0 && hit <= HitId::Lang4) {
                    activate(hit);
                    return 0;
                }
                close_language_dropdown();
                if (hit == HitId::Combo) return 0;
                // Let the click continue so title-bar and footer controls
                // still work while the dropdown closes.
            }
            if (hit == HitId::PathField) {
                set_focus(HitId::PathField);
                return 0;
            }
            if (hit != HitId::None) {
                focus_ = hit;
                pressed_ = hit;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (GetCapture() == hwnd) ReleaseCapture();
            const float x = static_cast<float>(GET_X_LPARAM(lparam)) * 96.0f / dpi_;
            const float y = static_cast<float>(GET_Y_LPARAM(lparam)) * 96.0f / dpi_;
            const HitId hit = hit_test(x, y);
            const HitId pressed = pressed_;
            pressed_ = HitId::None;
            if (pressed != HitId::None && hit == pressed) activate(pressed);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN: {
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (wparam == VK_TAB) {
                if (language_open_) close_language_dropdown();
                focus_next(shift);
                return 0;
            }
            if (wparam == VK_ESCAPE) {
                if (language_open_) {
                    close_language_dropdown();
                } else {
                    request_close();
                }
                return 0;
            }
            if (language_open_) {
                if (wparam == VK_UP) {
                    language_hover_ = (language_hover_ <= 0)
                                          ? language_count() - 1
                                          : language_hover_ - 1;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (wparam == VK_DOWN) {
                    language_hover_ = (language_hover_ + 1) % language_count();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (wparam == VK_RETURN) {
                    choose_language(language_hover_);
                    return 0;
                }
                return 0;
            }
            if (wparam == VK_RETURN) {
                if (focus_ == HitId::Combo) {
                    open_language_dropdown();
                } else if (focus_ == HitId::Back ||
                           focus_ == HitId::Cancel ||
                           focus_ == HitId::Browse ||
                           focus_ == HitId::Finish ||
                           focus_ == HitId::Close ||
                           focus_ == HitId::Launch) {
                    activate(focus_);
                } else if (page_ == Page::Language ||
                           page_ == Page::Welcome ||
                           page_ == Page::Location ||
                           page_ == Page::Components) {
                    activate(HitId::Next);
                } else if (page_ == Page::Complete) {
                    activate(HitId::Finish);
                } else if (page_ == Page::Failed) {
                    activate(HitId::Close);
                } else if (page_ == Page::ThankYou) {
                    activate(HitId::Launch);
                } else if (page_ == Page::Installing) {
                    activate(HitId::Cancel);
                }
                return 0;
            }
            if (wparam == VK_SPACE) {
                if (focus_ == HitId::Combo) {
                    open_language_dropdown();
                } else if (focus_ == HitId::CheckDesktop ||
                           focus_ == HitId::CheckStart ||
                           focus_ == HitId::CheckLaunch ||
                           focus_ == HitId::CheckCompleteDesktop) {
                    activate(focus_);
                } else if (focus_ != HitId::None) {
                    activate(focus_);
                }
                return 0;
            }
            if (wparam == VK_DOWN && focus_ == HitId::Combo) {
                open_language_dropdown();
                return 0;
            }
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == kEditId && edit_) {
                const WORD notification = HIWORD(wparam);
                if (notification == EN_CHANGE && !edit_syncing_) {
                    read_path_from_edit();
                } else if (notification == EN_SETFOCUS) {
                    focus_ = HitId::PathField;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            break;
        case WM_CTLCOLOREDIT:
            if (reinterpret_cast<HWND>(lparam) == edit_ && edit_brush_) {
                HDC dc = reinterpret_cast<HDC>(wparam);
                SetTextColor(dc, RGB(237, 244, 255));
                SetBkColor(dc, RGB(8, 16, 31));
                return reinterpret_cast<LRESULT>(edit_brush_);
            }
            break;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(hwnd, &point);
                const float x = static_cast<float>(point.x) * 96.0f / dpi_;
                const float y = static_cast<float>(point.y) * 96.0f / dpi_;
                const HitId hit = hit_test(x, y);
                if (hit == HitId::PathField) {
                    SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                } else if (hit != HitId::None) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                } else {
                    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                }
                return TRUE;
            }
            break;
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) pressed_ = HitId::None;
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_CLOSE:
            request_close();
            return 0;
        case WM_DESTROY:
            if (timer_) {
                KillTimer(hwnd, timer_);
                timer_ = 0;
            }
            PostQuitMessage(0);
            return 0;
        case kMessageStartInstall:
            if (!update_started_) {
                update_started_ = true;
                start_install();
            }
            return 0;
        case kMessageInstallDone:
            on_install_done();
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

// ---------------------------------------------------------------------------
// Drawing

Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> SetupWindow::make_brush(
    D2D1_COLOR_F color_value) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_out;
    if (render_target_) {
        render_target_->CreateSolidColorBrush(color_value,
                                              brush_out.GetAddressOf());
    }
    return brush_out;
}

void SetupWindow::paint() {
    if (!ensure_render_target()) return;
    render_target_->BeginDraw();
    render_target_->SetTransform(D2D1::Matrix3x2F::Identity());
    draw_background();
    draw_rail();
    draw_titlebar();

    switch (page_) {
        case Page::Language: draw_language_page(); break;
        case Page::Welcome: draw_welcome_page(); break;
        case Page::Location: draw_location_page(); break;
        case Page::Components: draw_components_page(); break;
        case Page::Installing: draw_installing_page(); break;
        case Page::Complete: draw_complete_page(); break;
        case Page::Failed: draw_failed_page(); break;
        case Page::ThankYou: draw_thankyou_page(); break;
    }
    draw_footer();
    draw_language_dropdown();
    draw_window_border();
    const HRESULT hr = render_target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        discard_render_target();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void SetupWindow::draw_text(const std::wstring& text,
                            IDWriteTextFormat* format, D2D1_RECT_F box,
                            D2D1_COLOR_F color_value,
                            DWRITE_TEXT_ALIGNMENT align,
                            DWRITE_PARAGRAPH_ALIGNMENT paragraph) {
    if (!render_target_ || !format || text.empty()) return;
    const float width = std::max(1.0f, rect_width(box));
    const float height = std::max(1.0f, rect_height(box));
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_factory_->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), format, width,
            height, layout.GetAddressOf()))) {
        return;
    }
    layout->SetTextAlignment(align);
    layout->SetParagraphAlignment(paragraph);
    auto brush_out = make_brush(color_value);
    if (brush_out) {
        render_target_->DrawTextLayout(D2D1::Point2F(box.left, box.top),
                                       layout.Get(), brush_out.Get(),
                                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

float SetupWindow::measure_text(const std::wstring& text,
                                IDWriteTextFormat* format) const {
    if (!dwrite_factory_ || !format || text.empty()) return 0.0f;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_factory_->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), format, 10000.0f,
            1000.0f, layout.GetAddressOf()))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return 0.0f;
    return metrics.widthIncludingTrailingWhitespace;
}

void SetupWindow::fill_rect(D2D1_RECT_F rect, D2D1_COLOR_F color_value) {
    auto brush_out = make_brush(color_value);
    if (brush_out) render_target_->FillRectangle(rect, brush_out.Get());
}

void SetupWindow::fill_round(D2D1_RECT_F rect, float radius,
                             D2D1_COLOR_F color_value) {
    auto brush_out = make_brush(color_value);
    if (brush_out) {
        render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(rect, radius, radius), brush_out.Get());
    }
}

void SetupWindow::stroke_round(D2D1_RECT_F rect, float radius,
                               D2D1_COLOR_F color_value, float width) {
    auto brush_out = make_brush(color_value);
    if (brush_out) {
        render_target_->DrawRoundedRectangle(
            D2D1::RoundedRect(rect, radius, radius), brush_out.Get(), width);
    }
}

void SetupWindow::fill_round_gradient(D2D1_RECT_F rect, float radius,
                                      D2D1_COLOR_F a, D2D1_COLOR_F b) {
    if (!render_target_) return;
    D2D1_GRADIENT_STOP stops[2] = {{0.0f, a}, {1.0f, b}};
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
    if (FAILED(render_target_->CreateGradientStopCollection(
            stops, 2, collection.GetAddressOf()))) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> gradient;
    if (FAILED(render_target_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(rect.left, rect.top),
                D2D1::Point2F(rect.right, rect.bottom)),
            collection.Get(), gradient.GetAddressOf()))) {
        return;
    }
    render_target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius),
                                         gradient.Get());
}

void SetupWindow::glow_round(D2D1_RECT_F rect, float radius,
                             D2D1_COLOR_F color_value) {
    for (int i = 3; i >= 1; --i) {
        D2D1_COLOR_F glow = color_value;
        glow.a *= 0.06f * static_cast<float>(i);
        stroke_round(inset(rect, -static_cast<float>(i) * 2.0f),
                     radius + static_cast<float>(i), glow,
                     2.0f);
    }
}

void SetupWindow::draw_line(D2D1_POINT_2F a, D2D1_POINT_2F b,
                            D2D1_COLOR_F color_value, float width) {
    auto brush_out = make_brush(color_value);
    if (brush_out) render_target_->DrawLine(a, b, brush_out.Get(), width);
}

void SetupWindow::draw_background() {
    const D2D1_SIZE_F size = render_target_->GetSize();
    const D2D1_RECT_F full = D2D1::RectF(0, 0, size.width, size.height);
    fill_rect(full, color(0x03050D));
    if (brand_background_) {
        const D2D1_SIZE_F image = brand_background_->GetSize();
        if (image.width > 0 && image.height > 0) {
            const float scale = std::max(size.width / image.width,
                                         size.height / image.height);
            const float source_width = size.width / scale;
            const float source_height = size.height / scale;
            const D2D1_RECT_F source = D2D1::RectF(
                (image.width - source_width) / 2.0f,
                (image.height - source_height) / 2.0f,
                (image.width + source_width) / 2.0f,
                (image.height + source_height) / 2.0f);
            render_target_->DrawBitmap(brand_background_.Get(), &full, 1.0f,
                                       D2D1_INTERPOLATION_MODE_HIGH_QUALITY_LINEAR,
                                       &source);
        }
    }
    const float rail_fraction = size.width > 0
                                    ? std::min(1.0f, rail_width_ / size.width)
                                    : 0.4f;
    D2D1_GRADIENT_STOP stops[3] = {
        {0.0f, color(0x000000, 0.08f)},
        {rail_fraction, color(0x000000, 0.50f)},
        {1.0f, color(0x000000, 0.94f)}};
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
    if (SUCCEEDED(render_target_->CreateGradientStopCollection(
            stops, 3, collection.GetAddressOf()))) {
        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> gradient;
        if (SUCCEEDED(render_target_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(0, 0), D2D1::Point2F(size.width, 0)),
                collection.Get(), gradient.GetAddressOf()))) {
            render_target_->FillRectangle(full, gradient.Get());
        }
    }
    fill_rect(D2D1::RectF(0, size.height - 90.0f, size.width, size.height),
              color(0x02040A, 0.30f));
}

void SetupWindow::draw_titlebar() {
    const float width = width_dip();
    fill_rect(D2D1::RectF(0, 0, width, title_height_), color(0x02040A, 0.55f));
    fill_rect(D2D1::RectF(0, title_height_ - 1.0f, width, title_height_),
              color(kLine, 0.75f));
    if (brand_mark_) {
        const D2D1_SIZE_F image = brand_mark_->GetSize();
        if (image.width > 0 && image.height > 0) {
            const float mark_size = 18.0f;
            const D2D1_RECT_F dest =
                D2D1::RectF(14.0f, (title_height_ - mark_size) / 2.0f,
                            14.0f + mark_size,
                            (title_height_ + mark_size) / 2.0f);
            render_target_->DrawBitmap(brand_mark_.Get(), &dest, 1.0f,
                                       D2D1_INTERPOLATION_MODE_HIGH_QUALITY_LINEAR);
        }
    }
    draw_text(text_->app_title, button_format_.Get(),
              D2D1::RectF(40.0f, 0, width - 180.0f, title_height_),
              color(kTextSoft), DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    const bool active = GetForegroundWindow() == hwnd_;
    const D2D1_COLOR_F glyph_color = color(active ? kTextSoft : kMuted);
    for (HitId id : {HitId::TitleMin, HitId::TitleMax, HitId::TitleClose}) {
        const D2D1_RECT_F rect = hit_rect(id);
        if (rect_width(rect) <= 0) continue;
        const bool hovered = hover_ == id;
        if (hovered) {
            fill_round(inset(rect, 4.0f), 5.0f,
                       id == HitId::TitleClose ? color(0xE81123, 0.95f)
                                               : color(0xFFFFFF, 0.08f));
        }
        const D2D1_COLOR_F current =
            hovered && id == HitId::TitleClose ? color(0xFFFFFF) : glyph_color;
        const float cx = (rect.left + rect.right) / 2.0f;
        const float cy = (rect.top + rect.bottom) / 2.0f;
        if (id == HitId::TitleMin) {
            draw_line(D2D1::Point2F(cx - 5, cy + 4),
                      D2D1::Point2F(cx + 5, cy + 4), current, 1.2f);
        } else if (id == HitId::TitleMax) {
            if (IsZoomed(hwnd_)) {
                stroke_round(D2D1::RectF(cx - 3, cy - 5, cx + 5, cy + 3), 1.0f,
                             current, 1.1f);
                stroke_round(D2D1::RectF(cx - 5, cy - 3, cx + 3, cy + 5), 1.0f,
                             current, 1.1f);
            } else {
                stroke_round(D2D1::RectF(cx - 5, cy - 5, cx + 5, cy + 5), 1.0f,
                             current, 1.1f);
            }
        } else {
            draw_line(D2D1::Point2F(cx - 4.5f, cy - 4.5f),
                      D2D1::Point2F(cx + 4.5f, cy + 4.5f), current, 1.2f);
            draw_line(D2D1::Point2F(cx + 4.5f, cy - 4.5f),
                      D2D1::Point2F(cx - 4.5f, cy + 4.5f), current, 1.2f);
        }
    }
}

void SetupWindow::draw_logo_block(float center_x, float top, float width,
                                  bool show_tagline) {
    float bottom = top;
    if (brand_logo_) {
        const D2D1_SIZE_F image = brand_logo_->GetSize();
        if (image.width > 0 && image.height > 0) {
            const float height = width * image.height / image.width;
            const D2D1_RECT_F dest =
                D2D1::RectF(center_x - width / 2.0f, top,
                            center_x + width / 2.0f, top + height);
            render_target_->DrawBitmap(brand_logo_.Get(), &dest, 1.0f,
                                       D2D1_INTERPOLATION_MODE_HIGH_QUALITY_LINEAR);
            bottom = top + height;
        }
    } else {
        draw_text(L"AURORA", title_format_.Get(),
                  D2D1::RectF(center_x - width / 2.0f, top, center_x + width / 2.0f,
                              top + 44.0f),
                  color(0xEDF4FF), DWRITE_TEXT_ALIGNMENT_CENTER);
        bottom = top + 44.0f;
    }
    if (show_tagline) {
        draw_text(text_->tagline, tagline_format_.Get(),
                  D2D1::RectF(center_x - width / 2.0f, bottom + 10.0f,
                              center_x + width / 2.0f, bottom + 34.0f),
                  color(kMuted), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void SetupWindow::draw_rail() {
    if (page_ == Page::ThankYou) return;
    const float height = height_dip();
    draw_line(D2D1::Point2F(rail_width_ - 0.5f, title_height_),
              D2D1::Point2F(rail_width_ - 0.5f, height), color(kLine, 0.55f),
              1.0f);
    draw_line(D2D1::Point2F(rail_width_ - 2.0f, title_height_),
              D2D1::Point2F(rail_width_ - 2.0f, height), color(kAccent, 0.05f),
              3.0f);
    const bool welcome = page_ == Page::Welcome;
    const float logo_width =
        welcome ? 190.0f : (rail_width_ < 260.0f ? 130.0f : 160.0f);
    const float logo_top = title_height_ + 78.0f;
    draw_logo_block(rail_width_ / 2.0f, logo_top, logo_width, welcome);
}

void SetupWindow::draw_button(HitId id, const wchar_t* label, bool primary,
                              bool chevron_right, bool chevron_left) {
    D2D1_RECT_F rect = hit_rect(id);
    if (rect_width(rect) <= 0) return;
    bool enabled = true;
    for (const Hit& hit : hits_) {
        if (hit.id == id) {
            enabled = hit.enabled;
            break;
        }
    }
    if (id == HitId::Cancel && cancel_pending_ && page_ == Page::Installing) {
        enabled = false;
    }
    const bool hovered = hover_ == id && enabled;
    const bool pressed = pressed_ == id && hovered;
    if (pressed) rect = offset(rect, 0, 1.0f);
    const float radius = 8.0f;

    if (primary) {
        if (enabled) glow_round(rect, radius, color(kAccent, 0.8f));
        if (!enabled) {
            fill_round(rect, radius, color(0x0A1222, 0.80f));
            stroke_round(rect, radius, color(0x1B2740, 0.9f), 1.0f);
        } else if (hovered) {
            fill_round_gradient(rect, radius, color(0x3C93FF),
                                color(0xB44CFF));
            stroke_round(rect, radius, color(0xB9F0FF, 0.55f), 1.0f);
        } else {
            fill_round_gradient(rect, radius, color(0x2F7BFF),
                                color(0x9B3DFF));
            stroke_round(rect, radius, color(0x7BD7FF, 0.35f), 1.0f);
        }
    } else {
        fill_round(rect, radius,
                   enabled && hovered ? color(0x12203A, 0.98f)
                                      : color(0x090F1D, 0.94f));
        stroke_round(rect, radius,
                     enabled && hovered ? color(0x46628C, 0.95f)
                                        : color(kLine, 0.95f),
                     1.0f);
    }

    D2D1_COLOR_F text_color = color(kText);
    if (!enabled) {
        text_color = color(kFaint);
    } else if (!primary) {
        text_color = color(kTextSoft);
    }
    D2D1_RECT_F text_box = rect;
    if (chevron_right) text_box.right -= 10.0f;
    if (chevron_left) text_box.left += 10.0f;
    draw_text(label, button_format_.Get(), text_box, text_color,
              DWRITE_TEXT_ALIGNMENT_CENTER,
              DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    const D2D1_COLOR_F chevron_color =
        enabled ? text_color : color(kFaint, 0.7f);
    if (chevron_right) {
        draw_chevron(D2D1::Point2F(rect.right - 17.0f,
                                   (rect.top + rect.bottom) / 2.0f),
                     5.0f, false, false, chevron_color);
    } else if (chevron_left) {
        draw_chevron(D2D1::Point2F(rect.left + 17.0f,
                                   (rect.top + rect.bottom) / 2.0f),
                     5.0f, false, true, chevron_color);
    }

    if (focus_ == id && enabled &&
        (GetFocus() == hwnd_ || GetFocus() == edit_)) {
        stroke_round(inset(rect, -2.0f), radius + 2.0f,
                     color(kAccent, 0.85f), 1.4f);
    }
}

void SetupWindow::draw_checkbox(HitId id, bool checked, const wchar_t* label,
                                bool enabled) {
    const D2D1_RECT_F row = hit_rect(id);
    if (rect_width(row) <= 0) return;
    const float box_size = 18.0f;
    const D2D1_RECT_F box =
        D2D1::RectF(row.left + 2.0f,
                    (row.top + row.bottom) / 2.0f - box_size / 2.0f,
                    row.left + 2.0f + box_size,
                    (row.top + row.bottom) / 2.0f + box_size / 2.0f);
    if (hover_ == id && enabled) {
        fill_round(D2D1::RectF(row.left, row.top, row.right, row.bottom), 6.0f,
                   color(0x0D182B, 0.55f));
    }
    if (checked) {
        if (enabled) {
            fill_round_gradient(box, 4.5f, color(0x23D8FF), color(0x8B45FF));
            stroke_round(box, 4.5f, color(0x9BEFFF, 0.55f), 1.0f);
        } else {
            fill_round(box, 4.5f, color(0x1E3A55, 0.9f));
            stroke_round(box, 4.5f, color(0x2B4A68, 0.9f), 1.0f);
        }
        draw_check_glyph(D2D1::Point2F((box.left + box.right) / 2.0f,
                                       (box.top + box.bottom) / 2.0f),
                         9.0f,
                         color(enabled ? 0xFFFFFF : 0x7A8BA3));
    } else {
        fill_round(box, 4.5f, color(0x08111F, 0.95f));
        stroke_round(box, 4.5f,
                     enabled ? color(0x3A5379) : color(0x263248), 1.2f);
    }
    draw_text(label, body_format_.Get(),
              D2D1::RectF(box.right + 12.0f, row.top, row.right, row.bottom),
              color(enabled ? kTextSoft : kFaint),
              DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (focus_ == id && enabled &&
        (GetFocus() == hwnd_ || GetFocus() == edit_) && !language_open_) {
        stroke_round(inset(box, -2.0f), 6.0f, color(kAccent, 0.85f), 1.4f);
    }
}

void SetupWindow::draw_chevron(D2D1_POINT_2F center, float size, bool down,
                               bool left, D2D1_COLOR_F color_value) {
    if (down) {
        draw_line(D2D1::Point2F(center.x - size, center.y - size * 0.4f),
                  D2D1::Point2F(center.x, center.y + size * 0.6f), color_value,
                  1.4f);
        draw_line(D2D1::Point2F(center.x, center.y + size * 0.6f),
                  D2D1::Point2F(center.x + size, center.y - size * 0.4f),
                  color_value, 1.4f);
    } else if (!left) {
        draw_line(D2D1::Point2F(center.x - size * 0.5f, center.y - size),
                  D2D1::Point2F(center.x + size * 0.5f, center.y), color_value,
                  1.4f);
        draw_line(D2D1::Point2F(center.x + size * 0.5f, center.y),
                  D2D1::Point2F(center.x - size * 0.5f, center.y + size),
                  color_value, 1.4f);
    } else {
        draw_line(D2D1::Point2F(center.x + size * 0.5f, center.y - size),
                  D2D1::Point2F(center.x - size * 0.5f, center.y), color_value,
                  1.4f);
        draw_line(D2D1::Point2F(center.x - size * 0.5f, center.y),
                  D2D1::Point2F(center.x + size * 0.5f, center.y + size),
                  color_value, 1.4f);
    }
}

void SetupWindow::draw_check_glyph(D2D1_POINT_2F center, float size,
                                   D2D1_COLOR_F color_value) {
    draw_line(D2D1::Point2F(center.x - size * 0.55f, center.y),
              D2D1::Point2F(center.x - size * 0.1f, center.y + size * 0.45f),
              color_value, 1.8f);
    draw_line(D2D1::Point2F(center.x - size * 0.1f, center.y + size * 0.45f),
              D2D1::Point2F(center.x + size * 0.6f, center.y - size * 0.5f),
              color_value, 1.8f);
}

void SetupWindow::draw_folder_icon(D2D1_RECT_F box) {
    const D2D1_COLOR_F tint = color(kAccent);
    fill_round(D2D1::RectF(box.left + 1, box.top + 5, box.left + 12,
                           box.top + 11),
               2.0f, color(kAccent, 0.22f));
    fill_round(D2D1::RectF(box.left + 1, box.top + 8, box.right - 1,
                           box.bottom - 2),
               3.0f, color(kAccent, 0.12f));
    stroke_round(D2D1::RectF(box.left + 1, box.top + 8, box.right - 1,
                             box.bottom - 2),
                 3.0f, tint, 1.4f);
}

void SetupWindow::draw_components_icon(D2D1_RECT_F box) {
    if (!render_target_ || !d2d_factory_) return;
    const float left = box.left + 2.0f;
    const float right = box.right - 2.0f;
    const float top = box.top + 6.0f;
    const float bottom = box.bottom - 2.0f;
    const float middle = (left + right) / 2.0f;
    const float radius = std::min((right - left) * 0.22f,
                                  (bottom - top) * 0.22f);
    if (radius <= 0.5f) return;
    const float notch_middle = (top + bottom) / 2.0f + 1.0f;

    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    if (FAILED(d2d_factory_->CreatePathGeometry(path.GetAddressOf()))) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(sink.GetAddressOf()))) return;
    sink->BeginFigure(D2D1::Point2F(left, top), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(middle - radius, top));
    sink->AddArc(D2D1::ArcSegment(
        D2D1::Point2F(middle + radius, top), D2D1::SizeF(radius, radius), 0.0f,
        D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddLine(D2D1::Point2F(right, top));
    sink->AddLine(D2D1::Point2F(right, notch_middle - radius));
    sink->AddArc(D2D1::ArcSegment(
        D2D1::Point2F(right, notch_middle + radius),
        D2D1::SizeF(radius, radius), 0.0f,
        D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddLine(D2D1::Point2F(right, bottom));
    sink->AddLine(D2D1::Point2F(left, bottom));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();

    auto fill = make_brush(color(kAccent, 0.16f));
    auto stroke = make_brush(color(kAccent));
    if (fill) render_target_->FillGeometry(path.Get(), fill.Get());
    if (stroke) render_target_->DrawGeometry(path.Get(), stroke.Get(), 1.4f);
}

void SetupWindow::draw_globe_icon(D2D1_RECT_F box) {
    const D2D1_COLOR_F tint = color(kMuted);
    const float cx = (box.left + box.right) / 2.0f;
    const float cy = (box.top + box.bottom) / 2.0f;
    const float radius = rect_width(box) / 2.0f - 2.0f;
    auto brush_out = make_brush(color(kMuted, 0.8f));
    if (!brush_out) return;
    render_target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius,
                                              radius),
                                brush_out.Get(), 1.2f);
    render_target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy),
                                              radius * 0.45f, radius),
                                brush_out.Get(), 1.0f);
    draw_line(D2D1::Point2F(cx - radius, cy), D2D1::Point2F(cx + radius, cy),
              tint, 1.0f);
    draw_line(D2D1::Point2F(cx - radius * 0.72f, cy - radius * 0.55f),
              D2D1::Point2F(cx + radius * 0.72f, cy - radius * 0.55f), tint,
              0.9f);
    draw_line(D2D1::Point2F(cx - radius * 0.72f, cy + radius * 0.55f),
              D2D1::Point2F(cx + radius * 0.72f, cy + radius * 0.55f), tint,
              0.9f);
}

void SetupWindow::draw_success_icon(D2D1_RECT_F box) {
    const float cx = (box.left + box.right) / 2.0f;
    const float cy = (box.top + box.bottom) / 2.0f;
    const float radius = rect_width(box) / 2.0f - 3.0f;
    glow_round(box, radius, color(kAccent, 0.6f));
    fill_round(box, radius, color(kAccent, 0.07f));
    auto brush_out = make_brush(color(kAccent));
    if (brush_out) {
        render_target_->DrawEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius),
            brush_out.Get(), 2.4f);
    }
    draw_check_glyph(D2D1::Point2F(cx, cy), radius * 0.72f, color(kAccent));
}

void SetupWindow::draw_warning_icon(D2D1_RECT_F box) {
    const float cx = (box.left + box.right) / 2.0f;
    const float cy = (box.top + box.bottom) / 2.0f;
    const float radius = rect_width(box) / 2.0f - 3.0f;
    glow_round(box, radius, color(kDanger, 0.65f));
    fill_round(box, radius, color(kDanger, 0.08f));
    auto brush_out = make_brush(color(kDanger));
    if (brush_out) {
        render_target_->DrawEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius),
            brush_out.Get(), 2.4f);
    }
    draw_line(D2D1::Point2F(cx, cy - radius * 0.45f),
              D2D1::Point2F(cx, cy + radius * 0.15f), color(kDanger), 2.6f);
    auto dot = make_brush(color(kDanger));
    if (dot) {
        render_target_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx, cy + radius * 0.48f), 1.8f, 1.8f),
            dot.Get());
    }
}

void SetupWindow::draw_language_page() {
    draw_text(text_->lang_title, page_title_format_.Get(),
              D2D1::RectF(content_.left, content_.top + 18.0f, content_.right,
                          content_.top + 54.0f),
              color(kText));
    draw_text(text_->lang_subtitle, body_format_.Get(),
              D2D1::RectF(content_.left, content_.top + 58.0f,
                          content_.right, content_.top + 92.0f),
              color(kMuted));

    const bool focused = focus_ == HitId::Combo;
    fill_round(combo_, 8.0f, color(kField, 0.97f));
    stroke_round(combo_, 8.0f,
                 focused || language_open_ ? color(kAccent, 0.7f)
                                           : color(kLine, 0.95f),
                 focused ? 1.5f : 1.0f);
    draw_globe_icon(
        D2D1::RectF(combo_.left + 13.0f, combo_.top + 11.0f,
                    combo_.left + 33.0f, combo_.bottom - 11.0f));
    draw_text(language_list()[language_index(language_)].native_name,
              body_format_.Get(),
              D2D1::RectF(combo_.left + 46.0f, combo_.top, combo_.right - 44.0f,
                          combo_.bottom),
              color(kTextSoft), DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    draw_chevron(D2D1::Point2F(combo_.right - 22.0f,
                               (combo_.top + combo_.bottom) / 2.0f),
                 5.0f, !language_open_, false, color(kMuted));
    if (focused && (GetFocus() == hwnd_ || GetFocus() == edit_)) {
        stroke_round(inset(combo_, -2.0f), 10.0f, color(kAccent, 0.75f), 1.2f);
    }
}

void SetupWindow::draw_welcome_page() {
    draw_text(text_->welcome_title, title_format_.Get(),
              D2D1::RectF(content_.left, content_.top + 4.0f,
                          content_.right - 20.0f, content_.top + 92.0f),
              color(kText));
    draw_text(text_->welcome_body, body_format_.Get(),
              D2D1::RectF(content_.left, content_.top + 104.0f,
                          content_.right - 30.0f, content_.top + 166.0f),
              color(kTextSoft));
    draw_text(text_->welcome_hint, body_format_.Get(),
              D2D1::RectF(content_.left, content_.top + 176.0f,
                          content_.right - 30.0f, content_.top + 208.0f),
              color(kMuted));
}

void SetupWindow::draw_location_page() {
    draw_folder_icon(D2D1::RectF(content_.left, content_.top + 20.0f,
                                 content_.left + 26.0f, content_.top + 46.0f));
    draw_text(text_->location_title, page_title_format_.Get(),
              D2D1::RectF(content_.left + 38.0f, content_.top + 14.0f,
                          content_.right, content_.top + 50.0f),
              color(kText));
    draw_text(text_->location_subtitle, body_format_.Get(),
              D2D1::RectF(content_.left + 38.0f, content_.top + 52.0f,
                          content_.right, content_.top + 82.0f),
              color(kMuted));

    const bool focused = focus_ == HitId::PathField || GetFocus() == edit_;
    fill_round(edit_field_, 8.0f, color(kField, 0.98f));
    stroke_round(edit_field_, 8.0f,
                 focused ? color(kAccent, 0.7f) : color(kLine, 0.95f),
                 focused ? 1.5f : 1.0f);
    if (focused) {
        stroke_round(inset(edit_field_, -2.0f), 10.0f, color(kAccent, 0.45f),
                     1.0f);
    }
    draw_button(HitId::Browse, text_->location_browse, false, false, false);

    const std::uint64_t required = archive_.total_uncompressed();
    const std::uint64_t available = free_bytes_for_path(install_dir_);
    const std::wstring required_line = format_string(
        text_->space_required, format_bytes(required));
    const std::wstring available_line =
        format_string(text_->space_available,
                      available == 0 ? L"\u2014" : format_bytes(available));
    draw_text(required_line, body_format_.Get(),
              D2D1::RectF(content_.left, edit_field_.bottom + 18.0f,
                          content_.right, edit_field_.bottom + 40.0f),
              color(kMuted));
    draw_text(available_line, body_format_.Get(),
              D2D1::RectF(content_.left, edit_field_.bottom + 42.0f,
                          content_.right, edit_field_.bottom + 64.0f),
              color(kFaint));
}

void SetupWindow::draw_components_page() {
    draw_components_icon(D2D1::RectF(content_.left, content_.top + 18.0f,
                                     content_.left + 26.0f,
                                     content_.top + 44.0f));
    draw_text(text_->components_title, page_title_format_.Get(),
              D2D1::RectF(content_.left + 38.0f, content_.top + 14.0f,
                          content_.right, content_.top + 50.0f),
              color(kText));
    draw_text(text_->components_subtitle, body_format_.Get(),
              D2D1::RectF(content_.left + 38.0f, content_.top + 52.0f,
                          content_.right, content_.top + 80.0f),
              color(kMuted));
    if (!inline_message_.empty()) {
        draw_text(inline_message_, small_format_.Get(),
                  D2D1::RectF(content_.left + 38.0f, content_.top + 82.0f,
                              content_.right, content_.top + 102.0f),
                  color(kAccent));
    }

    const D2D1_RECT_F required = hit_rect(HitId::CheckRequired);
    const D2D1_RECT_F start = hit_rect(HitId::CheckStart);
    if (rect_width(required) <= 0) return;
    const D2D1_RECT_F group = D2D1::RectF(required.left - 8.0f,
                                          required.top - 8.0f,
                                          required.right + 8.0f,
                                          start.bottom + 8.0f);
    fill_round(group, 10.0f, color(kPanel, 0.90f));
    stroke_round(group, 10.0f, color(kLine, 0.9f), 1.0f);
    draw_checkbox(HitId::CheckRequired, true, text_->components_required,
                  false);
    draw_checkbox(HitId::CheckDesktop, desktop_choice_,
                  text_->components_desktop, true);
    draw_checkbox(HitId::CheckStart, start_menu_choice_,
                  text_->components_start_menu, true);

    const float card_left = group.right + 16.0f;
    if (card_left < content_.right - 140.0f) {
        const D2D1_RECT_F card = D2D1::RectF(card_left, group.top,
                                             content_.right, group.bottom);
        fill_round(card, 10.0f, color(kPanel, 0.88f));
        stroke_round(card, 10.0f, color(kLine, 0.9f), 1.0f);
        const float mark_size = 78.0f;
        if (brand_mark_) {
            const D2D1_RECT_F mark =
                D2D1::RectF((card.left + card.right) / 2.0f - mark_size / 2.0f,
                            card.top + 18.0f,
                            (card.left + card.right) / 2.0f + mark_size / 2.0f,
                            card.top + 18.0f + mark_size);
            render_target_->DrawBitmap(brand_mark_.Get(), &mark, 1.0f,
                                       D2D1_INTERPOLATION_MODE_HIGH_QUALITY_LINEAR);
        }
        draw_text(text_->components_description, small_format_.Get(),
                  D2D1::RectF(card.left + 14.0f, card.top + 108.0f,
                              card.right - 14.0f, card.bottom - 12.0f),
                  color(kMuted), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void SetupWindow::draw_installing_page() {
    const bool updating = args_.update_mode;
    draw_text(updating ? text_->update_title : text_->installing_title,
              page_title_format_.Get(),
              D2D1::RectF(content_.left, content_.top + 18.0f, content_.right,
                          content_.top + 54.0f),
              color(kText));
    draw_text(updating ? text_->update_subtitle : text_->installing_subtitle,
              body_format_.Get(),
              D2D1::RectF(content_.left, content_.top + 58.0f, content_.right,
                          content_.top + 94.0f),
              color(kMuted));

    const std::uint64_t total = progress_.bytes_total.load();
    const std::uint64_t done = progress_.bytes_done.load();
    int percent = 0;
    if (total > 0) {
        const std::uint64_t value = std::min<std::uint64_t>(done, total);
        percent = static_cast<int>((value * 100ull) / total);
    }
    const D2D1_RECT_F track = progress_bar_;
    fill_round(track, 6.0f, color(0x0A1424, 0.98f));
    stroke_round(track, 6.0f, color(kLine, 0.9f), 1.0f);
    if (percent > 0) {
        D2D1_RECT_F fill = track;
        fill.right = fill.left + rect_width(track) *
                                     static_cast<float>(percent) / 100.0f;
        if (rect_width(fill) >= 2.0f) {
            fill_round_gradient(fill, 6.0f, color(0x23D8FF), color(0x9B3DFF));
        }
    }
    wchar_t percent_text[16]{};
    swprintf_s(percent_text, L"%d%%", percent);
    draw_text(percent_text, body_format_.Get(),
              D2D1::RectF(track.right + 12.0f, track.top - 6.0f,
                          content_.right, track.bottom + 8.0f),
              color(kTextSoft), DWRITE_TEXT_ALIGNMENT_TRAILING,
              DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    const std::wstring current = progress_.current();
    const bool waiting = total == 0 || current.empty();
    const wchar_t* status =
        waiting ? text_->installing_preparing : text_->installing_files;
    draw_text(status, small_format_.Get(),
              D2D1::RectF(content_.left, track.bottom + 18.0f, content_.right,
                          track.bottom + 38.0f),
              color(kMuted));
    if (!current.empty()) {
        const std::wstring path = join_path(install_dir_, current);
        const std::wstring shown =
            elide_left(path, small_format_.Get(), rect_width(content_));
        draw_text(shown, small_format_.Get(),
                  D2D1::RectF(content_.left, track.bottom + 40.0f,
                              content_.right, track.bottom + 60.0f),
                  color(kFaint));
    }
}

void SetupWindow::draw_complete_page() {
    const D2D1_RECT_F icon = D2D1::RectF(content_.left, content_.top + 20.0f,
                                         content_.left + 58.0f,
                                         content_.top + 78.0f);
    draw_success_icon(icon);
    draw_text(text_->complete_title, page_title_format_.Get(),
              D2D1::RectF(content_.left + 76.0f, content_.top + 20.0f,
                          content_.right, content_.top + 56.0f),
              color(kText));
    std::wstring subtitle = text_->complete_subtitle;
    if (args_.update_mode && !options_.version.empty()) {
        subtitle = format_string(text_->complete_updated, options_.version);
    }
    draw_text(subtitle, body_format_.Get(),
              D2D1::RectF(content_.left + 76.0f, content_.top + 60.0f,
                          content_.right, content_.top + 106.0f),
              color(kMuted));
    draw_checkbox(HitId::CheckLaunch, launch_choice_, text_->complete_launch,
                  true);
    draw_checkbox(HitId::CheckCompleteDesktop, complete_desktop_choice_,
                  text_->complete_shortcut, true);
}

void SetupWindow::draw_failed_page() {
    const D2D1_RECT_F icon = D2D1::RectF(content_.left, content_.top + 20.0f,
                                         content_.left + 58.0f,
                                         content_.top + 78.0f);
    draw_warning_icon(icon);
    draw_text(text_->failed_title, page_title_format_.Get(),
              D2D1::RectF(content_.left + 76.0f, content_.top + 20.0f,
                          content_.right, content_.top + 56.0f),
              color(kText));
    draw_text(text_->failed_subtitle, body_format_.Get(),
              D2D1::RectF(content_.left + 76.0f, content_.top + 60.0f,
                          content_.right, content_.top + 104.0f),
              color(kMuted));

    HRESULT hr = result_.hr;
    DWORD error = result_.error;
    if (hr == S_OK) {
        hr = error != ERROR_SUCCESS ? HRESULT_FROM_WIN32(error)
                                    : E_UNEXPECTED;
    }
    if (error == ERROR_SUCCESS) error = ERROR_INTERNAL_ERROR;
    const std::wstring code_line =
        format_string(text_->failed_code, format_hresult(hr));
    draw_text(code_line, code_format_.Get(),
              D2D1::RectF(content_.left, content_.top + 122.0f, content_.right,
                          content_.top + 144.0f),
              color(kText));
    draw_text(format_win32_message(error), body_format_.Get(),
              D2D1::RectF(content_.left, content_.top + 148.0f,
                          content_.right, content_.top + 188.0f),
              color(kTextSoft));
    if (!result_.failed_path.empty()) {
        const std::wstring path =
            elide_left(result_.failed_path, small_format_.Get(),
                       rect_width(content_));
        draw_text(path, small_format_.Get(),
                  D2D1::RectF(content_.left, content_.top + 192.0f,
                              content_.right, content_.top + 212.0f),
                  color(kFaint));
    }
    const std::wstring log_hint =
        format_string(text_->failed_log, log_path());
    draw_text(elide_left(log_hint, small_format_.Get(), rect_width(content_)),
              small_format_.Get(),
              D2D1::RectF(content_.left, content_.top + 218.0f, content_.right,
                          content_.top + 238.0f),
              color(kFaint));
}

void SetupWindow::draw_thankyou_page() {
    const float width = width_dip();
    const float height = height_dip();
    const float logo_width = std::min(230.0f, height * 0.38f);
    const float logo_top = std::max(title_height_ + 24.0f, height * 0.10f);
    draw_logo_block(width / 2.0f, logo_top, logo_width, true);
    const float text_top = logo_top + logo_width * 0.783f + 40.0f;
    draw_text(text_->thanks_title, title_format_.Get(),
              D2D1::RectF(40.0f, text_top, width - 40.0f, text_top + 40.0f),
              color(kText), DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(text_->thanks_subtitle, body_format_.Get(),
              D2D1::RectF(40.0f, text_top + 42.0f, width - 40.0f,
                          text_top + 70.0f),
              color(kMuted), DWRITE_TEXT_ALIGNMENT_CENTER);
}

void SetupWindow::draw_footer() {
    switch (page_) {
        case Page::Language:
            draw_button(HitId::Back, text_->btn_back, false, false, true);
            draw_button(HitId::Next, text_->btn_next, true, true, false);
            draw_button(HitId::Cancel, text_->btn_cancel, false, false, false);
            break;
        case Page::Welcome:
            draw_button(HitId::Next, text_->btn_next, true, true, false);
            draw_button(HitId::Cancel, text_->btn_cancel, false, false, false);
            break;
        case Page::Location:
        case Page::Components:
            draw_button(HitId::Back, text_->btn_back, false, false, true);
            draw_button(HitId::Next, text_->btn_next, true, true, false);
            draw_button(HitId::Cancel, text_->btn_cancel, false, false, false);
            break;
        case Page::Installing:
            draw_button(HitId::Cancel, text_->btn_cancel, false, false, false);
            break;
        case Page::Complete:
            draw_button(HitId::Finish, text_->btn_finish, true, false, false);
            break;
        case Page::Failed:
            draw_button(HitId::Close, text_->btn_close, false, false, false);
            break;
        case Page::ThankYou:
            draw_button(HitId::Launch, text_->btn_launch, true, false, false);
            break;
    }
}

void SetupWindow::draw_language_dropdown() {
    if (!language_open_ || page_ != Page::Language) return;
    if (rect_width(dropdown_) <= 0) return;
    glow_round(dropdown_, 10.0f, color(0x000000, 0.8f));
    fill_round(dropdown_, 10.0f, color(0x060C18, 0.99f));
    stroke_round(dropdown_, 10.0f, color(0x314A70, 0.95f), 1.0f);
    for (int i = 0; i < language_count() && i < 5; ++i) {
        const HitId id = static_cast<HitId>(static_cast<int>(HitId::Lang0) + i);
        const D2D1_RECT_F item = hit_rect(id);
        if (rect_width(item) <= 0) continue;
        const bool hovered = language_hover_ == i;
        const bool selected = language_list()[i].id == language_;
        if (hovered || selected) {
            fill_round(inset(item, 1.0f), 6.0f,
                       hovered ? color(0x172743, 0.95f)
                               : color(0x101C31, 0.95f));
        }
        draw_text(language_list()[i].native_name, body_format_.Get(),
                  D2D1::RectF(item.left + 14.0f, item.top,
                              item.right - 32.0f, item.bottom),
                  color(selected ? kText : kTextSoft),
                  DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (selected) {
            draw_check_glyph(
                D2D1::Point2F(item.right - 18.0f, (item.top + item.bottom) / 2.0f),
                8.0f, color(kAccent));
        }
    }
}

void SetupWindow::draw_window_border() {
    const D2D1_SIZE_F size = render_target_->GetSize();
    const D2D1_RECT_F border =
        D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, size.height - 0.5f);
    const float radius = IsZoomed(hwnd_) ? 0.0f : 11.0f;
    stroke_round(border, radius, color(kAccent, 0.28f), 1.0f);
    stroke_round(inset(border, 1.0f), radius, color(kAccent2, 0.10f), 1.0f);
}

std::wstring SetupWindow::elide_left(const std::wstring& text,
                                     IDWriteTextFormat* format,
                                     float max_width) {
    if (text.empty() || measure_text(text, format) <= max_width) return text;
    const std::wstring ellipsis = L"\u2026";
    for (std::size_t start = 1; start < text.size(); ++start) {
        const std::wstring candidate = ellipsis + text.substr(start);
        if (measure_text(candidate, format) <= max_width) return candidate;
    }
    return ellipsis;
}

std::wstring SetupWindow::elide_right(const std::wstring& text,
                                      IDWriteTextFormat* format,
                                      float max_width) {
    if (text.empty() || measure_text(text, format) <= max_width) return text;
    const std::wstring ellipsis = L"\u2026";
    for (std::size_t length = text.size(); length > 0; --length) {
        const std::wstring candidate = text.substr(0, length - 1) + ellipsis;
        if (measure_text(candidate, format) <= max_width) return candidate;
    }
    return ellipsis;
}

} // namespace aurora::setup
