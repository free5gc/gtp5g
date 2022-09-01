#ifndef __GENL_REPORT_H__
#define __GENL_REPORT_H__

#include "genl.h"
#include "urr.h"
#include "pdr.h"

// VOLUME MEASUREMENT
#define REPORT_VOLUME_MEASUREMENT_TOVOL 0x1
#define REPORT_VOLUME_MEASUREMENT_UVOL  0x2
#define REPORT_VOLUME_MEASUREMENT_DVOL  0x4
#define REPORT_VOLUME_MEASUREMENT_TONOL 0x8
#define REPORT_VOLUME_MEASUREMENT_UNOP  0x10
#define REPORT_VOLUME_MEASUREMENT_DNOP  0x20


/* Nest in GTP5G_UR_VOLUME_MEASUREMENT */
enum gtp5g_usage_report_volume_measurement_attrs {
    GTP5G_UR_VOLUME_MEASUREMENT_FLAGS = 1,

    GTP5G_UR_VOLUME_MEASUREMENT_TOVOL,
    GTP5G_UR_VOLUME_MEASUREMENT_UVOL,
    GTP5G_UR_VOLUME_MEASUREMENT_DVOL,

    GTP5G_UR_VOLUME_MEASUREMENT_TOPACKET,
    GTP5G_UR_VOLUME_MEASUREMENT_UPACKET,
    GTP5G_UR_VOLUME_MEASUREMENT_DPACKET,

    __GTP5G_UR_VOLUME_MEASUREMENT_ATTR_MAX,
};
#define GTP5G_UR_VOLUME_MEASUREMENT_ATTR_MAX (__GTP5G_UR_VOLUME_MEASUREMENT_ATTR_MAX - 1)

enum gtp5g_usage_report_attrs {
    GTP5G_UR_URRID = 3,
    GTP5G_UR_USAGE_REPORT_TRIGGER,
    GTP5G_UR_URSEQN,
    GTP5G_UR_VOLUME_MEASUREMENT,
    GTP5G_UR_QUERY_URR_REFERENCE,

    __GTP5G_UR_ATTR_MAX,
};
#define GTP5G_UR_ATTR_MAX (__GTP5G_UR_ATTR_MAX - 1)

extern int gtp5g_genl_get_usage_report(struct sk_buff *, struct genl_info *);
extern int gtp5g_genl_fill_usage_report(struct sk_buff *skb, u32 snd_portid, u32 snd_seq,u32 type, struct urr *urr);

extern void resetPDRCnt(struct pdr *pdr);
extern void resetURR(struct urr *urr);

#endif // __GENL_URR_H__