/*
 * The test harness built natively for the host (glibc), for profiling the
 * portable code with the host's tools (e.g. gprofng). See harness_host.h.
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#define _GNU_SOURCE
#include "harness_host.h"

#include <poll.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

static uint8_t s_big_region[8u << 20] __attribute__((aligned(16)));

uint64_t
harness_host_time_us(void) {
  struct timespec ts;
  (void) clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((uint64_t) ts.tv_sec * 1000000u) + (ts.tv_nsec / 1000);
}

void
harness_host_sleep_us(uint64_t us) {
  struct timespec ts;
  ts.tv_sec = (us / 1000000u);
  ts.tv_nsec = ((us % 1000000u) * 1000u);
  (void) nanosleep(&ts, NULL);
}

int
harness_host_read_stdin(uint8_t* p_byte) {
  struct pollfd pfd;
  pfd.fd = 0;
  pfd.events = POLLIN;
  pfd.revents = 0;
  if ((poll(&pfd, 1, 0) <= 0) || !(pfd.revents & POLLIN)) {
    return 0;
  }
  return (read(0, p_byte, 1) == 1);
}

void
harness_host_write_stdout(uint8_t byte) {
  (void) write(1, &byte, 1);
}

size_t
harness_host_heap_used(void) {
  return 0;
}

size_t
harness_host_big_region(uint8_t** p_base) {
  *p_base = s_big_region;
  return sizeof(s_big_region);
}

void*
harness_host_fixed_region(void* p_addr, size_t size) {
  (void) p_addr;
  (void) size;
  return NULL;
}

void
harness_host_flush_code(void* p, size_t length) {
  (void) p;
  (void) length;
}

int
harness_host_needs_code_flush(void) {
  /* No JIT on the native build. */
  return 0;
}

uint64_t
harness_host_cycles(void) {
  return 0;
}

const char*
harness_host_cycles_unit(void) {
  return NULL;
}

void
harness_host_profile(int on) {
  (void) on;
}

void
harness_host_profile_save(const char* p_file_name) {
  (void) p_file_name;
}
