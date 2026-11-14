#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "btstack.h"
#include "controller_store.h"
uint32_t test_time;
static uint8_t delivered[32];static unsigned disconnects;
static const uint8_t desc[]={0x05,0x01,0x09,0x06,0xa1,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,0x95,1,0x75,8,0x81,1,0x95,6,0x75,8,0x15,0,0x25,0x65,0x05,7,0x19,0,0x29,0x65,0x81,0,0xc0};
const uint8_t *hid_descriptor_storage_get_descriptor_data(uint16_t c){(void)c;return desc;}
uint16_t hid_descriptor_storage_get_descriptor_len(uint16_t c){(void)c;return sizeof desc;}
uint8_t hid_host_connect(bd_addr_t a,hid_protocol_mode_t m,uint16_t *c){(void)a;(void)m;*c=7;return 0;}
uint8_t hid_host_accept_connection(uint16_t c,hid_protocol_mode_t m){(void)c;(void)m;return 0;}
uint8_t hid_host_decline_connection(uint16_t c){(void)c;return 0;}
void hid_host_disconnect(uint16_t c){assert(c==7);disconnects++;}
void gap_ssp_set_io_capability(int c){(void)c;}
int gap_pin_code_response(const bd_addr_t a,const char *p){(void)a;assert(!strcmp(p,"123456"));return 0;}
int gap_pin_code_negative(bd_addr_t a){(void)a;return 0;}
int gap_ssp_confirmation_response(const bd_addr_t a){(void)a;return 0;}
int gap_ssp_confirmation_negative(const bd_addr_t a){(void)a;return 0;}
void controller_bluetooth_keyboard(const uint8_t k[32]){memcpy(delivered,k,32);}
static bool bonded;static uint8_t ba[6],bk[16],bt;
bool keyboard_store_bond(uint8_t a[6],uint8_t k[16],uint8_t *t){if(!bonded)return false;memcpy(a,ba,6);memcpy(k,bk,16);*t=bt;return true;}
void keyboard_store_set_bond(const uint8_t a[6],const uint8_t k[16],uint8_t t){bonded=true;memcpy(ba,a,6);memcpy(bk,k,16);bt=t;}
void keyboard_store_forget(void){bonded=false;}
bool controller_store_bond(uint8_t a[6],uint8_t k[16],uint8_t *t){(void)a;(void)k;(void)t;return false;}
#include "keyboard_host.c"
int main(void){
 keyboard_host_init();uint8_t state[]={BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING};keyboard_host_event(state,sizeof state);
 assert(keyboard_host_command("connect 11:21:AC:52:12:23",false)==409);
 assert(keyboard_host_command("connect 11:21:AC:52:12:23",true)==200);
 uint8_t pin_event[]={HCI_EVENT_PIN_CODE_REQUEST,6,0x23,0x12,0x52,0xac,0x21,0x11};assert(keyboard_host_event(pin_event,sizeof pin_event));assert(!strcmp(pin,"123456"));
 uint8_t open[]={HCI_EVENT_HID_META,13,HID_SUBEVENT_CONNECTION_OPENED,7,0,0,0x23,0x12,0x52,0xac,0x21,0x11,0,0,0};assert(keyboard_host_event(open,sizeof open));assert(connected);
 uint8_t available[]={HCI_EVENT_HID_META,4,HID_SUBEVENT_DESCRIPTOR_AVAILABLE,7,0,0};assert(keyboard_host_event(available,sizeof available));
 uint8_t other[]={HCI_EVENT_HID_META,4,HID_SUBEVENT_DESCRIPTOR_AVAILABLE,8,0,0};assert(!keyboard_host_event(other,sizeof other));
 uint8_t data[]={0xa1,2,0,4,5,0,0,0,0};report(data,sizeof data);keyboard_host_frame(false,true);assert(delivered[0]&(1<<4));assert(delivered[225/8]&(1<<(225%8)));
 data[3]=1;report(data,sizeof data);assert(invalid_reports==1);assert(keys[0]&(1<<4));
 data[3]=0;data[4]=0;data[1]=0;report(data,sizeof data);keyboard_host_frame(false,true);for(unsigned i=0;i<32;i++)assert(!delivered[i]);
 for(unsigned len=2;len<sizeof data;len++)report(data,len);
 assert(invalid_reports>1);
 // Learning a key suppresses game input, ignores keys held at capture start,
 // and waits for release before restoring normal input after cancellation.
 memset(keys,0,sizeof keys);keys[0]=1<<4;
 assert(keyboard_host_command("capture",true)==200);keyboard_host_frame(false,true);assert(!captured_usage&&!delivered[0]);
 keys[0]=0;keyboard_host_frame(false,true);keys[0]=1<<5;keyboard_host_frame(false,true);assert(captured_usage==5&&!delivered[0]);
 keyboard_host_command("capture-cancel",true);keyboard_host_frame(false,true);assert(!delivered[0]);
 keys[0]=0;keyboard_host_frame(false,true);keys[0]=1<<5;keyboard_host_frame(false,true);assert(delivered[0]==(1<<5));
 keyboard_host_command("capture",true);test_time+=60001;keyboard_host_frame(false,true);assert(!capturing&&!delivered[0]);
 memset(keys,0,sizeof keys);
 link_key_t key={42};assert(keyboard_host_save_key(address,key,4)&&bonded);
 assert(keyboard_host_command("disconnect",true)==200);assert(disconnects==1);keyboard_host_frame(false,true);for(unsigned i=0;i<32;i++)assert(!delivered[i]);
 keyboard_host_command("forget",true);assert(!bonded);keyboard_host_save_key(address,key,4);assert(!bonded);
 // A failed attempt to select another device must reconnect the saved keyboard.
 memcpy(ba,(uint8_t[]){0x11,0x21,0xac,0x52,0x12,0x23},6);bonded=true;
 connected=connecting=false;cid=0;memset(address,0x55,6);
 assert(keyboard_host_command("reconnect",true)==200);assert(!memcmp(address,ba,6));assert(!pairing);
 memset(address,0x66,6);retry_at=0;keyboard_host_frame(true,true);assert(!memcmp(address,ba,6));assert(connecting);
 puts("Keyboard host: pairing prompt, CID routing, real HID parser, rollover, truncation, release and late bond rejection passed");
}
