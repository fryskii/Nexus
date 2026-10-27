#pragma once
// UI Automation support for the custom-drawn main window. Exposes the navigation list (selectable
// items that switch pages) and a content element whose name summarises the current page and
// selection, and raises notifications for status messages so screen readers announce them.
#include <windows.h>

#include <string>

namespace nx::a11y {

void Attach(HWND hwnd);
void Detach(HWND hwnd);
LRESULT HandleGetObject(HWND hwnd, WPARAM wp, LPARAM lp);
void NotifyPageChanged(HWND hwnd, const std::wstring& title);
void Announce(HWND hwnd, const std::wstring& text);

} // namespace nx::a11y
