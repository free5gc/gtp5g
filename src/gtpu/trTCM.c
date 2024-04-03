#include <linux/time.h>
#include <linux/slab.h>
#include <linux/random.h>

#include "trTCM.h"
#include "log.h"

TrafficPolicer* newTrafficPolicer(u64 bitRate) {
    TrafficPolicer* p = (TrafficPolicer*)kmalloc(sizeof(TrafficPolicer), GFP_KERNEL);
    if (p == NULL) {
        GTP5G_ERR(NULL, "traffic policer memory allocation error\n");
        return NULL;
    }
    
    spin_lock_init(&p->lock);
    
    p->byteRate = bitRate * 125 ; // Kbit/s to byte/s (*1000/8)

    // 1ms as burst size
    p->cbs = p->byteRate / 125; // bytes
    p->ebs = p->cbs * 4; // bytes

    // fill buckets at the begining
    p->tc = p->cbs; 
    p->te = p->ebs; 

    p->lastUpdate = ktime_get_ns();

    p->refillTokenTime = 0;

    return p;
} 

Color policePacket(TrafficPolicer* p, int pktLen) {
    u64 tokensToAdd = 0;
    u64 tc, te = 0;
    u64 elapsed = 0;
    u64 now = ktime_get_ns();

    spin_lock(&p->lock); 

    elapsed = now - p->lastUpdate;
    p->lastUpdate = now;

    // the total time elapsed since the last token refill
    p->refillTokenTime = p->refillTokenTime + elapsed;

    #define REFILL_TOKEN_INTERVAL 1000000 // ns (=1ms), 1 token = 1 byte
    #define SECOND_TO_NANOSECOND 1000000000
    if (p->refillTokenTime >= REFILL_TOKEN_INTERVAL) {
        // add at least one token
        p->refillTokenTime = p->refillTokenTime - REFILL_TOKEN_INTERVAL;
        tokensToAdd = p->byteRate * (REFILL_TOKEN_INTERVAL  / SECOND_TO_NANOSECOND); 
    } else {
        tokensToAdd = 0;
    }
 
    tc = p->tc + tokensToAdd;
    te = p->te;
    if (tc > p->cbs) {
        te += (tc - p->cbs);
        if (te > p->ebs){
            te = p->ebs;
        }
        tc = p->cbs; 
    }
   
    if (p->tc >= pktLen) {
        p->tc = tc - pktLen;
        p->te = te;
        spin_unlock(&p->lock); 
        return Green;
    }

    if (p->te >= pktLen) {
        p->tc = tc;
        p->te = te - pktLen;
        spin_unlock(&p->lock); 
        return Yellow;
    }

    p->tc = tc;
    p->te = te;

    spin_unlock(&p->lock); 
    return Red;
}