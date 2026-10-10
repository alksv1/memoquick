#include "window.hpp"
#include <commdlg.h>
#include <fstream>
#include <algorithm>
#include <cmath>

namespace mq {
void Window::insertMenu() {
    if (busy_ || view_ != View::Compose) return;
    const auto menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, InsertAttachment, L"添加附件");
    AppendMenuW(menu, MF_STRING, InsertImage, L"插入图片");
    AppendMenuW(menu, MF_STRING, InsertAudio, L"录音");
    AppendMenuW(menu, MF_STRING, InsertMemo, L"关联 Memo");
    AppendMenuW(menu, MF_STRING, InsertLocation, L"添加位置");
    RECT rect{}; GetWindowRect(control(AddButton), &rect);
    const auto command = trackMenu(menu, TPM_LEFTALIGN | TPM_BOTTOMALIGN, rect.left, rect.top);
    if (command) SendMessageW(handle_, WM_COMMAND, MAKEWPARAM(command, BN_CLICKED), 0);
}
void Window::chooseFiles(bool image) {
    std::vector<wchar_t> names(65536);
    OPENFILENAMEW chooser{}; chooser.lStructSize = sizeof(chooser); chooser.hwndOwner = handle_;
    chooser.lpstrFile = names.data(); chooser.nMaxFile = static_cast<DWORD>(names.size());
    chooser.lpstrFilter = image ? L"图片\0*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp;*.svg;*.avif;*.heic\0所有文件\0*.*\0" : L"所有文件\0*.*\0";
    chooser.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&chooser)) {
        if (CommDlgExtendedError()) throw Error("无法选择文件。", ErrorKind::Storage);
        return;
    }
    std::vector<std::filesystem::path> paths;
    const std::filesystem::path first(names.data());
    auto next = names.data() + wcslen(names.data()) + 1;
    if (!*next) paths.push_back(first);
    else while (*next) { paths.push_back(first / next); next += wcslen(next) + 1; }
    for (const auto& path : paths) {
        const auto type = fileMime(path);
        if (image && !type.starts_with("image/")) throw Error("请选择图片文件。", ErrorKind::Validation);
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) throw Error("无法打开附件。", ErrorKind::Storage);
        const auto size = file.tellg();
        if (size <= 0 || size > static_cast<std::streamoff>(AttachmentSizeMax)) throw Error("附件需非空且不超过 20 MiB。", ErrorKind::Validation);
        std::string bytes(static_cast<size_t>(size), '\0'); file.seekg(0);
        if (!file.read(bytes.data(), size)) throw Error("无法完整读取附件。", ErrorKind::Storage);
        cacheAttachment(utf8(path.filename().wstring()), type, bytes, image);
    }
    if (image) uploadPending(true);
    else SetFocus(control(Editor));
}
void Window::cacheAttachment(const std::string& filename, const std::string& type, const std::string& bytes, bool image) {
    if (extras_.attachments.size() >= 32) throw Error("每条记录最多添加 32 个附件。", ErrorKind::Validation);
    const auto id = store_.cacheAttachment(config_.server, user_.name, bytes);
    extras_.attachments.push_back({{}, filename, type, id, image});
    draftDirty_ = true;
    updateMetadata();
    if (!flushDraft()) {
        extras_.attachments.pop_back(); store_.removeAttachment(id); updateMetadata();
        throw Error("附件未能加入草稿，请检查磁盘后重试。", ErrorKind::Storage);
    }
}
void Window::prepareAttachments(Draft& draft, size_t cursor, bool imagesOnly) {
    for (auto& attachment : draft.attachments) {
        if (imagesOnly && !attachment.inlineImage) continue;
        if (attachment.name.empty()) {
            const auto bytes = store_.attachmentBytes(config_.server, user_.name, attachment.localId);
            attachment = client_->uploadAttachment(attachment, bytes);
            // Persist a confirmed upload before proceeding to another network write.
            store_.saveDraft(config_.server, user_.name, draft);
        }
        if (attachment.inlineImage) {
            auto content = wide(draft.content);
            cursor = std::min(cursor, content.size());
            if (cursor && cursor < content.size() && content[cursor] >= 0xdc00 && content[cursor] <= 0xdfff
                && content[cursor - 1] >= 0xd800 && content[cursor - 1] <= 0xdbff) --cursor;
            const auto image = wide(imageMarkdown(client_->server(), attachment)) + L"\r\n";
            content.insert(cursor, image); cursor += image.size();
            draft.content = utf8(content); attachment.inlineImage = false;
            store_.saveDraft(config_.server, user_.name, draft);
        }
    }
}
void Window::uploadPending(bool imagesOnly) {
    if (busy_ || !client_ || view_ != View::Compose) return;
    DWORD cursor = 0; SendMessageW(control(Editor), EM_GETSEL, reinterpret_cast<WPARAM>(&cursor), 0);
    auto draft = currentDraft();
    launch(Action::Upload, [this, draft = std::move(draft), cursor, imagesOnly](Completion& result) mutable {
        result.draft = std::move(draft);
        prepareAttachments(*result.draft, cursor, imagesOnly);
    });
}
void Window::updateMetadata() {
    std::wstring caption;
    if (!extras_.attachments.empty()) caption = std::to_wstring(extras_.attachments.size()) + L" 附件";
    if (!extras_.relations.empty()) caption += (caption.empty() ? L"" : L" · ") + std::to_wstring(extras_.relations.size()) + L" 关联";
    if (extras_.location) caption += (caption.empty() ? L"" : L" · ") + std::wstring(L"位置");
    text(MetadataButton, caption);
    EnableWindow(control(MetadataButton), !busy_ && !caption.empty());
}
void Window::metadataMenu() {
    const auto menu = CreatePopupMenu();
    const auto safeLabel = [](const std::string& value) {
        auto label = wide(value); std::replace(label.begin(), label.end(), L'&', L'＿');
        std::replace(label.begin(), label.end(), L'\n', L' '); std::replace(label.begin(), label.end(), L'\r', L' ');
        return label.substr(0, 80);
    };
    if (std::any_of(extras_.attachments.begin(), extras_.attachments.end(), [](const auto& a) { return a.name.empty() || a.inlineImage; }))
        AppendMenuW(menu, MF_STRING, RetryAttachments, L"重试上传");
    for (size_t i = 0; i < extras_.attachments.size(); ++i) {
        const auto& a = extras_.attachments[i];
        const auto label = L"移除 " + safeLabel(a.filename) + (a.name.empty() ? L"（待上传）" : L"");
        AppendMenuW(menu, MF_STRING, 2000 + i, label.c_str());
    }
    for (size_t i = 0; i < extras_.relations.size(); ++i)
        AppendMenuW(menu, MF_STRING, 3000 + i, (L"取消关联 " + safeLabel(extras_.relations[i].snippet)).c_str());
    if (extras_.location) {
        AppendMenuW(menu, MF_STRING, InsertLocation, (L"编辑位置 " + safeLabel(extras_.location->placeholder)).c_str());
        AppendMenuW(menu, MF_STRING, 4000, L"移除位置");
    }
    RECT rect{}; GetWindowRect(control(MetadataButton), &rect);
    const auto command = trackMenu(menu, TPM_LEFTALIGN | TPM_BOTTOMALIGN, rect.left, rect.top);
    if (command == RetryAttachments || command == InsertLocation) {
        SendMessageW(handle_, WM_COMMAND, MAKEWPARAM(command, BN_CLICKED), 0); return;
    }
    std::string removed;
    if (command >= 2000 && command < 2000 + extras_.attachments.size()) {
        const auto index = command - 2000;
        const auto& a = extras_.attachments[index]; removed = a.localId;
        if (!a.name.empty() && a.type.starts_with("image/")) {
            auto content = utf8(text(Editor));
            const auto markup = imageMarkdown(client_->server(), a);
            for (auto position = content.find(markup); position != std::string::npos; position = content.find(markup)) content.erase(position, markup.size());
            text(Editor, wide(content));
        }
        extras_.attachments.erase(extras_.attachments.begin() + index);
    } else if (command >= 3000 && command < 3000 + extras_.relations.size()) {
        extras_.relations.erase(extras_.relations.begin() + command - 3000);
    } else if (command == 4000) extras_.location.reset();
    else return;
    draftDirty_ = true; updateMetadata();
    if (flushDraft() && !removed.empty()) store_.removeAttachment(removed);
    SetFocus(control(Editor));
}
void Window::searchMemos(bool more) {
    if (busy_ || !client_) return;
    const auto search = utf8(text(SearchInput));
    const auto page = more && search == memoSearch_ ? memoPage_.nextPage : "";
    if (!more || search != memoSearch_) { memoPage_ = {}; SendMessageW(control(MemoList), LB_RESETCONTENT, 0, 0); }
    memoSearch_ = search;
    launch(Action::Search, [this, search, page](Completion& result) { result.page = client_->listMemos(search, page); });
}
void Window::confirmPanel() {
    if (locationReader_.active()) return;
    if (view_ == View::Link) {
        const auto index = SendMessageW(control(MemoList), LB_GETCURSEL, 0, 0);
        if (index == LB_ERR || static_cast<size_t>(index) >= memoPage_.memos.size()) return;
        const auto& memo = memoPage_.memos[static_cast<size_t>(index)];
        if (std::none_of(extras_.relations.begin(), extras_.relations.end(), [&](const auto& r) { return r.name == memo.name; })) {
            if (extras_.relations.size() >= 32) throw Error("每条记录最多关联 32 条 Memo。", ErrorKind::Validation);
            extras_.relations.push_back(memo);
        }
    } else if (view_ == View::Location) {
        extras_.location = parseLocation(utf8(text(LocationName)), utf8(text(LatitudeInput)), utf8(text(LongitudeInput)));
    } else if (view_ == View::Record) { finishRecording(true); return; }
    else return;
    draftDirty_ = true; updateMetadata();
    if (flushDraft()) { setNotice(""); setView(View::Compose); }
}
void Window::stopLocation() {
    KillTimer(handle_, 6);
    locationReader_.cancel();
    locationNames_.cancel();
    locationNameReplaceable_ = false;
    text(LocateButton, L"当前位置");
    for (const auto id : {LatitudeInput, LongitudeInput, PanelConfirm}) EnableWindow(control(id), !busy_);
}
void Window::locate() {
    if (busy_ || view_ != View::Location) return;
    if (locationReader_.active() || locationNames_.active()) { stopLocation(); setNotice(""); return; }
    setNotice(""); ShowWindow(control(LocationSettingsButton), SW_HIDE);
    try {
        locationReader_.start();
        text(LocateButton, L"取消定位");
        for (const auto id : {LatitudeInput, LongitudeInput, PanelConfirm}) EnableWindow(control(id), FALSE);
        if (!SetTimer(handle_, 6, 100, nullptr)) throw Error("无法开始定位，请重试。", ErrorKind::Validation);
    } catch (const Error& error) {
        stopLocation(); setNotice(error.what());
        if (error.kind == ErrorKind::LocationDenied) ShowWindow(control(LocationSettingsButton), SW_SHOW);
    }
}
void Window::locationTick() {
    try {
        const auto fix = locationReader_.poll();
        if (fix) {
            fillLocation(*fix);
            if (!config_.geocodingEndpoint.empty() && locationNameReplaceable_) {
                locationNames_.start(config_.geocodingEndpoint, fix->latitude, fix->longitude);
                text(LocateButton, L"取消查询");
                if (!SetTimer(handle_, 6, 100, nullptr)) throw Error("地名查询未能开始，坐标已保留。", ErrorKind::Validation);
            }
        }
        if (const auto name = locationNames_.poll()) {
            fillLocationName(*name);
            KillTimer(handle_, 6); text(LocateButton, L"当前位置");
        }
    } catch (const Error& error) {
        stopLocation(); setNotice(error.what());
        if (error.kind == ErrorKind::LocationDenied) ShowWindow(control(LocationSettingsButton), SW_SHOW);
    }
}
void Window::fillLocation(const LocationFix& fix) {
    if (view_ != View::Location) return;
    if (!validLocation({"当前位置", fix.latitude, fix.longitude}) || !std::isfinite(fix.accuracy) || fix.accuracy < 0)
        throw Error("Windows 返回了无效位置，可重试或手动填写。", ErrorKind::Validation);
    const auto latitude = coordinateText(fix.latitude, CoordinateFormat::Rounded);
    const auto longitude = coordinateText(fix.longitude, CoordinateFormat::Rounded);
    const auto name = text(LocationName);
    const bool replaceable = trim(utf8(name)).empty() || (!locationAutomaticName_.empty() && name == locationAutomaticName_);
    stopLocation();
    if (replaceable) { text(LocationName, L"当前位置"); locationAutomaticName_ = L"当前位置"; }
    text(LatitudeInput, latitude); text(LongitudeInput, longitude);
    locationNameBaseline_ = text(LocationName);
    locationLatitudeBaseline_ = latitude; locationLongitudeBaseline_ = longitude;
    locationNameReplaceable_ = replaceable; locationNameEdited_ = false;
    if (fix.isDefault) setNotice("Windows 默认位置，请核对。");
    else {
        const auto metres = static_cast<long long>(std::clamp(std::ceil(fix.accuracy), 1.0, 40075000.0));
        setNotice("精度约 " + std::to_string(metres) + " 米", true);
    }
    SetFocus(control(LocationName));
}
void Window::fillLocationName(const std::string& name) {
    if (view_ != View::Location || !locationNameReplaceable_ || locationNameEdited_
        || text(LocationName) != locationNameBaseline_ || text(LatitudeInput) != locationLatitudeBaseline_
        || text(LongitudeInput) != locationLongitudeBaseline_) return;
    if (!validLocation({name, 0, 0})) return;
    text(LocationName, wide(name));
    locationAutomaticName_ = wide(name);
    locationNameReplaceable_ = false;
}
void Window::record() {
    if (recorder_.active()) { finishRecording(false); return; }
    PlaySoundW(nullptr, nullptr, 0); recording_.clear();
    recorder_.start();
    SetTimer(handle_, 3, 100, nullptr);
    text(RecordButton, L"停止"); text(RecordTime, L"● 00:00");
    EnableWindow(control(PreviewButton), FALSE);
}
void Window::recordTick() {
    if (!recorder_.active()) return;
    bool full = false;
    try { full = recorder_.poll(); }
    catch (const Error& error) {
        KillTimer(handle_,3);
        try { finishRecording(false); } catch (...) {}
        setNotice(error.what()); return;
    }
    const auto seconds = recorder_.seconds();
    text(RecordTime, L"● " + std::to_wstring(seconds / 60) + L":" + (seconds % 60 < 10 ? L"0" : L"") + std::to_wstring(seconds % 60));
    if (full) finishRecording(false);
}
void Window::finishRecording(bool attach) {
    KillTimer(handle_, 3);
    if (recorder_.active()) {
        text(RecordButton, L"重新录制");
        recording_ = recorder_.finish();
        text(RecordButton, L"重新录制"); text(RecordTime, L"已录制 " + std::to_wstring((recording_.size() - 44) / 32000) + L" 秒");
        EnableWindow(control(PreviewButton), TRUE);
    }
    if (!attach) return;
    if (recording_.empty()) throw Error("先录一段，再添加。", ErrorKind::Validation);
    PlaySoundW(nullptr, nullptr, 0);
    SYSTEMTIME now{}; GetLocalTime(&now);
    wchar_t name[80]{};
    swprintf_s(name, L"voice-%04u%02u%02u-%02u%02u%02u.wav", now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond);
    cacheAttachment(utf8(name), "audio/wav", recording_, false);
    recording_.clear(); recording_.shrink_to_fit();
    setView(View::Compose);
}
} // namespace mq
