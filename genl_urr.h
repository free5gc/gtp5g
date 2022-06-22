#ifndef __GENL_URR_H__
#define __GENL_URR_H__

#include "genl.h"

enum gtp5g_urr_attrs {
    GTP5G_URR_ID = 3,
    GTP5G_URR_MEASUREMENT_METHOD,
    GTP5G_URR_REPORTING_TRIGGER,
    GTP5G_URR_MEASUREMENT_PERIOD,
    GTP5G_URR_MEASUREMENT_INFO,
    GTP5G_URR_SEQ, // 3GPP TS 29.244 table 7.5.8.3-1 UR-SEQN
    GTP5G_URR_SEID,

    __GTP5G_URR_ATTR_MAX,
};
#define GTP5G_URR_ATTR_MAX (__GTP5G_URR_ATTR_MAX - 1)

struct user_report {
    u32 flag;
    u64 totalVolume;
    u64 uplinkVolume;
    u64 downlinkVolume;
    u64 totalPktNum;
    u64 uplinkPktNum;
    u64 downlinkPktNum;
} __attribute__((packed));

extern int gtp5g_genl_add_urr(struct sk_buff *, struct genl_info *);
extern int gtp5g_genl_del_urr(struct sk_buff *, struct genl_info *);
extern int gtp5g_genl_get_urr(struct sk_buff *, struct genl_info *);
extern int gtp5g_genl_dump_urr(struct sk_buff *, struct netlink_callback *);

#endif // __GENL_URR_H__