#include "window.hpp"
#include <commdlg.h>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <wrl/client.h>

namespace mq {
bool Window::pasteClipboard() {
    if (view_ != View::Compose || busy_ || !client_) return false;
    Microsoft::WRL::ComPtr<IDataObject> source;
    if (FAILED(OleGetClipboard(&source)) || !source) throw Error(i18n::tr("error.clipboard_temp_unavail"), ErrorKind::Validation);
    return pasteAttachments(*source.Get());
}
bool Window::pasteAttachments(IDataObject& source) {
    if (view_ != View::Compose || busy_ || !client_) return false;
    const auto server = config_.server, user = user_.name;
    setBusy(true);
    std::vector<ClipboardAttachment> items;
    try { items = clipboardAttachments(source); }
    catch (...) { setBusy(false); throw; }
    setBusy(false);
    if (items.empty()) return false;
    // Delayed OLE providers can dispatch messages while supplying their bytes.
    if (busy_ || view_ != View::Compose || config_.server != server || user_.name != user)
        throw Error(i18n::tr("error.paste_cancelled"), ErrorKind::Validation);
    auto draft = currentDraft();
    if (items.size() > 32 - draft.attachments.size()) throw Error(i18n::tr("error.max_32_attachments"), ErrorKind::Validation);
    std::vector<std::string> cached;
    cached.reserve(items.size()); draft.attachments.reserve(draft.attachments.size() + items.size());
    bool images = false;
    try {
        for (auto& item : items) {
            auto id = store_.cacheAttachment(server, user, item.bytes);
            cached.push_back(std::move(id));
            const bool image = item.type.starts_with("image/"); images = images || image;
            draft.attachments.push_back({{}, std::move(item.filename), std::move(item.type), cached.back(), image});
        }
        // Commit the complete batch before starting any upload. A failed read or
        // local write leaves the existing draft and source clipboard untouched.
        store_.saveDraft(server, user, draft);
    } catch (...) {
        for (const auto& id : cached) store_.removeAttachment(id);
        throw;
    }
    extras_ = std::move(draft); draftDirty_ = false; updateMetadata(); setNotice("");
    if (images) uploadPending(true);
    else SetFocus(control(Editor));
    return true;
}
void Window::insertMenu() {
    if (busy_ || view_ != View::Compose) return;
    const auto menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, InsertAttachment, i18n::trW("insert.attachment").c_str());
    AppendMenuW(menu, MF_STRING, InsertImage, i18n::trW("insert.image").c_str());
    AppendMenuW(menu, MF_STRING, InsertAudio, i18n::trW("insert.audio").c_str());
    AppendMenuW(menu, MF_STRING, InsertMemo, i18n::trW("insert.memo").c_str());
    AppendMenuW(menu, MF_STRING, InsertLocation, i18n::trW("insert.location").c_str());
    RECT rect{}; GetWindowRect(control(AddButton), &rect);
    const auto command = trackMenu(menu, TPM_LEFTALIGN | TPM_BOTTOMALIGN, rect.left, rect.top);
    if (command) SendMessageW(handle_, WM_COMMAND, MAKEWPARAM(command, BN_CLICKED), 0);
}
void Window::chooseFiles(bool image) {
    std::vector<wchar_t> names(65536);
    OPENFILENAMEW chooser{}; chooser.lStructSize = sizeof(chooser); chooser.hwndOwner = handle_;
    chooser.lpstrFile = names.data(); chooser.nMaxFile = static_cast<DWORD>(names.size());
    std::vector<wchar_t> filter;
    if (image) {
        const auto imgLabel = i18n::trW("dialog.images");
        const auto allLabel = i18n::trW("dialog.all_files");
        const wchar_t imgExt[] = L"*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp;*.svg;*.avif;*.heic";
        const wchar_t allExt[] = L"*.*";
        filter.insert(filter.end(), imgLabel.begin(), imgLabel.end()); filter.push_back(L'\0');
        filter.insert(filter.end(), imgExt, imgExt + wcslen(imgExt)); filter.push_back(L'\0');
        filter.insert(filter.end(), allLabel.begin(), allLabel.end()); filter.push_back(L'\0');
        filter.insert(filter.end(), allExt, allExt + wcslen(allExt)); filter.push_back(L'\0');
        filter.push_back(L'\0');
    } else {
        const auto allLabel = i18n::trW("dialog.all_files");
        const wchar_t allExt[] = L"*.*";
        filter.insert(filter.end(), allLabel.begin(), allLabel.end()); filter.push_back(L'\0');
        filter.insert(filter.end(), allExt, allExt + wcslen(allExt)); filter.push_back(L'\0');
        filter.push_back(L'\0');
    }
    chooser.lpstrFilter = filter.data();
    chooser.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&chooser)) {
        if (CommDlgExtendedError()) throw Error(i18n::tr("error.choose_file_failed"), ErrorKind::Storage);
        return;
    }
    std::vector<std::filesystem::path> paths;
    const std::filesystem::path first(names.data());
    auto next = names.data() + wcslen(names.data()) + 1;
    if (!*next) paths.push_back(first);
    else while (*next) { paths.push_back(first / next); next += wcslen(next) + 1; }
    for (const auto& path : paths) {
        const auto type = fileMime(path);
        if (image && !type.starts_with("image/")) throw Error(i18n::tr("error.choose_image_only"), ErrorKind::Validation);
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) throw Error(i18n::tr("error.open_attach_failed"), ErrorKind::Storage);
        const auto size = file.tellg();
        if (size <= 0 || size > static_cast<std::streamoff>(AttachmentSizeMax)) throw Error(i18n::tr("error.attachment_size_limit"), ErrorKind::Validation);
        std::string bytes(static_cast<size_t>(size), '\0'); file.seekg(0);
        if (!file.read(bytes.data(), size)) throw Error(i18n::tr("error.read_attach_failed"), ErrorKind::Storage);
        cacheAttachment(utf8(path.filename().wstring()), type, bytes, image);
    }
    if (image) uploadPending(true);
    else SetFocus(control(Editor));
}
void Window::cacheAttachment(const std::string& filename, const std::string& type, const std::string& bytes, bool image) {
    if (extras_.attachments.size() >= 32) throw Error(i18n::tr("error.max_32_attachments"), ErrorKind::Validation);
    const auto id = store_.cacheAttachment(config_.server, user_.name, bytes);
    extras_.attachments.push_back({{}, filename, type, id, image});
    draftDirty_ = true;
    updateMetadata();
    if (!flushDraft()) {
        extras_.attachments.pop_back(); store_.removeAttachment(id); updateMetadata();
        throw Error(i18n::tr("error.draft_attach_failed"), ErrorKind::Storage);
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
    if (!extras_.attachments.empty()) {
        const auto n = extras_.attachments.size();
        const auto key = n == 1 ? "metadata.attachment_singular" : "metadata.attachments";
        caption = i18n::trFormatW(key, std::to_wstring(n));
    }
    if (!extras_.relations.empty()) {
        const auto n = extras_.relations.size();
        const auto key = n == 1 ? "metadata.relation_singular" : "metadata.relations";
        caption += (caption.empty() ? L"" : L" · ") + i18n::trFormatW(key, std::to_wstring(n));
    }
    if (extras_.location) caption += (caption.empty() ? L"" : L" · ") + i18n::trW("metadata.location");
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
        AppendMenuW(menu, MF_STRING, RetryAttachments, i18n::trW("metadata.retry_upload").c_str());
    for (size_t i = 0; i < extras_.attachments.size(); ++i) {
        const auto& a = extras_.attachments[i];
        const auto label = i18n::trW("metadata.remove_prefix") + safeLabel(a.filename) + (a.name.empty() ? i18n::trW("metadata.pending") : L"");
        AppendMenuW(menu, MF_STRING, 2000 + i, label.c_str());
    }
    for (size_t i = 0; i < extras_.relations.size(); ++i)
        AppendMenuW(menu, MF_STRING, 3000 + i, (i18n::trW("metadata.unlink_prefix") + safeLabel(extras_.relations[i].snippet)).c_str());
    if (extras_.location) {
        AppendMenuW(menu, MF_STRING, InsertLocation, (i18n::trW("metadata.edit_location_prefix") + safeLabel(extras_.location->placeholder)).c_str());
        AppendMenuW(menu, MF_STRING, 4000, i18n::trW("metadata.remove_location").c_str());
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
            if (extras_.relations.size() >= 32) throw Error(i18n::tr("error.max_32_memos"), ErrorKind::Validation);
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
    text(LocateButton, i18n::trW("location.current"));
    for (const auto id : {LatitudeInput, LongitudeInput, PanelConfirm}) EnableWindow(control(id), !busy_);
}
void Window::locate() {
    if (busy_ || view_ != View::Location) return;
    if (locationReader_.active() || locationNames_.active()) { stopLocation(); setNotice(""); return; }
    setNotice(""); ShowWindow(control(LocationSettingsButton), SW_HIDE);
    try {
        locationReader_.start();
        text(LocateButton, i18n::trW("location.cancel_locating"));
        for (const auto id : {LatitudeInput, LongitudeInput, PanelConfirm}) EnableWindow(control(id), FALSE);
        if (!SetTimer(handle_, 6, 100, nullptr)) throw Error(i18n::tr("error.location_unavailable"), ErrorKind::Validation);
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
                text(LocateButton, i18n::trW("location.cancel_lookup"));
                if (!SetTimer(handle_, 6, 100, nullptr)) throw Error(i18n::tr("error.geocoding_failed"), ErrorKind::Validation);
            }
        }
        if (const auto name = locationNames_.poll()) {
            fillLocationName(*name);
            KillTimer(handle_, 6); text(LocateButton, i18n::trW("location.current"));
        }
    } catch (const Error& error) {
        stopLocation(); setNotice(error.what());
        if (error.kind == ErrorKind::LocationDenied) ShowWindow(control(LocationSettingsButton), SW_SHOW);
    }
}
void Window::fillLocation(const LocationFix& fix) {
    if (view_ != View::Location) return;
    if (!validLocation({"location", fix.latitude, fix.longitude}) || !std::isfinite(fix.accuracy) || fix.accuracy < 0)
        throw Error(i18n::tr("error.location_bad_os_return"), ErrorKind::Validation);
    const auto latitude = coordinateText(fix.latitude, CoordinateFormat::Rounded);
    const auto longitude = coordinateText(fix.longitude, CoordinateFormat::Rounded);
    const auto name = text(LocationName);
    const auto defaultName = i18n::trW("location.current");
    const bool replaceable = trim(utf8(name)).empty() || (!locationAutomaticName_.empty() && name == locationAutomaticName_);
    stopLocation();
    if (replaceable) { text(LocationName, defaultName); locationAutomaticName_ = defaultName; }
    text(LatitudeInput, latitude); text(LongitudeInput, longitude);
    locationNameBaseline_ = text(LocationName);
    locationLatitudeBaseline_ = latitude; locationLongitudeBaseline_ = longitude;
    locationNameReplaceable_ = replaceable; locationNameEdited_ = false;
    if (fix.isDefault) setNotice(i18n::tr("location.default_pos"));
    else {
        const auto metres = static_cast<long long>(std::clamp(std::ceil(fix.accuracy), 1.0, 40075000.0));
        setNotice(i18n::trFormat("location.accuracy", std::to_string(metres)), true);
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
    text(RecordButton, i18n::trW("record.stop")); text(RecordTime, L"● 00:00");
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
        text(RecordButton, i18n::trW("record.again"));
        recording_ = recorder_.finish();
        text(RecordButton, i18n::trW("record.again"));
        text(RecordTime, i18n::trFormatW("record.duration", std::to_wstring((recording_.size() - 44) / 32000)));
        EnableWindow(control(PreviewButton), TRUE);
    }
    if (!attach) return;
    if (recording_.empty()) throw Error(i18n::tr("record.need_record"), ErrorKind::Validation);
    PlaySoundW(nullptr, nullptr, 0);
    SYSTEMTIME now{}; GetLocalTime(&now);
    wchar_t name[80]{};
    swprintf_s(name, L"voice-%04u%02u%02u-%02u%02u%02u.wav", now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond);
    cacheAttachment(utf8(name), "audio/wav", recording_, false);
    recording_.clear(); recording_.shrink_to_fit();
    setView(View::Compose);
}
} // namespace mq
