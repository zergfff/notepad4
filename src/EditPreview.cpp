/******************************************************************************
*
* Notepad4
*
* EditPreview.cpp
*   Markdown live preview pane (WebView2)
*
* Convert the current Markdown document to HTML with cmark and render it
* inside a WebView2 control in a split view on the right side.
*
******************************************************************************/
#include <windows.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <objbase.h>
#include <string>
#include <cstdlib>
#include <cstddef>
#include <WebView2.h>

// cmark is compiled as C with the default (Cdecl) calling convention.
// Declare the one function we need explicitly to avoid /Gv (VectorCall).
extern "C" char * __cdecl cmark_markdown_to_html(const char *text, size_t len, int options);

#include "Helpers.h"
#include "SciCall.h"
#include "Dialogs.h"
#include "Styles.h"
#include "Notepad4.h"
#include "resource.h"
#include "EditPreview.h"

#if NP2_SUPPORT_MD_PREVIEW

//! maximum document length (bytes) for live preview to avoid freezing the UI
#define MD_PREVIEW_MAX_SIZE		(1024 * 1024)
//! debounce delay in milliseconds before refreshing the preview
#define MD_PREVIEW_DEBOUNCE		300
//! default split pane width (CSS pixels, scaled by DPI)
#define MD_PREVIEW_SPLIT_WIDTH	400
//! gap between editor and preview pane
#define MD_PREVIEW_GAP			6

static const WCHAR *MD_PREVIEW_INI_KEY = L"MarkdownPreview";

//=============================================================================
// HTML template, %s is replaced with the cmark generated body fragment
//=============================================================================
static const char kHtmlHead[] = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body { font-family: -apple-system, "Segoe UI", "Microsoft YaHei", sans-serif;
       font-size: 14px; line-height: 1.7; margin: 16px; color: #1f2328; background: #ffffff; }
h1, h2, h3, h4, h5, h6 { margin: 1.2em 0 0.6em; line-height: 1.3; }
h1 { border-bottom: 1px solid #d8dee4; padding-bottom: .3em; }
h2 { border-bottom: 1px solid #d8dee4; padding-bottom: .3em; }
a { color: #0969da; text-decoration: none; }
a:hover { text-decoration: underline; }
code { font-family: Consolas, "Courier New", monospace; font-size: 88%;
       background: #f6f8fa; padding: 2px 4px; border-radius: 4px; }
pre { background: #f6f8fa; padding: 12px; border-radius: 6px; overflow: auto; }
pre code { background: none; padding: 0; }
blockquote { margin: 0; padding-left: 12px; border-left: 4px solid #d0d7de; color: #57606a; }
table { border-collapse: collapse; margin: 8px 0; }
th, td { border: 1px solid #d0d7de; padding: 6px 12px; }
th { background: #f6f8fa; font-weight: 600; }
img { max-width: 100%; }
hr { border: none; border-top: 1px solid #d0d7de; margin: 1.5em 0; }
@media (prefers-color-scheme: dark) {
  body { color: #c9d1d9; background: #0d1117; }
  a { color: #58a6ff; }
  code { background: #161b22; }
  pre { background: #161b22; }
  blockquote { border-left-color: #30363d; color: #8b949e; }
  th, td { border-color: #30363d; }
  th { background: #161b22; }
  h1, h2 { border-bottom-color: #21262d; }
  hr { border-top-color: #30363d; }
}
</style>
</head>
<body>
)HTML";

static const char kHtmlTail[] = R"HTML(
</body>
</html>
)HTML";

static const WCHAR kPlaceholder[] = L"<html><body style=\"font-family:'Segoe UI','Microsoft YaHei';color:#888;margin:16px;\">Not a Markdown document.</body></html>";

//=============================================================================
// Global state
//=============================================================================
static HWND g_hwndMain = nullptr;
static ICoreWebView2Controller *g_controller = nullptr;
static ICoreWebView2Controller2 *g_controller2 = nullptr;
static ICoreWebView2 *g_webview = nullptr;
static bool g_bInitialized = false;
static bool g_bEnabled = false;
static bool g_bVisible = false;
static bool g_bMarkdown = false;
static bool g_bPendingLayout = false;
static UINT_PTR g_uTimer = 0;
static int g_iSplitWidth = MD_PREVIEW_SPLIT_WIDTH;
static int g_lastY = 0;
static int g_lastCx = 0;
static int g_lastCy = 0;

//=============================================================================
// forward declarations
//=============================================================================
static void EditPreview_Update() noexcept;
static void EditPreview_ApplyLayout() noexcept;
COREWEBVIEW2_COLOR EditPreview_GetDefaultBackgroundColor() noexcept;

class EnvCompletedHandler final : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
	EnvCompletedHandler() = default;

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
		if (riid == __uuidof(IUnknown) || riid == __uuidof(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
			*ppvObject = this;
			AddRef();
			return S_OK;
		}
		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
	ULONG STDMETHODCALLTYPE Release() override { return 1; }

	HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment *env) override {
		if (SUCCEEDED(result) && env != nullptr) {
			class ControllerCompletedHandler final : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
			public:
				ControllerCompletedHandler() = default;

				HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
					if (riid == __uuidof(IUnknown) || riid == __uuidof(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
						*ppvObject = this;
						AddRef();
						return S_OK;
					}
					*ppvObject = nullptr;
					return E_NOINTERFACE;
				}

				ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
				ULONG STDMETHODCALLTYPE Release() override { return 1; }

				HRESULT STDMETHODCALLTYPE Invoke(HRESULT result2, ICoreWebView2Controller *controller) override {
					if (SUCCEEDED(result2) && controller != nullptr) {
						g_controller = controller;
						g_controller->get_CoreWebView2(&g_webview);
						if (g_webview != nullptr) {
							ICoreWebView2Settings *settings = nullptr;
							if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings != nullptr) {
								settings->put_IsStatusBarEnabled(FALSE);
								settings->put_AreDefaultContextMenusEnabled(FALSE);
								settings->put_AreDevToolsEnabled(FALSE);
								settings->put_AreDefaultScriptDialogsEnabled(FALSE);
								settings->put_IsZoomControlEnabled(FALSE);
								settings->Release();
							}
						}
						if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&g_controller2))) && g_controller2 != nullptr) {
							g_controller2->put_DefaultBackgroundColor(EditPreview_GetDefaultBackgroundColor());
						}
						g_controller->put_IsVisible(g_bVisible);
						if (g_bPendingLayout) {
							g_bPendingLayout = false;
							EditPreview_ApplyLayout();
							// re-layout the whole window now that the pane is ready
							PostMessage(g_hwndMain, WM_SIZE, SIZE_RESTORED, MAKELPARAM(g_lastCx, g_lastCy));
						}
						EditPreview_Update();
					}
					delete this;
					return S_OK;
				}
			};
			env->CreateCoreWebView2Controller(g_hwndMain, new ControllerCompletedHandler());
		}
		if (env != nullptr) {
			env->Release();
		}
		delete this;
		return S_OK;
	}
};

//=============================================================================
//
// EditPreview_GetDefaultBackgroundColor()
//
//=============================================================================
COREWEBVIEW2_COLOR EditPreview_GetDefaultBackgroundColor() noexcept {
	const bool dark = np2StyleTheme == StyleTheme_Dark;
	if (dark) {
		return { 255, 13, 17, 23 };	// #0d1117
	}
	return { 255, 255, 255, 255 };	// #ffffff
}

//=============================================================================
//
// EditPreview_Init()
//
//=============================================================================
void EditPreview_Init(HWND hwnd) noexcept {
	g_hwndMain = hwnd;
	g_bEnabled = IniGetInt(INI_SECTION_NAME_FLAGS, MD_PREVIEW_INI_KEY, 0) != 0;
}

//=============================================================================
//
// EditPreview_OnDestroy()
//
//=============================================================================
void EditPreview_OnDestroy() noexcept {
	if (g_uTimer != 0) {
		KillTimer(g_hwndMain, ID_MDPREVIEWTIMER);
		g_uTimer = 0;
	}
	if (g_controller != nullptr) {
		g_controller->Release();
		g_controller = nullptr;
	}
	if (g_controller2 != nullptr) {
		g_controller2->Release();
		g_controller2 = nullptr;
	}
	if (g_webview != nullptr) {
		g_webview->Release();
		g_webview = nullptr;
	}
	g_bInitialized = false;
}

//=============================================================================
//
// EditPreview_IsVisible()
//
//=============================================================================
bool EditPreview_IsVisible() noexcept {
	return g_bVisible;
}

//=============================================================================
//
// EditPreview_IsMarkdown()
//
//=============================================================================
bool EditPreview_IsMarkdown() noexcept {
	return pLexCurrent != nullptr && pLexCurrent->iLexer == SCLEX_MARKDOWN;
}

//=============================================================================
//
// EditPreview_InitWebView()
//
//=============================================================================
static void EditPreview_InitWebView() noexcept {
	if (g_bInitialized) {
		return;
	}
	g_bInitialized = true;
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr, new EnvCompletedHandler());
}

//=============================================================================
//
// EditPreview_ApplyLayout()
//
//=============================================================================
static void EditPreview_ApplyLayout() noexcept {
	if (g_controller == nullptr) {
		return;
	}
	if (!g_bVisible) {
		g_controller->put_IsVisible(FALSE);
		return;
	}
	RECT rc;
	rc.left = g_lastCx - g_iSplitWidth;
	rc.top = g_lastY;
	rc.right = g_lastCx;
	rc.bottom = g_lastY + g_lastCy;
	g_controller->put_Bounds(rc);
	g_controller->put_IsVisible(TRUE);
}

//=============================================================================
//
// EditPreview_OnSize()
//
//   Returns the editor pane width for the given client area.
//
//=============================================================================
int EditPreview_OnSize(int y, int cx, int cy) noexcept {
	g_lastY = y;
	g_lastCx = cx;
	g_lastCy = cy;
	if (!g_bVisible) {
		return cx;
	}
	if (g_controller == nullptr) {
		// pane not ready yet, mark for layout once initialized
		g_bPendingLayout = true;
		return cx;
	}
	int editWidth = cx - g_iSplitWidth - MD_PREVIEW_GAP;
	if (editWidth < 200) {
		editWidth = 200;
		g_iSplitWidth = cx - 200 - MD_PREVIEW_GAP;
		if (g_iSplitWidth < 200) {
			g_iSplitWidth = 200;
		}
	}
	EditPreview_ApplyLayout();
	return editWidth;
}

//=============================================================================
//
// EditPreview_ShowPlaceholder()
//
//=============================================================================
static void EditPreview_ShowPlaceholder(LPCWSTR text) noexcept {
	if (g_webview == nullptr) {
		return;
	}
	BSTR bstr = SysAllocString(text);
	if (bstr != nullptr) {
		g_webview->NavigateToString(bstr);
		SysFreeString(bstr);
	}
}

//=============================================================================
//
// EditPreview_Update()
//
//=============================================================================
static void EditPreview_Update() noexcept {
	if (g_webview == nullptr) {
		return;
	}
	if (!g_bVisible) {
		return;
	}
	if (!EditPreview_IsMarkdown()) {
		EditPreview_ShowPlaceholder(kPlaceholder);
		return;
	}

	const Sci_Position len = SciCall_GetLength();
	if (len <= 0 || len > MD_PREVIEW_MAX_SIZE) {
		EditPreview_ShowPlaceholder(L"<html><body style=\"font-family:'Segoe UI','Microsoft YaHei';color:#888;margin:16px;\">Document too large for live preview.</body></html>");
		return;
	}

	char *pText = static_cast<char *>(NP2HeapAlloc(static_cast<size_t>(len) + 1));
	if (pText == nullptr) {
		return;
	}
	SciCall_GetText(len + 1, pText);

	char *pBody = cmark_markdown_to_html(pText, static_cast<size_t>(len), 0);
	NP2HeapFree(pText);
	if (pBody == nullptr) {
		return;
	}

	std::string fullHtml;
	fullHtml.reserve(CSTRLEN(kHtmlHead) + strlen(pBody) + CSTRLEN(kHtmlTail) + 1);
	fullHtml.assign(kHtmlHead);
	fullHtml.append(pBody);
	fullHtml.append(kHtmlTail);
	free(pBody);

	// convert UTF-8 to UTF-16 for NavigateToString()
	const int wlen = MultiByteToWideChar(CP_UTF8, 0, fullHtml.data(), static_cast<int>(fullHtml.size()), nullptr, 0);
	if (wlen > 0) {
		wchar_t *pwsz = static_cast<wchar_t *>(NP2HeapAlloc(static_cast<size_t>(wlen + 1) * sizeof(wchar_t)));
		if (pwsz != nullptr) {
			MultiByteToWideChar(CP_UTF8, 0, fullHtml.data(), static_cast<int>(fullHtml.size()), pwsz, wlen);
			pwsz[wlen] = L'\0';
			BSTR bstr = SysAllocStringLen(pwsz, wlen);
			if (bstr != nullptr) {
				g_webview->NavigateToString(bstr);
				SysFreeString(bstr);
			}
			NP2HeapFree(pwsz);
		}
	}
}

//=============================================================================
//
// EditPreview_OnTimer()
//
//=============================================================================
void EditPreview_OnTimer() noexcept {
	if (g_uTimer != 0) {
		KillTimer(g_hwndMain, ID_MDPREVIEWTIMER);
		g_uTimer = 0;
	}
	EditPreview_Update();
}

//=============================================================================
//
// EditPreview_OnDocumentChanged()
//
//   Called on SCN_MODIFIED, schedules a debounced refresh.
//
//=============================================================================
void EditPreview_OnDocumentChanged() noexcept {
	if (!g_bVisible || !EditPreview_IsMarkdown()) {
		return;
	}
	if (g_uTimer != 0) {
		KillTimer(g_hwndMain, ID_MDPREVIEWTIMER);
	}
	g_uTimer = SetTimer(g_hwndMain, ID_MDPREVIEWTIMER, MD_PREVIEW_DEBOUNCE, nullptr);
}

//=============================================================================
//
// EditPreview_OnFileOpened()
//
//   Called after the current file/lexer has changed.
//
//=============================================================================
void EditPreview_OnFileOpened() noexcept {
	if (!g_bVisible) {
		return;
	}
	EditPreview_Update();
}

//=============================================================================
//
// EditPreview_OnThemeChanged()
//
//=============================================================================
void EditPreview_OnThemeChanged() noexcept {
	if (g_controller2 != nullptr) {
		g_controller2->put_DefaultBackgroundColor(EditPreview_GetDefaultBackgroundColor());
	}
	if (g_bVisible) {
		EditPreview_Update();
	}
}

//=============================================================================
//
// EditPreview_Toggle()
//
//=============================================================================
void EditPreview_Toggle() noexcept {
	if (!g_bEnabled) {
		MsgBoxInfo(MB_OK, IDS_ERR_MDPREVIEW_DISABLED);
		return;
	}
	g_bVisible = !g_bVisible;

	if (g_bVisible) {
		EditPreview_InitWebView();
		// request an immediate layout of the split view
		if (g_controller == nullptr) {
			g_bPendingLayout = true;
		} else {
			EditPreview_ApplyLayout();
		}
		EditPreview_OnDocumentChanged();
	} else {
		if (g_uTimer != 0) {
			KillTimer(g_hwndMain, ID_MDPREVIEWTIMER);
			g_uTimer = 0;
		}
		if (g_controller != nullptr) {
			g_controller->put_IsVisible(FALSE);
		}
	}

	// re-layout the editor window
	PostMessage(g_hwndMain, WM_SIZE, SIZE_RESTORED, MAKELPARAM(g_lastCx, g_lastCy));
}

#endif // NP2_SUPPORT_MD_PREVIEW
