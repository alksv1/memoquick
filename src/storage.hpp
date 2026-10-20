#pragma once
#include "core.hpp"
#include <memory>
#include <optional>

namespace mq {
struct WindowPosition {
    int x = 0, y = 0;
    int width = 0, height = 0;
    bool operator==(const WindowPosition&) const = default;
};
class Store {
public:
    explicit Store(std::filesystem::path directory);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Config config() const;
    void saveConfig(const Config& config);
    std::optional<WindowPosition> windowPosition() const;
    void saveWindowPosition(WindowPosition position);
    std::vector<Cookie> cookies(const std::string& server) const;
    void saveCookies(const std::string& server, const std::vector<Cookie>& cookies);
    Draft draft(const std::string& server, const std::string& user) const;
    void saveDraft(const std::string& server, const std::string& user, const Draft& draft);
    std::string cacheAttachment(const std::string& server, const std::string& user, const std::string& bytes);
    std::string attachmentBytes(const std::string& server, const std::string& user, const std::string& id) const;
    void removeAttachment(const std::string& id);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
void setStartup(bool enabled, const std::wstring& registryPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run");
} // namespace mq
