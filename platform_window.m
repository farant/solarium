/* platform_window.m — macOS window helpers above the RHI seam (Cocoa). The
   GLFW "fullscreen" mode (passing a monitor) is a borderless window covering
   the screen; it does NOT become a native fullscreen Space. To get the real
   macOS behavior (its own Space, swipeable in Mission Control), the window must
   be an ordinary window put into fullscreen via Cocoa's toggleFullScreen:. */
#import <Cocoa/Cocoa.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include "platform_window.h"

void platform_window_fullscreen(void *glfw_window) {
    @autoreleasepool {
        GLFWwindow *w   = (GLFWwindow *)glfw_window;
        NSWindow   *nsw = w ? glfwGetCocoaWindow(w) : nil;
        if (!nsw) return;
        /* permit a native fullscreen Space, then enter it if not already in one */
        [nsw setCollectionBehavior:[nsw collectionBehavior] |
                                   NSWindowCollectionBehaviorFullScreenPrimary];
        if (!([nsw styleMask] & NSWindowStyleMaskFullScreen))
            [nsw toggleFullScreen:nil];
    }
}
