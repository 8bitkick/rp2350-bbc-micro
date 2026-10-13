/*
 * Extras of the embedded os_* layer (os_embedded.c) beyond beebjit's os_*.h.
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#ifndef BEEBJIT_OS_EMBEDDED_H
#define BEEBJIT_OS_EMBEDDED_H

#include <stddef.h>
#include <stdint.h>

/* beebjit's util_malloc() and friends, via BEEBJIT_MALLOC etc. (util.c). */
void* beebjit_embedded_malloc(size_t size);
void beebjit_embedded_alloc_caller(void* p_caller);
void* beebjit_embedded_realloc(void* p, size_t size);
void beebjit_embedded_free(void* p);
/* Prefer the big region whatever the size: for things rarely touched. */
void* beebjit_embedded_malloc_big(size_t size);

extern int beebjit_embedded_log_allocs;
void beebjit_embedded_get_alloc_stats(size_t* p_big_used,
                                      size_t* p_big_peak,
                                      size_t* p_small_total);

/* Call p_handler as soon as a message is written to the channel that
 * read_handle reads.
 */
void os_channel_embedded_set_handler(intptr_t read_handle,
                                     void (*p_handler)(void* p),
                                     void* p_handler_object);

#endif
