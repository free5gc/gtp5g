#ifndef __UTIL_H__
#define __UTIL_H__

#include <linux/skbuff.h>

void ip_string(char *, int);
u32 get_skb_routing_mark(struct sk_buff *skb);

#endif // __UTIL_H__
