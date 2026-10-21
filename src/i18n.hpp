#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace mq::i18n {

struct LanguageInfo {
    std::string id;                         // e.g. "en", "zh-CN"
    std::wstring name;                      // e.g. L"English", L"简体中文"
    std::vector<std::string> prefixes;      // e.g. {"en"}, {"zh", "zh-cn", "zh-hans"}
};

// Returns available languages in display order.
std::vector<LanguageInfo> availableLanguages();

// Returns the display name of a language id, or empty string if unknown.
std::wstring languageName(std::string_view id);

// Detects the system language. Returns matching language id (e.g. "zh-CN"), falling back to "en".
std::string detectSystemLanguage();

// Sets active language. Empty or "system" resolves to detectSystemLanguage().
void setLanguage(const std::string& lang);

// Returns active resolved language id (e.g. "zh-CN" or "en").
std::string currentLanguage();

// Returns configured language preference (empty means system default).
std::string configuredLanguage();

// Translates a key to UTF-8 string, falling back to English, then to key itself.
std::string tr(std::string_view key);

// Translates a key to UTF-16 wide string.
std::wstring trW(std::string_view key);

// Translates a key and replaces "{}" or "{0}" with arg.
std::string trFormat(std::string_view key, const std::string& arg);
std::wstring trFormatW(std::string_view key, const std::wstring& arg);

// Registers a language definition with its translation dictionary.
void registerLanguage(LanguageInfo info, std::unordered_map<std::string, std::string> strings);

// Resets language registry to built-in defaults (useful for tests).
void resetToDefaults();

} // namespace mq::i18n
