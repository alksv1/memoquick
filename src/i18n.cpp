#include "i18n.hpp"
#include "core.hpp"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <mutex>

namespace mq::i18n {
namespace {

struct LanguageEntry {
    LanguageInfo info;
    std::unordered_map<std::string, std::string> strings;
};

const std::unordered_map<std::string, std::string> zhCnStrings = {
    // General & Navigation
    {"app.name", "MemoQuick"},
    {"app.slogan", "随手记下"},
    {"theme.toggle", "切换 Light / Dark"},
    {"header.settings", "设置"},
    {"header.close", "收起 · Esc"},
    {"button.back", "返回"},
    {"button.cancel", "取消"},
    {"button.done", "完成"},
    {"button.add", "添加"},

    // Login View
    {"login.server", "服务地址"},
    {"login.server_placeholder", "https://memos.example.com"},
    {"login.username", "用户名"},
    {"login.password", "密码"},
    {"login.signin", "登录"},
    {"login.connecting", "连接中…"},
    {"login.resume", "恢复登录"},

    // Compose View
    {"compose.visibility.private", "仅自己"},
    {"compose.visibility.protected", "已登录用户"},
    {"compose.visibility.public", "公开"},
    {"compose.add_tooltip", "添加附件、图片、录音、关联或位置"},
    {"compose.metadata_tooltip", "管理附件、关联与位置"},
    {"compose.save", "保存"},
    {"compose.saving", "保存中…"},
    {"compose.uploading", "上传中…"},
    {"compose.save_tooltip", "保存 · Ctrl + Enter"},

    // Markdown Toolbar Tooltips & Placeholders
    {"markdown.bold", "加粗 · Ctrl + B"},
    {"markdown.italic", "斜体 · Ctrl + I"},
    {"markdown.heading", "标题 · Ctrl + Shift + H"},
    {"markdown.link", "链接 · Ctrl + K"},
    {"markdown.bullet", "无序列表 · Ctrl + Shift + L"},
    {"markdown.numbered", "有序列表 · Ctrl + Shift + O"},
    {"markdown.task", "待办 · Ctrl + Shift + T"},
    {"markdown.quote", "引用 · Ctrl + Shift + Q"},
    {"markdown.inline_code", "行内代码 · Ctrl + E"},
    {"markdown.code_block", "代码块 · Ctrl + Shift + C"},
    {"markdown.placeholder.text", "文字"},
    {"markdown.placeholder.code", "代码"},

    // Insert Menu
    {"insert.attachment", "添加附件"},
    {"insert.image", "插入图片"},
    {"insert.audio", "录音"},
    {"insert.memo", "关联 Memo"},
    {"insert.location", "添加位置"},

    // Metadata Menu & Summary
    {"metadata.retry_upload", "重试上传"},
    {"metadata.remove_prefix", "移除 "},
    {"metadata.pending", "（待上传）"},
    {"metadata.unlink_prefix", "取消关联 "},
    {"metadata.edit_location_prefix", "编辑位置 "},
    {"metadata.remove_location", "移除位置"},
    {"metadata.attachment_singular", "{} 附件"},
    {"metadata.attachments", "{} 附件"},
    {"metadata.relation_singular", "{} 关联"},
    {"metadata.relations", "{} 关联"},
    {"metadata.location", "位置"},

    // Settings View
    {"settings.title", "设置"},
    {"settings.hotkey", "快捷键"},
    {"settings.hotkey_press", "按下快捷键"},
    {"settings.language", "语言"},
    {"settings.lang_system", "跟随系统"},
    {"settings.startup", "开机启动"},
    {"settings.open_memos", "打开 Memos"},
    {"settings.signout", "退出登录"},
    {"settings.geocoding", "地名服务（留空停用）"},

    // Link Memo View
    {"link.title", "关联 Memo"},
    {"link.search", "搜索"},
    {"link.more", "更多"},
    {"link.no_match", "没有找到匹配的 Memo。"},

    // Location View
    {"location.title", "位置"},
    {"location.name", "名称"},
    {"location.current", "当前位置"},
    {"location.cancel_locating", "取消定位"},
    {"location.cancel_lookup", "取消查询"},
    {"location.settings_btn", "定位设置"},
    {"location.latitude", "纬度"},
    {"location.longitude", "经度"},
    {"location.attribution", "© OpenStreetMap · 地名查询"},
    {"location.default_pos", "Windows 默认位置，请核对。"},
    {"location.accuracy", "精度约 {} 米"},
    {"location.cant_open_settings", "无法打开定位设置，请在 Windows 设置中搜索「位置」。"},

    // Audio View
    {"record.title", "录音"},
    {"record.start", "开始录音"},
    {"record.stop", "停止"},
    {"record.again", "重新录制"},
    {"record.play", "试听"},
    {"record.stopped", "已停止"},
    {"record.duration", "已录制 {} 秒"},
    {"record.need_record", "先录一段，再添加。"},

    // Tray Menu
    {"tray.tip", "MemoQuick · 随手记下"},
    {"tray.quick_note", "快速记录"},
    {"tray.open_memos", "打开 Memos"},
    {"tray.settings", "设置"},
    {"tray.quit", "退出 MemoQuick"},

    // Dialogs
    {"dialog.images", "图片"},
    {"dialog.all_files", "所有文件"},

    // Notices & Status
    {"notice.hotkey_taken", "快捷键已被占用，请在设置中换一个组合。"},
    {"notice.hotkey_invalid", "快捷键无效或已被占用，请使用含 Ctrl 或 Alt 的其他组合。"},
    {"notice.empty_note", "写点什么，或添加一个附件。 "},
    {"notice.busy_quit", "正在处理请求，请完成后再退出。"},
    {"notice.generic_failed", "操作未能完成，请检查本地数据与服务状态；草稿已保留。"},
    {"notice.cant_start", "MemoQuick 无法启动"},

    // Core & Network Errors
    {"error.network_creating", "连接中断或超时，服务端可能已保存。请先到 Memos 确认再重试；草稿已保留。"},
    {"error.network_connect", "无法连接 Memos，请检查地址、网络、系统代理或 HTTPS 证书。"},
    {"error.invalid_utf8", "文本不是有效的 UTF-8。"},
    {"error.invalid_chars", "文本包含无效字符。"},
    {"error.invalid_server_url", "请输入完整的 HTTP / HTTPS 服务地址，不能含账号、查询参数或片段。"},
    {"error.auth_expired", "登录已过期或凭据无效，请重新登录；草稿已保留。"},
    {"error.forbidden", "服务拒绝了请求，请检查账号权限或是否允许密码登录。"},
    {"error.rate_limited", "请求过于频繁，请稍后重试。"},
    {"error.redirected", "服务地址发生重定向，请填写跳转后的最终地址。"},
    {"error.server_uncertain_save", "服务未能确认保存结果，请先到 Memos 检查，再决定是否重试；草稿已保留。"},
    {"error.memos_error", "Memos 返回错误（{}）。请稍后检查；草稿已保留。"},
    {"error.memos_version_old", "需要 Memos 0.26 或更新版本。"},
    {"error.no_compatible_api", "未找到兼容 API。请检查服务地址，并使用 Memos 0.26 或更新版本。"},
    {"error.fill_credentials", "请填写用户名和密码。"},
    {"error.no_session", "服务未返回登录会话，请检查 Memos 版本。"},
    {"error.no_refresh_cookies", "服务未返回可恢复的登录凭据，请检查 Memos 版本或反向代理的 Cookie 设置。"},
    {"error.cant_resume", "无法恢复登录，请重新登录。"},
    {"error.cant_verify_user", "无法确认登录账号，请重新登录。"},
    {"error.fill_content_or_attach", "请填写内容或添加附件。"},
    {"error.wait_upload", "请先完成附件上传。"},
    {"error.cant_confirm_save", "无法确认保存结果，请先到 Memos 检查；草稿已保留。"},
    {"error.save_ok_session_fail", "记录可能已保存，但本地会话更新失败。请先到 Memos 确认；草稿已保留。"},
    {"error.non_api_response", "服务返回了非 API 响应，请确认地址指向 Memos。"},
    {"error.response_too_large", "服务返回的数据过大。"},

    // Compose & Attachment Errors
    {"error.invalid_response_data", "服务返回了无效数据。"},
    {"error.cant_encode_attachment", "无法编码附件。"},
    {"error.invalid_upload_progress", "上传进度无效。"},
    {"error.invalid_location_fields", "请填写名称、纬度（−90～90）和经度（−180～180）。"},
    {"error.invalid_image_attachment", "图片附件无效。"},
    {"error.attachment_size_limit", "附件需非空且不超过 20 MiB。"},
    {"error.cant_init_upload", "无法初始化附件上传。"},
    {"error.upload_progress_mismatch", "附件上传进度不一致，请重试。"},
    {"error.cant_confirm_upload", "无法确认附件上传结果，本地文件已保留。"},
    {"error.search_too_long", "搜索内容过长。"},

    // Clipboard Errors
    {"error.clipboard_read", "无法读取剪贴板附件，请重新复制后再试。"},
    {"error.attachment_20mb", "附件不能超过 20 MiB。"},
    {"error.empty_attachment", "不能粘贴空附件。"},
    {"error.image_too_large", "图片过大，请缩小到 1600 万像素以内再粘贴。"},
    {"error.batch_size_limit", "单次粘贴附件总量不能超过 64 MiB。"},
    {"error.batch_count_limit", "单次最多粘贴 32 个文件。"},
    {"error.folder_unsupported", "请复制文件，暂不支持粘贴文件夹。"},
    {"error.clipboard_temp_unavail", "剪贴板暂时无法读取，请重试粘贴。"},
    {"error.paste_cancelled", "粘贴已取消，请回到笔记后重试。"},
    {"error.max_32_attachments", "每条记录最多添加 32 个附件。"},
    {"error.max_32_memos", "每条记录最多关联 32 条 Memo。"},

    // Audio Errors
    {"error.audio_empty_or_bad", "录音为空或长度异常。"},
    {"error.mic_open", "无法打开麦克风，请检查设备及 Windows 麦克风权限。"},
    {"error.audio_prep", "无法准备录音。"},
    {"error.audio_start", "无法开始录音。"},
    {"error.audio_disconnected", "录音设备已断开，请停止并保留已有录音。"},
    {"error.audio_not_started", "录音尚未开始。"},

    // Location & Geocoding Errors
    {"error.location_denied", "定位未获允许，请开启 Windows 位置权限。"},
    {"error.location_timeout", "定位超时，可重试或手动填写。"},
    {"error.location_unavailable", "暂时无法定位，可重试或手动填写。"},
    {"error.coords_invalid", "坐标无效。"},
    {"error.coords_display", "无法显示坐标。"},
    {"error.location_not_ready", "Windows 定位尚未就绪，可重试或手动填写。"},
    {"error.location_bad_os_return", "Windows 返回了无效位置，可重试或手动填写。"},
    {"error.geocoding_failed", "地名查询失败，坐标已保留，可手动填写名称。"},
    {"error.geocoding_https", "地名服务请使用 HTTPS 地址。"},
    {"error.geocoding_rate", "请稍后重试地名查询，坐标已保留。"},

    // Storage Errors
    {"error.read_local_file", "无法读取本地数据文件，原文件已保留。"},
    {"error.local_file_size", "本地数据文件大小异常，原文件已保留。"},
    {"error.read_local_failed", "读取本地数据失败，原文件已保留。"},
    {"error.cant_write_local", "无法写入本地数据，请检查磁盘空间和目录权限。"},
    {"error.save_local_failed", "本地数据保存失败，请检查磁盘空间和目录权限。"},
    {"error.encrypt_failed", "Windows 无法加密数据，请检查当前用户账户。"},
    {"error.decrypt_failed", "无法解密会话或草稿，原文件已保留。请使用原 Windows 账户。"},
    {"error.no_data_dir", "无法找到用户数据目录。"},
    {"error.local_data_corrupt", "本地数据结构异常，原文件已保留。"},
    {"error.read_corrupt_backup", "无法读取本地数据，原文件已保留。请先备份后检查。"},
    {"error.session_invalid", "会话数据无效，请重新登录。"},
    {"error.session_corrupt", "会话数据异常，原文件已保留。"},
    {"error.clean_draft_failed", "记录已保存，但旧草稿清理失败。请检查磁盘，不要重复提交旧草稿。"},
    {"error.draft_attach_failed", "附件未能加入草稿，请检查磁盘后重试。"},

    // Window & Control Errors
    {"error.register_window", "无法注册窗口。"},
    {"error.create_window", "无法创建窗口。"},
    {"error.create_controls", "无法创建输入控件。"},
    {"error.content_length_limit", "内容已达到长度上限。"},
    {"error.choose_file_failed", "无法选择文件。"},
    {"error.choose_image_only", "请选择图片文件。"},
    {"error.open_attach_failed", "无法打开附件。"},
    {"error.read_attach_failed", "无法完整读取附件。"},
    {"error.open_menu_failed", "无法打开菜单。"},
    {"error.read_menu_failed", "无法读取菜单。"},
    {"error.read_menu_text", "无法读取菜单文字。"},
    {"error.set_menu_failed", "无法设置菜单。"},
    {"error.set_menu_theme", "无法设置菜单主题。"}
};

const std::unordered_map<std::string, std::string> enStrings = {
    // General & Navigation
    {"app.name", "MemoQuick"},
    {"app.slogan", "Quick Notes"},
    {"theme.toggle", "Toggle Light / Dark"},
    {"header.settings", "Settings"},
    {"header.close", "Close · Esc"},
    {"button.back", "Back"},
    {"button.cancel", "Cancel"},
    {"button.done", "Done"},
    {"button.add", "Add"},

    // Login View
    {"login.server", "Server Address"},
    {"login.server_placeholder", "https://memos.example.com"},
    {"login.username", "Username"},
    {"login.password", "Password"},
    {"login.signin", "Sign In"},
    {"login.connecting", "Connecting…"},
    {"login.resume", "Resume Session"},

    // Compose View
    {"compose.visibility.private", "Only Me"},
    {"compose.visibility.protected", "Signed-in Users"},
    {"compose.visibility.public", "Public"},
    {"compose.add_tooltip", "Add attachment, image, audio, link, or location"},
    {"compose.metadata_tooltip", "Manage attachments, links, and location"},
    {"compose.save", "Save"},
    {"compose.saving", "Saving…"},
    {"compose.uploading", "Uploading…"},
    {"compose.save_tooltip", "Save · Ctrl + Enter"},

    // Markdown Toolbar Tooltips & Placeholders
    {"markdown.bold", "Bold · Ctrl + B"},
    {"markdown.italic", "Italic · Ctrl + I"},
    {"markdown.heading", "Heading · Ctrl + Shift + H"},
    {"markdown.link", "Link · Ctrl + K"},
    {"markdown.bullet", "Bullet List · Ctrl + Shift + L"},
    {"markdown.numbered", "Numbered List · Ctrl + Shift + O"},
    {"markdown.task", "To-do · Ctrl + Shift + T"},
    {"markdown.quote", "Quote · Ctrl + Shift + Q"},
    {"markdown.inline_code", "Inline Code · Ctrl + E"},
    {"markdown.code_block", "Code Block · Ctrl + Shift + C"},
    {"markdown.placeholder.text", "text"},
    {"markdown.placeholder.code", "code"},

    // Insert Menu
    {"insert.attachment", "Add Attachment"},
    {"insert.image", "Insert Image"},
    {"insert.audio", "Record Audio"},
    {"insert.memo", "Link Memo"},
    {"insert.location", "Add Location"},

    // Metadata Menu & Summary
    {"metadata.retry_upload", "Retry Upload"},
    {"metadata.remove_prefix", "Remove "},
    {"metadata.pending", " (Pending)"},
    {"metadata.unlink_prefix", "Unlink "},
    {"metadata.edit_location_prefix", "Edit Location "},
    {"metadata.remove_location", "Remove Location"},
    {"metadata.attachment_singular", "{} Attachment"},
    {"metadata.attachments", "{} Attachments"},
    {"metadata.relation_singular", "{} Link"},
    {"metadata.relations", "{} Links"},
    {"metadata.location", "Location"},

    // Settings View
    {"settings.title", "Settings"},
    {"settings.hotkey", "Hotkey"},
    {"settings.hotkey_press", "Press hotkey"},
    {"settings.language", "Language"},
    {"settings.lang_system", "System Default"},
    {"settings.startup", "Start at Login"},
    {"settings.open_memos", "Open Memos"},
    {"settings.signout", "Sign Out"},
    {"settings.geocoding", "Geocoding Service (leave empty to disable)"},

    // Link Memo View
    {"link.title", "Link Memo"},
    {"link.search", "Search"},
    {"link.more", "More"},
    {"link.no_match", "No matching Memos found."},

    // Location View
    {"location.title", "Location"},
    {"location.name", "Name"},
    {"location.current", "Current Location"},
    {"location.cancel_locating", "Cancel Location"},
    {"location.cancel_lookup", "Cancel Lookup"},
    {"location.settings_btn", "Location Settings"},
    {"location.latitude", "Latitude"},
    {"location.longitude", "Longitude"},
    {"location.attribution", "© OpenStreetMap · Geocoding"},
    {"location.default_pos", "Default Windows location, please verify."},
    {"location.accuracy", "Accuracy ~{} m"},
    {"location.cant_open_settings", "Cannot open location settings. Please search for 'Location' in Windows Settings."},

    // Audio View
    {"record.title", "Record Audio"},
    {"record.start", "Record"},
    {"record.stop", "Stop"},
    {"record.again", "Record Again"},
    {"record.play", "Play"},
    {"record.stopped", "Stopped"},
    {"record.duration", "Recorded {} s"},
    {"record.need_record", "Record some audio before adding."},

    // Tray Menu
    {"tray.tip", "MemoQuick · Quick Notes"},
    {"tray.quick_note", "Quick Note"},
    {"tray.open_memos", "Open Memos"},
    {"tray.settings", "Settings"},
    {"tray.quit", "Quit MemoQuick"},

    // Dialogs
    {"dialog.images", "Images"},
    {"dialog.all_files", "All Files"},

    // Notices & Status
    {"notice.hotkey_taken", "Hotkey is already in use. Please choose another combination in Settings."},
    {"notice.hotkey_invalid", "Hotkey is invalid or already in use. Please use another combination with Ctrl or Alt."},
    {"notice.empty_note", "Write something or add an attachment. "},
    {"notice.busy_quit", "Processing request. Please wait before exiting."},
    {"notice.generic_failed", "Operation failed. Please check local data and server status; draft preserved."},
    {"notice.cant_start", "MemoQuick cannot start"},

    // Core & Network Errors
    {"error.network_creating", "Connection interrupted or timed out. The server may have saved it. Please check Memos before retrying; draft preserved."},
    {"error.network_connect", "Cannot connect to Memos. Please check address, network, proxy, or HTTPS certificate."},
    {"error.invalid_utf8", "Text is not valid UTF-8."},
    {"error.invalid_chars", "Text contains invalid characters."},
    {"error.invalid_server_url", "Please enter a complete HTTP / HTTPS server address without credentials, query parameters, or fragments."},
    {"error.auth_expired", "Session expired or credentials invalid. Please sign in again; draft preserved."},
    {"error.forbidden", "Server rejected the request. Please check account permissions or whether password sign-in is allowed."},
    {"error.rate_limited", "Too many requests. Please try again later."},
    {"error.redirected", "Server address was redirected. Please enter the final target address."},
    {"error.server_uncertain_save", "Server could not confirm save result. Please check Memos before deciding to retry; draft preserved."},
    {"error.memos_error", "Memos returned an error ({}). Please check later; draft preserved."},
    {"error.memos_version_old", "Requires Memos 0.26 or newer."},
    {"error.no_compatible_api", "No compatible API found. Please check server address and use Memos 0.26 or newer."},
    {"error.fill_credentials", "Please enter username and password."},
    {"error.no_session", "Server did not return a session. Please check Memos version."},
    {"error.no_refresh_cookies", "Server did not return resumable credentials. Please check Memos version or reverse proxy cookie settings."},
    {"error.cant_resume", "Cannot resume session. Please sign in again."},
    {"error.cant_verify_user", "Cannot verify account. Please sign in again."},
    {"error.fill_content_or_attach", "Please write something or add an attachment."},
    {"error.wait_upload", "Please wait for attachments to finish uploading."},
    {"error.cant_confirm_save", "Cannot confirm save result. Please check Memos first; draft preserved."},
    {"error.save_ok_session_fail", "Note may have been saved, but local session update failed. Please check Memos first; draft preserved."},
    {"error.non_api_response", "Server returned a non-API response. Please verify the address points to Memos."},
    {"error.response_too_large", "Server returned data that is too large."},

    // Compose & Attachment Errors
    {"error.invalid_response_data", "Server returned invalid data."},
    {"error.cant_encode_attachment", "Cannot encode attachment."},
    {"error.invalid_upload_progress", "Upload progress is invalid."},
    {"error.invalid_location_fields", "Please enter name, latitude (-90 to 90), and longitude (-180 to 180)."},
    {"error.invalid_image_attachment", "Image attachment is invalid."},
    {"error.attachment_size_limit", "Attachment must be non-empty and at most 20 MiB."},
    {"error.cant_init_upload", "Cannot initialize attachment upload."},
    {"error.upload_progress_mismatch", "Attachment upload progress is inconsistent. Please retry."},
    {"error.cant_confirm_upload", "Cannot confirm attachment upload result; local file preserved."},
    {"error.search_too_long", "Search text is too long."},

    // Clipboard Errors
    {"error.clipboard_read", "Cannot read clipboard attachment. Please copy and try again."},
    {"error.attachment_20mb", "Attachment cannot exceed 20 MiB."},
    {"error.empty_attachment", "Cannot paste empty attachment."},
    {"error.image_too_large", "Image is too large. Please resize below 16 megapixels before pasting."},
    {"error.batch_size_limit", "Total pasted attachments cannot exceed 64 MiB."},
    {"error.batch_count_limit", "Cannot paste more than 32 files at once."},
    {"error.folder_unsupported", "Please copy files; pasting folders is not supported."},
    {"error.clipboard_temp_unavail", "Clipboard is temporarily unavailable. Please retry pasting."},
    {"error.paste_cancelled", "Paste was cancelled. Please return to the note and retry."},
    {"error.max_32_attachments", "A note can have at most 32 attachments."},
    {"error.max_32_memos", "A note can reference at most 32 Memos."},

    // Audio Errors
    {"error.audio_empty_or_bad", "Recording is empty or has invalid length."},
    {"error.mic_open", "Cannot open microphone. Please check your device and Windows microphone permissions."},
    {"error.audio_prep", "Cannot prepare audio recording."},
    {"error.audio_start", "Cannot start audio recording."},
    {"error.audio_disconnected", "Recording device disconnected. Please stop and keep existing audio."},
    {"error.audio_not_started", "Recording has not started."},

    // Location & Geocoding Errors
    {"error.location_denied", "Location access not allowed. Please enable Windows location permissions."},
    {"error.location_timeout", "Location timed out. You can retry or enter manually."},
    {"error.location_unavailable", "Cannot determine location. You can retry or enter manually."},
    {"error.coords_invalid", "Invalid coordinates."},
    {"error.coords_display", "Cannot display coordinates."},
    {"error.location_not_ready", "Windows location is not ready yet. You can retry or enter manually."},
    {"error.location_bad_os_return", "Windows returned an invalid location. You can retry or enter manually."},
    {"error.geocoding_failed", "Geocoding failed. Coordinates preserved; you can enter name manually."},
    {"error.geocoding_https", "Please use an HTTPS address for geocoding service."},
    {"error.geocoding_rate", "Please retry geocoding later. Coordinates preserved."},

    // Storage Errors
    {"error.read_local_file", "Cannot read local data file; original file preserved."},
    {"error.local_file_size", "Local data file size is abnormal; original file preserved."},
    {"error.read_local_failed", "Failed to read local data; original file preserved."},
    {"error.cant_write_local", "Cannot write local data. Please check disk space and directory permissions."},
    {"error.save_local_failed", "Failed to save local data. Please check disk space and directory permissions."},
    {"error.encrypt_failed", "Windows cannot encrypt data. Please check current user account."},
    {"error.decrypt_failed", "Cannot decrypt session or draft; original file preserved. Please use the original Windows account."},
    {"error.no_data_dir", "Cannot find user data directory."},
    {"error.local_data_corrupt", "Local data structure is abnormal; original file preserved."},
    {"error.read_corrupt_backup", "Cannot read local data; original file preserved. Please back up and check."},
    {"error.session_invalid", "Session data is invalid. Please sign in again."},
    {"error.session_corrupt", "Session data is abnormal; original file preserved."},
    {"error.clean_draft_failed", "Note saved, but old draft cleanup failed. Please check disk and do not resubmit old draft."},
    {"error.draft_attach_failed", "Attachment could not be added to draft. Please check disk and retry."},

    // Window & Control Errors
    {"error.register_window", "Cannot register window."},
    {"error.create_window", "Cannot create window."},
    {"error.create_controls", "Cannot create controls."},
    {"error.content_length_limit", "Content has reached the maximum length."},
    {"error.choose_file_failed", "Cannot select files."},
    {"error.choose_image_only", "Please select image files."},
    {"error.open_attach_failed", "Cannot open attachment."},
    {"error.read_attach_failed", "Cannot read attachment completely."},
    {"error.open_menu_failed", "Cannot open menu."},
    {"error.read_menu_failed", "Cannot read menu."},
    {"error.read_menu_text", "Cannot read menu text."},
    {"error.set_menu_failed", "Cannot set menu."},
    {"error.set_menu_theme", "Cannot set menu theme."}
};

std::vector<LanguageEntry> defaultLanguages() {
    return {
        LanguageEntry{LanguageInfo{"zh-CN", L"简体中文", {"zh", "zh-cn", "zh-hans", "zh-sg"}}, zhCnStrings},
        LanguageEntry{LanguageInfo{"en", L"English", {"en", "en-us", "en-gb"}}, enStrings}
    };
}

class State {
public:
    State() : languages_(defaultLanguages()) {
        resolvedLang_ = detectSystemLanguageLocked();
    }

    void reset() {
        std::lock_guard lock(mutex_);
        languages_ = defaultLanguages();
        configuredLang_.clear();
        resolvedLang_ = detectSystemLanguageLocked();
    }

    std::vector<LanguageInfo> availableLanguages() const {
        std::lock_guard lock(mutex_);
        std::vector<LanguageInfo> result;
        result.reserve(languages_.size());
        for (const auto& entry : languages_) result.push_back(entry.info);
        return result;
    }

    std::wstring languageName(std::string_view id) const {
        std::lock_guard lock(mutex_);
        for (const auto& entry : languages_) {
            if (entry.info.id == id) return entry.info.name;
        }
        return {};
    }

    std::string detectSystemLanguage() const {
        std::lock_guard lock(mutex_);
        return detectSystemLanguageLocked();
    }

    void setLanguage(const std::string& lang) {
        std::lock_guard lock(mutex_);
        configuredLang_ = lang;
        if (lang.empty() || lang == "system") {
            resolvedLang_ = detectSystemLanguageLocked();
        } else {
            const auto it = findLanguage(lang);
            resolvedLang_ = (it != languages_.end()) ? it->info.id : "en";
        }
    }

    std::string currentLanguage() const {
        std::lock_guard lock(mutex_);
        return resolvedLang_;
    }

    std::string configuredLanguage() const {
        std::lock_guard lock(mutex_);
        return configuredLang_;
    }

    std::string tr(std::string_view key) const {
        std::lock_guard lock(mutex_);
        const auto currentIt = findLanguage(resolvedLang_);
        if (currentIt != languages_.end()) {
            const auto strIt = currentIt->strings.find(std::string(key));
            if (strIt != currentIt->strings.end()) return strIt->second;
        }
        // Fallback to English
        const auto enIt = findLanguage("en");
        if (enIt != languages_.end() && enIt != currentIt) {
            const auto strIt = enIt->strings.find(std::string(key));
            if (strIt != enIt->strings.end()) return strIt->second;
        }
        return std::string(key);
    }

    void registerLanguage(LanguageInfo info, std::unordered_map<std::string, std::string> strings) {
        std::lock_guard lock(mutex_);
        const auto it = findLanguage(info.id);
        if (it != languages_.end()) {
            it->info = std::move(info);
            it->strings = std::move(strings);
        } else {
            languages_.push_back(LanguageEntry{std::move(info), std::move(strings)});
        }
        if (configuredLang_.empty() || configuredLang_ == "system") {
            resolvedLang_ = detectSystemLanguageLocked();
        }
    }

private:
    std::vector<LanguageEntry>::iterator findLanguage(std::string_view id) {
        return std::find_if(languages_.begin(), languages_.end(), [&](const auto& e) { return e.info.id == id; });
    }

    std::vector<LanguageEntry>::const_iterator findLanguage(std::string_view id) const {
        return std::find_if(languages_.begin(), languages_.end(), [&](const auto& e) { return e.info.id == id; });
    }

    std::string detectSystemLanguageLocked() const {
        WCHAR localeName[LOCALE_NAME_MAX_LENGTH]{};
        if (GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) > 0) {
            std::string lower;
            for (const wchar_t c : std::wstring_view(localeName)) {
                lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            // Check exact prefix match
            for (const auto& entry : languages_) {
                for (const auto& prefix : entry.info.prefixes) {
                    if (lower == prefix || lower.starts_with(prefix + "-") || lower.starts_with(prefix + "_")) {
                        return entry.info.id;
                    }
                }
            }
            for (const auto& entry : languages_) {
                for (const auto& prefix : entry.info.prefixes) {
                    if (lower.starts_with(prefix)) return entry.info.id;
                }
            }
        }
        const LANGID langId = GetUserDefaultUILanguage();
        if (PRIMARYLANGID(langId) == LANG_CHINESE) return "zh-CN";
        if (PRIMARYLANGID(langId) == LANG_ENGLISH) return "en";
        return "en";
    }

    mutable std::mutex mutex_;
    std::vector<LanguageEntry> languages_;
    std::string configuredLang_;
    std::string resolvedLang_ = "zh-CN";
};

State& globalState() {
    static State state;
    return state;
}

} // namespace

std::vector<LanguageInfo> availableLanguages() { return globalState().availableLanguages(); }
std::wstring languageName(std::string_view id) { return globalState().languageName(id); }
std::string detectSystemLanguage() { return globalState().detectSystemLanguage(); }
void setLanguage(const std::string& lang) { globalState().setLanguage(lang); }
std::string currentLanguage() { return globalState().currentLanguage(); }
std::string configuredLanguage() { return globalState().configuredLanguage(); }
std::string tr(std::string_view key) { return globalState().tr(key); }
std::wstring trW(std::string_view key) { return wide(tr(key)); }

std::string trFormat(std::string_view key, const std::string& arg) {
    auto text = tr(key);
    auto pos = text.find("{}");
    if (pos != std::string::npos) {
        text.replace(pos, 2, arg);
        return text;
    }
    pos = text.find("{0}");
    if (pos != std::string::npos) {
        text.replace(pos, 3, arg);
        return text;
    }
    return text;
}

std::wstring trFormatW(std::string_view key, const std::wstring& arg) {
    auto text = trW(key);
    auto pos = text.find(L"{}");
    if (pos != std::wstring::npos) {
        text.replace(pos, 2, arg);
        return text;
    }
    pos = text.find(L"{0}");
    if (pos != std::wstring::npos) {
        text.replace(pos, 3, arg);
        return text;
    }
    return text;
}

void registerLanguage(LanguageInfo info, std::unordered_map<std::string, std::string> strings) {
    globalState().registerLanguage(std::move(info), std::move(strings));
}

void resetToDefaults() {
    globalState().reset();
}

} // namespace mq::i18n
