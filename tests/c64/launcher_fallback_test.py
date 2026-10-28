from pathlib import Path
r=Path(__file__).resolve().parents[2]
s=(r/'src/launcher/launcher.c').read_text().split('int main(void) {')[1]
prefix='''#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
#include <assert.h>
#define C64_DIAG_MAGIC 0x64360000u
#define C64_RETURN_MAGIC 0x64370000u
#define SYSTEM_SLOT_MASTER 0
#define SYSTEM_SLOT_BEEB 1
#define SYSTEM_SLOT_C64 3
#define XIP_NOCACHE_NOALLOC_BASE 0
#define BOARD_FLASH_SYSTEM_OFFSET 0
typedef int system_settings_t;
static struct {uint32_t scratch[8];} hw,*watchdog_hw=&hw;
static char s_work[4096];static bool warm,armed;static int selected,result;static jmp_buf done;
static bool watchdog_caused_reboot(void){return warm;}
static bool watchdog_enable_caused_reboot(void){return armed;}
static int system_settings_machine(const system_settings_t*s){return selected;}
static int system_slot(int s){return s;}
static void rom_load_partition_table(void*a,int b,bool c){}
static void chain(int s){result=s;longjmp(done,1);}
static void reset_usb_boot(int a,int b){}
static int launch(void){'''
tests='''
int main(void){
for(int w=0;w<2;w++)for(int a=0;a<2;a++)for(int magic=0;magic<2;magic++){
warm=w;armed=a;selected=3;hw.scratch[0]=magic?C64_DIAG_MAGIC:0;
if(!setjmp(done))launch();
assert(result==((a&&magic)?0:(w?3:0)));
if(a&&magic)assert(hw.scratch[0]==C64_RETURN_MAGIC);
}
}
'''
Path('/tmp/launcher-r5-test.c').write_text(prefix+s+tests)
