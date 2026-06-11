/* platform_fs.c — the quarantined POSIX translation unit (P3 item 6, §1.9).
   opendir/readdir/stat are not C89 and not portable; they live here, behind
   the clean platform_fs.h, exactly as GL lives behind rhi_gl.c. EXCLUDED
   from build.sh c89check. */

#include "platform_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const FsEntry *)a)->name, ((const FsEntry *)b)->name);
}

sol_bool fs_scan_dir(const char *path, FsListing *out) {
    DIR           *d;
    struct dirent *de;
    FsEntry       *entries = NULL;
    int            count = 0, cap = 0;
    char           full[1024];

    out->entries = NULL;
    out->count   = 0;

    d = opendir(path);
    if (!d) return SOL_FALSE;

    while ((de = readdir(d)) != NULL) {
        struct stat st;
        if (de->d_name[0] == '.') continue;          /* hidden + "." + ".." */
        if (count == cap) {
            FsEntry *grown;
            cap   = cap ? cap * 2 : 32;
            grown = (FsEntry *)realloc(entries, (size_t)cap * sizeof(FsEntry));
            if (!grown) { fs_listing_free(out); out->entries = entries; out->count = count;
                          fs_listing_free(out); closedir(d); return SOL_FALSE; }
            entries = grown;
        }
        snprintf(full, sizeof full, "%s/%s", path, de->d_name);
        if (stat(full, &st) != 0) continue;          /* vanished mid-scan: skip */
        entries[count].name   = strdup(de->d_name);
        entries[count].is_dir = S_ISDIR(st.st_mode) ? SOL_TRUE : SOL_FALSE;
        entries[count].size   = S_ISDIR(st.st_mode) ? 0 : (long)st.st_size;
        if (entries[count].name) count++;
    }
    closedir(d);

    qsort(entries, (size_t)count, sizeof(FsEntry), entry_cmp);
    out->entries = entries;
    out->count   = count;
    return SOL_TRUE;
}

void fs_listing_free(FsListing *l) {
    int i;
    if (!l) return;
    for (i = 0; i < l->count; i++) free(l->entries[i].name);
    free(l->entries);
    l->entries = NULL;
    l->count   = 0;
}

sol_bool fs_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? SOL_TRUE : SOL_FALSE;
}

char *fs_read_file(const char *path, long cap, long *out_len, int *out_truncated) {
    FILE *f;
    long  len, take;
    char *buf;
    if (out_len)       *out_len = 0;
    if (out_truncated) *out_truncated = 0;
    if (!path || cap <= 0) return NULL;
    f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0L, SEEK_END) != 0) { fclose(f); return NULL; }
    len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    rewind(f);
    take = (len > cap) ? cap : len;
    buf = (char *)malloc((size_t)take + 1);
    if (!buf) { fclose(f); return NULL; }
    if (take > 0 && fread(buf, 1, (size_t)take, f) != (size_t)take) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[take] = '\0';
    if (out_len)       *out_len = take;
    if (out_truncated) *out_truncated = (len > cap);
    return buf;
}

long fs_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (long)st.st_mtime;
}
