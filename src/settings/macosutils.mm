#include "macosutils.h"
#include "effectspolicy.h"
#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>
#include <QWindow>
#include <Cocoa/Cocoa.h>
#include <objc/runtime.h>

static NSString *const ContainerIdentifier = @"WindowBaseVibrancyContainerView";
static NSString *const MaterialIdentifier = @"WindowBaseVibrancyView";
static char ObserverKey;

// Owned by the native window; QPointer prevents delivery into a destroyed controller.
@interface WindowBaseAccessibilityObserver : NSObject {
    QPointer<QObject> _receiver;
}
- (instancetype)initWithReceiver:(QObject *)receiver;
- (void)preferencesChanged:(NSNotification *)notification;
@end
@implementation WindowBaseAccessibilityObserver
- (instancetype)initWithReceiver:(QObject *)receiver {
    if ((self = [super init])) {
        _receiver = receiver;
        [NSWorkspace.sharedWorkspace.notificationCenter addObserver:self
            selector:@selector(preferencesChanged:)
            name:NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification object:nil];
    }
    return self;
}
- (void)preferencesChanged:(NSNotification *)notification {
    Q_UNUSED(notification);
    if (_receiver) QMetaObject::invokeMethod(_receiver, "refresh", Qt::QueuedConnection);
}
- (void)dealloc {
    [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:self];
    [super dealloc];
}
@end

static NSWindow *nativeWindow(QWindow *window) {
    // Offscreen tests have synthetic WIds, not NSViews.
    if (!window || QGuiApplication::platformName() != QStringLiteral("cocoa")) return nil;
    NSView *view = reinterpret_cast<NSView *>(window->winId());
    return view.window;
}

void performMacOSTitlebarDoubleClick(QWindow *window) {
    NSWindow *native = nativeWindow(window);
    if (!native) return;
    // Read the user's system preference on the gesture; never write system defaults.
    NSString *preference = [NSUserDefaults.standardUserDefaults stringForKey:@"AppleActionOnDoubleClick"];
    const char *value = preference.UTF8String;
    const bool fullscreen = (native.styleMask & NSWindowStyleMaskFullScreen)
        || window->visibility() == QWindow::FullScreen;
    const bool canMinimize = (native.styleMask & NSWindowStyleMaskMiniaturizable)
        && [native standardWindowButton:NSWindowMiniaturizeButton].enabled;
    const bool canZoom = (native.styleMask & NSWindowStyleMaskResizable)
        && [native standardWindowButton:NSWindowZoomButton].enabled;
    switch (WindowEffectsPolicy::titlebarDoubleClickAction(value ? value : "", fullscreen,
                native.attachedSheet != nil, canMinimize, canZoom)) {
    case WindowEffectsPolicy::TitlebarAction::Minimize: [native performMiniaturize:nil]; break;
    case WindowEffectsPolicy::TitlebarAction::Zoom: [native performZoom:nil]; break;
    case WindowEffectsPolicy::TitlebarAction::None: break;
    }
}

void configureMacOSNativeWindow(QWindow *window) {
    NSWindow *native = nativeWindow(window);
    if (!native) return;
    // Retain AppKit resize/fullscreen behavior while drawing all visible chrome ourselves.
    native.styleMask |= NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
        | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView;
    native.titlebarAppearsTransparent = YES;
    native.titleVisibility = NSWindowTitleHidden;
    native.backgroundColor = NSColor.clearColor;
    native.opaque = NO;
    native.hasShadow = YES;
    native.movableByWindowBackground = NO;
    if (@available(macOS 11.0, *)) native.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
    [[native standardWindowButton:NSWindowCloseButton] setHidden:YES];
    [[native standardWindowButton:NSWindowMiniaturizeButton] setHidden:YES];
    [[native standardWindowButton:NSWindowZoomButton] setHidden:YES];
}

bool setMacOSBackdrop(QWindow *window, bool enabled) {
    NSWindow *native = nativeWindow(window);
    if (!native) return false;
    NSView *container = native.contentView;
    if (![container.identifier isEqualToString:ContainerIdentifier]) {
        if (!enabled) return false;
        NSView *content = [container retain];
        container = [[NSView alloc] initWithFrame:content.frame];
        container.identifier = ContainerIdentifier;
        container.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [native setContentView:container];
        content.frame = container.bounds;
        content.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [container addSubview:content];
        [content release];
        [container release];
    }
    NSVisualEffectView *material = nil;
    for (NSView *child in container.subviews) {
        if ([child.identifier isEqualToString:MaterialIdentifier]) {
            material = (NSVisualEffectView *)child;
            break;
        }
    }
    if (!enabled) {
        [material removeFromSuperview];
        return false;
    }
    if (!material) {
        material = [[NSVisualEffectView alloc] initWithFrame:container.bounds];
        material.identifier = MaterialIdentifier;
        material.material = NSVisualEffectMaterialWindowBackground;
        material.state = NSVisualEffectStateFollowsWindowActiveState;
        material.blendingMode = NSVisualEffectBlendingModeBehindWindow;
        material.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [container addSubview:material positioned:NSWindowBelow relativeTo:nil];
        [material release];
    }
    return true;
}

void setMacOSCornerRadius(QWindow *window, int radius) {
    NSWindow *native = nativeWindow(window);
    if (!native) return;
    native.contentView.wantsLayer = YES;
    native.contentView.layer.cornerRadius = radius;
    native.contentView.layer.masksToBounds = radius > 0;
}

MacOSAccessibilityPreferences macOSAccessibilityPreferences() {
    return {bool(NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceTransparency),
            bool(NSWorkspace.sharedWorkspace.accessibilityDisplayShouldIncreaseContrast)};
}

void observeMacOSAccessibility(QWindow *window, QObject *receiver) {
    NSWindow *native = nativeWindow(window);
    if (!native) return;
    auto *observer = [[WindowBaseAccessibilityObserver alloc] initWithReceiver:receiver];
    objc_setAssociatedObject(native, &ObserverKey, observer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [observer release];
}
