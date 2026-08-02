#ifdef _WIN32
#include <foobar2000/SDK/foobar2000.h>

#include "waveform_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <windowsx.h>

namespace {

constexpr GUID kLoopFinderElementGuid = {
    0x938e8349,
    0x6599,
    0x4a7a,
    {0x86, 0xc4, 0x49, 0xb4, 0x80, 0x26, 0x0a, 0xa6}};

constexpr wchar_t kLoopFinderWindowClass[] =
    L"{7EFA898C-73E2-4C7F-AC87-21C7A5E87422}";
constexpr UINT_PTR kCursorTimer = 1;
constexpr UINT kCursorTimerMilliseconds = 50;

UINT window_dpi(HWND window) noexcept {
    using GetDpiForWindowFunction = UINT(WINAPI*)(HWND);
    static const auto get_dpi_for_window =
        reinterpret_cast<GetDpiForWindowFunction>(GetProcAddress(
            GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (get_dpi_for_window != nullptr) {
        const UINT dpi = get_dpi_for_window(window);
        if (dpi != 0) {
            return dpi;
        }
    }

    HDC dc = GetDC(window);
    if (dc == nullptr) {
        return USER_DEFAULT_SCREEN_DPI;
    }
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(window, dc);
    return dpi > 0 ? static_cast<UINT>(dpi) : USER_DEFAULT_SCREEN_DPI;
}

int scale_for_dpi(int value, UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

COLORREF blend_colors(COLORREF foreground, COLORREF background) noexcept {
    const auto blend_channel = [](BYTE front, BYTE back) noexcept -> BYTE {
        return static_cast<BYTE>((static_cast<unsigned>(front) * 2U +
                                  static_cast<unsigned>(back) * 3U) /
                                 5U);
    };

    return RGB(blend_channel(GetRValue(foreground), GetRValue(background)),
               blend_channel(GetGValue(foreground), GetGValue(background)),
               blend_channel(GetBValue(foreground), GetBValue(background)));
}

class LoopFinderPanel : public ui_element_instance,
                        private play_callback_impl_base {
public:
    LoopFinderPanel(ui_element_config::ptr configuration,
                    ui_element_instance_callback::ptr callback)
        : play_callback_impl_base(
              flag_on_playback_starting | flag_on_playback_new_track |
              flag_on_playback_stop | flag_on_playback_pause |
              flag_on_playback_seek | flag_on_playback_time |
              flag_on_playback_edited | flag_on_playback_dynamic_info_track),
          configuration_(std::move(configuration)),
          callback_(std::move(callback)),
          playback_(playback_control::get()),
          analysis_([this](std::uint64_t generation,
                           const std::string& identity,
                           loop_finder::foobar::WaveformSnapshotPtr snapshot,
                           const std::string& error) {
              on_analysis_complete(generation,
                                   identity,
                                   std::move(snapshot),
                                   error);
          }) {
        titleformat_compiler::get()->compile_safe(
            title_script_, "$if2(%title%,%filename%)");
    }

    ~LoopFinderPanel() {
        core_api::ensure_main_thread();
        analysis_.cancel();
        if (window_ != nullptr) {
            KillTimer(window_, kCursorTimer);
        }
        discard_waveform_layer();
    }

    void initialize_window(HWND parent) {
        core_api::ensure_main_thread();
        ensure_window_class();

        const HWND window = CreateWindowExW(
            0,
            kLoopFinderWindowClass,
            L"Loop Finder",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0,
            0,
            0,
            0,
            parent,
            nullptr,
            core_api::get_my_instance(),
            this);
        if (window == nullptr) {
            throw std::runtime_error("Could not create the Loop Finder UI element window.");
        }

        refresh_playback_snapshot();
        metadb_handle_ptr current;
        if (playback_->get_now_playing(current)) {
            begin_analysis(std::move(current));
        }
    }

    HWND get_wnd() override {
        return window_;
    }

    void set_configuration(ui_element_config::ptr configuration) override {
        core_api::ensure_main_thread();
        if (configuration.is_valid() &&
            configuration->get_guid() == kLoopFinderElementGuid) {
            configuration_ = std::move(configuration);
        }
    }

    ui_element_config::ptr get_configuration() override {
        return configuration_;
    }

    GUID get_guid() override {
        return kLoopFinderElementGuid;
    }

    GUID get_subclass() override {
        return ui_element_subclass_playback_information;
    }

    ui_element_min_max_info get_min_max_info() override {
        ui_element_min_max_info result;
        const UINT dpi = window_dpi(window_);
        result.m_min_width = static_cast<t_uint32>(scale_for_dpi(240, dpi));
        result.m_min_height = static_cast<t_uint32>(scale_for_dpi(180, dpi));
        result.adjustForWindow(window_);
        return result;
    }

    void notify(const GUID& what,
                t_size,
                const void*,
                t_size) override {
        core_api::ensure_main_thread();
        if (what == ui_element_notify_colors_changed ||
            what == ui_element_notify_font_changed) {
            redraw();
        }
    }

private:
    enum class PlaybackState {
        playing,
        paused,
        stopped,
    };

    enum class WaveformState {
        no_track,
        analyzing,
        available,
        unavailable,
    };

    static void ensure_window_class() {
        static const ATOM window_class = [] {
            WNDCLASSEXW definition{};
            definition.cbSize = sizeof(definition);
            definition.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
            definition.lpfnWndProc = &LoopFinderPanel::window_proc;
            definition.hInstance = core_api::get_my_instance();
            definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            definition.lpszClassName = kLoopFinderWindowClass;

            const ATOM result = RegisterClassExW(&definition);
            if (result == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                throw std::runtime_error(
                    "Could not register the Loop Finder UI element window class.");
            }
            return result == 0 ? static_cast<ATOM>(1) : result;
        }();
        (void)window_class;
    }

    static LRESULT CALLBACK window_proc(HWND window,
                                        UINT message,
                                        WPARAM wparam,
                                        LPARAM lparam) noexcept {
        LoopFinderPanel* panel = reinterpret_cast<LoopFinderPanel*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));

        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            panel = static_cast<LoopFinderPanel*>(create->lpCreateParams);
            panel->window_ = window;
            SetWindowLongPtrW(
                window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(panel));
        }

        if (panel == nullptr) {
            return DefWindowProcW(window, message, wparam, lparam);
        }

        switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            panel->paint();
            return 0;

        case WM_SIZE:
            panel->discard_waveform_layer();
            return 0;

        case WM_TIMER:
            if (wparam == kCursorTimer) {
                panel->update_playback_cursor();
                return 0;
            }
            break;

        case WM_MOUSEWHEEL:
            panel->on_mouse_wheel(wparam, lparam);
            return 0;

        case WM_LBUTTONDOWN:
            panel->on_left_button_down(GET_X_LPARAM(lparam),
                                       GET_Y_LPARAM(lparam));
            return 0;

        case WM_MOUSEMOVE:
            panel->on_mouse_move(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            return 0;

        case WM_LBUTTONUP:
            panel->on_left_button_up(GET_X_LPARAM(lparam),
                                     GET_Y_LPARAM(lparam));
            return 0;

        case WM_LBUTTONDBLCLK:
            panel->reset_view();
            return 0;

        case WM_CAPTURECHANGED:
            panel->mouse_down_ = false;
            panel->dragging_ = false;
            return 0;

        case WM_DPICHANGED:
        case 0x02E3: // WM_DPICHANGED_AFTERPARENT
            panel->discard_waveform_layer();
            panel->callback_->on_min_max_info_change();
            panel->redraw();
            return 0;

        case WM_NCDESTROY: {
            KillTimer(window, kCursorTimer);
            const LRESULT result =
                DefWindowProcW(window, message, wparam, lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            panel->window_ = nullptr;
            return result;
        }

        default:
            return DefWindowProcW(window, message, wparam, lparam);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void paint() noexcept {
        PAINTSTRUCT paint_state{};
        HDC dc = BeginPaint(window_, &paint_state);
        if (dc == nullptr) {
            return;
        }

        RECT bounds{};
        GetClientRect(window_, &bounds);
        const COLORREF background =
            callback_->query_std_color(ui_color_background);
        const COLORREF foreground = callback_->query_std_color(ui_color_text);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, foreground);

        HFONT font = callback_->query_font_ex(ui_font_default);
        if (font == nullptr) {
            font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        }
        const HGDIOBJ previous_font = SelectObject(dc, font);

        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        const UINT dpi = window_dpi(window_);
        const int margin = scale_for_dpi(12, dpi);
        const int line_gap = scale_for_dpi(5, dpi);
        const int line_height =
            (std::max)(scale_for_dpi(18, dpi),
                       static_cast<int>(metrics.tmHeight) + line_gap);
        const UINT text_flags =
            DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS;

        RECT line{bounds.left + margin,
                  bounds.top + margin,
                  (std::max)(bounds.left + margin, bounds.right - margin),
                  bounds.bottom};
        RECT waveform{line.left,
                      line.top + line_height * 4 + line_gap,
                      (std::max)(line.left, bounds.right - margin),
                      (std::max)(line.top + line_height * 4 + line_gap,
                                 bounds.bottom - margin)};
        const bool waveform_only_paint =
            paint_state.rcPaint.left >= waveform.left &&
            paint_state.rcPaint.top >= waveform.top &&
            paint_state.rcPaint.right <= waveform.right &&
            paint_state.rcPaint.bottom <= waveform.bottom;

        HFONT heading_font = nullptr;
        if (!waveform_only_paint) {
            HBRUSH background_brush = CreateSolidBrush(background);
            if (background_brush != nullptr) {
                FillRect(dc, &bounds, background_brush);
                DeleteObject(background_brush);
            }

            LOGFONTW font_description{};
            if (GetObjectW(font, sizeof(font_description), &font_description) !=
                0) {
                font_description.lfWeight =
                    std::max<LONG>(font_description.lfWeight, FW_SEMIBOLD);
                heading_font = CreateFontIndirectW(&font_description);
            }
            if (heading_font != nullptr) {
                SelectObject(dc, heading_font);
            }
            line.bottom = line.top + line_height;
            DrawTextW(dc, L"Loop Finder", -1, &line, text_flags);

            SelectObject(dc, font);
            line.top += line_height;
            line.bottom = line.top + line_height;
            const pfc::stringcvt::string_wide_from_utf8 wide_title(track_title_);
            const std::wstring track_line =
                std::wstring(L"Track: ") + wide_title.get_ptr();
            DrawTextW(dc, track_line.c_str(), -1, &line, text_flags);

            line.top += line_height;
            line.bottom = line.top + line_height;
            const std::wstring playback_line =
                std::wstring(L"Playback: ") + playback_state_text();
            DrawTextW(dc, playback_line.c_str(), -1, &line, text_flags);

            line.top += line_height;
            line.bottom = line.top + line_height;
            SetTextColor(dc, blend_colors(foreground, background));
            DrawTextW(dc, L"Loop: Off", -1, &line, text_flags);
        }

        paint_waveform(dc,
                       waveform,
                       foreground,
                       background,
                       font,
                       line_height);

        if (heading_font != nullptr) {
            DeleteObject(heading_font);
        }
        if (previous_font != nullptr && previous_font != HGDI_ERROR) {
            SelectObject(dc, previous_font);
        }
        EndPaint(window_, &paint_state);
    }

    void paint_waveform(HDC dc,
                        const RECT& bounds,
                        COLORREF foreground,
                        COLORREF background,
                        HFONT font,
                        int line_height) noexcept {
        if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
            return;
        }

        if (ensure_waveform_layer(
                dc, bounds, foreground, background, font, line_height)) {
            HDC memory_dc = CreateCompatibleDC(dc);
            if (memory_dc != nullptr) {
                const HGDIOBJ previous =
                    SelectObject(memory_dc, waveform_layer_bitmap_);
                if (previous != nullptr && previous != HGDI_ERROR) {
                    BitBlt(dc,
                           bounds.left,
                           bounds.top,
                           bounds.right - bounds.left,
                           bounds.bottom - bounds.top,
                           memory_dc,
                           0,
                           0,
                           SRCCOPY);
                    SelectObject(memory_dc, previous);
                }
                DeleteDC(memory_dc);
            }
        } else {
            draw_waveform_layer(
                dc, bounds, foreground, background, font, line_height);
        }

        draw_cursor(dc, waveform_graph_bounds(bounds, line_height));
    }

    bool ensure_waveform_layer(HDC target,
                               const RECT& bounds,
                               COLORREF foreground,
                               COLORREF background,
                               HFONT font,
                               int line_height) noexcept {
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        if (!waveform_layer_dirty_ && waveform_layer_bitmap_ != nullptr &&
            waveform_layer_width_ == width && waveform_layer_height_ == height) {
            return true;
        }

        discard_waveform_layer();
        HDC memory_dc = CreateCompatibleDC(target);
        if (memory_dc == nullptr) {
            return false;
        }
        HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
        if (bitmap == nullptr) {
            DeleteDC(memory_dc);
            return false;
        }

        const HGDIOBJ previous = SelectObject(memory_dc, bitmap);
        if (previous == nullptr || previous == HGDI_ERROR) {
            DeleteObject(bitmap);
            DeleteDC(memory_dc);
            return false;
        }

        const RECT local_bounds{0, 0, width, height};
        draw_waveform_layer(memory_dc,
                            local_bounds,
                            foreground,
                            background,
                            font,
                            line_height);
        SelectObject(memory_dc, previous);
        DeleteDC(memory_dc);

        waveform_layer_bitmap_ = bitmap;
        waveform_layer_width_ = width;
        waveform_layer_height_ = height;
        waveform_layer_dirty_ = false;
        return true;
    }

    void discard_waveform_layer() noexcept {
        if (waveform_layer_bitmap_ != nullptr) {
            DeleteObject(waveform_layer_bitmap_);
            waveform_layer_bitmap_ = nullptr;
        }
        waveform_layer_width_ = 0;
        waveform_layer_height_ = 0;
        waveform_layer_dirty_ = true;
    }

    void draw_waveform_layer(HDC dc,
                             const RECT& bounds,
                             COLORREF foreground,
                             COLORREF background,
                             HFONT font,
                             int line_height) noexcept {
        if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
            return;
        }

        HBRUSH background_brush = CreateSolidBrush(background);
        if (background_brush != nullptr) {
            FillRect(dc, &bounds, background_brush);
            DeleteObject(background_brush);
        }
        SetBkMode(dc, TRANSPARENT);

        const COLORREF muted = blend_colors(foreground, background);
        HPEN border_pen = CreatePen(PS_SOLID, 1, muted);
        const HGDIOBJ previous_pen =
            border_pen != nullptr ? SelectObject(dc, border_pen) : nullptr;
        const HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
        if (previous_brush != nullptr && previous_brush != HGDI_ERROR) {
            SelectObject(dc, previous_brush);
        }
        if (previous_pen != nullptr && previous_pen != HGDI_ERROR) {
            SelectObject(dc, previous_pen);
        }
        if (border_pen != nullptr) {
            DeleteObject(border_pen);
        }

        RECT status_bounds{bounds.left + 5,
                           bounds.top + 3,
                           bounds.right - 5,
                           (std::min)(bounds.bottom, bounds.top + line_height)};
        SetTextColor(dc, muted);
        SelectObject(dc, font);
        const std::wstring status = waveform_status_text();
        DrawTextW(dc,
                  status.c_str(),
                  -1,
                  &status_bounds,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX |
                      DT_END_ELLIPSIS);

        if (waveform_state_ != WaveformState::available || !waveform_) {
            return;
        }

        const RECT graph = waveform_graph_bounds(bounds, line_height);
        if (graph.right <= graph.left || graph.bottom <= graph.top) {
            return;
        }

        try {
            const auto display = loop_finder::resample_waveform(
                waveform_->bins,
                view_begin_,
                view_end_,
                static_cast<std::size_t>(graph.right - graph.left));
            const int center = graph.top + (graph.bottom - graph.top) / 2;
            const double amplitude =
                static_cast<double>((std::max)(
                    1, static_cast<int>((graph.bottom - graph.top) / 2) - 1));

            HPEN waveform_pen = CreatePen(PS_SOLID, 1, foreground);
            const HGDIOBJ old_waveform_pen =
                waveform_pen != nullptr ? SelectObject(dc, waveform_pen) : nullptr;
            for (std::size_t index = 0; index < display.size(); ++index) {
                const auto& bin = display[index];
                const int x = graph.left + static_cast<int>(index);
                const int top = center - static_cast<int>(
                                             std::clamp(bin.maximum, -1.0F, 1.0F) *
                                             amplitude);
                const int bottom = center - static_cast<int>(
                                                std::clamp(bin.minimum, -1.0F, 1.0F) *
                                                amplitude);
                MoveToEx(dc, x, top, nullptr);
                LineTo(dc, x, (std::max)(top + 1, bottom));
            }
            if (old_waveform_pen != nullptr && old_waveform_pen != HGDI_ERROR) {
                SelectObject(dc, old_waveform_pen);
            }
            if (waveform_pen != nullptr) {
                DeleteObject(waveform_pen);
            }

        } catch (...) {
            // Painting must never destabilize the host. Analysis data remains
            // intact and the next invalidation can retry at a new panel size.
        }

        RECT help{bounds.left + 5,
                  (std::max)(graph.bottom, bounds.bottom - line_height),
                  bounds.right - 5,
                  bounds.bottom - 2};
        SetTextColor(dc, muted);
        DrawTextW(dc,
                  L"Wheel: zoom  Drag: pan  Click: seek  Double-click: overview",
                  -1,
                  &help,
                  DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX |
                      DT_END_ELLIPSIS);
    }

    static RECT waveform_graph_bounds(const RECT& bounds,
                                      int line_height) noexcept {
        return RECT{bounds.left + 2,
                    (std::min)(bounds.bottom, bounds.top + line_height),
                    bounds.right - 2,
                    bounds.bottom - line_height};
    }

    void draw_cursor(HDC dc, const RECT& graph) noexcept {
        const auto x = cursor_x(playback_position_, graph);
        if (!x.has_value()) {
            return;
        }
        const COLORREF highlight =
            callback_->query_std_color(ui_color_highlight);
        HPEN cursor_pen = CreatePen(PS_SOLID, 1, highlight);
        const HGDIOBJ previous =
            cursor_pen != nullptr ? SelectObject(dc, cursor_pen) : nullptr;
        MoveToEx(dc, *x, graph.top, nullptr);
        LineTo(dc, *x, graph.bottom);
        if (previous != nullptr && previous != HGDI_ERROR) {
            SelectObject(dc, previous);
        }
        if (cursor_pen != nullptr) {
            DeleteObject(cursor_pen);
        }
    }

    std::optional<int> cursor_x(double playback_position,
                                const RECT& graph) const noexcept {
        if (!waveform_ || waveform_->duration_seconds <= 0.0 ||
            graph.right <= graph.left || graph.bottom <= graph.top) {
            return std::nullopt;
        }
        const double position = playback_position / waveform_->duration_seconds;
        if (position < view_begin_ || position > view_end_) {
            return std::nullopt;
        }
        const double relative =
            (position - view_begin_) / (view_end_ - view_begin_);
        return graph.left + static_cast<int>(
                                relative * (graph.right - graph.left - 1));
    }

    std::wstring waveform_status_text() const {
        switch (waveform_state_) {
        case WaveformState::no_track:
            return L"Waveform: No track";
        case WaveformState::analyzing:
            return L"Waveform: Analyzing...";
        case WaveformState::available:
            return L"Waveform available";
        case WaveformState::unavailable: {
            const pfc::stringcvt::string_wide_from_utf8 wide_error(
                analysis_error_.c_str());
            return std::wstring(L"Waveform: Analysis unavailable") +
                   (analysis_error_.empty()
                        ? L""
                        : std::wstring(L" - ") + wide_error.get_ptr());
        }
        }
        return L"Waveform: Analysis unavailable";
    }

    const wchar_t* playback_state_text() const noexcept {
        switch (playback_state_) {
        case PlaybackState::playing:
            return L"Playing";
        case PlaybackState::paused:
            return L"Paused";
        case PlaybackState::stopped:
            return L"Stopped";
        }
        return L"Stopped";
    }

    int waveform_line_height() const noexcept {
        const UINT dpi = window_dpi(window_);
        const int line_gap = scale_for_dpi(5, dpi);
        int line_height = scale_for_dpi(18, dpi);
        HDC dc = GetDC(window_);
        if (dc != nullptr) {
            HFONT font = callback_->query_font_ex(ui_font_default);
            if (font == nullptr) {
                font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            }
            const HGDIOBJ previous = SelectObject(dc, font);
            TEXTMETRICW metrics{};
            if (GetTextMetricsW(dc, &metrics)) {
                line_height = (std::max)(
                    line_height, static_cast<int>(metrics.tmHeight) + line_gap);
            }
            if (previous != nullptr && previous != HGDI_ERROR) {
                SelectObject(dc, previous);
            }
            ReleaseDC(window_, dc);
        }
        return line_height;
    }

    RECT waveform_bounds() const noexcept {
        RECT bounds{};
        if (window_ == nullptr || !GetClientRect(window_, &bounds)) {
            return bounds;
        }

        const UINT dpi = window_dpi(window_);
        const int margin = scale_for_dpi(12, dpi);
        const int line_gap = scale_for_dpi(5, dpi);
        const int line_height = waveform_line_height();

        return RECT{bounds.left + margin,
                    bounds.top + margin + line_height * 4 + line_gap,
                    (std::max)(bounds.left + margin, bounds.right - margin),
                    (std::max)(bounds.top + margin + line_height * 4 + line_gap,
                               bounds.bottom - margin)};
    }

    static bool contains(const RECT& bounds, int x, int y) noexcept {
        return x >= bounds.left && x < bounds.right && y >= bounds.top &&
               y < bounds.bottom;
    }

    void begin_analysis(metadb_handle_ptr track) {
        core_api::ensure_main_thread();
        ++analysis_generation_;
        waveform_.reset();
        analysis_error_.clear();
        reset_view_values();

        try {
            current_identity_ =
                loop_finder::foobar::make_track_identity(track);
            if (current_identity_.empty()) {
                throw std::runtime_error("Track identity is unavailable");
            }
            if (auto cached = waveform_cache_.find(current_identity_)) {
                analysis_.cancel();
                waveform_ = std::move(cached);
                waveform_state_ = WaveformState::available;
                sync_cursor_timer();
                redraw();
                return;
            }

            waveform_state_ = WaveformState::analyzing;
            analysis_.request(
                analysis_generation_, current_identity_, std::move(track));
        } catch (const std::exception& exception) {
            analysis_.cancel();
            waveform_state_ = WaveformState::unavailable;
            analysis_error_ = exception.what();
        }
        sync_cursor_timer();
        redraw();
    }

    void clear_analysis() {
        core_api::ensure_main_thread();
        ++analysis_generation_;
        current_identity_.clear();
        waveform_.reset();
        waveform_state_ = WaveformState::no_track;
        analysis_error_.clear();
        reset_view_values();
        analysis_.cancel();
        sync_cursor_timer();
    }

    void on_analysis_complete(
        std::uint64_t generation,
        const std::string& identity,
        loop_finder::foobar::WaveformSnapshotPtr snapshot,
        const std::string& error) {
        core_api::ensure_main_thread();
        if (generation != analysis_generation_ || identity != current_identity_) {
            return;
        }

        if (snapshot) {
            waveform_cache_.store(identity, snapshot);
            waveform_ = std::move(snapshot);
            waveform_state_ = WaveformState::available;
            analysis_error_.clear();
        } else {
            waveform_.reset();
            waveform_state_ = WaveformState::unavailable;
            analysis_error_ = error.empty() ? "Decoder returned no waveform" : error;
        }
        sync_cursor_timer();
        redraw();
    }

    void reset_view_values() noexcept {
        view_begin_ = 0.0;
        view_end_ = 1.0;
    }

    void reset_view() noexcept {
        if (waveform_state_ != WaveformState::available) {
            return;
        }
        reset_view_values();
        redraw_waveform();
    }

    void on_mouse_wheel(WPARAM wparam, LPARAM lparam) noexcept {
        if (waveform_state_ != WaveformState::available || !waveform_) {
            return;
        }
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window_, &point);
        const RECT bounds = waveform_bounds();
        if (!contains(bounds, point.x, point.y) || bounds.right <= bounds.left) {
            return;
        }

        const int wheel_delta = GET_WHEEL_DELTA_WPARAM(wparam);
        if (wheel_delta == 0) {
            return;
        }
        const double steps =
            static_cast<double>(wheel_delta) / static_cast<double>(WHEEL_DELTA);
        const double current_width = view_end_ - view_begin_;
        const double new_width = std::clamp(
            current_width * std::pow(0.8, steps), 1.0 / 64.0, 1.0);
        const double anchor = std::clamp(
            static_cast<double>(point.x - bounds.left) /
                static_cast<double>(bounds.right - bounds.left),
            0.0,
            1.0);
        const double anchor_position = view_begin_ + anchor * current_width;
        view_begin_ =
            std::clamp(anchor_position - anchor * new_width, 0.0, 1.0 - new_width);
        view_end_ = view_begin_ + new_width;
        redraw_waveform();
    }

    void on_left_button_down(int x, int y) noexcept {
        const RECT bounds = waveform_bounds();
        if (waveform_state_ != WaveformState::available || !waveform_ ||
            !contains(bounds, x, y)) {
            return;
        }
        SetCapture(window_);
        mouse_down_ = true;
        dragging_ = false;
        mouse_down_x_ = x;
        drag_view_begin_ = view_begin_;
        drag_view_end_ = view_end_;
    }

    void on_mouse_move(int x, int) noexcept {
        if (!mouse_down_ || GetCapture() != window_) {
            return;
        }
        const RECT bounds = waveform_bounds();
        const int width = bounds.right - bounds.left;
        if (width <= 0) {
            return;
        }
        const int distance = x - mouse_down_x_;
        if (!dragging_ &&
            std::abs(distance) >= scale_for_dpi(3, window_dpi(window_))) {
            dragging_ = true;
        }
        if (!dragging_) {
            return;
        }

        const double view_width = drag_view_end_ - drag_view_begin_;
        view_begin_ = std::clamp(
            drag_view_begin_ - static_cast<double>(distance) /
                                   static_cast<double>(width) * view_width,
            0.0,
            1.0 - view_width);
        view_end_ = view_begin_ + view_width;
        redraw_waveform();
    }

    void on_left_button_up(int x, int y) noexcept {
        if (!mouse_down_) {
            return;
        }
        const bool was_dragging = dragging_;
        mouse_down_ = false;
        dragging_ = false;
        if (GetCapture() == window_) {
            ReleaseCapture();
        }
        if (!was_dragging) {
            seek_at(x, y);
        }
    }

    void seek_at(int x, int y) noexcept {
        const RECT bounds = waveform_bounds();
        if (!waveform_ || waveform_->duration_seconds <= 0.0 ||
            !contains(bounds, x, y) || bounds.right <= bounds.left ||
            !playback_->is_playing() || !playback_->playback_can_seek()) {
            return;
        }
        const double relative = std::clamp(
            static_cast<double>(x - bounds.left) /
                static_cast<double>(bounds.right - bounds.left),
            0.0,
            1.0);
        const double normalized =
            view_begin_ + relative * (view_end_ - view_begin_);
        playback_->playback_seek(normalized * waveform_->duration_seconds);
    }

    void update_playback_cursor() noexcept {
        core_api::ensure_main_thread();
        if (playback_state_ != PlaybackState::playing ||
            !playback_->is_playing()) {
            sync_cursor_timer();
            return;
        }
        set_playback_position(playback_->playback_get_position());
    }

    void set_playback_position(double position) noexcept {
        const double previous_position = playback_position_;
        playback_position_ = position;
        if (window_ == nullptr || waveform_state_ != WaveformState::available) {
            return;
        }

        const RECT bounds = waveform_bounds();
        const RECT graph =
            waveform_graph_bounds(bounds, waveform_line_height());
        const auto previous_x = cursor_x(previous_position, graph);
        const auto current_x = cursor_x(playback_position_, graph);
        if (previous_x == current_x) {
            return;
        }

        const int cursor_padding =
            (std::max)(1, scale_for_dpi(2, window_dpi(window_)));
        const auto invalidate_cursor = [&](int x) noexcept {
            RECT cursor_bounds{(std::max)(static_cast<int>(graph.left),
                                          x - cursor_padding),
                               graph.top,
                               (std::min)(static_cast<int>(graph.right),
                                          x + cursor_padding + 1),
                               graph.bottom};
            InvalidateRect(window_, &cursor_bounds, FALSE);
        };
        if (previous_x.has_value()) {
            invalidate_cursor(*previous_x);
        }
        if (current_x.has_value()) {
            invalidate_cursor(*current_x);
        }
    }

    void sync_cursor_timer() noexcept {
        if (window_ == nullptr) {
            return;
        }
        if (playback_state_ == PlaybackState::playing &&
            waveform_state_ == WaveformState::available) {
            SetTimer(window_,
                     kCursorTimer,
                     kCursorTimerMilliseconds,
                     nullptr);
        } else {
            KillTimer(window_, kCursorTimer);
        }
    }

    void refresh_playback_snapshot() {
        core_api::ensure_main_thread();

        if (!playback_->is_playing()) {
            playback_state_ = PlaybackState::stopped;
            track_title_ = "No track";
            redraw();
            return;
        }

        playback_state_ = playback_->is_paused() ? PlaybackState::paused
                                                 : PlaybackState::playing;
        playback_position_ = playback_->playback_get_position();
        pfc::string8 title;
        if (playback_->playback_format_title(nullptr,
                                             title,
                                             title_script_,
                                             nullptr,
                                             playback_control::display_level_titles) &&
            !title.is_empty()) {
            track_title_ = title;
        } else {
            track_title_ = "Opening...";
        }
        sync_cursor_timer();
        redraw();
    }

    void redraw() noexcept {
        if (window_ != nullptr) {
            waveform_layer_dirty_ = true;
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void redraw_waveform() noexcept {
        if (window_ != nullptr) {
            waveform_layer_dirty_ = true;
            const RECT bounds = waveform_bounds();
            InvalidateRect(window_, &bounds, FALSE);
        }
    }

    void on_playback_starting(play_control::t_track_command, bool paused) override {
        core_api::ensure_main_thread();
        playback_state_ =
            paused ? PlaybackState::paused : PlaybackState::playing;
        track_title_ = "Opening...";
        clear_analysis();
        redraw();
    }

    void on_playback_new_track(metadb_handle_ptr track) override {
        refresh_playback_snapshot();
        begin_analysis(std::move(track));
    }

    void on_playback_stop(play_control::t_stop_reason) override {
        core_api::ensure_main_thread();
        playback_state_ = PlaybackState::stopped;
        track_title_ = "No track";
        playback_position_ = 0.0;
        clear_analysis();
        redraw();
    }

    void on_playback_pause(bool) override {
        refresh_playback_snapshot();
    }

    void on_playback_seek(double time) override {
        core_api::ensure_main_thread();
        set_playback_position(time);
    }

    void on_playback_time(double time) override {
        core_api::ensure_main_thread();
        set_playback_position(time);
    }

    void on_playback_edited(metadb_handle_ptr track) override {
        refresh_playback_snapshot();
        begin_analysis(std::move(track));
    }

    void on_playback_dynamic_info_track(const file_info&) override {
        refresh_playback_snapshot();
    }

    HWND window_ = nullptr;
    ui_element_config::ptr configuration_;
    const ui_element_instance_callback::ptr callback_;
    const playback_control::ptr playback_;
    titleformat_object::ptr title_script_;
    pfc::string8 track_title_ = "No track";
    PlaybackState playback_state_ = PlaybackState::stopped;
    WaveformState waveform_state_ = WaveformState::no_track;
    double playback_position_ = 0.0;
    double view_begin_ = 0.0;
    double view_end_ = 1.0;
    double drag_view_begin_ = 0.0;
    double drag_view_end_ = 1.0;
    int mouse_down_x_ = 0;
    bool mouse_down_ = false;
    bool dragging_ = false;
    std::uint64_t analysis_generation_ = 0;
    std::string current_identity_;
    std::string analysis_error_;
    loop_finder::foobar::WaveformSnapshotPtr waveform_;
    loop_finder::foobar::WaveformCache waveform_cache_{8};
    HBITMAP waveform_layer_bitmap_ = nullptr;
    int waveform_layer_width_ = 0;
    int waveform_layer_height_ = 0;
    bool waveform_layer_dirty_ = true;
    loop_finder::foobar::WaveformAnalysis analysis_;
};

// The SDK's ui_element_impl helper uses ATL/WTL. This equivalent lifetime
// wrapper keeps a child HWND alive until the host releases its service and
// guarantees that destruction is deferred to foobar2000's main thread.
template <typename Instance>
class WindowService final : public implement_service_query<Instance> {
public:
    template <typename... Arguments>
    explicit WindowService(Arguments&&... arguments)
        : implement_service_query<Instance>(
              std::forward<Arguments>(arguments)...) {}

    int service_release() noexcept override {
        const int remaining = static_cast<int>(--reference_count_);
        if (remaining != 0) {
            return remaining;
        }

        const HWND window = this->get_wnd();
        if ((!core_api::is_main_thread() || window != nullptr) &&
            InterlockedExchange(&delayed_release_, 1) == 0) {
            service_impl_helper::release_object_delayed(this->as_service_base());
        } else if (window != nullptr &&
                   InterlockedExchange(&destroying_window_, 1) == 0) {
            service_ptr_t<service_base> keep_alive(this);
            DestroyWindow(window);
        } else {
            delete this;
        }
        return 0;
    }

    int service_add_ref() noexcept override {
        return static_cast<int>(++reference_count_);
    }

private:
    pfc::refcounter reference_count_;
    volatile LONG delayed_release_ = 0;
    volatile LONG destroying_window_ = 0;
};

class LoopFinderUiElement : public ui_element {
public:
    GUID get_guid() override {
        return kLoopFinderElementGuid;
    }

    GUID get_subclass() override {
        return ui_element_subclass_playback_information;
    }

    void get_name(pfc::string_base& output) override {
        output = "Loop Finder";
    }

    ui_element_instance::ptr instantiate(
        HWND parent,
        ui_element_config::ptr configuration,
        ui_element_instance_callback::ptr callback) override {
        core_api::ensure_main_thread();
        service_ptr_t<LoopFinderPanel> panel =
            new WindowService<LoopFinderPanel>(configuration, callback);
        panel->initialize_window(parent);
        return panel;
    }

    ui_element_config::ptr get_default_configuration() override {
        return ui_element_config::g_create_empty(kLoopFinderElementGuid);
    }

    ui_element_children_enumerator::ptr enumerate_children(
        ui_element_config::ptr) override {
        return nullptr;
    }

    bool get_description(pfc::string_base& output) override {
        output = "Shows an interactive waveform for the current local track.";
        return true;
    }
};

service_factory_single_t<LoopFinderUiElement> g_loop_finder_ui_element_factory;

} // namespace
#endif
