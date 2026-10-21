#pragma once
#include "storage.hpp"
#include "markdown.hpp"
#include "audio.hpp"
#include "location.hpp"
#include "geocoding.hpp"
#include "clipboard.hpp"
#include <commctrl.h>
#include <functional>
#include <map>
#include <thread>

namespace mq {
inline constexpr wchar_t WindowClass[] = L"MemoQuick.Native.Window";
inline constexpr UINT ShowMessage = WM_APP + 1;
inline constexpr UINT TrayMessage = WM_APP + 2;
inline constexpr UINT CompleteMessage = WM_APP + 3;
enum ControlId : int {
    Heading = 100, Subtitle, ServerLabel, ServerInput, UsernameLabel, UsernameInput,
    PasswordLabel, PasswordInput, LoginButton, ResumeButton, Editor, Visibility,
    SaveButton, AccountLabel, HotkeyHint, SettingsButton, NoticeLabel, PrivacyLabel,
    HotkeyLabel, HotkeyInput, StartupInput, SettingsSaveButton, BackButton,
    LogoutButton, BrowserButton, DraftHint, ThemeButton, CloseButton,
    MdBold, MdItalic, MdHeading, MdLink, MdBullet, MdNumbered, MdTask, MdQuote, MdInlineCode, MdCodeBlock,
    AddButton, MetadataButton, SearchInput, SearchButton, MemoList, MoreButton,
    LocationLabel, LocationName, LatitudeLabel, LatitudeInput, LongitudeLabel, LongitudeInput,
    PanelConfirm, RecordButton, RecordTime, PreviewButton, LocateButton, LocationSettingsButton,
    GeocodingLabel, GeocodingInput, LocationAttribution,
    LanguageLabel, LanguageInput,
    InsertAttachment = 1000, InsertImage, InsertAudio, InsertMemo, InsertLocation, RetryAttachments
};
enum class View { Login, Compose, Settings, Link, Location, Record };
enum class Action { Login, Resume, Save, Logout, Upload, Search };
struct Completion {
    Action action;
    std::unique_ptr<Client> client;
    std::string error;
    ErrorKind errorKind = ErrorKind::Http;
    unsigned status = 0;
    bool saved = false;
    std::optional<Draft> draft;
    MemoPage page;
};
class Window {
public:
    Window(Store& store, bool smoke = false);
    ~Window();
    HWND create(HINSTANCE instance, bool background);
    HWND handle() const { return handle_; }
    void show();
    void hide();
    bool preprocess(MSG& message);
    bool busy() const { return busy_; }
    View view() const { return view_; }
    std::string notice() const { return notice_; }
    Draft currentDraft() const;
    void smokeLogin(const std::string& server);
    void smokeResume() { restore(); }
    void fillLocation(const LocationFix& fix);
    void fillLocationName(const std::string& name);
    bool pasteAttachments(IDataObject& source);
    bool flushDraft();
private:
    static LRESULT CALLBACK procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT message(UINT message, WPARAM wparam, LPARAM lparam);
    void createControls();
    void layout();
    void fonts();
    void applyTheme();
    void toggleTheme();
    void chooseVisibility();
    void chooseLanguage();
    void retranslate();
    UINT trackMenu(HMENU menu, UINT flags, int x, int y);
    void measureMenu(MEASUREITEMSTRUCT& measure);
    void drawMenu(const DRAWITEMSTRUCT& draw);
    void ensurePositionVisible();
    void rememberPosition();
    void markdown(MarkdownAction action);
    void insertMenu();
    void chooseFiles(bool image);
    bool pasteClipboard();
    void cacheAttachment(const std::string& filename, const std::string& type, const std::string& bytes, bool image);
    void uploadPending(bool imagesOnly = false);
    void prepareAttachments(Draft& draft, size_t cursor, bool imagesOnly = false);
    void metadataMenu();
    void updateMetadata();
    void searchMemos(bool more = false);
    void confirmPanel();
    void locate();
    void locationTick();
    void stopLocation();
    void record();
    void finishRecording(bool attach);
    void recordTick();
    void setView(View view);
    void setBusy(bool busy);
    void setNotice(const std::string& notice, bool informational = false);
    void enterCompose();
    void login();
    void save();
    void logout();
    void settingsSave();
    void trayMenu();
    void browser();
    void launch(Action action, std::function<void(Completion&)> work);
    void complete(std::unique_ptr<Completion> result);
    void restore();
    void quit();
    bool registerHotkey(unsigned modifiers, unsigned key, bool alternate = false);
    std::wstring text(int id) const;
    void text(int id, const std::wstring& value);
    HWND control(int id) const;
    void place(int id, int x, int y, int width, int height);
    Store& store_;
    Config config_;
    HWND handle_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND tips_ = nullptr;
    std::string pendingLanguage_;
    std::map<int, HWND> controls_;
    std::unique_ptr<Client> client_;
    User user_;
    View view_ = View::Login;
    View beforeSettings_ = View::Login;
    std::jthread worker_;
    bool busy_ = false;
    bool draftDirty_ = false;
    bool hotkeyRegistered_ = false;
    bool settingEditor_ = false;
    bool smoke_ = false;
    bool positionReady_ = false;
    bool startupChecked_ = false;
    bool resumePending_ = false;
    unsigned resumeAttempts_ = 0;
    int visibility_ = 0;
    int wheelDelta_ = 0;
    Draft extras_;
    MemoPage memoPage_;
    std::string memoSearch_;
    AudioRecorder recorder_;
    LocationReader locationReader_;
    LocationNames locationNames_;
    std::wstring locationAutomaticName_, locationNameBaseline_, locationLatitudeBaseline_, locationLongitudeBaseline_;
    bool locationNameEdited_ = false, locationNameReplaceable_ = false;
    std::string recording_;
    unsigned dpi_ = 96;
    HFONT font_ = nullptr, headingFont_ = nullptr, smallFont_ = nullptr;
    HBRUSH background_ = nullptr, inputBackground_ = nullptr;
    std::string notice_;
    bool noticeInformational_ = false;
    UINT taskbarCreated_ = 0;
    int hotkeyId_ = 1;
};
} // namespace mq
