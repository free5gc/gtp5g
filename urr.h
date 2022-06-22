#ifndef __URR_H__
#define __URR_H__

#include <linux/kernel.h>
#include <linux/rculist.h>
#include <linux/net.h>

#include "dev.h"

#define SEID_U32ID_HEX_STR_LEN 24

struct urr {
    struct hlist_node hlist_id;
    u64 seid;
    u32 id;
    uint64_t method;
    uint64_t trigger;
    uint64_t period;
    uint64_t info;
    uint64_t seq;
    struct net_device *dev;
    struct rcu_head rcu_head;
};

extern void urr_context_delete(struct urr *);
extern struct urr *find_urr_by_id(struct gtp5g_dev *, u64, u32);
extern void urr_update(struct urr *, struct gtp5g_dev *);
extern void urr_append(u64, u32, struct urr *, struct gtp5g_dev *);
extern int urr_get_pdr_ids(u16 *, int, struct urr *, struct gtp5g_dev *);
extern void urr_set_pdr(u64, u32, struct hlist_node *, struct gtp5g_dev *);

#endif // __URR_H__
