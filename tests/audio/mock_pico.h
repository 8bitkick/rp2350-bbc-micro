#ifndef MOCK_PICO_H
#define MOCK_PICO_H
#include <stdint.h>
#include <stdbool.h>
typedef unsigned int uint;
#define __not_in_flash_func(x) x
#define GPIO_FUNC_PWM 4
#define GPIO_OUT 1
#define clk_sys 0
struct repeating_timer { int x; };
typedef struct {int x;} pwm_config;
static uint16_t mock_level;
static uint16_t mock_wrap;
static float mock_divider;
static uint32_t mock_slept_ms;
static inline void pwm_set_wrap(uint slice, uint16_t wrap){(void)slice;mock_wrap=wrap;}
static inline void sleep_ms(uint32_t ms){mock_slept_ms+=ms;}
static bool mock_timer_fail;
static inline void pwm_set_gpio_level(uint p, uint16_t v){(void)p;mock_level=v;}
static inline uint32_t clock_get_hz(int c){(void)c;return 250000000;}
static inline pwm_config pwm_get_default_config(void){return (pwm_config){0};}
static inline void pwm_config_set_clkdiv(pwm_config*c,float v){(void)c;mock_divider=v;}
static inline void pwm_config_set_wrap(pwm_config*c,uint16_t v){(void)c;mock_wrap=v;}
static inline uint pwm_gpio_to_slice_num(uint p){(void)p;return 9;}
static inline void pwm_init(uint s,const pwm_config*c,bool b){(void)s;(void)c;(void)b;}
static inline void gpio_set_function(uint p,int f){(void)p;(void)f;}
static inline void pwm_set_enabled(uint s,bool b){(void)s;(void)b;}
static inline bool add_repeating_timer_us(int64_t us,bool(*fn)(struct repeating_timer*),void*u,struct repeating_timer*t){(void)us;(void)fn;(void)u;(void)t;return !mock_timer_fail;}
static inline bool cancel_repeating_timer(struct repeating_timer*t){(void)t;return true;}
static inline void gpio_init(uint p){(void)p;}
static inline void gpio_set_dir(uint p,bool b){(void)p;(void)b;}
static inline void gpio_put(uint p,bool b){(void)p;(void)b;}
static inline uint32_t save_and_disable_interrupts(void){return 0;}
static inline void restore_interrupts(uint32_t x){(void)x;}
#endif
