#ifndef SOL_PLATFORM_WINDOW_H
#define SOL_PLATFORM_WINDOW_H

/* Put a GLFW window into the platform's NATIVE fullscreen — on macOS a real
   fullscreen Space (Mission Control / swipe between desktops), NOT GLFW's
   borderless cover-the-monitor mode. The window is passed as an opaque pointer
   so this header stays GLFW-free. No-op if the pointer is null or the window is
   already fullscreen. */
void platform_window_fullscreen(void *glfw_window);

#endif /* SOL_PLATFORM_WINDOW_H */
