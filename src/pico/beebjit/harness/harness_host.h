/*
 * What the test harness (harness_main.c) needs from where it runs: as a
 * 32-bit Linux process on an Arm64 host (a32/), bare metal on QEMU's
 * mps2-an505 Cortex-M33 board (qemu/), or natively on the host (native_host.c).
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#ifndef BEEBJIT_HARNESS_HOST_H
#define BEEBJIT_HARNESS_HOST_H

#include <stddef.h>
#include <stdint.h>

/* Real (wall) time; may be 0 where there is none (QEMU). */
uint64_t harness_host_time_us(void);
void harness_host_sleep_us(uint64_t us);

/* Non-blocking: 0 if there is nothing to read. */
int harness_host_read_stdin(uint8_t* p_byte);
void harness_host_write_stdout(uint8_t byte);

/* Bytes taken by malloc's heap so far (0 if unknown). */
size_t harness_host_heap_used(void);

/* Stand-in for the Presto's PSRAM. */
size_t harness_host_big_region(uint8_t** p_base);
/* Executable memory at a fixed address (the JIT), or NULL; and making
 * instruction fetch see what was written there.
 */
void* harness_host_fixed_region(void* p_addr, size_t size);
void harness_host_flush_code(void* p, size_t length);
/* Whether instruction fetch needs harness_host_flush_code() to see writes. */
int harness_host_needs_code_flush(void);

/* A count of executed cycles or instructions for profiling, with its unit
 * name; returns 0 / NULL where there's none.
 */
uint64_t harness_host_cycles(void);
const char* harness_host_cycles_unit(void);

/* PC sampling, where the host can (QEMU): on/off, then save the samples
 * (raw little-endian uint32 PCs) to a file.
 */
void harness_host_profile(int on);
void harness_host_profile_save(const char* p_file_name);

#endif
