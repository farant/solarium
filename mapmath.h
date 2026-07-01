#ifndef MAPMATH_H
#define MAPMATH_H
/* mapmath.h — equirectangular (plate carree) world-map math. Pure: no GL, no
   IO, no engine deps. Maps lon/lat <-> UV into a single equirect world image
   and computes the UV crop window a map board shows. See the world-map-boards
   design spec. */

/* lon in [-180,180], lat in [-90,90]; u,v in [0,1]. v=0 is the SOUTH edge
   (lat -90), v=1 the NORTH edge (+90): this matches image_load's vertical flip
   so north ends up at the top of the board. Inputs are clamped. */
void mapmath_lonlat_to_uv(double lon, double lat, double *u, double *v);
void mapmath_uv_to_lonlat(double u, double v, double *lon, double *lat);

/* The UV rectangle a board centered at (lon,lat) shows at integer zoom z
   (0 = whole world) for a board of the given aspect (= width/height). The shown
   width fraction is du = 1/2^z of the image; dv = 2*du/aspect keeps the geography
   undistorted (the image is 2:1, W=2H). The window is clamped into [0,1]; if it
   would cross an edge the center is shifted to keep it fully inside (no
   antimeridian/pole wrap in v1). Always yields u0<u1, v0<v1.
   Output pointers must be distinct (no aliasing). */
void mapmath_window(double lon, double lat, int z, double aspect,
                    double *u0, double *v0, double *u1, double *v1);

/* Project a pin at (plon,plat) onto a map whose crop window is (u0,v0)-(u1,v1)
   and whose quad is w wide, h tall (local x in [-w/2,w/2], y in [0,h], bottom-
   origin — matching make_map_quad). Writes the map-local (lx,ly) and returns 1
   if the pin is inside the window, else returns 0 and leaves *lx,*ly untouched. */
int map_pin_local(double u0, double v0, double u1, double v1,
                  double w, double h, double plon, double plat,
                  double *lx, double *ly);

#endif /* MAPMATH_H */
