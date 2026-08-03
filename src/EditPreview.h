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

void EditPreview_Init(HWND hwnd) noexcept;
int EditPreview_OnSize(int y, int cx, int cy) noexcept;
void EditPreview_Toggle() noexcept;
void EditPreview_OnDocumentChanged() noexcept;
void EditPreview_OnFileOpened() noexcept;
void EditPreview_OnTimer() noexcept;
void EditPreview_OnThemeChanged() noexcept;
void EditPreview_OnDestroy() noexcept;
bool EditPreview_IsVisible() noexcept;
bool EditPreview_IsMarkdown() noexcept;

#endif // NP2_SUPPORT_MD_PREVIEW

#endif // RC_INVOKED
