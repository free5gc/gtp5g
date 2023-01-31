#include <net/genetlink.h>
#include <net/sock.h>
#include <linux/rculist.h>
#include <net/netns/generic.h>

#include "dev.h"
#include "genl.h"
#include "report.h"
#include "genl_report.h"
#include "genl_urr.h"
#include "hash.h"
#include "common.h"

#include "net.h"
#include "log.h"
#include "pdr.h"
#include "urr.h"

static int gtp5g_genl_fill_volume_measurement(struct sk_buff *, struct urr *);
static int parse_sess_urr(struct sess_urrs *, struct nlattr *);

int gtp5g_genl_get_usage_report(struct sk_buff *skb, struct genl_info *info)
{
    struct gtp5g_dev *gtp;
    struct urr *urr;
    int ifindex;
    int netnsfd;
    u32 urr_num, i;
    struct sk_buff *skb_ack;
    int err;
    struct nlattr *hdr = nlmsg_attrdata(info->nlhdr, 0);
    int remaining = nlmsg_attrlen(info->nlhdr, 0);
    struct sess_urrs *sessurrs;
    struct urr **urrs;

    if (!info->attrs[GTP5G_LINK])
        return -EINVAL;
    ifindex = nla_get_u32(info->attrs[GTP5G_LINK]);

    if (info->attrs[GTP5G_NET_NS_FD])
        netnsfd = nla_get_u32(info->attrs[GTP5G_NET_NS_FD]);
    else
        netnsfd = -1;

    if (!info->attrs[GTP5G_URR_NUM]) 
        return -EINVAL;
    urr_num = nla_get_u32(info->attrs[GTP5G_URR_NUM]);

    rcu_read_lock();
    sessurrs = kzalloc(urr_num * sizeof(struct sess_urrs), GFP_KERNEL);

    if (info->attrs[GTP5G_SESS_URRS]) {
        err =  parse_sess_urr(&sessurrs[i++], info->attrs[GTP5G_SESS_URRS]);
        if (err){
            kfree(sessurrs);
            return err;
        }
    }

    hdr = nla_next(hdr, &remaining);
    while (nla_ok(hdr,remaining)) {
        switch (nla_type(hdr)) {
        case GTP5G_SESS_URRS:
            err = parse_sess_urr(&sessurrs[i++], hdr);
            if (err) {
                kfree(sessurrs);
                rcu_read_unlock();
                return err;
            }
            break;
        }
        hdr = nla_next(hdr, &remaining);
    }

    gtp = gtp5g_find_dev(sock_net(skb->sk), ifindex, netnsfd);
    if (!gtp) {
        rcu_read_unlock();
        return -ENODEV;
    }

    urrs = kzalloc(sizeof(struct urr *) * urr_num , GFP_KERNEL);
    for (i = 0; i < urr_num; i++) {
        urr = find_urr_by_id(gtp, sessurrs[i].seid, sessurrs[i].urrid);
        if (!urr) {
            kfree(sessurrs);
            kfree(urrs);
            rcu_read_unlock();
            return -ENOENT;
        }

        skb_ack = genlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC);
        if (!skb_ack) {
            rcu_read_unlock();
            return -ENOMEM;
        }

        urr->end_time = ktime_get_real();
        urrs[i] = urr;
    }

    err = gtp5g_genl_fill_usage_reports(skb_ack,
    NETLINK_CB(skb).portid,
    info->snd_seq,
    info->nlhdr->nlmsg_type,
    urrs, urr_num);
        
    
    for (i = 0; i < urr_num; i++){
        urrs[i]->start_time = ktime_get_real();
        if (err) {
            kfree(sessurrs);
            kfree(urrs); 
            kfree_skb(skb_ack);
            rcu_read_unlock();
            return err;
        }
    }
    kfree(sessurrs);
    kfree(urrs);
    rcu_read_unlock();

    return genlmsg_unicast(genl_info_net(info), skb_ack, info->snd_portid); 
}

static int gtp5g_genl_fill_volume_measurement(struct sk_buff *skb, struct urr *urr)
{
    struct nlattr *nest_volume_measurement;

    nest_volume_measurement = nla_nest_start(skb, GTP5G_UR_VOLUME_MEASUREMENT);
    if (!nest_volume_measurement)
        return -EMSGSIZE;
    if (nla_put_u64_64bit(skb, GTP5G_UR_VOLUME_MEASUREMENT_TOVOL, urr->bytes.totalVolume , 0))
        return -EMSGSIZE;
    if (nla_put_u64_64bit(skb, GTP5G_UR_VOLUME_MEASUREMENT_UVOL, urr->bytes.uplinkVolume, 0))
        return -EMSGSIZE;
    if (nla_put_u64_64bit(skb, GTP5G_UR_VOLUME_MEASUREMENT_DVOL, urr->bytes.downlinkVolume, 0))
        return -EMSGSIZE;
    if (nla_put_u64_64bit(skb, GTP5G_UR_VOLUME_MEASUREMENT_TOPACKET, urr->bytes.totalPktNum, 0))
        return -EMSGSIZE;
    if (nla_put_u64_64bit(skb, GTP5G_UR_VOLUME_MEASUREMENT_UPACKET, urr->bytes.uplinkPktNum, 0))
        return -EMSGSIZE;
    if (nla_put_u64_64bit(skb, GTP5G_UR_VOLUME_MEASUREMENT_DPACKET, urr->bytes.downlinkPktNum, 0))
        return -EMSGSIZE;
    nla_nest_end(skb, nest_volume_measurement);

    memset(&urr->bytes, 0, sizeof(struct VolumeMeasurement));

    return 0;
}

int gtp5g_genl_fill_ur(struct sk_buff *skb, struct urr *urr, u32 reporting_trigger)
{
    struct nlattr *nest_usage_report;

    nest_usage_report = nla_nest_start(skb, GTP5G_UR);
    if (!nest_usage_report)
        return -EMSGSIZE;

    if (nla_put_u32(skb, GTP5G_UR_URRID, urr->id))
        return -EMSGSIZE;
    if (nla_put_u64_64bit(skb, GTP5G_UR_SEID, urr->seid, 0))
        return -EMSGSIZE;
    if (nla_put_u64_64bit(skb, GTP5G_UR_START_TIME, urr->start_time, 0))
        return -EMSGSIZE;
    if (nla_put_u64_64bit(skb, GTP5G_UR_END_TIME, urr->end_time, 0))
        return -EMSGSIZE;
    if (gtp5g_genl_fill_volume_measurement(skb, urr))
    return -EMSGSIZE;     


    if (reporting_trigger > 0) {
        if (nla_put_u32(skb, GTP5G_UR_USAGE_REPORT_TRIGGER, reporting_trigger))
            return -EMSGSIZE;
    }

    nla_nest_end(skb, nest_usage_report);

    return 0;
}

int gtp5g_genl_fill_usage_reports(struct sk_buff *skb, u32 snd_portid, u32 snd_seq,
    u32 type, struct urr **urrs, u32 urr_num)
{
    int i;
    void *genlh;
    genlh = genlmsg_put(skb, snd_portid, snd_seq,
            &gtp5g_genl_family, 0, type);
    if (!genlh)
        goto genlmsg_fail;

    for (i = 0; i < urr_num; i++) {
        gtp5g_genl_fill_ur(skb, urrs[i], 0);
    }

    genlmsg_end(skb, genlh);

    return 0;

genlmsg_fail:
    genlmsg_cancel(skb, genlh);
    return -EMSGSIZE;
}

static int parse_sess_urr(struct sess_urrs *sess_urr ,struct nlattr *a){
    struct nlattr *attrs[GTP5G_UR_ATTR_MAX + 1];
    int err;

    if (!sess_urr) {
        sess_urr= kzalloc(sizeof(sess_urr), GFP_ATOMIC);
        if (!sess_urr)
            return -ENOMEM;
    }

    err = nla_parse_nested(attrs, GTP5G_UR_ATTR_MAX, a, NULL, NULL);
    if (err)
        return err;
    
    if (attrs[GTP5G_URR_ID]) {
        sess_urr->urrid = nla_get_u32(attrs[GTP5G_URR_ID]);
    }

    if (attrs[GTP5G_URR_SEID]) {
        sess_urr->seid = nla_get_u64(attrs[GTP5G_URR_SEID]);
    }

    return 0;
}