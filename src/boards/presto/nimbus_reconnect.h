#pragma once
#include <stdbool.h>
#include <stdint.h>
#define RECONNECT_LISTEN_MS 3000u
static inline uint32_t nimbus_resume_deadline(bool enabled,uint32_t now,uint32_t deadline) {
    return enabled?deadline:now+RECONNECT_LISTEN_MS;
}
static inline bool nimbus_retry_due(bool ready,bool enabled,bool busy,uint32_t now,uint32_t deadline) {
    return ready&&enabled&&!busy&&(int32_t)(now-deadline)>=0;
}

// Keep the first retry quick for machine switches, then leave the radio quiet
// for longer if the remembered controller remains off. Delays follow failures.
static inline uint32_t nimbus_failure_delay(unsigned failures) {
    return failures<=1?3000u:failures==2?10000u:failures==3?30000u:60000u;
}
static inline uint32_t nimbus_failed_deadline(unsigned *streak,uint32_t now) {
    if(*streak<4)(*streak)++;
    return now+nimbus_failure_delay(*streak);
}
static inline uint32_t nimbus_manual_deadline(bool enabled,bool busy,
        unsigned *streak,uint32_t now,uint32_t deadline) {
    if(busy)return deadline;
    if(!enabled || *streak>=2){*streak=0;return now+RECONNECT_LISTEN_MS;}
    return deadline;
}
