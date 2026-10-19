#pragma once

// macOS-only CEF/AppKit glue (implemented in CefMacSupport.mm). Plain C++
// declarations so .cpp translation units can call them without Objective-C.
namespace aurora::cef {

// Create the process-wide NSApplication as AuroraApplication (which
// implements CefAppProtocol) if no NSApp exists yet. Must run before
// CefInitialize and before QApplication is constructed.
void mac_prepare_application();

// True when NSApp exists and implements CefAppProtocol.
bool mac_application_is_cef_compatible();

// Fit CEF's browser NSView (CefBrowserHost::GetWindowHandle()) to its parent
// view's bounds and let AppKit keep it sized with the parent.
void mac_fit_child_view(void* child_view);

} // namespace aurora::cef
