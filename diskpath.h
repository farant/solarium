#ifndef DISKPATH_H
#define DISKPATH_H
/* diskpath.h — pure path string math for the Files browser provider. No GL,
   no scene, no POSIX; depends only on sol_base + libc string. The risky bit of
   the filesystem browser (parent/join/basename edge cases) lives here so it can
   be headless-tested (the caret.c / route.c "split pure logic" law). All output
   writers always NUL-terminate and clamp to cap. Paths are expected absolute.
   The `out` buffer must not alias `dir`/`name`/`path`. */
#include "sol_base.h"

/* Parent directory of `path` into out[cap]. "/a/b/c"->"/a/b"; "/a"->"/";
   "/a/b/"->"/a"; "/"->"/"; ""->"". Trailing slashes ignored. */
void        diskpath_parent(const char *path, char *out, int cap);

/* Join dir + name with exactly one '/'. "/a"+"b"->"/a/b"; "/"+"b"->"/b";
   "/a/"+"b"->"/a/b". Always NUL-terminates; clamps to cap. */
void        diskpath_join(const char *dir, const char *name, char *out, int cap);

/* Pointer to the last path segment (basename), borrowed from `path`.
   "/a/b"->"b"; "foo"->"foo"; "/"->"". */
const char *diskpath_basename(const char *path);

/* SOL_TRUE iff path is exactly the filesystem root "/". */
sol_bool    diskpath_is_root(const char *path);

#endif /* DISKPATH_H */
