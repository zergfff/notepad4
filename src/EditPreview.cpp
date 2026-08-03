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
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <cstddef>
#include <cstdarg>
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

//=============================================================================
// Logging: append a line to Notepad4_MDPreview.log next to the exe
//=============================================================================
static void EditPreview_Log(const char *fmt, ...) {
	WCHAR exePath[MAX_PATH];
	GetModuleFileName(nullptr, exePath, COUNTOF(exePath));
	PathRemoveFileSpec(exePath);
	WCHAR logPath[MAX_PATH + 32];
	PathCombine(logPath, exePath, L"Notepad4_MDPreview.log");

	SYSTEMTIME st;
	GetLocalTime(&st);
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	FILE *f = nullptr;
	if (_wfopen_s(&f, logPath, L"a") == 0 && f != nullptr) {
		fprintf(f, "[%02u:%02u:%02u.%03u] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
		fclose(f);
	}
}

// throttle the noisy scroll logging to keep the log readable
static void EditPreview_LogScroll(const char *tag, double ratio) noexcept {
	static DWORD lastTick = 0;
	static double lastRatio = -1.0;
	const DWORD now = GetTickCount();
	if (now - lastTick > 500 || (lastRatio < 0.0 || ratio - lastRatio > 0.01 || lastRatio - ratio > 0.01)) {
		EditPreview_Log("[scroll] %s ratio=%.4f", tag, ratio);
		lastTick = now;
		lastRatio = ratio;
	}
}

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
    var md = '';
    var hx = 0;

    function ensureMermaid(cb) {
        if (typeof mermaid !== 'undefined') { cb(); return; }
        var s = document.createElement('script');
        s.src = 'https://appassets/mermaid.min.js';
        s.onload = cb;
        s.onerror = cb;
        document.head.appendChild(s);
    }

    function applyMermaid() {
        var attr = document.body.getAttribute('data-theme');
        var dark = attr === 'dark' || (attr === 'auto' && window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches);
        try {
            mermaid.initialize({ startOnLoad: false, theme: dark ? 'dark' : 'default', securityLevel: 'loose' });
            mermaid.run();
        } catch (e) { /* ignore render errors */ }
    }

    function renderMarkdown(source) {
        var body = document.getElementById('md-body');
        if (!body) return;
        md = source;
        if (typeof marked === 'undefined') { body.textContent = md; return; }
        // Tag ATX headings (# ) with a stable sequential id that matches the
        // editor's heading scan, so anchor indices align. Setext headings and
        // headings inside blockquotes are left untagged.
        var srcLines = md.split('\n');
        hx = 0;
        for (var i = 0; i < srcLines.length; i++) {
            // [^\r\n]+ instead of .+ because '.' does not match CR, which is
            // present on every line of CRLF files
            var m = /^(#{1,6})[ \t]+([^\r\n]+)/.exec(srcLines[i]);
            if (m) {
                var level = m[1].length;
                var inner = marked.parseInline(m[2]);
                srcLines[i] = '<h' + level + ' id="hx-' + (hx++) + '">' + inner + '</h' + level + '>';
            }
        }
        var html = marked.parse(srcLines.join('\n'));
        body.innerHTML = html;
        if (window.chrome && window.chrome.webview) {
            // report how many headings were tagged so the editor can verify
            // the anchor indices are in agreement and fall back if not
            window.chrome.webview.postMessage('headings:' + hx);
        }
        var mermaidBlocks = document.querySelectorAll('pre > code.language-mermaid');
        if (mermaidBlocks.length > 0) {
            mermaidBlocks.forEach(function (code) {
                var pre = code.parentNode;
                pre.classList.add('mermaid');
                pre.textContent = code.textContent;
                pre.removeChild(code);
            });
            ensureMermaid(applyMermaid);
        }
    }
    window.renderMarkdown = renderMarkdown;

    function firstRender() {
        var raw = document.getElementById('md-source');
        var src = raw ? JSON.parse(raw.textContent) : '';
        renderMarkdown(src);
    }

    window.previewAnchor = function (i, pos) {
        var el = document.getElementById('hx-' + i);
        if (!el) return;
        if (pos === undefined || pos < 0 || pos > 1) {
            el.scrollIntoView({ block: 'start' });
            return;
        }
        var target = el.offsetTop;
        var el2 = document.getElementById('hx-' + (i + 1));
        if (el2) target += pos * (el2.offsetTop - el.offsetTop);
        window.scrollTo(0, target - 16);
    };
    function topHeadingRange() {
        var els = document.querySelectorAll('[id^="hx-"]');
        var top = window.scrollY + 16;
        var prev = -1, next = -1;
        for (var i = 0; i < els.length; i++) {
            var idx = parseInt(els[i].id.substring(3), 10);
            if (els[i].offsetTop <= top) prev = idx;
            else { next = idx; break; }
        }
        if (prev >= 0) {
            var pe = document.getElementById('hx-' + prev);
            var pos = 0;
            if (next >= 0) {
                var ne = document.getElementById('hx-' + next);
                var span = ne.offsetTop - pe.offsetTop;
                if (span > 0) {
                    pos = (top - pe.offsetTop) / span;
                    if (pos < 0) pos = 0;
                    else if (pos > 1) pos = 1;
                }
            }
            return prev + ':' + pos.toFixed(4);
        }
        return null;
    }
    function syncScroll() {
        var doc = document.documentElement;
        var max = doc.scrollHeight - window.innerHeight;
        var y = window.scrollY;
        if (window.chrome && window.chrome.webview) {
            if (y <= 0) {
                window.chrome.webview.postMessage('scroll:0');
                return;
            }
            if (max <= 0 || y >= max - 2) {
                window.chrome.webview.postMessage('scroll:1');
                return;
            }
            var rng = topHeadingRange();
            if (rng !== null) {
                window.chrome.webview.postMessage('range:' + rng);
            } else {
                window.chrome.webview.postMessage('scroll:' + (y / max).toFixed(4));
            }
        }
    }
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', firstRender);
    } else {
        firstRender();
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
static ICoreWebView2WebMessageReceivedEventHandler *g_messageHandler = nullptr;
static EventRegistrationToken g_messageToken = {};
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
static int g_iAnchorIndex = -1;
static double g_dAnchorPos = 0.0;
static bool g_bPageReady = false;
static bool g_bAnchorValid = true;
static int g_iJsHeadingCount = -1;
// heading lines in document order (0-based Sci_Line), rebuilt on refresh
static std::vector<Sci_Line> g_headings;
// cumulative per-line content weight; lines that render tall in the preview
// (tables, code, lists, quotes) get a higher weight so interpolation between
// headings tracks the rendered height instead of raw line counts.
static std::vector<double> g_contentPrefix;

//=============================================================================
// forward declarations
//=============================================================================
static void EditPreview_ApplyLayout() noexcept;
static void EditPreview_Refresh() noexcept;
static void EditPreview_SaveSplitWidth() noexcept;
static void EditPreview_SyncToPreview() noexcept;
static void EditPreview_RequestRelayout() noexcept;
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
			EditPreview_RequestRelayout();
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
					EditPreview_LogScroll("recv", g_dScrollRatio);
					PostMessage(g_hwndMain, APPM_MDPREVIEW_SCROLL, 0, 0);
				} else if (WcsStartsWith(p, L"anchor:") && IsWindow(g_hwndMain)) {
					g_iAnchorIndex = _wtoi(p + CSTRLEN(L"anchor:"));
					g_dAnchorPos = 0.0;
					EditPreview_Log("[msg] anchor=%d", g_iAnchorIndex);
					PostMessage(g_hwndMain, APPM_MDPREVIEW_ANCHOR, 0, 0);
				} else if (WcsStartsWith(p, L"range:") && IsWindow(g_hwndMain)) {
					// format: range:<index>:<pos>
					const WCHAR *colon = StrChrW(p + CSTRLEN(L"range:"), L':');
					if (colon != nullptr) {
						g_iAnchorIndex = _wtoi(p + CSTRLEN(L"range:"));
						g_dAnchorPos = wcstod(colon + 1, nullptr);
						if (g_dAnchorPos < 0.0) {
							g_dAnchorPos = 0.0;
						} else if (g_dAnchorPos > 1.0) {
							g_dAnchorPos = 1.0;
						}
						EditPreview_Log("[msg] range=%d pos=%.3f", g_iAnchorIndex, g_dAnchorPos);
						PostMessage(g_hwndMain, APPM_MDPREVIEW_ANCHOR, 0, 0);
					}
				} else if (WcsStartsWith(p, L"headings:") && IsWindow(g_hwndMain)) {
					// the preview reports how many headings it tagged; if this
					// does not match our scan, fall back to proportional sync
					g_iJsHeadingCount = _wtoi(p + CSTRLEN(L"headings:"));
					g_bAnchorValid = (g_iJsHeadingCount == static_cast<int>(g_headings.size()));
					EditPreview_Log("[msg] headings js=%d cpp=%d anchor=%d", g_iJsHeadingCount,
						static_cast<int>(g_headings.size()), static_cast<int>(g_bAnchorValid));
				} else {
					EditPreview_Log("[msg] unknown webview message '%ls'", p);
				}
				CoTaskMemFree(json);
			}
		}
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
	EditPreview_Log("[init] hwnd=%p split=%d theme=%d", hwnd, g_iSplitWidth, g_iPreviewTheme);
	EditPreview_CreateSplitter();
}

//=============================================================================
//
// EditPreview_OnDestroy()
//
//=============================================================================
void EditPreview_OnDestroy() noexcept {
	EditPreview_Log("[destroy] releasing WebView2");
	if (g_uTimer != 0) {
		KillTimer(g_hwndMain, ID_MDPREVIEWTIMER);
		g_uTimer = 0;
	}
	if (g_hwndSplitter != nullptr) {
		DestroyWindow(g_hwndSplitter);
		g_hwndSplitter = nullptr;
	}
	if (g_webview != nullptr) {
		if (g_messageHandler != nullptr) {
			g_webview->remove_WebMessageReceived(g_messageToken);
		}
		g_webview->Release();
		g_webview = nullptr;
	}
	if (g_messageHandler != nullptr) {
		g_messageHandler->Release();
		g_messageHandler = nullptr;
	}
	if (g_controller != nullptr) {
		g_controller->Release();
		g_controller = nullptr;
	}
	if (g_controller2 != nullptr) {
		g_controller2->Release();
		g_controller2 = nullptr;
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
		EditPreview_Log("[env] created result=0x%08X hwnd=%p", result, g_hwndMain);
		// the window may be destroyed while WebView2 is still initializing
		if (!IsWindow(g_hwndMain)) {
			EditPreview_Log("[env] main window gone, aborting");
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
					EditPreview_Log("[ctrl] created result=0x%08X", result2);
					if (SUCCEEDED(result2) && controller != nullptr) {
						g_controller = controller;
						g_controller->AddRef();	// keep our own reference
						if (!IsWindow(g_hwndMain)) {
							EditPreview_Log("[ctrl] main window gone, aborting");
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
							g_messageHandler = new WebMessageHandler();
							const HRESULT hrMsg = g_webview->add_WebMessageReceived(g_messageHandler, &g_messageToken);
							EditPreview_Log("[ctrl] add_WebMessageReceived hr=0x%08X", hrMsg);
						}
						if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&g_controller2))) && g_controller2 != nullptr) {
							g_controller2->put_DefaultBackgroundColor(EditPreview_GetDefaultBackgroundColor());
						}
						g_controller->put_IsVisible(g_bVisible);
						if (g_bPendingLayout) {
							g_bPendingLayout = false;
							EditPreview_ApplyLayout();
							// re-layout the whole window now that the pane is ready
							EditPreview_RequestRelayout();
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
	EditPreview_Log("[webview] initializing WebView2...");
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
// EditPreview_RequestRelayout()
//
//   Post a WM_SIZE with the real client size so MsgSize recomputes the
//   layout. The stored g_lastCx/g_lastCy are already reduced by the toolbar
//   and statusbar heights, so reusing them would shrink the pane each time.
//
//=============================================================================
static void EditPreview_RequestRelayout() noexcept {
	RECT rc;
	GetClientRect(g_hwndMain, &rc);
	PostMessage(g_hwndMain, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
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
	g_bPageReady = false;
	BSTR bstr = SysAllocString(html);
	if (bstr != nullptr) {
		g_webview->NavigateToString(bstr);
		SysFreeString(bstr);
	}
}

//=============================================================================
//
// EditPreview_ExecuteScript()
//
//   Run a UTF-8 script string in the preview page.
//
//=============================================================================
static void EditPreview_ExecuteScript(const std::string &script) noexcept {
	if (g_webview == nullptr) {
		return;
	}
	const int wlen = MultiByteToWideChar(CP_UTF8, 0, script.data(), static_cast<int>(script.size()), nullptr, 0);
	if (wlen <= 0) {
		return;
	}
	std::wstring ws;
	ws.resize(static_cast<size_t>(wlen));
	MultiByteToWideChar(CP_UTF8, 0, script.data(), static_cast<int>(script.size()), &ws[0], wlen);
	g_webview->ExecuteScript(ws.c_str(), nullptr);
}

//=============================================================================
//
// EditPreview_ScanHeadings()
//
//   Collect the line numbers of Markdown headings (ATX and setext) in
//   document order, matching the headings rendered by marked so that the
//   preview can be anchored to the same index.
//
//=============================================================================
static void EditPreview_ScanHeadings() noexcept {
	g_headings.clear();
	g_contentPrefix.clear();
	const Sci_Position len = SciCall_GetLength();
	if (len <= 0) {
		return;
	}
	// fetch the whole document once and scan in memory instead of one
	// SendMessage per line. Only ATX headings are collected, matching the
	// headings the preview tags, so the anchor indices line up.
	char *text = static_cast<char *>(NP2HeapAlloc(static_cast<size_t>(len) + 1));
	if (text == nullptr) {
		return;
	}
	SciCall_GetText(len + 1, text);

	const char *end = text + len;
	const char *p = text;
	Sci_Line line = 0;
	double acc = 0.0;
	while (p < end) {
		const char *lineStart = p;
		while (p < end && *p != '\n') {
			++p;
		}
		const char *lineEnd = p;

		// estimate how tall this line renders in the preview
		double weight = 1.0;
		{
			bool empty = true;
			for (const char *c = lineStart; c < lineEnd; ++c) {
				if (*c != ' ' && *c != '\t' && *c != '\r') {
					empty = false;
					break;
				}
			}
			if (empty) {
				weight = 0.3;
			} else if (*lineStart == '|') {
				weight = 2.5;	// table row
			} else if (*lineStart == ' ' || *lineStart == '\t') {
				weight = 2.5;	// indented code / block
			} else if (*lineStart == '>') {
				weight = 1.3;	// blockquote
			} else if (*lineStart == '-' || *lineStart == '*' || (*lineStart >= '0' && *lineStart <= '9')) {
				weight = 1.6;	// list item
			} else if (*lineStart == '#') {
				weight = 2.0;	// heading
			}
		}
		acc += weight;
		g_contentPrefix.push_back(acc);

		// ATX heading: ^#{1,6}\s+<non-empty content>
		const char *q = lineStart;
		int level = 0;
		while (level < 6 && q < lineEnd && *q == '#') {
			++level;
			++q;
		}
		if (level >= 1 && q < lineEnd && (*q == ' ' || *q == '\t')) {
			const char *t = q;
			while (t < lineEnd && (*t == ' ' || *t == '\t')) {
				++t;
			}
			if (t < lineEnd) {
				g_headings.push_back(line);
			}
		}

		if (p < end) {
			++p;	// skip the '\n'
		}
		++line;
	}
	NP2HeapFree(text);
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
	EditPreview_ScanHeadings();
	if (len <= 0 || len > MD_PREVIEW_MAX_SIZE) {
		EditPreview_Log("[refresh] doc too large: %lld", static_cast<long long>(len));
		EditPreview_ShowPage(kTooLarge);
		return;
	}

	char *pText = static_cast<char *>(NP2HeapAlloc(static_cast<size_t>(len) + 1));
	if (pText == nullptr) {
		return;
	}
	SciCall_GetText(len + 1, pText);

	if (g_bPageReady) {
		// incremental update: re-render the content in place, no full page
		// navigation, so marked/mermaid are not reloaded on every keystroke.
		std::string js;
		js.reserve(static_cast<size_t>(len) * 2 + 64);
		js += "window.renderMarkdown(";
		JsonEscapeAppend(pText, static_cast<size_t>(len), js);
		js += ");";
		NP2HeapFree(pText);
		EditPreview_Log("[refresh] incremental len=%lld", static_cast<long long>(len));
		EditPreview_ExecuteScript(js);
		return;
	}

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
				EditPreview_Log("[refresh] NavigateToString len=%lld html=%d", static_cast<long long>(len), wlen);
				g_webview->NavigateToString(bstr);
				SysFreeString(bstr);
			}
			NP2HeapFree(pwsz);
		}
	}
	g_bPageReady = true;
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
	EditPreview_Log("[toggle] visible=%d", static_cast<int>(g_bVisible));

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
	EditPreview_RequestRelayout();
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
			// the theme is applied via <body data-theme>, which the incremental
			// update cannot change, so force a full page reload
			g_bPageReady = false;
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

	// top of the document -> preview top
	if (first <= 0) {
		g_webview->ExecuteScript(L"window.scrollTo(0,0);", nullptr);
		EditPreview_LogScroll("top->", 0);
		return;
	}
	// bottom of the document -> preview bottom (aligned end)
	const Sci_Line page = static_cast<Sci_Line>(SciCall(SCI_LINESONSCREEN, 0, 0));
	if (page > 0 && first + page >= total) {
		g_webview->ExecuteScript(L"window.scrollTo(0,document.documentElement.scrollHeight);", nullptr);
		EditPreview_LogScroll("bottom->", 1);
		return;
	}

	// anchor mode: scroll the preview to the nearest heading at/above the top
	// line, interpolated between that heading and the next one so long bodies
	// between headings stay roughly aligned
	if (g_bAnchorValid && !g_headings.empty()) {
		size_t lo = 0, hi = g_headings.size();
		while (lo < hi) {
			const size_t mid = (lo + hi) / 2;
			if (g_headings[mid] <= first) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		if (lo > 0) {
			--lo;	// lo = index of the nearest heading at/above first
			double pos = 0.0;
			if (lo + 1 < g_headings.size()) {
				const Sci_Line lineL = g_headings[lo];
				const Sci_Line lineN = g_headings[lo + 1];
				const double wL = g_contentPrefix[lineL];
				const double wN = g_contentPrefix[lineN];
				const double wSpan = wN - wL;
				if (wSpan > 0.0) {
					const double wX = g_contentPrefix[first];
					pos = (wX - wL) / wSpan;
					if (pos < 0.0) {
						pos = 0.0;
					} else if (pos > 1.0) {
						pos = 1.0;
					}
				}
			}
			WCHAR js[96];
			swprintf(js, COUNTOF(js), L"window.previewAnchor(%d,%f);", static_cast<int>(lo), pos);
			EditPreview_LogScroll("anchor->", static_cast<double>(lo) + pos);
			g_webview->ExecuteScript(js, nullptr);
			return;
		}
	}

	// fallback: proportional scroll
	double ratio = static_cast<double>(first) / total;
	if (ratio < 0.0) {
		ratio = 0.0;
	} else if (ratio > 1.0) {
		ratio = 1.0;
	}
	WCHAR js[128];
	swprintf(js, COUNTOF(js), L"window.scrollTo(0,%f*(document.documentElement.scrollHeight-window.innerHeight));", ratio);
	EditPreview_LogScroll("send", ratio);
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
	EditPreview_LogScroll("edit->line", static_cast<double>(line));
}

//=============================================================================
//
// EditPreview_SyncEditAnchor()
//
//   Called when the preview pane lands on a heading (APPM_MDPREVIEW_ANCHOR).
//   Scrolls the editor to that heading line.
//
//=============================================================================
void EditPreview_SyncEditAnchor() noexcept {
	if (!g_bVisible || g_bSyncingScroll) {
		return;
	}
	if (!g_bAnchorValid) {
		// heading anchors are unreliable, keep proportional sync only
		return;
	}
	if (g_iAnchorIndex < 0 || g_iAnchorIndex >= static_cast<int>(g_headings.size())) {
		return;
	}
	Sci_Line line = g_headings[g_iAnchorIndex];
	if (g_iAnchorIndex + 1 < static_cast<int>(g_headings.size()) && g_iAnchorIndex < static_cast<int>(g_contentPrefix.size())) {
		const Sci_Line lineN = g_headings[g_iAnchorIndex + 1];
		const double wL = g_contentPrefix[line];
		const double wN = g_contentPrefix[lineN];
		const double wSpan = wN - wL;
		if (wSpan > 0.0) {
			const double target = wL + g_dAnchorPos * wSpan;
			// inverse map: find the line whose content weight is closest to target
			Sci_Line lo2 = line, hi2 = lineN;
			while (lo2 + 1 < hi2) {
				const Sci_Line mid = (lo2 + hi2) / 2;
				if (g_contentPrefix[mid] <= target) {
					lo2 = mid;
				} else {
					hi2 = mid;
				}
			}
			line = lo2;
		}
	}
	if (line < 0) {
		line = 0;
	}
	g_bSyncingScroll = true;
	SciCall_SetFirstVisibleLine(line);
	g_bSyncingScroll = false;
	EditPreview_Log("[anchor] edit->line %lld", static_cast<long long>(line));
}

#endif // NP2_SUPPORT_MD_PREVIEW
