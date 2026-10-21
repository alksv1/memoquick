#pragma once
#include <cstddef>
#include <string>

namespace mq {
enum class MarkdownAction { Bold, Italic, Heading, Link, Bullet, Numbered, Task, Quote, InlineCode, CodeBlock };
struct MarkdownEdit {
    size_t start, end;
    std::wstring replacement;
    size_t selectionStart, selectionEnd;
};
MarkdownEdit formatMarkdown(const std::wstring& content, size_t start, size_t end, MarkdownAction action,
                            const std::wstring& textPlaceholder = L"文字",
                            const std::wstring& codePlaceholder = L"代码");
} // namespace mq
