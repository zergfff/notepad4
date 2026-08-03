/******************************************************************************
*
* Notepad4
*
* EditPreview.cpp
*   Markdown live preview pane (WebView2)
*
* The current Markdown document is rendered inside a WebView2 control in a
* split view on the right side. Rendering is done in JavaScript by marked
* (GFM support) and mermaid (diagrams), which are loaded from the virtual
* host "appassets" that maps to the folder of Notepad4.exe.
*
******************************************************************************/
#include <windows.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <objbase.h>
#include <oleauto.h>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <cstddef>
#include <cwchar>
#include <WebView2.h>

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
//! default editor pane width in the split view
#define MD_PREVIEW_SPLIT_WIDTH	480
//! minimum editor pane width
#define MD_PREVIEW_MIN_WIDTH	200
//! width of the draggable splitter
#define MD_PREVIEW_SPLITTER_W	4

//! virtual host name that maps to the Notepad4.exe folder
#define MD_PREVIEW_ASSETS_HOST	L"appassets"

static const WCHAR *MD_PREVIEW_WD_INI_KEY = L"MarkdownPreviewWidth";

static const WCHAR kPlaceholder[] = L"<html><head><meta charset=\"utf-8\"></head><body style=\"font-family:'Segoe UI','Microsoft YaHei';color:#888;margin:16px;\">Not a Markdown document.</body></html>";
static const WCHAR kTooLarge[] = L"<html><head><meta charset=\"utf-8\"></head><body style=\"font-family:'Segoe UI','Microsoft YaHei';color:#888;margin:16px;\">Document too large for live preview.</body></html>";

//=============================================================================
// HTML template. The Markdown source is embedded as JSON inside
// <script type="application/json" id="md-source">.
//=============================================================================
static const char kHtmlHead[] = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<script src="https://appassets/marked.min.js"></script>
<script src="https://appassets/mermaid.min.js"></script>
<style>
:root {
  --bg: #ffffff; --fg: #1f2328; --border: #d8dee4; --code-bg: #f6f8fa;
  --quote: #d0d7de; --muted: #57606a; --link: #0969da;
}
body[data-theme="light"] {
  --bg: #ffffff; --fg: #1f2328; --border: #d8dee4; --code-bg: #f6f8fa;
  --quote: #d0d7de; --muted: #57606a; --link: #0969da;
}
body[data-theme="dark"] {
  --bg: #0d1117; --fg: #c9d1d9; --border: #21262d; --code-bg: #161b22;
  --quote: #30363d; --muted: #8b949e; --link: #58a6ff;
}
@media (prefers-color-scheme: dark) {
  body[data-theme="auto"] {
    --bg: #0d1117; --fg: #c9d1d9; --border: #21262d; --code-bg: #161b22;
    --quote: #30363d; --muted: #8b949e; --link: #58a6ff;
  }
}
body { background: var(--bg); color: var(--fg); min-height: 100vh;
       box-sizing: border-box; margin: 0; padding: 16px;
       font-family: -apple-system, "Segoe UI", "Microsoft YaHei", sans-serif;
       font-size: 14px; line-height: 1.7; }
h1, h2, h3, h4, h5, h6 { margin: 1.2em 0 0.6em; line-height: 1.3; }
h1 { border-bottom: 1px solid var(--border); padding-bottom: .3em; }
h2 { border-bottom: 1px solid var(--border); padding-bottom: .3em; }
a { color: var(--link); text-decoration: none; }
a:hover { text-decoration: underline; }
code { font-family: Consolas, "Courier New", monospace; font-size: 88%;
       background: var(--code-bg); padding: 2px 4px; border-radius: 4px; }
pre { background: var(--code-bg); padding: 12px; border-radius: 6px; overflow: auto; }
pre code { background: none; padding: 0; }
blockquote { margin: 0; padding-left: 12px; border-left: 4px solid var(--quote); color: var(--muted); }
table { border-collapse: collapse; margin: 8px 0; }
th, td { border: 1px solid var(--quote); padding: 6px 12px; }
th { background: var(--code-bg); font-weight: 600; }
img { max-width: 100%; }
hr { border: none; border-top: 1px solid var(--quote); margin: 1.5em 0; }
input[type="checkbox"] { margin-right: 6px; }
.mermaid { background: transparent; }
pre.mermaid { text-align: center; }
</style>
</head>
)HTML";

static const char kRenderScript[] = R"HTML(<script>
(function () {
    function render() {
        var body = document.getElementById('md-body');
        if (!body) return;
        var raw = document.getElementById('md-source');
        var md = raw ? JSON.parse(raw.textContent) : '';
        if (typeof marked === 'undefined') {
            body.textContent = md;
            return;
        }
        var html = marked.parse(md);
        body.innerHTML = html;

        document.querySelectorAll('pre > code.language-mermaid').forEach(function (code) {
            var pre = code.parentNode;
            pre.classList.add('mermaid');
            pre.textContent = code.textContent;
            pre.removeChild(code);
        });
        if (typeof mermaid !== 'undefined') {
            var attr = document.body.getAttribute('data-theme');
            var dark = attr === 'dark' || (attr === 'auto' && window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches);
            try {
                mermaid.initialize({ startOnLoad: false, theme: dark ? 'dark' : 'default', securityLevel: 'loose' });
                mermaid.run();
            } catch (e) { /* ignore render errors */ }
        }
    }
    function syncScroll() {
        var doc = document.documentElement;
        var max = doc.scrollHeight - window.innerHeight;
        var r = max > 0 ? (window.scrollY / max) : 0;
        if (window.chrome && window.chrome.webview) {
            window.chrome.webview.postMessage('scroll:' + r.toFixed(4));
        }
    }
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', render);
    } else {
        render();
    }
    window.addEventListener('scroll', syncScroll, { passive: true });
})();
</script>
)HTML";

static const char kHtmlTail[] = R"HTML(
</body>
</html>
)HTML";

//=============================================================================
// Global state
//=============================================================================
static HWND g_hwndMain = nullptr;
static HWND g_hwndSplitter = nullptr;
static ICoreWebView2Controller *g_controller = nullptr;
static ICoreWebView2Controller2 *g_controller2 = nullptr;
static ICoreWebView2 *g_webview = nullptr;
static bool g_bInitialized = false;
static bool g_bVisible = false;
static bool g_bPendingLayout = false;
static bool g_bDragging = false;
static bool g_bSyncingScroll = false;
static UINT_PTR g_uTimer = 0;
static int g_iPreviewTheme = MDPreviewTheme_Auto;
static int g_iSplitWidth = MD_PREVIEW_SPLIT_WIDTH;
static int g_lastY = 0;
static int g_lastCx = 0;
static int g_lastCy = 0;
static double g_dScrollRatio = 0.0;

//=============================================================================
// forward declarations
//=============================================================================
static void EditPreview_ApplyLayout() noexcept;
static void EditPreview_Refresh() noexcept;
static void EditPreview_SaveSplitWidth() noexcept;
static void EditPreview_SyncToPreview() noexcept;
COREWEBVIEW2_COLOR EditPreview_GetDefaultBackgroundColor() noexcept;

//=============================================================================
// JSON escaping for embedding the Markdown source safely inside <script>
//=============================================================================
static void JsonEscapeAppend(const char *text, size_t len, std::string &out) noexcept {
	out.push_back('"');
	for (size_t i = 0; i < len; ++i) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		case '<': out += "\\u003c"; break;
		case '>': out += "\\u003e"; break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	out.push_back('"');
}

//=============================================================================
// Splitter window procedure
//=============================================================================
LRESULT CALLBACK EditPreview_SplitterProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_LBUTTONDOWN:
		SetCapture(hwnd);
		g_bDragging = true;
		return 0;

	case WM_MOUSEMOVE:
		if (g_bDragging) {
			POINT pt;
			GetCursorPos(&pt);
			ScreenToClient(g_hwndMain, &pt);
			int width = pt.x;
			if (width < MD_PREVIEW_MIN_WIDTH) {
				width = MD_PREVIEW_MIN_WIDTH;
			} else if (width > g_lastCx - MD_PREVIEW_MIN_WIDTH - MD_PREVIEW_SPLITTER_W) {
				width = g_lastCx - MD_PREVIEW_MIN_WIDTH - MD_PREVIEW_SPLITTER_W;
			}
			if (width < MD_PREVIEW_MIN_WIDTH) {
				width = MD_PREVIEW_MIN_WIDTH;
			}
			g_iSplitWidth = width;
			// move splitter and preview pane live, editor is laid out on release
			SetWindowPos(g_hwndSplitter, nullptr, width, g_lastY, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
			if (g_controller != nullptr) {
				RECT rc;
				rc.left = width + MD_PREVIEW_SPLITTER_W;
				rc.top = g_lastY;
				rc.right = g_lastCx;
				rc.bottom = g_lastY + g_lastCy;
				g_controller->put_Bounds(rc);
			}
		}
		return 0;

	case WM_LBUTTONUP:
		if (g_bDragging) {
			g_bDragging = false;
			ReleaseCapture();
			EditPreview_SaveSplitWidth();
			PostMessage(g_hwndMain, WM_SIZE, SIZE_RESTORED, MAKELPARAM(g_lastCx, g_lastCy));
		}
		return 0;

	case WM_SETCURSOR:
		SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
		return TRUE;

	case WM_ERASEBKGND:
		return 1;
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

//=============================================================================
//
// WebMessageHandler()
//
//   Receives "scroll:<ratio>" messages from the preview page and forwards
//   them to the main window so the editor can follow the preview.
//
//=============================================================================
class WebMessageHandler final : public ICoreWebView2WebMessageReceivedEventHandler {
	ULONG refCount = 1;
public:
	WebMessageHandler() = default;

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
		if (riid == __uuidof(IUnknown) || riid == __uuidof(ICoreWebView2WebMessageReceivedEventHandler)) {
			*ppvObject = this;
			AddRef();
			return S_OK;
		}
		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount; }

	ULONG STDMETHODCALLTYPE Release() override {
		const ULONG refs = --refCount;
		if (refs == 0) {
			delete this;
			return 0;
		}
		return refs;
	}

	HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args) override {
		UNREFERENCED_PARAMETER(sender);
		if (args != nullptr) {
			LPWSTR json = nullptr;
			// get_WebMessageAsJson returns the raw string as a JSON string, e.g. "\"scroll:0.5\""
			if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json != nullptr) {
				LPCWSTR p = json;
				if (*p == L'"') {
					++p;
				}
				if (WcsStartsWith(p, L"scroll:") && IsWindow(g_hwndMain)) {
					g_dScrollRatio = wcstod(p + CSTRLEN(L"scroll:"), nullptr);
					if (g_dScrollRatio < 0.0) {
						g_dScrollRatio = 0.0;
					} else if (g_dScrollRatio > 1.0) {
						g_dScrollRatio = 1.0;
					}
					PostMessage(g_hwndMain, APPM_MDPREVIEW_SCROLL, 0, 0);
				}
				CoTaskMemFree(json);
			}
		}
		Release();
		return S_OK;
	}
};

//=============================================================================
//
// EditPreview_InitWebView()
//
//=============================================================================
static bool EditPreview_RegisterSplitterClass() noexcept {
	WNDCLASS wc = {};
	wc.lpfnWndProc = EditPreview_SplitterProc;
	wc.hInstance = GetModuleHandle(nullptr);
	wc.lpszClassName = L"Notepad4Splitter";
	wc.hCursor = LoadCursor(nullptr, IDC_SIZEWE);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	if (RegisterClass(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		return false;
	}
	return true;
}

//=============================================================================
//
// EditPreview_CreateSplitter()
//
//=============================================================================
static void EditPreview_CreateSplitter() noexcept {
	if (g_hwndSplitter != nullptr) {
		return;
	}
	if (!EditPreview_RegisterSplitterClass()) {
		return;
	}
	g_hwndSplitter = CreateWindowEx(0, L"Notepad4Splitter", nullptr,
		WS_CHILD | WS_CLIPSIBLINGS, 0, 0, MD_PREVIEW_SPLITTER_W, 0,
		g_hwndMain, nullptr, GetModuleHandle(nullptr), nullptr);
}

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
	g_iSplitWidth = IniGetInt(INI_SECTION_NAME_FLAGS, MD_PREVIEW_WD_INI_KEY, MD_PREVIEW_SPLIT_WIDTH);
	g_iPreviewTheme = IniGetInt(INI_SECTION_NAME_FLAGS, L"MarkdownPreviewTheme", MDPreviewTheme_Auto);
	EditPreview_CreateSplitter();
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
	if (g_hwndSplitter != nullptr) {
		DestroyWindow(g_hwndSplitter);
		g_hwndSplitter = nullptr;
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
class EnvCompletedHandler final : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
	ULONG refCount = 1;
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

	ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount; }

	ULONG STDMETHODCALLTYPE Release() override {
		const ULONG refs = --refCount;
		if (refs == 0) {
			delete this;
			return 0;
		}
		return refs;
	}

	HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment *env) override {
		// the window may be destroyed while WebView2 is still initializing
		if (!IsWindow(g_hwndMain)) {
			Release();
			return S_OK;
		}
		if (SUCCEEDED(result) && env != nullptr) {
			class ControllerCompletedHandler final : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
				ULONG refCount = 1;
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

				ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount; }

				ULONG STDMETHODCALLTYPE Release() override {
					const ULONG refs = --refCount;
					if (refs == 0) {
						delete this;
						return 0;
					}
					return refs;
				}

				HRESULT STDMETHODCALLTYPE Invoke(HRESULT result2, ICoreWebView2Controller *controller) override {
					if (SUCCEEDED(result2) && controller != nullptr) {
						g_controller = controller;
						g_controller->AddRef();	// keep our own reference
						if (!IsWindow(g_hwndMain)) {
							g_controller->Release();
							g_controller = nullptr;
							Release();
							return S_OK;
						}
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
							// map "https://appassets" to the Notepad4.exe folder so that
							// marked.min.js and mermaid.min.js can be loaded offline.
							WCHAR exePath[MAX_PATH];
							GetModuleFileName(nullptr, exePath, COUNTOF(exePath));
							PathRemoveFileSpec(exePath);
							ICoreWebView2_3 *webview3 = nullptr;
							if (SUCCEEDED(g_webview->QueryInterface(IID_PPV_ARGS(&webview3))) && webview3 != nullptr) {
								webview3->SetVirtualHostNameToFolderMapping(MD_PREVIEW_ASSETS_HOST, exePath, COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
								webview3->Release();
							}
							// receive "scroll:<ratio>" messages from the page for sync scrolling
							g_webview->add_WebMessageReceived(new WebMessageHandler(), nullptr);
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
						EditPreview_Refresh();
					}
					Release();	// release the reference created by new
					return S_OK;
				}
			};
			env->CreateCoreWebView2Controller(g_hwndMain, new ControllerCompletedHandler());
		}
		Release();	// release the reference created by new
		return S_OK;
	}
};

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
//   Positions the splitter and the preview pane for the current client area.
//
//=============================================================================
static void EditPreview_ApplyLayout() noexcept {
	if (!g_bVisible) {
		if (g_hwndSplitter != nullptr) {
			ShowWindow(g_hwndSplitter, SW_HIDE);
		}
		if (g_controller != nullptr) {
			g_controller->put_IsVisible(FALSE);
		}
		return;
	}
	if (g_controller == nullptr) {
		g_bPendingLayout = true;
		return;
	}
	if (g_hwndSplitter != nullptr) {
		SetWindowPos(g_hwndSplitter, nullptr, g_iSplitWidth, g_lastY, MD_PREVIEW_SPLITTER_W, g_lastCy, SWP_NOZORDER | SWP_NOACTIVATE);
		ShowWindow(g_hwndSplitter, SW_SHOW);
	}
	RECT rc;
	rc.left = g_iSplitWidth + MD_PREVIEW_SPLITTER_W;
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
		EditPreview_ApplyLayout();
		return cx;
	}
	if (g_iSplitWidth < MD_PREVIEW_MIN_WIDTH) {
		g_iSplitWidth = MD_PREVIEW_MIN_WIDTH;
	}
	if (g_iSplitWidth > cx - MD_PREVIEW_MIN_WIDTH - MD_PREVIEW_SPLITTER_W) {
		g_iSplitWidth = cx - MD_PREVIEW_MIN_WIDTH - MD_PREVIEW_SPLITTER_W;
	}
	if (g_iSplitWidth < MD_PREVIEW_MIN_WIDTH) {
		g_iSplitWidth = MD_PREVIEW_MIN_WIDTH;
	}
	if (g_controller == nullptr) {
		g_bPendingLayout = true;
		return cx;
	}
	EditPreview_ApplyLayout();
	return g_iSplitWidth;
}

//=============================================================================
//
// EditPreview_SaveSplitWidth()
//
//=============================================================================
void EditPreview_SaveSplitWidth() noexcept {
	IniSetInt(INI_SECTION_NAME_FLAGS, MD_PREVIEW_WD_INI_KEY, g_iSplitWidth);
}

//=============================================================================
//
// EditPreview_ShowPage()
//
//=============================================================================
static void EditPreview_ShowPage(LPCWSTR html) noexcept {
	if (g_webview == nullptr) {
		return;
	}
	BSTR bstr = SysAllocString(html);
	if (bstr != nullptr) {
		g_webview->NavigateToString(bstr);
		SysFreeString(bstr);
	}
}

//=============================================================================
//
// EditPreview_Refresh()
//
//   Rebuilds the preview HTML from the current document.
//
//=============================================================================
static void EditPreview_Refresh() noexcept {
	if (g_webview == nullptr) {
		return;
	}
	if (!g_bVisible) {
		return;
	}
	if (!EditPreview_IsMarkdown()) {
		EditPreview_ShowPage(kPlaceholder);
		return;
	}

	const Sci_Position len = SciCall_GetLength();
	if (len <= 0 || len > MD_PREVIEW_MAX_SIZE) {
		EditPreview_ShowPage(kTooLarge);
		return;
	}

	char *pText = static_cast<char *>(NP2HeapAlloc(static_cast<size_t>(len) + 1));
	if (pText == nullptr) {
		return;
	}
	SciCall_GetText(len + 1, pText);

	std::string html;
	html.reserve(static_cast<size_t>(len) * 2 + CSTRLEN(kHtmlHead) + CSTRLEN(kRenderScript) + CSTRLEN(kHtmlTail) + 256);
	html.assign(kHtmlHead);
	const char *themeStr = "auto";
	switch (g_iPreviewTheme) {
	case MDPreviewTheme_Light: themeStr = "light"; break;
	case MDPreviewTheme_Dark: themeStr = "dark"; break;
	}
	html += "<body data-theme=\"";
	html += themeStr;
	html += "\">\n<div id=\"md-body\">Loading...</div>\n<script type=\"application/json\" id=\"md-source\">";
	JsonEscapeAppend(pText, static_cast<size_t>(len), html);
	html += "</script>";
	html += kRenderScript;
	html += kHtmlTail;
	NP2HeapFree(pText);

	// convert UTF-8 to UTF-16 for NavigateToString()
	const int wlen = MultiByteToWideChar(CP_UTF8, 0, html.data(), static_cast<int>(html.size()), nullptr, 0);
	if (wlen > 0) {
		wchar_t *pwsz = static_cast<wchar_t *>(NP2HeapAlloc(static_cast<size_t>(wlen + 1) * sizeof(wchar_t)));
		if (pwsz != nullptr) {
			MultiByteToWideChar(CP_UTF8, 0, html.data(), static_cast<int>(html.size()), pwsz, wlen);
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
	EditPreview_Refresh();
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
	EditPreview_Refresh();
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
		EditPreview_Refresh();
	}
}

//=============================================================================
//
// EditPreview_Toggle()
//
//=============================================================================
void EditPreview_Toggle() noexcept {
	g_bVisible = !g_bVisible;

	if (g_bVisible) {
		EditPreview_InitWebView();
		if (g_controller == nullptr) {
			g_bPendingLayout = true;
		}
		EditPreview_OnDocumentChanged();
	} else {
		if (g_uTimer != 0) {
			KillTimer(g_hwndMain, ID_MDPREVIEWTIMER);
			g_uTimer = 0;
		}
		EditPreview_ApplyLayout();
	}

	// re-layout the editor window
	PostMessage(g_hwndMain, WM_SIZE, SIZE_RESTORED, MAKELPARAM(g_lastCx, g_lastCy));
}

//=============================================================================
//
// EditPreview_GetTheme()
//
//=============================================================================
int EditPreview_GetTheme() noexcept {
	return g_iPreviewTheme;
}

//=============================================================================
//
// EditPreview_SetTheme()
//
//=============================================================================
void EditPreview_SetTheme(int theme) noexcept {
	if (theme < MDPreviewTheme_Auto || theme > MDPreviewTheme_Dark) {
		theme = MDPreviewTheme_Auto;
	}
	if (g_iPreviewTheme != theme) {
		g_iPreviewTheme = theme;
		IniSetInt(INI_SECTION_NAME_FLAGS, L"MarkdownPreviewTheme", theme);
		if (g_bVisible) {
			EditPreview_Refresh();
		}
	}
}

//=============================================================================
//
// EditPreview_SyncToPreview()
//
//   Scrolls the preview pane to match the editor position (ratio based).
//
//=============================================================================
static void EditPreview_SyncToPreview() noexcept {
	if (!g_bVisible || g_webview == nullptr || g_bSyncingScroll) {
		return;
	}
	const Sci_Line total = SciCall_GetLineCount();
	if (total <= 0) {
		return;
	}
	const Sci_Line first = SciCall_GetFirstVisibleLine();
	double ratio = static_cast<double>(first) / total;
	if (ratio < 0.0) {
		ratio = 0.0;
	} else if (ratio > 1.0) {
		ratio = 1.0;
	}
	WCHAR js[128];
	swprintf(js, COUNTOF(js), L"window.scrollTo(0,%f*(document.documentElement.scrollHeight-window.innerHeight));", ratio);
	g_webview->ExecuteScript(js, nullptr);
}

//=============================================================================
//
// EditPreview_OnEditScroll()
//
//   Called when the editor is scrolled (SCN_UPDATEUI, SC_UPDATE_V_SCROLL).
//
//=============================================================================
void EditPreview_OnEditScroll() noexcept {
	EditPreview_SyncToPreview();
}

//=============================================================================
//
// EditPreview_SyncEditScroll()
//
//   Called when the preview pane is scrolled (APPM_MDPREVIEW_SCROLL).
//   Scrolls the editor to match the preview position.
//
//=============================================================================
void EditPreview_SyncEditScroll() noexcept {
	if (!g_bVisible || g_bSyncingScroll) {
		return;
	}
	const Sci_Line total = SciCall_GetLineCount();
	if (total <= 0) {
		return;
	}
	Sci_Line line = static_cast<Sci_Line>(g_dScrollRatio * total);
	if (line < 0) {
		line = 0;
	} else if (line > total - 1) {
		line = total - 1;
	}
	g_bSyncingScroll = true;
	SciCall_SetFirstVisibleLine(line);
	g_bSyncingScroll = false;
}

#endif // NP2_SUPPORT_MD_PREVIEW
