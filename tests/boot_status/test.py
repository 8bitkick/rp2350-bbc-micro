#!/usr/bin/env python3
"""Run the real status renderer/toggle code with mocked board IO, under sanitizers."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    d=Path(tmp)
    source=(root/'src/common/boot_banner.c').read_text()
    # Host replacement for the one ARM-only pixel bit reversal instruction.
    source=source.replace('__asm("rbit %0, %1" : "=r"(pins) : "r"(rgb << 16));',
                          'pins = 0; for (unsigned bit=0; bit<16; bit++) pins |= ((rgb >> bit) & 1u) << (15-bit);')
    (d/'subject.c').write_text(source)
    for name in ['boot_button.h','boot_logo.h']:
        (d/name).write_text((root/'src/common'/name).read_text())
    for name in ['boot_banner.h','board.h','drive.h','machine_if.h','net_if.h','pico/stdlib.h','hardware/watchdog.h','hardware/sync.h','firmware_identity.h','sound.h','controller.h','presto_bluetooth.h']:
        p=d/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include "mock.h"\n')
    (d/'mock.h').write_text(r'''
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WITH_BOOT_FILE_SELECTOR 1
#define WITH_SOUND 1
#define BOARD_STATUS_BYTES (256u*1024u)
#define BOARD_BOOT_BUTTON_PIN 46
#define BOARD_DISPLAY_HEIGHT 480
#if MACHINE_C64 || MACHINE_SPECTRUM
#define BOARD_DISPLAY_ROW_WORDS 241
#else
#define BOARD_DISPLAY_ROW_WORDS 61
#endif
#define BOARD_DISPLAY_ROW_END 8u
#define DRIVE_IMAGE_MAX (1024u*1024u)
#define BOARD_HAS_NET 0
#define NIMBUS_TEST 1
#define BT_KEYBOARD_CLASSIC 1
#define RETRO_IMAGE_ID "trial"
#define GPIO_IN 0
static uint8_t ram[8u*1024u*1024u];
static const uint16_t *emulator[481];
static const uint16_t *const *display=emulator;
static uint32_t now_ms, frames=100;
static bool down,muted,render_held,joystick_online,joystick_fire,captured;
static unsigned joystick_directions,sd_count,sd_cursor;
static const char *sd_names[8];
static bool keyboard_online,keyboard_fire;
static unsigned keyboard_directions;
static void controller_keyboard_menu_state(unsigned *d,bool *f){*d=keyboard_directions;*f=keyboard_fire;}
static bool load_ok=true;
static char loaded[256];
static bool controller_menu_state(unsigned *d,bool *f){*d=joystick_online?joystick_directions:0;*f=joystick_online&&joystick_fire;return joystick_online;}
static void controller_menu_capture(bool c){captured=c;}
static const char *presto_bluetooth_boot_status(void){return "CONNECTED";}
static const char *presto_keyboard_boot_status(void){return keyboard_online?"CONNECTED":"NOT PAIRED";}
static void presto_bluetooth_boot_poll(void){}
static void *board_sd_discs_begin(void){sd_cursor=0;return sd_count?&sd_cursor:NULL;}
static bool board_sd_discs_next(void *it,char *name,uint32_t cap,uint32_t *size){
 (void)it;if(sd_cursor==sd_count)return false;
 snprintf(name,cap,"%s",sd_names[sd_cursor++]);*size=100;return true;
}
static void board_sd_discs_end(void *it){(void)it;}
static bool board_sd_load_disc(const char *n){snprintf(loaded,sizeof loaded,"%s",n);return load_ok;}
static uint32_t sram_pixels[272*241];
static void audio_status_mute(bool mute){muted=mute;}
static uint32_t *machine_status_framebuffer(bool active){render_held=active;return sram_pixels;}
static void gpio_init(unsigned p){(void)p;}
static void gpio_set_dir(unsigned p,unsigned v){(void)p;(void)v;}
static void gpio_pull_up(unsigned p){(void)p;}
static bool gpio_get(unsigned p){(void)p;return !down;}
static uint32_t get_absolute_time(void){return now_ms;}
static uint32_t to_ms_since_boot(uint32_t t){return t;}
static uint64_t time_us_64(void){return (uint64_t)now_ms*1000;}
static void sleep_ms(unsigned n){now_ms+=n;frames++;}
static uint32_t time_us_32(void){return now_ms*1000;}
static int board_display_scan_row(uint32_t now){(void)now;return 490;}
static void sleep_us(unsigned n){(void)n;frames++;}
static void watchdog_update(void){}
static bool watchdog_caused_reboot(void){return false;}
static void *drive0_staging(void){return ram;}
static uint8_t *board_psram(void){return ram;}
static size_t board_psram_size(void){return sizeof ram;}
static const uint16_t *const *board_display_get_rows(void){return display;}
static void board_display_set_rows(const uint16_t *const *p){display=p;}
static void board_display_backlight(unsigned b){(void)b;}
static uint32_t board_display_frames(void){return frames;}
static const char *machine_name(void){return "test";}
static void machine_perf_get(unsigned *a,unsigned *b,unsigned *c){*a=90;*b=30;*c=100;}
static void __dmb(void){}
''')
    (d/'test.c').write_text(r'''
#include <assert.h>
#include "subject.c"
static void step(bool pressed, uint32_t ms, uint32_t f){down=pressed;now_ms=ms;frames=f;boot_status_frame();}
int main(void){
 boot_button_t b={0};
 assert(!boot_button_press(&b,true,0)); // held on boot
 assert(!boot_button_press(&b,true,1000));
 assert(!boot_button_press(&b,false,1010));assert(!boot_button_press(&b,false,1050));
 assert(!boot_button_press(&b,true,1060));assert(!boot_button_press(&b,false,1070));
 assert(!boot_button_press(&b,true,1080));assert(boot_button_press(&b,true,1120));
 assert(!boot_button_press(&b,true,5000));
 b=(boot_button_t){0};assert(!boot_button_press(&b,false,UINT32_MAX-30));
 assert(!boot_button_press(&b,true,UINT32_MAX-20));assert(boot_button_press(&b,true,20));
 memset(ram,0xa5,sizeof ram);
 for(unsigned i=0;i<272*241;i++)sram_pixels[i]=(i%241==240)?0:0x12345678;
 step(false,0,100);step(true,100,105);step(true,140,107);
 assert(status_visible && display!=emulator && muted);
#if MACHINE_C64 || MACHINE_SPECTRUM
 assert(render_held);
 assert((const uint32_t *)display[0]>=sram_pixels);
#endif
 const uint16_t *const *first=display;
 for(unsigned i=0;i<sizeof ram-BOARD_STATUS_BYTES;i++)assert(ram[i]==0xa5);
 for(unsigned bank=status_active;bank<=status_active;bank++){
  const uint16_t **t=runtime_rows+bank*481;
  assert(t[480]==NULL);
  for(unsigned y=0;y<480;y++){
   assert(t[y]==t[y&~1u]);
   assert((const uint32_t *)t[y]>=status_sram+bank*STATUS_BANK_ROWS*BOARD_DISPLAY_ROW_WORDS);
   assert((const uint32_t *)t[y]<status_sram+(bank+1)*STATUS_BANK_ROWS*BOARD_DISPLAY_ROW_WORDS);
   const uint32_t *row=(const uint32_t *)t[y];
#if MACHINE_C64 || MACHINE_SPECTRUM
   assert(row[240]==0); // RGB565 padding, never packed-4 end marker
#else
   assert(row[60]==8); // packed-4 scanline sentinel
#endif
  }
 }
 // Solid red in the actual native encoding; boundary writes ignored.
 uint32_t scratch[BOARD_DISPLAY_ROW_WORDS*240]={0};
 pixel(scratch,0,0,4);
#if MACHINE_C64 || MACHINE_SPECTRUM
 assert(((uint16_t *)scratch)[0]==0x001f);
#else
 assert(scratch[0]==4);
#endif
 pixel(scratch,-1,0,7);pixel(scratch,480,0,7);pixel(scratch,0,240,7);
 step(true,1140,157);assert(display!=first); // refresh; held button doesn't toggle
 step(false,1160,158);step(false,1200,160);
 step(true,1220,161);step(true,1260,163);assert(!status_visible && display==emulator && !muted);
#if MACHINE_C64 || MACHINE_SPECTRUM
 assert(!render_held);
 for(unsigned i=0;i<272*241;i++)assert(sram_pixels[i]==((i%241==240)?0:0x12345678));
#endif
 step(false,1280,164);step(false,1320,166);
 step(true,1340,167);step(true,1380,169);assert(status_visible);

#if MACHINE_C64 || MACHINE_SPECTRUM
 assert(display!=emulator && status_visible); // SRAM scanout needs no flash suspension
#else
 assert(display!=emulator && status_visible);
#endif
 assert(display!=emulator && status_visible);
 status_close();assert(!captured);
 // Eligible titles open the chooser; names sort, held fire cannot auto-load.
 joystick_online=true;joystick_fire=true;sd_count=3;
 sd_names[0]="Zeta.tap";sd_names[1]="Alpha.tap";sd_names[2]="Beta.tap";
 titles_begin();assert(chooser);status_open();assert(captured);
 step(false,2000,200);assert(title_count==3 && !strcmp(titles()[0],"Alpha.tap"));
 assert(status_visible && !loaded[0]);
 joystick_fire=false;step(false,2100,205);
 joystick_directions=2;step(false,2200,210);assert(title_selected==1);
 step(false,2300,215);assert(title_selected==1);
 step(false,2600,230);assert(title_selected==2);
 joystick_directions=0;joystick_fire=true;step(false,2700,235);
 assert(!status_visible && !captured && !strcmp(loaded,"Zeta.tap"));
 // Three BOOT presses: chooser, status, emulator.
 joystick_fire=false;
 step(true,2800,240);step(true,2840,242);assert(chooser && status_visible);
 step(false,2900,245);step(false,2940,247);
 step(true,3000,250);step(true,3040,252);assert(!chooser && status_visible);
 step(false,3100,255);step(false,3140,257);
 step(true,3200,260);step(true,3240,262);assert(!status_visible);
 // Failed load keeps a readable status page and an exit path.
 titles_begin();status_open();load_ok=false;
 step(false,3300,265);step(false,3340,267);joystick_fire=true;
 step(false,3400,270);assert(status_visible && !chooser && load_error);
 status_close();sd_count=0;titles_begin();assert(!chooser);
 // A full chooser page must fit, and drawing the next screen leaves the live bank untouched.
 sd_count=8;for(unsigned i=0;i<8;i++)sd_names[i]="ABCDEFGHgjpqy.tap";
 titles_begin();status_open();titles_poll();
 status_active^=1;status_draw(status_active);status_present();
 assert(display==runtime_rows+status_active*481);
 uint32_t saved[STATUS_BANK_ROWS*BOARD_DISPLAY_ROW_WORDS];
 memcpy(saved,status_sram+status_active*STATUS_BANK_ROWS*BOARD_DISPLAY_ROW_WORDS,sizeof saved);
 unsigned live=status_active;status_active^=1;status_draw(status_active);status_present();
 assert(!memcmp(saved,status_sram+live*STATUS_BANK_ROWS*BOARD_DISPLAY_ROW_WORDS,sizeof saved));
 assert(display==runtime_rows+status_active*481);
 status_close();
#if !MACHINE_C64 && !MACHINE_SPECTRUM
 /* released after keyboard check */
#endif
 // Keyboard-only chooser: raw arrows and Return use the same navigation/load path.
 joystick_online=false;keyboard_online=true;keyboard_fire=false;
 titles_begin();assert(chooser);status_open();titles_poll();
 step(false,4000,400);keyboard_directions=2;step(false,4100,405);
 assert(title_selected==1);keyboard_directions=0;keyboard_fire=true;load_ok=true;
 step(false,4200,410);assert(!status_visible && !captured);
 free(status_sram);free(runtime_rows);puts("button, buffer bounds, row encoding and restore: PASS");
}
''')
    for machine in ['MASTER','BEEB']:
        subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-g',f'-DMACHINE_{machine}=1','-I',tmp,str(d/'test.c'),'-o',str(d/'test')],check=True)
        print(machine,flush=True);subprocess.run([str(d/'test')],check=True)
