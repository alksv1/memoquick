#include "preview.hpp"
#include "md4c.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace mq {
namespace {
void unit(std::string& out, unsigned c) {
    out += "\\u" + std::to_string(c < 0x8000 ? static_cast<int>(c) : static_cast<int>(c) - 65536) + "?";
}
void text(std::string& out, std::wstring_view s) {
    for (wchar_t ch : s) {
        const auto c = static_cast<std::uint32_t>(ch);
        if (c == L'\\' || c == L'{' || c == L'}') { out += '\\'; out += static_cast<char>(c); }
        else if (c == L'\n') out += "\\line ";
        else if (c == L'\r') continue;
        else if (c == L'\t') out += "\\tab ";
        else if (c >= 32 && c < 127) out += static_cast<char>(c);
        else if (c >= 0x10000 && c <= 0x10ffff) {
            const auto n = c - 0x10000; unit(out, 0xd800 + (n >> 10)); unit(out, 0xdc00 + (n & 0x3ff));
        } else if (c >= 0x80 && c <= 0xffff) unit(out, c);
        else unit(out, 0xfffd);
    }
}
struct List { bool ordered; unsigned next; wchar_t delimiter; bool firstParagraph = false; };
struct Renderer {
    std::string out;
    int width, quote = 0;
    unsigned columns = 0;
    bool paragraph = false;
    std::vector<List> lists;
    explicit Renderer(bool dark, int widthTwips) : width(std::clamp(widthTwips, 1200, 30000)) {
        out = "{\\rtf1\\ansi\\deff0\\uc1{\\fonttbl{\\f0\\fswiss Segoe UI;}{\\f1\\fmodern Consolas;}}";
        out += dark ? "{\\colortbl;\\red230\\green230\\blue230;\\red116\\green180\\blue255;\\red55\\green55\\blue55;\\red165\\green165\\blue165;}"
                    : "{\\colortbl;\\red32\\green32\\blue32;\\red0\\green90\\blue180;\\red235\\green235\\blue235;\\red100\\green100\\blue100;}";
        out += "\\viewkind4\\f0\\fs21\\cf1 ";
    }
    void closeParagraph() { if (paragraph) { out += "\\par}\n"; paragraph = false; } }
    void beginParagraph() {
        closeParagraph();
        out += "{\\pard\\sb0\\sa0\\f0\\fs21\\cf1\\sl-285\\slmult0 ";
        const auto depth = std::min<size_t>(lists.size(), 20);
        if (depth || quote) out += "\\li" + std::to_string(depth * 360 + std::min(quote, 20) * 360) + " ";
        if (quote) out += "\\brdrl\\brdrs\\brdrw20\\brsp120\\brdrcf4\\cf4 ";
        paragraph = true;
    }
};
int enterBlock(MD_BLOCKTYPE type, void* detail, void* data) {
    auto& r = *static_cast<Renderer*>(data);
    switch (type) {
    case MD_BLOCK_QUOTE: r.closeParagraph(); ++r.quote; break;
    case MD_BLOCK_UL: r.closeParagraph(); r.lists.push_back({false, 0, L'.'}); break;
    case MD_BLOCK_OL: {
        r.closeParagraph(); const auto& d = *static_cast<MD_BLOCK_OL_DETAIL*>(detail);
        r.lists.push_back({true, d.start, d.mark_delimiter}); break;
    }
    case MD_BLOCK_LI: {
        r.beginParagraph(); auto& list = r.lists.back(); list.firstParagraph = true;
        r.out += "\\fi-240 ";
        const auto& item = *static_cast<MD_BLOCK_LI_DETAIL*>(detail);
        if (item.is_task) text(r.out, item.task_mark == L' ' ? L"☐ " : L"☑ ");
        else if (list.ordered) { r.out += std::to_string(list.next); text(r.out, std::wstring(1, list.delimiter)); r.out += ' '; }
        else r.out += "\\u8226? ";
        if (list.ordered) ++list.next;
        break;
    }
    case MD_BLOCK_P:
        if (!r.lists.empty() && r.lists.back().firstParagraph) r.lists.back().firstParagraph = false;
        else r.beginParagraph();
        break;
    case MD_BLOCK_H: {
        r.closeParagraph(); const auto level = static_cast<MD_BLOCK_H_DETAIL*>(detail)->level;
        const int sizes[] = {40, 34, 30, 26, 24, 22};
        r.out += "{\\pard\\b\\fs" + std::to_string(sizes[std::clamp(level, 1u, 6u) - 1]) + " "; break;
    }
    case MD_BLOCK_HR:
        r.closeParagraph();
        r.out += "{\\trowd\\trgaph0\\trleft0\\trrh-20\\clbrdrt\\brdrnil\\clbrdrl\\brdrnil\\clbrdrr\\brdrnil\\clbrdrb\\brdrs\\brdrw15\\brdrcf4\\cellx" + std::to_string(r.width) + " \\pard\\intbl\\fs10 \\u160?\\cell\\row}"; break;
    case MD_BLOCK_CODE: {
        r.closeParagraph(); const auto& code = *static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
        if (code.lang.size) { r.out += "{\\pard\\li200\\cf4\\fs18 "; text(r.out, {code.lang.text, code.lang.size}); r.out += "\\par}"; }
        r.out += "{\\pard\\li200\\f1\\fs20\\highlight3 "; break;
    }
    case MD_BLOCK_TABLE: r.closeParagraph(); r.columns = static_cast<MD_BLOCK_TABLE_DETAIL*>(detail)->col_count; break;
    case MD_BLOCK_TR:
        r.out += "{\\trowd\\trgaph80\\trleft0 ";
        for (unsigned i = 1; i <= r.columns; ++i) {
            r.out += "\\clbrdrt\\brdrs\\brdrw10\\brdrcf4\\clbrdrl\\brdrs\\brdrw10\\brdrcf4\\clbrdrb\\brdrs\\brdrw10\\brdrcf4\\clbrdrr\\brdrs\\brdrw10\\brdrcf4\\cellx" +
                std::to_string(static_cast<unsigned long long>(r.width) * i / r.columns);
        }
        r.out += ' '; break;
    case MD_BLOCK_TH: case MD_BLOCK_TD: {
        const auto align = static_cast<MD_BLOCK_TD_DETAIL*>(detail)->align;
        r.out += type == MD_BLOCK_TH ? "\\pard\\intbl\\b " : "\\pard\\intbl\\b0 ";
        if (align == MD_ALIGN_CENTER) r.out += "\\qc ";
        else if (align == MD_ALIGN_RIGHT) r.out += "\\qr ";
        else r.out += "\\ql ";
        break;
    }
    default: break;
    }
    return 0;
}
int leaveBlock(MD_BLOCKTYPE type, void*, void* data) {
    auto& r = *static_cast<Renderer*>(data);
    switch (type) {
    case MD_BLOCK_QUOTE: r.closeParagraph(); --r.quote; break;
    case MD_BLOCK_UL: case MD_BLOCK_OL: r.closeParagraph(); r.lists.pop_back(); break;
    case MD_BLOCK_LI: case MD_BLOCK_P: r.closeParagraph(); break;
    case MD_BLOCK_H: r.out += "\\par}"; break;
    case MD_BLOCK_CODE: r.out += "\\highlight0\\par}"; break;
    case MD_BLOCK_TR: r.out += "\\row}"; break;
    case MD_BLOCK_TH: case MD_BLOCK_TD: r.out += "\\cell "; break;
    default: break;
    }
    return 0;
}
int enterSpan(MD_SPANTYPE type, void*, void* data) {
    auto& out = static_cast<Renderer*>(data)->out;
    switch (type) {
    case MD_SPAN_EM: out += "{\\i "; break;
    case MD_SPAN_STRONG: out += "{\\b "; break;
    case MD_SPAN_DEL: out += "{\\strike "; break;
    case MD_SPAN_CODE: out += "{\\f1\\highlight3 "; break;
    case MD_SPAN_A: out += "{\\cf2\\ul "; break;
    case MD_SPAN_IMG: out += "{\\cf4 "; text(out, L"[图片："); break;
    default: break;
    }
    return 0;
}
int leaveSpan(MD_SPANTYPE type, void*, void* data) {
    auto& out = static_cast<Renderer*>(data)->out;
    switch (type) {
    case MD_SPAN_IMG: text(out, L"]"); out += '}'; break;
    case MD_SPAN_EM: case MD_SPAN_STRONG: case MD_SPAN_DEL: case MD_SPAN_CODE: case MD_SPAN_A: out += '}'; break;
    default: break;
    }
    return 0;
}
int textCallback(MD_TEXTTYPE type, const MD_CHAR* value, MD_SIZE size, void* data) {
    auto& out = static_cast<Renderer*>(data)->out;
    if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) out += "\\line ";
    else if (type == MD_TEXT_NULLCHAR) unit(out, 0xfffd);
    else text(out, {value, size});
    return 0;
}
} // namespace

std::string previewRtf(const std::wstring& markdown, bool dark, int widthTwips) {
    if (markdown.size() > std::numeric_limits<MD_SIZE>::max()) throw std::length_error("Markdown is too large");
    Renderer r(dark, widthTwips);
    MD_PARSER parser{};
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTML;
    parser.enter_block = enterBlock; parser.leave_block = leaveBlock;
    parser.enter_span = enterSpan; parser.leave_span = leaveSpan; parser.text = textCallback;
    if (md_parse(markdown.data(), static_cast<MD_SIZE>(markdown.size()), &parser, &r) != 0)
        throw std::runtime_error("Markdown parsing failed");
    r.out += '}';
    return r.out;
}
} // namespace mq
