#include "storage.hpp"
#include "markdown.hpp"
#include "audio.hpp"
#include "location.hpp"
#include "geocoding.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <objbase.h>

int clipboardTests();

namespace {
int checks = 0;
void check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F work, mq::ErrorKind kind) {
    try { work(); } catch (const mq::Error& error) { check(error.kind == kind, "Unexpected error kind"); return; }
    throw std::runtime_error("Expected rejection");
}
std::string fileContents(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void markdownTests() {
    using A = mq::MarkdownAction;
    const auto apply = [](const std::wstring& source, size_t start, size_t end, A action) {
        const auto edit = mq::formatMarkdown(source, start, end, action);
        return source.substr(0, edit.start) + edit.replacement + source.substr(edit.end);
    };
    check(apply(L"灵感📝", 0, 4, A::Bold) == L"**灵感📝**", "Unicode bold formatting failed");
    check(apply(L"**灵感📝**", 2, 6, A::Bold) == L"灵感📝", "Bold toggle lost selection");
    check(apply(L"**内容**", 2, 4, A::Italic) == L"***内容***", "Italic inside bold failed");
    check(apply(L"***内容***", 3, 5, A::Italic) == L"**内容**", "Italic toggle removed bold");
    check(apply(L"abc\r\ndef", 1, 1, A::Heading) == L"## abc\r\ndef", "Heading changed another line");
    check(apply(L"# abc", 0, 5, A::Heading) == L"## abc", "Heading conversion failed");
    check(apply(L"## abc", 0, 6, A::Heading) == L"abc", "Heading toggle failed");
    check(apply(L"甲\r\n乙\r\n丙", 0, 6, A::Bullet) == L"- 甲\r\n- 乙\r\n丙", "Selection ending at next line formatted that line");
    check(apply(L"- a\r\n- b", 0, 8, A::Numbered) == L"1. a\r\n2. b", "Numbered list conversion failed");
    check(apply(L"- a\r\n\r\n- b", 0, 10, A::Task) == L"- [ ] a\r\n\r\n- [ ] b", "Tasks changed paragraph spacing");
    check(apply(L"> a\r\n> b", 0, 8, A::Quote) == L"a\r\nb", "Quote toggle failed");
    check(apply(L"  ", 2, 2, A::Heading) == L"  ## ", "Empty indented line cannot be formatted");
    check(apply(L"研究", 0, 2, A::Link) == L"[研究](https://)", "Link lost selected text");
    check(apply(L"a`b", 0, 3, A::InlineCode) == L"``a`b``", "Inline code broke embedded backticks");
    check(apply(L"```\r\n示例", 0, 7, A::CodeBlock) == L"````\r\n```\r\n示例\r\n````", "Code fence closed on embedded backticks");
    const auto link = mq::formatMarkdown(L"研究", 0, 2, A::Link);
    check(link.replacement.substr(link.selectionStart, link.selectionEnd - link.selectionStart) == L"https://", "Link did not select URL placeholder");
    const auto empty = mq::formatMarkdown(L"", 0, 0, A::Bold);
    check(empty.replacement == L"**文字**" && empty.selectionStart == 2 && empty.selectionEnd == 4, "Empty bold did not select placeholder");
}
void i18nTests() {
    mq::i18n::resetToDefaults();
    const auto langs = mq::i18n::availableLanguages();
    check(langs.size() >= 2, "Available languages list is incomplete");
    bool hasEn = false, hasZh = false;
    for (const auto& lang : langs) {
        if (lang.id == "en") { hasEn = true; check(lang.name == L"English", "English display name incorrect"); }
        if (lang.id == "zh-CN") { hasZh = true; check(lang.name == L"简体中文", "Simplified Chinese display name incorrect"); }
    }
    check(hasEn && hasZh, "Missing standard language definitions");

    // English strings
    mq::i18n::setLanguage("en");
    check(mq::i18n::currentLanguage() == "en", "Active language not English");
    check(mq::i18n::tr("button.done") == "Done", "English translation incorrect");
    check(mq::i18n::tr("button.back") == "Back", "English translation incorrect");
    check(mq::i18n::tr("compose.save") == "Save", "English translation incorrect");
    check(mq::i18n::tr("login.signin") == "Sign In", "English translation incorrect");
    check(mq::i18n::trW("settings.title") == L"Settings", "English wide translation incorrect");

    // Simplified Chinese strings
    mq::i18n::setLanguage("zh-CN");
    check(mq::i18n::currentLanguage() == "zh-CN", "Active language not Chinese");
    check(mq::i18n::tr("button.done") == "完成", "Chinese translation incorrect");
    check(mq::i18n::tr("button.back") == "返回", "Chinese translation incorrect");
    check(mq::i18n::tr("compose.save") == "保存", "Chinese translation incorrect");
    check(mq::i18n::tr("login.signin") == "登录", "Chinese translation incorrect");
    check(mq::i18n::trW("settings.title") == L"设置", "Chinese wide translation incorrect");

    // Custom language registration and fallback to English
    mq::i18n::registerLanguage(
        mq::i18n::LanguageInfo{"es", L"Español", {"es"}},
        {{"button.done", "Hecho"}}
    );
    mq::i18n::setLanguage("es");
    check(mq::i18n::currentLanguage() == "es", "Custom language not active");
    check(mq::i18n::tr("button.done") == "Hecho", "Custom language string failed");
    check(mq::i18n::tr("button.back") == "Back", "Missing translation did not fall back to English");
    check(mq::i18n::tr("nonexistent.key.xyz") == "nonexistent.key.xyz", "Unknown key did not return key name");

    // Formatting with placeholders
    mq::i18n::setLanguage("en");
    check(mq::i18n::trFormat("location.accuracy", "120") == "Accuracy ~120 m", "trFormat English failed");
    mq::i18n::setLanguage("zh-CN");
    check(mq::i18n::trFormat("location.accuracy", "120") == "精度约 120 米", "trFormat Chinese failed");

    // System default resolution
    mq::i18n::resetToDefaults();
    mq::i18n::setLanguage("");
    check(!mq::i18n::currentLanguage().empty(), "System default did not resolve a language");
}
void unitTests() {
    checks += clipboardTests();
    i18nTests();
    // GIVEN provider responses and configurable URLs, WHEN validated, THEN malformed names and unsafe endpoints are rejected.
    check(mq::parseLocationName(R"({"display_name":" 中国，广东省，深圳市 "})") == "中国，广东省，深圳市", "Address name was not normalized");
    for (const auto body : {"{}", "[]", "not json", R"({"display_name":42})", R"({"display_name":"  "})"})
        rejects([&] { mq::parseLocationName(body); }, mq::ErrorKind::Network);
    rejects([] { mq::parseLocationName(std::string(65537,'x')); }, mq::ErrorKind::Network);
    rejects([] { mq::geocodingUrl("https://example.com/reverse?key=secret",0,0); }, mq::ErrorKind::Validation);
    rejects([] { mq::geocodingUrl("http://example.com/reverse",0,0); }, mq::ErrorKind::Validation);
    check(mq::geocodingUrl(" https://example.com/custom/reverse ",-22.3,114.2)
        == "https://example.com/custom/reverse?format=json&accept-language=zh-CN&lat=-22.300000&lon=114.200000", "Custom provider URL lost its path or coordinates");

    // GIVEN a native location fix, WHEN coordinates are shown and submitted,
    // THEN signed values roundtrip through the real location parser with a decimal point.
    check(mq::parseLocation("当前位置", mq::utf8(mq::coordinateText(-22.123456)), mq::utf8(mq::coordinateText(114.654321)))
        == mq::Location{"当前位置", -22.123456, 114.654321}, "Automatic coordinates did not roundtrip");
    check(mq::coordinateText(0.000001, mq::CoordinateFormat::Rounded) == L"0.000001", "Small coordinates lost decimal precision");
    const double precise = 114.12345678901234;
    check(mq::parseLocation("手动位置", "0", mq::utf8(mq::coordinateText(precise))).longitude == precise,
          "Reopening a manual location reduced its precision");
    rejects([] { mq::coordinateText(std::numeric_limits<double>::infinity()); }, mq::ErrorKind::Validation);
    rejects([] { mq::coordinateText(std::numeric_limits<double>::quiet_NaN()); }, mq::ErrorKind::Validation);
    // GIVEN an unused reader, WHEN cancelled twice, THEN it remains idle without
    // initializing WinRT, asking for permission or accessing the device's location.
    mq::LocationReader idle;
    idle.cancel(); idle.cancel();
    check(!idle.active() && !idle.poll(), "An idle location reader acquired a position");
    // GIVEN an isolated real registry key, WHEN startup is enabled, repaired and disabled,
    // THEN the quoted executable uses background mode and removal is idempotent.
    const auto registryPath = L"Software\\MemoQuick.Tests\\" + std::to_wstring(GetCurrentProcessId());
    HKEY startupKey = nullptr;
    try {
        mq::setStartup(true, registryPath);
        check(RegOpenKeyExW(HKEY_CURRENT_USER, registryPath.c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &startupKey) == ERROR_SUCCESS, "Startup key missing");
        wchar_t executable[32768]{};
        check(GetModuleFileNameW(nullptr, executable, 32768) != 0, "Cannot query test executable");
        const auto expected = L"\"" + std::wstring(executable) + L"\" --background";
        const auto readStartup = [&] {
            wchar_t value[32768]{}; DWORD bytes = sizeof(value);
            check(RegGetValueW(startupKey, nullptr, L"MemoQuick", RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS, "Startup value missing");
            return std::wstring(value);
        };
        check(readStartup() == expected, "Startup command is not quoted or background-only");
        const wchar_t stale[] = L"\"C:\\old folder\\MemoQuick.exe\" --background";
        check(RegSetValueExW(startupKey, L"MemoQuick", 0, REG_SZ, reinterpret_cast<const BYTE*>(stale), sizeof(stale)) == ERROR_SUCCESS, "Cannot create stale startup fixture");
        mq::setStartup(true, registryPath);
        check(readStartup() == expected, "Moved executable did not repair stale startup path");
        mq::setStartup(false, registryPath); mq::setStartup(false, registryPath);
        DWORD bytes = 0;
        check(RegGetValueW(startupKey, nullptr, L"MemoQuick", RRF_RT_REG_SZ, nullptr, nullptr, &bytes) == ERROR_FILE_NOT_FOUND, "Disabling startup retained its value");
    } catch (...) {
        if (startupKey) RegCloseKey(startupKey);
        RegDeleteKeyW(HKEY_CURRENT_USER, registryPath.c_str());
        throw;
    }
    RegCloseKey(startupKey); RegDeleteKeyW(HKEY_CURRENT_USER, registryPath.c_str());
    // GIVEN structured compose metadata, WHEN validated, THEN coordinates and resource boundaries are enforced.
    check(mq::parseLocation(" 赤道 ","0","180") == mq::Location{"赤道",0,180}, "Location boundary parse failed");
    rejects([] { mq::parseLocation("地点","nan","1"); }, mq::ErrorKind::Validation);
    rejects([] { mq::parseLocation("地点","91","1"); }, mq::ErrorKind::Validation);
    rejects([] { mq::parseLocation("地点","1x","1"); }, mq::ErrorKind::Validation);
    rejects([] { mq::parseLocation(" ","1","1"); }, mq::ErrorKind::Validation);
    check(!mq::validResource("attachments/../secret","attachments/"), "Attachment traversal accepted");
    const mq::Attachment picture{"attachments/image-1","图[片].png","image/png"};
    check(mq::imageMarkdown(mq::parseServer("https://example.com/notes/"),picture) == "![图\\[片\\].png](/notes/file/attachments/image-1/image)", "Image Markdown did not escape labels or retain proxy path");
    const auto wave = mq::waveFile(std::string("\x01\x00\xff\x7f",4));
    const std::string expectedHeader("RIFF\x28\x00\x00\x00WAVEfmt \x10\x00\x00\x00\x01\x00\x01\x00\x80\x3e\x00\x00\x00\x7d\x00\x00\x02\x00\x10\x00" "data\x04\x00\x00\x00",44);
    check(wave == expectedHeader + std::string("\x01\x00\xff\x7f",4), "PCM WAV serialization failed");
    rejects([] { mq::waveFile(""); }, mq::ErrorKind::Validation);
    rejects([] { mq::waveFile("x"); }, mq::ErrorKind::Validation);
    markdownTests();
    // GIVEN Unicode notes, WHEN converted both ways, THEN all characters survive.
    const std::string unicode = "你好 📝\r\n#想法";
    check(mq::utf8(mq::wide(unicode)) == unicode, "UTF-8 roundtrip failed");
    rejects([] { mq::wide(std::string("\xff", 1)); }, mq::ErrorKind::Validation);

    // GIVEN user-entered server URLs, WHEN normalized, THEN paths and origin are retained.
    check(mq::parseServer("  https://MEMOS.example.com/notes/api/v1/ ").url == "https://memos.example.com/notes/", "URL normalization failed");
    check(mq::parseServer("http://localhost:5230").url == "http://localhost:5230/", "Local URL failed");
    check(mq::parseServer("http://[::1]:5230/").url == "http://[::1]:5230/", "IPv6 URL failed");
    for (const auto* invalid : {"", "localhost:5230", "ftp://example.com/", "https://user:secret@example.com/", "https://example.com/?token=x", "https://example.com/#memo"}) {
        rejects([&] { mq::parseServer(invalid); }, mq::ErrorKind::Validation);
    }

    // GIVEN refresh cookies, WHEN rotated or scoped, THEN only matching live cookies are sent.
    mq::CookieJar jar;
    const auto server = mq::parseServer("https://memos.example.com/");
    jar.receive("memos_refresh=first; Path=/api; HttpOnly; Secure", server);
    check(jar.header(L"/api/auth/refresh", true) == "memos_refresh=first", "Cookie missing");
    check(jar.header(L"/apix", true).empty(), "Cookie path boundary ignored");
    check(jar.header(L"/api", false).empty(), "Secure cookie sent over HTTP");
    jar.receive("memos_refresh=second; Path=/api; HttpOnly; Secure", server);
    check(jar.header(L"/api/auth/refresh", true) == "memos_refresh=second", "Cookie rotation failed");
    jar.receive("evil=secret; Domain=evil.example.net; Path=/", server);
    check(jar.cookies.size() == 1, "Foreign cookie accepted");
    jar.receive("memos_refresh=deleted; Path=/api; Max-Age=0; Expires=Wed, 01 Jan 2098 00:00:00 GMT", server);
    check(jar.cookies.empty(), "Max-Age did not override Expires");
    jar.receive("old=deleted; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT", server);
    check(jar.cookies.empty(), "Expired cookie accepted");
    // GIVEN Memos' Expires-only refresh cookie, WHEN received, THEN it survives a new client session.
    jar.receive("memos_refresh=future; Path=/; HttpOnly; Expires=Wed, 01 Jan 2098 00:00:00 GMT; SameSite=Lax; Secure", server);
    check(jar.header(L"/api/v1/auth/refresh", true) == "memos_refresh=future", "Expires-only refresh cookie was dropped");

    // GIVEN private data, WHEN encrypted with Windows DPAPI, THEN plaintext is absent and tampering fails.
    const std::string secret = "unique-secret-marker-草稿";
    auto encrypted = mq::protect(secret);
    check(mq::unprotect(encrypted) == secret, "DPAPI roundtrip failed");
    const std::string bytes(encrypted.begin(), encrypted.end());
    check(bytes.find(secret) == std::string::npos, "DPAPI stored plaintext");
    encrypted.back() ^= 1;
    rejects([&] { mq::unprotect(encrypted); }, mq::ErrorKind::Storage);

    // GIVEN saved sessions and drafts, WHEN restarted, THEN accounts remain isolated and deletion persists.
    const auto directory = std::filesystem::temp_directory_path() / (L"memoquick-core-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const mq::Draft draft{secret, "PRIVATE"};
    const std::vector<mq::Cookie> cookies{{"memos_refresh", "unique-refresh-value", "/", true, 0}};
    mq::Config config; config.server = server.url; config.dark = true; config.geocodingEndpoint = "https://maps.example.com/reverse";
    {
        mq::Store store(directory);
        check(!store.windowPosition(), "New installation already has a window position");
        store.saveWindowPosition({-640, 240, 520, 360});
        store.saveConfig(config); store.saveCookies(server.url, cookies);
        store.saveDraft(server.url, "users/alice", draft);
    }
    {
        mq::Store store(directory);
        check(store.config() == config, "Settings did not persist");
        check(store.windowPosition() == mq::WindowPosition{-640, 240, 520, 360}, "Negative monitor coordinates did not persist");
        check(store.cookies(server.url) == cookies, "Session did not persist");
        check(store.draft(server.url, "users/alice") == draft, "Draft did not persist");
        check(store.draft(server.url, "users/bob") == mq::Draft{}, "Draft crossed account");
        check(store.draft("https://another.example/", "users/alice") == mq::Draft{}, "Draft crossed server");
        const auto privateBytes = fileContents(directory / L"private.bin");
        check(privateBytes.find(secret) == std::string::npos && privateBytes.find("unique-refresh-value") == std::string::npos, "Private data was written as plaintext");
        store.saveDraft(server.url, "users/alice", {}); store.saveCookies(server.url, {});
    }
    {
        mq::Store store(directory);
        check(store.draft(server.url, "users/alice") == mq::Draft{} && store.cookies(server.url).empty(), "Private state deletion did not persist");
    }
    // GIVEN an attachment-only draft, WHEN restarted, THEN encrypted bytes and all metadata survive within their account.
    mq::Draft extended;
    std::string cached;
    {
        mq::Store store(directory);
        cached = store.cacheAttachment(server.url,"users/alice",wave);
        extended.attachments = {{"","voice.wav","audio/wav",cached}};
        extended.relations = {{"memos/reference-1","参考"}};
        extended.location = mq::Location{"地点",22.3,114.2};
        store.saveDraft(server.url,"users/alice",extended);
        const auto stored = fileContents(directory / L"attachments" / (mq::wide(cached) + L".bin"));
        check(stored.find("RIFF") == std::string::npos, "Audio cache was not encrypted");
    }
    {
        mq::Store store(directory);
        check(store.draft(server.url,"users/alice") == extended, "Attachment-only draft metadata did not survive restart");
        check(store.attachmentBytes(server.url,"users/alice",cached) == wave, "Cached audio changed");
        rejects([&] { store.attachmentBytes(server.url,"users/bob",cached); }, mq::ErrorKind::Storage);
        rejects([&] { store.attachmentBytes("https://other.example/","users/alice",cached); }, mq::ErrorKind::Storage);
        rejects([&] { store.attachmentBytes(server.url,"users/alice","../private"); }, mq::ErrorKind::Storage);
        auto invalid = extended; invalid.attachments.push_back(extended.attachments.front());
        check(!mq::validDraft(invalid), "Duplicate attachment accepted");
        store.saveDraft(server.url,"users/alice",{}); store.removeAttachment(cached);
        check(!std::filesystem::exists(directory / L"attachments" / (mq::wide(cached) + L".bin")), "Committed cache cleanup failed");
    }
    // GIVEN the 0.1 draft format, WHEN loaded by 0.2, THEN text and visibility remain intact.
    {
        const auto old = mq::protect("{\"sessions\":{},\"drafts\":{\"https://memos.example.com/\\nusers/alice\":{\"content\":\"legacy\",\"visibility\":\"PROTECTED\"}}}");
        std::ofstream file(directory / L"private.bin",std::ios::binary); file.write(reinterpret_cast<const char*>(old.data()),static_cast<std::streamsize>(old.size()));
    }
    {
        mq::Store store(directory);
        check(store.draft(server.url,"users/alice") == mq::Draft{"legacy","PROTECTED"}, "Legacy draft migration lost content");
    }
    // GIVEN old settings without a theme, WHEN upgraded, THEN Light is used and private data remains intact.
    { std::ofstream(directory / L"settings.json") << "{\"server\":\"https://memos.example.com/\"}"; }
    {
        mq::Store store(directory);
        check(!store.config().dark && store.config().server == server.url, "Legacy settings did not default to Light");
        auto disabled = store.config(); disabled.geocodingEndpoint.clear(); store.saveConfig(disabled);
    }
    {
        mq::Store store(directory); check(!store.config().dark && store.config().geocodingEndpoint.empty(), "Theme or disabled geocoding did not persist");
        check(store.windowPosition() == mq::WindowPosition{-640, 240, 520, 360}, "Settings update lost window position");
        check(store.config().language.empty(), "Legacy settings did not default to empty language (system)");
        auto withLang = store.config();
        withLang.language = "en";
        store.saveConfig(withLang);
    }
    {
        mq::Store store(directory);
        check(store.config().language == "en", "Saved language 'en' did not persist");
        auto withZh = store.config();
        withZh.language = "zh-CN";
        store.saveConfig(withZh);
    }
    {
        mq::Store store(directory);
        check(store.config().language == "zh-CN", "Saved language 'zh-CN' did not persist");
    }
    // GIVEN legacy window.json with only x and y, WHEN loaded, THEN width and height default to 0.
    {
        std::ofstream(directory / L"window.json") << "{\"x\": 150, \"y\": 220}";
        mq::Store store(directory);
        check(store.windowPosition() == mq::WindowPosition{150, 220, 0, 0}, "Legacy window.json migration failed");
    }
    // GIVEN corrupted storage, WHEN opened, THEN the original bytes are kept.
    { std::ofstream(directory / L"settings.json") << "{corrupted"; }
    rejects([&] { mq::Store store(directory); }, mq::ErrorKind::Storage);
    check(fileContents(directory / L"settings.json") == "{corrupted", "Corrupted settings were overwritten");
    check(std::filesystem::canonical(directory).parent_path() == std::filesystem::canonical(std::filesystem::temp_directory_path()), "Cleanup target escaped the test directory");
    std::filesystem::remove_all(directory);
}
void apiTests(const std::string& url) {
    // GIVEN a real Nominatim-compatible endpoint, WHEN polled on a UI thread,
    // THEN Unicode names are decoded, repeated coordinates are cached, and failures remain recoverable.
    const auto waitForName = [](mq::LocationNames& names) {
        const auto started = GetTickCount64();
        while (GetTickCount64() - started < 10000) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            if (const auto name = names.poll()) return *name;
            Sleep(10);
        }
        throw std::runtime_error("Geocoding fixture timed out");
    };
    {
        mq::LocationNames names;
        names.start(url + "reverse", 22.3, 114.2);
        check(waitForName(names) == "中国，广东省，深圳市，南山区", "Reverse geocoding lost the address name");
        names.start(url + "reverse", 22.3, 114.2);
        check(names.poll() == "中国，广东省，深圳市，南山区", "Repeated location missed the cache");
    }
    for (const auto suffix : {"reverse-empty", "reverse-error", "reverse-large", "reverse-redirect"}) {
        mq::LocationNames names;
        names.start(url + suffix, 0, 0);
        rejects([&] { waitForName(names); }, mq::ErrorKind::Network);
        check(!names.active(), "Failed geocoding retained an active request");
    }
    {
        mq::LocationNames names;
        names.start(url + "reverse", 0, 0); names.cancel(); names.cancel();
        check(!names.active() && !names.poll(), "Cancelled address lookup returned a stale name");
    }
    // GIVEN a real HTTP fixture, WHEN logging in and refreshing, THEN credentials and Unicode notes reach the API.
    const auto server = mq::parseServer(url);
    // GIVEN multiple Set-Cookie fields, WHEN WinHTTP sizes and reads each field, THEN none are skipped.
    const auto headers = mq::httpRequest(server, server.basePath + L"cookie-headers", "GET", {}, {}, {});
    check(headers.cookies == std::vector<std::string>{
        "memos_refresh=header-probe; Path=/; HttpOnly; Expires=Wed, 01 Jan 2098 00:00:00 GMT",
        "auxiliary=second; Path=/; Max-Age=86400"}, "WinHTTP skipped a Set-Cookie header");
    std::vector<mq::Cookie> persisted;
    mq::Client client(server, {}, [&](const auto& cookies) { persisted = cookies; });
    check(client.signIn("alice", "test-password") == mq::User{"users/7", "Alice"}, "Sign-in failed");
    const mq::Draft draft{"你好 📝\r\n#测试\r\n- [ ] 待办", "PRIVATE"};
    check(client.createMemo(draft) == "memos/fixture", "Create after token refresh failed");
    mq::Client restarted(server, persisted);
    check(restarted.resume() == mq::User{"users/7", "Alice"}, "Session did not survive restart");
    rejects([&] { restarted.createMemo({"FAIL", "PRIVATE"}); }, mq::ErrorKind::Http);
    restarted.setTimeout(200);
    rejects([&] { restarted.createMemo({"TIMEOUT", "PRIVATE"}); }, mq::ErrorKind::Network);
    restarted.setTimeout(10000);
    // GIVEN actual JSON endpoints, WHEN uploading and linking, THEN binary bytes, references and location reach the same memo mutation.
    const std::string binary(2 * 1024 * 1024 + 17,'x');
    auto image = restarted.uploadAttachment({"","图[片].png","image/png"},binary);
    check(image.name == "attachments/fixture-1", "Image upload failed");
    auto voice = restarted.uploadAttachment({"","voice.wav","audio/wav"},mq::waveFile(std::string(3200,'\0')));
    check(voice.name == "attachments/fixture-2", "Audio upload failed");
    rejects([&] { restarted.uploadAttachment({"","fail.bin","application/octet-stream"},"failure"); }, mq::ErrorKind::Http);
    const auto page = restarted.listMemos("灵感\" &");
    check(page.memos == std::vector<mq::MemoReference>{{"memos/reference-1","参考一"}} && page.nextPage == "next +/=", "Memo search leaked other users or archived notes");
    check(restarted.listMemos("灵感\" &",page.nextPage).memos == std::vector<mq::MemoReference>{{"memos/reference-2","参考二"}}, "Memo pagination failed");
    mq::Draft rich{"rich\n" + mq::imageMarkdown(server,image),"PRIVATE"};
    rich.attachments = {image,voice}; rich.relations = page.memos; rich.location = mq::Location{"赤道",0,0};
    check(restarted.createMemo(rich) == "memos/fixture", "Structured memo save failed");
    restarted.signOut();
    check(restarted.cookies().empty(), "Sign-out retained local cookies");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct ApartmentGuard { bool initialized; ~ApartmentGuard() { if (initialized) CoUninitialize(); } } apartmentGuard{SUCCEEDED(apartment)};
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--api") apiTests(mq::utf8(argv[2]));
        else if (argc == 4 && std::wstring(argv[1]) == L"--session-seed") {
            const auto server = mq::parseServer(mq::utf8(argv[2]));
            mq::Store store(argv[3]);
            mq::Config config; config.server = server.url;
            store.saveConfig(config);
            mq::Client client(server, {}, [&](const auto& cookies) { store.saveCookies(server.url, cookies); });
            check(client.signIn("alice", "test-password") == mq::User{"users/7", "Alice"}, "Persistent sign-in failed");
            check(!store.cookies(server.url).empty(), "Sign-in did not persist refresh credentials");
            store.saveDraft(server.url, "users/7", {"重启后保留的灵感 📝", "PROTECTED"});
        }
        else unitTests();
        std::cout << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
