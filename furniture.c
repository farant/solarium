/* furniture.c — see furniture.h. */
#include "furniture.h"
#include <string.h>
#include <math.h>

static const char *const FURN_CATALOG[] = { "table", "bookshelf" };
#define FURN_COUNT ((int)(sizeof FURN_CATALOG / sizeof FURN_CATALOG[0]))

int furniture_catalog_count(void) { return FURN_COUNT; }

const char *furniture_catalog_name(int i) {
    if (i < 0 || i >= FURN_COUNT) return (const char *)0;
    return FURN_CATALOG[i];
}

int furniture_catalog_cycle(int i, int dir) {
    int n = FURN_COUNT;
    return ((i + dir) % n + n) % n;
}

sol_bool furniture_is_table(const char *mesh_ref) {
    return (sol_bool)(mesh_ref && strcmp(mesh_ref, "table") == 0);
}

sol_bool furniture_is_shelf(const char *mesh_ref) {
    return (sol_bool)(mesh_ref && strcmp(mesh_ref, "bookshelf") == 0);
}

/* furniture_shelf_slot / furniture_table_point / furniture_surface_aim land in
   Tasks 3-5; leave them out until then (their tests arrive with them). */
