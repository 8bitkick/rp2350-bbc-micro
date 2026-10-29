#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#define __aligned(x) __attribute__((aligned(x)))
#define __not_in_flash_func(x) x
typedef unsigned uint;
typedef uint64_t absolute_time_t;
static inline absolute_time_t get_absolute_time(){return 0;}
static inline uint32_t to_ms_since_boot(absolute_time_t t){return t/1000;}
static inline uint64_t to_us_since_boot(absolute_time_t t){return t;}
static inline void sleep_ms(uint32_t n){}
static inline uint32_t time_us_32(){return 0;}
static inline void panic(const char *s){abort();}
