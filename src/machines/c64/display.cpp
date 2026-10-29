#include "board.h"
#include "pico/stdlib.h"
#include "Display.h"
#include "psram_allocator.h"
#include <cstring>
extern "C" void c64_keyboard_poll(uint8_t *,uint8_t *,uint8_t *);
// VIC renders indexed pixels into PSRAM. The Presto front end converts complete frames.
Display::Display(C64 *c):the_c64(c) {
    vic_pixels=(uint8_t *)psram_malloc(DISPLAY_X*DISPLAY_Y);
    if(!vic_pixels)panic("C64 framebuffer allocation");
    memset(vic_pixels,0,DISPLAY_X*DISPLAY_Y);
}
Display::~Display(){psram_free(vic_pixels);}
uint8_t *Display::BitmapBase(){return vic_pixels;}
int Display::BitmapXMod(){return DISPLAY_X;}
void Display::PollKeyboard(uint8_t *k,uint8_t *r,uint8_t *j){c64_keyboard_poll(k,r,j);}
bool Display::NumLock(){return false;}
void Display::Pause(){}
void Display::Resume(){}
void Display::NewPrefs(const Prefs *){}
void Display::Update(){}
void Display::SetLEDs(int,int,int,int){}
void Display::SetSpeedometer(int){}
void Display::ShowNotification(std::string){}
