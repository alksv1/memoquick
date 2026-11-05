#pragma once

#include <string>

namespace mq {
std::string previewRtf(const std::wstring& markdown, bool dark, int widthTwips = 6000);
} // namespace mq
