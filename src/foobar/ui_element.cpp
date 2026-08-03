#ifdef _WIN32
#include <foobar2000/SDK/foobar2000.h>

#include "editor_persistence.hpp"
#include "waveform_analysis.hpp"

#include "loop_finder/beat_grid.hpp"
#include "loop_finder/loop_engine.hpp"
#include "loop_finder/loop_transport.hpp"
#include "loop_finder/tap_tempo.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <iterator>
#include <locale.h>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <CommCtrl.h>
#include <windowsx.h>

#pragma comment(lib, "Comctl32.lib")

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
constexpr UINT_PTR kControlSubclassId = 1;

enum ControlId : int {
    kBpmEdit = 1001,
    kTapButton,
    kOffsetEdit,
    kSnapCombo,
    kSetInButton,
    kSetOutButton,
    kLoopCheckbox,
};

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

COLORREF mix_colors(COLORREF foreground,
                    COLORREF background,
                    unsigned foreground_parts,
                    unsigned total_parts = 8U) noexcept {
    const auto channel = [&](BYTE front, BYTE back) noexcept -> BYTE {
        return static_cast<BYTE>(
            (static_cast<unsigned>(front) * foreground_parts +
             static_cast<unsigned>(back) * (total_parts - foreground_parts)) /
            total_parts);
    };
    return RGB(channel(GetRValue(foreground), GetRValue(background)),
               channel(GetGValue(foreground), GetGValue(background)),
               channel(GetBValue(foreground), GetBValue(background)));
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
        transport_.reset(loop_finder::LoopTransportResetReason::shutdown);
        if (active_loop_panel_ == this) {
            active_loop_panel_ = nullptr;
        }
        cancel_interaction(true);
        analysis_.cancel();
        if (window_ != nullptr) {
            KillTimer(window_, kCursorTimer);
        }
        discard_background_brush();
        discard_waveform_layer();
    }

    void initialize_window(HWND parent) {
        core_api::ensure_main_thread();
        ensure_window_class();

        const HWND window = CreateWindowExW(
            WS_EX_CONTROLPARENT,
            kLoopFinderWindowClass,
            L"Loop Finder",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN |
                WS_CLIPSIBLINGS,
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

        create_controls();
        update_control_font();
        layout_controls();

        refresh_playback_snapshot();
        metadb_handle_ptr current;
        if (playback_->get_now_playing(current)) {
            load_editor_for_track(current);
            begin_analysis(std::move(current));
        } else {
            sync_controls();
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
        result.m_min_width = static_cast<t_uint32>(scale_for_dpi(560, dpi));
        result.m_min_height = static_cast<t_uint32>(scale_for_dpi(380, dpi));
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
            discard_background_brush();
            update_control_font();
            layout_controls();
            redraw(true);
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

    enum class Interaction {
        none,
        pending_pan,
        panning,
        marker_in,
        marker_out,
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
            panel->layout_controls();
            return 0;

        case WM_COMMAND:
            panel->on_command(LOWORD(wparam), HIWORD(wparam));
            return 0;

        case WM_KEYDOWN:
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
                (wparam == 'T' || wparam == 't')) {
                panel->record_tap();
                return 0;
            }
            break;

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORLISTBOX:
            return panel->control_color(reinterpret_cast<HDC>(wparam));

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
            panel->on_double_click(GET_X_LPARAM(lparam),
                                   GET_Y_LPARAM(lparam));
            return 0;

        case WM_CAPTURECHANGED:
            panel->on_capture_lost();
            return 0;

        case WM_DPICHANGED:
        case 0x02E3: // WM_DPICHANGED_AFTERPARENT
            panel->discard_waveform_layer();
            panel->update_control_font();
            panel->layout_controls();
            panel->callback_->on_min_max_info_change();
            panel->redraw(true);
            return 0;

        case WM_NCDESTROY: {
            KillTimer(window, kCursorTimer);
            panel->cancel_interaction(false);
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

    struct PanelLayout {
        int margin{};
        int line_height{};
        int control_height{};
        int row_one_y{};
        int row_two_y{};
        RECT bpm_label{};
        RECT offset_label{};
        RECT snap_label{};
        RECT feedback{};
        RECT marker_info{};
        RECT duration_info{};
        RECT waveform{};
    };

    static LRESULT CALLBACK control_subclass_proc(HWND control,
                                                   UINT message,
                                                   WPARAM wparam,
                                                   LPARAM lparam,
                                                   UINT_PTR,
                                                   DWORD_PTR reference) noexcept {
        auto* panel = reinterpret_cast<LoopFinderPanel*>(reference);
        const bool is_numeric_edit =
            control == panel->bpm_edit_ || control == panel->offset_edit_;
        if (message == WM_GETDLGCODE && is_numeric_edit) {
            const LRESULT base = DefSubclassProc(control,
                                                 message,
                                                 wparam,
                                                 lparam);
            const auto* key_message = reinterpret_cast<const MSG*>(lparam);
            if (key_message != nullptr &&
                key_message->message == WM_KEYDOWN &&
                (key_message->wParam == VK_RETURN ||
                 key_message->wParam == VK_ESCAPE)) {
                return base | DLGC_WANTMESSAGE;
            }
            return base;
        }
        if (message == WM_KEYDOWN) {
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
                (wparam == 'T' || wparam == 't')) {
                panel->record_tap();
                return 0;
            }
            if (is_numeric_edit) {
                if (wparam == VK_RETURN) {
                    if (control == panel->bpm_edit_) panel->commit_bpm();
                    else panel->commit_offset();
                    return 0;
                }
                if (wparam == VK_ESCAPE) {
                    panel->restore_edit(control);
                    return 0;
                }
            }
        } else if (message == WM_CHAR && is_numeric_edit &&
                   (wparam == VK_RETURN || wparam == VK_ESCAPE)) {
            // The corresponding WM_KEYDOWN already committed or restored.
            return 0;
        } else if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(control,
                                 &LoopFinderPanel::control_subclass_proc,
                                 kControlSubclassId);
        }
        return DefSubclassProc(control, message, wparam, lparam);
    }

    HWND create_control(DWORD extended_style,
                        const wchar_t* class_name,
                        const wchar_t* text,
                        DWORD style,
                        int identifier) {
        HWND control = CreateWindowExW(
            extended_style,
            class_name,
            text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
            0,
            0,
            0,
            0,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
            core_api::get_my_instance(),
            nullptr);
        if (control == nullptr) {
            throw std::runtime_error("Could not create a Loop Finder editor control.");
        }
        SetWindowSubclass(control,
                          &LoopFinderPanel::control_subclass_proc,
                          kControlSubclassId,
                          reinterpret_cast<DWORD_PTR>(this));
        return control;
    }

    void create_controls() {
        bpm_edit_ = create_control(WS_EX_CLIENTEDGE,
                                   L"EDIT",
                                   L"120",
                                   ES_AUTOHSCROLL,
                                   kBpmEdit);
        tap_button_ = create_control(0,
                                     L"BUTTON",
                                     L"&Tap",
                                     BS_PUSHBUTTON,
                                     kTapButton);
        offset_edit_ = create_control(WS_EX_CLIENTEDGE,
                                      L"EDIT",
                                      L"0",
                                      ES_AUTOHSCROLL,
                                      kOffsetEdit);
        snap_combo_ = create_control(0,
                                     WC_COMBOBOXW,
                                     L"",
                                     CBS_DROPDOWNLIST | WS_VSCROLL,
                                     kSnapCombo);
        for (const wchar_t* option : {L"Off / free",
                                      L"1 beat",
                                      L"1/2 beat",
                                      L"1/4 beat",
                                      L"1/8 beat",
                                      L"1/16 beat"}) {
            SendMessageW(snap_combo_, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(option));
        }
        set_in_button_ = create_control(0,
                                        L"BUTTON",
                                        L"Set &IN",
                                        BS_PUSHBUTTON,
                                        kSetInButton);
        set_out_button_ = create_control(0,
                                         L"BUTTON",
                                         L"Set &OUT",
                                         BS_PUSHBUTTON,
                                         kSetOutButton);
        loop_checkbox_ = create_control(0,
                                        L"BUTTON",
                                        L"&Loop (seek-based)",
                                        BS_AUTOCHECKBOX,
                                        kLoopCheckbox);
        SendMessageW(bpm_edit_, EM_SETCUEBANNER, FALSE,
                     reinterpret_cast<LPARAM>(L"20-300"));
        SendMessageW(offset_edit_, EM_SETCUEBANNER, FALSE,
                     reinterpret_cast<LPARAM>(L"milliseconds"));
    }

    PanelLayout panel_layout() const noexcept {
        PanelLayout layout;
        RECT bounds{};
        if (window_ == nullptr || !GetClientRect(window_, &bounds)) {
            return layout;
        }
        const UINT dpi = window_dpi(window_);
        layout.margin = scale_for_dpi(12, dpi);
        layout.line_height = waveform_line_height();
        layout.control_height = (std::max)(scale_for_dpi(22, dpi),
                                            layout.line_height -
                                                scale_for_dpi(2, dpi));
        const int gap = scale_for_dpi(6, dpi);
        layout.row_one_y = bounds.top + layout.margin +
                           layout.line_height * 3 + gap;
        layout.row_two_y = layout.row_one_y + layout.line_height + gap;

        int x = bounds.left + layout.margin;
        layout.bpm_label = RECT{x, layout.row_one_y,
                                x + scale_for_dpi(42, dpi),
                                layout.row_one_y + layout.line_height};
        x = layout.bpm_label.right + scale_for_dpi(4, dpi) +
            scale_for_dpi(68, dpi) + scale_for_dpi(6, dpi) +
            scale_for_dpi(54, dpi) + scale_for_dpi(12, dpi);
        layout.offset_label = RECT{x, layout.row_one_y,
                                   x + scale_for_dpi(82, dpi),
                                   layout.row_one_y + layout.line_height};

        x = bounds.left + layout.margin;
        layout.snap_label = RECT{x, layout.row_two_y,
                                 x + scale_for_dpi(42, dpi),
                                 layout.row_two_y + layout.line_height};

        const int feedback_top = layout.row_two_y + layout.line_height + gap;
        layout.feedback = RECT{bounds.left + layout.margin,
                               feedback_top,
                               bounds.right - layout.margin,
                               feedback_top + layout.line_height};
        layout.marker_info = layout.feedback;
        OffsetRect(&layout.marker_info, 0, layout.line_height);
        layout.duration_info = layout.marker_info;
        OffsetRect(&layout.duration_info, 0, layout.line_height);
        const int waveform_top = layout.duration_info.bottom + gap;
        layout.waveform = RECT{bounds.left + layout.margin,
                               waveform_top,
                               (std::max)(static_cast<int>(bounds.left) +
                                              layout.margin,
                                          static_cast<int>(bounds.right) -
                                              layout.margin),
                               (std::max)(waveform_top,
                                          static_cast<int>(bounds.bottom) -
                                              layout.margin)};
        return layout;
    }

    void layout_controls() noexcept {
        if (bpm_edit_ == nullptr) {
            return;
        }
        auto layout = panel_layout();
        const UINT dpi = window_dpi(window_);
        const int gap = scale_for_dpi(6, dpi);
        int x = layout.bpm_label.right + scale_for_dpi(4, dpi);
        MoveWindow(bpm_edit_, x, layout.row_one_y,
                   scale_for_dpi(68, dpi), layout.control_height, TRUE);
        x += scale_for_dpi(68, dpi) + gap;
        MoveWindow(tap_button_, x, layout.row_one_y,
                   scale_for_dpi(54, dpi), layout.control_height, TRUE);
        x = layout.offset_label.right + scale_for_dpi(4, dpi);
        MoveWindow(offset_edit_, x, layout.row_one_y,
                   scale_for_dpi(92, dpi), layout.control_height, TRUE);

        x = layout.snap_label.right + scale_for_dpi(4, dpi);
        MoveWindow(snap_combo_, x, layout.row_two_y,
                   scale_for_dpi(118, dpi),
                   scale_for_dpi(180, dpi), TRUE);
        x += scale_for_dpi(118, dpi) + gap;
        MoveWindow(set_in_button_, x, layout.row_two_y,
                   scale_for_dpi(72, dpi), layout.control_height, TRUE);
        x += scale_for_dpi(72, dpi) + gap;
        MoveWindow(set_out_button_, x, layout.row_two_y,
                   scale_for_dpi(78, dpi), layout.control_height, TRUE);
        x += scale_for_dpi(78, dpi) + gap;
        MoveWindow(loop_checkbox_, x, layout.row_two_y,
                   scale_for_dpi(156, dpi), layout.control_height, TRUE);
    }

    void update_control_font() noexcept {
        if (window_ == nullptr) {
            return;
        }
        HFONT font = callback_->query_font_ex(ui_font_default);
        if (font == nullptr) {
            font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        }
        for (HWND control : {bpm_edit_, tap_button_, offset_edit_, snap_combo_,
                             set_in_button_, set_out_button_, loop_checkbox_}) {
            if (control != nullptr) {
                SendMessageW(control, WM_SETFONT,
                             reinterpret_cast<WPARAM>(font), TRUE);
            }
        }
    }

    void discard_background_brush() noexcept {
        if (background_brush_ != nullptr) {
            DeleteObject(background_brush_);
            background_brush_ = nullptr;
        }
    }

    LRESULT control_color(HDC dc) noexcept {
        const COLORREF background =
            callback_->query_std_color(ui_color_background);
        const COLORREF foreground = callback_->query_std_color(ui_color_text);
        SetBkColor(dc, background);
        SetTextColor(dc, foreground);
        if (background_brush_ == nullptr ||
            background_brush_color_ != background) {
            discard_background_brush();
            background_brush_ = CreateSolidBrush(background);
            background_brush_color_ = background;
        }
        return reinterpret_cast<LRESULT>(background_brush_);
    }

    static std::wstring format_number(double value, int precision = 8) {
        std::wostringstream stream;
        stream << std::setprecision(precision) << std::defaultfloat << value;
        return stream.str();
    }

    static std::optional<double> parse_number(HWND edit) noexcept {
        wchar_t buffer[128]{};
        const int length = GetWindowTextW(edit, buffer,
                                          static_cast<int>(std::size(buffer)));
        if (length <= 0) {
            return std::nullopt;
        }
        static _locale_t c_numeric_locale = _wcreate_locale(LC_NUMERIC, L"C");
        wchar_t* end = nullptr;
        const double value = c_numeric_locale != nullptr
            ? _wcstod_l(buffer, &end, c_numeric_locale)
            : std::wcstod(buffer, &end);
        while (end != nullptr && *end != L'\0' && std::iswspace(*end)) {
            ++end;
        }
        if (end == buffer || end == nullptr || *end != L'\0' ||
            !std::isfinite(value)) {
            return std::nullopt;
        }
        return value;
    }

    static void replace_edit_text_preserving_selection(
        HWND edit,
        const std::wstring& text) noexcept {
        DWORD selection_start = 0;
        DWORD selection_end = 0;
        SendMessageW(edit,
                     EM_GETSEL,
                     reinterpret_cast<WPARAM>(&selection_start),
                     reinterpret_cast<LPARAM>(&selection_end));
        SetWindowTextW(edit, text.c_str());
        const DWORD text_length = static_cast<DWORD>(text.size());
        selection_start = (std::min)(selection_start, text_length);
        selection_end = (std::min)(selection_end, text_length);
        SendMessageW(edit,
                     EM_SETSEL,
                     selection_start,
                     selection_end);
    }

    static int snap_combo_index(loop_finder::SnapMode mode) noexcept {
        switch (mode) {
        case loop_finder::SnapMode::off: return 0;
        case loop_finder::SnapMode::beat: return 1;
        case loop_finder::SnapMode::half_beat: return 2;
        case loop_finder::SnapMode::quarter_beat: return 3;
        case loop_finder::SnapMode::eighth_beat: return 4;
        case loop_finder::SnapMode::sixteenth_beat: return 5;
        }
        return 0;
    }

    static loop_finder::SnapMode snap_mode_at(int index) noexcept {
        switch (index) {
        case 1: return loop_finder::SnapMode::beat;
        case 2: return loop_finder::SnapMode::half_beat;
        case 3: return loop_finder::SnapMode::quarter_beat;
        case 4: return loop_finder::SnapMode::eighth_beat;
        case 5: return loop_finder::SnapMode::sixteenth_beat;
        default: return loop_finder::SnapMode::off;
        }
    }

    static const wchar_t* snap_mode_text(loop_finder::SnapMode mode) noexcept {
        switch (mode) {
        case loop_finder::SnapMode::off: return L"Off / free";
        case loop_finder::SnapMode::beat: return L"1 beat";
        case loop_finder::SnapMode::half_beat: return L"1/2 beat";
        case loop_finder::SnapMode::quarter_beat: return L"1/4 beat";
        case loop_finder::SnapMode::eighth_beat: return L"1/8 beat";
        case loop_finder::SnapMode::sixteenth_beat: return L"1/16 beat";
        }
        return L"Off / free";
    }

    void sync_controls() noexcept {
        if (bpm_edit_ == nullptr) {
            return;
        }
        updating_controls_ = true;
        const auto& state = loop_engine_.state();
        SetWindowTextW(bpm_edit_, format_number(state.bpm).c_str());
        SetWindowTextW(offset_edit_,
                       format_number(state.grid_offset_seconds * 1000.0,
                                     10).c_str());
        SendMessageW(snap_combo_, CB_SETCURSEL,
                     snap_combo_index(loop_finder::snap_mode(state)), 0);
        SendMessageW(loop_checkbox_, BM_SETCHECK,
                     state.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        const BOOL enabled = current_track_.is_valid() ? TRUE : FALSE;
        for (HWND control : {bpm_edit_, tap_button_, offset_edit_, snap_combo_,
                             set_in_button_, set_out_button_, loop_checkbox_}) {
            EnableWindow(control, enabled);
        }
        updating_controls_ = false;
    }

    void restore_edit(HWND edit) noexcept {
        if (edit == bpm_edit_) {
            SetWindowTextW(edit, format_number(loop_engine_.state().bpm).c_str());
        } else if (edit == offset_edit_) {
            SetWindowTextW(edit,
                           format_number(
                               loop_engine_.state().grid_offset_seconds * 1000.0,
                               10).c_str());
        }
        editor_feedback_ = L"Last valid value restored";
        redraw(false, false);
    }

    void commit_bpm() noexcept {
        if (updating_controls_ || current_track_.is_empty()) {
            return;
        }
        const auto value = parse_number(bpm_edit_);
        if (!value.has_value()) {
            editor_feedback_ = L"BPM must be a finite number from 20 to 300";
            redraw(false, false);
            return;
        }
        const auto result = loop_engine_.set_bpm(*value);
        if (!result.valid) {
            editor_feedback_ = L"BPM must be from 20 to 300";
            redraw(false, false);
            return;
        }
        replace_edit_text_preserving_selection(
            bpm_edit_, format_number(loop_engine_.state().bpm));
        editor_feedback_ = L"BPM updated; markers were not moved";
        persist_editor();
        redraw(false, true);
    }

    void commit_offset() noexcept {
        if (updating_controls_ || current_track_.is_empty()) {
            return;
        }
        const auto milliseconds = parse_number(offset_edit_);
        if (!milliseconds.has_value()) {
            editor_feedback_ = L"Grid offset must be a finite millisecond value";
            redraw(false, false);
            return;
        }
        const auto result = loop_engine_.set_grid_offset(*milliseconds / 1000.0);
        if (!result.valid) {
            editor_feedback_ = L"Grid offset must be finite";
            redraw(false, false);
            return;
        }
        replace_edit_text_preserving_selection(
            offset_edit_,
            format_number(
                loop_engine_.state().grid_offset_seconds * 1000.0,
                10));
        editor_feedback_ = L"Grid phase updated; markers were not moved";
        persist_editor();
        redraw(false, true);
    }

    void on_command(int identifier, int notification) noexcept {
        if (updating_controls_) {
            return;
        }
        switch (identifier) {
        case kBpmEdit:
            if (notification == EN_KILLFOCUS) commit_bpm();
            break;
        case kOffsetEdit:
            if (notification == EN_KILLFOCUS) commit_offset();
            break;
        case kTapButton:
            if (notification == BN_CLICKED) record_tap();
            break;
        case kSnapCombo:
            if (notification == CBN_SELCHANGE) change_snapping();
            break;
        case kSetInButton:
            if (notification == BN_CLICKED) set_marker_from_playback(true);
            break;
        case kSetOutButton:
            if (notification == BN_CLICKED) set_marker_from_playback(false);
            break;
        case kLoopCheckbox:
            if (notification == BN_CLICKED) change_loop_enabled();
            break;
        default:
            break;
        }
    }

    void record_tap() noexcept {
        if (current_track_.is_empty()) {
            return;
        }
        const double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto tap = tap_tempo_.tap(now);
        if (!tap.bpm.has_value()) {
            std::wostringstream feedback;
            feedback << L"Tap tempo: " << tap.tap_count
                     << L"/4 taps (Ctrl+T)";
            if (tap.sequence_restarted) {
                feedback << L" - new sequence after timeout";
            }
            editor_feedback_ = feedback.str();
            redraw(false, false);
            return;
        }

        const auto result = loop_engine_.set_bpm(*tap.bpm);
        if (!result.valid) {
            editor_feedback_ = L"Tapped tempo is outside 20-300 BPM";
            redraw(false, false);
            return;
        }
        SetWindowTextW(bpm_edit_,
                       format_number(loop_engine_.state().bpm, 6).c_str());
        std::wostringstream feedback;
        feedback << L"Tap tempo: " << std::fixed << std::setprecision(2)
                 << loop_engine_.state().bpm << L" BPM (Ctrl+T)";
        editor_feedback_ = feedback.str();
        persist_editor();
        redraw(false, true);
    }

    void change_snapping() noexcept {
        if (current_track_.is_empty()) {
            return;
        }
        const int selected = static_cast<int>(
            SendMessageW(snap_combo_, CB_GETCURSEL, 0, 0));
        const auto result = loop_engine_.set_snapping(snap_mode_at(selected));
        if (!result.valid) {
            editor_feedback_ = L"Unsupported snapping selection";
            sync_controls();
            redraw(false, false);
            return;
        }
        editor_feedback_ = L"Snapping changed; existing markers were not moved";
        persist_editor();
        redraw(false, true);
    }

    double track_duration() const noexcept {
        if (waveform_ && std::isfinite(waveform_->duration_seconds) &&
            waveform_->duration_seconds > 0.0) {
            return waveform_->duration_seconds;
        }
        if (current_track_.is_valid()) {
            try {
                const double duration = current_track_->get_length();
                if (std::isfinite(duration) && duration > 0.0) {
                    return duration;
                }
            } catch (...) {
            }
        }
        return 0.0;
    }

    void set_marker_from_playback(bool is_in) noexcept {
        const double duration = track_duration();
        if (duration <= 0.0) {
            editor_feedback_ = L"Markers require a track with a known duration";
            redraw(false, false);
            return;
        }
        const auto result = is_in
            ? loop_engine_.set_in_clamped(playback_position_, duration)
            : loop_engine_.set_out_clamped(playback_position_, duration);
        if (!result.valid) {
            editor_feedback_ = is_in
                ? L"IN was rejected: it must remain before OUT"
                : L"OUT was rejected: it must remain after IN";
            redraw(false, false);
            return;
        }
        editor_feedback_ = is_in ? L"IN set from playback position"
                                 : L"OUT set from playback position";
        reconfigure_transport_for_markers();
        persist_editor();
        redraw(false, false);
    }

    static double monotonic_seconds() noexcept {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    loop_finder::LoopRegion loop_region() const noexcept {
        return {loop_engine_.state().in_seconds,
                loop_engine_.state().out_seconds};
    }

    bool source_can_seek() const noexcept {
        try {
            return playback_->is_playing() && playback_->playback_can_seek();
        } catch (...) {
            return false;
        }
    }

    std::optional<double> transport_position() const noexcept {
        try {
            if (!playback_->is_playing()) {
                return std::nullopt;
            }
            const double position = playback_->playback_get_position();
            return std::isfinite(position)
                ? std::optional<double>(position)
                : std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }

    void force_loop_off(loop_finder::LoopTransportResetReason reason,
                        const wchar_t* feedback,
                        bool repaint = true) noexcept {
        (void)loop_engine_.set_enabled(false);
        transport_.reset(reason);
        if (active_loop_panel_ == this) {
            active_loop_panel_ = nullptr;
        }
        if (feedback != nullptr) {
            editor_feedback_ = feedback;
        }
        sync_controls();
        sync_cursor_timer();
        if (repaint) {
            redraw(false, false);
        }
    }

    void yield_loop_ownership() noexcept {
        force_loop_off(loop_finder::LoopTransportResetReason::loop_disabled,
                       L"Loop: Off - another Loop Finder panel took control");
    }

    void reconfigure_transport_for_markers() noexcept {
        if (!loop_engine_.state().enabled) {
            return;
        }
        if (!source_can_seek()) {
            force_loop_off(loop_finder::LoopTransportResetReason::unseekable,
                           L"Loop: Off - source is not seekable");
            return;
        }
        if (!transport_.update_markers(loop_region(), transport_position())) {
            force_loop_off(
                loop_finder::LoopTransportResetReason::invalid_markers,
                L"Loop: Off - markers are invalid");
            return;
        }
        try {
            transport_.set_paused(playback_->is_paused());
        } catch (...) {
            transport_.set_paused(true);
        }
    }

    void change_loop_enabled() noexcept {
        const bool requested =
            SendMessageW(loop_checkbox_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (!requested) {
            force_loop_off(loop_finder::LoopTransportResetReason::loop_disabled,
                           L"Loop: Off");
            return;
        }

        if (!source_can_seek()) {
            force_loop_off(loop_finder::LoopTransportResetReason::unseekable,
                           L"Loop: Off - source is not seekable");
            return;
        }

        const auto result = loop_engine_.set_enabled(true);
        if (!result.valid) {
            force_loop_off(
                loop_finder::LoopTransportResetReason::invalid_markers,
                L"Loop: Off - markers are invalid");
            return;
        }

        if (active_loop_panel_ != nullptr && active_loop_panel_ != this) {
            active_loop_panel_->yield_loop_ownership();
        }
        if (!transport_.enable(loop_region(), true, transport_position())) {
            force_loop_off(transport_.reset_reason(),
                           L"Loop: Off - transport could not be armed");
            return;
        }
        try {
            transport_.set_paused(playback_->is_paused());
        } catch (...) {
            transport_.set_paused(true);
        }
        active_loop_panel_ = this;
        editor_feedback_ = L"Loop: On (seek-based)";
        sync_controls();
        sync_cursor_timer();
        redraw(false, false);
    }

    void persist_editor() noexcept {
        if (current_track_.is_empty()) {
            return;
        }
        std::string error;
        if (!loop_finder::foobar::EditorPersistence::save(
                current_track_, loop_engine_.state(), error)) {
            const pfc::stringcvt::string_wide_from_utf8 wide(error.c_str());
            editor_feedback_ = std::wstring(L"Persistence: ") + wide.get_ptr();
        }
    }

    void load_editor_for_track(const metadb_handle_ptr& track) noexcept {
        cancel_interaction(true);
        transport_.reset(loop_finder::LoopTransportResetReason::track_changed);
        if (active_loop_panel_ == this) {
            active_loop_panel_ = nullptr;
        }
        current_track_ = track;
        tap_tempo_.reset();
        std::string warning;
        auto saved = loop_finder::foobar::EditorPersistence::load(track, warning);
        loop_finder::LoopEngine next(saved.has_value()
                                         ? *saved
                                         : loop_finder::LoopState{});

        double duration = 0.0;
        try {
            duration = track->get_length();
        } catch (...) {
        }
        if (std::isfinite(duration) && duration > 0.0) {
            clamp_engine_to_duration(next, duration);
        }
        loop_engine_ = next;
        (void)loop_engine_.set_enabled(false);
        if (!warning.empty()) {
            const pfc::stringcvt::string_wide_from_utf8 wide(warning.c_str());
            editor_feedback_ = wide.get_ptr();
        } else if (saved.has_value()) {
            editor_feedback_ = L"Saved grid and markers restored; Loop: Off";
        } else {
            editor_feedback_ = L"Editor ready - Ctrl+T taps tempo; Loop: Off";
        }
        sync_controls();
    }

    static void clamp_engine_to_duration(loop_finder::LoopEngine& engine,
                                         double duration) noexcept {
        if (!std::isfinite(duration) || duration <= 0.0) {
            return;
        }
        const double in_seconds = engine.state().in_seconds;
        const double out_seconds = engine.state().out_seconds;
        if (!engine.set_markers_clamped(in_seconds,
                                        out_seconds,
                                        duration).valid) {
            (void)engine.set_markers(0.0, duration);
        }
    }

    void clear_editor(loop_finder::LoopTransportResetReason reason) noexcept {
        cancel_interaction(true);
        transport_.reset(reason);
        if (active_loop_panel_ == this) {
            active_loop_panel_ = nullptr;
        }
        current_track_.release();
        loop_engine_ = loop_finder::LoopEngine{};
        tap_tempo_.reset();
        editor_feedback_ = L"No track - Loop: Off";
        sync_controls();
    }

    static std::wstring format_timecode(double seconds) {
        seconds = (std::max)(0.0, seconds);
        const auto total_milliseconds = static_cast<long long>(
            std::llround(seconds * 1000.0));
        const auto minutes = total_milliseconds / 60000;
        const auto whole_seconds = (total_milliseconds / 1000) % 60;
        const auto milliseconds = total_milliseconds % 1000;
        std::wostringstream output;
        output << minutes << L':' << std::setw(2) << std::setfill(L'0')
               << whole_seconds << L'.' << std::setw(3) << milliseconds;
        return output.str();
    }

    std::wstring marker_info_text() const {
        const auto& state = loop_engine_.state();
        return std::wstring(L"IN ") + format_timecode(state.in_seconds) +
               L"   OUT " + format_timecode(state.out_seconds) +
               L"   Length " + format_timecode(loop_engine_.loop_length_seconds()) +
               L" (" + format_number(loop_engine_.loop_length_seconds(), 7) + L" s)";
    }

    std::wstring duration_info_text() const {
        std::wostringstream output;
        output << std::setprecision(6) << loop_engine_.loop_length_beats()
               << L" beats";
        if (const auto bars = loop_engine_.loop_length_bars(); bars.has_value()) {
            output << L" / " << *bars << L" bars in "
                   << loop_engine_.state().beats_per_bar << L"/4";
        } else {
            output << L" / bars: - in " << loop_engine_.state().beats_per_bar
                   << L"/4";
        }
        output << L"   BPM " << loop_engine_.state().bpm
               << L"   Snap "
               << snap_mode_text(loop_finder::snap_mode(loop_engine_.state()));
        return output.str();
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

        auto layout = panel_layout();
        const UINT text_flags =
            DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS;

        RECT line{bounds.left + layout.margin,
                  bounds.top + layout.margin,
                  (std::max)(bounds.left + layout.margin,
                             bounds.right - layout.margin),
                  bounds.bottom};
        const bool waveform_only_paint =
            paint_state.rcPaint.left >= layout.waveform.left &&
            paint_state.rcPaint.top >= layout.waveform.top &&
            paint_state.rcPaint.right <= layout.waveform.right &&
            paint_state.rcPaint.bottom <= layout.waveform.bottom;

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
            line.bottom = line.top + layout.line_height;
            DrawTextW(dc, L"Loop Finder", -1, &line, text_flags);

            SelectObject(dc, font);
            line.top += layout.line_height;
            line.bottom = line.top + layout.line_height;
            const pfc::stringcvt::string_wide_from_utf8 wide_title(track_title_);
            const std::wstring track_line =
                std::wstring(L"Track: ") + wide_title.get_ptr();
            DrawTextW(dc, track_line.c_str(), -1, &line, text_flags);

            line.top += layout.line_height;
            line.bottom = line.top + layout.line_height;
            const std::wstring playback_line =
                std::wstring(L"Playback: ") + playback_state_text();
            DrawTextW(dc, playback_line.c_str(), -1, &line, text_flags);

            DrawTextW(dc, L"BPM:", -1, &layout.bpm_label, text_flags);
            DrawTextW(dc, L"Offset ms:", -1, &layout.offset_label, text_flags);
            DrawTextW(dc, L"Snap:", -1, &layout.snap_label, text_flags);

            SetTextColor(dc, blend_colors(foreground, background));
            DrawTextW(dc, editor_feedback_.c_str(), -1,
                      &layout.feedback, text_flags);
            SetTextColor(dc, foreground);
            const std::wstring markers = marker_info_text();
            DrawTextW(dc, markers.c_str(), -1,
                      &layout.marker_info, text_flags);
            const std::wstring duration = duration_info_text();
            DrawTextW(dc, duration.c_str(), -1,
                      &layout.duration_info, text_flags);
        }

        paint_waveform(dc,
                       layout.waveform,
                       foreground,
                       background,
                       font,
                       layout.line_height);

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

        if (ensure_editor_layer(
                dc, bounds, foreground, background, font, line_height)) {
            HDC memory_dc = CreateCompatibleDC(dc);
            if (memory_dc != nullptr) {
                const HGDIOBJ previous =
                    SelectObject(memory_dc, editor_layer_bitmap_);
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
            draw_grid_overlay(dc,
                              waveform_graph_bounds(bounds, line_height),
                              foreground,
                              background);
        }

        const RECT graph = waveform_graph_bounds(bounds, line_height);
        draw_markers(dc, graph, foreground, background, font);
        draw_cursor(dc, graph);
    }

    bool ensure_editor_layer(HDC target,
                             const RECT& bounds,
                             COLORREF foreground,
                             COLORREF background,
                             HFONT font,
                             int line_height) noexcept {
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        if (!ensure_waveform_layer(target,
                                   bounds,
                                   foreground,
                                   background,
                                   font,
                                   line_height)) {
            return false;
        }
        if (!editor_layer_dirty_ && editor_layer_bitmap_ != nullptr &&
            editor_layer_width_ == width && editor_layer_height_ == height) {
            return true;
        }

        discard_editor_layer();
        HDC memory_dc = CreateCompatibleDC(target);
        HDC waveform_dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
        if (memory_dc == nullptr || waveform_dc == nullptr || bitmap == nullptr) {
            if (bitmap != nullptr) DeleteObject(bitmap);
            if (waveform_dc != nullptr) DeleteDC(waveform_dc);
            if (memory_dc != nullptr) DeleteDC(memory_dc);
            return false;
        }
        const HGDIOBJ previous_bitmap = SelectObject(memory_dc, bitmap);
        const HGDIOBJ previous_waveform =
            SelectObject(waveform_dc, waveform_layer_bitmap_);
        if (previous_bitmap == nullptr || previous_bitmap == HGDI_ERROR ||
            previous_waveform == nullptr || previous_waveform == HGDI_ERROR) {
            if (previous_bitmap != nullptr && previous_bitmap != HGDI_ERROR) {
                SelectObject(memory_dc, previous_bitmap);
            }
            if (previous_waveform != nullptr && previous_waveform != HGDI_ERROR) {
                SelectObject(waveform_dc, previous_waveform);
            }
            DeleteObject(bitmap);
            DeleteDC(waveform_dc);
            DeleteDC(memory_dc);
            return false;
        }

        BitBlt(memory_dc, 0, 0, width, height, waveform_dc, 0, 0, SRCCOPY);
        SelectObject(memory_dc, font);
        const RECT local_bounds{0, 0, width, height};
        draw_grid_overlay(memory_dc,
                          waveform_graph_bounds(local_bounds, line_height),
                          foreground,
                          background);

        SelectObject(waveform_dc, previous_waveform);
        SelectObject(memory_dc, previous_bitmap);
        DeleteDC(waveform_dc);
        DeleteDC(memory_dc);
        editor_layer_bitmap_ = bitmap;
        editor_layer_width_ = width;
        editor_layer_height_ = height;
        editor_layer_dirty_ = false;
        return true;
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
        discard_editor_layer();
        if (waveform_layer_bitmap_ != nullptr) {
            DeleteObject(waveform_layer_bitmap_);
            waveform_layer_bitmap_ = nullptr;
        }
        waveform_layer_width_ = 0;
        waveform_layer_height_ = 0;
        waveform_layer_dirty_ = true;
    }

    void discard_editor_layer() noexcept {
        if (editor_layer_bitmap_ != nullptr) {
            DeleteObject(editor_layer_bitmap_);
            editor_layer_bitmap_ = nullptr;
        }
        editor_layer_width_ = 0;
        editor_layer_height_ = 0;
        editor_layer_dirty_ = true;
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
                  L"Marker: drag  Empty: pan/seek  Wheel: zoom  Double-click: overview",
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

    std::optional<int> time_x(double seconds, const RECT& graph) const noexcept {
        if (!waveform_ || waveform_->duration_seconds <= 0.0 ||
            graph.right <= graph.left || graph.bottom <= graph.top) {
            return std::nullopt;
        }
        const double normalized = seconds / waveform_->duration_seconds;
        if (normalized < view_begin_ || normalized > view_end_) {
            return std::nullopt;
        }
        const double relative =
            (normalized - view_begin_) / (view_end_ - view_begin_);
        return graph.left + static_cast<int>(relative *
                                             (graph.right - graph.left - 1));
    }

    void draw_grid_overlay(HDC dc,
                           const RECT& graph,
                           COLORREF foreground,
                           COLORREF background) noexcept {
        if (!waveform_ || waveform_->duration_seconds <= 0.0 ||
            graph.right <= graph.left || graph.bottom <= graph.top) {
            return;
        }

        const double duration = waveform_->duration_seconds;
        const double from_seconds = view_begin_ * duration;
        const double to_seconds = view_end_ * duration;
        const double visible_seconds = to_seconds - from_seconds;
        const int width = graph.right - graph.left;
        if (visible_seconds <= 0.0 || width <= 0) {
            return;
        }

        try {
            const int grid_spacing =
                (std::max)(1, scale_for_dpi(3, window_dpi(window_)));
            const std::size_t maximum_lines = static_cast<std::size_t>(
                (std::max)(1, width / grid_spacing));
            const auto lines = loop_finder::grid_lines(from_seconds,
                                                        to_seconds,
                                                        loop_engine_.state(),
                                                        maximum_lines);
            const double beat_pixels = loop_finder::beat_duration(
                loop_engine_.state().bpm) / visible_seconds * width;
            const double subdivision_pixels = beat_pixels /
                loop_engine_.state().subdivisions_per_beat;
            const double minimum_pixels = scale_for_dpi(4, window_dpi(window_));

            HPEN subdivision_pen = CreatePen(
                PS_SOLID, 1, mix_colors(foreground, background, 1));
            HPEN beat_pen = CreatePen(
                PS_SOLID, 1, mix_colors(foreground, background, 3));
            HPEN bar_pen = CreatePen(
                PS_SOLID,
                (std::max)(1, scale_for_dpi(2, window_dpi(window_))),
                mix_colors(callback_->query_std_color(ui_color_highlight),
                           background,
                           5));
            for (const auto& line : lines) {
                if (!line.is_beat && subdivision_pixels < minimum_pixels) {
                    continue;
                }
                if (line.is_beat && !line.is_bar &&
                    beat_pixels < minimum_pixels) {
                    continue;
                }
                const auto x = time_x(line.seconds, graph);
                if (!x.has_value()) {
                    continue;
                }
                HPEN pen = line.is_bar ? bar_pen
                                      : (line.is_beat ? beat_pen
                                                      : subdivision_pen);
                const HGDIOBJ previous =
                    pen != nullptr ? SelectObject(dc, pen) : nullptr;
                MoveToEx(dc, *x, graph.top, nullptr);
                LineTo(dc, *x, graph.bottom);
                if (previous != nullptr && previous != HGDI_ERROR) {
                    SelectObject(dc, previous);
                }
            }
            if (subdivision_pen != nullptr) DeleteObject(subdivision_pen);
            if (beat_pen != nullptr) DeleteObject(beat_pen);
            if (bar_pen != nullptr) DeleteObject(bar_pen);
        } catch (...) {
            // Overlay failures must not invalidate the cached waveform layer.
        }

    }

    void draw_markers(HDC dc,
                      const RECT& graph,
                      COLORREF foreground,
                      COLORREF background,
                      HFONT font) noexcept {
        draw_marker(dc,
                    graph,
                    loop_engine_.state().in_seconds,
                    L"IN",
                    callback_->query_std_color(ui_color_highlight),
                    background,
                    font,
                    false);
        draw_marker(dc,
                    graph,
                    loop_engine_.state().out_seconds,
                    L"OUT",
                    foreground,
                    background,
                    font,
                    true);
    }

    void draw_marker(HDC dc,
                     const RECT& graph,
                     double seconds,
                     const wchar_t* label,
                     COLORREF color,
                     COLORREF background,
                     HFONT font,
                     bool label_left) noexcept {
        const auto x = time_x(seconds, graph);
        if (!x.has_value()) {
            return;
        }
        const UINT dpi = window_dpi(window_);
        HPEN pen = CreatePen(PS_SOLID,
                             (std::max)(1, scale_for_dpi(2, dpi)),
                             color);
        const HGDIOBJ previous_pen =
            pen != nullptr ? SelectObject(dc, pen) : nullptr;
        MoveToEx(dc, *x, graph.top, nullptr);
        LineTo(dc, *x, graph.bottom);
        if (previous_pen != nullptr && previous_pen != HGDI_ERROR) {
            SelectObject(dc, previous_pen);
        }
        if (pen != nullptr) DeleteObject(pen);

        const int label_width = scale_for_dpi(label_left ? 34 : 24, dpi);
        const int label_height = scale_for_dpi(18, dpi);
        int label_x = label_left ? *x - label_width : *x;
        const int maximum_label_x = (std::max)(
            static_cast<int>(graph.left),
            static_cast<int>(graph.right) - label_width);
        label_x = std::clamp(label_x,
                             static_cast<int>(graph.left),
                             maximum_label_x);
        RECT label_bounds{label_x,
                          graph.top,
                          label_x + label_width,
                          (std::min)(graph.bottom, graph.top + label_height)};
        HBRUSH brush = CreateSolidBrush(background);
        if (brush != nullptr) {
            FillRect(dc, &label_bounds, brush);
            DeleteObject(brush);
        }
        SetTextColor(dc, color);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, font);
        DrawTextW(dc, label, -1, &label_bounds,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
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
        return time_x(playback_position, graph);
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
        int line_height = scale_for_dpi(24, dpi);
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
        return panel_layout().waveform;
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
            const double previous_in = loop_engine_.state().in_seconds;
            const double previous_out = loop_engine_.state().out_seconds;
            clamp_engine_to_duration(loop_engine_, waveform_->duration_seconds);
            if (loop_engine_.state().in_seconds != previous_in ||
                loop_engine_.state().out_seconds != previous_out) {
                reconfigure_transport_for_markers();
            }
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

    void on_double_click(int x, int y) noexcept {
        const RECT graph = waveform_graph_bounds(waveform_bounds(),
                                                 waveform_line_height());
        if (contains(graph, x, y)) {
            reset_view();
        }
    }

    void on_mouse_wheel(WPARAM wparam, LPARAM lparam) noexcept {
        if (waveform_state_ != WaveformState::available || !waveform_) {
            return;
        }
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window_, &point);
        const RECT bounds = waveform_graph_bounds(waveform_bounds(),
                                                  waveform_line_height());
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
        const RECT bounds = waveform_graph_bounds(waveform_bounds(),
                                                  waveform_line_height());
        if (waveform_state_ != WaveformState::available || !waveform_ ||
            !contains(bounds, x, y)) {
            return;
        }
        const int hit_radius = scale_for_dpi(8, window_dpi(window_));
        const auto in_x = time_x(loop_engine_.state().in_seconds, bounds);
        const auto out_x = time_x(loop_engine_.state().out_seconds, bounds);
        const int in_distance = in_x.has_value()
            ? std::abs(x - *in_x)
            : (std::numeric_limits<int>::max)();
        const int out_distance = out_x.has_value()
            ? std::abs(x - *out_x)
            : (std::numeric_limits<int>::max)();
        if ((std::min)(in_distance, out_distance) <= hit_radius) {
            interaction_ = in_distance <= out_distance
                ? Interaction::marker_in
                : Interaction::marker_out;
            drag_marker_in_ = loop_engine_.state().in_seconds;
            drag_marker_out_ = loop_engine_.state().out_seconds;
            editor_feedback_ = interaction_ == Interaction::marker_in
                ? L"Dragging IN (release to save)"
                : L"Dragging OUT (release to save)";
            redraw(false, false);
        } else {
            interaction_ = Interaction::pending_pan;
        }
        SetCapture(window_);
        mouse_down_x_ = x;
        drag_view_begin_ = view_begin_;
        drag_view_end_ = view_end_;
    }

    void on_mouse_move(int x, int) noexcept {
        if (interaction_ == Interaction::none || GetCapture() != window_) {
            return;
        }
        const RECT bounds = waveform_graph_bounds(waveform_bounds(),
                                                  waveform_line_height());
        const int width = bounds.right - bounds.left;
        if (width <= 0) {
            return;
        }

        if (interaction_ == Interaction::marker_in ||
            interaction_ == Interaction::marker_out) {
            const bool dragging_out = interaction_ == Interaction::marker_out;
            const double previous_seconds = dragging_out
                ? loop_engine_.state().out_seconds
                : loop_engine_.state().in_seconds;
            const double relative = std::clamp(
                static_cast<double>(x - bounds.left) /
                    static_cast<double>(width),
                0.0,
                1.0);
            const double normalized =
                view_begin_ + relative * (view_end_ - view_begin_);
            const double seconds = normalized * waveform_->duration_seconds;
            const auto result = !dragging_out
                ? loop_engine_.set_in_clamped(seconds,
                                              waveform_->duration_seconds)
                : loop_engine_.set_out_clamped(seconds,
                                               waveform_->duration_seconds);
            if (result.valid) {
                const double current_seconds = dragging_out
                    ? loop_engine_.state().out_seconds
                    : loop_engine_.state().in_seconds;
                if (current_seconds != previous_seconds) {
                    reconfigure_transport_for_markers();
                    redraw_marker_drag(previous_seconds,
                                       current_seconds,
                                       dragging_out);
                }
            }
            return;
        }

        const int distance = x - mouse_down_x_;
        if (interaction_ == Interaction::pending_pan &&
            std::abs(distance) >= scale_for_dpi(3, window_dpi(window_))) {
            interaction_ = Interaction::panning;
        }
        if (interaction_ != Interaction::panning) {
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
        if (interaction_ == Interaction::none) {
            return;
        }
        const Interaction completed = interaction_;
        interaction_ = Interaction::none;
        if (GetCapture() == window_) {
            ReleaseCapture();
        }
        if (completed == Interaction::pending_pan) {
            seek_at(x, y);
        } else if (completed == Interaction::marker_in ||
                   completed == Interaction::marker_out) {
            editor_feedback_ = completed == Interaction::marker_in
                ? L"IN marker saved"
                : L"OUT marker saved";
            persist_editor();
            redraw(false, false);
        }
    }

    void on_capture_lost() noexcept {
        if (interaction_ == Interaction::marker_in ||
            interaction_ == Interaction::marker_out) {
            (void)loop_engine_.set_markers(drag_marker_in_, drag_marker_out_);
            reconfigure_transport_for_markers();
            editor_feedback_ = L"Marker drag cancelled";
            redraw(false, false);
        }
        interaction_ = Interaction::none;
    }

    void cancel_interaction(bool release_capture) noexcept {
        if (interaction_ == Interaction::marker_in ||
            interaction_ == Interaction::marker_out) {
            (void)loop_engine_.set_markers(drag_marker_in_, drag_marker_out_);
            reconfigure_transport_for_markers();
        }
        interaction_ = Interaction::none;
        if (release_capture && window_ != nullptr && GetCapture() == window_) {
            ReleaseCapture();
        }
    }

    void seek_at(int x, int y) noexcept {
        const RECT bounds = waveform_graph_bounds(waveform_bounds(),
                                                  waveform_line_height());
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

    void log_transport_request(
        const loop_finder::LoopSeekRequest& request) noexcept {
#ifndef NDEBUG
        try {
            FB2K_console_formatter()
                << "Loop Finder M5: OUT crossing observed at "
                << request.crossing_position_seconds << " s; seek IN "
                << request.target_seconds << " s; overshoot "
                << request.boundary_overshoot_seconds << " s; request "
                << request.request_id;
        } catch (...) {
        }
#else
        (void)request;
#endif
    }

    void log_completed_transport_timing() noexcept {
#ifndef NDEBUG
        const auto& timing = transport_.diagnostics();
        if (!timing.has_value() ||
            !timing->return_observed_after_seconds.has_value() ||
            timing->request_id == logged_return_request_id_) {
            return;
        }
        logged_return_request_id_ = timing->request_id;
        try {
            FB2K_console_formatter()
                << "Loop Finder M5: request " << timing->request_id
                << " observed back inside loop at "
                << *timing->return_position_seconds << " s after "
                << *timing->return_observed_after_seconds << " s";
        } catch (...) {
        }
#endif
    }

    void observe_transport_position(double position) noexcept {
        if (!loop_engine_.state().enabled ||
            playback_state_ != PlaybackState::playing) {
            return;
        }

        const auto prior_state = transport_.state();
        const auto request = transport_.observe_position(
            position, monotonic_seconds(), transport_.generation());
        if (prior_state == loop_finder::LoopTransportState::manually_disarmed &&
            transport_.state() == loop_finder::LoopTransportState::armed) {
            editor_feedback_ =
                L"Loop: On (seek-based) - playback re-entered region";
            redraw(false, false);
        }
        log_completed_transport_timing();
        if (!request.has_value()) {
            return;
        }
        if (request->generation != transport_.generation() ||
            !loop_engine_.state().enabled) {
            return;
        }

        log_transport_request(*request);
        queue_transport_seek(*request);
    }

    void execute_transport_seek(
        const loop_finder::LoopSeekRequest& request) noexcept {
        core_api::ensure_main_thread();
        if (window_ == nullptr || active_loop_panel_ != this ||
            !loop_engine_.state().enabled ||
            !transport_.is_pending(request)) {
            return;
        }

        try {
            if (!playback_->is_playing()) {
                force_loop_off(loop_finder::LoopTransportResetReason::stopped,
                               L"Loop: Off - playback stopped");
                return;
            }

            metadb_handle_ptr now_playing;
            if (!playback_->get_now_playing(now_playing) ||
                now_playing != current_track_) {
                force_loop_off(
                    loop_finder::LoopTransportResetReason::track_changed,
                    L"Loop: Off - track changed");
                return;
            }

            if (playback_->is_paused()) {
                (void)transport_.update_markers(
                    loop_region(), transport_position());
                transport_.set_paused(true);
                return;
            }
            if (!playback_->playback_can_seek()) {
                force_loop_off(
                    loop_finder::LoopTransportResetReason::unseekable,
                    L"Loop: Off - source is not seekable");
                return;
            }

            // This runs on a later main-loop turn, outside the playback
            // callback that detected the crossing. Calling playback_seek()
            // directly from on_playback_time triggers foobar2000's bug check.
            playback_->playback_seek(request.target_seconds);
        } catch (...) {
            force_loop_off(loop_finder::LoopTransportResetReason::unseekable,
                           L"Loop: Off - seek request failed");
        }
    }

    void queue_transport_seek(
        const loop_finder::LoopSeekRequest& request) noexcept {
        try {
            service_ptr_t<LoopFinderPanel> self = this;
            fb2k::inMainThread([self, request] {
                self->execute_transport_seek(request);
            });
        } catch (...) {
            force_loop_off(loop_finder::LoopTransportResetReason::unseekable,
                           L"Loop: Off - could not queue seek request");
        }
    }

    void observe_transport_seek(double position) noexcept {
        if (!loop_engine_.state().enabled) {
            return;
        }
        const auto kind = transport_.observe_seek(
            position, monotonic_seconds(), transport_.generation());
        switch (kind) {
        case loop_finder::SeekNotificationKind::manual_seek_inside:
            editor_feedback_ =
                L"Loop: On (seek-based) - manual seek inside re-armed";
            redraw(false, false);
            break;
        case loop_finder::SeekNotificationKind::manual_seek_outside:
            editor_feedback_ =
                L"Loop: On - manual seek outside; waiting to re-enter region";
            redraw(false, false);
            break;
        case loop_finder::SeekNotificationKind::automatic_seek:
        case loop_finder::SeekNotificationKind::ignored:
            break;
        }
    }

    void process_playback_position(double position) noexcept {
        set_playback_position(position);
        observe_transport_position(position);
    }

    void update_playback_cursor() noexcept {
        core_api::ensure_main_thread();
        if (playback_state_ != PlaybackState::playing ||
            !playback_->is_playing()) {
            sync_cursor_timer();
            return;
        }
        process_playback_position(playback_->playback_get_position());
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
            (waveform_state_ == WaveformState::available ||
             loop_engine_.state().enabled)) {
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
            redraw(false, false);
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
        redraw(false, false);
    }

    void redraw(bool invalidate_waveform_layer = true,
                bool invalidate_grid_layer = true) noexcept {
        if (window_ != nullptr) {
            if (invalidate_waveform_layer) {
                waveform_layer_dirty_ = true;
            }
            if (invalidate_grid_layer) {
                editor_layer_dirty_ = true;
            }
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void redraw_marker_drag(double previous_seconds,
                            double current_seconds,
                            bool label_left) noexcept {
        if (window_ == nullptr) {
            return;
        }
        const auto layout = panel_layout();
        const RECT graph = waveform_graph_bounds(layout.waveform,
                                                  layout.line_height);
        const auto invalidate_marker = [&](double seconds) noexcept {
            const auto x = time_x(seconds, graph);
            if (!x.has_value()) {
                return;
            }
            const UINT dpi = window_dpi(window_);
            const int padding = (std::max)(2, scale_for_dpi(3, dpi));
            const int label_width = scale_for_dpi(label_left ? 34 : 24, dpi);
            int label_x = label_left ? *x - label_width : *x;
            const int maximum_label_x = (std::max)(
                static_cast<int>(graph.left),
                static_cast<int>(graph.right) - label_width);
            label_x = std::clamp(label_x,
                                 static_cast<int>(graph.left),
                                 maximum_label_x);
            RECT marker_bounds{
                (std::max)(static_cast<int>(graph.left),
                           (std::min)(*x - padding, label_x)),
                graph.top,
                (std::min)(static_cast<int>(graph.right),
                           (std::max)(*x + padding + 1,
                                      label_x + label_width)),
                graph.bottom};
            if (marker_bounds.right > marker_bounds.left) {
                InvalidateRect(window_, &marker_bounds, FALSE);
            }
        };
        invalidate_marker(previous_seconds);
        invalidate_marker(current_seconds);
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
        clear_editor(loop_finder::LoopTransportResetReason::track_changed);
        redraw();
    }

    void on_playback_new_track(metadb_handle_ptr track) override {
        refresh_playback_snapshot();
        load_editor_for_track(track);
        begin_analysis(std::move(track));
    }

    void on_playback_stop(play_control::t_stop_reason) override {
        core_api::ensure_main_thread();
        playback_state_ = PlaybackState::stopped;
        track_title_ = "No track";
        playback_position_ = 0.0;
        clear_analysis();
        clear_editor(loop_finder::LoopTransportResetReason::stopped);
        redraw();
    }

    void on_playback_pause(bool paused) override {
        transport_.set_paused(paused);
        refresh_playback_snapshot();
    }

    void on_playback_seek(double time) override {
        core_api::ensure_main_thread();
        set_playback_position(time);
        observe_transport_seek(time);
    }

    void on_playback_time(double time) override {
        core_api::ensure_main_thread();
        process_playback_position(time);
    }

    void on_playback_edited(metadb_handle_ptr track) override {
        refresh_playback_snapshot();
        load_editor_for_track(track);
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
    Interaction interaction_ = Interaction::none;
    double drag_marker_in_ = 0.0;
    double drag_marker_out_ = 4.0;
    metadb_handle_ptr current_track_;
    loop_finder::LoopEngine loop_engine_;
    loop_finder::LoopTransport transport_;
    loop_finder::TapTempo tap_tempo_;
    std::wstring editor_feedback_ =
        L"No track - Loop: Off";
    HWND bpm_edit_ = nullptr;
    HWND tap_button_ = nullptr;
    HWND offset_edit_ = nullptr;
    HWND snap_combo_ = nullptr;
    HWND set_in_button_ = nullptr;
    HWND set_out_button_ = nullptr;
    HWND loop_checkbox_ = nullptr;
    bool updating_controls_ = false;
    std::uint64_t logged_return_request_id_ = 0;
    std::uint64_t analysis_generation_ = 0;
    std::string current_identity_;
    std::string analysis_error_;
    loop_finder::foobar::WaveformSnapshotPtr waveform_;
    loop_finder::foobar::WaveformCache waveform_cache_{8};
    HBITMAP waveform_layer_bitmap_ = nullptr;
    int waveform_layer_width_ = 0;
    int waveform_layer_height_ = 0;
    bool waveform_layer_dirty_ = true;
    HBITMAP editor_layer_bitmap_ = nullptr;
    int editor_layer_width_ = 0;
    int editor_layer_height_ = 0;
    bool editor_layer_dirty_ = true;
    HBRUSH background_brush_ = nullptr;
    COLORREF background_brush_color_ = CLR_INVALID;
    loop_finder::foobar::WaveformAnalysis analysis_;
    inline static LoopFinderPanel* active_loop_panel_ = nullptr;
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
        output = "Shows a waveform with a manual rhythmic loop editor for the "
                 "current local track.";
        return true;
    }
};

service_factory_single_t<LoopFinderUiElement> g_loop_finder_ui_element_factory;

} // namespace
#endif
