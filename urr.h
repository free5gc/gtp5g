#ifndef __URR_H__
#define __URR_H__

#include <linux/kernel.h>
#include <linux/rculist.h>
#include <linux/net.h>

#include "dev.h"

#define SEID_U32ID_HEX_STR_LEN 24
// Measurement Method
#define URR_METHOD_DURAT 0x1 // not use temporarily
#define URR_METHOD_VOLUM 0x2 
#define URR_METHOD_EVENT 0x4 // not use temporarily

// GTP5G_URR_VOLUME_QUOTA_FLAGS 8.2.50
#define URR_VOLUME_QUOTA_TOVOL 0x1
#define URR_VOLUME_QUOTA_ULVOL 0x2
#define URR_VOLUME_QUOTA_DLVOL 0x4

// GTP5G_URR_VOLUME_THRESHOLD_FLAGS 8.2.13
#define URR_VOLUME_THRESHOLD_TOVOL 0x1
#define URR_VOLUME_THRESHOLD_ULVOL 0x2
#define URR_VOLUME_THRESHOLD_DLVOL 0x4

// Measurement Information
#define URR_INFO_MBQE   01
#define URR_INFO_INAM   02
#define URR_INFO_RADI   04
#define URR_INFO_ISTM   010
#define URR_INFO_MNOP   020
#define URR_INFO_SSPOC  040
#define URR_INFO_ASPOC  0100
#define URR_INFO_CIAM   0200



struct VolumeThreshold{        
    uint8_t flag;

    uint64_t totalVolume;
    uint64_t uplinkVolume;
    uint64_t downlinkVolume;
}; 

struct VolumeQuota{        
    uint8_t flag;

    uint64_t totalVolume;
    uint64_t uplinkVolume;
    uint64_t downlinkVolume;
}; 

struct urr {
    struct hlist_node hlist_id;
    u64 seid;
    u32 id;
    uint64_t method;
    uint64_t trigger;
    uint64_t period;
    uint64_t info;
    uint64_t seq;

// threasholds after adjusting
    u64 threshold_tovol;
    u64 threshold_uvol;
    u64 threshold_dvol;

    struct VolumeThreshold *volumethreshold; 
    struct VolumeQuota *volumequota;      

    // For usage report time info
    ktime_t  time_of_fst_pkt;
    ktime_t  time_of_lst_pkt;
    ktime_t  start_time;
    ktime_t  end_time;

    struct net_device *dev;
    struct rcu_head rcu_head;
};

extern void urr_context_delete(struct urr *);
extern struct urr *find_urr_by_id(struct gtp5g_dev *, u64, u32);
extern void urr_update(struct urr *, struct gtp5g_dev *);
extern void urr_append(u64, u32, struct urr *, struct gtp5g_dev *);
extern int urr_get_pdr_ids(u16 *, int, struct urr *, struct gtp5g_dev *);
extern void urr_set_pdr(u64, u32, struct hlist_node *, struct gtp5g_dev *);
void seid_urr_id_to_hex_str(u64 seid_int, u32 urr_id, char *buff);
#endif // __URR_H__
