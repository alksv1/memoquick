#include "preview.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

namespace mq {
namespace {
using View = std::wstring_view;

bool space(wchar_t c) { return c == L' ' || c == L'\t'; }
bool punctuation(wchar_t c) {
    return c < 128 && ((c >= L'!' && c <= L'/') || (c >= L':' && c <= L'@') ||
                       (c >= L'[' && c <= L'`') || (c >= L'{' && c <= L'~'));
}
void unit(std::string& out, unsigned int c) {
    const int value = c < 0x8000 ? static_cast<int>(c) : static_cast<int>(c) - 65536;
    out += "\\u" + std::to_string(value) + "?";
}
void character(std::string& out, wchar_t ch) {
    const auto c = static_cast<std::uint32_t>(ch);
    if (c == L'\\' || c == L'{' || c == L'}') { out += '\\'; out += static_cast<char>(c); }
    else if (c == L'\t') out += "\\tab ";
    else if (c >= 32 && c < 127) out += static_cast<char>(c);
    else if (c >= 0x10000 && c <= 0x10ffff) {
        const auto n = c - 0x10000;
        unit(out, 0xd800 + (n >> 10));
        unit(out, 0xdc00 + (n & 0x3ff));
    } else if (c >= 0x80 && c <= 0xffff) unit(out, c);
    else if (c < 32 || c == 127 || c > 0x10ffff) unit(out, 0xfffd);
}
void text(std::string& out, View s) { for (wchar_t c : s) character(out, c); }

// Find a delimiter that is not escaped by a backslash.
size_t closing(View s, View delimiter, size_t from) {
    const size_t limit = std::min(s.size(), from + 512);
    for (size_t i = from; i + delimiter.size() <= limit; ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) { ++i; continue; }
        if (s.substr(i, delimiter.size()) == delimiter) return i;
    }
    return View::npos;
}
void inlineText(std::string& out, View s, unsigned int depth = 0) {
    if (depth >= 32) { text(out, s); return; }
    for (size_t i = 0; i < s.size();) {
        if (s[i] == L'\\' && i + 1 < s.size() && punctuation(s[i + 1])) {
            character(out, s[i + 1]); i += 2; continue;
        }
        if (s[i] == L'`') {
            size_t count = 1;
            while (i + count < s.size() && s[i + count] == L'`') ++count;
            size_t end = i + count;
            const size_t limit = std::min(s.size(), end + 512);
            while (end < limit) {
                if (s[end] != L'`') { ++end; continue; }
                size_t run = 1;
                while (end + run < s.size() && s[end + run] == L'`') ++run;
                if (run == count) break;
                end += run;
            }
            if (end < limit) {
                View code = s.substr(i + count, end - i - count);
                if (code.size() >= 2 && code.front() == L' ' && code.back() == L' ' &&
                    code.find_first_not_of(L' ') != View::npos) code = code.substr(1, code.size() - 2);
                out += "{\\f1\\highlight3 "; text(out, code); out += '}';
                i = end + count; continue;
            }
            text(out, s.substr(i, count)); i += count; continue;
        }
        if (s[i] == L'[' && (i == 0 || s[i - 1] != L'!')) {
            size_t end = closing(s, L"]", i + 1);
            if (end != View::npos && end + 1 < s.size() && s[end + 1] == L'(') {
                size_t j = end + 2; unsigned int nesting = 1;
                const size_t limit = std::min(s.size(), j + 2048);
                for (; j < limit && nesting; ++j) {
                    if (s[j] == L'\\' && j + 1 < s.size()) { ++j; continue; }
                    if (s[j] == L'(') ++nesting;
                    else if (s[j] == L')') --nesting;
                }
                if (!nesting) {
                    out += "{\\cf2\\ul "; inlineText(out, s.substr(i + 1, end - i - 1), depth + 1); out += '}';
                    i = j; continue;
                }
            }
        }
        if (s[i] == L'*' || s[i] == L'_' || s[i] == L'~') {
            const wchar_t marker = s[i];
            size_t count = 1;
            while (i + count < s.size() && s[i + count] == marker) ++count;
            const size_t used = marker == L'~' ? 2 : (count >= 3 ? 3 : count);
            const bool insideWord = marker == L'_' && i > 0 && !space(s[i - 1]) && !punctuation(s[i - 1]);
            if (count >= used && (marker != L'~' || count >= 2) && !insideWord &&
                i + used < s.size() && !space(s[i + used])) {
                View delimiter = s.substr(i, used);
                size_t end = closing(s, delimiter, i + used);
                if (end != View::npos && end > i + used && !space(s[end - 1])) {
                    out += marker == L'~' ? "{\\strike " : used == 3 ? "{\\b\\i " : used == 2 ? "{\\b " : "{\\i ";
                    inlineText(out, s.substr(i + used, end - i - used), depth + 1); out += '}';
                    i = end + used; continue;
                }
            }
            text(out, s.substr(i, count)); i += count; continue;
        }
        character(out, s[i++]);
    }
}

size_t indent(View line) { size_t i = 0; while (i < line.size() && i < 3 && line[i] == L' ') ++i; return i; }
bool rule(View line) {
    size_t i = indent(line), count = 0;
    if (i == line.size() || (line[i] != L'-' && line[i] != L'*' && line[i] != L'_')) return false;
    const wchar_t marker = line[i];
    for (; i < line.size(); ++i) {
        if (line[i] == marker) ++count;
        else if (!space(line[i])) return false;
    }
    return count >= 3;
}
struct Fence { wchar_t marker = 0; size_t length = 0; };
Fence fence(View line) {
    size_t i = indent(line);
    if (i == line.size() || (line[i] != L'`' && line[i] != L'~')) return {};
    const wchar_t marker = line[i]; size_t j = i;
    while (j < line.size() && line[j] == marker) ++j;
    if (j - i < 3 || (marker == L'`' && line.substr(j).find(L'`') != View::npos)) return {};
    return {marker, j - i};
}
bool fenceEnd(View line, Fence opening) {
    size_t i = indent(line), j = i;
    while (j < line.size() && line[j] == opening.marker) ++j;
    if (j - i < opening.length) return false;
    while (j < line.size() && space(line[j])) ++j;
    return j == line.size();
}
} // namespace

std::string previewRtf(const std::wstring& markdown, bool dark) {
    std::string out = "{\\rtf1\\ansi\\deff0\\uc1{\\fonttbl{\\f0\\fswiss Segoe UI;}{\\f1\\fmodern Consolas;}}";
    out += dark ? "{\\colortbl;\\red230\\green230\\blue230;\\red116\\green180\\blue255;\\red55\\green55\\blue55;\\red165\\green165\\blue165;}"
                : "{\\colortbl;\\red32\\green32\\blue32;\\red0\\green90\\blue180;\\red235\\green235\\blue235;\\red100\\green100\\blue100;}";
    out += "\\viewkind4\\f0\\fs22\\cf1 ";
    std::vector<View> lines;
    View input(markdown);
    size_t start = 0;
    while (start < input.size()) {
        size_t end = input.find_first_of(L"\r\n", start);
        if (end == View::npos) { lines.push_back(input.substr(start)); start = input.size(); break; }
        lines.push_back(input.substr(start, end - start));
        start = end + 1;
        if (input[end] == L'\r' && start < input.size() && input[start] == L'\n') ++start;
    }
    Fence active;
    for (size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        View line = lines[lineIndex];
        if (active.marker) {
            if (fenceEnd(line, active)) { active = {}; continue; }
            out += "{\\pard\\li360\\f1\\fs20\\highlight3 "; text(out, line); out += "\\highlight0\\par}\n";
            continue;
        }
        Fence opening = fence(line);
        if (opening.marker) { active = opening; continue; }
        out += "{\\pard\\f0\\fs22\\cf1 ";
        if (rule(line)) { out += "\\brdrb\\brdrs\\brdrw10\\brdrcf4 \\par}\n"; continue; }
        size_t p = indent(line), quote = 0;
        while (p < line.size() && line[p] == L'>') {
            ++quote; ++p; if (p < line.size() && line[p] == L' ') ++p;
        }
        if (quote) out += "\\li" + std::to_string(quote > 20 ? 7200 : quote * 360) + "\\brdrl\\brdrs\\brdrw20\\brsp120\\brdrcf4\\cf4 ";
        size_t h = p;
        while (h < line.size() && line[h] == L'#' && h - p < 6) ++h;
        if (h > p && (h == line.size() || space(line[h]))) {
            static const int sizes[] = {40, 34, 30, 26, 24, 22};
            out += "\\b\\fs" + std::to_string(sizes[h - p - 1]) + " ";
            while (h < line.size() && space(line[h])) ++h;
            size_t end = line.size();
            while (end > h && space(line[end - 1])) --end;
            size_t hashes = end;
            while (hashes > h && line[hashes - 1] == L'#') --hashes;
            if (hashes > h && hashes < end && space(line[hashes - 1])) {
                end = hashes; while (end > h && space(line[end - 1])) --end;
            }
            inlineText(out, line.substr(h, end - h)); out += "\\par}\n"; continue;
        }
        // Recognize list markers after arbitrary leading spaces, preserving indentation.
        size_t list = p;
        while (list < line.size() && space(line[list])) ++list;
        size_t content = list;
        bool unordered = content + 1 < line.size() && (line[content] == L'-' || line[content] == L'+' || line[content] == L'*') && space(line[content + 1]);
        bool ordered = false;
        if (unordered) content += 2;
        else {
            while (content < line.size() && content - list < 9 && line[content] >= L'0' && line[content] <= L'9') ++content;
            ordered = content > list && content + 1 < line.size() && (line[content] == L'.' || line[content] == L')') && space(line[content + 1]);
            if (ordered) content += 2;
        }
        if (unordered || ordered) {
            const size_t nesting = list - p + (quote ? 0 : p);
            out += "\\li" + std::to_string((nesting > 30 ? 30 : nesting) * 120 + 360) + "\\fi-240 ";
            if (unordered) out += "\\u8226? ";
            else { text(out, line.substr(list, content - list - 1)); out += ' '; }
            while (content < line.size() && space(line[content])) ++content;
            if (content + 2 < line.size() && line[content] == L'[' && line[content + 2] == L']' &&
                (line[content + 1] == L' ' || line[content + 1] == L'x' || line[content + 1] == L'X') &&
                (content + 3 == line.size() || space(line[content + 3]))) {
                out += line[content + 1] == L' ' ? "[ ] " : "[x] "; content += 3;
                while (content < line.size() && space(line[content])) ++content;
            }
            inlineText(out, line.substr(content));
        } else inlineText(out, line.substr(p));
        out += "\\par}\n";
    }
    out += '}';
    return out;
}
} // namespace mq
