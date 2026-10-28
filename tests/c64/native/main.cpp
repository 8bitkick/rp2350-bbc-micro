#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
extern "C" {
void *psram_malloc(size_t n){return malloc(n);}void psram_free(void *p){free(p);}void *psram_realloc(void *p,size_t n){return realloc(p,n);}
void c64_keyboard_poll(uint8_t *a,uint8_t *b,uint8_t *j){memset(a,255,8);memset(b,255,8);*j=255;}
uint8_t input_get_joystick2(){return 255;}void sid_add_sample(int16_t,int16_t){}int sid_get_buffer_fill(){return 0;}
void c64_init();bool c64_run_frame();uint8_t *c64_get_ram();uint8_t *c64_get_framebuffer();void c64_disk_set(uint8_t *,size_t);void c64_mount_disk(const uint8_t *,uint32_t,const char *);void c64_reset();
}
int main(int argc,char **argv){
 c64_init();for(int i=0;i<1000;i++)c64_run_frame();puts("BASIC 1000 frames passed");fflush(stdout);
 FILE *f=fopen(argv[1],"rb");uint8_t *disk=(uint8_t*)malloc(174848);if(!f || fread(disk,1,174848,f)!=174848)return 1;fclose(f);
 c64_disk_set(disk,174848);c64_mount_disk(disk,174848,"presto.d64");c64_reset();const char *typing="LOAD\"*\",8,1\rRUN\r";
 for(int i=0;i<10000;i++){c64_run_frame();if(i>=100 && *typing){uint8_t *ram=c64_get_ram();if(ram[198]<10){ram[631+ram[198]]=*typing++;ram[198]++;}}}
 puts("Elite 10000 frames passed");FILE *o=fopen("/tmp/c64host/frame.raw","wb");fwrite(c64_get_framebuffer(),1,384*272,o);fclose(o);
}
