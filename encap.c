#include <linux/version.h>
#include <linux/socket.h>
#include <linux/rculist.h>
#include <linux/udp.h>
#include <linux/gtp.h>

#include <net/ip.h>
#include <net/udp.h>
#include <net/udp_tunnel.h>

#include "dev.h"
#include "link.h"
#include "encap.h"
#include "gtp.h"
#include "pdr.h"
#include "far.h"
#include "qer.h"
#include "urr.h"

#include "genl.h"
#include "genl_urr.h"

#include "log.h"
#include "api_version.h"
#include "pktinfo.h"

/* used to compatible with api with/without seid */
#define MSG_URR_BAR_KOV_LEN 4
#define MSG_SEID_KOV_LEN 3
#define MSG_NO_SEID_KOV_LEN 2

enum msg_type {
    TYPE_BUFFER = 1,
    TYPE_URR_REPORT,
    TYPE_BAR_INFO,
};

static void gtp5g_encap_disable_locked(struct sock *);
static int gtp5g_encap_recv(struct sock *, struct sk_buff *);
static int gtp1u_udp_encap_recv(struct gtp5g_dev *, struct sk_buff *);
static int gtp5g_rx(struct pdr *, struct sk_buff *, unsigned int, unsigned int);
static int gtp5g_fwd_skb_encap(struct sk_buff *, struct net_device *,
        unsigned int, struct pdr *);
static int unix_sock_send(struct pdr *, void *, u32);
static int gtp5g_fwd_skb_ipv4(struct sk_buff *, 
    struct net_device *, struct gtp5g_pktinfo *, 
    struct pdr *);

struct sock *gtp5g_encap_enable(int fd, int type, struct gtp5g_dev *gtp){
    struct udp_tunnel_sock_cfg tuncfg = {NULL};
    struct socket *sock;
    struct sock *sk;
    int err;

    GTP5G_LOG(NULL, "enable gtp5g for the fd(%d) type(%d)\n", fd, type);

    sock = sockfd_lookup(fd, &err);
    if (!sock) {
        GTP5G_ERR(NULL, "Failed to find the socket fd(%d)\n", fd);
        return NULL;
    }

    if (sock->sk->sk_protocol != IPPROTO_UDP) {
        GTP5G_ERR(NULL, "socket fd(%d) is not a UDP\n", fd);
        sk = ERR_PTR(-EINVAL);
        goto out_sock;
    }

    lock_sock(sock->sk);
    if (sock->sk->sk_user_data) {
        GTP5G_ERR(NULL, "Failed to set sk_user_datat of socket fd(%d)\n", fd);
        sk = ERR_PTR(-EBUSY);
        goto out_sock;
    }

    sk = sock->sk;
    sock_hold(sk);

    tuncfg.sk_user_data = gtp;
    tuncfg.encap_type = type;
    tuncfg.encap_rcv = gtp5g_encap_recv;
    tuncfg.encap_destroy = gtp5g_encap_disable_locked;

    setup_udp_tunnel_sock(sock_net(sock->sk), sock, &tuncfg);

out_sock:
    release_sock(sock->sk);
    sockfd_put(sock);
    return sk;
}


void gtp5g_encap_disable(struct sock *sk)
{
    struct gtp5g_dev *gtp;

    if (!sk) {
        return;
    }

    lock_sock(sk);
    gtp = sk->sk_user_data;
    if (gtp) {
        gtp->sk1u = NULL;
        udp_sk(sk)->encap_type = 0;
        rcu_assign_sk_user_data(sk, NULL);
        sock_put(sk);
    }
    release_sock(sk);
}

static void gtp5g_encap_disable_locked(struct sock *sk)
{
    rtnl_lock();
    gtp5g_encap_disable(sk);
    rtnl_unlock();
}

static int gtp5g_encap_recv(struct sock *sk, struct sk_buff *skb)
{
    struct gtp5g_dev *gtp;
    int ret = 0;

    gtp = rcu_dereference_sk_user_data(sk);
    if (!gtp) {
        return 1;
    }

    switch (udp_sk(sk)->encap_type) {
    case UDP_ENCAP_GTP1U:
        ret = gtp1u_udp_encap_recv(gtp, skb);
        break;
    default:
        ret = -1; // Should not happen
    }

    switch (ret) {
    case 1:
        GTP5G_ERR(gtp->dev, "Pass up to the process\n");
        break;
    case 0:
        break;
    case -1:
        GTP5G_ERR(gtp->dev, "GTP packet has been dropped\n");
        kfree_skb(skb);
        ret = 0;
        break;
    }

    return ret;
}

static int gtp1c_handle_echo_req(struct sk_buff *skb, struct gtp5g_dev *gtp)
{
    struct gtpv1_hdr *req_gtp1;
    struct gtp1_hdr_opt *req_gtpOptHdr;

    struct gtpv1_echo_resp *gtp_pkt;

    struct rtable *rt;
    struct flowi4 fl4;
    struct iphdr *iph;

    __u8   flags = 0;
    __be16 seq_number = 0;

    req_gtp1 = (struct gtpv1_hdr *)(skb->data + sizeof(struct udphdr));

    flags = req_gtp1->flags;
    if (flags & GTPV1_HDR_FLG_SEQ){
         req_gtpOptHdr = (struct gtp1_hdr_opt *)(skb->data + sizeof(struct udphdr) 
                                                            + sizeof(struct gtpv1_hdr));
         seq_number = req_gtpOptHdr->seq_number;
    } else {
        GTP5G_ERR(gtp->dev, "GTP echo request shall bring sequence number\n");
        seq_number = 0;
    }

    pskb_pull(skb, skb->len);          

    gtp_pkt = skb_push(skb, sizeof(struct gtpv1_echo_resp));
    if (!gtp_pkt){
        GTP5G_ERR(gtp->dev, "can not construct GTP Echo Response\n");
        return 1;
    }
    memset(gtp_pkt, 0, sizeof(struct gtpv1_echo_resp));

    /* gtp header*/
    gtp_pkt->gtpv1_h.flags = GTPV1 | GTPV1_HDR_FLG_SEQ;
    gtp_pkt->gtpv1_h.type = GTPV1_MSG_TYPE_ECHO_RSP;
    gtp_pkt->gtpv1_h.length =
        htons(sizeof(struct gtpv1_echo_resp) - sizeof(struct gtpv1_hdr));
    gtp_pkt->gtpv1_h.tid = 0;

    /* gtp opt header*/
    gtp_pkt->gtpv1_opt_h.seq_number = seq_number;

    /* gtp recovery*/
    gtp_pkt->recov.type_num = GTPV1_IE_RECOVERY;
    gtp_pkt->recov.cnt = 0;

    iph = ip_hdr(skb);
  
    rt = ip4_find_route(skb, iph, gtp->sk1u, gtp->dev, 
        iph->daddr ,
        iph->saddr, 
        &fl4);
    if (IS_ERR(rt)) {
        GTP5G_ERR(gtp->dev, "no route for GTP echo response from %pI4\n", 
        &iph->saddr);
        return 1;
    }

    udp_tunnel_xmit_skb(rt, gtp->sk1u, skb,
                    fl4.saddr, fl4.daddr,
                    iph->tos,
                    ip4_dst_hoplimit(&rt->dst),
                    0,
                    htons(GTP1U_PORT), htons(GTP1U_PORT),
                    !net_eq(sock_net(gtp->sk1u),
                        dev_net(gtp->dev)),
                    false);

    return 0;
}

static int gtp1u_udp_encap_recv(struct gtp5g_dev *gtp, struct sk_buff *skb)
{
    unsigned int hdrlen = sizeof(struct udphdr) + sizeof(struct gtpv1_hdr);
    struct gtpv1_hdr *gtpv1;
    struct pdr *pdr;
    int gtpv1_hdr_len;

    if (!pskb_may_pull(skb, hdrlen)) {
        GTP5G_ERR(gtp->dev, "Failed to pull skb length %#x\n", hdrlen);
        return -1;
    }

    gtpv1 = (struct gtpv1_hdr *)(skb->data + sizeof(struct udphdr));
    if ((gtpv1->flags >> 5) != GTP_V1) {
        GTP5G_ERR(gtp->dev, "GTP version is not v1: %#x\n",
            gtpv1->flags);
        return 1;
    }

    if (gtpv1->type == GTPV1_MSG_TYPE_ECHO_REQ) {
        GTP5G_INF(gtp->dev, "GTP-C message type is GTP echo request: %#x\n",
            gtpv1->type);

        return gtp1c_handle_echo_req(skb, gtp);
    }

    if (gtpv1->type != GTPV1_MSG_TYPE_TPDU && gtpv1->type != GTPV1_MSG_TYPE_EMARK) {
        GTP5G_ERR(gtp->dev, "GTP-U message type is not a TPDU or End Marker: %#x\n",
            gtpv1->type);
        return 1;
    }

    gtpv1_hdr_len = get_gtpu_header_len(gtpv1, skb);
    // pskb_may_pull() may be called in get_gtpu_header_len(), so gtpv1 may be invalidated here.
    if (gtpv1_hdr_len < 0) {
        GTP5G_ERR(gtp->dev, "Invalid extension header length or else\n");
        return -1;
    }

    hdrlen = sizeof(struct udphdr) + gtpv1_hdr_len;
    if (!pskb_may_pull(skb, hdrlen)) {
        GTP5G_ERR(gtp->dev, "Failed to pull skb length %#x\n", hdrlen);
        return -1;
    }
    // pskb_may_pull() is called, so gtpv1 may be invalidated here.

    // recalculation gtpv1
    gtpv1 = (struct gtpv1_hdr *)(skb->data + sizeof(struct udphdr));
    pdr = pdr_find_by_gtp1u(gtp, skb, hdrlen, gtpv1->tid);
    // pskb_may_pull() is called in pdr_find_by_gtp1u(), so gtpv1 may be invalidated here.
    // recalculation gtpv1
    gtpv1 = (struct gtpv1_hdr *)(skb->data + sizeof(struct udphdr));
    if (!pdr) {
        GTP5G_ERR(gtp->dev, "No PDR match this skb : teid[%d]\n", ntohl(gtpv1->tid));
        return -1;
    }

    return gtp5g_rx(pdr, skb, hdrlen, gtp->role);
}

static int gtp5g_drop_skb_encap(struct sk_buff *skb, struct net_device *dev, 
    struct pdr *pdr)
{
    pdr->ul_drop_cnt++;
    dev_kfree_skb(skb);
    return 0;
}

static int gtp5g_buf_skb_encap(struct sk_buff *skb, struct net_device *dev, 
    unsigned int hdrlen, struct pdr *pdr)
{
    // Get rid of the GTP-U + UDP headers.
    if (iptunnel_pull_header(skb,
            hdrlen, 
            skb->protocol,
            !net_eq(sock_net(pdr->sk), dev_net(dev)))) {
        GTP5G_ERR(dev, "Failed to pull GTP-U and UDP headers\n");
        return -1;
    }

    if (unix_sock_send(pdr, skb->data, skb_headlen(skb)) < 0) {
        GTP5G_ERR(dev, "Failed to send skb to unix domain socket PDR(%u)", pdr->id);
        ++pdr->ul_drop_cnt;
    }

    dev_kfree_skb(skb);
    return 0;
}

/* Function unix_sock_{...} are used to handle buffering */
// Send PDR ID, FAR action and buffered packet to user space
static int unix_sock_send(struct pdr *pdr, void *buf, u32 len)
{
    struct msghdr msg;
    struct kvec *kov;

    int msg_kovlen;
    int total_kov_len = 0;
    int i, rt;
    u8  type_hdr[1] = {TYPE_BUFFER};
    u64 self_seid_hdr[1] = {pdr->seid};
    u16 self_hdr[2] = {pdr->id, pdr->far->action};

    if (!pdr->sock_for_buf) {
        GTP5G_ERR(NULL, "Failed Socket buffer is NULL\n");
        return -EINVAL;
    }

    memset(&msg, 0, sizeof(msg));
        if (get_api_with_seid() && get_api_with_urr_bar()) {    
        msg_kovlen = MSG_URR_BAR_KOV_LEN;
        kov = kmalloc_array(msg_kovlen, sizeof(struct kvec),
            GFP_KERNEL);
        if (kov == NULL)
            return -ENOMEM;

        memset(kov, 0, sizeof(struct kvec) * msg_kovlen);

        kov[0].iov_base = type_hdr;
        kov[0].iov_len = sizeof(type_hdr);
        kov[1].iov_base = self_seid_hdr;
        kov[1].iov_len = sizeof(self_seid_hdr);
        kov[2].iov_base = self_hdr;
        kov[2].iov_len = sizeof(self_hdr);
        kov[3].iov_base = buf;
        kov[3].iov_len = len;
    } else if (get_api_with_seid()) {
        msg_kovlen = MSG_SEID_KOV_LEN;
        kov = kmalloc_array(msg_kovlen, sizeof(struct kvec),
            GFP_KERNEL);
        if (kov == NULL)
            return -ENOMEM;

        memset(kov, 0, sizeof(struct kvec) * msg_kovlen);

        kov[0].iov_base = self_seid_hdr;
        kov[0].iov_len = sizeof(self_seid_hdr);
        kov[1].iov_base = self_hdr;
        kov[1].iov_len = sizeof(self_hdr);
        kov[2].iov_base = buf;
        kov[2].iov_len = len;
    } else {
        // for backward compatible
        msg_kovlen = MSG_NO_SEID_KOV_LEN;
        kov = kmalloc_array(msg_kovlen, sizeof(struct kvec),
            GFP_KERNEL);
        if (kov == NULL)
            return -ENOMEM;

        memset(kov, 0, sizeof(struct kvec) * msg_kovlen);

        kov[0].iov_base = self_hdr;
        kov[0].iov_len = sizeof(self_hdr);
        kov[1].iov_base = buf;
        kov[1].iov_len = len;
    }

    for (i = 0; i < msg_kovlen; i++)
        total_kov_len += kov[i].iov_len;

    msg.msg_name = 0;
    msg.msg_namelen = 0;
    iov_iter_kvec(&msg.msg_iter, WRITE, kov, msg_kovlen, total_kov_len);
    msg.msg_control = NULL;
    msg.msg_controllen = 0;
    msg.msg_flags = MSG_DONTWAIT;

    rt = sock_sendmsg(pdr->sock_for_buf, &msg);
    kfree(kov);

    return rt;
}

static int gtp5g_send_usage_report(struct pdr *pdr, struct urr *urr)
{
    struct msghdr msg;
    struct iovec *iov;
    mm_segment_t oldfs;
    u8 flag;
    int msg_iovlen;
    int total_iov_len = 0;
    int i, rt;
    u8  type_hdr[1] = {TYPE_URR_REPORT};
    u64 self_seid_hdr[1] = {pdr->seid};
    u16 self_hdr[1] = {pdr->far->action};
    struct user_report report;
    struct VolumeMeasurement volmeasurement;
    struct UsageReportTrigger trigger;
    // 8.2.44 Volume Measurement octet 5
    // flags are control by GTP5G
    flag = URR_VOLUME_MEASUREMENT_TOVOL | URR_VOLUME_MEASUREMENT_ULVOL | URR_VOLUME_MEASUREMENT_DLVOL;
    if (urr->info & URR_INFO_MNOP)
        flag |= (URR_VOLUME_MEASUREMENT_TONOP | URR_VOLUME_MEASUREMENT_ULNOP | URR_VOLUME_MEASUREMENT_DLNOP);
    // report[0].volumemeasurement.flags = flag;

    if(urr->time_of_fst_pkt != 0 && urr->time_of_lst_pkt == 0)
        urr->time_of_lst_pkt = urr->time_of_fst_pkt;
    urr->end_time = ktime_get_real();


    trigger = (struct UsageReportTrigger){
        // TODO: urr trigger transform to user report trigger
    };
    volmeasurement = (struct VolumeMeasurement){   
        flag, // flag
            
        (pdr->ul_byte_cnt + pdr->dl_byte_cnt),
        pdr->ul_byte_cnt,
        pdr->dl_byte_cnt,

        (pdr->ul_pkt_cnt + pdr->dl_pkt_cnt),
        pdr->ul_pkt_cnt,
        pdr->dl_pkt_cnt, 
    };

    report = (struct user_report){
            urr->id,
            // TODO: uRSEQN
            0,
            0, //queryurrreference

            trigger,
            volmeasurement, // volumemeasurement

            urr->start_time,
            urr->end_time,
            urr->time_of_fst_pkt,
            urr->time_of_lst_pkt
    };
    urr->start_time = ktime_get_real();
    urr->end_time = 0;
    urr->time_of_fst_pkt = 0;
    urr->time_of_lst_pkt = 0;

    GTP5G_TRC(NULL,"flags:%d, total_volume_cnt:%lld, ul_byte_cnt:%lld, dl_byte_cnt:%lld\n",
                flag, (((uint64_t)(pdr->ul_byte_cnt + pdr->dl_byte_cnt) >> 32) | ((uint64_t)(pdr->ul_byte_cnt + pdr->dl_byte_cnt))),
                (((uint64_t)pdr->ul_byte_cnt >> 32) | ((uint64_t)pdr->ul_byte_cnt)),
                (((uint64_t)pdr->dl_byte_cnt >> 32) | ((uint64_t)pdr->dl_byte_cnt)));
                
    if (!pdr->sock_for_report) {
        GTP5G_ERR(NULL, "Failed: Socket for URR  is NULL\n");
        return -EINVAL;
    }

    memset(&msg, 0, sizeof(msg));
  
    msg_iovlen = 3;
    iov = kmalloc_array(msg_iovlen, sizeof(struct iovec),
        GFP_KERNEL);

    memset(iov, 0, sizeof(struct iovec) * msg_iovlen);

    iov[0].iov_base = self_seid_hdr;
    iov[0].iov_len = sizeof(self_seid_hdr);
    iov[1].iov_base = self_hdr;
    iov[1].iov_len = sizeof(self_hdr);
    iov[2].iov_base = &report;
    iov[2].iov_len = sizeof(report);

    for (i = 0; i < msg_iovlen; i++)
        total_iov_len += iov[i].iov_len;

    msg.msg_name = 0;
    msg.msg_namelen = 0;
    iov_iter_init(&msg.msg_iter, WRITE, iov, msg_iovlen, total_iov_len);
    msg.msg_control = NULL;
    msg.msg_controllen = 0;
    msg.msg_flags = MSG_DONTWAIT;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
    oldfs = force_uaccess_begin();
#else
    oldfs = get_fs();
    set_fs(KERNEL_DS);
#endif

    rt = sock_sendmsg(pdr->sock_for_report, &msg);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
    force_uaccess_end(oldfs);
#else
    set_fs(oldfs);
#endif

    return rt;
}

static int gtp5g_rx(struct pdr *pdr, struct sk_buff *skb,
    unsigned int hdrlen, unsigned int role)
{
    int rt = -1;
    struct far *far = pdr->far;
    // struct qer *qer = pdr->qer;
    struct urr *urr = pdr->urr;
    u8 info;
    bool inactive =false;
    u64 volume;
    struct iphdr *iph; 
    struct tcphdr *tcp; 

    if (!far) {
        GTP5G_ERR(pdr->dev, "FAR not exists for PDR(%u)\n", pdr->id);
        goto out;
    }

    // GTP5G_LOG(pdr->dev, "uplink (before qer)\n");
    iph = ip_hdr(skb);
    volume = skb->len - hdrlen - iph->ihl * 4;
    // GTP5G_LOG(pdr->dev, "ul mbqe skb->len: %d\n", skb->len);
    // GTP5G_LOG(pdr->dev, "ul mbqe hdrlen: %d\n", hdrlen);
    // GTP5G_LOG(pdr->dev, "ul mbqe iph->ihl*4: %d\n", iph->ihl*4);

    // GTP5G_LOG(pdr->dev, "ul volume(mbqe): %lld\n", volume);
    // GTP5G_LOG(pdr->dev, "ul iph->protocol(mbqe): %d\n", iph->protocol);


    // Deduct the header length of transport layer
    switch (iph->protocol) {
		case IPPROTO_TCP: 
			// tcp = (struct tcphdr *)(skb_transport_header(skb) + (iph->ihl << 2));
			tcp = (struct tcphdr *)(skb_transport_header(skb));
            GTP5G_LOG(pdr->dev, "ul ip tcp->doff(mbqe): %d\n", tcp->doff);
            volume -= tcp->doff * 4;
			break;
		case IPPROTO_UDP : 
            volume -= 8; // udp header len = 8B
			break;
		default:
			break;
	}

    if(urr->time_of_fst_pkt == 0)
        urr->time_of_fst_pkt = ktime_get_real();
    // URR uplink URR_INFO_MBQE = 1
    urr = pdr->urr;
    if (urr && (urr->info & URR_INFO_MBQE)) {
        info = urr->info;

        if ((info & URR_INFO_ASPOC) && (info & URR_INFO_CIAM) && (info & URR_INFO_INAM))
            inactive = true;
        
        if (!inactive) {
            if (urr->method & URR_METHOD_VOLUM) {
                if (urr->info & URR_INFO_MNOP) { 
                    ++pdr->ul_pkt_cnt; 
                }
                pdr->ul_byte_cnt += volume; // not sure
                
                // Check threshold/quata
                if (urr->trigger & URR_TRIGGER_VOLTH) {
                    GTP5G_TRC(NULL,"URR Volume Threshold Uplink volume:%lld\n",urr->threshold_uvol);
                    GTP5G_TRC(NULL,"URR Volume Threshold Total volume:%lld\n",urr->threshold_tovol);

                    if(urr->threshold_tovol && pdr->ul_byte_cnt + pdr->dl_byte_cnt >= urr->threshold_tovol && ((urr->volumethreshold->flag) & URR_VOLUME_THRESHOLD_TOVOL)){
                        urr->time_of_lst_pkt = ktime_get_real();
                        if (gtp5g_send_usage_report(pdr, urr) < 0) {
                            GTP5G_ERR(pdr->dev, "Failed to send report to unix domain socket PDR(%u)", pdr->id);
                        }
                        else {
                            // Reset
                            pdr->ul_byte_cnt = 0;
                            pdr->dl_byte_cnt = 0;

                            pdr->dl_pkt_cnt = 0;
                            pdr->ul_pkt_cnt = 0;

                            // re-apply all the thresholds
                            urr->threshold_tovol = urr->volumethreshold->totalVolume;
                            urr->threshold_uvol = urr->volumethreshold->uplinkVolume;
                            urr->threshold_dvol = urr->volumethreshold->downlinkVolume;
                        }
                    }
                    else if(urr->threshold_uvol && pdr->ul_byte_cnt >= urr->threshold_uvol && ((urr->volumethreshold->flag) & URR_VOLUME_THRESHOLD_ULVOL)){
                        urr->time_of_lst_pkt = ktime_get_real();
                            // send report
                        if (gtp5g_send_usage_report(pdr, urr) < 0) {
                            GTP5G_ERR(pdr->dev, "Failed to send report to unix domain socket PDR(%u)", pdr->id);
                        }
                        else {
                            // Reset
                            pdr->ul_byte_cnt = 0;
                            pdr->dl_byte_cnt = 0;

                            pdr->dl_pkt_cnt = 0;
                            pdr->ul_pkt_cnt = 0;

                            // re-apply all the thresholds
                            urr->threshold_tovol = urr->volumethreshold->totalVolume;
                            urr->threshold_uvol = urr->volumethreshold->uplinkVolume;
                            urr->threshold_dvol = urr->volumethreshold->downlinkVolume;
                        }
                    }

                    // if (urr->trigger & URR_TRIGGER_VOLQU) {
                    // // send a usage report when reaching the Volume Threshold and also later on when subsequently reaching the Volume Quota.
                    // // if (pdr->urr->)
                    // }
                }
            }
            else {
                GTP5G_ERR(pdr->dev, "method is not volume based(%llu) in URR(%u) and related to PDR(%u)",
                    urr->method, urr->id, pdr->id);
            }
        }
    }

    //TODO: QER
    //if (qer) {
    //    printk_ratelimited("%s:%d QER Rule found, id(%#x) qfi(%#x)\n", __func__, __LINE__,
    //        qer->id, qer->qfi);
    //}

    // TODO: not reading the value of outer_header_removal now,
    // just check if it is assigned.
    if (pdr->outer_header_removal) {
        // One and only one of the DROP, FORW and BUFF flags shall be set to 1.
        // The NOCP flag may only be set if the BUFF flag is set.
        // The DUPL flag may be set with any of the DROP, FORW, BUFF and NOCP flags.
        switch(far->action & FAR_ACTION_MASK) {
        case FAR_ACTION_DROP: 
            rt = gtp5g_drop_skb_encap(skb, pdr->dev, pdr);
            break;
        case FAR_ACTION_FORW:
            rt = gtp5g_fwd_skb_encap(skb, pdr->dev, hdrlen, pdr);
            break;
        case FAR_ACTION_BUFF:
            rt = gtp5g_buf_skb_encap(skb, pdr->dev, hdrlen, pdr);
            break;
        default:
            GTP5G_ERR(pdr->dev, "Unhandled apply action(%u) in FAR(%u) and related to PDR(%u)\n",
                far->action, far->id, pdr->id);
        }
        goto out;
    } 

    // TODO: this action is not supported
    GTP5G_ERR(pdr->dev, "Uplink: PDR(%u) didn't has a OHR information "
        "(which routed to the gtp interface and matches a PDR)\n", pdr->id);

out:
    return rt;
}

static int gtp5g_fwd_skb_encap(struct sk_buff *skb, struct net_device *dev,
    unsigned int hdrlen, struct pdr *pdr)
{
    struct far *far = pdr->far;
    struct forwarding_parameter *fwd_param = far->fwd_param;
    struct outer_header_creation *hdr_creation;
    struct forwarding_policy *fwd_policy;
    struct gtpv1_hdr *gtp1;
    struct iphdr *iph;
    struct udphdr *uh;
    struct pcpu_sw_netstats *stats;
    int ret;
    struct urr *urr = pdr->urr;
    u8 info;
    bool inactive = false;
    u64 volume;
    struct iphdr *iph_urr; 
    struct tcphdr *tcp; 


    if (fwd_param) {
        if ((fwd_policy = fwd_param->fwd_policy))
            skb->mark = fwd_policy->mark;

        if ((hdr_creation = fwd_param->hdr_creation)) {
            // Just modify the teid and packet dest ip
            gtp1 = (struct gtpv1_hdr *)(skb->data + sizeof(struct udphdr));
            gtp1->tid = hdr_creation->teid;

            skb_push(skb, 20); // L3 Header Length
            iph = ip_hdr(skb);

            if (!pdr->pdi->f_teid) {
                GTP5G_ERR(dev, "Failed to hdr removal + creation "
                    "due to pdr->pdi->f_teid not exist\n");
                return -1;
            }
            
            iph->saddr = pdr->pdi->f_teid->gtpu_addr_ipv4.s_addr;
            iph->daddr = hdr_creation->peer_addr_ipv4.s_addr;
            iph->check = 0;

            uh = udp_hdr(skb);
            uh->check = 0;

            if (ip_xmit(skb, pdr->sk, dev) < 0) {
                GTP5G_ERR(dev, "Failed to transmit skb through ip_xmit\n");
                return -1;
            }

            return 0;
        }
    }

    // Get rid of the GTP-U + UDP headers.
    if (iptunnel_pull_header(skb,
            hdrlen, 
            skb->protocol,
            !net_eq(sock_net(pdr->sk), 
            dev_net(dev)))) {
        GTP5G_ERR(dev, "Failed to pull GTP-U and UDP headers\n");
        return -1;
    }

    /* Now that the UDP and the GTP header have been removed, set up the
     * new network header. This is required by the upper layer to
     * calculate the transport header.
     * */
    skb_reset_network_header(skb);

    skb->dev = dev;

    stats = this_cpu_ptr(skb->dev->tstats);
    u64_stats_update_begin(&stats->syncp);
    stats->rx_packets++;
    stats->rx_bytes += skb->len;
    u64_stats_update_end(&stats->syncp);

    pdr->ul_pkt_cnt++;
    pdr->ul_byte_cnt += skb->len; /* length without GTP header */
    GTP5G_INF(NULL, "PDR (%u) UL_PKT_CNT (%llu) UL_BYTE_CNT (%llu)", pdr->id, pdr->ul_pkt_cnt, pdr->ul_byte_cnt);

    ret = netif_rx(skb);
    if (ret != NET_RX_SUCCESS) {
        GTP5G_ERR(dev, "Uplink: Packet got dropped\n");
    }

    // GTP5G_LOG(dev, "uplink (after qer)\n");
    // GTP5G_LOG(pdr->dev, "ul skb->len(~mbqe): %d\n", skb->len);
    iph_urr = ip_hdr(skb);
    volume = skb->len - iph_urr->ihl * 4;
    // GTP5G_LOG(pdr->dev, "ul ~mbqe iph_urr->protocol: %d\n", iph_urr->protocol);
    // GTP5G_LOG(pdr->dev, "ul ~mbqe skb_transport_header - skb: %ld\n", skb_transport_header(skb) - skb_network_header(skb));

    // Deduct the header length of transport layer
    switch (iph_urr->protocol) {
		case IPPROTO_TCP: 
			// tcp = (struct tcphdr *)(skb_transport_header(skb));
			tcp = (struct tcphdr *)(skb_network_header(skb) + 20);

            // GTP5G_LOG(pdr->dev, "ul ip tcp->doff(~mbqe): %d\n", tcp->doff);
            volume -= tcp->doff * 4;
			break;
		case IPPROTO_UDP : 
            volume -= 8; // udp header len = 8B
			break;
		default:
			break;
	}

    // URR uplink URR_INFO_MBQE = 0
    urr = pdr->urr;
    if (urr && !(urr->info & URR_INFO_MBQE)) {
        info = urr->info;

        if ((info & URR_INFO_ASPOC) && (info & URR_INFO_CIAM) && (info & URR_INFO_INAM))
            inactive = true;

        if (!inactive) {
            if (urr->method & URR_METHOD_VOLUM) {
                if (urr->info & URR_INFO_MNOP) { 
                    ++pdr->ul_pkt_cnt; 
                }
                pdr->ul_byte_cnt += volume; 

                // Check threshold/quata
                if (urr->trigger & URR_TRIGGER_VOLTH) {
                    GTP5G_TRC(NULL,"URR Volume Threshold Uplink volume:%lld\n", urr->threshold_uvol);
                    GTP5G_TRC(NULL,"URR Volume Threshold Total volume:%lld\n", urr->threshold_tovol);

                    if(urr->threshold_tovol && pdr->ul_byte_cnt + pdr->dl_byte_cnt >= urr->threshold_tovol && ((urr->volumethreshold->flag) & URR_VOLUME_THRESHOLD_TOVOL)){
                        urr->time_of_lst_pkt = ktime_get_real();

                        if (gtp5g_send_usage_report(pdr, urr) < 0) {
                            GTP5G_ERR(pdr->dev, "Failed to send report to unix domain socket PDR(%u)", pdr->id);
                        }
                        else {
                            // Reset
                            pdr->ul_byte_cnt = 0;
                            pdr->dl_byte_cnt = 0;

                            pdr->dl_pkt_cnt = 0;
                            pdr->ul_pkt_cnt = 0;

                            // re-apply all the thresholds
                            urr->threshold_tovol = urr->volumethreshold->totalVolume;
                            urr->threshold_uvol = urr->volumethreshold->uplinkVolume;
                            urr->threshold_dvol = urr->volumethreshold->downlinkVolume;
                        }
                    }
                    else if(urr->threshold_uvol && pdr->ul_byte_cnt >= urr->threshold_uvol && ((urr->volumethreshold->flag) & URR_VOLUME_THRESHOLD_ULVOL)){
                        urr->time_of_lst_pkt = ktime_get_real();
                        // send report
                        if (gtp5g_send_usage_report(pdr, urr) < 0) {
                            GTP5G_ERR(pdr->dev, "Failed to send report to unix domain socket PDR(%u)", pdr->id);
                        }
                        else {
                            // Reset
                            pdr->ul_byte_cnt = 0;
                            pdr->dl_byte_cnt = 0;

                            pdr->dl_pkt_cnt = 0;
                            pdr->ul_pkt_cnt = 0;

                            // re-apply all the thresholds
                            urr->threshold_tovol = urr->volumethreshold->totalVolume;
                            urr->threshold_uvol = urr->volumethreshold->uplinkVolume;
                            urr->threshold_dvol = urr->volumethreshold->downlinkVolume;
                        }
                    }

                    if (urr->trigger & URR_TRIGGER_VOLQU) {
                    // send a usage report when reaching the Volume Threshold and also later on when subsequently reaching the Volume Quota.
                    // if (pdr->urr->)
                    }
                }
            }
            else {
                GTP5G_ERR(dev, "method is not volume based(%llu) in URR(%u) and related to PDR(%u)",
                    urr->method, urr->id, pdr->id);
            }
        }
    }

    return 0;
}

static int gtp5g_drop_skb_ipv4(struct sk_buff *skb, struct net_device *dev, 
    struct pdr *pdr)
{
    ++pdr->dl_drop_cnt;
    dev_kfree_skb(skb);
    return FAR_ACTION_DROP;
}

static int gtp5g_fwd_skb_ipv4(struct sk_buff *skb, 
    struct net_device *dev, struct gtp5g_pktinfo *pktinfo, 
    struct pdr *pdr)
{
    struct rtable *rt;
    struct flowi4 fl4;
    struct iphdr *iph = ip_hdr(skb);
    struct outer_header_creation *hdr_creation;
    struct urr *urr;
    u8 info;
    bool inactive = false;
    u64 volume;
    struct tcphdr *tcp; 


    if (!(pdr->far && pdr->far->fwd_param &&
        pdr->far->fwd_param->hdr_creation)) {
        GTP5G_ERR(dev, "Unknown RAN address\n");
        goto err;
    }

    hdr_creation = pdr->far->fwd_param->hdr_creation;
    rt = ip4_find_route(skb, 
        iph, 
        pdr->sk,
        dev, 
        pdr->role_addr_ipv4.s_addr, 
        hdr_creation->peer_addr_ipv4.s_addr, 
        &fl4);
    if (IS_ERR(rt))
        goto err;

    if (!pdr->qer) {
        gtp5g_set_pktinfo_ipv4(pktinfo,
            pdr->sk, 
            iph, 
            hdr_creation,
            NULL, 
            rt, 
            &fl4, 
            dev);
    } else {
        gtp5g_set_pktinfo_ipv4(pktinfo,
            pdr->sk, 
            iph, 
            hdr_creation, 
            pdr->qer, 
            rt, 
            &fl4, 
            dev);
    }

    pdr->dl_pkt_cnt++;
    pdr->dl_byte_cnt += skb->len;
    GTP5G_INF(NULL, "PDR (%u) DL_PKT_CNT (%llu) DL_BYTE_CNT (%llu)", pdr->id, pdr->dl_pkt_cnt, pdr->dl_byte_cnt);

    volume = skb->len;
    gtp5g_push_header(skb, pktinfo);

    // GTP5G_LOG(dev, "downlink (after qer)\n");
    // GTP5G_LOG(pdr->dev, "dl skb->len(~mbqe): %d sizeof(pktinfo): %ld\n", skb->len, sizeof(pktinfo));
    volume -= iph->ihl * 4; // iphdr->ihl: length of ip header (unit: 4 bit) 
    // GTP5G_LOG(pdr->dev, "dl volume(~mbqe): %lld iph: %d\n", volume, iph->ihl);

    // Deduct the header length of transport layer
    switch (iph->protocol) {
		case IPPROTO_TCP: 
			// tcp = (struct tcphdr *)(skb_transport_header(skb) + (iph->ihl << 2));
			tcp = (struct tcphdr *)(skb_transport_header(skb));
            // GTP5G_LOG(pdr->dev, "dl ip tcp->doff(~mbqe): %d\n", tcp->doff);
            volume -= tcp->doff * 4;
			break;
		case IPPROTO_UDP : 
            volume -= 8; // udp header len = 8B
			break;
		default:
			break;
	}

    // urr URR_INFO_MBQE=0

    urr = pdr->urr;
    if (urr && !(urr->info & URR_INFO_MBQE)) {
        info = urr->info;

        if ((info & URR_INFO_ASPOC) && (info & URR_INFO_CIAM) && (info & URR_INFO_INAM))
            inactive = true;
        
        if (!inactive) {
            if (urr->method & URR_METHOD_VOLUM) {
                if (urr->info & URR_INFO_MNOP) { 
                    ++pdr->dl_pkt_cnt; 
                }
                pdr->dl_byte_cnt += volume; 
                
                // Check threshold/quata
                if (urr->trigger & URR_TRIGGER_VOLTH) {
                    GTP5G_TRC(NULL,"URR Volume Threshold Downlink volume:%lld\n",urr->threshold_dvol);
                    GTP5G_TRC(NULL,"URR Volume Threshold Total volume:%lld\n",urr->threshold_tovol);

                    if(urr->threshold_tovol && pdr->dl_byte_cnt + pdr->ul_byte_cnt >= urr->threshold_tovol && ((urr->volumethreshold->flag) & URR_VOLUME_THRESHOLD_TOVOL)){
                        urr->time_of_lst_pkt = ktime_get_real();

                        if (gtp5g_send_usage_report(pdr, urr) < 0) {
                            GTP5G_ERR(pdr->dev, "Failed to send report to unix domain socket PDR(%u)", pdr->id);
                        }
                        else {
                            // Reset
                            pdr->ul_byte_cnt = 0;
                            pdr->dl_byte_cnt = 0;

                            pdr->dl_pkt_cnt = 0;
                            pdr->ul_pkt_cnt = 0;

                            // re-apply all the thresholds
                            urr->threshold_tovol = urr->volumethreshold->totalVolume;
                            urr->threshold_uvol = urr->volumethreshold->uplinkVolume;
                            urr->threshold_dvol = urr->volumethreshold->downlinkVolume;
                        }
                    }
                    else if(urr->threshold_dvol && pdr->dl_byte_cnt >= urr->threshold_dvol && ((urr->volumethreshold->flag) & URR_VOLUME_THRESHOLD_DLVOL)){
                        // send report'
                        urr->time_of_lst_pkt = ktime_get_real();
                        if (gtp5g_send_usage_report(pdr, urr) < 0) {
                            GTP5G_ERR(pdr->dev, "Failed to send report to unix domain socket PDR(%u)", pdr->id);
                        }
                        else {
                            // Reset
                            pdr->ul_byte_cnt = 0;
                            pdr->dl_byte_cnt = 0;

                            pdr->dl_pkt_cnt = 0;
                            pdr->ul_pkt_cnt = 0;

                            // re-apply all the thresholds
                            urr->threshold_tovol = urr->volumethreshold->totalVolume;
                            urr->threshold_uvol = urr->volumethreshold->uplinkVolume;
                            urr->threshold_dvol = urr->volumethreshold->downlinkVolume;
                        }
                    }

                    if (urr->trigger & URR_TRIGGER_VOLQU) {
                    // send a usage report when reaching the Volume Threshold and also later on when subsequently reaching the Volume Quota.
                    // if (pdr->urr->)
                    }
                }
            }
            else {
                GTP5G_ERR(dev, "method is not volume based(%llu) in URR(%u) and related to PDR(%u)",
                    urr->method, urr->id, pdr->id);
            }
        }
    }

    return FAR_ACTION_FORW;
err:
    return -EBADMSG;
}

static int gtp5g_buf_skb_ipv4(struct sk_buff *skb, struct net_device *dev,
    struct pdr *pdr)
{
    // TODO: handle nonlinear part
    if (unix_sock_send(pdr, skb->data, skb_headlen(skb)) < 0) {
        GTP5G_ERR(dev, "Failed to send skb to unix domain socket PDR(%u)", pdr->id);
        ++pdr->dl_drop_cnt;
    }

    dev_kfree_skb(skb);
    return FAR_ACTION_BUFF;
}

int gtp5g_handle_skb_ipv4(struct sk_buff *skb, struct net_device *dev,
    struct gtp5g_pktinfo *pktinfo)
{
    struct gtp5g_dev *gtp = netdev_priv(dev);
    struct pdr *pdr;
    struct far *far;
    //struct gtp5g_qer *qer;
    struct urr *urr;
    struct iphdr *iph;
    u64 volume;
    bool inactive = false;
    u8 info;
    struct tcphdr *tcp; 


    /* Read the IP destination address and resolve the PDR.
     * Prepend PDR header with TEI/TID from PDR.
     */
    iph = ip_hdr(skb);
    if (gtp->role == GTP5G_ROLE_UPF)
        pdr = pdr_find_by_ipv4(gtp, skb, 0, iph->daddr);
    else
        pdr = pdr_find_by_ipv4(gtp, skb, 0, iph->saddr);

    if (!pdr) {
        GTP5G_ERR(dev, "no PDR found for %pI4, skip\n", &iph->daddr);
        return -ENOENT;
    }

        volume = skb->len - iph->ihl * 4;

    // Deduct the header length of transport layer
    switch (iph->protocol) {
		case IPPROTO_TCP: 
			// tcp = (struct tcphdr *)(skb_transport_header(skb) + (iph->ihl << 2));
			tcp = (struct tcphdr *)(skb_transport_header(skb));
            // GTP5G_LOG(pdr->dev, "dl ip tcp->doff(mbqe): %d\n", tcp->doff);
            volume -= tcp->doff*4;
			break;
		case IPPROTO_UDP : 
            volume -= 8; // udp header len = 8B
			break;
		default:
			break;
	}

    urr = pdr->urr;

    if(urr->time_of_fst_pkt == 0){
        urr->time_of_fst_pkt = ktime_get_real();
    }

    if (urr && (urr->info & URR_INFO_MBQE)) {
        info = urr->info;

        if ((info & URR_INFO_ASPOC) && (info & URR_INFO_CIAM) && (info & URR_INFO_INAM))
            inactive = true;

        if (!inactive) {
            if (urr->method & URR_METHOD_VOLUM) {
                if (urr->info & URR_INFO_MNOP) { 
                    ++pdr->dl_pkt_cnt; 
                }
                pdr->dl_byte_cnt += volume;
                
                // Check threshold/quata
                if (urr->trigger & URR_TRIGGER_VOLTH) {
                    GTP5G_TRC(NULL,"URR Volume Threshold Downlink volume:%lld\n",urr->threshold_dvol);
                    GTP5G_TRC(NULL,"URR Volume Threshold Total volume:%lld\n",urr->threshold_tovol);
                    if(urr->threshold_tovol && pdr->dl_byte_cnt + pdr->ul_byte_cnt >= urr->threshold_tovol && ((urr->volumethreshold->flag) & URR_VOLUME_THRESHOLD_TOVOL)){
                        urr->time_of_lst_pkt = ktime_get_real();

                        if (gtp5g_send_usage_report(pdr, urr) < 0) {
                            GTP5G_ERR(pdr->dev, "Failed to send report to unix domain socket PDR(%u)", pdr->id);
                        }
                        else {
                            // Reset
                            pdr->ul_byte_cnt = 0;
                            pdr->dl_byte_cnt = 0;

                            pdr->dl_pkt_cnt = 0;
                            pdr->ul_pkt_cnt = 0;

                            // re-apply all the thresholds
                            urr->threshold_tovol = urr->volumethreshold->totalVolume;
                            urr->threshold_uvol = urr->volumethreshold->uplinkVolume;
                            urr->threshold_dvol = urr->volumethreshold->downlinkVolume;
                        }
                    }
                else if(urr->threshold_dvol && pdr->dl_byte_cnt >= urr->threshold_dvol && ((urr->volumethreshold->flag)& URR_VOLUME_THRESHOLD_DLVOL)){
                        urr->time_of_lst_pkt = ktime_get_real();

                            // send report
                        if (gtp5g_send_usage_report(pdr, urr) < 0) {
                            GTP5G_ERR(pdr->dev, "Failed to send report to unix domain socket PDR(%u)", pdr->id);
                        }
                        else {
                            // Reset
                            pdr->ul_byte_cnt = 0;
                            pdr->dl_byte_cnt = 0;

                            pdr->dl_pkt_cnt = 0;
                            pdr->ul_pkt_cnt = 0;

                            // re-apply all the thresholds
                            urr->threshold_tovol = urr->volumethreshold->totalVolume;
                            urr->threshold_uvol = urr->volumethreshold->uplinkVolume;
                            urr->threshold_dvol = urr->volumethreshold->downlinkVolume;
                        }
                    }

                    if (urr->trigger & URR_TRIGGER_VOLQU) {
                    // send a usage report when reaching the Volume Threshold and also later on when subsequently reaching the Volume Quota.
                    // if (pdr->urr->)
                    }
                }
            }
            else {
                GTP5G_ERR(dev, "method is not volume based(%llu) in URR(%u) and related to PDR(%u)",
                    urr->method, urr->id, pdr->id);
            }
        }
    }

    /* TODO: QoS rule have to apply before apply FAR 
     * */
    //qer = pdr->qer;
    //if (qer) {
    //    GTP5G_ERR(dev, "%s:%d QER Rule found, id(%#x) qfi(%#x) TODO\n", 
    //            __func__, __LINE__, qer->id, qer->qfi);
    //}

    far = pdr->far;
    if (far) {
        // One and only one of the DROP, FORW and BUFF flags shall be set to 1.
        // The NOCP flag may only be set if the BUFF flag is set.
        // The DUPL flag may be set with any of the DROP, FORW, BUFF and NOCP flags.
        switch (far->action & FAR_ACTION_MASK) {
        case FAR_ACTION_DROP:
            return gtp5g_drop_skb_ipv4(skb, dev, pdr);
        case FAR_ACTION_FORW:
            return gtp5g_fwd_skb_ipv4(skb, dev, pktinfo, pdr);
        case FAR_ACTION_BUFF:
            return gtp5g_buf_skb_ipv4(skb, dev, pdr);
        default:
            GTP5G_ERR(dev, "Unspec apply action(%u) in FAR(%u) and related to PDR(%u)",
                far->action, far->id, pdr->id);
        }
    }

    return -ENOENT;
}
