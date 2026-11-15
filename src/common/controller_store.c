/* Versioned, redundant flash record. All callers run on core 0; writes are
 * deferred out of BTstack callbacks and use the existing safe flash writer. */
#include "controller_store.h"
#include "mem.h"
#include <stddef.h>
#include <string.h>
#define MAGIC 0x4e494d33u
struct record {
    uint32_t magic,version,sequence;
    uint8_t bonded,type,address[6],key[16];
    uint32_t configured;
    int32_t config[3][19];
    uint32_t checksum;
    // Append-only extension: legacy firmware still validates the v1 prefix.
    uint32_t spectrum_magic;
    int32_t spectrum_config[19];
    uint32_t spectrum_checksum;
    uint32_t preferences_magic, preferences_mask;
    uint8_t preferences[4][5]; // sound, volume, ambient, brightness, lock indicators
    uint32_t preferences_checksum;
    uint32_t keyboard_magic;
    uint8_t keyboard_bonded,keyboard_type,keyboard_address[6],keyboard_key[16];
    uint32_t keyboard_checksum;
    uint32_t mapping_magic;
    uint8_t keyboard_maps[4][KEYBOARD_MAP_BYTES];
    uint32_t mapping_checksum;
};
_Static_assert(offsetof(struct record,checksum)==268,"Keep legacy controller record layout");
_Static_assert(offsetof(struct record,spectrum_magic)==272,"Append Spectrum after the legacy checksum");
static struct record state;
static int current=-1;
static bool dirty;
static uint32_t checksum(const struct record *p) {
    const uint8_t *b=(const uint8_t *)p;uint32_t crc=0xffffffffu;
    for(size_t i=0;i<(p->version==2?sizeof *p:offsetof(struct record,checksum));i++) {
        if(i>=offsetof(struct record,checksum)&&i<offsetof(struct record,checksum)+4)continue;
        crc^=b[i];for(int k=0;k<8;k++)crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    return ~crc;
}
static uint32_t spectrum_checksum(const struct record *p) {
    const uint8_t *b=(const uint8_t *)&p->spectrum_magic;uint32_t crc=0xffffffffu;
    for(size_t i=0;i<sizeof p->spectrum_magic+sizeof p->spectrum_config;i++){
        crc^=b[i];for(int k=0;k<8;k++)crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    return ~crc;
}
static bool spectrum_valid(const struct record *p){return p->spectrum_magic==0x53504331u&&p->spectrum_checksum==spectrum_checksum(p);}
static uint32_t preferences_checksum(const struct record *p){
 const uint8_t *b=(const uint8_t *)&p->preferences_magic;uint32_t crc=0xffffffffu;
 for(size_t i=0;i<28;i++){crc^=b[i];for(int k=0;k<8;k++)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}
 return ~crc;
}
static bool preferences_values_valid(const uint8_t p[5]){return p[0]<=1&&p[1]<=100&&p[2]<=1&&p[3]<=100&&p[4]<=1;}
static bool preferences_valid(const struct record *p){
 if(p->preferences_magic!=0x50524631u||p->preferences_mask>15||p->preferences_checksum!=preferences_checksum(p))return false;
 for(unsigned i=0;i<4;i++)if((p->preferences_mask&(1u<<i))&&!preferences_values_valid(p->preferences[i]))return false;
 return true;
}
bool controller_store_get_preferences(unsigned m,uint8_t out[5]){
 if(m>=4||!preferences_valid(&state)||!(state.preferences_mask&(1u<<m)))return false;
 memcpy(out,state.preferences[m],5);return true;
}
void controller_store_set_preferences(unsigned m,const uint8_t p[5]){
 if(m>=4||!preferences_values_valid(p))return;
 if(!preferences_valid(&state)){state.preferences_magic=0x50524631u;state.preferences_mask=0;memset(state.preferences,0,sizeof state.preferences);}
 if((state.preferences_mask&(1u<<m))&&!memcmp(state.preferences[m],p,5))return;
 memcpy(state.preferences[m],p,5);state.preferences_mask|=1u<<m;
 state.preferences_checksum=preferences_checksum(&state);dirty=true;
}
static bool valid(const struct record *p) {
    return p->magic==MAGIC&&(p->version==1||p->version==2)&&p->bonded<=1&&p->configured<=7&&p->checksum==checksum(p);
}
void controller_store_init(void) {
    memset(&state,0,sizeof state);current=-1;dirty=false;
    for(int i=0;i<2;i++) {
        uint32_t offset,size;if(!mem_flash_region(MEM_FLASH_CONTROLLER_A+i,&offset,&size)||size<sizeof state)continue;
        const struct record *p=(const struct record *)mem_flash_ptr(offset);
        if(valid(p)&&(current<0||(int32_t)(p->sequence-state.sequence)>0)){memcpy(&state,p,sizeof state);current=i;}
    }
    if(!spectrum_valid(&state)){state.spectrum_magic=0;memset(state.spectrum_config,0,sizeof state.spectrum_config);state.spectrum_checksum=0;}
}
bool controller_store_bond(uint8_t a[6],uint8_t k[16],uint8_t *type) {
    if(!state.bonded)return false;
    memcpy(a,state.address,6);memcpy(k,state.key,16);*type=state.type;return true;
}
void controller_store_set_bond(const uint8_t a[6],const uint8_t k[16],uint8_t type) {
    if(state.bonded&&state.type==type&&!memcmp(a,state.address,6)&&!memcmp(k,state.key,16))return;
    state.bonded=1;state.type=type;memcpy(state.address,a,6);memcpy(state.key,k,16);dirty=true;
}
void controller_store_forget(void) {
    if(!state.bonded)return;
    state.bonded=0;state.type=0;memset(state.address,0,6);memset(state.key,0,16);dirty=true;
}
bool controller_store_get_config(unsigned m,int32_t cfg[19]) {
    if(m==3){if(!spectrum_valid(&state))return false;memcpy(cfg,state.spectrum_config,sizeof state.spectrum_config);return true;}
    if(m>=3||!(state.configured&(1u<<m)))return false;
    memcpy(cfg,state.config[m],sizeof state.config[m]);return true;
}
void controller_store_set_config(unsigned m,const int32_t cfg[19]) {
    if(m==3){
        if(spectrum_valid(&state)&&!memcmp(cfg,state.spectrum_config,sizeof state.spectrum_config))return;
        memcpy(state.spectrum_config,cfg,sizeof state.spectrum_config);state.spectrum_magic=0x53504331u;
        state.spectrum_checksum=spectrum_checksum(&state);dirty=true;return;
    }
    if(m>=3)return;
    if((state.configured&(1u<<m))&&!memcmp(cfg,state.config[m],sizeof state.config[m]))return;
    memcpy(state.config[m],cfg,sizeof state.config[m]);state.configured|=1u<<m;dirty=true;
}
bool controller_store_pending(void){return dirty;}
bool controller_store_flush(void) {
    if(!dirty)return true;
    uint32_t off,size;int next=current<0?0:current^1;
    if(!mem_flash_region(MEM_FLASH_CONTROLLER_A+next,&off,&size)||size<sizeof state)return false;
    state.magic=MAGIC;state.version=2;state.sequence++;state.checksum=checksum(&state);
    if(!mem_flash_write_sector(off,&state,sizeof state))return false;
    if(memcmp(mem_flash_ptr(off),&state,sizeof state))return false;
    current=next;dirty=false;return true;
}

static uint32_t keyboard_checksum(void){
 const uint8_t *b=(const uint8_t *)&state.keyboard_magic;uint32_t crc=0xffffffffu;
 for(unsigned i=0;i<28;i++){crc^=b[i];for(int k=0;k<8;k++)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}
 return ~crc;
}
bool keyboard_store_bond(uint8_t a[6],uint8_t k[16],uint8_t *type){
 if(state.keyboard_magic!=0x4b424431u||state.keyboard_bonded!=1||state.keyboard_checksum!=keyboard_checksum())return false;
 memcpy(a,state.keyboard_address,6);memcpy(k,state.keyboard_key,16);*type=state.keyboard_type;return true;
}
void keyboard_store_set_bond(const uint8_t a[6],const uint8_t k[16],uint8_t type){
 uint8_t olda[6],oldk[16],oldt;
 if(keyboard_store_bond(olda,oldk,&oldt)&&oldt==type&&!memcmp(a,olda,6)&&!memcmp(k,oldk,16))return;
 state.keyboard_magic=0x4b424431u;state.keyboard_bonded=1;state.keyboard_type=type;
 memcpy(state.keyboard_address,a,6);memcpy(state.keyboard_key,k,16);
 state.keyboard_checksum=keyboard_checksum();dirty=true;
}
void keyboard_store_forget(void){
 state.keyboard_magic=0x4b424431u;state.keyboard_bonded=0;state.keyboard_type=0;
 memset(state.keyboard_address,0,6);memset(state.keyboard_key,0,16);
 state.keyboard_checksum=keyboard_checksum();dirty=true;
}

static uint32_t mapping_checksum(void){
 const uint8_t *b=(const uint8_t *)&state.mapping_magic;uint32_t crc=0xffffffffu;
 for(unsigned i=0;i<4+sizeof state.keyboard_maps;i++){crc^=b[i];for(int k=0;k<8;k++)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}
 return ~crc;
}
static bool mapping_valid(void){return state.mapping_magic==0x4b4d5031u&&state.mapping_checksum==mapping_checksum();}
void keyboard_store_get_map(unsigned machine,uint8_t out[KEYBOARD_MAP_BYTES]){
 memset(out,0,KEYBOARD_MAP_BYTES);
 if(machine<4&&mapping_valid())memcpy(out,state.keyboard_maps[machine],KEYBOARD_MAP_BYTES);
}
void keyboard_store_set_map(unsigned machine,const uint8_t map[KEYBOARD_MAP_BYTES]){
 if(machine>=4)return;
 if(!mapping_valid()){state.mapping_magic=0x4b4d5031u;memset(state.keyboard_maps,0,sizeof state.keyboard_maps);}
 if(!memcmp(state.keyboard_maps[machine],map,KEYBOARD_MAP_BYTES))return;
 memcpy(state.keyboard_maps[machine],map,KEYBOARD_MAP_BYTES);state.mapping_checksum=mapping_checksum();dirty=true;
}
