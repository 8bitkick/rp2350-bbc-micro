#include <cstdio>
#include <cstring>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/structs/qmi.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/scb.h"
#include "board.h"
#include "machine_if.h"
#include "frontend.h"
#include "drive.h"
#include "psram_allocator.h"
extern "C" {
#include "sound.h"
extern const uint8_t c64_default_disc[];
extern const uint32_t c64_default_disc_size;
void c64_init(void); bool c64_run_frame(void); void c64_reset(void); void c64_nmi(void);
uint8_t *c64_get_framebuffer(void); uint8_t *c64_get_ram(void);
void c64_mount_disk(const uint8_t *,uint32_t,const char *);void c64_unmount_disk(void);
void c64_disk_set(uint8_t *,size_t);uint32_t c64_disk_writes(void);uint32_t c64_memory_used(void);
}
#if !WITH_SOUND
extern "C" void sid_add_sample(int16_t,int16_t){}
extern "C" int sid_get_buffer_fill(){return 0;}
#endif
// Scratch 0..3 survive watchdog resets; SDK boot control uses 4..7.
static constexpr uint32_t DIAG_MAGIC=0x64360000;
static bool recovery;
static uint32_t previous_phase, previous_pc, previous_cfsr;
static volatile bool fault_seen;
static uint32_t trace_frames;
static volatile uint32_t trace_rendered;
static void phase(uint32_t n){if(!fault_seen)watchdog_hw->scratch[0]=DIAG_MAGIC|(n&255);}
extern "C" void c64_network_phase(uint32_t n){phase(n);}
extern "C" void c64_fault(uint32_t *frame){
    fault_seen=true;
    watchdog_hw->scratch[2]=frame[6];
    watchdog_hw->scratch[1]=scb_hw->cfsr;
    watchdog_hw->scratch[3]=scb_hw->bfar;
    watchdog_hw->scratch[0]|=get_core_num()<<8;
    // Let the armed watchdog expire, preserving its reset-cause marker.
    for(;;)__wfi();
}
extern "C" void __attribute__((naked)) isr_hardfault(){
    __asm volatile("tst lr, #4\n ite eq\n mrseq r0, msp\n mrsne r0, psp\n"
                   "b c64_fault\n");
}
extern "C" void c64_diagnostics_json(char *out,uint32_t size){
    snprintf(out,size,",\"c64\":{\"build\":\"r11\",\"recovery\":%s,\"phase\":%lu,\"pc\":\"%08lx\",\"cfsr\":\"%08lx\",\"uptime_ms\":%lu,\"live_phase\":%lu,\"frames\":%lu,\"rendered\":%lu}",
        recovery?"true":"false",(unsigned long)previous_phase,(unsigned long)previous_pc,(unsigned long)previous_cfsr,
        (unsigned long)to_ms_since_boot(get_absolute_time()),(unsigned long)(watchdog_hw->scratch[0]&511),
        (unsigned long)trace_frames,(unsigned long)trace_rendered);
}
static bool turbo, reset_pending, install_hold;
static uint8_t keys[8],reverse_keys[8];
static uint32_t boot_frames;
static const char *typing;
static uint8_t joystick=255;
static bool pressed[256];
static void rebuild_keys(){
 memset(keys,255,8);memset(reverse_keys,255,8);joystick=255;
 for(unsigned c=0;c<256;c++)if(pressed[c]){
 unsigned code=c;
 if(c>=129 && c<=133){joystick&=~(1u<<(c-129));continue;}
 if(c>=64 && c<128){code-=64;keys[1]&=~128;reverse_keys[7]&=~2;}
 if(code>=64)continue;
 unsigned row=code/8,bit=code%8;keys[row]&=~(1u<<bit);reverse_keys[bit]&=~(1u<<row);
 }
}
static unsigned speed=100, load0, load1;
// One RGB565 row per VIC row; the LCD row table stretches 272 rows to 480.
static uint16_t pixels[272][482] __attribute__((aligned(4)));
static const uint16_t *rows[481];
static volatile bool frame_ready;
static volatile uint32_t conversion_us;
static const uint32_t palette[16]={0x000000,0xffffff,0x813338,0x75cec8,0x8e3c97,0x56ac4d,0x2e2c9b,0xedf171,0x8e5029,0x553800,0xc46c71,0x4a4a4a,0x7b7b7b,0xa9ff9f,0x706deb,0xb2b2b2};
static uint16_t pin_palette[16];
static void convert_frame(){
    const uint8_t *src=c64_get_framebuffer();
    for(int y=0;y<272;y++)for(int group=0;group<96;group++){
        const uint8_t *in=src+y*384+group*4;uint16_t *out=pixels[y]+group*5;
        out[0]=out[1]=pin_palette[in[0]&15];out[2]=pin_palette[in[1]&15];
        out[3]=pin_palette[in[2]&15];out[4]=pin_palette[in[3]&15];
    }
}
static void core1(){
    flash_safe_execute_core_init();
#if WITH_SOUND
    audio_init();
#endif
    for(;;){
        if(!frame_ready){__wfe();continue;}
        __dmb();uint32_t t=time_us_32();convert_frame();trace_rendered++;conversion_us=time_us_32()-t;
#if WITH_AMBIENT
        if(board_leds_due())board_leds_update();
#endif
        __dmb();frame_ready=false;__sev();
    }
}
// App installation is shared with BBC modes. Keep the last C64 picture while
// paused; the BBC splash renderer cannot draw into this RGB565 framebuffer.
extern "C" void machine_install_screen(const char *, int permille){
    install_hold = permille >= 0;
}
extern "C" const char *machine_name(){return "c64";}
extern "C" void machine_key(int code,bool down){
    if(recovery)return;
    // A real key takes over from the startup typing, including during its delay.
    if(down){typing=nullptr;boot_frames=0;}
    if(code==134){if(down)c64_nmi();return;}
    if(code<0 || code>=256)return;
    pressed[code]=down;rebuild_keys();
}
extern "C" void c64_keyboard_poll(uint8_t *k,uint8_t *r,uint8_t *j){memcpy(k,keys,8);memcpy(r,reverse_keys,8);*j=255;}
extern "C" uint8_t input_get_joystick2(){return joystick;}
extern "C" void machine_key_sdl(uint32_t s,bool down){
    static const uint8_t letters[]={10,28,20,18,14,21,26,29,33,34,37,42,36,39,38,41,62,17,13,22,30,31,9,23,25,12};
    static const uint8_t digits[]={56,59,8,11,16,19,24,27,32,35};
    if(s>=4 && s<=29){machine_key(letters[s-4],down);return;}
    if(s>=30 && s<=39){machine_key(digits[s-30],down);return;}
    int c=-1;switch(s){case 40:c=1;break;case 41:c=63;break;case 42:c=0;break;case 44:c=60;break;
    case 45:c=43;break;case 46:c=53;break;case 51:c=50;break;case 54:c=47;break;case 55:c=44;break;case 56:c=55;break;
    case 58:c=4;break;case 59:c=68;break;case 60:c=5;break;case 61:c=69;break;case 62:c=6;break;case 63:c=70;break;case 64:c=3;break;case 65:c=67;break;
    case 74:c=51;break;case 79:c=2;break;case 80:c=66;break;case 81:c=7;break;case 82:c=71;break;
    case 224:case 228:c=58;break;case 225:case 229:c=15;break;case 226:case 230:c=61;break;}
    if(c>=0)machine_key(c,down);
}
extern "C" void machine_break(){if(recovery){watchdog_reboot(0,0,10);return;}reset_pending=true;}
extern "C" void machine_boot_shift(){boot_frames=100;}
extern "C" bool machine_turbo_get(){return turbo;}
extern "C" void machine_turbo_set(bool on){turbo=on;}
extern "C" bool machine_overlay_has(){return false;}
extern "C" bool machine_overlay_get(){return false;}
extern "C" void machine_overlay_set(bool){}
extern "C" void machine_perf_get(unsigned *a,unsigned *b,unsigned *s){*a=load0;*b=load1;*s=speed;}
extern "C" uint32_t machine_psram_used(){return c64_memory_used();}
extern "C" unsigned machine_leds(){return 0;}
extern "C" void machine_print_stats(){printf("STATS machine=c64 core0=%u core1=%u speed=%u%%\n",load0,load1,speed);}
extern "C" bool machine_usb_command(const char *cmd){if(!strcmp(cmd,"restore")){c64_nmi();return true;}return false;}
extern "C" void machine_drive_load(const uint8_t *image,uint32_t n,bool,bool boot){
    if(recovery)return;
    c64_unmount_disk();typing=nullptr;boot_frames=0;
    uint8_t *dst=board_psram()+1024*1024;
    if(image && n){memmove(dst,image,n);c64_disk_set(dst,n);c64_mount_disk(dst,n,"presto.d64");}
    else c64_disk_set(nullptr,0);
    if(boot){reset_pending=true;boot_frames=image?100:0;}
}
extern "C" uint32_t machine_drive_writes(){return c64_disk_writes();}
extern "C" bool machine_drive_sector(int,int,uint8_t *){return false;}
extern "C" void machine_drive_clean(){}
static void __no_inline_not_in_flash_func(slow_flash)(){
    hw_write_masked(&qmi_hw->m[0].timing,(3u<<QMI_M0_TIMING_CLKDIV_LSB)|(2u<<QMI_M0_TIMING_RXDELAY_LSB),QMI_M0_TIMING_CLKDIV_BITS|QMI_M0_TIMING_RXDELAY_BITS);
}
int main(){
    slow_flash();vreg_set_voltage(VREG_VOLTAGE_1_25);set_sys_clock_khz(250000,true);
    recovery=watchdog_enable_caused_reboot() && (watchdog_hw->scratch[0]&0xffff0000u)==DIAG_MAGIC;
    previous_phase=watchdog_hw->scratch[0]&511;previous_pc=watchdog_hw->scratch[2];previous_cfsr=watchdog_hw->scratch[1];
    watchdog_disable();
    stdio_init_all();board_init();board_psram_init();c64_memory_init();
    if(recovery){
        printf("C64 RECOVERY phase=%lu pc=%08lx cfsr=%08lx\n",(unsigned long)previous_phase,(unsigned long)previous_pc,(unsigned long)previous_cfsr);
        frontend_init();
        for(;;){frontend_frame();sleep_ms(20);}
    }
    watchdog_hw->scratch[0]=DIAG_MAGIC;watchdog_hw->scratch[1]=0;watchdog_hw->scratch[2]=0;watchdog_hw->scratch[3]=0;
    phase(1);watchdog_enable(8000,true);
    memset(keys,255,8);memset(reverse_keys,255,8);
    for(int i=0;i<16;i++){
        uint32_t p=palette[i],v=((p>>8)&0xf800)|((p>>5)&0x07e0)|((p>>3)&0x1f),rev;
        __asm("rbit %0, %1":"=r"(rev):"r"(v<<16));pin_palette[i]=(uint16_t)rev;
    }
#if WITH_AMBIENT
    board_leds_init();
#endif
    for(int y=0;y<480;y++)rows[y]=pixels[y*272/480];
    rows[480]=nullptr;
    board_display_init(rows);phase(2);c64_init();phase(3);drive0_init(c64_default_disc,c64_default_disc_size,board_psram());boot_frames=100;frontend_init();
    multicore_launch_core1(core1);watchdog_enable(8000,true);
    uint64_t due=time_us_64(),stat=due,busy=0;unsigned frames=0;
    for(;;){
        if(install_hold){
            frontend_frame();watchdog_update();sleep_ms(1);
            due=time_us_64();stat=due;busy=0;frames=0;
            continue;
        }
        if(reset_pending){c64_reset();reset_pending=false;memset(pressed,0,sizeof pressed);rebuild_keys();}
        phase(4);uint64_t t=time_us_64();c64_run_frame();trace_frames++;busy+=time_us_64()-t;
        phase(5);
#if WITH_AMBIENT
        board_leds_frame(0);
#endif
        // Turbo skips alternate display conversions, giving emulation more time.
        if(!turbo || (trace_frames&1)==0){__dmb();frame_ready=true;__sev();}
        // Do not mutate the VIC framebuffer until core 1 has converted it.
        while(frame_ready){frontend_poll();sleep_us(50);}
        phase(6);frontend_frame();watchdog_update();
        if(boot_frames && !--boot_frames)typing="LOAD\"*\",8,1\rRUN\r";
        if(typing && *typing){uint8_t *ram=c64_get_ram();if(ram[198]<10){ram[631+ram[198]]= *typing++;ram[198]++;}}
        phase(7);frames++;due+=20000;
        if(!turbo){while(time_us_64()<due){frontend_poll();sleep_us(100);}}
        else due=time_us_64();
        if(time_us_64()-due>100000)due=time_us_64();
        uint64_t now=time_us_64();if(now-stat>=5000000){uint64_t dt=now-stat;load0=busy*100/dt;load1=conversion_us/200;speed=frames*2000000ull/dt;machine_print_stats();stat=now;busy=0;frames=0;}
    }
}
