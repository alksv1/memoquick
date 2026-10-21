#pragma once
#include <windows.h>
#include <winhttp.h>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include <optional>
#include "i18n.hpp"

namespace mq {
enum class ErrorKind { Validation, Network, Auth, Protocol, Storage, Http, LocationDenied };
class Error : public std::runtime_error {
public:
    Error(std::string message, ErrorKind kind = ErrorKind::Http, unsigned status = 0)
        : std::runtime_error(std::move(message)), kind(kind), status(status) {}
    ErrorKind kind;
    unsigned status;
};
std::wstring wide(const std::string& text);
std::string utf8(const std::wstring& text);
std::string trim(std::string text);
struct Server {
    std::string url;
    std::wstring host;
    std::wstring basePath;
    std::wstring origin;
    INTERNET_PORT port = 0;
    bool secure = false;
};
Server parseServer(const std::string& value);

struct Cookie {
    std::string name;
    std::string value;
    std::string path = "/";
    bool secure = false;
    long long expires = 0;
    bool operator==(const Cookie&) const = default;
};
class CookieJar {
public:
    std::vector<Cookie> cookies;
    void receive(const std::string& header, const Server& server);
    std::string header(const std::wstring& requestPath, bool secure) const;
};
struct User {
    std::string name;
    std::string displayName;
    bool operator==(const User&) const = default;
};
inline constexpr size_t AttachmentSizeMax = 20 * 1024 * 1024;
struct Attachment {
    std::string name, filename, type, localId;
    bool inlineImage = false;
    bool operator==(const Attachment&) const = default;
};
struct MemoReference {
    std::string name, snippet;
    bool operator==(const MemoReference&) const = default;
};
struct MemoPage {
    std::vector<MemoReference> memos;
    std::string nextPage;
};
struct Location {
    std::string placeholder;
    double latitude = 0, longitude = 0;
    bool operator==(const Location&) const = default;
};
struct Draft {
    std::string content;
    std::string visibility = "PRIVATE";
    std::vector<Attachment> attachments;
    std::vector<MemoReference> relations;
    std::optional<Location> location;
    bool operator==(const Draft&) const = default;
};
bool validResource(const std::string& name, const std::string& prefix);
bool validLocation(const Location& location);
std::string encodeUrl(const std::string& value);
std::string imageMarkdown(const Server& server, const Attachment& attachment);
std::string fileMime(const std::filesystem::path& path);
Location parseLocation(const std::string& label, const std::string& latitude, const std::string& longitude);
struct Config {
    std::string server;
    unsigned modifiers = MOD_CONTROL | MOD_ALT;
    unsigned key = 'M';
    bool startAtLogin = false;
    bool dark = false;
    std::string geocodingEndpoint = "https://nominatim.openstreetmap.org/reverse";
    std::string language;
    bool operator==(const Config&) const = default;
};
struct HttpResponse {
    unsigned status = 0;
    std::string body;
    std::vector<std::string> cookies;
};
HttpResponse httpRequest(const Server& server, const std::wstring& path,
                         const std::string& method, const std::string& body,
                         const std::string& token, const std::string& cookies,
                         int timeoutMs = 10000);

class Client {
public:
    explicit Client(Server server, std::vector<Cookie> cookies = {},
                    std::function<void(const std::vector<Cookie>&)> persist = {});
    void discover();
    User signIn(const std::string& username, const std::string& password);
    User resume();
    std::string createMemo(const Draft& draft);
    Attachment uploadAttachment(const Attachment& attachment, const std::string& bytes);
    MemoPage listMemos(const std::string& search, const std::string& page = {});
    void signOut();
    const Server& server() const { return server_; }
    const User& user() const { return user_; }
    const std::vector<Cookie>& cookies() const { return jar_.cookies; }
    void setTimeout(int milliseconds) { timeoutMs_ = milliseconds; }
private:
    std::string request(const std::string& path, const std::string& method = "GET",
                        const std::string& body = {}, bool authenticated = false);
    void refresh();
    std::string authRequest(const std::string& procedure, const std::string& restPath,
                            const std::string& body = "{}", bool authenticated = false);
    std::string authenticated(const std::string& path, const std::string& method = "GET", const std::string& body = {});
    User currentUser();
    Server server_;
    CookieJar jar_;
    User user_;
    std::string token_;
    std::string api_;
    bool connectAuth_ = false;
    int creatorFilter_ = 0;
    int timeoutMs_ = 10000;
    std::function<void(const std::vector<Cookie>&)> persist_;
};
std::vector<unsigned char> protect(const std::string& plain);
std::string unprotect(const std::vector<unsigned char>& encrypted);
std::filesystem::path dataDirectory();
bool validDraft(const Draft& draft);
std::wstring hotkeyLabel(unsigned modifiers, unsigned key);
} // namespace mq
