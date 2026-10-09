#include "markdown.hpp"
#include <algorithm>
#include <vector>

namespace mq {
namespace {
size_t run(const std::wstring& text, wchar_t marker) {
    size_t longest = 0, current = 0;
    for (const auto character : text) { current = character == marker ? current + 1 : 0; longest = std::max(longest, current); }
    return longest;
}
size_t listPrefix(const std::wstring& text) {
    if (text.starts_with(L"- [ ] ") || text.starts_with(L"- [x] ") || text.starts_with(L"- [X] ")) return 6;
    if (text.starts_with(L"- ") || text.starts_with(L"* ") || text.starts_with(L"+ ")) return 2;
    const auto digits = text.find_first_not_of(L"0123456789");
    return digits != 0 && digits != std::wstring::npos && text.substr(digits, 2) == L". " ? digits + 2 : 0;
}
size_t headingPrefix(const std::wstring& text) {
    const auto length = text.find_first_not_of(L'#');
    return length >= 1 && length <= 6 && length < text.size() && text[length] == L' ' ? length + 1 : 0;
}
} // namespace

MarkdownEdit formatMarkdown(const std::wstring& content, size_t start, size_t end, MarkdownAction action) {
    start = std::min(start, content.size()); end = std::clamp(end, start, content.size());
    const auto selected = content.substr(start, end - start);
    const auto wrap = [&](const std::wstring& before, const std::wstring& body, const std::wstring& after) {
        return MarkdownEdit{start, end, before + body + after, start + before.size(), start + before.size() + body.size()};
    };
    if (action == MarkdownAction::Bold || action == MarkdownAction::Italic) {
        const std::wstring marker = action == MarkdownAction::Bold ? L"**" : L"*";
        const auto length = marker.size();
        if (selected.size() >= length * 2 && selected.starts_with(marker) && selected.ends_with(marker))
            return wrap(L"", selected.substr(length, selected.size() - length * 2), L"");
        size_t left = start, right = end;
        while (left > 0 && content[left - 1] == L'*') --left;
        while (right < content.size() && content[right] == L'*') ++right;
        const bool outside = start - left >= length && right - end >= length
            && (action == MarkdownAction::Bold || ((start - left) % 2 == 1 && (right - end) % 2 == 1));
        if (outside) return {start - length, end + length, selected, start - length, end - length};
        return wrap(marker, selected.empty() ? L"文字" : selected, marker);
    }
    if (action == MarkdownAction::Link) {
        const auto body = selected.empty() ? L"文字" : selected;
        const auto urlStart = start + 1 + std::wstring(body).size() + 2;
        return {start, end, L"[" + std::wstring(body) + L"](https://)", urlStart, urlStart + 8};
    }
    if (action == MarkdownAction::InlineCode) {
        const auto body = selected.empty() ? L"代码" : selected;
        const std::wstring marker(run(body, L'`') + 1, L'`');
        const bool pad = body.front() == L'`' || body.back() == L'`';
        return wrap(marker + (pad ? L" " : L""), body, (pad ? L" " : L"") + marker);
    }
    if (action == MarkdownAction::CodeBlock) {
        const auto body = selected.empty() ? L"代码" : selected;
        const std::wstring fence(std::max<size_t>(3, run(body, L'`') + 1), L'`');
        const auto before = (start > 0 && content[start - 1] != L'\n' ? L"\r\n" : L"") + fence + L"\r\n";
        const auto after = (body.ends_with(L"\n") ? L"" : L"\r\n") + fence
            + (end < content.size() && content[end] != L'\r' && content[end] != L'\n' ? L"\r\n" : L"");
        return wrap(before, body, after);
    }

    // Line tools operate on complete lines; a selection ending at the next line excludes it.
    const auto preceding = start == 0 ? std::wstring::npos : content.rfind(L'\n', start - 1);
    const auto lineStart = preceding == std::wstring::npos ? 0 : preceding + 1;
    size_t last = end;
    if (end > start && content[end - 1] == L'\n') --last;
    const auto following = content.find(L'\n', last);
    size_t lineEnd = following == std::wstring::npos ? content.size() : following;
    if (lineEnd > lineStart && content[lineEnd - 1] == L'\r') --lineEnd;
    struct Line { std::wstring indent, body, newline; size_t prefix; };
    std::vector<Line> lines;
    bool remove = true;
    const auto block = content.substr(lineStart, lineEnd - lineStart);
    for (size_t offset = 0; offset <= block.size();) {
        const auto next = block.find(L'\n', offset);
        const auto stop = next == std::wstring::npos ? block.size() : next;
        auto body = block.substr(offset, stop - offset);
        const bool cr = !body.empty() && body.back() == L'\r'; if (cr) body.pop_back();
        const auto indentation = body.find_first_not_of(L" \t");
        const auto indent = body.substr(0, indentation); body.erase(0, indent.size());
        size_t prefix = 0;
        if (action == MarkdownAction::Heading && body.starts_with(L"## ")) prefix = 3;
        if (action == MarkdownAction::Quote && body.starts_with(L"> ")) prefix = 2;
        if (action == MarkdownAction::Bullet && body.starts_with(L"- ") && listPrefix(body) == 2) prefix = 2;
        if (action == MarkdownAction::Task && listPrefix(body) == 6) prefix = 6;
        if (action == MarkdownAction::Numbered && !body.empty() && body[0] >= L'0' && body[0] <= L'9') prefix = listPrefix(body);
        if (!body.empty() || start == end) remove = remove && prefix != 0;
        lines.push_back({indent, body, next == std::wstring::npos ? L"" : cr ? L"\r\n" : L"\n", prefix});
        if (next == std::wstring::npos) break;
        offset = next + 1;
    }
    std::wstring replacement;
    size_t number = 1, caret = start - lineStart;
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        std::wstring prefix; size_t strip = remove ? line.prefix : 0;
        if (!remove && (!line.body.empty() || start == end)) {
            if (action == MarkdownAction::Heading) { prefix = L"## "; strip = headingPrefix(line.body); }
            if (action == MarkdownAction::Quote) prefix = L"> ";
            if (action == MarkdownAction::Bullet) prefix = L"- ";
            if (action == MarkdownAction::Numbered) prefix = std::to_wstring(number++) + L". ";
            if (action == MarkdownAction::Task) prefix = L"- [ ] ";
            if (action == MarkdownAction::Bullet || action == MarkdownAction::Numbered || action == MarkdownAction::Task) strip = listPrefix(line.body);
        }
        if (i == 0) caret = line.indent.size() + prefix.size() + (caret > line.indent.size() + strip ? caret - line.indent.size() - strip : 0);
        replacement += line.indent + prefix + line.body.substr(strip) + line.newline;
    }
    if (start == end) return {lineStart, lineEnd, replacement, lineStart + caret, lineStart + caret};
    return {lineStart, lineEnd, replacement, lineStart, lineStart + replacement.size()};
}
} // namespace mq
