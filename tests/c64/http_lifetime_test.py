from pathlib import Path
s=(Path(__file__).resolve().parents[2] / 'src/boards/presto/presto_net.c').read_text()
def fn(name):
 a=s.index('static ',s.index(name)-30) if False else None
 import re
 m=re.search(r'static [^\n]+\b'+name+r'\([^\n]+\) \{',s);assert m,name
 a=m.start();b=s.index('{',a);depth=1;i=b+1
 masked=re.sub(r'"(?:\\.|[^"\\])*"',lambda m:' '*len(m.group()),s)
 while depth:
  if masked[i]=='{':depth+=1
  elif masked[i]=='}':depth-=1
  i+=1
 return s[a:i]+'\n'
head=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef int err_t;typedef unsigned uint;typedef uint16_t u16_t;
#define ERR_OK 0
#define ERR_ABRT -13
#define TCP_WRITE_FLAG_COPY 1
#define TCP_WRITE_FLAG_MORE 2
#define MIN(a,b) ((a)<(b)?(a):(b))
#define STAGE_OFFSET 0
struct tcp_pcb {int alive;};struct pbuf {void *payload;unsigned len,tot_len;struct pbuf *next;};
typedef void api_cmd_t;
static int close_result,freed,aborted,acked;
#define tcp_arg(p,a) ((void)0)
#define tcp_recv(p,a) ((void)0)
#define tcp_sent(p,a) ((void)0)
#define tcp_err(p,a) ((void)0)
#define tcp_poll(p,a,b) ((void)0)
static int tcp_close(struct tcp_pcb *p){assert(p->alive);return close_result;}
static void tcp_abort(struct tcp_pcb *p){assert(p->alive);p->alive=0;aborted++;}
static unsigned tcp_sndbuf(struct tcp_pcb *p){assert(p->alive);return 65535;}
static int tcp_write(struct tcp_pcb *p,const void *b,unsigned n,int flags){assert(p->alive);return 0;}
static void tcp_output(struct tcp_pcb *p){assert(p->alive);}
static void tcp_recved(struct tcp_pcb *p,unsigned n){assert(p->alive);acked+=n;}
static void pbuf_free(struct pbuf *p){}
static bool app_install_data(const uint8_t *p,unsigned n){return true;}
static void app_install_abort(const char *s){}
static uint8_t staging[32];static uint8_t *drive0_staging(){return staging;}
'''
a=s.index('enum { C_HEAD');b=s.index('#define MAX_CONNS',a)
parts=[head,s[a:b], 'static void conn_free(conn_t *c){free(c->out);free(c);freed++;}\n']
parts += [r"""
#define LIST_CHUNK 1024
static unsigned list_index;
static void *board_sd_discs_begin(void){list_index=0;return (void*)1;}
static bool board_sd_discs_next(void *d,char *name,unsigned size,uint32_t *bytes){
 if(list_index==75)return false;snprintf(name,size,"disk%u.d64",list_index++);*bytes=174848;return true;
}
static void board_sd_discs_end(void *d){}
static void api_json_str(char *out,unsigned size,const char *s){snprintf(out,size,"\"%s\"",s);}
""",fn('list_refill')]
parts += [fn(n) for n in ['conn_close','send_more','respond_disc_list','respond','respond_json','respond_error']]
parts+=['static void route(conn_t *c){respond_json(c,200,"{}");}\nstatic void body_done(conn_t *c){respond_json(c,200,"{}");}\n']
parts +=[fn(n) for n in ['on_recv','on_sent','on_poll']]
parts +=[r'''
int main(){
 for(int fail=0;fail<2;fail++){
  struct tcp_pcb pcb={1};conn_t *c=calloc(1,sizeof *c);c->pcb=&pcb;
  char request[]="GET / HTTP/1.0\r\n\r\nTRAILING";
  struct pbuf tail={request,3,3,NULL},buf={request,sizeof(request)-1,sizeof(request)+2,&tail};
  close_result=fail;int before=freed;
  assert(on_recv(c,&pcb,&buf,0)==ERR_OK);assert(freed==before);assert(c->state==C_DONE);
  assert(on_sent(c,&pcb,2)==(fail?ERR_ABRT:ERR_OK));assert(freed==before+1);
  pcb.alive=1;c=calloc(1,sizeof *c);c->pcb=&pcb;c->state=C_DONE;
  assert(on_poll(c,&pcb)==(fail?ERR_ABRT:ERR_OK));
  pcb.alive=1;c=calloc(1,sizeof *c);c->pcb=&pcb;
  assert(on_recv(c,&pcb,NULL,0)==(fail?ERR_ABRT:ERR_OK));
 }
 struct tcp_pcb pcb={1};conn_t *c=calloc(1,sizeof *c);c->pcb=&pcb;close_result=0;
 int before=freed;respond_disc_list(c);assert(list_index==75);assert(!c->listing);assert(c->state==C_DONE);assert(freed==before);
 assert(on_sent(c,&pcb,1)==ERR_OK);assert(freed==before+1);
 puts("HTTP receive lifetime and close/abort callbacks passed");
}
''']
Path('/tmp/http-lifetime.c').write_text('\n'.join(parts))
