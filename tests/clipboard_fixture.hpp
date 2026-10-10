#pragma once
#include "clipboard.hpp"
#include <shlobj.h>
#include <wrl.h>
#include <cstring>

namespace mq::test {
// An isolated OLE data provider. Tests never read, replace or clear the user's clipboard.
class ClipboardData final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDataObject> {
public:
    struct Entry { CLIPFORMAT format; LONG index; DWORD type; std::string bytes; };
    std::vector<Entry> entries;
    HBITMAP bitmap = nullptr;
    ~ClipboardData() override { if (bitmap) DeleteObject(bitmap); }
    void add(CLIPFORMAT format, std::string bytes, LONG index = -1, DWORD type = TYMED_HGLOBAL) {
        entries.push_back({format, index, type, std::move(bytes)});
    }
    void files(const std::vector<std::pair<std::wstring, std::string>>& files, bool stream = true) {
        std::string group(offsetof(FILEGROUPDESCRIPTORW, fgd) + files.size() * sizeof(FILEDESCRIPTORW), '\0');
        const auto count = static_cast<UINT>(files.size()); memcpy(group.data(), &count, sizeof(count));
        const auto content = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
        for (size_t i = 0; i < files.size(); ++i) {
            FILEDESCRIPTORW descriptor{};
            descriptor.dwFlags = FD_FILESIZE; descriptor.nFileSizeLow = static_cast<DWORD>(files[i].second.size());
            wcsncpy_s(descriptor.cFileName, files[i].first.c_str(), _TRUNCATE);
            memcpy(group.data() + offsetof(FILEGROUPDESCRIPTORW, fgd) + i * sizeof(descriptor), &descriptor, sizeof(descriptor));
            add(content, files[i].second, static_cast<LONG>(i), stream ? TYMED_ISTREAM : TYMED_HGLOBAL);
        }
        add(static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)), std::move(group));
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* request, STGMEDIUM* medium) override {
        if (!request || !medium) return E_POINTER;
        *medium = {};
        for (const auto& entry : entries) {
            if (request->cfFormat != entry.format || request->lindex != entry.index || !(request->tymed & entry.type)) continue;
            medium->tymed = entry.type;
            if (entry.type == TYMED_GDI) {
                medium->hBitmap = static_cast<HBITMAP>(CopyImage(bitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
                return medium->hBitmap ? S_OK : E_FAIL;
            }
            if (entry.type == TYMED_ISTREAM) {
                auto status = CreateStreamOnHGlobal(nullptr, TRUE, &medium->pstm);
                if (FAILED(status)) return status;
                ULONG written = 0;
                status = medium->pstm->Write(entry.bytes.data(), static_cast<ULONG>(entry.bytes.size()), &written);
                if (FAILED(status) || written != entry.bytes.size()) { ReleaseStgMedium(medium); *medium = {}; return E_FAIL; }
                LARGE_INTEGER start{}; return medium->pstm->Seek(start, STREAM_SEEK_SET, nullptr);
            }
            medium->hGlobal = GlobalAlloc(GMEM_MOVEABLE, entry.bytes.size());
            if (!medium->hGlobal) return E_OUTOFMEMORY;
            const auto bytes = GlobalLock(medium->hGlobal);
            if (!bytes) { GlobalFree(medium->hGlobal); *medium = {}; return E_FAIL; }
            memcpy(bytes, entry.bytes.data(), entry.bytes.size()); GlobalUnlock(medium->hGlobal);
            return S_OK;
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* request) override {
        if (!request) return E_POINTER;
        for (const auto& entry : entries)
            if (request->cfFormat == entry.format && request->lindex == entry.index && (request->tymed & entry.type)) return S_OK;
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override { if (output) output->ptd = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};
} // namespace mq::test
