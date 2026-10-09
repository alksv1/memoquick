#include "core.hpp"
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <memory>
#include <sstream>

namespace mq {
using Json = nlohmann::json;
namespace {
struct InternetDeleter { void operator()(void* handle) const { if (handle) WinHttpCloseHandle(handle); } };
using Internet = std::unique_ptr<void, InternetDeleter>;
long long nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
Json parseJson(const std::string& body) {
    const auto value = Json::parse(body, nullptr, false);
    if (!value.is_object()) throw Error("服务返回了非 API 响应，请确认地址指向 Memos。", ErrorKind::Protocol);
    return value;
}
std::string field(const Json& value, const char* key) {
    const auto it = value.find(key);
    return it != value.end() && it->is_string() ? it->get<std::string>() : "";
}
std::wstring queryHeader(HINTERNET handle, DWORD info, DWORD* index = nullptr) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(handle, info, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, index);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes > 65536) return {};
    std::wstring result(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(handle, info, WINHTTP_HEADER_NAME_BY_INDEX, result.data(), &bytes, index)) return {};
    result.resize(bytes / sizeof(wchar_t));
    while (!result.empty() && result.back() == L'\0') result.pop_back();
    return result;
}
void networkError(bool creating) {
    throw Error(creating
        ? "连接中断或超时，服务端可能已保存。请先到 Memos 确认再重试；草稿已保留。"
        : "无法连接 Memos，请检查地址、网络、系统代理或 HTTPS 证书。", ErrorKind::Network);
}
} // namespace

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) throw Error("文本不是有效的 UTF-8。", ErrorKind::Validation);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw Error("文本包含无效字符。", ErrorKind::Validation);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \r\n\t");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \r\n\t") - first + 1);
}
Server parseServer(const std::string& value) {
    const auto input = wide(trim(value));
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (input.empty() || !WinHttpCrackUrl(input.c_str(), 0, 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
        !parts.dwHostNameLength || parts.dwUserNameLength || parts.dwPasswordLength || parts.dwExtraInfoLength) {
        throw Error("请输入完整的 HTTP / HTTPS 服务地址，不能含账号、查询参数或片段。", ErrorKind::Validation);
    }
    Server server;
    server.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    server.basePath.assign(parts.lpszUrlPath ? parts.lpszUrlPath : L"", parts.dwUrlPathLength);
    while (!server.basePath.empty() && server.basePath.back() == L'/') server.basePath.pop_back();
    for (const auto* suffix : { L"/api/v1", L"/api" }) {
        if (server.basePath.ends_with(suffix)) { server.basePath.resize(server.basePath.size() - wcslen(suffix)); break; }
    }
    server.basePath += L'/';
    server.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    server.port = parts.nPort;
    auto host = server.host;
    std::transform(host.begin(), host.end(), host.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (host.find(L':') != std::wstring::npos && !host.starts_with(L'[')) host = L"[" + host + L"]";
    server.origin = (server.secure ? L"https://" : L"http://") + host;
    if (server.port != (server.secure ? 443 : 80)) server.origin += L":" + std::to_wstring(server.port);
    server.url = utf8(server.origin + server.basePath);
    return server;
}

void CookieJar::receive(const std::string& headerValue, const Server& server) {
    std::istringstream input(headerValue);
    std::string item;
    std::getline(input, item, ';');
    const auto equals = item.find('=');
    if (equals == std::string::npos || !equals) return;
    Cookie cookie{trim(item.substr(0, equals)), trim(item.substr(equals + 1))};
    if (cookie.name.find_first_of("\r\n \t,") != std::string::npos || cookie.value.find_first_of("\r\n") != std::string::npos) return;
    bool rejected = false;
    bool hasMaxAge = false;
    long long maxAge = 0;
    while (std::getline(input, item, ';')) {
        const auto split = item.find('=');
        const auto key = lower(trim(item.substr(0, split)));
        const auto val = split == std::string::npos ? "" : trim(item.substr(split + 1));
        if (key == "path" && val.starts_with('/')) cookie.path = val;
        if (key == "secure") cookie.secure = true;
        if (key == "domain") {
            auto domain = lower(val);
            if (domain.starts_with('.')) domain.erase(0, 1);
            const auto host = lower(utf8(server.host));
            if (domain.empty() || (host != domain && !host.ends_with("." + domain))) rejected = true;
        }
        if (key == "max-age") {
            try { size_t used = 0; maxAge = std::stoll(val, &used); hasMaxAge = used == val.size(); } catch (...) { /* Ignore invalid age. */ }
        }
        if (key == "expires") {
            SYSTEMTIME time{};
            FILETIME file{};
            if (WinHttpTimeToSystemTime(wide(val).c_str(), &time) && SystemTimeToFileTime(&time, &file)) {
                ULARGE_INTEGER ticks{};
                ticks.LowPart = file.dwLowDateTime; ticks.HighPart = file.dwHighDateTime;
                cookie.expires = static_cast<long long>(ticks.QuadPart / 10000000ULL) - 11644473600LL;
                if (!cookie.expires) cookie.expires = -1;
            }
        }
    }
    if (rejected || (cookie.secure && !server.secure)) return;
    if (hasMaxAge) cookie.expires = nowSeconds() + std::clamp(maxAge, -1LL, 315360000LL);
    std::erase_if(cookies, [&](const Cookie& c) { return c.name == cookie.name && c.path == cookie.path; });
    if (!cookie.value.empty() && (!cookie.expires || cookie.expires > nowSeconds())) cookies.push_back(std::move(cookie));
}
std::string CookieJar::header(const std::wstring& requestPath, bool secure) const {
    const auto path = utf8(requestPath);
    std::string result;
    for (const auto& cookie : cookies) {
        if ((cookie.expires && cookie.expires <= nowSeconds()) || (cookie.secure && !secure)) continue;
        if (path != cookie.path && !path.starts_with(cookie.path.ends_with('/') ? cookie.path : cookie.path + '/')) continue;
        if (!result.empty()) result += "; ";
        result += cookie.name + '=' + cookie.value;
    }
    return result;
}

HttpResponse httpRequest(const Server& server, const std::wstring& path,
                         const std::string& method, const std::string& body,
                         const std::string& token, const std::string& cookies, int timeoutMs) {
    const bool creating = method == "POST" && path.ends_with(L"/memos");
    Internet session(WinHttpOpen(L"MemoQuick/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0));
    if (!session) networkError(creating);
    if (!WinHttpSetTimeouts(session.get(), timeoutMs, timeoutMs, timeoutMs, timeoutMs)) networkError(creating);
    Internet connection(WinHttpConnect(session.get(), server.host.c_str(), server.port, 0));
    if (!connection) networkError(creating);
    const auto verb = wide(method);
    Internet request(WinHttpOpenRequest(connection.get(), verb.c_str(), path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, server.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) networkError(creating);
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    DWORD disabled = WINHTTP_DISABLE_COOKIES;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) ||
        !WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled))) networkError(creating);
    std::wstring headers = L"Accept: application/json\r\nOrigin: " + server.origin + L"\r\n";
    if (!body.empty()) headers += L"Content-Type: application/json; charset=utf-8\r\n";
    if (!token.empty()) headers += L"Authorization: Bearer " + wide(token) + L"\r\n";
    if (!cookies.empty()) headers += L"Cookie: " + wide(cookies) + L"\r\n";
    const auto started = GetTickCount64();
    if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
            body.empty() ? nullptr : const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) networkError(creating);
    HttpResponse response;
    DWORD status = 0, bytes = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX)) networkError(creating);
    response.status = status;
    DWORD index = 0;
    for (;;) {
        const auto cookie = queryHeader(request.get(), WINHTTP_QUERY_SET_COOKIE, &index);
        if (cookie.empty()) break;
        response.cookies.push_back(utf8(cookie));
    }
    for (;;) {
        DWORD available = 0;
        if (GetTickCount64() - started > static_cast<ULONGLONG>(timeoutMs) * 2) networkError(creating);
        if (!WinHttpQueryDataAvailable(request.get(), &available)) networkError(creating);
        if (!available) break;
        if (response.body.size() + available > 4 * 1024 * 1024) throw Error("服务返回的数据过大。", ErrorKind::Protocol);
        const auto offset = response.body.size();
        response.body.resize(offset + available);
        DWORD received = 0;
        if (!WinHttpReadData(request.get(), response.body.data() + offset, available, &received)) networkError(creating);
        response.body.resize(offset + received);
        if (!received) break;
    }
    return response;
}

Client::Client(Server server, std::vector<Cookie> cookies, std::function<void(const std::vector<Cookie>&)> persist)
    : server_(std::move(server)), persist_(std::move(persist)) { jar_.cookies = std::move(cookies); }

std::string Client::request(const std::string& path, const std::string& method, const std::string& body, bool authenticated) {
    const auto fullPath = server_.basePath + wide(path);
    const auto response = httpRequest(server_, fullPath, method, body, authenticated ? token_ : "", jar_.header(fullPath, server_.secure), timeoutMs_);
    if (!response.cookies.empty()) {
        for (const auto& cookie : response.cookies) jar_.receive(cookie, server_);
        if (persist_) {
            try { persist_(jar_.cookies); }
            catch (...) {
                if (method == "POST" && path.ends_with("/memos") && response.status >= 200 && response.status < 300) {
                    throw Error("记录可能已保存，但本地会话更新失败。请先到 Memos 确认；草稿已保留。", ErrorKind::Storage);
                }
                throw;
            }
        }
    }
    if (response.status < 200 || response.status >= 300) {
        if (response.status == 401) throw Error("登录已过期或凭据无效，请重新登录；草稿已保留。", ErrorKind::Auth, 401);
        if (response.status == 403) throw Error("服务拒绝了请求，请检查账号权限或是否允许密码登录。", ErrorKind::Http, 403);
        if (response.status == 429) throw Error("请求过于频繁，请稍后重试。", ErrorKind::Http, 429);
        if (response.status >= 300 && response.status < 400) throw Error("服务地址发生重定向，请填写跳转后的最终地址。", ErrorKind::Http, response.status);
        throw Error("Memos 返回错误（" + std::to_string(response.status) + "）。请稍后检查；草稿已保留。", ErrorKind::Http, response.status);
    }
    return response.body.empty() ? "{}" : response.body;
}
void Client::discover() {
    if (!api_.empty()) return;
    for (const auto& prefix : { "api/v1", "api" }) {
        try {
            const auto profile = parseJson(request(std::string(prefix) + "/instance/profile"));
            const auto version = field(profile, "version");
            if (version.empty()) continue;
            int major = 0, minor = 0;
            const auto value = version.starts_with('v') ? version.substr(1) : version;
            if (sscanf_s(value.c_str(), "%d.%d", &major, &minor) == 2 && major == 0 && minor < 26) {
                throw Error("需要 Memos 0.26 或更新版本。", ErrorKind::Validation);
            }
            api_ = prefix;
            return;
        } catch (const Error& error) {
            if (error.kind != ErrorKind::Protocol && error.status != 404 && error.status != 410) throw;
        }
    }
    throw Error("未找到兼容 API。请检查服务地址，并使用 Memos 0.26 或更新版本。", ErrorKind::Protocol);
}
User Client::signIn(const std::string& username, const std::string& password) {
    if (trim(username).empty() || password.empty()) throw Error("请填写用户名和密码。", ErrorKind::Validation);
    discover();
    const Json body = { {"passwordCredentials", {{"username", trim(username)}, {"password", password}}} };
    const auto result = parseJson(request(api_ + "/auth/signin", "POST", body.dump()));
    token_ = field(result, "accessToken");
    if (token_.empty()) token_ = field(result, "access_token");
    if (token_.empty()) throw Error("服务未返回登录会话，请检查 Memos 版本。", ErrorKind::Protocol);
    return currentUser();
}
void Client::refresh() {
    discover();
    const auto result = parseJson(request(api_ + "/auth/refresh", "POST", "{}"));
    token_ = field(result, "accessToken");
    if (token_.empty()) token_ = field(result, "access_token");
    if (token_.empty()) throw Error("无法恢复登录，请重新登录。", ErrorKind::Auth);
}
User Client::currentUser() {
    const auto result = parseJson(request(api_ + "/auth/me", "GET", {}, true));
    const auto it = result.find("user");
    if (it == result.end() || !it->is_object()) throw Error("无法确认登录账号，请重新登录。", ErrorKind::Auth);
    user_ = { field(*it, "name"), field(*it, "displayName") };
    if (user_.name.empty()) throw Error("无法确认登录账号，请重新登录。", ErrorKind::Auth);
    if (user_.displayName.empty()) user_.displayName = field(*it, "username");
    if (user_.displayName.empty()) user_.displayName = user_.name;
    return user_;
}
User Client::resume() { refresh(); return currentUser(); }
bool validDraft(const Draft& draft) {
    return draft.content.size() <= 2'000'000 && (draft.visibility == "PRIVATE" || draft.visibility == "PROTECTED" || draft.visibility == "PUBLIC");
}
std::string Client::createMemo(const Draft& draft) {
    if (!validDraft(draft) || trim(draft.content).empty()) throw Error("请填写有效内容并选择可见范围。", ErrorKind::Validation);
    discover();
    if (token_.empty()) refresh();
    const Json body = { {"content", draft.content}, {"visibility", draft.visibility} };
    std::string result;
    try { result = request(api_ + "/memos", "POST", body.dump(), true); }
    catch (const Error& error) {
        // Retry only a definite authentication rejection, never an uncertain write.
        if (error.status != 401) throw;
        token_.clear();
        refresh();
        result = request(api_ + "/memos", "POST", body.dump(), true);
    }
    const auto name = field(parseJson(result), "name");
    if (!name.starts_with("memos/")) throw Error("无法确认保存结果，请先到 Memos 检查；草稿已保留。", ErrorKind::Protocol);
    return name;
}
void Client::signOut() {
    try {
        if (!api_.empty()) request(api_ + "/auth/signout", "POST", "{}", true);
    } catch (...) {
        token_.clear(); user_ = {}; jar_.cookies.clear();
        if (persist_) persist_(jar_.cookies);
        throw;
    }
    token_.clear(); user_ = {}; jar_.cookies.clear();
    if (persist_) persist_(jar_.cookies);
}
std::wstring hotkeyLabel(unsigned modifiers, unsigned key) {
    std::wstring result;
    if (modifiers & MOD_CONTROL) result += L"Ctrl + ";
    if (modifiers & MOD_ALT) result += L"Alt + ";
    if (modifiers & MOD_SHIFT) result += L"Shift + ";
    if (modifiers & MOD_WIN) result += L"Win + ";
    if (key >= 'A' && key <= 'Z') result += static_cast<wchar_t>(key);
    else if (key >= '0' && key <= '9') result += static_cast<wchar_t>(key);
    else if (key >= VK_F1 && key <= VK_F24) result += L"F" + std::to_wstring(key - VK_F1 + 1);
    else result += L"Key " + std::to_wstring(key);
    return result;
}
} // namespace mq
