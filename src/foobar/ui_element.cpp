#ifdef _WIN32
#include <foobar2000/SDK/foobar2000.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

constexpr GUID kLoopFinderElementGuid = {
    0x938e8349,
    0x6599,
    0x4a7a,
    {0x86, 0xc4, 0x49, 0xb4, 0x80, 0x26, 0x0a, 0xa6}};

constexpr wchar_t kLoopFinderWindowClass[] =
    L"{7EFA898C-73E2-4C7F-AC87-21C7A5E87422}";

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
              flag_on_playback_edited | flag_on_playback_dynamic_info_track),
          configuration_(std::move(configuration)),
          callback_(std::move(callback)),
          playback_(playback_control::get()) {
        titleformat_compiler::get()->compile_safe(
            title_script_, "$if2(%title%,%filename%)");
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
        result.m_min_width = static_cast<t_uint32>(scale_for_dpi(180, dpi));
        result.m_min_height = static_cast<t_uint32>(scale_for_dpi(104, dpi));
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

    static void ensure_window_class() {
        static const ATOM window_class = [] {
            WNDCLASSEXW definition{};
            definition.cbSize = sizeof(definition);
            definition.style = CS_HREDRAW | CS_VREDRAW;
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

        case WM_DPICHANGED:
        case 0x02E3: // WM_DPICHANGED_AFTERPARENT
            panel->callback_->on_min_max_info_change();
            panel->redraw();
            return 0;

        case WM_NCDESTROY: {
            const LRESULT result =
                DefWindowProcW(window, message, wparam, lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            panel->window_ = nullptr;
            return result;
        }

        default:
            return DefWindowProcW(window, message, wparam, lparam);
        }
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

        HBRUSH background_brush = CreateSolidBrush(background);
        if (background_brush != nullptr) {
            FillRect(dc, &bounds, background_brush);
            DeleteObject(background_brush);
        }

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, foreground);

        HFONT font = callback_->query_font_ex(ui_font_default);
        if (font == nullptr) {
            font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        }
        const HGDIOBJ previous_font = SelectObject(dc, font);

        LOGFONTW font_description{};
        HFONT heading_font = nullptr;
        if (GetObjectW(font, sizeof(font_description), &font_description) != 0) {
            font_description.lfWeight =
                std::max<LONG>(font_description.lfWeight, FW_SEMIBOLD);
            heading_font = CreateFontIndirectW(&font_description);
        }

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

        if (heading_font != nullptr) {
            DeleteObject(heading_font);
        }
        if (previous_font != nullptr && previous_font != HGDI_ERROR) {
            SelectObject(dc, previous_font);
        }
        EndPaint(window_, &paint_state);
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
        redraw();
    }

    void redraw() noexcept {
        if (window_ != nullptr) {
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void on_playback_starting(play_control::t_track_command, bool paused) override {
        core_api::ensure_main_thread();
        playback_state_ =
            paused ? PlaybackState::paused : PlaybackState::playing;
        track_title_ = "Opening...";
        redraw();
    }

    void on_playback_new_track(metadb_handle_ptr) override {
        refresh_playback_snapshot();
    }

    void on_playback_stop(play_control::t_stop_reason) override {
        core_api::ensure_main_thread();
        playback_state_ = PlaybackState::stopped;
        track_title_ = "No track";
        redraw();
    }

    void on_playback_pause(bool) override {
        refresh_playback_snapshot();
    }

    void on_playback_edited(metadb_handle_ptr) override {
        refresh_playback_snapshot();
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
        output = "Shows the current track and playback state for Loop Finder.";
        return true;
    }
};

service_factory_single_t<LoopFinderUiElement> g_loop_finder_ui_element_factory;

} // namespace
#endif
