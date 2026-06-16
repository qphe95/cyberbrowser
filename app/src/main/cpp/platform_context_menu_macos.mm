/* macOS native context menu for URL text box */

#include <Cocoa/Cocoa.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

/* Forward declarations for C++ handlers in main_macos.cpp / vulkan_ui.cpp */
extern "C" {
    void handle_cut(void);
    void handle_copy(void);
    void handle_paste(const char *text);
    void handle_select_all(void);
    const char *platform_get_clipboard(void);
}

@interface BGMDWLDRMenuTarget : NSObject
@end

@implementation BGMDWLDRMenuTarget

- (void)cutAction:(id)sender {
    (void)sender;
    handle_cut();
}

- (void)copyAction:(id)sender {
    (void)sender;
    handle_copy();
}

- (void)pasteAction:(id)sender {
    (void)sender;
    const char *clip = platform_get_clipboard();
    if (clip) {
        handle_paste(clip);
    }
}

- (void)selectAllAction:(id)sender {
    (void)sender;
    handle_select_all();
}

@end

extern "C" void platform_show_text_context_menu(GLFWwindow *window, double x, double y) {
    NSWindow *nsWindow = glfwGetCocoaWindow(window);
    if (!nsWindow) return;
    NSView *view = [nsWindow contentView];
    if (!view) return;

    NSRect bounds = [view bounds];
    NSPoint loc;
    loc.x = (CGFloat)x;
    loc.y = bounds.size.height - (CGFloat)y;

    BGMDWLDRMenuTarget *target = [[BGMDWLDRMenuTarget alloc] init];

    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];

    NSMenuItem *cutItem = [[NSMenuItem alloc] initWithTitle:@"Cut"
                                                      action:@selector(cutAction:)
                                               keyEquivalent:@""];
    [cutItem setTarget:target];
    [menu addItem:cutItem];

    NSMenuItem *copyItem = [[NSMenuItem alloc] initWithTitle:@"Copy"
                                                       action:@selector(copyAction:)
                                                keyEquivalent:@""];
    [copyItem setTarget:target];
    [menu addItem:copyItem];

    NSMenuItem *pasteItem = [[NSMenuItem alloc] initWithTitle:@"Paste"
                                                        action:@selector(pasteAction:)
                                                 keyEquivalent:@""];
    [pasteItem setTarget:target];
    [menu addItem:pasteItem];

    [menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *selectAllItem = [[NSMenuItem alloc] initWithTitle:@"Select All"
                                                            action:@selector(selectAllAction:)
                                                     keyEquivalent:@""];
    [selectAllItem setTarget:target];
    [menu addItem:selectAllItem];

    /* Show menu modally; blocks until dismissed */
    [menu popUpMenuPositioningItem:nil
                        atLocation:loc
                            inView:view];
}
