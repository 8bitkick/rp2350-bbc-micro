/* Classic keyboard trial: independent CID, bond and keyboard input ownership. */
#include "keyboard_host.h"
#include "controller_store.h"
#include "controller.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>
static bd_addr_t address;
static uint16_t cid;
static bool enabled=true,connecting,connected,descriptor_ready,ready,pairing;
static unsigned error,reports,invalid_reports;
static uint32_t retry_at,started;
static char pin[7];
static uint8_t keys[32];
static bool capturing,suppress_keys;
static unsigned captured_usage;
static uint32_t capture_started;
static uint8_t capture_previous[32];
static struct {bool valid;uint16_t id;uint8_t keys[32];} report_keys[4];
static uint32_t now(void){return to_ms_since_boot(get_absolute_time());}
static bool saved(void){bd_addr_t a;link_key_t k;uint8_t t;return keyboard_store_bond(a,k,&t);}
static bool owns(const bd_addr_t a){static const bd_addr_t zero={0};return memcmp(address,zero,6)&&!memcmp(a,address,6);}
bool keyboard_host_busy(void){return connecting;}
void keyboard_host_init(void){link_key_t k;uint8_t t;keyboard_store_bond(address,k,&t);retry_at=now()+3000;}
bool keyboard_host_save_key(bd_addr_t a,link_key_t k,link_key_type_t t){
 if(!owns(a))return false;
 if(enabled&&(connecting||connected))keyboard_store_set_bond(a,k,(uint8_t)t);
 return true;
}
bool keyboard_host_delete_key(bd_addr_t a){if(!owns(a))return false;keyboard_store_forget();return true;}
static void clear(void){memset(keys,0,sizeof keys);memset(report_keys,0,sizeof report_keys);descriptor_ready=false;pin[0]=0;}
static void connect_keyboard(void){
 connecting=true;started=now();clear();
 if(pairing)gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_ONLY);
 error=hid_host_connect(address,HID_PROTOCOL_MODE_REPORT,&cid);
 if(error){connecting=false;cid=0;retry_at=now()+10000;gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);}
}
/* HID descriptor parsing supports boot-style arrays as well as bitmap reports.
 * Reject short fields and rollover without reading outside the packet. */
static void report(const uint8_t *data,unsigned len){
 if(!descriptor_ready||len<2||data[0]!=0xa1)return;
 uint8_t next[32]={0};bool keyboard_report=false;uint16_t report_id=HID_REPORT_ID_UNDEFINED;
 btstack_hid_parser_t parser;
 btstack_hid_parser_init(&parser,hid_descriptor_storage_get_descriptor_data(cid),hid_descriptor_storage_get_descriptor_len(cid),HID_REPORT_TYPE_INPUT,data+1,len-1);
 while(btstack_hid_parser_has_more(&parser)){
  const btstack_hid_usage_item_t *it=&parser.descriptor_usage_item;
  unsigned end=it->bit_pos+it->size+(it->report_id!=HID_REPORT_ID_UNDEFINED?8:0);
  if(!it->size||it->size>31||end>(len-1)*8){invalid_reports++;memset(keys,0,sizeof keys);memset(report_keys,0,sizeof report_keys);return;}
  uint16_t page,usage;int32_t value;btstack_hid_parser_get_field(&parser,&page,&usage,&value);
  if(page!=7)continue;
  keyboard_report=true;report_id=it->report_id;
  if(usage>=1&&usage<=3&&value){invalid_reports++;return;}
  if(usage>=4&&usage<256&&value)next[usage/8]|=1u<<(usage%8);
 }
 if(keyboard_report){
  unsigned slot=4;
  for(unsigned i=0;i<4;i++)if(report_keys[i].valid&&report_keys[i].id==report_id){slot=i;break;}
  if(slot==4)for(unsigned i=0;i<4;i++)if(!report_keys[i].valid){slot=i;break;}
  if(slot==4){invalid_reports++;memset(keys,0,sizeof keys);memset(report_keys,0,sizeof report_keys);return;}
  report_keys[slot].valid=true;report_keys[slot].id=report_id;memcpy(report_keys[slot].keys,next,32);
  memset(keys,0,32);for(unsigned i=0;i<4;i++)if(report_keys[i].valid)for(unsigned j=0;j<32;j++)keys[j]|=report_keys[i].keys[j];
  reports++;
 }
}
bool keyboard_host_event(uint8_t *p,uint16_t size){
 if(size<2||(unsigned)p[1]+2>size)return false;
 bd_addr_t a;unsigned type=p[0];
 if(type==BTSTACK_EVENT_STATE&&size>=3){ready=btstack_event_state_get_state(p)==HCI_STATE_WORKING;if(!ready){cid=0;connecting=connected=false;clear();}return false;}
 if(type==HCI_EVENT_PIN_CODE_REQUEST&&size>=8){
  hci_event_pin_code_request_get_bd_addr(p,a);if(!owns(a))return false;
  if(enabled&&connecting){strcpy(pin,"123456");gap_pin_code_response(a,pin);}else gap_pin_code_negative(a);return true;
 }
 if(type==HCI_EVENT_USER_PASSKEY_NOTIFICATION&&size>=12){
  hci_event_user_passkey_notification_get_bd_addr(p,a);if(!owns(a))return false;
  snprintf(pin,sizeof pin,"%06lu",(unsigned long)hci_event_user_passkey_notification_get_numeric_value(p));return true;
 }
 if(type==HCI_EVENT_USER_CONFIRMATION_REQUEST&&size>=12){
  hci_event_user_confirmation_request_get_bd_addr(p,a);if(!owns(a))return false;
  if(enabled&&connecting)gap_ssp_confirmation_response(a);else gap_ssp_confirmation_negative(a);return true;
 }
 if(type!=HCI_EVENT_HID_META||size<5)return false;
 unsigned sub=p[2];uint16_t event_cid=0;
 if(sub==HID_SUBEVENT_INCOMING_CONNECTION){
  if(size<13)return true;
  hid_subevent_incoming_connection_get_address(p,a);if(!owns(a))return false;
  event_cid=hid_subevent_incoming_connection_get_hid_cid(p);
  if(enabled&&!connecting&&!connected&&saved()){cid=event_cid;connecting=true;started=now();hid_host_accept_connection(cid,HID_PROTOCOL_MODE_REPORT);}
  else hid_host_decline_connection(event_cid);return true;
 }
 if(sub==HID_SUBEVENT_CONNECTION_OPENED){
  if(size<15)return true;
  hid_subevent_connection_opened_get_bd_addr(p,a);if(!owns(a))return false;
  event_cid=hid_subevent_connection_opened_get_hid_cid(p);
  error=hid_subevent_connection_opened_get_status(p);connecting=false;connected=!error&&enabled;cid=connected?event_cid:0;
  if(!error&&!enabled)hid_host_disconnect(event_cid);
  pairing=false;pin[0]=0;retry_at=now()+10000;
  gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);return true;
 }
 switch(sub){
 case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:if(size<6)return true;event_cid=hid_subevent_descriptor_available_get_hid_cid(p);break;
 case HID_SUBEVENT_REPORT:if(size<7)return true;event_cid=hid_subevent_report_get_hid_cid(p);break;
 case HID_SUBEVENT_CONNECTION_CLOSED:event_cid=hid_subevent_connection_closed_get_hid_cid(p);break;
 default:return false;
 }
 if(!cid||event_cid!=cid)return false;
 if(sub==HID_SUBEVENT_DESCRIPTOR_AVAILABLE){error=hid_subevent_descriptor_available_get_status(p);descriptor_ready=!error;}
 if(sub==HID_SUBEVENT_REPORT){unsigned len=hid_subevent_report_get_report_len(p);const uint8_t *data=hid_subevent_report_get_report(p);if(data+len<=p+size)report(data,len);}
 if(sub==HID_SUBEVENT_CONNECTION_CLOSED){cid=0;connecting=connected=false;clear();retry_at=now()+3000;}
 return true;
}
void keyboard_host_frame(bool available,bool deliver){
 if(capturing){
  if(!connected||(uint32_t)(now()-capture_started)>60000){capturing=false;captured_usage=0;}
  else if(!captured_usage)for(unsigned i=4;i<256;i++)if((keys[i/8]&~capture_previous[i/8])&(1u<<(i%8))){captured_usage=i;break;}
  memcpy(capture_previous,keys,32);
 }
 bool any=false;for(unsigned i=0;i<32;i++)any|=keys[i]!=0;
 if(!any&&!capturing)suppress_keys=false;
 if(deliver){static const uint8_t empty[32]={0};controller_bluetooth_keyboard(capturing||suppress_keys?empty:keys);}
 if(connecting&&(uint32_t)(now()-started)>30000){enabled=false;if(cid)hid_host_disconnect(cid);connecting=false;connected=false;cid=0;clear();error=0xff;gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);}
 if(enabled&&ready&&available&&!connecting&&!connected&&saved()&&(int32_t)(now()-retry_at)>=0){
  link_key_t key;uint8_t type;keyboard_store_bond(address,key,&type);pairing=false;connect_keyboard();
 }
}
int keyboard_host_command(const char *s,bool available){
 if(!strcmp(s,"capture")){
  if(!connected||!descriptor_ready)return 409;
  capturing=suppress_keys=true;captured_usage=0;capture_started=now();memcpy(capture_previous,keys,32);return 200;
 }
 if(!strcmp(s,"capture-cancel")){capturing=false;captured_usage=0;return 200;}

 if(!strcmp(s,"disconnect")||!strcmp(s,"forget")){
  enabled=false;pairing=false;if(cid)hid_host_disconnect(cid);clear();
  if(!strcmp(s,"forget"))keyboard_store_forget();
  return 200;
 }
 if(!strcmp(s,"reconnect")){if(connecting||connected)return 409;if(!saved())return 404;link_key_t key;uint8_t type;keyboard_store_bond(address,key,&type);pairing=false;enabled=true;retry_at=now();return 200;}
 if(!strncmp(s,"connect ",8)){
  bd_addr_t target;if(!sscanf_bd_addr(s+8,target))return 400;
  if(!ready||!available||connecting||connected)return 409;
  bd_addr_t nimbus;link_key_t key;uint8_t t;
  if(controller_store_bond(nimbus,key,&t)&&!memcmp(nimbus,target,6))return 409;
  memcpy(address,target,6);enabled=true;pairing=true;connect_keyboard();return error?500:200;
 }
 return 400;
}
void keyboard_host_json(char *out,size_t size){
 snprintf(out,size,",\"bluetooth_keyboard\":{\"transport\":\"Classic\",\"state\":\"%s\",\"address\":\"%s\",\"remembered\":%s,\"reconnect\":%s,\"descriptor\":%s,\"error\":%u,\"reports\":%u,\"invalid_reports\":%u,\"pin\":\"%s\",\"capturing\":%s,\"captured_usage\":%u}",!ready?"starting":connected?"connected":connecting?"connecting":"idle",bd_addr_to_str(address),saved()?"true":"false",enabled?"true":"false",descriptor_ready?"true":"false",error,reports,invalid_reports,pin,capturing?"true":"false",captured_usage);
}

const char *keyboard_host_boot_status(void) {
 if(connected && descriptor_ready)return "CONNECTED";
 if(connecting || connected)return pin[0]?"PAIRING - SEE WEB":"CONNECTING";
 if(!saved())return "NOT PAIRED";
 if(!enabled)return "DISCONNECTED";
 return ready?"SEARCHING":"STARTING";
}
