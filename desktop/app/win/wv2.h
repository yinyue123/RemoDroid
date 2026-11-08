/* The WebView2 COM interfaces the panel window (web.c) uses, in C. Each vtable lists every method of the
 * interface in the order of the SDK's WebView2.h (Microsoft.Web.WebView2 1.0, BSD-3-Clause), so the slots line
 * up; the methods web.c does not call are plain pointers. The runtime itself is found through the registry
 * (web.c), so neither the SDK nor WebView2Loader.dll is needed to build or ship. */
#pragma once
#include <windows.h>

typedef struct EventRegistrationToken { __int64 value; } EventRegistrationToken;

typedef struct ICoreWebView2Environment ICoreWebView2Environment;
typedef struct ICoreWebView2Controller ICoreWebView2Controller;
typedef struct ICoreWebView2 ICoreWebView2;
typedef struct ICoreWebView2NewWindowRequestedEventArgs ICoreWebView2NewWindowRequestedEventArgs;
typedef struct ICoreWebView2NavigationStartingEventArgs ICoreWebView2NavigationStartingEventArgs;
typedef struct ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler;
typedef struct ICoreWebView2CreateCoreWebView2ControllerCompletedHandler ICoreWebView2CreateCoreWebView2ControllerCompletedHandler;
typedef struct ICoreWebView2NavigationStartingEventHandler ICoreWebView2NavigationStartingEventHandler;
typedef struct ICoreWebView2NewWindowRequestedEventHandler ICoreWebView2NewWindowRequestedEventHandler;
typedef struct ICoreWebView2DocumentTitleChangedEventHandler ICoreWebView2DocumentTitleChangedEventHandler;
typedef struct ICoreWebView2ContainsFullScreenElementChangedEventHandler ICoreWebView2ContainsFullScreenElementChangedEventHandler;
typedef struct ICoreWebView2AcceleratorKeyPressedEventHandler ICoreWebView2AcceleratorKeyPressedEventHandler;
typedef struct ICoreWebView2AcceleratorKeyPressedEventArgs ICoreWebView2AcceleratorKeyPressedEventArgs;

struct ICoreWebView2EnvironmentVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2Environment *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ICoreWebView2Environment *);
    ULONG(STDMETHODCALLTYPE *Release)(ICoreWebView2Environment *);
    HRESULT(STDMETHODCALLTYPE *CreateCoreWebView2Controller)(ICoreWebView2Environment *, HWND, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *);
    void *CreateWebResourceResponse;
    void *get_BrowserVersionString;
    void *add_NewBrowserVersionAvailable;
    void *remove_NewBrowserVersionAvailable;
};
struct ICoreWebView2Environment { const struct ICoreWebView2EnvironmentVtbl *lpVtbl; };

struct ICoreWebView2ControllerVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2Controller *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ICoreWebView2Controller *);
    ULONG(STDMETHODCALLTYPE *Release)(ICoreWebView2Controller *);
    void *get_IsVisible;
    HRESULT(STDMETHODCALLTYPE *put_IsVisible)(ICoreWebView2Controller *, BOOL);
    void *get_Bounds;
    HRESULT(STDMETHODCALLTYPE *put_Bounds)(ICoreWebView2Controller *, RECT);
    void *get_ZoomFactor;
    void *put_ZoomFactor;
    void *add_ZoomFactorChanged;
    void *remove_ZoomFactorChanged;
    void *SetBoundsAndZoomFactor;
    HRESULT(STDMETHODCALLTYPE *MoveFocus)(ICoreWebView2Controller *, int);
    void *add_MoveFocusRequested;
    void *remove_MoveFocusRequested;
    void *add_GotFocus;
    void *remove_GotFocus;
    void *add_LostFocus;
    void *remove_LostFocus;
    HRESULT(STDMETHODCALLTYPE *add_AcceleratorKeyPressed)(ICoreWebView2Controller *, ICoreWebView2AcceleratorKeyPressedEventHandler *, EventRegistrationToken *);
    void *remove_AcceleratorKeyPressed;
    void *get_ParentWindow;
    HRESULT(STDMETHODCALLTYPE *put_ParentWindow)(ICoreWebView2Controller *, HWND);
    HRESULT(STDMETHODCALLTYPE *NotifyParentWindowPositionChanged)(ICoreWebView2Controller *);
    HRESULT(STDMETHODCALLTYPE *Close)(ICoreWebView2Controller *);
    HRESULT(STDMETHODCALLTYPE *get_CoreWebView2)(ICoreWebView2Controller *, ICoreWebView2 **);
};
struct ICoreWebView2Controller { const struct ICoreWebView2ControllerVtbl *lpVtbl; };

struct ICoreWebView2Vtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2 *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ICoreWebView2 *);
    ULONG(STDMETHODCALLTYPE *Release)(ICoreWebView2 *);
    void *get_Settings;
    HRESULT(STDMETHODCALLTYPE *get_Source)(ICoreWebView2 *, LPWSTR *);
    HRESULT(STDMETHODCALLTYPE *Navigate)(ICoreWebView2 *, LPCWSTR);
    void *NavigateToString;
    HRESULT(STDMETHODCALLTYPE *add_NavigationStarting)(ICoreWebView2 *, ICoreWebView2NavigationStartingEventHandler *, EventRegistrationToken *);
    void *remove_NavigationStarting;
    void *add_ContentLoading;
    void *remove_ContentLoading;
    void *add_SourceChanged;
    void *remove_SourceChanged;
    void *add_HistoryChanged;
    void *remove_HistoryChanged;
    void *add_NavigationCompleted;
    void *remove_NavigationCompleted;
    void *add_FrameNavigationStarting;
    void *remove_FrameNavigationStarting;
    void *add_FrameNavigationCompleted;
    void *remove_FrameNavigationCompleted;
    void *add_ScriptDialogOpening;
    void *remove_ScriptDialogOpening;
    void *add_PermissionRequested;
    void *remove_PermissionRequested;
    void *add_ProcessFailed;
    void *remove_ProcessFailed;
    void *AddScriptToExecuteOnDocumentCreated;
    void *RemoveScriptToExecuteOnDocumentCreated;
    void *ExecuteScript;
    void *CapturePreview;
    HRESULT(STDMETHODCALLTYPE *Reload)(ICoreWebView2 *);
    void *PostWebMessageAsJson;
    void *PostWebMessageAsString;
    void *add_WebMessageReceived;
    void *remove_WebMessageReceived;
    void *CallDevToolsProtocolMethod;
    void *get_BrowserProcessId;
    void *get_CanGoBack;
    void *get_CanGoForward;
    void *GoBack;
    void *GoForward;
    void *GetDevToolsProtocolEventReceiver;
    void *Stop;
    HRESULT(STDMETHODCALLTYPE *add_NewWindowRequested)(ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventHandler *, EventRegistrationToken *);
    void *remove_NewWindowRequested;
    HRESULT(STDMETHODCALLTYPE *add_DocumentTitleChanged)(ICoreWebView2 *, ICoreWebView2DocumentTitleChangedEventHandler *, EventRegistrationToken *);
    void *remove_DocumentTitleChanged;
    HRESULT(STDMETHODCALLTYPE *get_DocumentTitle)(ICoreWebView2 *, LPWSTR *);
    void *AddHostObjectToScript;
    void *RemoveHostObjectFromScript;
    void *OpenDevToolsWindow;
    HRESULT(STDMETHODCALLTYPE *add_ContainsFullScreenElementChanged)(ICoreWebView2 *, ICoreWebView2ContainsFullScreenElementChangedEventHandler *, EventRegistrationToken *);
    void *remove_ContainsFullScreenElementChanged;
    HRESULT(STDMETHODCALLTYPE *get_ContainsFullScreenElement)(ICoreWebView2 *, BOOL *);
    void *add_WebResourceRequested;
    void *remove_WebResourceRequested;
    void *AddWebResourceRequestedFilter;
    void *RemoveWebResourceRequestedFilter;
    void *add_WindowCloseRequested;
    void *remove_WindowCloseRequested;
};
struct ICoreWebView2 { const struct ICoreWebView2Vtbl *lpVtbl; };

struct ICoreWebView2NewWindowRequestedEventArgsVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2NewWindowRequestedEventArgs *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ICoreWebView2NewWindowRequestedEventArgs *);
    ULONG(STDMETHODCALLTYPE *Release)(ICoreWebView2NewWindowRequestedEventArgs *);
    HRESULT(STDMETHODCALLTYPE *get_Uri)(ICoreWebView2NewWindowRequestedEventArgs *, LPWSTR *);
    void *put_NewWindow;
    void *get_NewWindow;
    HRESULT(STDMETHODCALLTYPE *put_Handled)(ICoreWebView2NewWindowRequestedEventArgs *, BOOL);
    void *get_Handled;
    void *get_IsUserInitiated;
    void *GetDeferral;
    void *get_WindowFeatures;
};
struct ICoreWebView2NewWindowRequestedEventArgs { const struct ICoreWebView2NewWindowRequestedEventArgsVtbl *lpVtbl; };

struct ICoreWebView2NavigationStartingEventArgsVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2NavigationStartingEventArgs *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ICoreWebView2NavigationStartingEventArgs *);
    ULONG(STDMETHODCALLTYPE *Release)(ICoreWebView2NavigationStartingEventArgs *);
    HRESULT(STDMETHODCALLTYPE *get_Uri)(ICoreWebView2NavigationStartingEventArgs *, LPWSTR *);
    HRESULT(STDMETHODCALLTYPE *get_IsUserInitiated)(ICoreWebView2NavigationStartingEventArgs *, BOOL *);
    void *get_IsRedirected;
    void *get_RequestHeaders;
    void *get_Cancel;
    HRESULT(STDMETHODCALLTYPE *put_Cancel)(ICoreWebView2NavigationStartingEventArgs *, BOOL);
    void *get_NavigationId;
};
struct ICoreWebView2NavigationStartingEventArgs { const struct ICoreWebView2NavigationStartingEventArgsVtbl *lpVtbl; };

struct ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
    ULONG(STDMETHODCALLTYPE *Release)(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
    HRESULT(STDMETHODCALLTYPE *Invoke)(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *, HRESULT, ICoreWebView2Environment *);
};
struct ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler { const struct ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl *lpVtbl; };

struct ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *);
    ULONG(STDMETHODCALLTYPE *Release)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *);
    HRESULT(STDMETHODCALLTYPE *Invoke)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *, HRESULT, ICoreWebView2Controller *);
};
struct ICoreWebView2CreateCoreWebView2ControllerCompletedHandler { const struct ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl *lpVtbl; };

struct ICoreWebView2NavigationStartingEventHandlerVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2NavigationStartingEventHandler *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ICoreWebView2NavigationStartingEventHandler *);
    ULONG(STDMETHODCALLTYPE *Release)(ICoreWebView2NavigationStartingEventHandler *);
    HRESULT(STDMETHODCALLTYPE *Invoke)(ICoreWebView2NavigationStartingEventHandler *, ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *);
};
struct ICoreWebView2NavigationStartingEventHandler { const struct ICoreWebView2NavigationStartingEventHandlerVtbl *lpVtbl; };

struct ICoreWebView2NewWindowRequestedEventHandlerVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2NewWindowRequestedEventHandler *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ICoreWebView2NewWindowRequestedEventHandler *);
    ULONG(STDMETHODCALLTYPE *Release)(ICoreWebView2NewWindowRequestedEventHandler *);
    HRESULT(STDMETHODCALLTYPE *Invoke)(ICoreWebView2NewWindowRequestedEventHandler *, ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *);
};
struct ICoreWebView2NewWindowRequestedEventHandler { const struct ICoreWebView2NewWindowRequestedEventHandlerVtbl *lpVtbl; };

struct ICoreWebView2DocumentTitleChangedEventHandlerVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2DocumentTitleChangedEventHandler *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ICoreWebView2DocumentTitleChangedEventHandler *);
    ULONG(STDMETHODCALLTYPE *Release)(ICoreWebView2DocumentTitleChangedEventHandler *);
    HRESULT(STDMETHODCALLTYPE *Invoke)(ICoreWebView2DocumentTitleChangedEventHandler *, ICoreWebView2 *, IUnknown *);
};
struct ICoreWebView2DocumentTitleChangedEventHandler { const struct ICoreWebView2DocumentTitleChangedEventHandlerVtbl *lpVtbl; };

struct ICoreWebView2ContainsFullScreenElementChangedEventHandlerVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2ContainsFullScreenElementChangedEventHandler *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ICoreWebView2ContainsFullScreenElementChangedEventHandler *);
    ULONG(STDMETHODCALLTYPE *Release)(ICoreWebView2ContainsFullScreenElementChangedEventHandler *);
    HRESULT(STDMETHODCALLTYPE *Invoke)(ICoreWebView2ContainsFullScreenElementChangedEventHandler *, ICoreWebView2 *, IUnknown *);
};
struct ICoreWebView2ContainsFullScreenElementChangedEventHandler { const struct ICoreWebView2ContainsFullScreenElementChangedEventHandlerVtbl *lpVtbl; };

/* COREWEBVIEW2_KEY_EVENT_KIND */
#define COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN 0
#define COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN 2
struct ICoreWebView2AcceleratorKeyPressedEventArgsVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2AcceleratorKeyPressedEventArgs *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ICoreWebView2AcceleratorKeyPressedEventArgs *);
    ULONG(STDMETHODCALLTYPE *Release)(ICoreWebView2AcceleratorKeyPressedEventArgs *);
    HRESULT(STDMETHODCALLTYPE *get_KeyEventKind)(ICoreWebView2AcceleratorKeyPressedEventArgs *, int *);
    HRESULT(STDMETHODCALLTYPE *get_VirtualKey)(ICoreWebView2AcceleratorKeyPressedEventArgs *, UINT *);
    void *get_KeyEventLParam;
    void *get_PhysicalKeyStatus;
    void *get_Handled;
    HRESULT(STDMETHODCALLTYPE *put_Handled)(ICoreWebView2AcceleratorKeyPressedEventArgs *, BOOL);
};
struct ICoreWebView2AcceleratorKeyPressedEventArgs { const struct ICoreWebView2AcceleratorKeyPressedEventArgsVtbl *lpVtbl; };

struct ICoreWebView2AcceleratorKeyPressedEventHandlerVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2AcceleratorKeyPressedEventHandler *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ICoreWebView2AcceleratorKeyPressedEventHandler *);
    ULONG(STDMETHODCALLTYPE *Release)(ICoreWebView2AcceleratorKeyPressedEventHandler *);
    HRESULT(STDMETHODCALLTYPE *Invoke)(ICoreWebView2AcceleratorKeyPressedEventHandler *, ICoreWebView2Controller *, ICoreWebView2AcceleratorKeyPressedEventArgs *);
};
struct ICoreWebView2AcceleratorKeyPressedEventHandler { const struct ICoreWebView2AcceleratorKeyPressedEventHandlerVtbl *lpVtbl; };

typedef struct ICoreWebView2EnvironmentOptions ICoreWebView2EnvironmentOptions;
struct ICoreWebView2EnvironmentOptionsVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2EnvironmentOptions *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ICoreWebView2EnvironmentOptions *);
    ULONG(STDMETHODCALLTYPE *Release)(ICoreWebView2EnvironmentOptions *);
    HRESULT(STDMETHODCALLTYPE *get_AdditionalBrowserArguments)(ICoreWebView2EnvironmentOptions *, LPWSTR *);
    HRESULT(STDMETHODCALLTYPE *put_AdditionalBrowserArguments)(ICoreWebView2EnvironmentOptions *, LPCWSTR);
    HRESULT(STDMETHODCALLTYPE *get_Language)(ICoreWebView2EnvironmentOptions *, LPWSTR *);
    HRESULT(STDMETHODCALLTYPE *put_Language)(ICoreWebView2EnvironmentOptions *, LPCWSTR);
    HRESULT(STDMETHODCALLTYPE *get_TargetCompatibleBrowserVersion)(ICoreWebView2EnvironmentOptions *, LPWSTR *);
    HRESULT(STDMETHODCALLTYPE *put_TargetCompatibleBrowserVersion)(ICoreWebView2EnvironmentOptions *, LPCWSTR);
    HRESULT(STDMETHODCALLTYPE *get_AllowSingleSignOnUsingOSPrimaryAccount)(ICoreWebView2EnvironmentOptions *, BOOL *);
    HRESULT(STDMETHODCALLTYPE *put_AllowSingleSignOnUsingOSPrimaryAccount)(ICoreWebView2EnvironmentOptions *, BOOL);
};
struct ICoreWebView2EnvironmentOptions { const struct ICoreWebView2EnvironmentOptionsVtbl *lpVtbl; };

static const IID IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler = {0x4e8a3389,0xc9d8,0x4bd2,{0xb6,0xb5,0x12,0x4f,0xee,0x6c,0xc1,0x4d}};
static const IID IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler = {0x6c4819f3,0xc9b7,0x4260,{0x81,0x27,0xc9,0xf5,0xbd,0xe7,0xf6,0x8c}};
static const IID IID_ICoreWebView2NavigationStartingEventHandler = {0x9adbe429,0xf36d,0x432b,{0x9d,0xdc,0xf8,0x88,0x1f,0xbd,0x76,0xe3}};
static const IID IID_ICoreWebView2NewWindowRequestedEventHandler = {0xd4c185fe,0xc81c,0x4989,{0x97,0xaf,0x2d,0x3f,0xa7,0xab,0x56,0x51}};
static const IID IID_ICoreWebView2DocumentTitleChangedEventHandler = {0xf5f2b923,0x953e,0x4042,{0x9f,0x95,0xf3,0xa1,0x18,0xe1,0xaf,0xd4}};
static const IID IID_ICoreWebView2ContainsFullScreenElementChangedEventHandler = {0xe45d98b1,0xafef,0x45be,{0x8b,0xaf,0x6c,0x77,0x28,0x86,0x7f,0x73}};
static const IID IID_ICoreWebView2AcceleratorKeyPressedEventHandler = {0xb29c7e28,0xfa79,0x41a8,{0x8e,0x44,0x65,0x81,0x1c,0x76,0xdc,0xb2}};
static const IID IID_ICoreWebView2EnvironmentOptions = {0x2fde08a8,0x1e9a,0x4766,{0x8c,0x05,0x95,0xa9,0xce,0xb9,0xd1,0xc5}};

#define COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC 0
