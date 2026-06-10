/* platform_fs.h — the filesystem seam (P3 item 6, §1.9). The fourth
   quarantine in the house pattern: GL behind rhi_gl.c, stb behind image.c /
   font.c, and now POSIX (opendir/readdir/stat) behind platform_fs.c —
   excluded from c89check, clean C89 interface here, so a future platform
   port touches one file. Everything above this header is portable. */
#ifndef PLATFORM_FS_H
#define PLATFORM_FS_H

#include "sol_base.h"

typedef struct {
    char    *name;      /* entry name only, not the full path; owned by the listing */
    sol_bool is_dir;
    long     size;      /* bytes; 0 for directories */
} FsEntry;

typedef struct {
    FsEntry *entries;
    int      count;
} FsListing;

/* Scan a directory into a listing: hidden entries (leading '.') skipped,
   sorted by name so scan order is DETERMINISTIC (reconciliation and tray
   layouts must not depend on readdir's whims). SOL_FALSE if the directory
   cannot be opened. Caller frees with fs_listing_free. */
sol_bool fs_scan_dir(const char *path, FsListing *out);
void     fs_listing_free(FsListing *l);

/* Does the path exist (file or directory)? Stale-alias detection. */
sol_bool fs_exists(const char *path);

/* Read a whole file (binary), heap-allocated and NUL-terminated, capped at
   `cap` bytes — a capped read is an HONEST partial: *out_truncated says so
   (the reader appends its own marker). NULL on failure. *out_len excludes
   the NUL. Either out pointer may be NULL. Caller frees. */
char *fs_read_file(const char *path, long cap, long *out_len, int *out_truncated);

#endif /* PLATFORM_FS_H */
