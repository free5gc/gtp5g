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

#include "net.h"
#include "log.h"
#include "pdr.h"
#include "urr.h"

static int gtp5g_genl_fill_usage_report(struct sk_buff *, u32, u32, u32, struct urr *);
static int gtp5g_genl_fill_volume_measurement(struct sk_buff *skb, struct urr *urr);

int gtp5g_genl_get_usage_report(struct sk_buff *skb, struct genl_info *info)
{

    struct gtp5g_dev *gtp;
    struct urr *urr;
    int ifindex;
    int netnsfd;
    u64 seid;
    u32 urr_id;
    struct sk_buff *skb_ack;
    int err;

    if (!info->attrs[GTP5G_LINK])
        return -EINVAL;
    ifindex = nla_get_u32(info->attrs[GTP5G_LINK]);

    if (info->attrs[GTP5G_NET_NS_FD])
        netnsfd = nla_get_u32(info->attrs[GTP5G_NET_NS_FD]);
    else
        netnsfd = -1;

    rcu_read_lock();

    gtp = gtp5g_find_dev(sock_net(skb->sk), ifindex, netnsfd);
    if (!gtp) {
        rcu_read_unlock();
        return -ENODEV;
    }

    if (info->attrs[GTP5G_URR_SEID]) {
        seid = nla_get_u64(info->attrs[GTP5G_URR_SEID]);
    } else {
        seid = 0;
    }

    if (info->attrs[GTP5G_URR_ID]) {
        urr_id = nla_get_u32(info->attrs[GTP5G_URR_ID]);
    } else {
        rcu_read_unlock();
        return -ENODEV;
    }

    urr = find_urr_by_id(gtp, seid, urr_id);
    if (!urr) {
        rcu_read_unlock();
        return -ENOENT;
    }

    skb_ack = genlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC);
    if (!skb_ack) {
        rcu_read_unlock();
        return -ENOMEM;
    }

    err = gtp5g_genl_fill_usage_report(skb_ack,
            NETLINK_CB(skb).portid,
            info->snd_seq,
            info->nlhdr->nlmsg_type,
            urr);
    if (err) {
        kfree_skb(skb_ack);
        rcu_read_unlock();
        return err;
    }
    rcu_read_unlock();

    return genlmsg_unicast(genl_info_net(info), skb_ack, info->snd_portid); 
}


static int gtp5g_genl_fill_volume_measurement(struct sk_buff *skb, struct urr *urr)
{
    struct nlattr *nest_volume_measurement;
    struct hlist_head *head;
    struct pdr *pdr;
    char seid_urr_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};
    struct gtp5g_dev *gtp = netdev_priv(urr->dev);
    u8 flag;
    
    seid_urr_id_to_hex_str(urr->seid, urr->id, seid_urr_id_hexstr);
    head = &gtp->related_urr_hash[str_hashfn(seid_urr_id_hexstr) % gtp->hash_size];       

    hlist_for_each_entry_rcu(pdr, head, hlist_related_urr) {
        if (*pdr->urr_id == urr->id){
            break; // found the pdr
        }
    }
    
    // flags are control by GTP5G
    flag = REPORT_VOLUME_MEASUREMENT_TOVOL | REPORT_VOLUME_MEASUREMENT_UVOL | REPORT_VOLUME_MEASUREMENT_DVOL;
    if (urr->info & URR_INFO_MNOP)
        flag |= (REPORT_VOLUME_MEASUREMENT_TONOL | REPORT_VOLUME_MEASUREMENT_UNOP | REPORT_VOLUME_MEASUREMENT_DNOP);

    nest_volume_measurement = nla_nest_start(skb, GTP5G_UR_VOLUME_MEASUREMENT);
    if (!nest_volume_measurement)
        return -EMSGSIZE;

    if (nla_put_u8(skb, GTP5G_UR_VOLUME_MEASUREMENT_FLAGS, flag))
        return -EMSGSIZE;
    if (nla_put_u64_64bit(skb, GTP5G_UR_VOLUME_MEASUREMENT_TOVOL, pdr->ul_pkt_cnt + pdr->ul_pkt_cnt, 0))
        return -EMSGSIZE;
    if (nla_put_u64_64bit(skb, GTP5G_UR_VOLUME_MEASUREMENT_UVOL, pdr->ul_pkt_cnt, 0))
        return -EMSGSIZE;
    if (nla_put_u64_64bit(skb, GTP5G_UR_VOLUME_MEASUREMENT_DPACKET, pdr->dl_byte_cnt, 0))
        return -EMSGSIZE;
    if (nla_put_u64_64bit(skb, GTP5G_UR_VOLUME_MEASUREMENT_TOPACKET, pdr->ul_pkt_cnt + pdr->dl_pkt_cnt, 0))
        return -EMSGSIZE;
    if (nla_put_u64_64bit(skb, GTP5G_UR_VOLUME_MEASUREMENT_UPACKET, pdr->ul_pkt_cnt, 0))
        return -EMSGSIZE;
    if (nla_put_u64_64bit(skb, GTP5G_UR_VOLUME_MEASUREMENT_DPACKET, pdr->dl_pkt_cnt, 0))
        return -EMSGSIZE;

    GTP5G_LOG(NULL,"flag %d\n",flag);
    GTP5G_LOG(NULL,"GTP5G_UR_VOLUME_MEASUREMENT_TOVOL %lld\n",pdr->ul_pkt_cnt + pdr->ul_pkt_cnt);

    pdr->ul_pkt_cnt = 0;
    pdr->dl_byte_cnt = 0;
    pdr->ul_pkt_cnt = 0;
    pdr->dl_pkt_cnt = 0;

    nla_nest_end(skb, nest_volume_measurement);
    return 0;
}

static int gtp5g_genl_fill_usage_report(struct sk_buff *skb, u32 snd_portid, u32 snd_seq,
    u32 type, struct urr *urr)
{
    void *genlh;
    genlh = genlmsg_put(skb, snd_portid, snd_seq,
            &gtp5g_genl_family, 0, type);
    if (!genlh)
        goto genlmsg_fail;

    /* urrid, usagereporttrigger, queryurrreference*/
    if (nla_put_u32(skb, GTP5G_UR_URRID, urr->id))
        goto genlmsg_fail;
    if (nla_put_u32(skb, GTP5G_UR_URSEQN, 0))
        goto genlmsg_fail;
    if(nla_put_u32(skb, GTP5G_UR_USAGE_REPORT_TRIGGER, REPORT_TRIGGER_IMMER))
        goto genlmsg_fail;
    if(nla_put_u32(skb, GTP5G_UR_QUERY_URR_REFERENCE, 0))
        goto genlmsg_fail;
    // if(gtp5g_genl_fill_volume_measurement(skb,urr))
    //     goto genlmsg_fail;

    genlmsg_end(skb, genlh);
    return 0;

genlmsg_fail:
    genlmsg_cancel(skb, genlh);
    return -EMSGSIZE;
}