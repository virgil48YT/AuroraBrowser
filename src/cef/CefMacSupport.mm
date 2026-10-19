// macOS-only glue between CEF, AppKit and Qt. Compiled only on Apple when
// AURORA_ENABLE_CEF=ON (see the top-level CMakeLists.txt).
//
// 1. CEF requires the process's NSApplication to implement CefAppProtocol
//    (isHandlingSendEvent / setHandlingSendEvent:) so Chromium's message pump
//    can tell nested event dispatch apart. Qt would otherwise create its own
//    QNSApplication, which does not. NSApplication is a singleton: the first
//    +sharedApplication call decides its class, and Qt's cocoa plugin reuses
//    an existing NSApp. So AuroraApplication is instantiated here, before
//    CefInitialize and before QApplication — the same order cefclient uses.
// 2. A windowed CEF browser on macOS is an NSView added to the parent view
//    Qt gives us (QWidget::winId() is an NSView*). Views are sized in points,
//    not device pixels, so the child is fitted to the parent's bounds instead
//    of using the caller's device-pixel size.
#include "cef/CefMacSupport.hpp"

#import <AppKit/AppKit.h>

#include "include/cef_application_mac.h"

@interface AuroraApplication : NSApplication <CefAppProtocol> {
@private
    BOOL handlingSendEvent_;
}
@end

@implementation AuroraApplication

- (BOOL)isHandlingSendEvent {
    return handlingSendEvent_;
}

- (void)setHandlingSendEvent:(BOOL)handlingSendEvent {
    handlingSendEvent_ = handlingSendEvent;
}

- (void)sendEvent:(NSEvent*)event {
    CefScopedSendingEvent sendingEventScoper;
    [super sendEvent:event];
}

@end

namespace aurora::cef {

void mac_prepare_application() {
    @autoreleasepool {
        if (NSApp == nil) {
            [AuroraApplication sharedApplication];
        }
    }
}

bool mac_application_is_cef_compatible() {
    return NSApp != nil && [NSApp conformsToProtocol:@protocol(CefAppProtocol)];
}

void mac_fit_child_view(void* child_view) {
    if (child_view == nullptr) return;
    NSView* view = (__bridge NSView*)child_view;
    NSView* parent = [view superview];
    if (parent == nil) return;
    [view setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    [view setFrame:[parent bounds]];
}

} // namespace aurora::cef
