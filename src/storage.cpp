#include "storage.hpp"
#include "json.hpp"
#include <shlobj.h>
#include <wincrypt.h>
#include <atomic>
#include <fstream>
#include <mutex>

namespace mq {
using Json = nlohmann::json;
namespace {
std::vector<unsigned char> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw Error("无法读取本地数据文件，原文件已保留。", ErrorKind::Storage);
    const auto size = file.tellg();
    if (size < 0 || size > 64 * 1024 * 1024) throw Error("本地数据文件大小异常，原文件已保留。", ErrorKind::Storage);
    std::vector<unsigned char> data(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) throw Error("读取本地数据失败，原文件已保留。", ErrorKind::Storage);
    return data;
}
void atomicWrite(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    static std::atomic<unsigned> sequence = 0;
    auto temporary = path;
    temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(sequence++);
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw Error("无法写入本地数据，请检查磁盘空间和目录权限。", ErrorKind::Storage);
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        throw Error("本地数据保存失败，请检查磁盘空间和目录权限。", ErrorKind::Storage);
    }
}
std::string keyFor(const std::string& server, const std::string& user) { return server + '\n' + user; }
} // namespace

std::vector<unsigned char> protect(const std::string& plain) {
    DATA_BLOB input{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"MemoQuick private data", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        throw Error("Windows 无法加密数据，请检查当前用户账户。", ErrorKind::Storage);
    }
    std::vector<unsigned char> result(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    return result;
}
std::string unprotect(const std::vector<unsigned char>& encrypted) {
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()), const_cast<BYTE*>(encrypted.data())};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        throw Error("无法解密会话或草稿，原文件已保留。请使用原 Windows 账户。", ErrorKind::Storage);
    }
    std::string result(reinterpret_cast<char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}
std::filesystem::path dataDirectory() {
    PWSTR location = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &location))) {
        throw Error("无法找到用户数据目录。", ErrorKind::Storage);
    }
    const auto path = std::filesystem::path(location) / L"MemoQuick";
    CoTaskMemFree(location);
    return path;
}
struct Store::Impl {
    std::filesystem::path directory;
    mutable std::mutex mutex;
    Config config;
    std::optional<WindowPosition> windowPosition;
    Json privateData = { {"sessions", Json::object()}, {"drafts", Json::object()} };
    void savePrivate(const Json& next) {
        atomicWrite(directory / L"private.bin", protect(next.dump()));
        privateData = next;
    }
};
Store::Store(std::filesystem::path directory) : impl_(std::make_unique<Impl>()) {
    impl_->directory = std::move(directory);
    std::filesystem::create_directories(impl_->directory);
    try {
        const auto windowPath = impl_->directory / L"window.json";
        if (std::filesystem::exists(windowPath)) {
            const auto value = Json::parse(readFile(windowPath));
            impl_->windowPosition = WindowPosition{value.at("x").get<int>(), value.at("y").get<int>()};
        }
        const auto configPath = impl_->directory / L"settings.json";
        if (std::filesystem::exists(configPath)) {
            const auto bytes = readFile(configPath);
            const auto value = Json::parse(bytes);
            impl_->config = { value.value("server", ""), value.value("modifiers", static_cast<unsigned>(MOD_CONTROL | MOD_ALT)),
                value.value("key", static_cast<unsigned>('M')), value.value("startAtLogin", false), value.value("theme", "light") == "dark" };
            if (!impl_->config.server.empty()) parseServer(impl_->config.server);
        }
        const auto privatePath = impl_->directory / L"private.bin";
        if (std::filesystem::exists(privatePath)) {
            const auto value = Json::parse(unprotect(readFile(privatePath)));
            if (!value.is_object() || !value.at("sessions").is_object() || !value.at("drafts").is_object()) {
                throw Error("本地数据结构异常，原文件已保留。", ErrorKind::Storage);
            }
            impl_->privateData = value;
        }
    } catch (const Error&) { throw; }
    catch (...) { throw Error("无法读取本地数据，原文件已保留。请先备份后检查。", ErrorKind::Storage); }
}
Store::~Store() = default;
Config Store::config() const { std::lock_guard lock(impl_->mutex); return impl_->config; }
std::optional<WindowPosition> Store::windowPosition() const {
    std::lock_guard lock(impl_->mutex); return impl_->windowPosition;
}
void Store::saveWindowPosition(WindowPosition position) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->windowPosition == position) return;
    const auto plain = Json{{"x", position.x}, {"y", position.y}}.dump(2);
    atomicWrite(impl_->directory / L"window.json", {plain.begin(), plain.end()});
    impl_->windowPosition = position;
}
void Store::saveConfig(const Config& config) {
    const Json value = { {"server", config.server}, {"modifiers", config.modifiers}, {"key", config.key}, {"startAtLogin", config.startAtLogin}, {"theme", config.dark ? "dark" : "light"} };
    const auto plain = value.dump(2);
    const std::vector<unsigned char> bytes(plain.begin(), plain.end());
    std::lock_guard lock(impl_->mutex);
    atomicWrite(impl_->directory / L"settings.json", bytes);
    impl_->config = config;
}
std::vector<Cookie> Store::cookies(const std::string& server) const {
    std::lock_guard lock(impl_->mutex);
    const auto& sessions = impl_->privateData.at("sessions");
    if (!sessions.contains(server)) return {};
    std::vector<Cookie> result;
    try {
        for (const auto& value : sessions.at(server)) {
            Cookie cookie{value.at("name").get<std::string>(), value.at("value").get<std::string>(),
                value.value("path", "/"), value.value("secure", false), value.value("expires", 0LL)};
            if (cookie.name.find_first_of("\r\n;=") != std::string::npos || cookie.value.find_first_of("\r\n;") != std::string::npos) {
                throw Error("会话数据无效，请重新登录。", ErrorKind::Storage);
            }
            result.push_back(std::move(cookie));
        }
    } catch (const Json::exception&) { throw Error("会话数据异常，原文件已保留。", ErrorKind::Storage); }
    return result;
}
void Store::saveCookies(const std::string& server, const std::vector<Cookie>& cookies) {
    Json serialized = Json::array();
    for (const auto& c : cookies) serialized.push_back({ {"name", c.name}, {"value", c.value}, {"path", c.path}, {"secure", c.secure}, {"expires", c.expires} });
    std::lock_guard lock(impl_->mutex);
    auto next = impl_->privateData;
    if (cookies.empty()) next["sessions"].erase(server);
    else next["sessions"][server] = std::move(serialized);
    impl_->savePrivate(next);
}
Draft Store::draft(const std::string& server, const std::string& user) const {
    std::lock_guard lock(impl_->mutex);
    const auto& drafts = impl_->privateData.at("drafts");
    const auto key = keyFor(server, user);
    if (!drafts.contains(key)) return {};
    try {
        const auto& value = drafts.at(key);
        Draft draft{value.at("content").get<std::string>(), value.value("visibility", "PRIVATE")};
        for (const auto& a : value.value("attachments", Json::array()))
            draft.attachments.push_back({a.value("name",""),a.at("filename").get<std::string>(),a.value("type","application/octet-stream"),a.value("localId",""),a.value("inlineImage",false)});
        for (const auto& r : value.value("relations", Json::array()))
            draft.relations.push_back({r.at("name").get<std::string>(),r.value("snippet","")});
        if (value.contains("location") && !value.at("location").is_null()) {
            const auto& l = value.at("location");
            draft.location = Location{l.at("placeholder").get<std::string>(),l.at("latitude").get<double>(),l.at("longitude").get<double>()};
        }
        if (!validDraft(draft)) throw Error("草稿内容无效，原文件已保留。", ErrorKind::Storage);
        return draft;
    } catch (const Json::exception&) { throw Error("草稿数据异常，原文件已保留。", ErrorKind::Storage); }
}
void Store::saveDraft(const std::string& server, const std::string& user, const Draft& draft) {
    if (!validDraft(draft)) throw Error("草稿内容无效。", ErrorKind::Validation);
    std::lock_guard lock(impl_->mutex);
    auto next = impl_->privateData;
    const auto key = keyFor(server, user);
    if (draft.content.empty() && draft.attachments.empty() && draft.relations.empty() && !draft.location) next["drafts"].erase(key);
    else {
        Json value{{"content",draft.content},{"visibility",draft.visibility},{"attachments",Json::array()},{"relations",Json::array()}};
        for (const auto& a : draft.attachments)
            value["attachments"].push_back({{"name",a.name},{"filename",a.filename},{"type",a.type},{"localId",a.localId},{"inlineImage",a.inlineImage}});
        for (const auto& r : draft.relations) value["relations"].push_back({{"name",r.name},{"snippet",r.snippet}});
        if (draft.location) value["location"] = {{"placeholder",draft.location->placeholder},{"latitude",draft.location->latitude},{"longitude",draft.location->longitude}};
        next["drafts"][key] = std::move(value);
    }
    impl_->savePrivate(next);
}
std::string Store::cacheAttachment(const std::string& server, const std::string& user, const std::string& bytes) {
    if (bytes.empty() || bytes.size() > AttachmentSizeMax) throw Error("附件需非空且不超过 20 MiB。", ErrorKind::Validation);
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) throw Error("无法创建附件缓存。", ErrorKind::Storage);
    wchar_t buffer[40]{}; StringFromGUID2(guid, buffer, 40);
    const auto id = utf8(std::wstring(buffer + 1, 36));
    std::lock_guard lock(impl_->mutex);
    std::filesystem::create_directories(impl_->directory / L"attachments");
    atomicWrite(impl_->directory / L"attachments" / (wide(id) + L".bin"), protect(keyFor(server, user) + '\n' + bytes));
    return id;
}
std::string Store::attachmentBytes(const std::string& server, const std::string& user, const std::string& id) const {
    if (!validResource(id, "")) throw Error("附件缓存标识无效。", ErrorKind::Storage);
    std::lock_guard lock(impl_->mutex);
    const auto value = unprotect(readFile(impl_->directory / L"attachments" / (wide(id) + L".bin")));
    const auto prefix = keyFor(server, user) + '\n';
    if (!value.starts_with(prefix) || value.size() - prefix.size() > AttachmentSizeMax)
        throw Error("附件缓存不属于当前账号，原文件已保留。", ErrorKind::Storage);
    return value.substr(prefix.size());
}
void Store::removeAttachment(const std::string& id) {
    if (!validResource(id, "")) return;
    std::lock_guard lock(impl_->mutex);
    std::error_code error;
    std::filesystem::remove(impl_->directory / L"attachments" / (wide(id) + L".bin"), error);
}
void setStartup(bool enabled) {
    HKEY key = nullptr;
    const auto status = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) throw Error("无法更新开机启动设置。", ErrorKind::Storage);
    LSTATUS result;
    if (enabled) {
        std::wstring executable(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length == executable.size()) { RegCloseKey(key); throw Error("无法确定程序路径。", ErrorKind::Storage); }
        executable.resize(length);
        const auto command = L"\"" + executable + L"\" --background";
        result = RegSetValueExW(key, L"MemoQuick", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, L"MemoQuick");
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) throw Error("无法更新开机启动设置。", ErrorKind::Storage);
}
} // namespace mq
