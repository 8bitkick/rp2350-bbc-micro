#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *psram_malloc(size_t);
void *psram_realloc(void *, size_t);
void psram_free(void *);
void c64_memory_init(void);
#ifdef __cplusplus
}
#endif
