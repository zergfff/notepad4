/******************************************************************************
*
* Notepad4
*
* EditPreview.h
*   Markdown live preview pane (WebView2)
*
******************************************************************************/
#pragma once
#ifndef RC_INVOKED
#include <windows.h>
#include "config.h"

//! Markdown live preview is available when enabled at build time.
#if defined(NP2_ENABLE_MD_PREVIEW) && NP2_ENABLE_MD_PREVIEW != 0
#define NP2_SUPPORT_MD_PREVIEW 1
#else
#define NP2_SUPPORT_MD_PREVIEW 0
#endif

#if NP2_SUPPORT_MD_PREVIEW

//! timer used to debounce the live preview refresh
#define ID_MDPREVIEWTIMER			0xA003
//! posted to the main window when the preview pane scrolls
#define APPM_MDPREVIEW_SCROLL		(WM_APP + 8)
//! posted to the main window when the preview pane lands on a heading
#define APPM_MDPREVIEW_ANCHOR		(WM_APP + 9)

//! preview color theme
enum {
	MDPreviewTheme_Auto = 0,
	MDPreviewTheme_Light,
	MDPreviewTheme_Dark,
};

void EditPreview_Init(HWND hwnd) noexcept;
int EditPreview_OnSize(int y, int cx, int cy) noexcept;
void EditPreview_Toggle() noexcept;
void EditPreview_OnDocumentChanged() noexcept;
void EditPreview_OnFileOpened() noexcept;
void EditPreview_OnTimer() noexcept;
void EditPreview_OnEditScroll() noexcept;
void EditPreview_SyncEditScroll() noexcept;
void EditPreview_SyncEditAnchor() noexcept;
void EditPreview_OnThemeChanged() noexcept;
void EditPreview_OnDestroy() noexcept;
bool EditPreview_IsVisible() noexcept;
bool EditPreview_IsMarkdown() noexcept;
int EditPreview_GetTheme() noexcept;
void EditPreview_SetTheme(int theme) noexcept;
#endif // NP2_SUPPORT_MD_PREVIEW

#endif // RC_INVOKED
