/* BearSSL needs a few <string.h> functions; the SG/ELKA build has no libc (__NO_LIBC): declared here
 * (forced include), defined by the SDK crt (memcpy) or src/libc.c. */
#include <stddef.h>
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
