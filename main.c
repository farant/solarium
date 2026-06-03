#define GL_SILENCE_DEPRECATION   /* macOS GL is deprecated-but-alive; quiet the warnings */

#include <stdio.h>
#include <stdlib.h>

#define GLFW_INCLUDE_NONE        /* don't let GLFW drag in the legacy <OpenGL/gl.h> */
#include <GLFW/glfw3.h>
#include <OpenGL/gl3.h>          /* the 3.3 core API, straight from the system framework */

/* The whole world, frame to frame. Nearly empty now — that's the point. */
typedef struct {
    int fb_width;
    int fb_height;
} AppState;

static void update(AppState *state, double dt) {
    (void)state;
    (void)dt;
    /* nothing to simulate yet — the seam exists, empty, on purpose */
}

static void render(const AppState *state) {
    glViewport(0, 0, state->fb_width, state->fb_height);
    glClearColor(0.10f, 0.12f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void on_key(GLFWwindow *window, int key, int scancode, int action, int mods) {
    (void)scancode;
    (void)mods;
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
}

int main(void) {
    if (!glfwInit()) {
        fprintf(stderr, "glfwInit failed\n");
        return EXIT_FAILURE;
    }

    /* The four hints — set BEFORE the window exists, or macOS hands you 2.1 */
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    GLFWwindow *window = glfwCreateWindow(960, 540, "solarium", NULL, NULL);
    if (!window) {
        fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwMakeContextCurrent(window);   /* this context is now "the current one" */
    glfwSwapInterval(1);              /* present in step with vsync */
    glfwSetKeyCallback(window, on_key);

    /* Proof the context is real — read this before trusting it in step 2 */
    printf("GL_VERSION : %s\n", glGetString(GL_VERSION));
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    /* Definitive profile check — query the context, don't parse a string */
    GLint profile = 0, flags = 0;
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
    printf("CORE PROFILE  : %s\n", (profile & GL_CONTEXT_CORE_PROFILE_BIT) ? "yes" : "no");
    printf("FWD COMPATIBLE: %s\n", (flags & GL_CONTEXT_FLAG_FORWARD_COMPATIBLE_BIT) ? "yes" : "no");

    AppState state = {0};
    double last = glfwGetTime();

    while (!glfwWindowShouldClose(window)) {   /* the loop, in its final shape */
        glfwPollEvents();                      /* poll    */

        double now = glfwGetTime();
        double dt  = now - last;
        last = now;

        glfwGetFramebufferSize(window, &state.fb_width, &state.fb_height);

        update(&state, dt);                    /* update(state, dt) */
        render(&state);                        /* render(state)     */

        glfwSwapBuffers(window);               /* present */
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
