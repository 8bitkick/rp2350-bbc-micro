/*
 * The few things beebjit's embedded platform layer (os_embedded.c) needs from
 * the machine it runs on. Implemented once for the Presto (presto/) and once
 * for the AArch32 Linux test harness (a32/).
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#ifndef BEEBJIT_HOST_H
#define BEEBJIT_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic time. */
uint64_t beebjit_host_time_us(void);
void beebjit_host_sleep_us(uint64_t us);

/* A byte stream for beebjit's "terminal" (the BBC's RS423 port). Return 0 if
 * there's nothing to read.
 */
int beebjit_host_term_read(uint8_t* p_byte);
void beebjit_host_term_write(uint8_t byte);

/* A region for memory that won't fit in SRAM (the Presto's PSRAM): large,
 * rarely touched allocations such as disc images go there. Returns its size,
 * or 0 if there is none (everything then comes from malloc).
 */
size_t beebjit_host_big_region(uint8_t** p_base);

/* Memory at a fixed address, for the JIT backend's code region and stub
 * pages (which it needs at K_JIT_ADDR; there's no MMU to put them there).
 * Returns p_addr if the host has size bytes there for beebjit, else NULL.
 */
void* beebjit_host_fixed_region(void* p_addr, size_t size);

#ifdef __cplusplus
}
#endif

#endif
