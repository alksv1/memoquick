# MemoQuick 0.2.1

修复 Dark 模式下「＋」菜单仍显示浅色的问题。添加、可见性、附件与关联管理、托盘菜单统一使用应用的主题色、字体、紧凑间距与悬停背景。
保留原生菜单的方向键选择、Enter 确认、Esc 取消和已选可见性标记；长文件名限制宽度并显示省略号。

自动测试实际打开上述菜单，检查 Light / Dark 的背景与选中色、保留菜单文字，并验证键盘操作与草稿保持。

## MemoQuick 0.2.0

这次迭代将原来的纯文本随手记扩展为支持文件、图片、声音、关联和位置的原生 Memos 编辑器。
主界面只增加一个「＋」入口与按需显示的摘要，保留 Light / Dark、位置记忆、Markdown 工具与快捷键切换。

## 五项新增功能

- **添加附件**：多选文件、本地加密缓存、提交时上传，支持仅附件记录。
- **插入图片**：上传后在光标处插入 Markdown 图片，并在同次 Memo 创建请求中绑定附件。
- **录音**：原生麦克风录制、停止、试听与取消，最多五分钟，保存为 WAV 音频附件。
- **关联 Memo**：搜索自己的活跃记录，分页选择，保存正式 REFERENCE 关系。
- **添加位置**：名称与经纬度校验，保存为 Memos 原生 location 数据，支持编辑和移除。

## 兼容与恢复

保留 Memos 0.26+ 的 API 路径识别与会话续期。新服务使用有界分块上传，旧服务在确认不支持分块路由后使用 JSON/Base64 上传。
关联搜索兼容新版本的资源名过滤和旧版本的数字用户 ID 过滤，并在客户端再次校验作者及归档状态。
普通附件和录音在保存时上传；图片按官方内联图片的交互先上传再插入。
每次确定成功的上传都会持久化引用，后续失败保留剩余待上传文件；网络结果不明确时不自动重放写入。
0.1 的加密草稿保持可读。文件缓存按服务、账号和 Windows 用户隔离。

窗口仍使用 Win32 原生控件，无 Electron、WebView 或额外运行时。
手动坐标输入替代网页地图选点；未增加自动定位、AI 转写或媒体预览。这些不是五个按钮基础功能的前置依赖。

## 官方核对来源

核对时间：2026-10-09。官方主分支固定在 `ca0361379112441e50b43b465e9de89b67fbb30a`，另核对 v0.26.0 / v0.31.0 的接口。

- [官方附件交互说明](https://usememos.com/docs/usage/attachments)
- [官方关联与位置说明](https://usememos.com/docs/usage/memos)
- [五个菜单项的实现](https://github.com/usememos/memos/blob/ca0361379112441e50b43b465e9de89b67fbb30a/web/src/components/MemoEditor/Toolbar/InsertMenu.tsx)
- [录音生命周期](https://github.com/usememos/memos/blob/ca0361379112441e50b43b465e9de89b67fbb30a/web/src/components/MemoEditor/hooks/useAudioRecorder.ts)
- [附件与分块上传接口](https://github.com/usememos/memos/blob/ca0361379112441e50b43b465e9de89b67fbb30a/proto/api/attachment_service.proto)
- [Memo、关联和位置接口](https://github.com/usememos/memos/blob/ca0361379112441e50b43b465e9de89b67fbb30a/proto/api/memo_service.proto)
- [旧版关联搜索](https://github.com/usememos/memos/blob/v0.26.0/web/src/components/MemoEditor/hooks/useLinkMemo.ts)
- [旧版附件文件路由](https://github.com/usememos/memos/blob/v0.26.0/server/router/fileserver/fileserver.go)
