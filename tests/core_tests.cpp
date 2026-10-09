#include "storage.hpp"
#include "markdown.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>

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
void unitTests() {
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
    mq::Config config; config.server = server.url; config.dark = true;
    {
        mq::Store store(directory);
        check(!store.windowPosition(), "New installation already has a window position");
        store.saveWindowPosition({-640, 240});
        store.saveConfig(config); store.saveCookies(server.url, cookies);
        store.saveDraft(server.url, "users/alice", draft);
    }
    {
        mq::Store store(directory);
        check(store.config() == config, "Settings did not persist");
        check(store.windowPosition() == mq::WindowPosition{-640, 240}, "Negative monitor coordinates did not persist");
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
    // GIVEN old settings without a theme, WHEN upgraded, THEN Light is used and private data remains intact.
    { std::ofstream(directory / L"settings.json") << "{\"server\":\"https://memos.example.com/\"}"; }
    {
        mq::Store store(directory);
        check(!store.config().dark && store.config().server == server.url, "Legacy settings did not default to Light");
        store.saveConfig(store.config());
    }
    {
        mq::Store store(directory); check(!store.config().dark, "Light mode did not persist");
        check(store.windowPosition() == mq::WindowPosition{-640, 240}, "Settings update lost window position");
    }
    // GIVEN corrupted storage, WHEN opened, THEN the original bytes are kept.
    { std::ofstream(directory / L"settings.json") << "{corrupted"; }
    rejects([&] { mq::Store store(directory); }, mq::ErrorKind::Storage);
    check(fileContents(directory / L"settings.json") == "{corrupted", "Corrupted settings were overwritten");
    check(std::filesystem::canonical(directory).parent_path() == std::filesystem::canonical(std::filesystem::temp_directory_path()), "Cleanup target escaped the test directory");
    std::filesystem::remove_all(directory);
}
void apiTests(const std::string& url) {
    // GIVEN a real HTTP fixture, WHEN logging in and refreshing, THEN credentials and Unicode notes reach the API.
    const auto server = mq::parseServer(url);
    std::vector<mq::Cookie> persisted;
    mq::Client client(server, {}, [&](const auto& cookies) { persisted = cookies; });
    check(client.signIn("alice", "test-password") == mq::User{"users/alice", "Alice"}, "Sign-in failed");
    const mq::Draft draft{"你好 📝\r\n#测试\r\n- [ ] 待办", "PRIVATE"};
    check(client.createMemo(draft) == "memos/fixture", "Create after token refresh failed");
    mq::Client restarted(server, persisted);
    check(restarted.resume() == mq::User{"users/alice", "Alice"}, "Session did not survive restart");
    rejects([&] { restarted.createMemo({"FAIL", "PRIVATE"}); }, mq::ErrorKind::Http);
    restarted.setTimeout(200);
    rejects([&] { restarted.createMemo({"TIMEOUT", "PRIVATE"}); }, mq::ErrorKind::Network);
    restarted.setTimeout(10000);
    restarted.signOut();
    check(restarted.cookies().empty(), "Sign-out retained local cookies");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--api") apiTests(argv[2]);
        else unitTests();
        std::cout << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
