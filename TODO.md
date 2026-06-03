First phase of TODO


1. Build harness + a cleared window. clang plus a build script (build.sh / .bat), no IDE. Open a window and a GL 3.3 core context — and for the first pixel, use GLFW as scaffolding so window creation isn't a second ocean to boil; the platform layer is an independent seam you'll own later. Stand up the step-based loop now, in its final shape: poll → update(state, dt) → render(state) → present, with update and render as functions, never logic melted into the loop. glClear to a color. Checkpoint: a colored window that closes cleanly — validates toolchain, context, and loop.

2. The triangle (the literal hello world). Hardcode three vertices in a VBO, write a trivial vertex+fragment shader as inline GLSL strings — don't build a shader pipeline yet; the cross-compile question isn't real until a second backend exists — compile and link the program, set up a VAO, issue one draw call. Ugly, hardcoded, one file. Checkpoint: a triangle. This exercises the whole GPU path: buffer, shader, draw.

3. Motion + a texture. Add a uniform and drive it from dt — a color-cycling or spinning triangle — proving the per-frame CPU→GPU data path. Then a textured quad: upload pixel data (hardcode the pixels first to skip a loader dependency), sample it in the fragment shader. Now you've touched the four core resource types: buffers, shaders, uniforms, textures.

4. 3D: a rotating cube with depth. The "3D on screen" milestone you named. Write the minimal math layer here — vec3/vec4/mat4 and the handful of functions (perspective, look-at, multiply); hand-roll it, it's a few hundred lines and you'll want to own it. Add model/view/projection matrices, an index buffer, enable depth testing, draw an indexed cube, rotate it. New concepts: indices, depth buffer, MVP, a camera.

5. A real mesh + one light. A trivial model loader — OBJ is the classic first format (text, easily parsed in C89), or your own dead-simple binary. Render a loaded mesh, then add one directional light in the fragment shader (Lambert or Blinn-Phong) so it reads as a solid object instead of a flat silhouette. Checkpoint: your engine renders a lit 3D model from a file — the recognizable "it's an engine now" moment.

6. Extract the RHI seam. Only now. Look at the GL calls strewn through steps 2–5 and lift the interface out of them: buffer/texture/shader/pipeline create, pass begin/end, bind, draw, frame begin/present. The GL code collapses into gl_backend.c implementing that interface; everything above it calls only the interface, never GL directly. This is the architectural turn — the modern-shaped RHI we discussed, now grounded in code you've already written rather than guessed at.
