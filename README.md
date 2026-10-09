# MemoQuick

独立的 Memos 快速记录工具。C++20 + Win32 原生窗口，直接连接已有 Memos 服务。
不依赖 Memospot、Electron、WebView、Node.js、Python 或 .NET 运行时。

## 使用

1. 双击 `dist/MemoQuick.exe`。
2. 填写 Memos 服务地址、用户名和密码，登录一次。
3. 在任意应用按 **Ctrl + Alt + M** 呼出小窗，直接输入内容。
   再按一次相同快捷键收起小窗，未提交的草稿会保留；最小化时按快捷键恢复窗口。
4. 点击保存或按 **Ctrl + Enter**；保存成功后小窗自动收起到托盘。

`Esc`、窗口关闭按钮和托盘都可以收起或重新打开窗口。草稿自动保留，重启后恢复；
提交失败时保留内容。超时或连接中断后不会自动重复提交，先去 Memos 确认保存结果再重试。
输入法正在组词时，Enter 不会触发提交。

设置中可修改全局快捷键、启用登录 Windows 时启动、退出登录或切换服务。
主界面为 420 × 280 的紧凑输入窗，拖动顶栏移动，拖动边缘调整大小。
自动记住窗口位置，收起后呼出或重启程序仍在原处；显示器布局变化时自动移回可见区域。
顶栏 **Light / Dark** 按钮一键切换明暗主题，自动记住选择；**···** 打开设置。
保存快捷键仍为 **Ctrl + Enter**，操作提示在按钮悬停时显示。
快捷键冲突时仍可从托盘打开，再到设置选择其他组合。
默认新记录为「仅自己」，也可选择已登录用户可见或公开。

## Markdown 工具

输入区上方的小工具栏支持选区格式化；未选中文字时插入占位内容，列表、标题和引用作用于当前行或选中的整行。
加粗、斜体及行前格式可再次点击取消；列表之间可直接转换，格式操作可用 **Ctrl + Z** 撤销。
链接插入后自动选中地址，直接粘贴 URL 即可。仅编辑 Markdown 源文本，不自动上传或打开链接。

| 工具 | 快捷键 |
| --- | --- |
| 加粗 / 斜体 | Ctrl + B / Ctrl + I |
| 二级标题 | Ctrl + Shift + H |
| 链接 | Ctrl + K |
| 无序 / 有序列表 | Ctrl + Shift + L / Ctrl + Shift + O |
| 待办 / 引用 | Ctrl + Shift + T / Ctrl + Shift + Q |
| 行内代码 / 代码块 | Ctrl + E / Ctrl + Shift + C |

工具栏按钮悬停显示用途与快捷键，输入法组词时不会触发格式快捷键。

## 环境与边界

- Windows 10 / 11 x64。
- 连接已有的 **Memos 0.26 或更新版本**，服务需要允许用户名与密码登录。
- 自动识别 `/api/v1` 与新版 `/api` 路径，支持反向代理的子路径。
- 当前版本不提供 SSO 登录、附件上传、Markdown 预览或本地 Memos 服务。
- 不能继承其他客户端的登录状态；首次在本工具登录一次，后续自动续期。
- 「打开 Memos」使用系统浏览器，浏览器有自己的登录状态。
- HTTPS 使用 Windows 证书验证，服务地址需是最终地址，不自动跟随重定向。

## 数据

数据默认保存在 `%LOCALAPPDATA%\MemoQuick`：

- `settings.json`：服务地址、快捷键、开机启动偏好与明暗主题。
- `window.json`：窗口位置，支持多显示器的负坐标。
- `private.bin`：Windows DPAPI 加密的刷新会话和草稿，绑定当前 Windows 用户。

不保存密码。短期访问令牌仅存在于进程内存中；退出程序后消失，之后用加密保存的刷新会话恢复。
草稿按服务地址和账号分别保存，切换账号不会混用或删除其他账号的草稿。
数据无法解密或损坏时保留原文件并报告错误，不覆盖它。

## 从源码构建

安装 Visual Studio 2022 或 2026 的「使用 C++ 的桌面开发」组件，包括 CMake 与 Windows SDK。

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Test
```

也可在 Visual Studio Developer PowerShell 中使用 CMake：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

成品静态链接 MSVC C++ 运行库，可单独分发一个 `MemoQuick.exe`。
源码附带固定版本 nlohmann/json 单头文件，构建不需要联网下载依赖。
测试中的真实 HTTP 接口和窗口验证使用 Python 3.9+ 标准库；Python 不参与程序运行。

## 验证

单元测试覆盖 Unicode、URL、Cookie 路径与轮换、DPAPI 加密、防篡改、草稿与账号隔离、损坏数据保护。
集成测试使用本机隔离 HTTP 服务，验证登录、重启恢复、401 自动续期、新旧 API、失败和超时不重复提交。
原生窗口测试覆盖登录表单、草稿保留、失败提示、保存成功收起、快捷键消息唤出、明暗主题切换与草稿保持；
它会输出截图及运行时指标到 `test-output/`。
窗口位置测试覆盖拖动保存、收起呼出、独立进程重启、屏幕外恢复和负坐标持久化。

真实公网服务、系统输入法、不同 DPI 显示器及实际键盘快捷键仍需按使用环境确认。

## 实现

| 文件 | 职责 |
| --- | --- |
| `src/core.cpp` | WinHTTP 网络、Memos API、会话续期与 Cookie |
| `src/storage.cpp` | DPAPI、原子文件保存与开机启动 |
| `src/markdown.cpp` | Markdown 选区、行前格式与代码语法 |
| `src/window.cpp` | 原生控件、托盘、全局快捷键、后台工作线程 |
| `src/main.cpp` | 进程入口、单实例与桌面测试 |

项目采用 MIT 许可证，第三方许可见 `THIRD_PARTY.md`。
