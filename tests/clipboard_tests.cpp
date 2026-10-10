#include "clipboard_fixture.hpp"
#include <wincodec.h>
#include <array>
#include <fstream>

int clipboardTests() {
    int checks = 0;
    const auto check = [&](bool value) { ++checks; if (!value) throw std::runtime_error("Clipboard regression failed"); };
    const auto rejects = [&](mq::test::ClipboardData& source) {
        try { mq::clipboardAttachments(source); }
        catch (const mq::Error& error) { check(error.kind == mq::ErrorKind::Validation); return; }
        throw std::runtime_error("Invalid clipboard data was accepted");
    };
    const auto pixels = [&](const std::string& png) {
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))));
        Microsoft::WRL::ComPtr<IWICStream> stream;
        check(SUCCEEDED(factory->CreateStream(&stream)));
        check(SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(png.data())), static_cast<DWORD>(png.size()))));
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        check(SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)));
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        check(SUCCEEDED(decoder->GetFrame(0, &frame)));
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        check(SUCCEEDED(factory->CreateFormatConverter(&converter)));
        check(SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)));
        UINT width = 0, height = 0; check(SUCCEEDED(converter->GetSize(&width, &height)));
        std::vector<unsigned char> result(static_cast<size_t>(width) * height * 4);
        check(SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(result.size()), result.data())));
        return result;
    };
    // GIVEN a bottom-up screenshot with padded rows, WHEN pasted, THEN PNG preserves orientation and opaque colors.
    BITMAPINFOHEADER header{}; header.biSize = sizeof(header); header.biWidth = 2; header.biHeight = 2; header.biPlanes = 1; header.biBitCount = 24;
    std::string dib(reinterpret_cast<const char*>(&header), sizeof(header));
    const unsigned char rows[] = {255,0,0, 0,255,255, 0,0, 0,0,255, 0,255,0, 0,0};
    dib.append(reinterpret_cast<const char*>(rows), sizeof(rows));
    auto screenshot = Microsoft::WRL::Make<mq::test::ClipboardData>(); screenshot->add(CF_DIB, dib);
    const auto image = mq::clipboardAttachments(*screenshot.Get());
    check(image.size() == 1 && image[0].type == "image/png");
    check(pixels(image[0].bytes) == std::vector<unsigned char>{0,0,255,255, 0,255,0,255, 255,0,0,255, 0,255,255,255});
    // AND a device bitmap is converted without treating undefined alpha as transparency.
    auto device = Microsoft::WRL::Make<mq::test::ClipboardData>();
    BITMAPINFO bitmapInfo{}; bitmapInfo.bmiHeader = header;
    void* bits = nullptr;
    device->bitmap = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    check(device->bitmap && bits); memcpy(bits, rows, sizeof(rows));
    device->add(CF_BITMAP, {}, -1, TYMED_GDI);
    const auto deviceImage = mq::clipboardAttachments(*device.Get());
    check(pixels(deviceImage[0].bytes) == pixels(image[0].bytes));
    // GIVEN a top-down DIBV5 with an alpha mask, WHEN pasted, THEN transparency is retained.
    BITMAPV5HEADER alpha{}; alpha.bV5Size = sizeof(alpha); alpha.bV5Width = 2; alpha.bV5Height = -1;
    alpha.bV5Planes = 1; alpha.bV5BitCount = 32; alpha.bV5Compression = BI_BITFIELDS;
    alpha.bV5RedMask = 0xff0000; alpha.bV5GreenMask = 0xff00; alpha.bV5BlueMask = 0xff; alpha.bV5AlphaMask = 0xff000000; alpha.bV5CSType = LCS_sRGB;
    std::string transparent(reinterpret_cast<const char*>(&alpha), sizeof(alpha));
    const unsigned char rgba[] = {10,20,30,64, 40,50,60,255};
    transparent.append(reinterpret_cast<const char*>(rgba), sizeof(rgba));
    auto alphaSource = Microsoft::WRL::Make<mq::test::ClipboardData>(); alphaSource->add(CF_DIBV5, transparent);
    const auto alphaImage = mq::clipboardAttachments(*alphaSource.Get());
    check(pixels(alphaImage[0].bytes) == std::vector<unsigned char>(std::begin(rgba), std::end(rgba)));
    // GIVEN registered PNG plus a text URL, WHEN pasted, THEN the image wins and encoded bytes are retained.
    auto png = Microsoft::WRL::Make<mq::test::ClipboardData>();
    png->add(static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"PNG")), alphaImage[0].bytes);
    png->add(CF_UNICODETEXT, std::string(32, '\0'));
    const auto original = mq::clipboardAttachments(*png.Get()); check(original.size() == 1 && original[0].bytes == alphaImage[0].bytes);
    // GIVEN virtual attachments from an OLE provider, WHEN copied as streams or memory, THEN no temporary source file is required.
    const std::string binary("a\0b\0c", 5);
    for (const bool stream : {true, false}) {
        auto files = Microsoft::WRL::Make<mq::test::ClipboardData>();
        files->files({{L"附件.pdf", binary}, {L"图片.png", image[0].bytes}}, stream);
        check(mq::clipboardAttachments(*files.Get()) == std::vector<mq::ClipboardAttachment>{{"附件.pdf", "application/pdf", binary}, {"图片.png", "image/png", image[0].bytes}});
    }
    // GIVEN file-manager copies, WHEN read, THEN original bytes and Unicode filenames survive and source files remain intact.
    const auto directory = std::filesystem::temp_directory_path() / (L"memoquick-clipboard-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() {
            std::error_code error;
            const auto temporary = std::filesystem::temp_directory_path(error);
            if (error) return;
            const auto root = std::filesystem::weakly_canonical(temporary, error);
            if (error) return;
            const auto target = std::filesystem::weakly_canonical(directory, error);
            if (error || target.parent_path() != root || !target.filename().wstring().starts_with(L"memoquick-clipboard-")) return;
            std::filesystem::remove_all(target, error);
        }
    } cleanup{directory};
    const auto path = directory / L"剪贴板.pdf";
    { std::ofstream file(path, std::ios::binary); file.write(binary.data(), binary.size()); }
    const auto makeDrop = [](const std::filesystem::path& path) {
        DROPFILES drop{}; drop.pFiles = sizeof(drop); drop.fWide = TRUE;
        auto filename = path.wstring(); filename.push_back(L'\0'); filename.push_back(L'\0');
        std::string bytes(reinterpret_cast<const char*>(&drop), sizeof(drop));
        bytes.append(reinterpret_cast<const char*>(filename.data()), filename.size() * sizeof(wchar_t));
        auto data = Microsoft::WRL::Make<mq::test::ClipboardData>(); data->add(CF_HDROP, std::move(bytes)); return data;
    };
    const auto disk = makeDrop(path);
    check(mq::clipboardAttachments(*disk.Get()) == std::vector<mq::ClipboardAttachment>{{"剪贴板.pdf", "application/pdf", binary}});
    check(std::filesystem::file_size(path) == binary.size());
    auto folder = makeDrop(directory); rejects(*folder.Get());
    auto missing = makeDrop(directory / L"missing.pdf"); rejects(*missing.Get());
    // GIVEN text, malformed images, truncated lists and oversized dimensions, WHEN pasted, THEN text falls through and invalid attachments are rejected.
    auto text = Microsoft::WRL::Make<mq::test::ClipboardData>(); text->add(CF_UNICODETEXT, std::string(16, '\0'));
    check(mq::clipboardAttachments(*text.Get()).empty());
    auto truncated = Microsoft::WRL::Make<mq::test::ClipboardData>(); truncated->add(CF_DIB, dib.substr(0, sizeof(header))); rejects(*truncated.Get());
    header.biWidth = 100000; header.biHeight = 100000;
    auto oversized = Microsoft::WRL::Make<mq::test::ClipboardData>(); oversized->add(CF_DIB, {reinterpret_cast<const char*>(&header), sizeof(header)}); rejects(*oversized.Get());
    auto broken = Microsoft::WRL::Make<mq::test::ClipboardData>(); broken->add(CF_HDROP, "invalid"); rejects(*broken.Get());
    auto badPng = Microsoft::WRL::Make<mq::test::ClipboardData>(); badPng->add(static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"PNG")), "not a PNG"); rejects(*badPng.Get());
    auto tooMany = Microsoft::WRL::Make<mq::test::ClipboardData>(); tooMany->files(std::vector<std::pair<std::wstring, std::string>>(33, {L"a.txt", "x"})); rejects(*tooMany.Get());
    return checks;
}
