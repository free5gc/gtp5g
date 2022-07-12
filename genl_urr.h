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

    GTP5G_URR_VOLUME_THRESHOLD,
	GTP5G_URR_VOLUME_QUOTA,

    __GTP5G_URR_ATTR_MAX,
};
#define GTP5G_URR_ATTR_MAX (__GTP5G_URR_ATTR_MAX - 1)

/* Nest in GTP5G_URR_VOL_THRESHOLD */
enum gtp5g_urr_volume_threshold_attrs {
    GTP5G_URR_VOLUME_THRESHOLD_FLAG = 1,

    GTP5G_URR_VOLUME_THRESHOLD_TOVOL_HIGH32,
    GTP5G_URR_VOLUME_THRESHOLD_TOVOL_LOW32,
    GTP5G_URR_VOLUME_THRESHOLD_UVOL_HIGH32,
    GTP5G_URR_VOLUME_THRESHOLD_UVOL_LOW32,
    GTP5G_URR_VOLUME_THRESHOLD_DVOL_HIGH32,
    GTP5G_URR_VOLUME_THRESHOLD_DVOL_LOW32,

    __GTP5G_URR_VOLUME_THRESHOLD_ATTR_MAX,
};
#define GTP5G_URR_VOLUME_THRESHOLD_ATTR_MAX (__GTP5G_URR_VOLUME_THRESHOLD_ATTR_MAX - 1)

/* Nest in GTP5G_URR_VOL_QUOTA */
enum gtp5g_urr_volume_quota_attrs {
    GTP5G_URR_VOLUME_QUOTA_FLAG = 1,

    GTP5G_URR_VOLUME_QUOTA_TOVOL_HIGH32,
    GTP5G_URR_VOLUME_QUOTA_TOVOL_LOW32,
    GTP5G_URR_VOLUME_QUOTA_UVOL_HIGH32,
    GTP5G_URR_VOLUME_QUOTA_UVOL_LOW32,
    GTP5G_URR_VOLUME_QUOTA_DVOL_HIGH32,
    GTP5G_URR_VOLUME_QUOTA_DVOL_LOW32,

    __GTP5G_URR_VOLUME_QUOTA_ATTR_MAX,
};
#define GTP5G_URR_VOLUME_QUOTA_ATTR_MAX (__GTP5G_URR_VOLUME_QUOTA_ATTR_MAX - 1)

struct user_report {
	uint32_t urrid; 					/* 8.2.54 URR_ID */
    uint32_t uRSEQN;
    uint32_t queryUrrReference;         

    struct {
        u8 perio; 
        u8 volth; 
        u8 timth;
        u8 quhti; 
        u8 start; 
        u8 stopt; 
        u8 droth; 
        u8 immer; 
        u8 volqu; 
        u8 timqu; 
        u8 liusa; 
        u8 termr; 
        u8 monit; 
        u8 envcl; 
        u8 macar; 
        u8 eveth; 
        u8 evequ; 
        u8 tebur; 
        u8 ipmjl; 
        u8 quvti; 
        u8 emrre; 
    }UsageReportTrigger;

    struct{
        u8 flag;
        u64 totalVolume;
        u64 uplinkVolume;
        u64 downlinkVolume;
        u64 totalPktNum;
        u64 uplinkPktNum;
        u64 downlinkPktNum;
    } volumeMeasurement;

    u64 start_time;
    u64 end_time;
    u64 time_of_fst_pkt;
    u64 time_of_lst_pkt;

} __attribute__((packed));


extern int gtp5g_genl_add_urr(struct sk_buff *, struct genl_info *);
extern int gtp5g_genl_del_urr(struct sk_buff *, struct genl_info *);
extern int gtp5g_genl_get_urr(struct sk_buff *, struct genl_info *);
extern int gtp5g_genl_dump_urr(struct sk_buff *, struct netlink_callback *);

#endif // __GENL_URR_H__