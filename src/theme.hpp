#pragma once
#include <windows.h>

namespace mq {
struct Palette {
    COLORREF background, ink, muted, accent, hover, line, error;
};
inline Palette palette(bool dark) {
    return dark ? Palette{RGB(29,31,35), RGB(229,231,235), RGB(151,158,169), RGB(54,111,86), RGB(43,46,52), RGB(58,62,69), RGB(246,174,133)}
                : Palette{RGB(253,253,253), RGB(38,43,48), RGB(117,124,134), RGB(42,105,78), RGB(239,241,243), RGB(225,228,232), RGB(157,67,36)};
}
inline int scaled(int value, unsigned dpi) { return MulDiv(value, static_cast<int>(dpi), 96); }
} // namespace mq
