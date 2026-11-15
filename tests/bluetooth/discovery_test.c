#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "btstack.h"
#include "keyboard_discovery.h"
uint32_t test_time;
static unsigned stops;
int gap_inquiry_start(uint8_t length){assert(length==8);return 0;}
int gap_inquiry_stop(void){stops++;return 0;}
static char json[8192];
int main(void){
 assert(keyboard_discovery_start()==200);assert(keyboard_discovery_start()==409);
 for(unsigned len=2;len<27;len++){uint8_t p[27]={GAP_EVENT_INQUIRY_RESULT};p[1]=len-2;assert(keyboard_discovery_event(p,len));}
 for(unsigned i=0;i<40;i++){uint8_t p[27]={GAP_EVENT_INQUIRY_RESULT,25};p[2]=i;keyboard_discovery_event(p,sizeof p);}
 uint8_t key[45]={GAP_EVENT_INQUIRY_RESULT,43,99,2,3,4,5,6};key[9]=0x40;key[10]=5;key[25]=1;key[26]=18;memcpy(key+27,"Bluetooth Keyboard",18);
 assert(keyboard_discovery_event(key,sizeof key));
 key[25]=0;assert(keyboard_discovery_event(key,sizeof key));
 keyboard_discovery_json(json,sizeof json);assert(strstr(json,"Bluetooth Keyboard"));assert(strstr(json,"\"hid_hint\":true"));assert(strstr(json,"\"truncated\":true"));assert(!strstr(json,"BLE"));
 uint8_t ble[]={GAP_EVENT_ADVERTISING_REPORT,0};assert(!keyboard_discovery_event(ble,sizeof ble));
 uint8_t done[]={GAP_EVENT_INQUIRY_COMPLETE,1,0};assert(keyboard_discovery_event(done,sizeof done));assert(!keyboard_discovery_busy());
 assert(keyboard_discovery_start()==200);test_time=15001;keyboard_discovery_frame();assert(!keyboard_discovery_busy()&&stops==1);
 assert(keyboard_discovery_start()==200);keyboard_discovery_stop();assert(stops==2);
 puts("Classic discovery: bounds, crowded list, name retention, BLE ignored, completion and timeout passed");
}
