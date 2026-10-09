#include "window.hpp"
#include "theme.hpp"
#include <algorithm>

namespace mq {
namespace {
struct MenuItem {
    std::wstring caption;
    bool separator = false;
};
struct MenuOwner {
    HMENU handle;
    std::vector<MenuItem> items;
    ~MenuOwner() { DestroyMenu(handle); }
};
} // namespace

UINT Window::trackMenu(HMENU menu, UINT flags, int x, int y) {
    // Menu data and the theme brush must outlive the native modal menu loop.
    MenuOwner owner{menu, {}};
    if (!menu) throw Error("无法打开菜单。", ErrorKind::Storage);
    const auto count = GetMenuItemCount(menu);
    if (count < 0) throw Error("无法读取菜单。", ErrorKind::Storage);
    owner.items.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW info{sizeof(info)};
        info.fMask = MIIM_FTYPE | MIIM_STRING;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info))
            throw Error("无法读取菜单。", ErrorKind::Storage);
        auto& item = owner.items[static_cast<size_t>(i)];
        item.separator = (info.fType & MFT_SEPARATOR) != 0;
        item.caption.resize(static_cast<size_t>(info.cch) + 1);
        info.dwTypeData = item.caption.data(); ++info.cch;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info))
            throw Error("无法读取菜单文字。", ErrorKind::Storage);
        item.caption.resize(info.cch);
        info.fMask = MIIM_FTYPE | MIIM_DATA;
        info.fType |= MFT_OWNERDRAW;
        info.dwItemData = reinterpret_cast<ULONG_PTR>(&item);
        if (!SetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info))
            throw Error("无法设置菜单。", ErrorKind::Storage);
    }
    MENUINFO info{sizeof(info)};
    info.fMask = MIM_BACKGROUND | MIM_STYLE;
    info.hbrBack = background_;
    info.dwStyle = MNS_NOCHECK;
    if (!SetMenuInfo(menu, &info)) throw Error("无法设置菜单主题。", ErrorKind::Storage);
    return static_cast<UINT>(TrackPopupMenu(menu, flags | TPM_RETURNCMD, x, y, 0, handle_, nullptr));
}

void Window::measureMenu(MEASUREITEMSTRUCT& measure) {
    const auto& item = *reinterpret_cast<const MenuItem*>(measure.itemData);
    const auto dc = GetDC(handle_);
    const auto previous = SelectObject(dc, font_);
    SIZE size{};
    GetTextExtentPoint32W(dc, item.caption.c_str(), static_cast<int>(item.caption.size()), &size);
    SelectObject(dc, previous); ReleaseDC(handle_, dc);
    measure.itemWidth = static_cast<UINT>(std::clamp<LONG>(size.cx + scaled(38, dpi_), scaled(128, dpi_), scaled(320, dpi_)));
    measure.itemHeight = static_cast<UINT>(item.separator ? scaled(7, dpi_) : std::max<LONG>(scaled(28, dpi_), size.cy + scaled(10, dpi_)));
}

void Window::drawMenu(const DRAWITEMSTRUCT& draw) {
    const auto& item = *reinterpret_cast<const MenuItem*>(draw.itemData);
    const auto colors = palette(config_.dark);
    const auto dc = draw.hDC;
    const auto saved = SaveDC(dc);
    FillRect(dc, &draw.rcItem, background_);
    if (item.separator) {
        auto line = draw.rcItem;
        line.left += scaled(8, dpi_); line.right -= scaled(8, dpi_);
        line.top = (line.top + line.bottom) / 2; line.bottom = line.top + 1;
        const auto brush = CreateSolidBrush(colors.line);
        FillRect(dc, &line, brush); DeleteObject(brush);
    } else {
        const bool disabled = (draw.itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
        if ((draw.itemState & ODS_SELECTED) && !disabled) {
            auto selection = draw.rcItem;
            InflateRect(&selection, -scaled(3, dpi_), -scaled(1, dpi_));
            const auto brush = CreateSolidBrush(colors.hover);
            SelectObject(dc, brush); SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, selection.left, selection.top, selection.right, selection.bottom, scaled(4, dpi_), scaled(4, dpi_));
            SelectObject(dc, GetStockObject(NULL_BRUSH)); DeleteObject(brush);
        }
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, disabled ? colors.muted : colors.ink);
        SelectObject(dc, font_);
        auto label = draw.rcItem;
        label.left += scaled(26, dpi_); label.right -= scaled(12, dpi_);
        DrawTextW(dc, item.caption.c_str(), -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (draw.itemState & ODS_CHECKED) {
            auto check = draw.rcItem;
            check.left += scaled(4, dpi_); check.right = check.left + scaled(20, dpi_);
            DrawTextW(dc, L"✓", -1, &check, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    RestoreDC(dc, saved);
}
} // namespace mq
