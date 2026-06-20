/* editor.h — the top-down spatial-tree editor (item 3 of the fs-tree arc).
   A headless interaction core: pure footprint geometry + resize math + connect
   validation (unit-tested), the per-frame interaction state machine driven by
   ortho-camera rays, and the Editor state struct that lives in AppState. No
   GLFW, no GL, no AppState — main.c owns the glue (cursor, overlay, rebuild). */
#ifndef SOL_EDITOR_H
#define SOL_EDITOR_H

#include "sol_base.h"
#include "sol_types.h"
#include "scene.h"
#include "camera.h"

#define EDITOR_GRAB_BAND 0.6f   /* world-unit band around a wall = resize handle */
#define EDITOR_MIN_SIZE  3.0f   /* a room footprint side can't shrink below this */
#define EDITOR_PORT_LIFT 2.6f   /* connection node hovers this far above the floor */
#define EDITOR_PORT_NDC  0.05f  /* port grab radius, in NDC half-units */

/* A room footprint on its floor plane, world space. cx/cz = center,
   hw/hd = half width (X) / half depth (Z), floor_y = world Y of the floor. */
typedef struct { float cx, cz, hw, hd, floor_y; } RoomRect;

/* Which part of a footprint a ground point falls on (axis-named: P=+, N=-). */
typedef enum {
    EDIT_ZONE_NONE = 0,
    EDIT_ZONE_BODY,
    EDIT_ZONE_EDGE_XP, EDIT_ZONE_EDGE_XN,
    EDIT_ZONE_EDGE_ZP, EDIT_ZONE_EDGE_ZN,
    EDIT_ZONE_CORNER_XPZP, EDIT_ZONE_CORNER_XPZN,
    EDIT_ZONE_CORNER_XNZP, EDIT_ZONE_CORNER_XNZN
} EditZone;

/* What the editor is doing between press and release. */
typedef enum { EDIT_IDLE = 0, EDIT_MOVE, EDIT_RESIZE, EDIT_CONNECT } EditAction;

/* The editor's whole state (lives in AppState). Zero = inactive, idle. */
typedef struct {
    sol_bool   active;
    sol_bool   was_active;     /* main.c edge-detects enter/exit on this */
    Camera     saved_cam;      /* first-person camera, restored on exit */
    EditAction action;
    sol_u32    room;           /* room parent handle under manipulation */
    EditZone   zone;           /* RESIZE: which handle */
    sol_u32    selected_wk;    /* walkway selected for delete; 0 = none */
    sol_u32    connect_from;   /* CONNECT: source room */
    vec3       grab_off;       /* MOVE: room center - grab ground point (XZ) */
    vec3       cursor_world;   /* latest ground point (rubber-band end) */
    sol_bool   dirty;          /* geometry changed -> re-thread this frame */
    sol_bool   commit;         /* interaction ended -> save */
} Editor;

/* ---- pure geometry (headless, unit-tested) ---- */
RoomRect editor_room_rect(Scene *s, sol_u32 room);
EditZone editor_classify(RoomRect r, float gx, float gz, float band);
void     editor_resize_axis(float center, float half, int sign,
                            float face_world, float min_size,
                            float *new_center, float *new_half);
sol_bool editor_can_connect(Scene *s, sol_u32 a, sol_u32 b);

/* ---- scene ops (headless, unit-tested) ---- */
sol_u32  editor_connect(Scene *s, sol_u32 a, sol_u32 b);   /* new walkway, 0 if invalid */
void     editor_disconnect(Scene *s, sol_u32 walkway);
void     editor_apply_move(Scene *s, sol_u32 room, float cx, float cz);
void     editor_apply_resize(Scene *s, sol_u32 room, EditZone zone,
                             float gx, float gz);

/* ---- cursor-driven interaction (built in Task 4; verified live) ---- */
void     editor_press(Editor *e, Scene *s, const Camera *c,
                      float ndc_x, float ndc_y, float aspect);
void     editor_drag(Editor *e, Scene *s, const Camera *c,
                     float ndc_x, float ndc_y, float aspect);
void     editor_release(Editor *e, Scene *s, const Camera *c,
                        float ndc_x, float ndc_y, float aspect);
void     editor_delete_selected(Editor *e, Scene *s);

#endif /* SOL_EDITOR_H */
