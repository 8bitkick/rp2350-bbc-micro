#!/usr/bin/env python3
"""Run the real banner against a simulated panel/network; optionally export a PPM."""
import pathlib, subprocess, tempfile, sys
root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
 p=pathlib.Path(tmp)
 for name in ['boot_banner.c','boot_banner.h','boot_logo.h']:
  (p/name).write_bytes((root/'src/common'/name).read_bytes())
 for folder in ['pico','hardware']: (p/folder).mkdir()
 common='''#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define BOARD_HAS_NET 1
#define BOARD_DISPLAY_WIDTH 480
#define BOARD_DISPLAY_HEIGHT 480
#define BOARD_DISPLAY_ROW_WORDS 61
#define BOARD_DISPLAY_ROW_END 8
#define DRIVE_IMAGE_MAX 409600
uint8_t *drive0_staging(void);
const uint16_t *const *board_display_get_rows(void);
void board_display_set_rows(const uint16_t *const *);
void board_display_backlight(uint8_t);
uint32_t board_display_frames(void);
uint64_t time_us_64(void);
void sleep_ms(unsigned);
void watchdog_update(void);
bool watchdog_caused_reboot(void);
void net_boot_poll(void);
bool net_boot_configured(void);
bool net_boot_address_ready(void);
void net_boot_status(char *, size_t);
'''
 for name in ['board.h','drive.h','machine_if.h','net_if.h','pico/stdlib.h','hardware/watchdog.h','hardware/sync.h']:(p/name).write_text(common)
 (p/'test.c').write_text('''#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "boot_banner.h"
static uint32_t fb[DRIVE_IMAGE_MAX/4];
static const uint16_t *original[481], *const *current=original;
static uint64_t now;
static bool reset, configured, address;
static unsigned swaps,polls;
static uint32_t image[240*61];
uint8_t *drive0_staging(void){return (uint8_t *)fb;}
const uint16_t *const *board_display_get_rows(void){return current;}
void board_display_set_rows(const uint16_t *const *r){
 current=r; swaps++;
 if(r==original)return;
 assert(!r[480]);
 for(int y=0;y<480;y++)assert(((uint32_t *)r[y])[60]==8);
 memcpy(image,r[0],sizeof image);
}
void board_display_backlight(uint8_t v){(void)v;}
uint32_t board_display_frames(void){return now/20000;}
uint64_t time_us_64(void){return now;}
void sleep_ms(unsigned ms){now+=ms*1000;}
void watchdog_update(void){}
bool watchdog_caused_reboot(void){return reset;}
void net_boot_poll(void){polls++;}
bool net_boot_configured(void){return configured;}
bool net_boot_address_ready(void){return address && now>=1000000;}
void net_boot_status(char *o,size_t n){snprintf(o,n,"%s",net_boot_address_ready()?"192.168.1.42":"JOINING");}
void presto_bluetooth_boot_poll(void){}
const char *presto_bluetooth_boot_status(void){return "CONNECTED";}
const char *presto_keyboard_boot_status(void){return "CONNECTED";}
int main(int argc,char **argv){
 reset=true;boot_banner_show();assert(!swaps && !polls);
 reset=false;configured=false;boot_banner_show();assert(current==original && now>=3000000 && now<4000000);
 now=0;configured=true;address=false;boot_banner_show();assert(current==original && now>=30000000 && now<31000000);
 now=0;address=true;boot_banner_show();assert(current==original && now>=3000000 && now<4000000);
 if(argc>1){FILE *f=fopen(argv[1],"wb");assert(f);fprintf(f,"P6\\n480 480\\n255\\n");for(int y=0;y<480;y++)for(int x=0;x<480;x++){unsigned v=(image[y/2*61+x/8]>>(x%8*4))&7;unsigned char rgb[3]={v&4?255:0,v&2?255:0,v&1?255:0};fwrite(rgb,1,3,f);}fclose(f);}
 puts("PASS: watchdog skip, offline timeout, DHCP, row sentinels and display restoration");
}
''')
 for bt in [0,1]:
  subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',f'-DNIMBUS_TEST={bt}','-DBT_KEYBOARD_CLASSIC=1','-DBOOT_BUILD_ID="TEST-BUILD"','-I'+tmp,str(p/'test.c'),str(p/'boot_banner.c'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')]+(sys.argv[1:] if bt else []),check=True)
