#ifndef SOL_PLATFORM_CLIPBOARD_H
#define SOL_PLATFORM_CLIPBOARD_H
#include "sol_base.h"

/* Read the system clipboard's image as PNG bytes. On success returns SOL_TRUE,
   sets *out_bytes (malloc'd — caller frees with free()) and *out_len. Returns
   SOL_FALSE (and leaves the outputs untouched) when the clipboard holds no
   image (text / empty / unsupported). */
sol_bool clipboard_read_image(unsigned char **out_bytes, int *out_len);

#endif
