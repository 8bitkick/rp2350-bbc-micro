#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "controller.h"
#include "machine_if.h"
static bool keys[256],fire[2];static unsigned dirs[2];static uint16_t xs[2],ys[2];
void machine_key_raw(int code,bool down){assert(code>=0&&code<256);keys[code]=down;}
void machine_controller(unsigned port,uint16_t x,uint16_t y,unsigned d,bool f){assert(port==1||port==2);xs[port-1]=x;ys[port-1]=y;dirs[port-1]=d;fire[port-1]=f;}
void machine_key_sdl(uint32_t code,bool down){machine_key(code==225||code==229?0:(int)code,down);}
static int16_t axes[4];static int32_t buttons[8];
static void feed(bool connected,unsigned pad){controller_input(connected,axes,pad,buttons,false);controller_frame();}
int main(void){
 uint8_t usages[32]={0};usages[4/8]|=1u<<(4%8);controller_bluetooth_keyboard(usages);assert(keys[4]);
 machine_key(4,true);memset(usages,0,32);controller_bluetooth_keyboard(usages);assert(keys[4]);machine_key(4,false);assert(!keys[4]);
 usages[225/8]|=1u<<(225%8);usages[229/8]|=1u<<(229%8);controller_bluetooth_keyboard(usages);assert(keys[0]);
 usages[225/8]&=~(1u<<(225%8));controller_bluetooth_keyboard(usages);assert(keys[0]);
 memset(usages,0,32);controller_bluetooth_keyboard(usages);assert(!keys[0]);

 assert(controller_normalize(-127,-127,127)==-32767);
 assert(controller_normalize(127,-127,127)==32767);
 assert(controller_normalize(0,-128,127)==0);
 assert(controller_normalize(128,0,255)==0);
 assert(controller_normalize(0,0,255)==-32767);
 assert(controller_normalize(255,0,255)==32767);
 assert(controller_normalize(99,1,1)==0);
 buttons[0]=1;axes[0]=32767;feed(true,0);assert(!fire[1]&&!dirs[1]); // disabled
 assert(controller_config("1 2 0 15 0 0 -1 -1 -1 -1 -2 -1 -1 -1 -1 -1 -1 -1 -1")==200);
 feed(true,0);assert(fire[1]&&dirs[1]==8&&xs[1]==1);assert(!fire[0]);
 axes[0]=1000;axes[1]=32767;feed(true,0);assert(dirs[1]==1&&xs[1]==32768&&ys[1]==65535);
 feed(false,0);assert(!fire[1]&&!dirs[1]&&xs[1]==32768&&ys[1]==32768);
 assert(controller_config("1 1 2 15 0 0 -1 -1 -1 -1 -2 -1 -1 -1 -1 -1 -1 -1 -1")==200);
 feed(true,4);assert(dirs[0]==4&&fire[0]&&!fire[1]);
 feed(true,15);assert(dirs[0]==0&&xs[0]==32768&&ys[0]==32768);
 assert(controller_config("1 1 0 15 1 1 -1 -1 -1 -1 -2 -1 -1 -1 -1 -1 -1 -1 -1")==200);
 axes[0]=32767;axes[1]=32767;feed(true,0);assert(dirs[0]==6);
 // Two controller buttons share key 42; releasing one must not release the other.
 assert(controller_config("2 1 2 15 0 0 10 11 12 13 42 42 -1 -1 -1 -1 -1 -1 -1")==200);
 buttons[0]=buttons[1]=1;feed(true,1);assert(keys[42]&&keys[10]&&!fire[0]);
 buttons[0]=0;feed(true,0);assert(keys[42]&&!keys[10]);
 machine_key(42,true);feed(false,0);assert(keys[42]); // physical/web keyboard still holds it
 machine_key(42,false);assert(!keys[42]);
 char before[160],after[160];controller_json(before,sizeof before);
 assert(controller_config("1 2")==400);
 assert(controller_config("1 2 0 99 0 0 -1 -1 -1 -1 -2 -1 -1 -1 -1 -1 -1 -1 -1")==400);
 controller_json(after,sizeof after);assert(!strcmp(before,after));
 feed(true,0);assert(keys[42]);controller_reset();assert(!keys[42]);controller_frame();assert(keys[42]);
 assert(controller_config("0 1 2 15 0 0 10 11 12 13 42 42 -1 -1 -1 -1 -1 -1 -1")==200);
 controller_frame();assert(!keys[42]);
 // Menu sees navigation/fire, but the emulator receives neutral input.
 memset(buttons,0,sizeof buttons);memset(axes,0,sizeof axes);
 assert(controller_config("1 1 2 15 0 0 -1 -1 -1 -1 -2 -1 -1 -1 -1 -1 -1 -1 -1")==200);
 buttons[0]=1;feed(true,2);assert(fire[0]&&dirs[0]==2);
 controller_menu_capture(true);feed(true,2);assert(!fire[0]&&!dirs[0]);
 unsigned md;bool mf;assert(controller_menu_state(&md,&mf)&&md==2&&mf);
 controller_menu_capture(false);feed(true,2);assert(!fire[0]&&!dirs[0]);
 buttons[0]=0;feed(true,0);buttons[0]=1;feed(true,1);assert(fire[0]&&dirs[0]==1);
 feed(false,0);assert(!controller_menu_state(&md,&mf));
 // A selects even when assigned to a keyboard key rather than emulated Fire.
 assert(controller_config("2 1 2 15 0 0 10 11 12 13 42 -1 -1 -1 -1 -1 -1 -1 -1")==200);
 controller_menu_capture(true);buttons[0]=1;feed(true,0);
 assert(controller_menu_state(&md,&mf)&&mf&&!keys[42]);
 buttons[0]=0;feed(true,0);assert(controller_menu_state(&md,&mf)&&!mf);
 controller_menu_capture(false);
 controller_reset();
 uint8_t remap[64]={4,42,5,42};memset(usages,0,sizeof usages);controller_keyboard_map_set(remap);
 usages[0]=(1<<4)|(1<<5);controller_bluetooth_keyboard(usages);assert(keys[42]);
 usages[0]=1<<5;controller_bluetooth_keyboard(usages);assert(keys[42]);
 machine_key(42,true);usages[0]=0;controller_bluetooth_keyboard(usages);assert(keys[42]);
 machine_key(42,false);assert(!keys[42]);
 memset(remap,0,sizeof remap);controller_keyboard_map_set(remap);
 puts("Normalization, ports, dead zone, inversion, keyboard ownership, reset, disconnect and validation passed");
}
