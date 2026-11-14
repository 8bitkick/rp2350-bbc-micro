#include <assert.h>
#include <stdio.h>
#include "nimbus_reconnect.h"
int main(void){
 uint32_t deadline=nimbus_resume_deadline(false,100,0);
 assert(deadline==3100);
 for(uint32_t now=100;now<3100;now+=10){
   deadline=nimbus_resume_deadline(true,now,deadline);
   assert(deadline==3100);
   assert(!nimbus_retry_due(true,true,false,now,deadline));
 }
 assert(nimbus_retry_due(true,true,false,3100,deadline));
 assert(!nimbus_retry_due(true,true,true,9999,deadline));
 assert(!nimbus_retry_due(true,false,false,9999,deadline));
 assert(!nimbus_retry_due(false,true,false,9999,deadline));
 deadline=nimbus_resume_deadline(false,UINT32_MAX-100,0);
 assert(!nimbus_retry_due(true,true,false,UINT32_MAX-10,deadline));
 assert(nimbus_retry_due(true,true,false,deadline,deadline));
 unsigned streak=0;
 const uint32_t delays[]={3000,10000,30000,60000,60000,60000};
 for(unsigned i=0;i<6;i++){
   uint32_t now=100000+i*1000;
   deadline=nimbus_failed_deadline(&streak,now);
   assert(deadline-now==delays[i]);assert(streak<=4);
   assert(!nimbus_retry_due(true,true,false,deadline-1,deadline));
   assert(nimbus_retry_due(true,true,false,deadline,deadline));
 }
 // A click shortens a long backoff, but further clicks cannot postpone it.
 deadline=nimbus_manual_deadline(true,false,&streak,100,60000);
 assert(streak==0&&deadline==3100);
 for(unsigned now=101;now<3100;now++){
   deadline=nimbus_manual_deadline(true,false,&streak,now,deadline);
   assert(deadline==3100);
 }
 // A connection or scan in flight is never restarted by a manual click.
 streak=4;assert(nimbus_manual_deadline(true,true,&streak,100,9000)==9000);assert(streak==4);
 deadline=nimbus_manual_deadline(false,false,&streak,200,9000);
 assert(streak==0&&deadline==3200);
 streak=3;uint32_t now=UINT32_MAX-100;
 deadline=nimbus_failed_deadline(&streak,now);assert(deadline-now==60000);
 assert(!nimbus_retry_due(true,true,false,now+59999,deadline));
 assert(nimbus_retry_due(true,true,false,now+60000,deadline));
 puts("Progressive retry delays, cap, manual recovery, busy guard and clock wrap passed");
 puts("Reconnect listening window, repeated clicks, busy guard and clock wrap passed");
}
