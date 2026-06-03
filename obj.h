#ifndef OBJ_H
#define OBJ_H

/* Minimal OBJ loader. Returns a malloc'd array of interleaved 8-float vertices
   [px,py,pz, nx,ny,nz, u,v] (uv is 0 — OBJ here carries no texcoords), with the
   vertex count via out_count. NULL on failure. Caller frees. Pure CPU — no GL. */
float *obj_load(const char *path, int *out_count);

#endif /* OBJ_H */
