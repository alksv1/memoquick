#include "window.hpp"
#include <shellapi.h>
#include <psapi.h>
#include <objbase.h>
#include <dwmapi.h>
#include <array>
#include <chrono>
#include <fstream>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void requireFrameless(HWND window) {
    RECT outer{}, client{};
    GetWindowRect(window, &outer); GetClientRect(window, &client);
    POINT origin{}; ClientToScreen(window, &origin);
    require(origin.x == outer.left && origin.y == outer.top
        && client.right == outer.right - outer.left && client.bottom == outer.bottom - outer.top,
        "Native nonclient frame is still present");
}
void screenshot(HWND window, const std::filesystem::path& output, bool live = false, bool frameless = true) {
    if (!live) RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    if (frameless) requireFrameless(window);
    RECT rect{}; GetWindowRect(window, &rect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    const auto screen = GetWindowDC(window);
    const auto memory = CreateCompatibleDC(screen);
    const auto bitmap = CreateCompatibleBitmap(screen, width, height);
    const auto previous = SelectObject(memory, bitmap);
    if (live) require(BitBlt(memory, 0, 0, width, height, screen, 0, 0, SRCCOPY) != FALSE, "Live screenshot failed");
    else require(PrintWindow(window, memory, 0) != FALSE, "Full-window screenshot failed");
    SelectObject(memory, previous);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    require(GetDIBits(memory, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info, DIB_RGB_COLORS) != 0, "Screenshot failed");
    BITMAPFILEHEADER header{}; header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
    std::ofstream file(output, std::ios::binary);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window, screen);
}
std::array<COLORREF, 4> edgePixels(HWND window) {
    RECT bounds{}; GetWindowRect(window, &bounds);
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    const auto dc = GetWindowDC(window);
    const std::array<COLORREF, 4> pixels{
        GetPixel(dc, 2, height / 2), GetPixel(dc, width - 3, height / 2),
        GetPixel(dc, width / 2, 2), GetPixel(dc, width / 2, height - 3)};
    ReleaseDC(window, dc);
    for (const auto pixel : pixels) require(pixel != CLR_INVALID, "Cannot read live window edges");
    return pixels;
}
void verifyFocusFrame(HWND window, const std::filesystem::path& output) {
    // GIVEN the actual window, WHEN another native window takes activation,
    // THEN the already displayed edges remain unchanged without a forced repaint.
    RECT bounds{}; GetWindowRect(window, &bounds);
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    const auto other = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"STATIC", L"MemoQuick focus fixture",
        WS_POPUP | WS_VISIBLE, bounds.right + 8, bounds.top, 24, 24, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(other != nullptr, "Cannot create focus fixture");
    try {
        SetActiveWindow(window);
        require(GetActiveWindow() == window, "Cannot activate memo window");
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        GdiFlush(); DwmFlush();
        const auto baseline = edgePixels(window);
        SetActiveWindow(other);
        require(GetActiveWindow() == other, "Focus did not move to fixture");
        GdiFlush(); DwmFlush();
        screenshot(window, output, true);
        require(edgePixels(window) == baseline, "Losing focus repainted native borders over the client area");
        SetActiveWindow(window);
        GdiFlush(); DwmFlush();
        require(edgePixels(window) == baseline, "Regaining focus repainted native borders");
    } catch (...) {
        DestroyWindow(other);
        SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        throw;
    }
    DestroyWindow(other);
    SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
struct MenuFixture {
    HMENU menu = nullptr;
    HWND popup = nullptr;
    std::filesystem::path output;
    bool dark = false;
    bool confirm = false;
    int target = -1;
    int stage = 0;
    int ticks = 0;
    std::exception_ptr failure;
};
LRESULT CALLBACK menuFixtureProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    if (message == WM_INITMENUPOPUP) reinterpret_cast<MenuFixture*>(data)->menu = reinterpret_cast<HMENU>(wparam);
    return DefSubclassProc(window, message, wparam, lparam);
}
void CALLBACK menuFixtureTick(HWND window, UINT, UINT_PTR timer, DWORD) {
    auto& fixture = *reinterpret_cast<MenuFixture*>(GetPropW(window, L"MemoQuick.MenuFixture"));
    try {
        require(++fixture.ticks < 20, "Popup did not respond to keyboard");
        if (!fixture.popup) {
            EnumThreadWindows(GetCurrentThreadId(), [](HWND candidate, LPARAM data) -> BOOL {
                wchar_t name[32]{}; GetClassNameW(candidate, name, 32);
                if (std::wstring_view(name) == L"#32768" && IsWindowVisible(candidate)) {
                    reinterpret_cast<MenuFixture*>(data)->popup = candidate; return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&fixture));
        }
        if (!fixture.popup || !fixture.menu) return;
        if (fixture.stage == 0) {
            // GIVEN a real tracked popup, WHEN shown in either theme, THEN its background follows the editor.
            RECT bounds{}, item{}; GetWindowRect(fixture.popup, &bounds);
            require(GetMenuItemRect(window, fixture.menu, 0, &item) != FALSE, "Cannot locate popup item");
            wchar_t caption[128]{};
            require(GetMenuStringW(fixture.menu, 0, caption, 128, MF_BYPOSITION) > 0, "Owner-drawn menu lost accessible text");
            const auto dc = GetWindowDC(fixture.popup);
            const auto pixel = GetPixel(dc, item.right - bounds.left - 2, (item.top + item.bottom) / 2 - bounds.top);
            ReleaseDC(fixture.popup, dc);
            screenshot(fixture.popup, fixture.output, true, false);
            require(pixel == (fixture.dark ? RGB(29,31,35) : RGB(253,253,253)), "Popup background does not follow theme");
            fixture.stage = 1;
            PostMessageW(window, WM_KEYDOWN, VK_DOWN, 0);
        } else {
            MENUITEMINFOW info{sizeof(info)}; info.fMask = MIIM_STATE;
            int selected = -1;
            for (int i = 0; i < GetMenuItemCount(fixture.menu); ++i) {
                require(GetMenuItemInfoW(fixture.menu, static_cast<UINT>(i), TRUE, &info) != FALSE, "Cannot inspect popup selection");
                if (info.fState & MFS_HILITE) selected = i;
            }
            if (selected < 0 || (fixture.target >= 0 && selected != fixture.target)) {
                PostMessageW(window, WM_KEYDOWN, VK_DOWN, 0); return;
            }
            RECT bounds{}, item{}; GetWindowRect(fixture.popup, &bounds);
            require(GetMenuItemRect(window, fixture.menu, static_cast<UINT>(selected), &item) != FALSE, "Cannot locate selected item");
            const auto dc = GetWindowDC(fixture.popup);
            const auto pixel = GetPixel(dc, item.right - bounds.left - 8, (item.top + item.bottom) / 2 - bounds.top);
            ReleaseDC(fixture.popup, dc);
            require(pixel == (fixture.dark ? RGB(43,46,52) : RGB(239,241,243)), "Popup selection does not follow theme");
            screenshot(fixture.popup, fixture.output.parent_path() / (fixture.output.stem().wstring() + L"-selected.bmp"), true, false);
            fixture.stage = 2;
            KillTimer(window, timer);
            PostMessageW(window, WM_KEYDOWN, fixture.confirm ? VK_RETURN : VK_ESCAPE, 0);
        }
    } catch (...) {
        fixture.failure = std::current_exception(); KillTimer(window, timer); EndMenu();
    }
}
void verifyMenu(mq::Window& window, int button, bool dark, bool confirm, const std::filesystem::path& output, int target = -1) {
    const auto before = window.currentDraft();
    MenuFixture fixture{nullptr, nullptr, output, dark, confirm, target};
    const auto handle = window.handle();
    require(SetWindowSubclass(handle, menuFixtureProcedure, 891, reinterpret_cast<DWORD_PTR>(&fixture)) != FALSE, "Cannot attach menu fixture");
    require(SetPropW(handle, L"MemoQuick.MenuFixture", &fixture) != FALSE, "Cannot configure menu fixture");
    require(SetTimer(handle, 891, 100, menuFixtureTick) != 0, "Cannot start menu fixture");
    if (button) SendMessageW(handle, WM_COMMAND, MAKEWPARAM(button, BN_CLICKED), 0);
    else SendMessageW(handle, mq::TrayMessage, 0, WM_RBUTTONUP);
    KillTimer(handle, 891); RemovePropW(handle, L"MemoQuick.MenuFixture"); RemoveWindowSubclass(handle, menuFixtureProcedure, 891);
    if (fixture.failure) std::rethrow_exception(fixture.failure);
    require(fixture.stage == 2, "Popup was not exercised");
    require(window.currentDraft() == before, "Opening or cancelling menu changed draft");
}
class Smoke {
public:
    Smoke(mq::Window& window, mq::Store& store, std::string server, std::filesystem::path output)
        : window_(window), store_(store), server_(std::move(server)), output_(std::move(output)) {
        std::filesystem::create_directories(output_);
        SetTimer(window_.handle(), 777, 30, nullptr);
        UpdateWindow(window_.handle());
        screenshot(window_.handle(), output_ / L"login.bmp");
        verifyMenu(window_, 0, false, false, output_ / L"tray-menu-light.bmp");
        MONITORINFO monitor{sizeof(monitor)};
        require(GetMonitorInfoW(MonitorFromWindow(window_.handle(), MONITOR_DEFAULTTONEAREST), &monitor) != FALSE, "Monitor lookup failed");
        SetWindowPos(window_.handle(), nullptr, monitor.rcWork.left + 20, monitor.rcWork.top + 20, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(window_.handle(), WM_EXITSIZEMOVE, 0, 0);
        RECT moved{}; GetWindowRect(window_.handle(), &moved);
        require(store_.windowPosition() == mq::WindowPosition{moved.left, moved.top}, "Drag end did not save position");
        SendMessageW(window_.handle(), WM_COMMAND, MAKEWPARAM(mq::ThemeButton, BN_CLICKED), 0);
        screenshot(window_.handle(), output_ / L"login-dark.bmp");
        verifyMenu(window_, 0, true, false, output_ / L"tray-menu-dark.bmp");
        SendMessageW(window_.handle(), WM_COMMAND, MAKEWPARAM(mq::ThemeButton, BN_CLICKED), 0);
        window_.smokeLogin(server_);
    }
    bool tick() {
        if (window_.busy()) return false;
        const auto handle = window_.handle();
        if (stage_ == 0) {
            require(window_.view() == mq::View::Compose, window_.notice().c_str());
            require(GetWindowTextLengthW(GetDlgItem(handle, mq::PasswordInput)) == 0, "Password was retained");
            SendMessageW(GetDlgItem(handle, mq::Editor), EM_REPLACESEL, TRUE,
                reinterpret_cast<LPARAM>(L"原生 C++ 快速记录 📝\r\n#测试\r\n- [ ] 整理灵感"));
            const auto original = window_.currentDraft();
            const auto editor = GetDlgItem(handle, mq::Editor);
            SendMessageW(editor, EM_SETSEL, 0, 2);
            SetFocus(GetDlgItem(handle, mq::MdBold));
            SendMessageW(handle, WM_COMMAND, MAKEWPARAM(mq::MdBold, BN_CLICKED), 0);
            require(window_.currentDraft().content.starts_with("**原生**"), "Toolbar did not format selected Unicode text");
            require(GetFocus() == editor, "Markdown toolbar lost editor focus");
            SendMessageW(editor, EM_UNDO, 0, 0);
            require(window_.currentDraft() == original, "Markdown operation could not be undone");
            SendMessageW(editor, EM_SETSEL, 0, 2);
            SendMessageW(handle, WM_COMMAND, MAKEWPARAM(mq::MdLink, BN_CLICKED), 0);
            DWORD linkStart = 0, linkEnd = 0;
            SendMessageW(editor, EM_GETSEL, reinterpret_cast<WPARAM>(&linkStart), reinterpret_cast<LPARAM>(&linkEnd));
            require(linkStart == 5 && linkEnd == 13, "Link URL placeholder was not selected");
            SendMessageW(editor, EM_UNDO, 0, 0);
            require(window_.currentDraft() == original, "Link undo lost note content");
            require(window_.flushDraft(), "Draft persistence failed");
            require(!store_.draft(mq::parseServer(server_).url, "users/7").content.empty(), "Draft not persisted");
            stage_ = 1;
            SendMessageW(handle, WM_COMMAND, MAKEWPARAM(mq::SaveButton, BN_CLICKED), 0);
            require(!IsWindowEnabled(GetDlgItem(handle, mq::MdBold)), "Markdown toolbar stayed enabled while saving");
        } else if (stage_ == 1) {
            require(!window_.notice().empty(), "Failed write did not show an error");
            require(IsWindowVisible(handle) && !window_.currentDraft().content.empty(), "Failed write lost content or hid window");
            stage_ = 2;
            SendMessageW(handle, WM_COMMAND, MAKEWPARAM(mq::SaveButton, BN_CLICKED), 0);
        } else if (stage_ == 2) {
            require(!IsWindowVisible(handle), window_.notice().c_str());
            require(window_.currentDraft().content.empty(), "Successful write did not clear editor");
            require(store_.draft(mq::parseServer(server_).url, "users/7").content.empty(), "Successful write left a draft");
            const auto start = std::chrono::steady_clock::now();
            SendMessageW(handle, WM_HOTKEY, 1, 0);
            latency_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            require(IsWindowVisible(handle), "Hotkey handler did not show window");
            require(!IsWindowVisible(GetDlgItem(handle, mq::BrowserButton)), "Compose contains account actions");
            RECT size{}; GetClientRect(handle, &size);
            const auto dpi = static_cast<int>(GetDpiForWindow(handle));
            require(MulDiv(size.right, 96, dpi) <= 422 && MulDiv(size.bottom, 96, dpi) <= 282, "Window is not compact");
            SendMessageW(GetDlgItem(handle, mq::Editor), EM_REPLACESEL, TRUE,
                reinterpret_cast<LPARAM>(L"今天想到的，先记下来。\r\n\r\n#灵感\r\n- [ ] 给忙碌的生活留一点空白\r\n- [ ] 把下一步写得更具体"));
            window_.flushDraft();
            UpdateWindow(handle);
            screenshot(handle, output_ / L"compose.bmp");
            verifyMenu(window_, mq::AddButton, false, false, output_ / L"add-menu-light.bmp");
            verifyMenu(window_, mq::Visibility, false, false, output_ / L"visibility-menu-light.bmp");
            verifyFocusFrame(handle, output_ / L"compose-inactive.bmp");
            draft_ = window_.currentDraft();
            SendMessageW(handle, WM_COMMAND, MAKEWPARAM(mq::ThemeButton, BN_CLICKED), 0);
            require(store_.config().dark, "Dark mode was not saved");
            require(window_.currentDraft() == draft_, "Theme switch changed draft");
            require(GetFocus() == GetDlgItem(handle, mq::Editor), "Theme switch lost editor focus");
            const auto editor = GetDlgItem(handle, mq::Editor);
            std::wstring longNote;
            for (int i = 0; i < 50; ++i) longNote += L"滚动检查\r\n";
            SetWindowTextW(editor, longNote.c_str());
            SendMessageW(editor, EM_SETSEL, 0, 0);
            SendMessageW(editor, EM_SCROLLCARET, 0, 0);
            SendMessageW(editor, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
            require(SendMessageW(editor, EM_GETFIRSTVISIBLELINE, 0, 0) > 0, "Borderless editor cannot scroll");
            SetWindowTextW(editor, mq::wide(draft_.content).c_str());
            UpdateWindow(handle);
            screenshot(handle, output_ / L"compose-dark.bmp");
            verifyMenu(window_, mq::AddButton, true, false, output_ / L"add-menu-dark.bmp");
            verifyMenu(window_, mq::Visibility, true, true, output_ / L"visibility-menu-dark.bmp", 0);
            verifyMenu(window_, mq::AddButton, true, true, output_ / L"add-menu-keyboard.bmp", 4);
            require(window_.view() == mq::View::Location, "Enter did not execute the selected menu command");
            SendMessageW(handle, WM_COMMAND, MAKEWPARAM(mq::BackButton, BN_CLICKED), 0);
            verifyFocusFrame(handle, output_ / L"compose-dark-inactive.bmp");
            RECT originalSize{}; GetWindowRect(handle, &originalSize);
            SetWindowPos(handle, nullptr, 0, 0, MulDiv(340, dpi, 96), MulDiv(416, dpi, 96),
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            verifyFocusFrame(handle, output_ / L"compose-dark-resized-inactive.bmp");
            SetWindowPos(handle, nullptr, 0, 0, originalSize.right - originalSize.left, originalSize.bottom - originalSize.top,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessageW(handle, WM_COMMAND, MAKEWPARAM(mq::SettingsButton, BN_CLICKED), 0);
            stage_ = 3;
        } else if (stage_ == 3) {
            UpdateWindow(handle);
            screenshot(handle, output_ / L"settings.bmp");
            require(IsWindowVisible(GetDlgItem(handle, mq::ThemeButton)) && IsWindowVisible(GetDlgItem(handle, mq::CloseButton)), "Settings lost header actions");
            require(IsWindowVisible(GetDlgItem(handle, mq::BrowserButton)), "Settings lost account actions");
            require(!IsWindowVisible(GetDlgItem(handle, mq::MdBold)), "Settings contains Markdown toolbar");
            SendMessageW(handle, WM_COMMAND, MAKEWPARAM(mq::ThemeButton, BN_CLICKED), 0);
            require(!store_.config().dark, "Light mode was not saved");
            stage_ = 4;
        } else if (stage_ == 4) {
            UpdateWindow(handle);
            screenshot(handle, output_ / L"settings-light.bmp");
            SendMessageW(handle, WM_COMMAND, MAKEWPARAM(mq::BackButton, BN_CLICKED), 0);
            require(window_.currentDraft() == draft_, "Settings changed draft");
            SendMessageW(GetDlgItem(handle, mq::Editor), EM_SETSEL, static_cast<WPARAM>(-1), -1);
            SendMessageW(GetDlgItem(handle, mq::Editor), EM_REPLACESEL, TRUE,
                reinterpret_cast<LPARAM>(L"\r\n还未手动保存的灵感"));
            draft_ = window_.currentDraft();
            RECT beforeHide{}, afterShow{}; GetWindowRect(handle, &beforeHide);
            SendMessageW(handle, WM_HOTKEY, 1, 0);
            require(!IsWindowVisible(handle), "Second hotkey press did not hide window");
            require(store_.draft(mq::parseServer(server_).url, "users/7") == draft_, "Hotkey hide lost unsaved draft");
            SendMessageW(handle, WM_HOTKEY, 1, 0);
            require(IsWindowVisible(handle) && window_.currentDraft() == draft_, "Hotkey reopen lost draft");
            requireFrameless(handle);
            GetWindowRect(handle, &afterShow);
            require(beforeHide.left == afterShow.left && beforeHide.top == afterShow.top, "Reopening moved the window");
            SendMessageW(handle, mq::ShowMessage, 0, 0);
            require(IsWindowVisible(handle), "Launching an existing instance hid window");
            PROCESS_MEMORY_COUNTERS_EX metrics{};
            metrics.cb = sizeof(metrics);
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&metrics), sizeof(metrics));
            std::ofstream report(output_ / L"metrics.json");
            report << "{\n  \"working_set_bytes\": " << metrics.WorkingSetSize
                << ",\n  \"private_bytes\": " << metrics.PrivateUsage
                << ",\n  \"show_handler_ms\": " << latency_ << "\n}\n";
            report.close();
            stage_ = 5;
            SendMessageW(handle,WM_COMMAND,MAKEWPARAM(mq::InsertLocation,BN_CLICKED),0);
        } else if (stage_ == 5) {
            // GIVEN the native location panel, WHEN invalid and valid values are confirmed, THEN validation preserves the note.
            require(window_.view() == mq::View::Location, "Location panel did not open");
            SetWindowTextW(GetDlgItem(handle,mq::LocationName),L"书桌");
            SetWindowTextW(GetDlgItem(handle,mq::LatitudeInput),L"nan");
            SetWindowTextW(GetDlgItem(handle,mq::LongitudeInput),L"114.2");
            SendMessageW(handle,WM_COMMAND,MAKEWPARAM(mq::PanelConfirm,BN_CLICKED),0);
            require(!window_.currentDraft().location && !window_.notice().empty(), "Invalid location was accepted");
            SetWindowTextW(GetDlgItem(handle,mq::LatitudeInput),L"22.3");
            screenshot(handle,output_ / L"location.bmp");
            SendMessageW(handle,WM_COMMAND,MAKEWPARAM(mq::PanelConfirm,BN_CLICKED),0);
            require(window_.currentDraft().location == mq::Location{"书桌",22.3,114.2}, "Location confirmation lost coordinates");
            require(window_.currentDraft().content == draft_.content, "Location panel changed editor content");
            stage_ = 6;
            SendMessageW(handle,WM_COMMAND,MAKEWPARAM(mq::InsertMemo,BN_CLICKED),0);
        } else if (stage_ == 6) {
            require(window_.view() == mq::View::Link, "Link panel did not open");
            require(SendMessageW(GetDlgItem(handle,mq::MemoList),LB_GETCOUNT,0,0) == 1, "Link selector leaked other users or archived memos");
            screenshot(handle,output_ / L"link-memo.bmp");
            SendMessageW(GetDlgItem(handle,mq::MemoList),LB_SETCURSEL,0,0);
            SendMessageW(handle,WM_COMMAND,MAKEWPARAM(mq::PanelConfirm,BN_CLICKED),0);
            draft_ = window_.currentDraft();
            require(draft_.relations == std::vector<mq::MemoReference>{{"memos/reference-1","参考一"}}, "Link selector did not create a reference");
            SendMessageW(handle,WM_COMMAND,MAKEWPARAM(mq::InsertAudio,BN_CLICKED),0);
            require(window_.view() == mq::View::Record, "Audio panel did not open");
            screenshot(handle,output_ / L"audio.bmp");
            SendMessageW(handle,WM_COMMAND,MAKEWPARAM(mq::BackButton,BN_CLICKED),0);
            require(window_.currentDraft() == draft_, "Cancelling audio changed the draft");
            // GIVEN cached attachments, WHEN the same account signs in again, THEN upload and inline insertion resume without the original files.
            const auto url = mq::parseServer(server_).url;
            const auto picture = store_.cacheAttachment(url,"users/7","fixture-image-bytes");
            const auto voice = store_.cacheAttachment(url,"users/7",mq::waveFile(std::string(3200,'\0')));
            draft_.attachments = {{"","小图.png","image/png",picture,true},{"","voice.wav","audio/wav",voice}};
            store_.saveDraft(url,"users/7",draft_);
            stage_ = 7; window_.smokeLogin(server_);
        } else if (stage_ == 7) {
            require(window_.currentDraft() == draft_, "Sign-in did not restore attachments and metadata");
            stage_ = 8;
            SendMessageW(handle,WM_COMMAND,MAKEWPARAM(mq::RetryAttachments,BN_CLICKED),0);
        } else if (stage_ == 8) {
            // GIVEN one confirmed upload and one failed upload, WHEN retrying, THEN only the pending file is uploaded.
            const auto partial = window_.currentDraft();
            require(!window_.notice().empty() && partial.attachments[0].name == "attachments/fixture-1"
                && partial.attachments[1].name.empty(), "Partial upload failure did not preserve confirmed and pending attachments");
            require(partial == store_.draft(mq::parseServer(server_).url,"users/7"), "Partial upload progress was not saved");
            stage_ = 81;
            SendMessageW(handle,WM_COMMAND,MAKEWPARAM(mq::RetryAttachments,BN_CLICKED),0);
        } else if (stage_ == 81) {
            const auto draft = window_.currentDraft();
            require(draft.attachments.size() == 2 && draft.attachments[0].name == "attachments/fixture-1" && !draft.attachments[0].inlineImage,
                window_.notice().c_str());
            require(draft.content.find("![小图.png](/notes/file/attachments/fixture-1/image)") != std::string::npos, "Uploaded image was not inserted into Markdown");
            require(draft == store_.draft(mq::parseServer(server_).url,"users/7"), "Upload metadata was not persisted");
            SendMessageW(GetDlgItem(handle,mq::Editor),EM_UNDO,0,0);
            require(window_.currentDraft().content == draft_.content, "Image insertion could not be undone");
            SendMessageW(GetDlgItem(handle,mq::Editor),EM_UNDO,0,0);
            require(window_.currentDraft() == draft, "Image insertion redo changed attachment metadata");
            screenshot(handle,output_ / L"compose-rich.bmp");
            verifyMenu(window_, mq::MetadataButton, false, false, output_ / L"metadata-menu-light.bmp");
            SendMessageW(handle,WM_COMMAND,MAKEWPARAM(mq::ThemeButton,BN_CLICKED),0);
            screenshot(handle,output_ / L"compose-rich-dark.bmp");
            verifyMenu(window_, mq::MetadataButton, true, false, output_ / L"metadata-menu-dark.bmp");
            stage_ = 9;
            SendMessageW(handle,WM_COMMAND,MAKEWPARAM(mq::SaveButton,BN_CLICKED),0);
        } else if (stage_ == 9) {
            require(!IsWindowVisible(handle),window_.notice().c_str());
            require(window_.currentDraft() == mq::Draft{}, "Saving did not clear all compose metadata");
            require(store_.draft(mq::parseServer(server_).url,"users/7") == mq::Draft{}, "Saving left metadata in the stored draft");
            stage_ = 10;
            KillTimer(handle, 777);
            DestroyWindow(handle);
            return true;
        }
        return false;
    }
private:
    mq::Window& window_;
    mq::Store& store_;
    std::string server_;
    std::filesystem::path output_;
    int stage_ = 0;
    double latency_ = 0;
    mq::Draft draft_;
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int count = 0;
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    bool background = false, smoke = false, checkPosition = false;
    std::string server;
    std::filesystem::path data, output;
    for (int i = 1; i < count; ++i) {
        const std::wstring argument = arguments[i];
        if (argument == L"--background") background = true;
        if (argument == L"--check-position") checkPosition = true;
        if (argument == L"--smoke" && i + 1 < count) { smoke = true; server = mq::utf8(arguments[++i]); }
        if (argument == L"--data-dir" && i + 1 < count) data = arguments[++i];
        if (argument == L"--output" && i + 1 < count) output = arguments[++i];
    }
    LocalFree(arguments);
    HANDLE mutex = nullptr;
    int result = 0;
    try {
        if ((smoke || checkPosition) && (data.empty() || output.empty())) throw std::runtime_error("Desktop tests require isolated data and output directories.");
        if (!smoke && !checkPosition) {
            mutex = CreateMutexW(nullptr, FALSE, L"Local\\MemoQuick.Native.SingleInstance");
            if (!mutex) throw std::runtime_error("Cannot acquire application mutex");
            if (GetLastError() == ERROR_ALREADY_EXISTS) {
                if (const auto existing = FindWindowW(mq::WindowClass, nullptr)) {
                    DWORD process = 0; GetWindowThreadProcessId(existing, &process);
                    AllowSetForegroundWindow(process);
                    PostMessageW(existing, mq::ShowMessage, 0, 0);
                }
                CloseHandle(mutex);
                if (SUCCEEDED(com)) CoUninitialize();
                return 0;
            }
        }
        mq::Store store(data.empty() ? mq::dataDirectory() : data);
        mq::Window window(store, smoke || checkPosition);
        window.create(instance, background);
        if (checkPosition) {
            RECT bounds{}; GetWindowRect(window.handle(), &bounds);
            MONITORINFO monitor{sizeof(monitor)};
            require(GetMonitorInfoW(MonitorFromWindow(window.handle(), MONITOR_DEFAULTTONEAREST), &monitor) != FALSE, "Monitor lookup failed");
            std::ofstream report(output / L"position.json");
            report << "{\"x\":" << bounds.left << ",\"y\":" << bounds.top
                << ",\"right\":" << bounds.right << ",\"bottom\":" << bounds.bottom
                << ",\"workLeft\":" << monitor.rcWork.left << ",\"workTop\":" << monitor.rcWork.top
                << ",\"workRight\":" << monitor.rcWork.right << ",\"workBottom\":" << monitor.rcWork.bottom << '}';
            require(report.good(), "Position report failed");
            DestroyWindow(window.handle());
        }
        std::unique_ptr<Smoke> test;
        if (smoke) test = std::make_unique<Smoke>(window, store, server, output);
        const auto deadline = GetTickCount64() + 60000;
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (smoke && GetTickCount64() > deadline) throw std::runtime_error("Desktop test timed out");
            if (smoke && message.message == WM_TIMER && message.wParam == 777) { test->tick(); continue; }
            try {
                if (!window.preprocess(message)) { TranslateMessage(&message); DispatchMessageW(&message); }
            } catch (const std::exception& error) {
                MessageBoxW(window.handle(), mq::wide(error.what()).c_str(), L"MemoQuick", MB_OK | MB_ICONINFORMATION);
            }
        }
    } catch (const std::exception& error) {
        result = 1;
        if (smoke || checkPosition) {
            std::filesystem::create_directories(output);
            std::ofstream(output / L"failure.txt") << error.what();
        } else MessageBoxW(nullptr, mq::wide(error.what()).c_str(), L"MemoQuick 无法启动", MB_OK | MB_ICONERROR);
    }
    if (mutex) CloseHandle(mutex);
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
