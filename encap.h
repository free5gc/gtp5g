#ifndef ENCAP_H__
#define ENCAP_H__

#include <linux/socket.h>

#include "dev.h"
#include "pktinfo.h"

extern struct sock *gtp5g_encap_enable(int, int, struct gtp5g_dev *);
extern void gtp5g_encap_disable(struct sock *);
extern int gtp5g_handle_skb_ipv4(struct sk_buff *, struct net_device *,
        struct gtp5g_pktinfo *);

#endif