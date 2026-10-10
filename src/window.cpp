#include "window.hpp"
#include "theme.hpp"
#include <shellapi.h>
#include <dwmapi.h>
#include <imm.h>
#include <windowsx.h>
#include <algorithm>

namespace mq {
namespace {
constexpr UINT_PTR DraftTimer = 1;
constexpr UINT_PTR PositionTimer = 2;
constexpr UINT_PTR TrayTimer = 4;
constexpr UINT_PTR ResumeTimer = 5;
constexpr UINT_PTR LocationTimer = 6;
constexpr DWORD WindowStyle = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN;
bool trayIcon(HWND window, HINSTANCE instance, DWORD operation) {
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon); icon.hWnd = window; icon.uID = 1;
    icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    icon.uCallbackMessage = TrayMessage;
    icon.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wcscpy_s(icon.szTip, L"MemoQuick · 随手记下");
    const bool succeeded = Shell_NotifyIconW(operation, &icon) != FALSE;
    if (operation == NIM_ADD) {
        if (succeeded) KillTimer(window, TrayTimer);
        else SetTimer(window, TrayTimer, 3000, nullptr);
    }
    return succeeded;
}
bool imeComposing(HWND control) {
    const auto context = ImmGetContext(control);
    if (!context) return false;
    const bool composing = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0) > 0;
    ImmReleaseContext(control, context);
    return composing;
}
} // namespace

Window::Window(Store& store, bool smoke) : store_(store), config_(store.config()), smoke_(smoke) {
    background_ = CreateSolidBrush(palette(config_.dark).background);
    inputBackground_ = CreateSolidBrush(palette(config_.dark).background);
}
Window::~Window() {
    if (worker_.joinable()) worker_.join();
    PlaySoundW(nullptr, nullptr, 0);
    DeleteObject(font_); DeleteObject(headingFont_); DeleteObject(smallFont_);
    DeleteObject(background_); DeleteObject(inputBackground_);
}
HWND Window::create(HINSTANCE instance, bool background) {
    instance_ = instance;
    WNDCLASSEXW klass{};
    klass.cbSize = sizeof(klass); klass.hInstance = instance;
    klass.lpfnWndProc = procedure; klass.lpszClassName = WindowClass;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    klass.hIconSm = klass.hIcon;
    if (!RegisterClassExW(&klass)) throw Error("无法注册窗口。", ErrorKind::Storage);
    const auto systemDpi = GetDpiForSystem();
    RECT bounds{0, 0, scaled(420, systemDpi), scaled(280, systemDpi)};
    const auto position = store_.windowPosition();
    handle_ = CreateWindowExW(0, WindowClass, L"MemoQuick", WindowStyle,
        position ? position->x : CW_USEDEFAULT, position ? position->y : CW_USEDEFAULT,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, this);
    if (!handle_) throw Error("无法创建窗口。", ErrorKind::Storage);
    // Apply the full-client frame before the first show, including a fresh startup.
    const DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(handle_, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
    SetWindowPos(handle_, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ensurePositionVisible();
    positionReady_ = true;
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    trayIcon(handle_, instance_, NIM_ADD);
    hotkeyRegistered_ = smoke_ || registerHotkey(config_.modifiers, config_.key);
    if (!hotkeyRegistered_) setNotice("快捷键已被占用，请在设置中换一个组合。");
    // A manual launch after moving/upgrading the portable executable repairs its Run path.
    // Do not rewrite Run while Windows is executing the background startup entry.
    if (!smoke_ && !background && config_.startAtLogin) {
        try { setStartup(true); }
        catch (const std::exception& error) { setNotice(error.what()); }
    }
    if (!background) show();
    if (!smoke_ && !config_.server.empty() && !store_.cookies(config_.server).empty()) restore();
    return handle_;
}
void Window::show() {
    ShowWindow(handle_, smoke_ ? SW_SHOWNOACTIVATE : SW_RESTORE);
    ensurePositionVisible();
    if (!smoke_) SetForegroundWindow(handle_);
    if (view_ == View::Compose && !busy_) SetFocus(control(Editor));
    if (resumePending_ && !busy_ && view_ == View::Login) restore();
}
void Window::ensurePositionVisible() {
    if (IsIconic(handle_) || IsZoomed(handle_)) return;
    RECT bounds{}; if (!GetWindowRect(handle_, &bounds)) return;
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const auto& work = monitor.rcWork;
    const LONG x = std::clamp(bounds.left, work.left, std::max(work.left, work.right - (bounds.right - bounds.left)));
    const LONG y = std::clamp(bounds.top, work.top, std::max(work.top, work.bottom - (bounds.bottom - bounds.top)));
    if (x != bounds.left || y != bounds.top)
        SetWindowPos(handle_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
void Window::rememberPosition() {
    KillTimer(handle_, PositionTimer);
    if (!positionReady_ || IsIconic(handle_) || IsZoomed(handle_)) return;
    RECT bounds{};
    if (GetWindowRect(handle_, &bounds)) store_.saveWindowPosition({bounds.left, bounds.top});
}
bool Window::flushDraft() {
    KillTimer(handle_, DraftTimer);
    if (!draftDirty_ || user_.name.empty()) return true;
    try {
        store_.saveDraft(config_.server, user_.name, currentDraft());
        draftDirty_ = false;
        return true;
    } catch (const std::exception& error) { setNotice(error.what()); return false; }
}
void Window::hide() {
    stopLocation();
    if (view_ == View::Record && (recorder_.active() || !recording_.empty())) finishRecording(true);
    if (flushDraft()) { rememberPosition(); ShowWindow(handle_, SW_HIDE); }
}
HWND Window::control(int id) const { const auto it = controls_.find(id); return it == controls_.end() ? nullptr : it->second; }
std::wstring Window::text(int id) const {
    const auto input = control(id);
    std::wstring result(static_cast<size_t>(GetWindowTextLengthW(input)) + 1, L'\0');
    GetWindowTextW(input, result.data(), static_cast<int>(result.size()));
    result.resize(wcslen(result.c_str()));
    return result;
}
void Window::text(int id, const std::wstring& value) { SetWindowTextW(control(id), value.c_str()); }
void Window::place(int id, int x, int y, int width, int height) {
    MoveWindow(control(id), scaled(x, dpi_), scaled(y, dpi_), scaled(width, dpi_), scaled(height, dpi_), TRUE);
}
void Window::fonts() {
    const auto normal = CreateFontW(-scaled(14, dpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    const auto heading = CreateFontW(-scaled(14, dpi_), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    const auto captionFont = CreateFontW(-scaled(12, dpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (const auto& [id, window] : controls_) {
        const auto font = id == Heading ? heading : (id == Subtitle || id == AccountLabel || id == PrivacyLabel || id == HotkeyHint || id == DraftHint ? captionFont : normal);
        SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    DeleteObject(font_); DeleteObject(headingFont_); DeleteObject(smallFont_);
    font_ = normal; headingFont_ = heading; smallFont_ = captionFont;
}
void Window::applyTheme() {
    const auto colors = palette(config_.dark);
    DeleteObject(background_); DeleteObject(inputBackground_);
    background_ = CreateSolidBrush(colors.background);
    inputBackground_ = CreateSolidBrush(colors.background);
    const BOOL dark = config_.dark;
    DwmSetWindowAttribute(handle_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    text(ThemeButton, config_.dark ? L"Dark" : L"Light");
    RedrawWindow(handle_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}
void Window::toggleTheme() {
    auto next = config_; next.dark = !next.dark;
    store_.saveConfig(next);
    config_ = next;
    applyTheme();
    if (view_ == View::Compose) SetFocus(control(Editor));
}
void Window::markdown(MarkdownAction action) {
    if (view_ != View::Compose || busy_ || imeComposing(control(Editor))) return;
    DWORD start = 0, end = 0;
    SendMessageW(control(Editor), EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    const auto content = text(Editor);
    const auto edit = formatMarkdown(content, start, end, action);
    if (content.size() - (edit.end - edit.start) + edit.replacement.size() > 500000)
        throw Error("内容已达到长度上限。", ErrorKind::Validation);
    SendMessageW(control(Editor), EM_SETSEL, edit.start, static_cast<LPARAM>(edit.end));
    SendMessageW(control(Editor), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(edit.replacement.c_str()));
    SendMessageW(control(Editor), EM_SETSEL, edit.selectionStart, static_cast<LPARAM>(edit.selectionEnd));
    SetFocus(control(Editor));
    SendMessageW(control(Editor), EM_SCROLLCARET, 0, 0);
}
void Window::createControls() {
    const auto add = [&](int id, const wchar_t* klass, const wchar_t* textValue, DWORD style = 0, DWORD extended = 0) {
        const auto child = CreateWindowExW(extended, klass, textValue, WS_CHILD | style, 0, 0, 0, 0,
            handle_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (!child) throw Error("无法创建输入控件。", ErrorKind::Storage);
        controls_[id] = child;
    };
    add(Heading, L"STATIC", L"MemoQuick");
    add(ThemeButton, L"BUTTON", L"Light", BS_OWNERDRAW | WS_TABSTOP);
    add(CloseButton, L"BUTTON", L"×", BS_OWNERDRAW | WS_TABSTOP);
    add(SettingsButton, L"BUTTON", L"···", BS_OWNERDRAW | WS_TABSTOP);
    add(ServerLabel, L"STATIC", L"服务地址");
    add(ServerInput, L"EDIT", wide(config_.server).c_str(), ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER);
    add(UsernameLabel, L"STATIC", L"用户名");
    add(UsernameInput, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER);
    add(PasswordLabel, L"STATIC", L"密码");
    add(PasswordInput, L"EDIT", L"", ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP | WS_BORDER);
    add(LoginButton, L"BUTTON", L"登录", BS_OWNERDRAW | WS_TABSTOP);
    add(ResumeButton, L"BUTTON", L"恢复登录", BS_OWNERDRAW | WS_TABSTOP);
    add(Editor, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL | WS_TABSTOP);
    SetWindowSubclass(control(Editor), [](HWND input, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) -> LRESULT {
        if (message == WM_MOUSEWHEEL) {
            const auto self = reinterpret_cast<Window*>(data);
            self->wheelDelta_ += GET_WHEEL_DELTA_WPARAM(wparam);
            const int steps = self->wheelDelta_ / WHEEL_DELTA;
            self->wheelDelta_ %= WHEEL_DELTA;
            UINT lines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            if (lines == WHEEL_PAGESCROLL) {
                for (int i = 0; i < std::abs(steps); ++i) SendMessageW(input, EM_SCROLL, steps > 0 ? SB_PAGEUP : SB_PAGEDOWN, 0);
            } else SendMessageW(input, EM_LINESCROLL, 0, -steps * static_cast<int>(lines));
            return 0;
        }
        return DefSubclassProc(input, message, wparam, lparam);
    }, 1, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(control(Editor), EM_SETLIMITTEXT, 500000, 0);
    add(Visibility, L"BUTTON", L"仅自己 ▾", BS_OWNERDRAW | WS_TABSTOP);
    add(SaveButton, L"BUTTON", L"保存", BS_OWNERDRAW | WS_TABSTOP);
    add(AccountLabel, L"STATIC", L"", SS_PATHELLIPSIS);
    add(BrowserButton, L"BUTTON", L"打开 Memos", BS_OWNERDRAW | WS_TABSTOP);
    add(NoticeLabel, L"STATIC", L"");
    add(HotkeyLabel, L"STATIC", L"快捷键");
    add(HotkeyInput, HOTKEY_CLASSW, L"", WS_TABSTOP);
    SetWindowSubclass(control(HotkeyInput), [](HWND input, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) -> LRESULT {
        const auto self = reinterpret_cast<Window*>(data);
        if (message == WM_PAINT || message == WM_PRINTCLIENT) {
            PAINTSTRUCT paint{};
            const auto dc = message == WM_PAINT ? BeginPaint(input, &paint) : reinterpret_cast<HDC>(wparam);
            RECT rect{}; GetClientRect(input, &rect);
            FillRect(dc, &rect, self->inputBackground_);
            const auto colors = palette(self->config_.dark);
            const auto outline = CreateSolidBrush(colors.line); FrameRect(dc, &rect, outline); DeleteObject(outline);
            const auto value = static_cast<WORD>(SendMessageW(input, HKM_GETHOTKEY, 0, 0));
            unsigned modifiers = 0;
            if (HIBYTE(value) & HOTKEYF_CONTROL) modifiers |= MOD_CONTROL;
            if (HIBYTE(value) & HOTKEYF_ALT) modifiers |= MOD_ALT;
            if (HIBYTE(value) & HOTKEYF_SHIFT) modifiers |= MOD_SHIFT;
            const auto label = LOBYTE(value) ? hotkeyLabel(modifiers, LOBYTE(value)) : L"按下快捷键";
            const auto font = SelectObject(dc, self->font_);
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, colors.ink);
            rect.left += scaled(6, self->dpi_);
            DrawTextW(dc, label.c_str(), -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, font);
            if (GetFocus() == input) { GetClientRect(input, &rect); InflateRect(&rect, -2, -2); DrawFocusRect(dc, &rect); }
            if (message == WM_PAINT) EndPaint(input, &paint);
            return 0;
        }
        if (message == WM_ERASEBKGND) return 1;
        const auto result = DefSubclassProc(input, message, wparam, lparam);
        if (message == WM_KEYUP || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == HKM_SETHOTKEY)
            InvalidateRect(input, nullptr, TRUE);
        return result;
    }, 1, reinterpret_cast<DWORD_PTR>(this));
    add(StartupInput, L"BUTTON", L"开机启动", BS_OWNERDRAW | WS_TABSTOP);
    add(SettingsSaveButton, L"BUTTON", L"完成", BS_OWNERDRAW | WS_TABSTOP);
    add(BackButton, L"BUTTON", L"返回", BS_OWNERDRAW | WS_TABSTOP);
    add(LogoutButton, L"BUTTON", L"退出登录", BS_OWNERDRAW | WS_TABSTOP);
    add(AddButton, L"BUTTON", L"＋", BS_OWNERDRAW | WS_TABSTOP);
    add(MetadataButton, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP);
    add(SearchInput, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER);
    add(SearchButton, L"BUTTON", L"搜索", BS_OWNERDRAW | WS_TABSTOP);
    add(MemoList, L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE);
    add(MoreButton, L"BUTTON", L"更多", BS_OWNERDRAW | WS_TABSTOP);
    add(LocationLabel, L"STATIC", L"名称");
    add(LocateButton, L"BUTTON", L"当前位置", BS_OWNERDRAW | WS_TABSTOP);
    add(LocationSettingsButton, L"BUTTON", L"定位设置", BS_OWNERDRAW | WS_TABSTOP);
    add(LocationName, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER);
    add(LatitudeLabel, L"STATIC", L"纬度");
    add(LatitudeInput, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER);
    add(LongitudeLabel, L"STATIC", L"经度");
    add(LongitudeInput, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER);
    add(PanelConfirm, L"BUTTON", L"添加", BS_OWNERDRAW | WS_TABSTOP);
    add(RecordButton, L"BUTTON", L"开始录音", BS_OWNERDRAW | WS_TABSTOP);
    add(RecordTime, L"STATIC", L"00:00", SS_CENTER);
    add(PreviewButton, L"BUTTON", L"试听", BS_OWNERDRAW | WS_TABSTOP);
    for (const auto id : {SearchInput, LocationName}) SendMessageW(control(id), EM_SETLIMITTEXT, 250, 0);
    for (const auto id : {LatitudeInput, LongitudeInput}) SendMessageW(control(id), EM_SETLIMITTEXT, 32, 0);
    const wchar_t* markdownIcons[] = {L"B", L"I", L"H", L"↗", L"•", L"1.", L"☐", L">", L"`", L"</>"};
    for (int id = MdBold; id <= MdCodeBlock; ++id)
        add(id, L"BUTTON", markdownIcons[id - MdBold], BS_OWNERDRAW | WS_TABSTOP);
    const auto tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, handle_, nullptr, instance_, nullptr);
    for (const auto& [id, tip] : std::map<int, const wchar_t*>{{ThemeButton, L"切换 Light / Dark"},
            {SettingsButton, L"设置"}, {CloseButton, L"收起 · Esc"}, {SaveButton, L"保存 · Ctrl + Enter"},
            {AddButton, L"添加附件、图片、录音、关联或位置"}, {MetadataButton, L"管理附件、关联与位置"},
            {MdBold, L"加粗 · Ctrl + B"}, {MdItalic, L"斜体 · Ctrl + I"}, {MdHeading, L"标题 · Ctrl + Shift + H"},
            {MdLink, L"链接 · Ctrl + K"}, {MdBullet, L"无序列表 · Ctrl + Shift + L"}, {MdNumbered, L"有序列表 · Ctrl + Shift + O"},
            {MdTask, L"待办 · Ctrl + Shift + T"}, {MdQuote, L"引用 · Ctrl + Shift + Q"},
            {MdInlineCode, L"行内代码 · Ctrl + E"}, {MdCodeBlock, L"代码块 · Ctrl + Shift + C"}}) {
        TOOLINFOW tool{}; tool.cbSize = sizeof(tool); tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = handle_; tool.uId = reinterpret_cast<UINT_PTR>(control(id)); tool.lpszText = const_cast<wchar_t*>(tip);
        SendMessageW(tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
    SendMessageW(control(ServerInput), EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"https://memos.example.com"));
    dpi_ = GetDpiForWindow(handle_);
    fonts(); applyTheme();
    setView(View::Login);
}
void Window::layout() {
    if (!handle_ || controls_.empty()) return;
    RECT bounds{}; GetClientRect(handle_, &bounds);
    const int width = MulDiv(bounds.right, 96, static_cast<int>(dpi_));
    const int height = MulDiv(bounds.bottom, 96, static_cast<int>(dpi_));
    const int x = 12, contentWidth = width - 24;
    place(Heading, x, 9, contentWidth - 126, 20);
    place(ThemeButton, width - 120, 4, 52, 26);
    place(SettingsButton, width - 66, 4, 28, 26);
    place(CloseButton, width - 36, 4, 28, 26);
    place(NoticeLabel, x, height - 77, contentWidth, 34);
    const int noticeHeight = notice_.empty() ? 0 : 40;
    if (view_ == View::Login) {
        place(ServerLabel, x, 44, contentWidth, 20);
        place(ServerInput, x, 68, contentWidth, 28);
        const int fieldWidth = (contentWidth - 12) / 2;
        place(UsernameLabel, x, 110, fieldWidth, 20);
        place(UsernameInput, x, 134, fieldWidth, 28);
        place(PasswordLabel, x + fieldWidth + 12, 110, fieldWidth, 20);
        place(PasswordInput, x + fieldWidth + 12, 134, fieldWidth, 28);
        place(LoginButton, width - 88, height - 40, 76, 28);
        place(ResumeButton, x, height - 40, 84, 28);
    } else if (view_ == View::Compose) {
        for (int id = MdBold; id <= MdCodeBlock; ++id) place(id, x - 4 + (id - MdBold) * 30, 34, 28, 24);
        const int editorHeight = std::max(80, height - 110 - noticeHeight);
        place(Editor, x, 64, contentWidth, editorHeight);
        RECT editRect{0, scaled(4, dpi_), scaled(contentWidth, dpi_), scaled(editorHeight - 4, dpi_)};
        SendMessageW(control(Editor), EM_SETRECT, 0, reinterpret_cast<LPARAM>(&editRect));
        place(Visibility, x - 4, height - 39, 100, 28);
        place(AddButton, x + 96, height - 39, 28, 28);
        place(MetadataButton, x + 128, height - 39, std::max(40, width - 224), 28);
        place(SaveButton, width - 80, height - 39, 68, 28);
    } else if (view_ == View::Settings) {
        place(HotkeyLabel, x, 48, 72, 22);
        place(HotkeyInput, x + 82, 44, contentWidth - 82, 28);
        place(StartupInput, x, 84, 130, 28);
        place(AccountLabel, x, 128, contentWidth, 22);
        place(BrowserButton, x - 4, 158, 106, 28);
        place(LogoutButton, x + 108, 158, 88, 28);
        place(BackButton, width - 160, height - 40, 64, 28);
        place(SettingsSaveButton, width - 88, height - 40, 76, 28);
    } else {
        place(BackButton, x - 4, height - 40, 60, 28);
        place(PanelConfirm, width - 88, height - 40, 76, 28);
        if (view_ == View::Link) {
            place(SearchInput, x, 44, contentWidth - 68, 28);
            place(SearchButton, width - 76, 44, 64, 28);
            place(MemoList, x, 82, contentWidth, std::max(60, height - 130 - noticeHeight));
            place(MoreButton, width - 158, height - 40, 60, 28);
        } else if (view_ == View::Location) {
            place(LocationLabel, x, 44, contentWidth - 84, 20); place(LocationName, x, 68, contentWidth, 28);
            place(LocateButton, width - 88, 38, 76, 26);
            place(LocationSettingsButton, x + 60, height - 40, 76, 28);
            const int field = (contentWidth - 12) / 2;
            place(LatitudeLabel, x, 110, field, 20); place(LatitudeInput, x, 134, field, 28);
            place(LongitudeLabel, x + field + 12, 110, field, 20); place(LongitudeInput, x + field + 12, 134, field, 28);
        } else {
            place(RecordTime, x, 76, contentWidth, 28);
            place(RecordButton, width / 2 - 92, 126, 112, 28);
            place(PreviewButton, width / 2 + 28, 126, 64, 28);
        }
    }
    InvalidateRect(handle_, nullptr, TRUE);
}
void Window::setView(View view) {
    stopLocation();
    view_ = view;
    if (view != View::Compose) {
        RECT bounds{}; GetWindowRect(handle_, &bounds);
        if (bounds.bottom - bounds.top < scaled(270, dpi_))
            SetWindowPos(handle_, nullptr, 0, 0, bounds.right - bounds.left, scaled(270, dpi_), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    for (const auto& [id, window] : controls_)
        if (id != Heading && id != ThemeButton && id != SettingsButton && id != CloseButton) ShowWindow(window, SW_HIDE);
    const auto visible = [&](std::initializer_list<int> ids) { for (const auto id : ids) ShowWindow(control(id), SW_SHOW); };
    visible({Heading, ThemeButton, SettingsButton, CloseButton});
    if (!notice_.empty()) visible({NoticeLabel});
    text(Heading, view == View::Settings ? L"设置" : view == View::Link ? L"关联 Memo" : view == View::Location ? L"位置" : view == View::Record ? L"录音" : L"MemoQuick");
    text(BackButton, view == View::Settings ? L"返回" : L"取消");
    if (view == View::Login) {
        visible({ServerLabel, ServerInput, UsernameLabel, UsernameInput, PasswordLabel, PasswordInput, LoginButton});
        if (!config_.server.empty()) visible({ResumeButton});
    } else if (view == View::Compose) {
        visible({Editor, Visibility, SaveButton, AddButton, MetadataButton});
        updateMetadata();
        for (int id = MdBold; id <= MdCodeBlock; ++id) ShowWindow(control(id), SW_SHOW);
    } else if (view == View::Settings) {
        visible({HotkeyLabel, HotkeyInput, StartupInput, SettingsSaveButton, BackButton, AccountLabel});
        if (!user_.name.empty() || !config_.server.empty()) visible({LogoutButton, BrowserButton});
        BYTE modifiers = 0;
        if (config_.modifiers & MOD_CONTROL) modifiers |= HOTKEYF_CONTROL;
        if (config_.modifiers & MOD_ALT) modifiers |= HOTKEYF_ALT;
        if (config_.modifiers & MOD_SHIFT) modifiers |= HOTKEYF_SHIFT;
        SendMessageW(control(HotkeyInput), HKM_SETHOTKEY, MAKEWORD(static_cast<BYTE>(config_.key), modifiers), 0);
        startupChecked_ = config_.startAtLogin;
        text(AccountLabel, wide(config_.server));
    } else if (view == View::Link) {
        visible({SearchInput, SearchButton, MemoList, MoreButton, BackButton, PanelConfirm});
        EnableWindow(control(MoreButton), !memoPage_.nextPage.empty());
    } else if (view == View::Location) {
        visible({LocationLabel, LocationName, LatitudeLabel, LatitudeInput, LongitudeLabel, LongitudeInput, BackButton, PanelConfirm, LocateButton});
        text(LocationName, extras_.location ? wide(extras_.location->placeholder) : L"");
        text(LatitudeInput, extras_.location ? coordinateText(extras_.location->latitude) : L"");
        text(LongitudeInput, extras_.location ? coordinateText(extras_.location->longitude) : L"");
    } else if (view == View::Record) {
        visible({RecordTime, RecordButton, PreviewButton, BackButton, PanelConfirm});
        text(RecordButton, recording_.empty() ? L"开始录音" : L"重新录制");
        text(RecordTime, recording_.empty() ? L"00:00" : L"已停止");
        EnableWindow(control(PreviewButton), !recording_.empty());
    }
    layout();
    applyTheme();
    if (view == View::Compose) SetFocus(control(Editor));
}
void Window::chooseVisibility() {
    const auto menu = CreatePopupMenu();
    const wchar_t* scopes[] = {L"仅自己", L"已登录用户", L"公开"};
    for (int i = 0; i < 3; ++i) AppendMenuW(menu, MF_STRING | (visibility_ == i ? MF_CHECKED : 0), static_cast<UINT_PTR>(i + 1), scopes[i]);
    RECT rect{}; GetWindowRect(control(Visibility), &rect);
    const auto command = trackMenu(menu, TPM_LEFTALIGN | TPM_BOTTOMALIGN, rect.left, rect.top);
    if (command) {
        visibility_ = command - 1;
        text(Visibility, std::wstring(scopes[visibility_]) + L" ▾");
        draftDirty_ = true; SetTimer(handle_, DraftTimer, 400, nullptr);
    }
}
void Window::setBusy(bool busy) {
    busy_ = busy;
    for (int id = MdBold; id <= MdCodeBlock; ++id) EnableWindow(control(id), !busy);
    for (const auto id : {ServerInput, UsernameInput, PasswordInput, LoginButton, ResumeButton, Visibility,
            SaveButton, SettingsButton, ThemeButton, HotkeyInput, StartupInput, SettingsSaveButton, BackButton, LogoutButton}) {
        EnableWindow(control(id), !busy);
    }
    SendMessageW(control(Editor), EM_SETREADONLY, busy ? TRUE : FALSE, 0);
    for (const auto id : {AddButton, MetadataButton, SearchInput, SearchButton, MemoList, PanelConfirm, RecordButton, PreviewButton}) EnableWindow(control(id), !busy);
    for (const auto id : {LocateButton, LocationSettingsButton, LatitudeInput, LongitudeInput}) EnableWindow(control(id), !busy);
    EnableWindow(control(MoreButton), !busy && !memoPage_.nextPage.empty());
    updateMetadata();
    text(LoginButton, busy ? L"连接中…" : L"登录");
    text(SaveButton, busy ? L"保存中…" : L"保存");
}
void Window::setNotice(const std::string& notice, bool informational) {
    notice_ = notice;
    noticeInformational_ = informational;
    text(NoticeLabel, wide(notice));
    ShowWindow(control(NoticeLabel), notice.empty() ? SW_HIDE : SW_SHOW);
    layout();
}
Draft Window::currentDraft() const {
    const auto index = visibility_;
    auto draft = extras_;
    draft.content = utf8(text(Editor)); draft.visibility = index == 1 ? "PROTECTED" : index == 2 ? "PUBLIC" : "PRIVATE";
    return draft;
}
void Window::enterCompose() {
    user_ = client_->user();
    const auto draft = store_.draft(config_.server, user_.name);
    extras_ = draft;
    settingEditor_ = true;
    text(Editor, wide(draft.content));
    visibility_ = draft.visibility == "PROTECTED" ? 1 : draft.visibility == "PUBLIC" ? 2 : 0;
    text(Visibility, visibility_ == 1 ? L"已登录用户 ▾" : visibility_ == 2 ? L"公开 ▾" : L"仅自己 ▾");
    settingEditor_ = false;
    draftDirty_ = false;
    updateMetadata();
    setView(View::Compose);
    SetFocus(control(Editor));
}
void Window::launch(Action action, std::function<void(Completion&)> work) {
    if (busy_) return;
    setNotice(""); setBusy(true);
    if (action == Action::Upload) text(SaveButton, L"上传中…");
    if (worker_.joinable()) worker_.join();
    worker_ = std::jthread([this, action, work = std::move(work)] {
        auto result = std::make_unique<Completion>(); result->action = action;
        try { work(*result); }
        catch (const Error& error) { result->error = error.what(); result->errorKind = error.kind; result->status = error.status; }
        catch (const std::exception&) { result->error = "操作未能完成，请检查本地数据与服务状态；草稿已保留。"; }
        const auto pointer = result.release();
        if (!PostMessageW(handle_, CompleteMessage, 0, reinterpret_cast<LPARAM>(pointer))) delete pointer;
    });
}
void Window::complete(std::unique_ptr<Completion> result) {
    if (worker_.joinable()) worker_.join();
    setBusy(false);
    if (result->client) { client_ = std::move(result->client); config_ = store_.config(); }
    try {
        if (result->draft) {
            extras_ = *result->draft;
            const auto previous = text(Editor), next = wide(extras_.content);
            if (previous != next) {
                // Preserve the native edit control's undo record for uploaded images.
                size_t start = 0, suffix = 0;
                while (start < previous.size() && start < next.size() && previous[start] == next[start]) ++start;
                while (suffix < previous.size() - start && suffix < next.size() - start
                    && previous[previous.size() - suffix - 1] == next[next.size() - suffix - 1]) ++suffix;
                const auto replacement = next.substr(start, next.size() - start - suffix);
                settingEditor_ = true;
                SendMessageW(control(Editor), EM_SETSEL, start, static_cast<LPARAM>(previous.size() - suffix));
                SendMessageW(control(Editor), EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(replacement.c_str()));
                settingEditor_ = false;
            }
            draftDirty_ = true; flushDraft(); updateMetadata();
            if (result->action == Action::Upload && IsWindowVisible(handle_)) {
                SendMessageW(control(Editor), EM_SETSEL, static_cast<WPARAM>(-1), -1);
                SetFocus(control(Editor));
            }
        }
        if (result->action == Action::Search && result->error.empty()) {
            for (const auto& memo : result->page.memos) {
                if (std::any_of(memoPage_.memos.begin(), memoPage_.memos.end(), [&](const auto& r) { return r.name == memo.name; })) continue;
                if (std::any_of(extras_.relations.begin(), extras_.relations.end(), [&](const auto& r) { return r.name == memo.name; })) continue;
                memoPage_.memos.push_back(memo);
                auto label = wide(memo.snippet.empty() ? memo.name : memo.snippet);
                std::replace(label.begin(), label.end(), L'\r', L' '); std::replace(label.begin(), label.end(), L'\n', L' ');
                SendMessageW(control(MemoList), LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
            memoPage_.nextPage = result->page.nextPage;
            EnableWindow(control(MoreButton), !memoPage_.nextPage.empty());
            if (memoPage_.memos.empty()) setNotice("没有找到匹配的 Memo。");
        }
        if (result->action == Action::Logout) {
            client_.reset(); user_ = {}; extras_ = {}; draftDirty_ = false;
            settingEditor_ = true; text(Editor, L""); settingEditor_ = false;
            setView(View::Login);
        } else if (result->error.empty() && (result->action == Action::Login || result->action == Action::Resume)) {
            KillTimer(handle_, ResumeTimer); resumePending_ = false; resumeAttempts_ = 0;
            enterCompose();
        } else if (result->action == Action::Save && result->saved) {
            extras_ = {}; updateMetadata();
            settingEditor_ = true; text(Editor, L""); settingEditor_ = false;
            visibility_ = 0; text(Visibility, L"仅自己 ▾");
            draftDirty_ = false;
            if (result->error.empty()) { rememberPosition(); ShowWindow(handle_, SW_HIDE); }
        }
        if (!result->error.empty()) {
            if (result->action == Action::Resume) {
                resumePending_ = result->errorKind == ErrorKind::Network || result->status == 429 || result->status >= 500;
                if (resumePending_) {
                    constexpr UINT delays[]{1000, 5000, 15000, 30000, 60000};
                    SetTimer(handle_, ResumeTimer, delays[std::min(resumeAttempts_, 4u)], nullptr);
                    if (resumeAttempts_ < 4) ++resumeAttempts_;
                }
            }
            if (result->errorKind == ErrorKind::Auth) { text(PasswordInput, L""); setView(View::Login); }
            setNotice(result->error);
        } else if (!hotkeyRegistered_) { setNotice("快捷键已被占用，请在设置中换一个组合。"); }
    } catch (const std::exception& error) { setNotice(error.what()); }
}
void Window::login() {
    if (busy_) return;
    const auto server = parseServer(utf8(text(ServerInput)));
    const auto username = utf8(text(UsernameInput));
    auto password = utf8(text(PasswordInput));
    if (username.empty() || password.empty()) throw Error("请填写用户名和密码。", ErrorKind::Validation);
    if (!flushDraft()) return;
    KillTimer(handle_, ResumeTimer); resumePending_ = false; resumeAttempts_ = 0;
    text(PasswordInput, L"");
    launch(Action::Login, [this, server, username, password = std::move(password)](Completion& result) mutable {
        auto next = std::make_unique<Client>(server, store_.cookies(server.url),
            [this, url = server.url](const auto& cookies) { store_.saveCookies(url, cookies); });
        next->signIn(username, password);
        SecureZeroMemory(password.data(), password.size()); password.clear();
        auto nextConfig = store_.config(); nextConfig.server = server.url; store_.saveConfig(nextConfig);
        result.client = std::move(next);
    });
}
void Window::restore() {
    if (busy_ || config_.server.empty() || !flushDraft()) return;
    const auto server = parseServer(config_.server);
    KillTimer(handle_, ResumeTimer);
    launch(Action::Resume, [this, server](Completion& result) {
        auto next = std::make_unique<Client>(server, store_.cookies(server.url),
            [this, url = server.url](const auto& cookies) { store_.saveCookies(url, cookies); });
        next->resume(); result.client = std::move(next);
    });
}
void Window::save() {
    if (busy_ || !client_ || user_.name.empty()) return;
    const auto draft = currentDraft();
    if (!validDraft(draft) || (trim(draft.content).empty() && draft.attachments.empty())) { setNotice("写点什么，或添加一个附件。 "); return; }
    draftDirty_ = true;
    if (!flushDraft()) return;
    launch(Action::Save, [this, draft](Completion& result) {
        result.draft = draft;
        prepareAttachments(*result.draft, wide(draft.content).size());
        client_->createMemo(*result.draft);
        result.saved = true;
        try { store_.saveDraft(client_->server().url, client_->user().name, {}); }
        catch (...) { throw Error("记录已保存，但旧草稿清理失败。请检查磁盘，不要重复提交旧草稿。", ErrorKind::Storage); }
        for (const auto& a : result.draft->attachments) if (!a.localId.empty()) store_.removeAttachment(a.localId);
        result.draft.reset();
    });
}
void Window::logout() {
    if (busy_ || !flushDraft()) return;
    KillTimer(handle_, ResumeTimer); resumePending_ = false; resumeAttempts_ = 0;
    launch(Action::Logout, [this](Completion&) {
        if (client_) client_->signOut();
        else if (!config_.server.empty()) store_.saveCookies(config_.server, {});
    });
}
bool Window::registerHotkey(unsigned modifiers, unsigned key, bool alternate) {
    if (!(modifiers & (MOD_CONTROL | MOD_ALT | MOD_WIN)) || !key) return false;
    return RegisterHotKey(handle_, alternate ? (hotkeyId_ == 1 ? 2 : 1) : hotkeyId_, modifiers | MOD_NOREPEAT, key) != FALSE;
}
void Window::settingsSave() {
    if (busy_) return;
    const auto value = static_cast<WORD>(SendMessageW(control(HotkeyInput), HKM_GETHOTKEY, 0, 0));
    const auto flags = HIBYTE(value);
    auto next = config_;
    next.key = LOBYTE(value); next.modifiers = 0;
    if (flags & HOTKEYF_CONTROL) next.modifiers |= MOD_CONTROL;
    if (flags & HOTKEYF_ALT) next.modifiers |= MOD_ALT;
    if (flags & HOTKEYF_SHIFT) next.modifiers |= MOD_SHIFT;
    next.startAtLogin = startupChecked_;
    const bool changed = next.modifiers != config_.modifiers || next.key != config_.key || !hotkeyRegistered_;
    if (changed && !registerHotkey(next.modifiers, next.key, true)) throw Error("快捷键无效或已被占用，请使用含 Ctrl 或 Alt 的其他组合。", ErrorKind::Validation);
    try {
        if (!smoke_) setStartup(next.startAtLogin);
        store_.saveConfig(next);
    } catch (...) {
        if (changed) UnregisterHotKey(handle_, hotkeyId_ == 1 ? 2 : 1);
        if (!smoke_) { try { setStartup(config_.startAtLogin); } catch (...) {} }
        throw;
    }
    if (changed) { UnregisterHotKey(handle_, hotkeyId_); hotkeyId_ = hotkeyId_ == 1 ? 2 : 1; hotkeyRegistered_ = true; }
    config_ = next; setNotice(""); setView(beforeSettings_);
    if (resumePending_ && view_ == View::Login) restore();
}
void Window::browser() {
    if (!config_.server.empty()) ShellExecuteW(handle_, L"open", wide(parseServer(config_.server).url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
void Window::trayMenu() {
    const auto menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"快速记录");
    AppendMenuW(menu, MF_STRING | (config_.server.empty() ? MF_GRAYED : 0), 2, L"打开 Memos");
    AppendMenuW(menu, MF_STRING, 3, L"设置");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 4, L"退出 MemoQuick");
    POINT point{}; GetCursorPos(&point); SetForegroundWindow(handle_);
    const auto command = trackMenu(menu, TPM_RIGHTBUTTON, point.x, point.y);
    if (command == 1) show();
    if (command == 2) browser();
    if (command == 3 && !busy_) {
        if (view_ == View::Record && (recorder_.active() || !recording_.empty())) { finishRecording(true); show(); return; }
        beforeSettings_ = view_; setView(View::Settings); setNotice(""); show();
    }
    if (command == 4) quit();
    PostMessageW(handle_, WM_NULL, 0, 0);
}
void Window::quit() {
    if (view_ == View::Record && (recorder_.active() || !recording_.empty())) finishRecording(true);
    if (busy_) { setNotice("正在处理请求，请完成后再退出。"); show(); return; }
    if (!flushDraft()) { show(); return; }
    rememberPosition();
    DestroyWindow(handle_);
}
bool Window::preprocess(MSG& msg) {
    if (msg.message == WM_KEYDOWN && (msg.hwnd == handle_ || IsChild(handle_, msg.hwnd))) {
        if (view_ == View::Compose && msg.hwnd == control(Editor) && !busy_ && !imeComposing(control(Editor))
            && (GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            int command = 0;
            if (!shift) {
                if (msg.wParam == 'B') command = MdBold;
                if (msg.wParam == 'I') command = MdItalic;
                if (msg.wParam == 'K') command = MdLink;
                if (msg.wParam == 'E') command = MdInlineCode;
            } else {
                if (msg.wParam == 'H') command = MdHeading;
                if (msg.wParam == 'L') command = MdBullet;
                if (msg.wParam == 'O') command = MdNumbered;
                if (msg.wParam == 'T') command = MdTask;
                if (msg.wParam == 'Q') command = MdQuote;
                if (msg.wParam == 'C') command = MdCodeBlock;
            }
            if (command) { markdown(static_cast<MarkdownAction>(command - MdBold)); return true; }
        }
        if (msg.wParam == VK_ESCAPE && !imeComposing(GetFocus())) { hide(); return true; }
        if (msg.wParam == VK_RETURN && !busy_ && !imeComposing(GetFocus())) {
            if (view_ == View::Compose && (GetKeyState(VK_CONTROL) & 0x8000)) { save(); return true; }
            if (view_ == View::Login) { login(); return true; }
            if (view_ == View::Link && msg.hwnd == control(SearchInput)) { searchMemos(); return true; }
            if (view_ == View::Link && msg.hwnd == control(MemoList)) { confirmPanel(); return true; }
        }
    }
    return IsDialogMessageW(handle_, &msg) != FALSE;
}
void Window::smokeLogin(const std::string& server) {
    text(ServerInput, wide(server)); text(UsernameInput, L"alice"); text(PasswordInput, L"test-password"); login();
}
LRESULT CALLBACK Window::procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) {
    auto self = reinterpret_cast<Window*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->handle_ = handle;
        SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) {
        try { return self->message(message, wparam, lparam); }
        catch (const std::exception& error) { self->setNotice(error.what()); return 0; }
    }
    return DefWindowProcW(handle, message, wparam, lparam);
}
LRESULT Window::message(UINT msg, WPARAM wparam, LPARAM lparam) {
    if (taskbarCreated_ && msg == taskbarCreated_) { trayIcon(handle_, instance_, NIM_ADD); return 0; }
    switch (msg) {
    case WM_CREATE: createControls(); return 0;
    case WM_SIZE: layout(); return 0;
    case WM_MOVE:
        if (positionReady_ && !IsIconic(handle_) && !IsZoomed(handle_)) SetTimer(handle_, PositionTimer, 300, nullptr);
        break;
    case WM_EXITSIZEMOVE: rememberPosition(); return 0;
    case WM_DISPLAYCHANGE: ensurePositionVisible(); return 0;
    case WM_NCCALCSIZE: return 0;
    case WM_NCACTIVATE:
        // Keep activation bookkeeping but suppress the default frame repaint.
        return DefWindowProcW(handle_, msg, wparam, -1);
    case WM_NCPAINT:
        // The entire window is client area; its one-pixel edge is painted in WM_PAINT.
        return 0;
    case WM_NCHITTEST: {
        RECT rect{}; GetWindowRect(handle_, &rect);
        const int x = GET_X_LPARAM(lparam) - rect.left, y = GET_Y_LPARAM(lparam) - rect.top;
        const int edge = scaled(5, dpi_);
        const bool left = x < edge, right = x >= rect.right - rect.left - edge;
        const bool top = y < edge, bottom = y >= rect.bottom - rect.top - edge;
        if (top) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
        if (bottom) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (y < scaled(33, dpi_) && x < rect.right - rect.left - scaled(126, dpi_)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_ERASEBKGND: {
        RECT rect{}; GetClientRect(handle_, &rect);
        FillRect(reinterpret_cast<HDC>(wparam), &rect, background_); return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{}; const auto dc = BeginPaint(handle_, &paint);
        RECT rect{}; GetClientRect(handle_, &rect);
        const auto brush = CreateSolidBrush(palette(config_.dark).line);
        FrameRect(dc, &rect, brush); DeleteObject(brush);
        EndPaint(handle_, &paint); return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wparam); fonts();
        const auto rect = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(handle_, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout(); return 0;
    }
    case WM_GETMINMAXINFO: {
        const auto info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize = {scaled(340, dpi_), scaled(view_ == View::Compose ? 240 : 270, dpi_)}; return 0;
    }
    case WM_CLOSE: hide(); return 0;
    case WM_QUERYENDSESSION:
        if (recorder_.active() || !recording_.empty()) return FALSE;
        // Avoid losing an in-flight write; Windows may still force termination.
        if (busy_ || !flushDraft()) return FALSE;
        rememberPosition(); return TRUE;
    case WM_ENDSESSION: if (wparam) { flushDraft(); rememberPosition(); DestroyWindow(handle_); } return 0;
    case WM_DESTROY:
        stopLocation();
        KillTimer(handle_, TrayTimer); KillTimer(handle_, ResumeTimer);
        UnregisterHotKey(handle_, 1); UnregisterHotKey(handle_, 2);
        trayIcon(handle_, instance_, NIM_DELETE); PostQuitMessage(0); return 0;
    case WM_HOTKEY:
        if (IsWindowVisible(handle_) && !IsIconic(handle_)) hide();
        else show();
        return 0;
    case ShowMessage: show(); return 0;
    case TrayMessage:
        if (LOWORD(lparam) == WM_LBUTTONUP || LOWORD(lparam) == NIN_SELECT) show();
        if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_CONTEXTMENU) trayMenu();
        return 0;
    case CompleteMessage: complete(std::unique_ptr<Completion>(reinterpret_cast<Completion*>(lparam))); return 0;
    case WM_TIMER:
        if (wparam == DraftTimer) flushDraft();
        if (wparam == PositionTimer) rememberPosition();
        if (wparam == 3) recordTick();
        if (wparam == TrayTimer) trayIcon(handle_, instance_, NIM_ADD);
        if (wparam == ResumeTimer && resumePending_ && !busy_ && view_ == View::Login) restore();
        if (wparam == LocationTimer) locationTick();
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wparam), event = HIWORD(wparam);
        if (!settingEditor_ && !busy_ && ((id == Editor && event == EN_CHANGE) || (id == Visibility && event == CBN_SELCHANGE))) {
            draftDirty_ = true; SetTimer(handle_, DraftTimer, 400, nullptr);
        }
        if (event != BN_CLICKED || busy_) return 0;
        if (id == AddButton) insertMenu();
        if (id == MetadataButton) metadataMenu();
        if (id == InsertAttachment || id == InsertImage) chooseFiles(id == InsertImage);
        if (id == RetryAttachments) uploadPending();
        if (id == InsertMemo) { setNotice(""); text(SearchInput, L""); setView(View::Link); searchMemos(); }
        if (id == InsertLocation) { setNotice(""); setView(View::Location); }
        if (id == InsertAudio) { setNotice(""); setView(View::Record); }
        if (id == SearchButton) searchMemos();
        if (id == MoreButton) searchMemos(true);
        if (id == PanelConfirm) confirmPanel();
        if (id == LocateButton) locate();
        if (id == LocationSettingsButton) {
            const auto opened = ShellExecuteW(handle_, L"open", L"ms-settings:privacy-location", nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(opened) <= 32) setNotice("无法打开定位设置，请在 Windows 设置中搜索「位置」。");
        }
        if (id == RecordButton) record();
        if (id == PreviewButton && !recording_.empty()) PlaySoundW(reinterpret_cast<LPCWSTR>(recording_.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
        if (id >= MdBold && id <= MdCodeBlock) markdown(static_cast<MarkdownAction>(id - MdBold));
        if (id == LoginButton) login();
        if (id == ResumeButton) restore();
        if (id == SaveButton) save();
        if (id == BrowserButton) browser();
        if (id == CloseButton) hide();
        if (id == ThemeButton) toggleTheme();
        if (id == Visibility) chooseVisibility();
        if (id == StartupInput) { startupChecked_ = !startupChecked_; InvalidateRect(control(StartupInput), nullptr, TRUE); }
        if (id == SettingsButton) {
            if (view_ == View::Record && (recorder_.active() || !recording_.empty())) { finishRecording(true); return 0; }
            setNotice("");
            if (view_ == View::Settings) setView(beforeSettings_);
            else { beforeSettings_ = view_; setView(View::Settings); }
        }
        if (id == BackButton) {
            if (view_ == View::Record) { PlaySoundW(nullptr, nullptr, 0); recorder_.cancel(); recording_.clear(); KillTimer(handle_,3); }
            setNotice(""); setView(view_ == View::Settings ? beforeSettings_ : View::Compose);
        }
        if (id == SettingsSaveButton) settingsSave();
        if (id == LogoutButton) logout();
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        const auto dc = reinterpret_cast<HDC>(wparam);
        const auto id = GetDlgCtrlID(reinterpret_cast<HWND>(lparam));
        const auto colors = palette(config_.dark);
        SetTextColor(dc, id == NoticeLabel ? (noticeInformational_ ? colors.muted : colors.error) : id == AccountLabel ? colors.muted : colors.ink);
        SetBkColor(dc, colors.background);
        return reinterpret_cast<LRESULT>(msg == WM_CTLCOLOREDIT ? inputBackground_ : background_);
    }
    case WM_MEASUREITEM: {
        const auto item = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
        if (item->CtlType == ODT_MENU && item->itemData) { measureMenu(*item); return TRUE; }
        break;
    }
    case WM_DRAWITEM: {
        const auto item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (item->CtlType == ODT_MENU && item->itemData) { drawMenu(*item); return TRUE; }
        const auto dc = item->hDC;
        const auto colors = palette(config_.dark);
        const bool disabled = item->itemState & ODS_DISABLED;
        const bool pressed = item->itemState & ODS_SELECTED;
        const bool primary = item->CtlID == SaveButton || item->CtlID == LoginButton || item->CtlID == SettingsSaveButton || item->CtlID == PanelConfirm;
        const auto brush = CreateSolidBrush(primary ? (disabled ? colors.muted : colors.accent) : pressed ? colors.hover : colors.background);
        FillRect(dc, &item->rcItem, background_);
        const auto oldBrush = SelectObject(dc, brush); const auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, item->rcItem.left, item->rcItem.top, item->rcItem.right, item->rcItem.bottom, scaled(6, dpi_), scaled(6, dpi_));
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush);
        const bool markdownTool = item->CtlID >= MdBold && item->CtlID <= MdCodeBlock;
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, primary ? RGB(255,255,255) : (disabled || markdownTool) ? colors.muted : colors.ink);
        const auto oldFont = SelectObject(dc, font_);
        auto rect = item->rcItem; const auto caption = text(static_cast<int>(item->CtlID));
        if (item->CtlID == StartupInput) {
            RECT box{rect.left + scaled(3, dpi_), rect.top + scaled(7, dpi_), rect.left + scaled(17, dpi_), rect.top + scaled(21, dpi_)};
            const auto outline = CreateSolidBrush(colors.muted); FrameRect(dc, &box, outline); DeleteObject(outline);
            if (startupChecked_) DrawTextW(dc, L"✓", -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            rect.left += scaled(25, dpi_);
            DrawTextW(dc, caption.c_str(), -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else DrawTextW(dc, caption.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (item->itemState & ODS_FOCUS) { InflateRect(&rect, -3, -3); DrawFocusRect(dc, &rect); }
        SelectObject(dc, oldFont); return TRUE;
    }
    }
    return DefWindowProcW(handle_, msg, wparam, lparam);
}
} // namespace mq
