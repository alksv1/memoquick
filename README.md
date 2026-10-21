# MemoQuick

English | [简体中文](README_zh.md)

A standalone quick-capture tool for Memos, current version **0.2.6**. Native C++20 + Win32 window connecting directly to an existing Memos instance.
No dependencies on Memospot, Electron, WebView, Node.js, Python, or .NET runtimes.

## Usage

1. Download the Windows x64 executable or portable ZIP from [Releases](https://github.com/alksv1/memoquick/releases) and run `MemoQuick.exe`.
2. Enter the Memos server URL, username, and password, then sign in once.
3. Press **Ctrl + Alt + M** from any application to summon the compact window and start typing immediately.
   Press the same hotkey again to hide the window; uncommitted drafts are automatically preserved. Press the hotkey while minimized to restore the window.
4. Click Save or press **Ctrl + Enter**; once saved successfully, the window automatically minimizes to the system tray.

`Esc`, the window close button, and the system tray icon can all collapse or reopen the window. Drafts are preserved automatically and restored across restarts.
Failed submissions retain draft content. The app does not automatically replay writes after timeouts, connection interruptions, or server errors—verify on Memos before retrying.
Enter does not trigger submission while an IME composition is in progress.

In Settings, you can configure the global hotkey, interface language (defaults to system UI language, with manual selection for Simplified Chinese and English), launch on Windows startup, sign out, or switch server address.
Startup runs silently in the background, opened via hotkey or tray icon. For the portable version, keep it in a fixed directory; launching it manually once after moving repairs the startup path.
0.2.2 fixed credential persistence for newer Memos versions; after upgrading from older versions, sign in once to restore automatic session renewal.
The main interface is a compact 420 × 280 window. Drag the top bar to move, or drag edges to resize.
Window position and size are remembered across restarts and restored automatically; when monitor layout changes, the window automatically moves back to the visible work area.
The top-bar **Light / Dark** button toggles themes with instant persistence; **···** opens Settings.
The save shortcut is **Ctrl + Enter**, with tooltip hints displayed on hover.
If a hotkey conflicts with another application, you can still open MemoQuick from the tray and pick a different shortcut in Settings.
Default visibility for new memos is "Only Me", with options for signed-in users or public.

## Markdown Tools

The toolbar above the text area supports selection formatting; when no text is selected, it inserts placeholder text. List, heading, and quote tools apply to the current line or full selected lines.
Bold, italic, and line prefixes can be clicked again to cancel. Lists can be converted directly into other list types, and formatting operations can be undone with **Ctrl + Z**.
When inserting a link, the URL placeholder is selected automatically—just paste your URL. Only Markdown source text is edited; links are not uploaded or opened automatically.

| Tool | Shortcut |
| --- | --- |
| Bold / Italic | Ctrl + B / Ctrl + I |
| Heading 2 | Ctrl + Shift + H |
| Link | Ctrl + K |
| Bullet / Numbered List | Ctrl + Shift + L / Ctrl + Shift + O |
| To-do / Quote | Ctrl + Shift + T / Ctrl + Shift + Q |
| Inline Code / Code Block | Ctrl + E / Ctrl + Shift + C |

Toolbar buttons show usage and shortcuts on hover. Shortcuts are suppressed while IME composition is active.

## Adding Content

The bottom **＋** menu provides five actions while keeping the main editor compact:

| Action | Behavior |
| --- | --- |
| Add Attachment | Multi-select local files; uploaded and bound when saving. Notes can consist of attachments alone. |
| Insert Image | Multi-select images; uploads and inserts Markdown image syntax at cursor while binding to note. |
| Record Audio | Click to start, stop, preview, and add to draft; uploaded as a WAV attachment when saving. |
| Link Memo | Search and paginate through your non-archived memos to establish a formal `REFERENCE` relation. |
| Add Location | Enter location name, latitude, and longitude; saved as structured location viewable on Memos map. |

Press **Ctrl + V** or **Shift + Insert** in the editor to directly paste screenshots, copied images, files from File Explorer, or standard OLE clipboard attachments.
Images upload automatically and insert Markdown at the cursor; other files are added as attachments and uploaded when manually saving. Plain text pastes normally with native undo preserved.
No need to save to a file first. Screenshots are encoded in memory as PNG; transparent PNGs preserve raw alpha channels without generating plaintext temporary files. Uncommitted attachments use Windows DPAPI encrypted draft caching, surviving application restarts.
A pasted batch is read and saved completely before starting any upload; failed reads do not add half-batches, and failed uploads retain retryable drafts. Source files from copy/cut are never modified or deleted.
Supports PNG, DIB / DIBV5, and Windows bitmaps; up to 32 files per paste, 64 MiB total batch limit, 20 MiB per file, and up to ~16 megapixels for bitmaps. Folders and private application clipboard formats are not supported.

A summary of attachments, links, and location appears at the bottom; click the summary to remove items, edit location, or retry uploads.
Editor input is paused during image upload to prevent cursor position misalignment. Normal attachments and audio recordings upload during manual memo save.
If an upload partially succeeds and fails, saving again only uploads the remaining pending files; successfully uploaded references are restored with drafts.

Individual attachments are capped at **20 MiB**, with a maximum of **32 attachments / 32 links** per memo (subject to server limits).
Audio recording uses the default Windows microphone in 16 kHz mono PCM WAV, with each segment capped at **5 minutes**.
Recording automatically stops at the limit and can be previewed before adding. Collapsing the window while recording stops and caches the audio in encrypted drafts; cancelling discards the segment.
Windows must permit desktop applications to access the microphone; missing devices or insufficient permissions display an error.

Attachments and recordings are cached locally using DPAPI encryption without depending on original files remaining in place. Local cache is cleaned up after confirmed save.
Inserted images upload before memo creation; removing or abandoning uploaded files does not delete server files—unused attachments can be cleaned in Memos attachment storage. Linking does not modify original memo permissions; location displays according to note visibility.
Clicking "Current Location" in the location panel automatically fills coordinates via Windows location services and displays estimated accuracy; coordinates attach only upon confirmation.
First-time use requires Windows location permissions; if disabled, click "Location Settings". Locating can be cancelled, and closing or leaving the panel cancels pending requests.
Coordinates automatically query reverse geocoding for place names while preserving manual edits; if geocoding is unavailable, coordinates remain and names can be entered manually.
Geocoding follows Memos [Nominatim](https://github.com/usememos/memos/blob/v0.31.0/web/src/components/map/useReverseGeocoding.ts) reverse-lookup convention, sending coordinates to OpenStreetMap without sending Memos credentials, notes, or accounts.
In Settings, "Geocoding Service" can be replaced with any Nominatim-compatible HTTPS reverse endpoint, or left empty to disable. HTTP is allowed only for localhost testing; credentials, query parameters, or fragments in URLs are rejected.
Public services must comply with [Nominatim Usage Policy](https://operations.osmfoundation.org/policies/nominatim/): user-triggered, low frequency, max 1 request per second, cached results, attribution displayed, and endpoint switchable. The app caches up to 100 successful lookups per session; large-scale deployments should use self-hosted or commercial endpoints.
Lookups time out after 8 seconds and can be cancelled; no automatic retries or background tracking. No map point selection. Audio does not include AI transcription.

See [Release Notes](RELEASE_NOTES.md) for detailed version upgrade notes. Version upgrades automatically preserve 0.1 sessions, drafts, themes, and location data.

## Environment & Boundaries

- Windows 10 / 11 x64.
- Connects to an existing **Memos 0.26 or newer** instance supporting username and password sign-in.
- Automatically detects `/api/v1` and newer `/api` routes, including reverse proxy subpaths.
- Current version does not provide SSO login, Markdown preview, or local embedded Memos server.
- Cannot inherit session state from other clients; sign in once in MemoQuick, with automatic renewals thereafter.
- "Open Memos" uses the default system browser with its own session state.
- HTTPS uses Windows certificate validation; server address must be the final destination without following redirects.

## Data Storage

Data is stored by default in `%LOCALAPPDATA%\MemoQuick`:

- `settings.json`: Server address, hotkey, startup preference, interface language, and theme.
- `window.json`: Window position and dimensions, supporting negative coordinates on multi-monitor setups.
- `private.bin`: Windows DPAPI encrypted refresh sessions and drafts, bound to the current Windows user.
- `attachments/*.bin`: Windows DPAPI encrypted attachment and audio cache, isolated by server and user account.

Passwords are not stored. Short-lived access tokens exist only in memory, cleared on exit, and restored using encrypted refresh sessions.
Drafts are saved separately by server URL and username; switching accounts does not mix or delete other drafts.
Corrupted or undecryptable data files are preserved with errors reported, never overwritten.

## Building from Source

Requires Visual Studio 2022 or 2026 with the "Desktop development with C++" workload, including CMake and Windows SDK.

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Test
```

Or using CMake in Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The output statically links the MSVC C++ runtime, producing a self-contained single `MemoQuick.exe`.
Bundles a pinned single-header nlohmann/json; no external network downloads required during build.
Integration tests use the Python 3.9+ standard library; Python is not required for application runtime.

## Automated Releases

[Release Windows](https://github.com/alksv1/memoquick/actions/workflows/release.yml) automatically compiles and publishes releases when a version tag like `v0.2.6` is pushed.
Tags must correspond to an existing commit and match the version in `CMakeLists.txt` and application resources; tags with suffixes like `-rc.1` publish as prereleases.
You can also trigger **Run workflow** on the Actions page with an existing tag to publish for older commits without moving tags.

The workflow builds on Windows x64 / MSVC Release, runs core and real HTTP API tests, then packages the EXE, portable ZIP with licenses, and `SHA256SUMS.txt`.
Drafts are created first and published only after all jobs succeed. Uses default repository `GITHUB_TOKEN` without additional secret configuration.

## Verification

Unit tests cover Unicode, URLs, Cookie paths and rotation, DPAPI encryption, tamper protection, draft and account isolation, and corrupted data protection.
Integration tests use an isolated local HTTP service to verify login, restart recovery, 401 automatic renewal, legacy and new APIs, and non-repeating failures.
Native window tests cover login form, draft preservation, failure notices, close-on-success, hotkey summoning, theme toggling, and multi-language menus;
screenshots and runtime metrics are saved to `test-output/`.
Window position and dimension tests cover drag persistence, minimize/restore, independent process restarts, offscreen recovery, and negative coordinate persistence.

## Architecture

| File | Responsibility |
| --- | --- |
| `src/core.cpp` | WinHTTP networking, Memos API, session renewal, and Cookies |
| `src/storage.cpp` | DPAPI, atomic file writes, and startup registration |
| `src/i18n.cpp` | Internationalization, multi-language dictionaries, system detection, and fallbacks |
| `src/markdown.cpp` | Markdown selections, line prefixes, and syntax |
| `src/window.cpp` | Native controls, system tray, global hotkeys, and background worker threads |
| `src/main.cpp` | Process entry point, single-instance mutex, and desktop tests |

Released under the MIT License. Third-party licenses in `THIRD_PARTY.md`.
