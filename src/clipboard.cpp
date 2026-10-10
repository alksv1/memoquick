#include "clipboard.hpp"
#include <shlobj.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>

namespace mq {
using Microsoft::WRL::ComPtr;
namespace {
constexpr size_t BatchSizeMax = 64 * 1024 * 1024;
constexpr uint64_t PixelCountMax = 16 * 1024 * 1024;
Error pasteError() { return Error("无法读取剪贴板附件，请重新复制后再试。", ErrorKind::Validation); }
void checked(HRESULT status) { if (FAILED(status)) throw pasteError(); }
struct Medium {
    STGMEDIUM value{};
    Medium(IDataObject& source, CLIPFORMAT format, DWORD type, LONG index = -1) {
        FORMATETC request{format, nullptr, DVASPECT_CONTENT, index, type};
        checked(source.GetData(&request, &value));
        if (!(value.tymed & type)) { ReleaseStgMedium(&value); value = {}; throw pasteError(); }
    }
    ~Medium() { ReleaseStgMedium(&value); }
    Medium(const Medium&) = delete;
    Medium& operator=(const Medium&) = delete;
};
struct GlobalBytes {
    HGLOBAL handle;
    const unsigned char* data;
    size_t size;
    explicit GlobalBytes(HGLOBAL value) : handle(value), data(static_cast<const unsigned char*>(GlobalLock(value))), size(GlobalSize(value)) {
        if (!data || !size) { if (data) GlobalUnlock(handle); throw pasteError(); }
    }
    ~GlobalBytes() { GlobalUnlock(handle); }
};
bool available(IDataObject& source, CLIPFORMAT format, DWORD type) {
    FORMATETC request{format, nullptr, DVASPECT_CONTENT, -1, type};
    return source.QueryGetData(&request) == S_OK;
}
std::string streamBytes(IStream& stream) {
    STATSTG stat{};
    if (SUCCEEDED(stream.Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > AttachmentSizeMax)
        throw Error("附件不能超过 20 MiB。", ErrorKind::Validation);
    LARGE_INTEGER start{};
    // Virtual providers may expose a non-seekable stream already at its start.
    (void)stream.Seek(start, STREAM_SEEK_SET, nullptr);
    std::string bytes;
    char buffer[65536];
    for (;;) {
        ULONG read = 0;
        const auto status = stream.Read(buffer, sizeof(buffer), &read);
        checked(status);
        if (read > sizeof(buffer) || bytes.size() + read > AttachmentSizeMax)
            throw Error("附件不能超过 20 MiB。", ErrorKind::Validation);
        bytes.append(buffer, read);
        if (status == S_FALSE || !read) break;
    }
    if (bytes.empty()) throw Error("不能粘贴空附件。", ErrorKind::Validation);
    return bytes;
}
std::string mediumBytes(const Medium& medium) {
    if (medium.value.tymed == TYMED_ISTREAM && medium.value.pstm) return streamBytes(*medium.value.pstm);
    if (medium.value.tymed != TYMED_HGLOBAL) throw pasteError();
    const GlobalBytes bytes(medium.value.hGlobal);
    if (bytes.size > AttachmentSizeMax) throw Error("附件不能超过 20 MiB。", ErrorKind::Validation);
    return {reinterpret_cast<const char*>(bytes.data), bytes.size};
}
void dimensions(IWICBitmapSource& source) {
    UINT width = 0, height = 0; checked(source.GetSize(&width, &height));
    if (!width || !height || static_cast<uint64_t>(width) * height > PixelCountMax)
        throw Error("图片过大，请缩小到 1600 万像素以内再粘贴。", ErrorKind::Validation);
}
ComPtr<IWICImagingFactory> imagingFactory() {
    ComPtr<IWICImagingFactory> factory;
    checked(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    return factory;
}
ComPtr<IWICBitmapFrameDecode> decode(IWICImagingFactory& factory, const std::string& bytes) {
    ComPtr<IWICStream> stream; checked(factory.CreateStream(&stream));
    checked(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())), static_cast<DWORD>(bytes.size())));
    ComPtr<IWICBitmapDecoder> decoder; checked(factory.CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder));
    ComPtr<IWICBitmapFrameDecode> frame; checked(decoder->GetFrame(0, &frame));
    dimensions(*frame.Get());
    return frame;
}
std::string encodePng(IWICImagingFactory& factory, IWICBitmapSource& source) {
    dimensions(source);
    ComPtr<IStream> stream; checked(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
    ComPtr<IWICBitmapEncoder> encoder; checked(factory.CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    checked(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame; checked(encoder->CreateNewFrame(&frame, nullptr)); checked(frame->Initialize(nullptr));
    UINT width = 0, height = 0; checked(source.GetSize(&width, &height)); checked(frame->SetSize(width, height));
    auto format = GUID_WICPixelFormat32bppBGRA; checked(frame->SetPixelFormat(&format));
    ComPtr<IWICFormatConverter> converter; checked(factory.CreateFormatConverter(&converter));
    checked(converter->Initialize(&source, format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    checked(frame->WriteSource(converter.Get(), nullptr)); checked(frame->Commit()); checked(encoder->Commit());
    return streamBytes(*stream.Get());
}
std::string dibPng(const GlobalBytes& bytes) {
    if (bytes.size < sizeof(BITMAPINFOHEADER)) throw pasteError();
    BITMAPINFOHEADER header{}; memcpy(&header, bytes.data, sizeof(header));
    if ((header.biSize != 40 && header.biSize != 52 && header.biSize != 56 && header.biSize != 108 && header.biSize != 124)
        || header.biSize > bytes.size || header.biPlanes != 1 || header.biWidth <= 0 || !header.biHeight
        || header.biHeight == std::numeric_limits<LONG>::min()) throw pasteError();
    const auto height = static_cast<uint64_t>(header.biHeight < 0 ? -header.biHeight : header.biHeight);
    if (static_cast<uint64_t>(header.biWidth) * height > PixelCountMax)
        throw Error("图片过大，请缩小到 1600 万像素以内再粘贴。", ErrorKind::Validation);
    if (header.biBitCount != 1 && header.biBitCount != 4 && header.biBitCount != 8
        && header.biBitCount != 16 && header.biBitCount != 24 && header.biBitCount != 32) throw pasteError();
    if (header.biCompression != BI_RGB && header.biCompression != BI_BITFIELDS && header.biCompression != 6) throw pasteError();
    const auto colors = header.biClrUsed ? header.biClrUsed : (header.biBitCount <= 8 ? 1u << header.biBitCount : 0u);
    if (colors > 256 || (header.biBitCount <= 8 && colors > (1u << header.biBitCount))) throw pasteError();
    size_t offset = header.biSize + static_cast<size_t>(colors) * sizeof(RGBQUAD);
    if (header.biSize == 40 && header.biCompression != BI_RGB) offset += (header.biCompression == 6 ? 4 : 3) * sizeof(DWORD);
    if (header.biSize == sizeof(BITMAPV5HEADER)) {
        BITMAPV5HEADER extended{}; memcpy(&extended, bytes.data, sizeof(extended));
        if (extended.bV5ProfileSize) {
            if (extended.bV5ProfileData > bytes.size || extended.bV5ProfileSize > bytes.size - extended.bV5ProfileData) throw pasteError();
            if (extended.bV5ProfileData == offset) offset += extended.bV5ProfileSize;
        }
    }
    const auto stride = ((static_cast<uint64_t>(header.biWidth) * header.biBitCount + 31) / 32) * 4;
    if (offset > bytes.size || stride * height > bytes.size - offset || bytes.size > BatchSizeMax + 4096) throw pasteError();
    BITMAPFILEHEADER file{}; file.bfType = 0x4d42; file.bfSize = static_cast<DWORD>(sizeof(file) + bytes.size);
    file.bfOffBits = static_cast<DWORD>(sizeof(file) + offset);
    std::string bmp(reinterpret_cast<const char*>(&file), sizeof(file));
    bmp.append(reinterpret_cast<const char*>(bytes.data), bytes.size);
    const auto factory = imagingFactory();
    const auto frame = decode(*factory.Get(), bmp);
    return encodePng(*factory.Get(), *frame.Get());
}
std::string imageFilename() {
    SYSTEMTIME now{}; GetLocalTime(&now);
    wchar_t name[80]{};
    swprintf_s(name, L"pasted-%04u%02u%02u-%02u%02u%02u.png", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    return utf8(name);
}
void append(std::vector<ClipboardAttachment>& result, size_t& total, std::string filename, std::string bytes) {
    total += bytes.size();
    if (total > BatchSizeMax) throw Error("单次粘贴附件总量不能超过 64 MiB。", ErrorKind::Validation);
    const auto path = std::filesystem::path(wide(filename)).filename();
    if (path.empty() || path == L"." || path == L"..") throw pasteError();
    result.push_back({utf8(path.wstring()), fileMime(path), std::move(bytes)});
}
} // namespace

std::vector<ClipboardAttachment> clipboardAttachments(IDataObject& source) {
    std::vector<ClipboardAttachment> result;
    size_t total = 0;
    if (available(source, CF_HDROP, TYMED_HGLOBAL)) {
        const Medium medium(source, CF_HDROP, TYMED_HGLOBAL);
        const GlobalBytes bytes(medium.value.hGlobal);
        if (bytes.size < sizeof(DROPFILES)) throw pasteError();
        DROPFILES header{}; memcpy(&header, bytes.data, sizeof(header));
        if (header.pFiles < sizeof(header) || header.pFiles >= bytes.size || bytes.size > 2 * 1024 * 1024) throw pasteError();
        bool terminated = false;
        if (header.fWide) {
            if (header.pFiles % sizeof(wchar_t) || (bytes.size - header.pFiles) % sizeof(wchar_t)) throw pasteError();
            const auto paths = reinterpret_cast<const wchar_t*>(bytes.data + header.pFiles);
            const auto length = (bytes.size - header.pFiles) / sizeof(wchar_t);
            for (size_t i = 1; i < length; ++i) if (!paths[i - 1] && !paths[i]) { terminated = true; break; }
        } else {
            for (size_t i = header.pFiles + 1; i < bytes.size; ++i)
                if (!bytes.data[i - 1] && !bytes.data[i]) { terminated = true; break; }
        }
        if (!terminated) throw pasteError();
        const auto drop = reinterpret_cast<HDROP>(medium.value.hGlobal);
        const auto count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
        if (!count || count > 32) throw Error("单次最多粘贴 32 个文件。", ErrorKind::Validation);
        for (UINT i = 0; i < count; ++i) {
            const auto length = DragQueryFileW(drop, i, nullptr, 0);
            if (!length || length >= 32768) throw pasteError();
            std::wstring filename(length + 1, L'\0');
            if (DragQueryFileW(drop, i, filename.data(), length + 1) != length) throw pasteError();
            filename.resize(length);
            const std::filesystem::path path(filename);
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
                throw Error("请复制文件，暂不支持粘贴文件夹。", ErrorKind::Validation);
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file) throw pasteError();
            const auto size = file.tellg();
            if (size <= 0 || size > static_cast<std::streamoff>(AttachmentSizeMax))
                throw Error("附件需非空且不超过 20 MiB。", ErrorKind::Validation);
            if (total + static_cast<size_t>(size) > BatchSizeMax) throw Error("单次粘贴附件总量不能超过 64 MiB。", ErrorKind::Validation);
            std::string data(static_cast<size_t>(size), '\0'); file.seekg(0);
            if (!file.read(data.data(), size)) throw pasteError();
            append(result, total, utf8(path.filename().wstring()), std::move(data));
        }
        return result;
    }
    const auto descriptors = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
    const auto contents = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
    if (available(source, descriptors, TYMED_HGLOBAL)) {
        const Medium medium(source, descriptors, TYMED_HGLOBAL);
        const GlobalBytes bytes(medium.value.hGlobal);
        if (bytes.size < offsetof(FILEGROUPDESCRIPTORW, fgd)) throw pasteError();
        UINT count = 0; memcpy(&count, bytes.data, sizeof(count));
        if (!count || count > 32 || count > (bytes.size - offsetof(FILEGROUPDESCRIPTORW, fgd)) / sizeof(FILEDESCRIPTORW)) throw pasteError();
        for (UINT i = 0; i < count; ++i) {
            FILEDESCRIPTORW descriptor{};
            memcpy(&descriptor, bytes.data + offsetof(FILEGROUPDESCRIPTORW, fgd) + i * sizeof(descriptor), sizeof(descriptor));
            if (((descriptor.dwFlags & FD_ATTRIBUTES) && (descriptor.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                || wcsnlen(descriptor.cFileName, MAX_PATH) == MAX_PATH) throw pasteError();
            if ((descriptor.dwFlags & FD_FILESIZE) && (descriptor.nFileSizeHigh || descriptor.nFileSizeLow > AttachmentSizeMax))
                throw Error("附件不能超过 20 MiB。", ErrorKind::Validation);
            const Medium content(source, contents, TYMED_ISTREAM | TYMED_HGLOBAL, static_cast<LONG>(i));
            auto data = mediumBytes(content);
            if (descriptor.dwFlags & FD_FILESIZE) {
                if (data.size() < descriptor.nFileSizeLow || (content.value.tymed == TYMED_ISTREAM && data.size() != descriptor.nFileSizeLow)) throw pasteError();
                data.resize(descriptor.nFileSizeLow);
                if (data.empty()) throw Error("不能粘贴空附件。", ErrorKind::Validation);
            }
            append(result, total, utf8(descriptor.cFileName), std::move(data));
        }
        return result;
    }
    const auto png = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"PNG"));
    if (available(source, png, TYMED_HGLOBAL)) {
        const Medium medium(source, png, TYMED_HGLOBAL);
        auto bytes = mediumBytes(medium);
        if (!bytes.starts_with("\x89PNG\r\n\x1a\n")) throw pasteError();
        const auto factory = imagingFactory();
        decode(*factory.Get(), bytes); // Validate dimensions; retain original PNG and transparency.
        return {{imageFilename(), "image/png", std::move(bytes)}};
    }
    for (const auto format : {CF_DIBV5, CF_DIB}) {
        if (!available(source, static_cast<CLIPFORMAT>(format), TYMED_HGLOBAL)) continue;
        const Medium medium(source, static_cast<CLIPFORMAT>(format), TYMED_HGLOBAL);
        const GlobalBytes bytes(medium.value.hGlobal);
        return {{imageFilename(), "image/png", dibPng(bytes)}};
    }
    if (available(source, CF_BITMAP, TYMED_GDI)) {
        const Medium medium(source, CF_BITMAP, TYMED_GDI);
        BITMAP info{};
        if (GetObjectW(medium.value.hBitmap, sizeof(info), &info) != sizeof(info) || info.bmWidth <= 0 || info.bmHeight <= 0) throw pasteError();
        if (static_cast<uint64_t>(info.bmWidth) * info.bmHeight > PixelCountMax)
            throw Error("图片过大，请缩小到 1600 万像素以内再粘贴。", ErrorKind::Validation);
        const auto factory = imagingFactory();
        ComPtr<IWICBitmap> bitmap;
        checked(factory->CreateBitmapFromHBITMAP(medium.value.hBitmap, nullptr, WICBitmapIgnoreAlpha, &bitmap));
        return {{imageFilename(), "image/png", encodePng(*factory.Get(), *bitmap.Get())}};
    }
    return {};
}
} // namespace mq
