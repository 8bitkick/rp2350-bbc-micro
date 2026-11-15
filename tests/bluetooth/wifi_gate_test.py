"""Exercise the actual firmware retry function with a changing Wi-Fi link."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'src/boards/presto/presto_bluetooth.c').read_text()
a=s.index('static void bluetooth_frame(bool save) {')
b=s.index('\nvoid presto_bluetooth_frame',a)
fn=s[a:b]
source=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "nimbus_reconnect.h"
static bool store_started=true,store_error,ready=true,reconnect=true,scanning,connecting,connected;
static unsigned cid,attempts,error,retry_streak,failures;
static bool keyboard_busy;
#define BT_KEYBOARD_DISCOVERY 1
static void keyboard_discovery_frame(void){}
static bool keyboard_discovery_busy(void){return keyboard_busy;}
static uint32_t retry_at=3000,now;
static bool wifi;
static unsigned selected;
#define HID_PROTOCOL_MODE_REPORT 1
static bool controller_store_pending(void){return false;}
static bool controller_store_flush(void){return true;}
static uint32_t now_ms(void){return now;}
static bool net_boot_has_address(void){return wifi;}
static bool remembered(unsigned x){(void)x;return true;}
static unsigned hid_host_connect(unsigned a,int b,unsigned *c){(void)a;(void)b;(void)c;return 4;}
static void note_failure(unsigned e,unsigned p){(void)e;(void)p;failures++;}
'''+fn+r'''
int main(void){
 // A slow/failed Wi-Fi join must never initiate outgoing Bluetooth paging.
 for(now=0;now<90000;now+=100)bluetooth_frame(false);
 assert(attempts==0 && failures==0 && retry_streak==0 && retry_at==3000);
 // Obtaining an address releases an overdue retry immediately.
 wifi=true;bluetooth_frame(false);
 assert(attempts==1 && failures==1 && retry_streak==1 && retry_at==93000);
 // Loss of the address also protects re-association without erasing backoff.
 wifi=false;now=100000;bluetooth_frame(true);
 assert(attempts==1 && retry_at==93000 && retry_streak==1);
 wifi=true;bluetooth_frame(true);
 assert(attempts==2 && retry_streak==2 && retry_at==110000);
 // If Wi-Fi joins quickly, retain the original three-second listening window.
 attempts=failures=retry_streak=0;retry_at=3000;now=1000;
 bluetooth_frame(false);assert(attempts==0);
 now=3000;bluetooth_frame(false);assert(attempts==1);
 keyboard_busy=true;now=100000;bluetooth_frame(true);assert(attempts==1);
 keyboard_busy=false;bluetooth_frame(true);assert(attempts==2);
 reconnect=false;now=200000;bluetooth_frame(true);assert(attempts==2);
 puts("Actual retry function: Wi-Fi startup/rejoin gate and original quick reconnect window passed");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(source)
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(root/'src/boards/presto'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
