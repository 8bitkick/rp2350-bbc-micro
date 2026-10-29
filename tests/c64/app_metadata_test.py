"""Exercise actual metadata readers without mapping executable APP_SLOT_XIP."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'src/common/app.c').read_text()
a=s.index('uint32_t app_flash_used(void)');b=s.index('bool app_turn_on',a)
c=s.index('static const struct app_header *described(void)');d=s.index('void app_json',c)
code='''#include <assert.h>
#include <string.h>
#include "app_api.h"
static struct app_header physical,app_header,s_hooks;
static const struct app_header *s_app;
static bool s_builtin;
static const void *mem_flash_ptr(uint32_t offset){assert(offset==APP_SLOT_OFFSET);return &physical;}
'''+s[a:b]+s[c:d]+'''
int main(void){
memset(&physical,255,sizeof physical);
assert(app_flash_used()==0);assert(described()==0);
physical.magic=APP_MAGIC;physical.image_size=4096;
assert(app_flash_used()==4096);assert(described()==&physical);
physical.image_size=APP_SLOT_SIZE+1;assert(app_flash_used()==0);
s_app=&s_hooks;assert(described()==&s_hooks);
s_app=0;s_builtin=true;assert(described()==&app_header);
}
'''
with tempfile.TemporaryDirectory() as temp:
 p=Path(temp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-fsanitize=address,undefined','-I'+str(root/'src/common'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Unmapped app metadata: erased, valid, oversized and loaded/built-in cases passed')
