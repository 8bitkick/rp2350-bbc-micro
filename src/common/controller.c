/* Main-core controller mapping. Keyboard sources are combined before delivery. */
#include "controller.h"
#include "machine_if.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static bool online;
static int16_t axes[4];
static unsigned pad;
static bool button[9];
/* mode 0 off, 1 joystick, 2 keyboard; source 0 left, 1 right, 2 D-pad */
static int cfg[19]={0,2,0,15,0,0,-1,-1,-1,-1,-2,-1,-1,-1,-1,-1,-1,-1,-1};
static bool keyboard[256],controller_keys[256],bluetooth_keys[256];
static bool *keyboard_capture;
static uint8_t bluetooth_map[64];
void controller_keyboard_map_set(const uint8_t map[64]){memcpy(bluetooth_map,map,64);}
void controller_keyboard_map_get(uint8_t map[64]){memcpy(map,bluetooth_map,64);}
static unsigned joystick_port;
static unsigned menu_directions;
static bool menu_fire,menu_capture,menu_release;
bool controller_menu_state(unsigned *directions,bool *fire){
 *directions=menu_directions;*fire=menu_fire;return online && cfg[0]!=0;
}
void controller_menu_capture(bool capture){menu_capture=capture;menu_release=true;}
void machine_key_raw(int code,bool down);
void machine_key(int code,bool down) {
    if(code<0||code>255)return;
    if(keyboard_capture){keyboard_capture[code]|=down;return;}
    bool was=keyboard[code]||controller_keys[code]||bluetooth_keys[code];keyboard[code]=down;
    bool now=keyboard[code]||controller_keys[code]||bluetooth_keys[code];
    if(was!=now)machine_key_raw(code,now);
}
void controller_reset(void) {
    for(int i=0;i<256;i++)if(keyboard[i]||controller_keys[i]||bluetooth_keys[i])machine_key_raw(i,false);
    memset(keyboard,0,sizeof keyboard);memset(controller_keys,0,sizeof controller_keys);memset(bluetooth_keys,0,sizeof bluetooth_keys);
}
int16_t controller_normalize(int32_t v,int32_t lo,int32_t hi) {
    if(hi<=lo)return 0;
    if(v<lo)v=lo;
        if(v>hi)v=hi;
    /* Signed Nimbus axes centre exactly on zero; unsigned HID axes use midpoint. */
    int64_t centre=lo<0 && hi>0?0:((int64_t)lo+hi+1)/2;
    int64_t span=v>=centre?(int64_t)hi-centre:centre-lo;
    return span?(int16_t)(((int64_t)v-centre)*32767/span):0;
}
void controller_input(bool connected,const int16_t a[4],unsigned directions,const int32_t b[8],bool menu) {
    online=connected;memcpy(axes,a,sizeof axes);pad=directions;
    for(int i=0;i<8;i++)button[i]=b[i]!=0;
    button[8]=menu;
}
static int deadzone(int value) {
    int threshold=cfg[3]*32767/100, magnitude=abs(value);
    if(magnitude<=threshold)return 0;
    int scaled=(magnitude-threshold)*32767/(32767-threshold);
    return value<0?-scaled:scaled;
}
void controller_frame(void) {
    bool desired[256]={0};bool enabled=online&&cfg[0];
    unsigned directions=0;int x=0,y=0;bool fire=false;
    if(enabled) {
        if(cfg[2]<2) {
            x=deadzone(axes[cfg[2]*2]);
            y=deadzone(-axes[cfg[2]*2+1]); /* Original Nimbus Y grows upwards. */
        }
        if(cfg[4])x=-x;
        if(cfg[5])y=-y;
        directions=pad;
        if(x < -8192)directions|=4;
        if(x>8192)directions|=8;
        if(y < -8192)directions|=1;
        if(y>8192)directions|=2;
        if((directions&3)==3)directions&=~3u;
        if((directions&12)==12)directions&=~12u;
        if(pad&4)x=-32767;
        if(pad&8)x=32767;
        if(pad&1)y=-32767;
        if(pad&2)y=32767;
        if((pad&12)==12)x=0;
        if((pad&3)==3)y=0;
        for(int i=0;i<13;i++) {
            bool pressed=i<4 ? cfg[0]==2 && (directions&(1u<<i)) : button[i-4];
            if(!pressed)continue;
            int key=cfg[6+i];
            if(key==-2)fire=true;else if(key>=0)desired[key]=true;
        }
    }
    menu_directions=directions;menu_fire=enabled && (button[0] || fire);
    bool any_button=false;for(int i=0;i<9;i++)any_button|=button[i];
    if(!directions && !any_button)menu_release=false;
    if(menu_capture || menu_release){memset(desired,0,sizeof desired);enabled=false;}
    for(int k=0;k<256;k++)if(desired[k]!=controller_keys[k]) {
        bool before=keyboard[k]||controller_keys[k]||bluetooth_keys[k];controller_keys[k]=desired[k];
        bool after=keyboard[k]||controller_keys[k]||bluetooth_keys[k];
        if(before!=after)machine_key_raw(k,after);
    }
    unsigned next_port=enabled&&cfg[0]==1?(unsigned)cfg[1]:0;
    if(joystick_port && joystick_port!=next_port)
        machine_controller(joystick_port,32768,32768,0,false);
    if(next_port)
        machine_controller(next_port,(uint16_t)(32768-x),(uint16_t)(32768-y),directions,fire);
    joystick_port=next_port;
}
int controller_config(const char *s) {
    int next[19];
    for(int i=0;i<19;i++) {
        char *end;long v=strtol(s,&end,10);
        if(end==s||v < -2||v>127)return 400;
        next[i]=(int)v;s=end;
    }
    while(*s==' ')s++;
        if(*s)return 400;
    if(next[0]<0||next[0]>2||next[1]<1||next[1]>2||next[2]<0||next[2]>2||next[3]<0||next[3]>50||next[4]<0||next[4]>1||next[5]<0||next[5]>1)return 400;
    memcpy(cfg,next,sizeof cfg);return 200;
}
void controller_json(char *out,size_t size) {
    size_t n=0;
    for(int i=0;i<19;i++) {
        if(n>=size)return;
        int wrote=snprintf(out+n,size-n,"%s%d",i?",":"",cfg[i]);
        if(wrote<0)return;
        n+=(size_t)wrote;
    }
}

void controller_get_config(int32_t out[19]) {for(int i=0;i<19;i++)out[i]=cfg[i];}

/* Called between frames. Aggregate aliases (e.g. both Shift keys) before release. */
void controller_bluetooth_keyboard(const uint8_t usages[32]){
 bool desired[256]={0};keyboard_capture=desired;
 for(unsigned i=4;i<256;i++)if(usages[i/8]&(1u<<(i%8))){
  bool mapped=false;
  for(unsigned j=0;j<64;j+=2)if(bluetooth_map[j]==i){machine_key(bluetooth_map[j+1],true);mapped=true;break;}
  if(!mapped)machine_key_sdl(i,true);
 }
 keyboard_capture=NULL;
 for(unsigned i=0;i<256;i++)if(desired[i]!=bluetooth_keys[i]){
  bool before=keyboard[i]||controller_keys[i]||bluetooth_keys[i];bluetooth_keys[i]=desired[i];
  bool after=keyboard[i]||controller_keys[i]||bluetooth_keys[i];if(before!=after)machine_key_raw(i,after);
 }
}
