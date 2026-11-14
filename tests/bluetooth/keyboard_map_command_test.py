"""Run the actual keyboard mapping command branch with bounded storage mocks."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'src/boards/presto/presto_bluetooth.c').read_text()
a=s.index('    if(!strncmp(s,"keyboard map ",13)');b=s.index('    if(!strncmp(s,"keyboard ",9)){',a)
source='''#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static uint8_t active[64],saved[64];static bool fail,store_error;
static unsigned machine_index(void){return 2;}
static void controller_keyboard_map_get(uint8_t *p){memcpy(p,active,64);}
static void controller_keyboard_map_set(const uint8_t *p){memcpy(active,p,64);}
static void keyboard_store_set_map(unsigned m,const uint8_t *p){assert(m==2);memcpy(saved,p,64);}
static bool controller_store_flush(void){return !fail;}
static int keyboard_host_command(const char *s,bool b){(void)b;assert(!strcmp(s,"capture-cancel"));return 200;}
static int command(const char *s){
'''+s[a:b]+'''return 400;}
int main(void){
 assert(command("keyboard map 3 42")==400);assert(command("keyboard map 4 128")==400);
 assert(command("keyboard map 4 42 trailing")==400);
 for(unsigned i=4;i<36;i++){char c[64];snprintf(c,sizeof c,"keyboard map %u 42",i);assert(command(c)==200);}
 assert(command("keyboard map 36 42")==409);assert(command("keyboard map 4 0")==200);assert(active[1]==0);
 assert(command("keyboard unmap 4")==200);assert(command("keyboard unmap 4")==404);
 fail=true;assert(command("keyboard map 36 7")==500);assert(active[0]==0);
 fail=false;assert(command("keyboard map 36 7")==200);assert(active[0]==36&&active[1]==7);
 puts("Actual mapping command: bounds, capacity, replacement, delete and save failure passed");}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(source)
 subprocess.run(['cc','-std=c11','-fsanitize=address,undefined',str(p),'-o',str(Path(d)/'test')],check=True)
 subprocess.run([str(Path(d)/'test')],check=True)
