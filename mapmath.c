#include "mapmath.h"

static double clampd(double x, double lo, double hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

void mapmath_lonlat_to_uv(double lon, double lat, double *u, double *v) {
    lon = clampd(lon, -180.0, 180.0);
    lat = clampd(lat,  -90.0,  90.0);
    *u = (lon + 180.0) / 360.0;
    *v = (lat +  90.0) / 180.0;
}

void mapmath_uv_to_lonlat(double u, double v, double *lon, double *lat) {
    u = clampd(u, 0.0, 1.0);
    v = clampd(v, 0.0, 1.0);
    *lon = u * 360.0 - 180.0;
    *lat = v * 180.0 -  90.0;
}

void mapmath_window(double lon, double lat, int z, double aspect,
                    double *u0, double *v0, double *u1, double *v1) {
    double du, dv, cu, cv, hu, hv;
    if (z < 0) z = 0;
    if (z > 30) z = 30;   /* 1<<31 overflows int (UB); 2^30 is finer than any board needs */
    if (aspect <= 0.0) aspect = 2.0;
    du = 1.0 / (double)(1 << z);   /* fraction of image WIDTH shown */
    dv = 2.0 * du / aspect;        /* undistorted: image is 2:1 (W = 2H) */
    if (dv > 1.0) dv = 1.0;   /* only dv can exceed 1 (small aspect); du = 1/2^z <= 1 */
    mapmath_lonlat_to_uv(lon, lat, &cu, &cv);
    hu = du * 0.5;
    hv = dv * 0.5;
    cu = clampd(cu, hu, 1.0 - hu); /* shift center so the window fits inside */
    cv = clampd(cv, hv, 1.0 - hv);
    *u0 = cu - hu; *u1 = cu + hu;
    *v0 = cv - hv; *v1 = cv + hv;
}

int map_pin_local(double u0, double v0, double u1, double v1,
                  double w, double h, double plon, double plat,
                  double *lx, double *ly) {
    double pu, pv;
    int    in;
    mapmath_lonlat_to_uv(plon, plat, &pu, &pv);
    in = (pu >= u0 && pu <= u1 && pv >= v0 && pv <= v1);
    if (in) {
        *lx = -w * 0.5 + w * (pu - u0) / (u1 - u0);
        *ly = h * (pv - v0) / (v1 - v0);
    }
    return in ? 1 : 0;
}
