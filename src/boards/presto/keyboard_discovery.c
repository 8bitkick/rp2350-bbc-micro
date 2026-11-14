/* Read-only keyboard discovery trial. Never pairs, connects or writes bonds. */
#include "keyboard_discovery.h"
#include "btstack.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>
#define MAX_DEVICES 32
static struct { bd_addr_t address; char name[40]; uint32_t device_class; int rssi; bool hid,keyboard; } devices[MAX_DEVICES];
static unsigned count,stage,error;
static uint32_t deadline;
static bool truncated;
static uint32_t now(void){return to_ms_since_boot(get_absolute_time());}
bool keyboard_discovery_busy(void){return stage==1;}
int keyboard_discovery_start(void){
 if(keyboard_discovery_busy())return 409;
 memset(devices,0,sizeof devices);count=0;truncated=false;
 error=gap_inquiry_start(8);
 if(error){stage=4;return 500;}
 stage=1;deadline=now()+15000;return 200;
}
void keyboard_discovery_stop(void){
 if(stage==1)gap_inquiry_stop();
 stage=3;
}
void keyboard_discovery_frame(void){
 if(!keyboard_discovery_busy()||(int32_t)(now()-deadline)<0)return;
 keyboard_discovery_stop();
}
static bool keyboard_name(const uint8_t *name,unsigned len){
 const char word[]="keyboard";
 for(unsigned i=0;i+8<=len;i++){unsigned j=0;while(j<8 && (name[i+j]|32)==(unsigned)word[j])j++;if(j==8)return true;}
 return false;
}
static unsigned priority(unsigned i){return (devices[i].keyboard?8:0)+(devices[i].hid?4:0)+(devices[i].name[0]?2:0);}
static void add(const bd_addr_t a,const uint8_t *name,unsigned len,int rssi,uint32_t cod,bool hid,bool keyboard){
 keyboard|=keyboard_name(name,len);
 unsigned i;for(i=0;i<count;i++)if(!memcmp(a,devices[i].address,6))break;
 if(i==count){
  if(count==MAX_DEVICES){
   truncated=true;
   unsigned score=(keyboard?8:0)+(hid?4:0)+(len?2:0);
   i=0;for(unsigned j=1;j<count;j++)if(priority(j)<priority(i)||(priority(j)==priority(i)&&devices[j].rssi<devices[i].rssi))i=j;
   if(score<priority(i)||(score==priority(i)&&rssi<=devices[i].rssi))return;
  }else count++;
  memset(&devices[i],0,sizeof devices[i]);
  memcpy(devices[i].address,a,6);
 }
 if(len){if(len>39)len=39;for(unsigned j=0;j<len;j++){unsigned c=name[j];devices[i].name[j]=(c>=32&&c<127&&c!='"'&&c!='\\')?c:'_';}devices[i].name[len]=0;}
 devices[i].rssi=rssi;devices[i].device_class=cod;devices[i].hid|=hid;devices[i].keyboard|=keyboard;
}
bool keyboard_discovery_event(uint8_t *p,uint16_t size){
 if(size<2 || (unsigned)p[1]+2>size)return false;
 size=(uint16_t)p[1]+2;
 bd_addr_t address;
 switch(hci_event_packet_get_type(p)){
 case GAP_EVENT_INQUIRY_RESULT:
  if(stage!=1)return false;
  if(size<27)return true;
  gap_event_inquiry_result_get_bd_addr(p,address);
  {unsigned n=0;const uint8_t *name=NULL;
   if(gap_event_inquiry_result_get_name_available(p)){n=gap_event_inquiry_result_get_name_len(p);name=gap_event_inquiry_result_get_name(p);if(name+n>p+size)return true;}
   uint32_t cod=gap_event_inquiry_result_get_class_of_device(p);
   add(address,name,n,gap_event_inquiry_result_get_rssi_available(p)?(int8_t)gap_event_inquiry_result_get_rssi(p):-127,cod,(cod&0x1f00)==0x0500,(cod&0x1f40)==0x0540);
  }return true;
 case GAP_EVENT_INQUIRY_COMPLETE:
  if(stage!=1)return false;stage=3;return true;
 default:return false;
 }
}
void keyboard_discovery_json(char *out,size_t size){
 const char *state=stage==1?"classic_scan":stage==3?"finished":stage==4?"error":"idle";
 int n=snprintf(out,size,"{\"version\":3,\"capacity\":32,\"trial\":true,\"state\":\"%s\",\"error\":%u,\"truncated\":%s,\"devices\":[",state,error,truncated?"true":"false");
 if(n<0||(size_t)n>=size)return;
 size_t used=n;
 unsigned order[MAX_DEVICES];for(unsigned i=0;i<count;i++)order[i]=i;
 for(unsigned i=1;i<count;i++){unsigned v=order[i],j=i;while(j&&priority(v)>priority(order[j-1])){order[j]=order[j-1];j--;}order[j]=v;}
 bool output_truncated=false;
 for(unsigned pos=0;pos<count;pos++){
  unsigned i=order[pos];
  char entry[256];int k=snprintf(entry,sizeof entry,"%s{\"address\":\"%s\",\"name\":\"%s\",\"transport\":\"%s\",\"address_type\":%u,\"class\":%lu,\"hid_hint\":%s,\"rssi\":%d}",pos?",":"",bd_addr_to_str(devices[i].address),devices[i].name,"Classic",0u,(unsigned long)devices[i].device_class,devices[i].hid?"true":"false",devices[i].rssi);
  if(k<0||(size_t)k>=sizeof entry||(size_t)k+used+32>=size){output_truncated=true;break;}
  memcpy(out+used,entry,k);used+=k;
 }
 snprintf(out+used,size-used,"],\"output_truncated\":%s}",output_truncated?"true":"false");
}
