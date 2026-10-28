from pathlib import Path
import tempfile,subprocess
r=Path(__file__).resolve().parents[2];s=(r/'src/machines/c64/audio.c').read_text()
def function(name):
 a=s.index(name);a=s.rfind('\n',0,a)+1;b=s.index('{',a);depth=1;i=b+1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]+'\n'
code='''#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#define N 4096u
#define B 256u
#define __dmb() ((void)0)
static int16_t ring[N];static uint32_t head,tail,underruns,tone_remaining;
static bool enabled;static unsigned volume,tone_phase,pwm_min=1023,pwm_max;
'''+function('static void fill(')+function('void audio_set(')+'''
int main(void){uint32_t out[B];
audio_set(1,200);assert(volume==200);
for(int i=0;i<B;i++)ring[i]=i%2?32767:-32768;head=B;fill(out);
assert(tail==B);assert(out[0]==0);assert(out[1]==0x03ff03ff);
// A quiet SID waveform must receive the new gain, without wrapping.
head=tail+B;for(int i=0;i<B;i++)ring[(tail+i)&(N-1)]=2000;
fill(out);assert((out[0]&65535)==1012);
audio_set(0,-1);fill(out);assert(out[0]==0x02000200);
tone_remaining=44100;fill(out);assert(out[0]!=out[22]);assert(tone_remaining==44100-B);
for(int i=0;i<B;i++)assert((out[i]&65535)==(out[i]>>16));
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.c').write_text(code)
 subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Audio gain, clipping, channel packing, mute and test-tone checks passed')
