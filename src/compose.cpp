#include "core.hpp"
#include "json.hpp"
#include <wincrypt.h>
#include <algorithm>
#include <cmath>
#include <set>
#include <cctype>

namespace mq {
using Json = nlohmann::json;
namespace {
Json responseJson(const std::string& value) {
    try { return Json::parse(value); }
    catch (const Json::exception&) { throw Error("服务返回了无效数据。", ErrorKind::Protocol); }
}
std::string base64(const std::string& bytes) {
    DWORD length = 0;
    const auto data = reinterpret_cast<const BYTE*>(bytes.data());
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length))
        throw Error("无法编码附件。", ErrorKind::Storage);
    std::string result(length, '\0');
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &length))
        throw Error("无法编码附件。", ErrorKind::Storage);
    result.resize(length);
    return result;
}
long long integer(const Json& value) {
    try {
        if (value.is_string()) return std::stoll(value.get<std::string>());
        return value.get<long long>();
    } catch (...) { throw Error("上传进度无效。", ErrorKind::Protocol); }
}
} // namespace
bool validResource(const std::string& name, const std::string& prefix) {
    if (!name.starts_with(prefix) || name.size() <= prefix.size() || name.size() > prefix.size() + 64) return false;
    return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(prefix.size()), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    });
}
bool validLocation(const Location& location) {
    return !trim(location.placeholder).empty() && location.placeholder.size() <= 500
        && std::isfinite(location.latitude) && std::isfinite(location.longitude)
        && location.latitude >= -90 && location.latitude <= 90 && location.longitude >= -180 && location.longitude <= 180;
}
Location parseLocation(const std::string& label, const std::string& latitude, const std::string& longitude) {
    Location result; result.placeholder = trim(label);
    try {
        const auto lat = trim(latitude), lon = trim(longitude);
        size_t latEnd = 0, lonEnd = 0;
        result.latitude = std::stod(lat, &latEnd); result.longitude = std::stod(lon, &lonEnd);
        if (latEnd != lat.size() || lonEnd != lon.size() || !validLocation(result)) throw std::invalid_argument("location");
    } catch (...) { throw Error("请填写名称、纬度（−90～90）和经度（−180～180）。", ErrorKind::Validation); }
    return result;
}
bool validDraft(const Draft& draft) {
    if (draft.content.size() > 2'000'000 || (draft.visibility != "PRIVATE" && draft.visibility != "PROTECTED" && draft.visibility != "PUBLIC")
        || draft.attachments.size() > 32 || draft.relations.size() > 32 || (draft.location && !validLocation(*draft.location))) return false;
    std::set<std::string> names;
    for (const auto& a : draft.attachments) {
        if ((!a.name.empty() && !validResource(a.name, "attachments/")) || (!a.localId.empty() && !validResource(a.localId, ""))
            || (a.name.empty() && a.localId.empty()) || a.filename.empty() || a.filename.size() > 1024 || a.type.size() > 128
            || (a.inlineImage && !a.type.starts_with("image/")) || !names.insert(a.name.empty() ? a.localId : a.name).second) return false;
    }
    names.clear();
    for (const auto& r : draft.relations)
        if (!validResource(r.name, "memos/") || r.snippet.size() > 4096 || !names.insert(r.name).second) return false;
    return true;
}
std::string encodeUrl(const std::string& value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') result += static_cast<char>(c);
        else { result += '%'; result += digits[c >> 4]; result += digits[c & 15]; }
    }
    return result;
}
std::string imageMarkdown(const Server& server, const Attachment& attachment) {
    if (!validResource(attachment.name, "attachments/") || !attachment.type.starts_with("image/"))
        throw Error("图片附件无效。", ErrorKind::Validation);
    std::string label;
    for (const char c : attachment.filename) {
        if (c == '\r' || c == '\n') { label += ' '; continue; }
        if (c == '[' || c == ']' || c == '\\') label += '\\';
        label += c;
    }
    // Canonical managed paths avoid presigned S3 URLs and work with proxy subpaths.
    return "![" + label + "](" + utf8(server.basePath) + "file/" + attachment.name + "/image)";
}
std::string fileMime(const std::filesystem::path& path) {
    auto extension = utf8(path.extension().wstring());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::pair<const char*, const char*> types[] = {{".png","image/png"},{".jpg","image/jpeg"},{".jpeg","image/jpeg"},{".gif","image/gif"},
        {".webp","image/webp"},{".bmp","image/bmp"},{".svg","image/svg+xml"},{".avif","image/avif"},{".heic","image/heic"},
        {".pdf","application/pdf"},{".txt","text/plain"},{".md","text/markdown"},{".wav","audio/wav"},{".mp3","audio/mpeg"},
        {".m4a","audio/mp4"},{".ogg","audio/ogg"},{".webm","video/webm"},{".mp4","video/mp4"},{".zip","application/zip"}};
    for (const auto& [key, value] : types) if (extension == key) return value;
    return "application/octet-stream";
}
std::string Client::authenticated(const std::string& path, const std::string& method, const std::string& body) {
    if (token_.empty()) refresh();
    try { return request(path, method, body, true); }
    catch (const Error& e) {
        if (e.status != 401) throw;
        token_.clear(); refresh();
        return request(path, method, body, true);
    }
}
Attachment Client::uploadAttachment(const Attachment& attachment, const std::string& bytes) {
    if (bytes.empty() || bytes.size() > AttachmentSizeMax || attachment.filename.empty())
        throw Error("附件需非空且不超过 20 MiB。", ErrorKind::Validation);
    discover();
    const Json metadata{{"filename",attachment.filename},{"type",attachment.type}};
    Json uploaded;
    bool legacy = false;
    Json initial;
    try {
        initial = responseJson(authenticated(api_ + "/attachments:upload", "POST",
            Json{{"spec",{{"attachment",metadata},{"totalSize",std::to_string(bytes.size())}}}}.dump()));
    } catch (const Error& e) {
        // Only a missing route may fall back. Uncertain writes are never replayed.
        if (e.status != 404 && e.status != 405 && e.status != 501) throw;
        legacy = true;
    }
    if (legacy) {
        auto body = metadata; body["content"] = base64(bytes);
        uploaded = responseJson(authenticated(api_ + "/attachments", "POST", body.dump()));
    } else {
        const auto id = initial.value("uploadId", "");
        const auto maximum = integer(initial.value("maxChunkSize", Json(0)));
        if (id.empty() || maximum <= 0 || integer(initial.value("committedSize", Json(0))) != 0)
            throw Error("无法初始化附件上传。", ErrorKind::Protocol);
        const auto chunk = std::min<size_t>(static_cast<size_t>(maximum), 2 * 1024 * 1024);
        for (size_t offset = 0; offset < bytes.size();) {
            const auto end = std::min(offset + chunk, bytes.size());
            const Json body{{"uploadId",id},{"writeOffset",std::to_string(offset)},
                {"data",base64(bytes.substr(offset, end - offset))},{"finishWrite",end == bytes.size()}};
            const auto part = responseJson(authenticated(api_ + "/attachments:upload", "POST", body.dump()));
            if (integer(part.value("committedSize", Json(-1))) != static_cast<long long>(end))
                throw Error("附件上传进度不一致，请重试。", ErrorKind::Protocol);
            if (end == bytes.size()) uploaded = part.value("attachment", Json::object());
            offset = end;
        }
    }
    auto result = attachment;
    result.name = uploaded.value("name", "");
    result.filename = uploaded.value("filename", attachment.filename);
    result.type = uploaded.value("type", attachment.type);
    if (!validResource(result.name, "attachments/")) throw Error("无法确认附件上传结果，本地文件已保留。", ErrorKind::Protocol);
    return result;
}
MemoPage Client::listMemos(const std::string& search, const std::string& page) {
    discover();
    if (search.size() > 1024 || page.size() > 4096) throw Error("搜索内容过长。", ErrorKind::Validation);
    const auto query = [&] {
        std::string filter;
        if (creatorFilter_ == 0) filter = "creator == " + Json(user_.name).dump();
        if (creatorFilter_ == 1) filter = "creator_id == " + user_.name.substr(6);
        if (!search.empty()) filter += (filter.empty() ? "" : " && ") + std::string("content.contains(") + Json(search).dump() + ")";
        return api_ + "/memos?pageSize=50&state=NORMAL&pageToken=" + encodeUrl(page) + "&filter=" + encodeUrl(filter);
    };
    Json result;
    try { result = responseJson(authenticated(query())); }
    catch (const Error& error) {
        if (error.status != 400 || creatorFilter_ != 0) throw;
        const auto id = user_.name.starts_with("users/") ? user_.name.substr(6) : "";
        creatorFilter_ = !id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }) ? 1 : 2;
        result = responseJson(authenticated(query()));
    }
    MemoPage output; output.nextPage = result.value("nextPageToken", "");
    for (const auto& memo : result.value("memos", Json::array())) {
        const auto name = memo.value("name", "");
        if (memo.value("creator", "") != user_.name || memo.value("state", "NORMAL") != "NORMAL" || !validResource(name, "memos/")) continue;
        auto snippet = memo.value("snippet", "");
        if (snippet.empty()) snippet = memo.value("content", "");
        auto snippetWide = wide(snippet);
        if (snippetWide.size() > 240) snippetWide.resize(240 - (snippetWide[239] >= 0xd800 && snippetWide[239] <= 0xdbff ? 1 : 0));
        output.memos.push_back({name, utf8(snippetWide)});
    }
    return output;
}
} // namespace mq
