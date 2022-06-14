/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GTP5G according to 3GPP TS 29.281 / 3GPP TS 29.244
 *
 * Author: Muthuraman Elangovan <muthuramane.cs03g@g2.nctu.edu.tw>
 *      Yao-Wen Chang <yaowenowo@gmail.com>
 *      Chi Chang <edingroot@gmail.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/udp.h>
#include <linux/rculist.h>
#include <linux/jhash.h>
#include <linux/if_tunnel.h>
#include <linux/net.h>
#include <linux/inetdevice.h>
#include <linux/file.h>
#include <linux/gtp.h>
#include <linux/range.h>
#include <linux/un.h>
#include <linux/proc_fs.h>

#include <net/net_namespace.h>
#include <net/protocol.h>
#include <net/ip.h>
#include <net/udp.h>
#include <net/udp_tunnel.h>
#include <net/icmp.h>
#include <net/xfrm.h>
#include <net/genetlink.h>
#include <net/netns/generic.h>
#include <stdbool.h>

#include "gtp5g.h"

#define DRV_VERSION "0.5.4"

/* used to compatible with api with/without seid */
#define MSG_URR_BAR_IOV_LEN 4
#define MSG_SEID_IOV_LEN 3
#define MSG_NO_SEID_IOV_LEN 2
bool api_with_seid = false;
bool api_with_urr_bar = false;

enum msg_type {
    TYPE_BUFFER = 1,
    TYPE_URR_REPORT,
    TYPE_BAR_INFO,
};

int dbg_trace_lvl = 1;

#define DBG(level, dev, fmt, args...) do {      \
    if (level <= dbg_trace_lvl) {               \
        if (dev)                                \
            printk_ratelimited("%s:[gtp5g] %s: "fmt, netdev_name(dev), __func__, ##args);   \
        else                                    \
            printk_ratelimited("[gtp5g] %s: " fmt, __func__, ##args);       \
    } \
} while(0)

#define GTP5G_LOG(dev, fmt, args...) DBG(0, dev, fmt, ##args)
#define GTP5G_ERR(dev, fmt, args...) DBG(1, dev, fmt, ##args)
#define GTP5G_WAR(dev, fmt, args...) DBG(2, dev, fmt, ##args)
#define GTP5G_INF(dev, fmt, args...) DBG(3, dev, fmt, ##args)
#define GTP5G_TRC(dev, fmt, args...) DBG(4, dev, fmt, ##args)

struct local_f_teid {
    u32     teid;                       // i_teid
    struct in_addr gtpu_addr_ipv4;      // self upf ip
};

struct ip_filter_rule {
    uint8_t action;                     // permit only
    uint8_t direction;                  // in/out
    uint8_t proto;                      // number or "ip" which is not used for matching
    struct in_addr src, smask;          // ip addr or "any" -> 0.0.0.0
    struct in_addr dest, dmask;         // ip addr or "any" -> 0.0.0.0
    int sport_num;                      // Conut for sport
    struct range *sport;                // one value, range or not existed -> [0, 0]
    int dport_num;                      // Counter for dport
    struct range *dport;                // one value, range or not existed -> [0, 0]
};

struct gtp5g_qer {
    struct hlist_node    hlist_id;

    u64                     seid;

    u32 id;                             /* 8.2.75 QER_ID */
    uint8_t     ul_dl_gate;             /* 8.2.7 Gate Status */
    struct {
        uint32_t    ul_high;
        uint8_t     ul_low;
        uint32_t    dl_high;
        uint8_t     dl_low;
    } mbr;                              /* 8.2.8 MBR */
    struct {
        uint32_t    ul_high;
        uint8_t     ul_low;
        uint32_t    dl_high;
        uint8_t     dl_low;
    } gbr;                              /* 8.2.9 GBR */
    uint32_t        qer_corr_id;        /* 8.2.10 QER Correlation ID  */
    uint8_t         rqi;                /* 8.2.88 RQI */
    uint8_t         qfi;                /* 8.2.89 QFI */

    /* 8.2.115 Averaging Window (Optional) */

    uint8_t         ppi;                /* 8.2.116 Paging Policy Indicator */

    /* 8.2.139 Packet Rate Status */

    /* Rate Control Status Reporting */
    uint8_t         rcsr;               /* 8.2.174 QER Control Indications */

    struct net_device   *dev;
    struct rcu_head     rcu_head;
};

struct sdf_filter {
    struct ip_filter_rule *rule;
    uint16_t *tos_traffic_class;
    u32 *security_param_idx;
    u32 *flow_label;               // exactly 3 Octets
    u32 *bi_id;
};

struct gtp5g_pdi {
    //u8                src_iface;          // 0: Access, 1: Core, 2: SGi-LAN/N6-LAN, 3: CP-function
    struct in_addr      *ue_addr_ipv4;
    //char              *network_instance
    struct local_f_teid *f_teid;
    struct sdf_filter   *sdf;
};

struct outer_header_creation {
    u16    description;
    u32    teid;                        // o_teid
    struct in_addr peer_addr_ipv4;
    u16 port;
};

struct forwarding_policy {
    int len;
    char identifier[0xff + 1];

    /* Exact value to handle forwarding policy */
    u32 mark;
};

struct forwarding_parameter {
    //uint8_t dest_int;
    //char *network_instance;

    struct outer_header_creation *hdr_creation;
    struct forwarding_policy *fwd_policy;
};

struct gtp5g_far {
    struct hlist_node    hlist_id;

    u64                     seid;

    u32 id;

    //u8 dest_iface;
    u8 action;                              // apply action

    struct forwarding_parameter *fwd_param;

    struct net_device   *dev;
    struct rcu_head     rcu_head;
};

struct gtp5g_pdr {
    struct hlist_node       hlist_id;
    struct hlist_node       hlist_i_teid;
    struct hlist_node       hlist_addr;
    struct hlist_node       hlist_related_far;
    struct hlist_node       hlist_related_qer;

    u64                     seid;

    u16                     id;
    u32                     precedence;
    u8                      *outer_header_removal;

    struct gtp5g_pdi        *pdi;

    u32                     *far_id;
    struct gtp5g_far        *far;

    u32                     *qer_id;
    struct gtp5g_qer        *qer;

    // AF_UNIX socket for buffer
    struct sockaddr_un      addr_unix;
    struct socket           *sock_for_buf;

    u16     af;
    struct in_addr          role_addr_ipv4;
    struct sock             *sk;
    struct net_device       *dev;
    struct rcu_head         rcu_head;

    /* Drop Count */
    u64                     ul_drop_cnt;
    u64                     dl_drop_cnt;

    /* Packet Statistics */
    u64                     ul_pkt_cnt;
    u64                     dl_pkt_cnt;
    u64                     ul_byte_cnt;
    u64                     dl_byte_cnt;
};

/* One instance of the GTP device. */
struct gtp5g_dev {
    struct list_head        list;

    struct sock             *sk1u;

    struct net_device       *dev;

    unsigned int            role;

    unsigned int            hash_size;
    struct hlist_head       *pdr_id_hash;
    struct hlist_head       *far_id_hash;
    struct hlist_head       *qer_id_hash;

    struct hlist_head       *i_teid_hash;      // Used for GTP-U packet detect
    struct hlist_head       *addr_hash;        // Used for IPv4 packet detect

    /* IEs list related to PDR */
    struct hlist_head       *related_far_hash;     // PDR list waiting the FAR to handle
    struct hlist_head       *related_qer_hash;     // PDR list waiting the QER to handle

    /* Used by proc interface */
    struct list_head        proc_list;
};

struct gtp5g_pktinfo {
    struct sock                   *sk;
    struct iphdr                  *iph;
    struct flowi4                 fl4;
    struct rtable                 *rt;
    struct outer_header_creation  *hdr_creation;
    struct gtp5g_qer              *qer; 
    struct net_device             *dev;
    __be16                        gtph_port;
};

struct gtp5g_emark_pktinfo {
    u32 teid;
    u32 peer_addr;
    u32 local_addr;
    u32 role_addr;
    
    struct sock         *sk;
    struct flowi4       fl4;
    struct rtable       *rt;
    struct net_device   *dev;
    __be16              gtph_port;
};


static unsigned int gtp5g_net_id __read_mostly;

struct gtp5g_net {
    struct list_head gtp5g_dev_list;
};

struct list_head proc_gtp5g_dev;
struct proc_gtp5g_pdr {
    u16     id;
    u64     seid;
    u32     precedence;
    u8      ohr;
    u32     role_addr4;

    u32     pdi_ue_addr4;
    u32     pdi_fteid;
    u32     pdi_gtpu_addr4;
    
    u32     far_id;
    u32     qer_id;

    u64     ul_drop_cnt;
    u64     dl_drop_cnt;

    /* Packet Statistics */
    u64     ul_pkt_cnt;
    u64     dl_pkt_cnt;
    u64     ul_byte_cnt;
    u64     dl_byte_cnt;
};

struct proc_gtp5g_far {
    u32     id;
    u64     seid;
    u8      action;

    //OHC
    u16     description;
    u32     teid; 
    u32     peer_addr4;
};

struct proc_gtp5g_qer {
    u32     id;
    u64     seid;
    u8      qfi;
};

static struct gtp5g_qer *gtp5g_find_qer(struct net *net, struct nlattr *nla[]);
static struct gtp5g_qer *find_qer_by_id(struct gtp5g_dev *gtp, u64 seid, u32 qer_id);
static void qer_context_delete(struct gtp5g_qer *qer);

#define SEID_U32ID_HEX_STR_LEN 24

static void seid_and_u32id_to_hex_str(u64 seid_int, u32 id, char *seid_u32id_hexstr)
{
    #define SEID_HEX_STR_LEN 16
    #define U32_ID_HEX_STR_LEN 8
    
    char seid_hexstr[SEID_HEX_STR_LEN];
    char id_hexstr[U32_ID_HEX_STR_LEN];

    snprintf(seid_hexstr, SEID_HEX_STR_LEN, "%llx", seid_int);
    snprintf(id_hexstr, U32_ID_HEX_STR_LEN, "%x", id);
    strcpy(seid_u32id_hexstr, seid_hexstr);
    strcat(seid_u32id_hexstr, id_hexstr);
}

static void seid_pdr_id_to_hex_str(u64 seid_int, u16 pdr_id, char *buff)
{
    seid_and_u32id_to_hex_str(seid_int, (u32)(pdr_id), buff);
}

static void seid_far_id_to_hex_str(u64 seid_int, u32 far_id, char *buff)
{
    seid_and_u32id_to_hex_str(seid_int, far_id, buff);
}

static void seid_qer_id_to_hex_str(u64 seid_int, u32 qer_id, char *buff)
{
    seid_and_u32id_to_hex_str(seid_int, qer_id, buff);
}

/* Function unix_sock_{...} are used to handle buffering */
// Send PDR ID, FAR action and buffered packet to user space
static int unix_sock_send(struct gtp5g_pdr *pdr, void *buf, u32 len)
{
    struct msghdr msg;
    struct iovec *iov;
    mm_segment_t oldfs;

    int msg_iovlen;
    int total_iov_len = 0;
    int i, rt;
    u8  type_hdr[1] = {TYPE_BUFFER};
    u64 self_seid_hdr[1] = {pdr->seid};
    u16 self_hdr[2] = {pdr->id, pdr->far->action};

    if (!pdr->sock_for_buf) {
        GTP5G_ERR(NULL, "Failed Socket buffer is NULL\n");
        return -EINVAL;
    }

    memset(&msg, 0, sizeof(msg));
    if (api_with_seid && api_with_urr_bar) {    
        msg_iovlen = MSG_URR_BAR_IOV_LEN;
        iov = kmalloc_array(msg_iovlen, sizeof(struct iovec),
            GFP_KERNEL);
        if (iov == NULL)
            return -ENOMEM;

        memset(iov, 0, sizeof(struct iovec) * msg_iovlen);

        iov[0].iov_base = type_hdr;
        iov[0].iov_len = sizeof(type_hdr);
        iov[1].iov_base = self_seid_hdr;
        iov[1].iov_len = sizeof(self_seid_hdr);
        iov[2].iov_base = self_hdr;
        iov[2].iov_len = sizeof(self_hdr);
        iov[3].iov_base = buf;
        iov[3].iov_len = len;
    } else if (api_with_seid) {
        msg_iovlen = MSG_SEID_IOV_LEN;
        iov = kmalloc_array(msg_iovlen, sizeof(struct iovec),
            GFP_KERNEL);
        if (iov == NULL)
            return -ENOMEM;

        memset(iov, 0, sizeof(struct iovec) * msg_iovlen);

        iov[0].iov_base = self_seid_hdr;
        iov[0].iov_len = sizeof(self_seid_hdr);
        iov[1].iov_base = self_hdr;
        iov[1].iov_len = sizeof(self_hdr);
        iov[2].iov_base = buf;
        iov[2].iov_len = len;
    } else {
        // for backward compatible
        msg_iovlen = MSG_NO_SEID_IOV_LEN;
        iov = kmalloc_array(msg_iovlen, sizeof(struct iovec),
            GFP_KERNEL);
        if (iov == NULL)
            return -ENOMEM;

        memset(iov, 0, sizeof(struct iovec) * msg_iovlen);

        iov[0].iov_base = self_hdr;
        iov[0].iov_len = sizeof(self_hdr);
        iov[1].iov_base = buf;
        iov[1].iov_len = len;
    }
    
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

    rt = sock_sendmsg(pdr->sock_for_buf, &msg);
    kfree(iov);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
    force_uaccess_end(oldfs);
#else
    set_fs(oldfs);
#endif

    return rt;
}

// Delete the AF_UNIX client
static void unix_sock_client_delete(struct gtp5g_pdr *pdr)
{
    if (pdr->sock_for_buf)
        sock_release(pdr->sock_for_buf);
    
    pdr->sock_for_buf = NULL;
}

// Create a AF_UNIX client by specific name sent from user space
static int unix_sock_client_new(struct gtp5g_pdr *pdr)
{
    int rt;
    struct socket **psock = &pdr->sock_for_buf;
    struct sockaddr_un *addr = &pdr->addr_unix;

    if (!strlen(addr->sun_path)) {
        GTP5G_ERR(NULL, "Failed sun_path length is 0\n");
        return -EINVAL;
    }

    rt = sock_create(AF_UNIX, SOCK_DGRAM, 0, psock);
    if (rt) {
        GTP5G_ERR(NULL, "Failed to create dgram socket: %s\n", addr->sun_path);
        return rt;
    }

    rt = (*psock)->ops->connect(*psock, (struct sockaddr *) addr,
            sizeof(addr->sun_family) + strlen(addr->sun_path), 0);
    if (rt) {
        unix_sock_client_delete(pdr);
        GTP5G_ERR(NULL, "Failed to connect the socket: %s\n", addr->sun_path);
        return rt;
    }

    return 0;
}

// Handle PDR/FAR changed and affect buffering
static int unix_sock_client_update(struct gtp5g_pdr *pdr)
{
    struct gtp5g_far *far = pdr->far;
    
    unix_sock_client_delete(pdr);
    
    if (far && (far->action & FAR_ACTION_BUFF))
        return unix_sock_client_new(pdr);

    return 0;
}

static u32 gtp5g_h_initval;

static inline u32 u32_hashfn(u32 val)
{
    return jhash_1word(val, gtp5g_h_initval);
}

static inline u32 str_hashfn(char *str)
{
    return jhash(str, strlen(str), 0);
}

static inline u32 ipv4_hashfn(__be32 ip)
{
    return jhash_1word((__force u32)ip, gtp5g_h_initval);
}

static struct gtp5g_far *find_far_by_id(struct gtp5g_dev *gtp, u64 seid, u32 far_id)
{
    struct hlist_head *head;
    struct gtp5g_far *far;
    char seid_far_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};

    seid_far_id_to_hex_str(seid, far_id, seid_far_id_hexstr);
    head = &gtp->far_id_hash[str_hashfn(seid_far_id_hexstr) % gtp->hash_size];

    hlist_for_each_entry_rcu(far, head, hlist_id) {
        if (far->seid == seid && far->id == far_id)
            return far;
    }

    return NULL;
}

static int far_fill(struct gtp5g_far *far, struct gtp5g_dev *gtp, struct genl_info *info,
    u8 *flag, struct gtp5g_emark_pktinfo *epkt_info)
{
    struct nlattr *fwd_param_attrs[GTP5G_FORWARDING_PARAMETER_ATTR_MAX + 1];
    struct nlattr *hdr_creation_attrs[GTP5G_OUTER_HEADER_CREATION_ATTR_MAX + 1];
    struct outer_header_creation *hdr_creation;
    struct forwarding_policy *fwd_policy;
    struct gtp5g_pdr *pdr;
    struct hlist_head *head;

    char seid_far_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};

    if (!far) {
        GTP5G_ERR(NULL, "Far is null\n");
        return -EINVAL;
    }

    far->id = nla_get_u32(info->attrs[GTP5G_FAR_ID]);

    if (info->attrs[GTP5G_FAR_SEID])
        far->seid = nla_get_u64(info->attrs[GTP5G_FAR_SEID]);
    else
        far->seid = 0;

    if (info->attrs[GTP5G_FAR_APPLY_ACTION]) {
        far->action = nla_get_u8(info->attrs[GTP5G_FAR_APPLY_ACTION]);
    }

    if (info->attrs[GTP5G_FAR_FORWARDING_PARAMETER] &&
        !nla_parse_nested(fwd_param_attrs, 
                        GTP5G_FORWARDING_PARAMETER_ATTR_MAX, 
                        info->attrs[GTP5G_FAR_FORWARDING_PARAMETER], 
                        NULL, 
                        NULL)) {
        if (!far->fwd_param) {
            far->fwd_param = kzalloc(sizeof(*far->fwd_param), GFP_ATOMIC);
            if (!far->fwd_param) {
                GTP5G_ERR(NULL, "Failed to allocate memory for FAR fwd param\n");
                return -ENOMEM;
            }
        }

        if (fwd_param_attrs[GTP5G_FORWARDING_PARAMETER_OUTER_HEADER_CREATION] &&
            !nla_parse_nested(hdr_creation_attrs, 
                GTP5G_OUTER_HEADER_CREATION_ATTR_MAX, 
                fwd_param_attrs[GTP5G_FORWARDING_PARAMETER_OUTER_HEADER_CREATION], 
                NULL, 
                NULL)) {
            if (!hdr_creation_attrs[GTP5G_OUTER_HEADER_CREATION_DESCRIPTION] ||
                !hdr_creation_attrs[GTP5G_OUTER_HEADER_CREATION_O_TEID] ||
                !hdr_creation_attrs[GTP5G_OUTER_HEADER_CREATION_PEER_ADDR_IPV4] ||
                !hdr_creation_attrs[GTP5G_OUTER_HEADER_CREATION_PORT]) {
                GTP5G_ERR(NULL, "Header createion attributes error\n");
                return -EINVAL;
            }

            if (!far->fwd_param->hdr_creation) {
                far->fwd_param->hdr_creation = kzalloc(sizeof(*far->fwd_param->hdr_creation), 
                    GFP_ATOMIC);
                if (!far->fwd_param->hdr_creation) {
                    GTP5G_ERR(NULL, "Failed to allocate FAR fwd Hdr creation\n");
                    return -ENOMEM;
                }
                hdr_creation = far->fwd_param->hdr_creation;
                hdr_creation->description = nla_get_u16(hdr_creation_attrs[GTP5G_OUTER_HEADER_CREATION_DESCRIPTION]);
                hdr_creation->teid = htonl(nla_get_u32(hdr_creation_attrs[GTP5G_OUTER_HEADER_CREATION_O_TEID]));
                hdr_creation->peer_addr_ipv4.s_addr = nla_get_be32(hdr_creation_attrs[GTP5G_OUTER_HEADER_CREATION_PEER_ADDR_IPV4]);
                hdr_creation->port = htons(nla_get_u16(hdr_creation_attrs[GTP5G_OUTER_HEADER_CREATION_PORT]));
             } else {
                u32 old_teid, old_peer_addr;
                u16 old_port;

                hdr_creation = far->fwd_param->hdr_creation;
                old_teid = hdr_creation->teid;
                old_peer_addr = hdr_creation->peer_addr_ipv4.s_addr;
                old_port = hdr_creation->port; 
                hdr_creation->description = nla_get_u16(hdr_creation_attrs[GTP5G_OUTER_HEADER_CREATION_DESCRIPTION]);
                hdr_creation->teid = htonl(nla_get_u32(hdr_creation_attrs[GTP5G_OUTER_HEADER_CREATION_O_TEID]));
                hdr_creation->peer_addr_ipv4.s_addr = nla_get_be32(hdr_creation_attrs[GTP5G_OUTER_HEADER_CREATION_PEER_ADDR_IPV4]);
                hdr_creation->port = htons(nla_get_u16(hdr_creation_attrs[GTP5G_OUTER_HEADER_CREATION_PORT]));
                /* For Downlink traffic from UPF to gNB
                 * In some cases,
                 *  1) SMF will send PFCP Msg filled with FAR's TEID and gNB N3 addr as 0 
                 *  2) Later time, SMF will send PFCP Msg filled with right value in 1)
                 *      2.a) We should send the GTP-U EndMarker to gNB
                 *      2.b) SHOULD not set the flag as 1
                 *  3) Xn Handover in b/w gNB then
                 *      3.a) SMF will send modification of PDR, FAR(TEID and GTP-U)
                 *      3.b) SHOULD set the flag as 1 and send GTP-U Marker for old gNB
                 * */
                /* R15.3 29.281
                 * 5.1 General format
                 * When setting up a GTP-U tunnel, the GTP-U entity shall not assign the value 'all zeros' to its own TEID.
                 * However, for backward compatibility, if a GTP-U entity receives (via respective control plane message) a peer's
                 * TEID that is set to the value 'all zeros', the GTP-U entity shall accept this value as valid and send the subsequent
                 * G-PDU with the TEID field in the header set to the value 'all zeros'.
                 * */
                if ((flag != NULL && epkt_info != NULL)) {
                    if (((old_peer_addr & hdr_creation->peer_addr_ipv4.s_addr) != 0) &&
                        ((old_teid != hdr_creation->teid ) ||
                         (old_peer_addr != hdr_creation->peer_addr_ipv4.s_addr))) {
                        *flag = 1;
                        epkt_info->teid = old_teid;
                        epkt_info->peer_addr = old_peer_addr;
                        epkt_info->gtph_port = old_port;
                    }
                }
            }
        }

        if (fwd_param_attrs[GTP5G_FORWARDING_PARAMETER_FORWARDING_POLICY]) {
            if (!far->fwd_param->fwd_policy) {
                far->fwd_param->fwd_policy = kzalloc(sizeof(*far->fwd_param->fwd_policy), 
                                                    GFP_ATOMIC);
                if (!far->fwd_param->fwd_policy) {
                    GTP5G_ERR(NULL, "Failed to allocate FAR fwd policy\n");
                    return -ENOMEM;
                }
            }
            fwd_policy = far->fwd_param->fwd_policy;

            fwd_policy->len = nla_len(fwd_param_attrs[GTP5G_FORWARDING_PARAMETER_FORWARDING_POLICY]);
            if (fwd_policy->len >= sizeof(fwd_policy->identifier)) {
                GTP5G_ERR(NULL, "Failed FAR fwd policy length is bigger\n");
                return -EINVAL;
            }
            strncpy(fwd_policy->identifier, 
                nla_data(fwd_param_attrs[GTP5G_FORWARDING_PARAMETER_FORWARDING_POLICY]), 
                fwd_policy->len);

            /* Exact value to handle forwarding policy */
            if (!(fwd_policy->mark = simple_strtol(fwd_policy->identifier, NULL, 10))) {
                GTP5G_ERR(NULL, "Failed FAR fwd policy mark not set\n");
                return -EINVAL;
            }
        }
    }

    /* Update PDRs which has not linked to this FAR */
    seid_far_id_to_hex_str(far->seid, far->id, seid_far_id_hexstr);
    head = &gtp->related_far_hash[str_hashfn(seid_far_id_hexstr) % gtp->hash_size];
    hlist_for_each_entry_rcu(pdr, head, hlist_related_far) {
        if (pdr->seid == far->seid && *pdr->far_id == far->id) {
            if (flag != NULL && *flag == 1) {
                epkt_info->role_addr = pdr->role_addr_ipv4.s_addr;
                epkt_info->sk = pdr->sk;
                GTP5G_INF(NULL, "Flag is set, role_addr %#x\n", epkt_info->role_addr);
            }
            pdr->far = far;
            if (unix_sock_client_update(pdr) < 0)
                GTP5G_ERR(NULL, "PDR(%u) update fail when FAR(%u) apply action is changed",
                    pdr->id, far->id);
        }
    }

    return 0;
}

static struct gtp5g_pdr *find_pdr_by_id(struct gtp5g_dev *gtp, u64 seid, u16 pdr_id)
{
    struct hlist_head *head;
    struct gtp5g_pdr *pdr;
    char seid_pdr_id[SEID_U32ID_HEX_STR_LEN] = {0};

    seid_pdr_id_to_hex_str(seid, pdr_id, seid_pdr_id);
    head = &gtp->pdr_id_hash[str_hashfn(seid_pdr_id) % gtp->hash_size];
    hlist_for_each_entry_rcu(pdr, head, hlist_id) {
        if (pdr->seid == seid && pdr->id == pdr_id)
            return pdr;
    }

    return NULL;
}

static int ipv4_match(__be32 target_addr, __be32 ifa_addr, __be32 ifa_mask) {
    return !((target_addr ^ ifa_addr) & ifa_mask);
}

static bool ports_match(struct range *match_list, int list_len, __be16 port) {
    int i;

    if (!list_len)
        return true;

    for (i = 0; i < list_len; i++) {
        if (match_list[i].start <= port && match_list[i].end >= port){
            return true;
        }
    }
    
    return false;
}

static int sdf_filter_match(struct sdf_filter *sdf, struct sk_buff *skb, 
    unsigned int hdrlen, u8 direction)
{
    struct iphdr *iph;
    struct ip_filter_rule *rule;
    const __be16 *pptr;
    __be16 _ports[2];

    if (!sdf) {
        GTP5G_ERR(NULL, "SDF is null\n");
        return 1;
    }

    if (!pskb_may_pull(skb, hdrlen + sizeof(struct iphdr))) {
        GTP5G_ERR(NULL, "skb pull fail in sdf filter match\n");
        goto mismatch;
    }
 
    iph = (struct iphdr *)(skb->data + hdrlen);

    if (sdf->rule) {
        rule = sdf->rule;
        if (rule->direction != direction)
            goto mismatch;

        if (rule->proto != 0xff && rule->proto != iph->protocol)
            goto mismatch;

        if (!ipv4_match(iph->saddr, rule->src.s_addr, rule->smask.s_addr))
            goto mismatch;

        if (!ipv4_match(iph->daddr, rule->dest.s_addr, rule->dmask.s_addr))
            goto mismatch;
        
        if (rule->sport_num + rule->dport_num > 0) {
            if (!(pptr = skb_header_pointer(skb, hdrlen + sizeof(struct iphdr), 
                sizeof(_ports), _ports)))
                goto mismatch;

            if (!ports_match(rule->sport, rule->sport_num, ntohs(pptr[0])))
                goto mismatch;
            
            if (!ports_match(rule->dport, rule->dport_num, ntohs(pptr[1])))
                goto mismatch;
        }
    }

    if (sdf->tos_traffic_class)
        GTP5G_ERR(NULL, "TODO: SDF's ToS traffic class\n");
    
    if (sdf->security_param_idx)
        GTP5G_ERR(NULL, "TODO: SDF's Security parameter index\n");

    if (sdf->flow_label)
        GTP5G_ERR(NULL, "TODO: SDF's Flow label\n");

    if (sdf->bi_id)
        GTP5G_ERR(NULL, "TODO: SDF's SDF filter id\n");

    return 1;
mismatch:
    return 0;
}

static struct gtp5g_pdr *pdr_find_by_ipv4(struct gtp5g_dev *gtp, struct sk_buff *skb,
    unsigned int hdrlen, __be32 addr)
{
    struct hlist_head *head;
    struct gtp5g_pdr *pdr;
    struct gtp5g_pdi *pdi;

    head = &gtp->addr_hash[ipv4_hashfn(addr) % gtp->hash_size];

    hlist_for_each_entry_rcu(pdr, head, hlist_addr) {
        pdi = pdr->pdi;

        // TODO: Move the value we check into first level
        if (!(pdr->af == AF_INET && pdi->ue_addr_ipv4->s_addr == addr))
            continue;
        
        if (pdi->sdf)
            if (!sdf_filter_match(pdi->sdf, skb, hdrlen, GTP5G_SDF_FILTER_OUT))
                continue;

        GTP5G_INF(NULL, "Match PDR ID:%d\n", pdr->id);

        return pdr;
    }

    return NULL;
}

static int pdr_fill(struct gtp5g_pdr *pdr, struct gtp5g_dev *gtp, struct genl_info *info)
{
    struct nlattr *pdi_attrs[GTP5G_PDI_ATTR_MAX + 1];
    struct nlattr *f_teid_attrs[GTP5G_F_TEID_ATTR_MAX + 1];
    struct nlattr *sdf_attrs[GTP5G_SDF_FILTER_ATTR_MAX + 1];
    struct nlattr *rule_attrs[GTP5G_FLOW_DESCRIPTION_ATTR_MAX + 1];
    struct hlist_head *head;
    struct gtp5g_pdr *ppdr, *last_ppdr;
    struct gtp5g_pdi *pdi = NULL;
    struct local_f_teid *f_teid = NULL;
    struct sdf_filter *sdf;
    struct ip_filter_rule *rule;
    int i;
    char *str;

    char seid_far_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};
    char seid_qer_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};

    if (!pdr) {
        GTP5G_ERR(NULL, "PDR is NULL\n");
        return -EINVAL;
    }

    pdr->af = AF_INET;
    pdr->id = nla_get_u16(info->attrs[GTP5G_PDR_ID]);
    if (info->attrs[GTP5G_PDR_SEID]) {
        api_with_seid = true;
        pdr->seid = nla_get_u64(info->attrs[GTP5G_PDR_SEID]);
    }
    else {
        api_with_seid = false;
        pdr->seid = 0;
    }

    /* 
     * For backward compatability: 
     * If information has GTP5G_PDR_URR_ID, 
     * it means that user use the API which support 
     * URR and BAR feature. Otherwise, use the older API. 
     */
    if (info->attrs[GTP5G_PDR_URR_ID])
        api_with_urr_bar = true;
    else
        api_with_urr_bar = false;
    
    if (info->attrs[GTP5G_PDR_PRECEDENCE]) 
        pdr->precedence = nla_get_u32(info->attrs[GTP5G_PDR_PRECEDENCE]);

    if (info->attrs[GTP5G_OUTER_HEADER_REMOVAL]) {
        if (!pdr->outer_header_removal) {
            pdr->outer_header_removal = kzalloc(sizeof(*pdr->outer_header_removal), GFP_ATOMIC);
            if (!pdr->outer_header_removal) {
                GTP5G_ERR(NULL, "Failed to allocate OHC\n");
                return -ENOMEM;
            }
        }
        *pdr->outer_header_removal = nla_get_u8(info->attrs[GTP5G_OUTER_HEADER_REMOVAL]);
    }

    /* Not in 3GPP spec, just used for routing */
    if (info->attrs[GTP5G_PDR_ROLE_ADDR_IPV4]) {
        pdr->role_addr_ipv4.s_addr = nla_get_u32(info->attrs[GTP5G_PDR_ROLE_ADDR_IPV4]);
    }

    /* Not in 3GPP spec, just used for buffering */
    if (info->attrs[GTP5G_PDR_UNIX_SOCKET_PATH]) {
        str = nla_data(info->attrs[GTP5G_PDR_UNIX_SOCKET_PATH]);
        pdr->addr_unix.sun_family = AF_UNIX;
        strncpy(pdr->addr_unix.sun_path, str, nla_len(info->attrs[GTP5G_PDR_UNIX_SOCKET_PATH]));
    }

    /* FAR */
    if (info->attrs[GTP5G_PDR_FAR_ID]) {
        if (!pdr->far_id) {
            pdr->far_id = kzalloc(sizeof(*pdr->far_id), GFP_ATOMIC);
            if (!pdr->far_id) {
                GTP5G_ERR(NULL, "Failed to allocate FAR ID\n");
                return -ENOMEM;
            }
        }
        *pdr->far_id = nla_get_u32(info->attrs[GTP5G_PDR_FAR_ID]);

        if (!hlist_unhashed(&pdr->hlist_related_far))
            hlist_del_rcu(&pdr->hlist_related_far);

        seid_far_id_to_hex_str(pdr->seid, *pdr->far_id, seid_far_id_hexstr);
        hlist_add_head_rcu(&pdr->hlist_related_far, 
            &gtp->related_far_hash[str_hashfn(seid_far_id_hexstr) % gtp->hash_size]);
        pdr->far = find_far_by_id(gtp, pdr->seid, *pdr->far_id);
    } else {
        GTP5G_ERR(NULL, "FAR ID not exist\n");
    }

    /* QER */
    if (info->attrs[GTP5G_PDR_QER_ID]) {
        if (!pdr->qer_id) {
            pdr->qer_id = kzalloc(sizeof(*pdr->qer_id), GFP_ATOMIC);
            if (!pdr->qer_id) {
                GTP5G_ERR(NULL, "Failed to allocate memory qer id\n");
                return -ENOMEM;
            }
        }
        *pdr->qer_id = nla_get_u32(info->attrs[GTP5G_PDR_QER_ID]);

        if (!hlist_unhashed(&pdr->hlist_related_qer))
            hlist_del_rcu(&pdr->hlist_related_qer);

        seid_qer_id_to_hex_str(pdr->seid, *pdr->qer_id, seid_qer_id_hexstr);
        hlist_add_head_rcu(&pdr->hlist_related_qer, 
            &gtp->related_qer_hash[str_hashfn(seid_qer_id_hexstr) % gtp->hash_size]);

        pdr->qer = find_qer_by_id(gtp, pdr->seid, *pdr->qer_id);
        if (!pdr->qer)
            GTP5G_ERR(NULL, "Failed to find QER id(%u)\n", *pdr->qer_id);
    } 

    if (unix_sock_client_update(pdr) < 0) {
        GTP5G_ERR(NULL, "PDR sock client update fail\n");
        return -EINVAL;
    }

    /* Parse PDI in PDR */
    if (info->attrs[GTP5G_PDR_PDI] &&
        !nla_parse_nested(pdi_attrs, 
                        GTP5G_PDI_ATTR_MAX, 
                        info->attrs[GTP5G_PDR_PDI], 
                        NULL, 
                        NULL)) {
        if (!pdr->pdi) {
            pdr->pdi = kzalloc(sizeof(*pdr->pdi), GFP_ATOMIC);
            if (!pdr->pdi) {
                GTP5G_ERR(NULL, "Failed to allocate PDI\n");
                return -ENOMEM;
            }
        }
        pdi = pdr->pdi;

        if (pdi_attrs[GTP5G_PDI_UE_ADDR_IPV4]) {
            if (!pdi->ue_addr_ipv4) {
                pdi->ue_addr_ipv4 = kzalloc(sizeof(*pdi->ue_addr_ipv4), GFP_ATOMIC);
                if (!pdi->ue_addr_ipv4) {
                    GTP5G_ERR(NULL, "Failed to allocate UE IPv4 address\n");
                    return -ENOMEM;
                }
            }

            pdi->ue_addr_ipv4->s_addr = nla_get_be32(pdi_attrs[GTP5G_PDI_UE_ADDR_IPV4]);
        }

        /* Parse F-TEID in PDI */
        if (pdi_attrs[GTP5G_PDI_F_TEID] &&
            !nla_parse_nested(f_teid_attrs, 
                            GTP5G_F_TEID_ATTR_MAX, 
                            pdi_attrs[GTP5G_PDI_F_TEID], 
                            NULL, 
                            NULL)) {
            if (!f_teid_attrs[GTP5G_F_TEID_I_TEID] || 
                !f_teid_attrs[GTP5G_F_TEID_GTPU_ADDR_IPV4]) {
                GTP5G_ERR(NULL, "TEID is not preset\n");
                return -EINVAL;
            }

            if (!pdi->f_teid) {
                pdi->f_teid = kzalloc(sizeof(*pdi->f_teid), GFP_ATOMIC);
                if (!pdi->f_teid) {
                    GTP5G_ERR(NULL, "Failed to allocate UE IPv4 address\n");
                    return -ENOMEM;
                }
            } 
            f_teid = pdi->f_teid;
            f_teid->teid = htonl(nla_get_u32(f_teid_attrs[GTP5G_F_TEID_I_TEID]));
            f_teid->gtpu_addr_ipv4.s_addr = nla_get_be32(f_teid_attrs[GTP5G_F_TEID_GTPU_ADDR_IPV4]);
        }

        /* Parse SDF Filter in PDI */
        if (pdi_attrs[GTP5G_PDI_SDF_FILTER] &&
            !nla_parse_nested(sdf_attrs, 
                            GTP5G_SDF_FILTER_ATTR_MAX, 
                            pdi_attrs[GTP5G_PDI_SDF_FILTER], 
                            NULL, 
                            NULL)) {
            if (!pdi->sdf) {
                pdi->sdf = kzalloc(sizeof(*pdi->sdf), GFP_ATOMIC);
                if (!pdi->sdf) {
                    GTP5G_ERR(NULL, "Failed to allocate SDF\n");
                    return -ENOMEM;
                }
            }
            sdf = pdi->sdf;

            if (sdf_attrs[GTP5G_SDF_FILTER_FLOW_DESCRIPTION] &&
                !nla_parse_nested(rule_attrs, 
                                GTP5G_FLOW_DESCRIPTION_ATTR_MAX, 
                                sdf_attrs[GTP5G_SDF_FILTER_FLOW_DESCRIPTION], 
                                NULL, 
                                NULL)) {
                if (!rule_attrs[GTP5G_FLOW_DESCRIPTION_ACTION] ||
                    !rule_attrs[GTP5G_FLOW_DESCRIPTION_DIRECTION] ||
                    !rule_attrs[GTP5G_FLOW_DESCRIPTION_PROTOCOL] ||
                    !rule_attrs[GTP5G_FLOW_DESCRIPTION_SRC_IPV4] ||
                    !rule_attrs[GTP5G_FLOW_DESCRIPTION_DEST_IPV4])
                    return -EINVAL;

                if (!sdf->rule) {
                    sdf->rule = kzalloc(sizeof(*sdf->rule), GFP_ATOMIC);
                    if (!sdf->rule) {
                        GTP5G_ERR(NULL, "Failed to allocate SDF's Rule\n"); 
                        return -ENOMEM;
                    }
                }
                rule = sdf->rule;

                rule->action = nla_get_u8(rule_attrs[GTP5G_FLOW_DESCRIPTION_ACTION]);
                rule->direction = nla_get_u8(rule_attrs[GTP5G_FLOW_DESCRIPTION_DIRECTION]);
                rule->proto = nla_get_u8(rule_attrs[GTP5G_FLOW_DESCRIPTION_PROTOCOL]);
                rule->src.s_addr = nla_get_be32(rule_attrs[GTP5G_FLOW_DESCRIPTION_SRC_IPV4]);
                rule->dest.s_addr = nla_get_be32(rule_attrs[GTP5G_FLOW_DESCRIPTION_DEST_IPV4]);

                if (rule_attrs[GTP5G_FLOW_DESCRIPTION_SRC_MASK])
                    rule->smask.s_addr = nla_get_be32(rule_attrs[GTP5G_FLOW_DESCRIPTION_SRC_MASK]);
                else
                    rule->smask.s_addr = -1;

                if (rule_attrs[GTP5G_FLOW_DESCRIPTION_DEST_MASK])
                    rule->dmask.s_addr = nla_get_be32(rule_attrs[GTP5G_FLOW_DESCRIPTION_DEST_MASK]);
                else
                    rule->dmask.s_addr = -1;

                if (rule_attrs[GTP5G_FLOW_DESCRIPTION_SRC_PORT]) {
                    u32 *sport_encode = nla_data(rule_attrs[GTP5G_FLOW_DESCRIPTION_SRC_PORT]);
                    rule->sport_num = nla_len(rule_attrs[GTP5G_FLOW_DESCRIPTION_SRC_PORT]) / sizeof(u32);
                    if (rule->sport)
                        kfree(rule->sport);
                    rule->sport = kzalloc(rule->sport_num * sizeof(*rule->sport), GFP_ATOMIC);
                    if (!rule->sport) {
                        GTP5G_ERR(NULL, "Failed to allocate SDF's Rule Source Port\n");
                        return -ENOMEM;
                    }

                    for (i = 0; i < rule->sport_num; i++) {
                        if ((sport_encode[i] & 0xFFFF) <= (sport_encode[i] >> 16)) {
                            rule->sport[i].start = (sport_encode[i] & 0xFFFF);
                            rule->sport[i].end = (sport_encode[i] >> 16);
                        } else {
                            rule->sport[i].start = (sport_encode[i] >> 16);
                            rule->sport[i].end = (sport_encode[i] & 0xFFFF);
                        }
                    }
                }

                if (rule_attrs[GTP5G_FLOW_DESCRIPTION_DEST_PORT]) {
                    u32 *dport_encode = nla_data(rule_attrs[GTP5G_FLOW_DESCRIPTION_DEST_PORT]);
                    rule->dport_num = nla_len(rule_attrs[GTP5G_FLOW_DESCRIPTION_DEST_PORT]) / sizeof(u32);

                    if (rule->dport)
                        kfree(rule->dport);

                    rule->dport = kzalloc(rule->dport_num * sizeof(*rule->dport), GFP_ATOMIC);
                    if (!rule->dport) {
                        GTP5G_ERR(NULL, "Failed to allocate SDF's Rule Destination Port\n");
                        return -ENOMEM;
                    }

                    for (i = 0; i < rule->dport_num; i++) {
                        if ((dport_encode[i] & 0xFFFF) <= (dport_encode[i] >> 16)) {
                            rule->dport[i].start = (dport_encode[i] & 0xFFFF);
                            rule->dport[i].end = (dport_encode[i] >> 16);
                        } else {
                            rule->dport[i].start = (dport_encode[i] >> 16);
                            rule->dport[i].end = (dport_encode[i] & 0xFFFF);
                        }
                    }
                }
            }

            if (sdf_attrs[GTP5G_SDF_FILTER_TOS_TRAFFIC_CLASS]) {
                if (!sdf->tos_traffic_class) {
                    sdf->tos_traffic_class = kzalloc(sizeof(*sdf->tos_traffic_class), GFP_ATOMIC);
                    if (!sdf->tos_traffic_class) {
                        GTP5G_ERR(NULL, "Failed to allocate SDF's TOS Traffic class\n");
                        return -ENOMEM;
                    }
                }
                *sdf->tos_traffic_class = nla_get_u16(sdf_attrs[GTP5G_SDF_FILTER_TOS_TRAFFIC_CLASS]);
            }

            if (sdf_attrs[GTP5G_SDF_FILTER_SECURITY_PARAMETER_INDEX]) {
                if (!sdf->security_param_idx) {
                    sdf->security_param_idx = kzalloc(sizeof(*sdf->security_param_idx), GFP_ATOMIC);
                    if (!sdf->security_param_idx) {
                        GTP5G_ERR(NULL, "Failed to allocate SDF's Security Param Index\n"); 
                        return -ENOMEM;
                    }
                }
                *sdf->security_param_idx = nla_get_u32(sdf_attrs[GTP5G_SDF_FILTER_SECURITY_PARAMETER_INDEX]);
            }

            if (sdf_attrs[GTP5G_SDF_FILTER_FLOW_LABEL]) {
                if (!sdf->flow_label) {
                    sdf->flow_label = kzalloc(sizeof(*sdf->flow_label), GFP_ATOMIC);
                    if (!sdf->flow_label) {
                        GTP5G_ERR(NULL, "Failed to allocate SDF's Flow label\n");
                        return -ENOMEM;
                    }
                }
                *sdf->flow_label = nla_get_u32(sdf_attrs[GTP5G_SDF_FILTER_FLOW_LABEL]);
            }

            if (sdf_attrs[GTP5G_SDF_FILTER_SDF_FILTER_ID]) {
                if (!sdf->bi_id) {
                    sdf->bi_id = kzalloc(sizeof(*sdf->bi_id), GFP_ATOMIC);
                    if (!sdf->bi_id) {
                        GTP5G_ERR(NULL, "Failed to allocate SDF's Filter id\n"); 
                        return -ENOMEM;
                    }
                }
                *sdf->bi_id = nla_get_u32(sdf_attrs[GTP5G_SDF_FILTER_SDF_FILTER_ID]);
            }
        }
    }

    if (!hlist_unhashed(&pdr->hlist_i_teid))
        hlist_del_rcu(&pdr->hlist_i_teid);

    if (!hlist_unhashed(&pdr->hlist_addr))
        hlist_del_rcu(&pdr->hlist_addr);

    // Update hlist table
    if ((pdi = pdr->pdi)) {
        if ((f_teid = pdi->f_teid)) {
            last_ppdr = NULL;
            head = &gtp->i_teid_hash[u32_hashfn(f_teid->teid) % gtp->hash_size];
            hlist_for_each_entry_rcu(ppdr, head, hlist_i_teid) {
                if (pdr->precedence > ppdr->precedence)
                    last_ppdr = ppdr;
                else
                    break;
            }

            if (!last_ppdr)
                hlist_add_head_rcu(&pdr->hlist_i_teid, head);
            else
                hlist_add_behind_rcu(&pdr->hlist_i_teid, &last_ppdr->hlist_i_teid);
        } else if (pdi->ue_addr_ipv4) {
            last_ppdr = NULL;
            head = &gtp->addr_hash[u32_hashfn(pdi->ue_addr_ipv4->s_addr) % gtp->hash_size];
            hlist_for_each_entry_rcu(ppdr, head, hlist_addr) {
                if (pdr->precedence > ppdr->precedence)
                    last_ppdr = ppdr;
                else
                    break;
            }

            if (!last_ppdr)
                hlist_add_head_rcu(&pdr->hlist_addr, head);
            else
                hlist_add_behind_rcu(&pdr->hlist_addr, &last_ppdr->hlist_addr);
        }
    }

    return 0;
}

static int gtp5g_dev_init(struct net_device *dev)
{
    struct gtp5g_dev *gtp = netdev_priv(dev);

    gtp->dev = dev;

    dev->tstats = netdev_alloc_pcpu_stats(struct pcpu_sw_netstats);
    if (!dev->tstats) {
        GTP5G_ERR(dev, "Failled to allocate stats\n");
        return -ENOMEM;
    }

    return 0;
}

static void __gtp5g_encap_destroy(struct sock *sk)
{
    struct gtp5g_dev *gtp;

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

static void gtp5g_encap_disable_sock(struct sock *sk)
{
    if (!sk)
        return;

    __gtp5g_encap_destroy(sk);
}

static void gtp5g_encap_disable(struct gtp5g_dev *gtp)
{
    gtp5g_encap_disable_sock(gtp->sk1u);
}

static void gtp5g_dev_uninit(struct net_device *dev)
{
    struct gtp5g_dev *gtp = netdev_priv(dev);

    gtp5g_encap_disable(gtp);
    free_percpu(dev->tstats);
}

static void gtp5g_push_header(struct sk_buff *skb, struct gtp5g_pktinfo *pktinfo)
{
    int payload_len = skb->len;
    struct gtpv1_hdr *gtp1;
    gtpv1_hdr_opt_t *gtp1opt;
    ext_pdu_sess_ctr_t *dl_pdu_sess;
    int ext_flag = 0;

    GTP5G_TRC(NULL, "SKBLen(%u) GTP-U V1(%zu) Opt(%zu) DL_PDU(%zu)\n", 
        payload_len, sizeof(*gtp1), sizeof(*gtp1opt), sizeof(*dl_pdu_sess));

    pktinfo->gtph_port = pktinfo->hdr_creation->port;

    /* Suppport for extension header, sequence number and N-PDU.
     * Update the length field if any of them is available.
     */
    if (pktinfo->qer) {
        ext_flag = 1; 

        /* Push PDU Session container information */
        dl_pdu_sess = skb_push(skb, sizeof(*dl_pdu_sess));
        /* Multiple of 4 (TODO include PPI) */
        dl_pdu_sess->length = 1;
        dl_pdu_sess->pdu_sess_ctr.type_spare = 0; /* For DL */
        dl_pdu_sess->pdu_sess_ctr.u.dl.ppp_rqi_qfi = pktinfo->qer->qfi;
        //TODO: PPI
        dl_pdu_sess->next_ehdr_type = 0; /* No more extension Header */
        
        /* Push optional header information */
        gtp1opt = skb_push(skb, sizeof(*gtp1opt));
        gtp1opt->seq_number = 0;
        gtp1opt->NPDU = 0;
        gtp1opt->next_ehdr_type = 0x85; /* PDU Session Container */
        // Increment the GTP-U payload length by size of optional headers length
        payload_len += (sizeof(*gtp1opt) + sizeof(*dl_pdu_sess));
    }

    /* Bits 8  7  6  5  4  3  2  1
     *    +--+--+--+--+--+--+--+--+
     *    |version |PT| 0| E| S|PN|
     *    +--+--+--+--+--+--+--+--+
     *      0  0  1  1  0  0  0  0
     */
    gtp1 = skb_push(skb, sizeof(*gtp1));
    gtp1->flags = 0x30; /* v1, GTP-non-prime. */
    if (ext_flag) 
        gtp1->flags |= GTPV1_HDR_FLG_EXTHDR; /* v1, Extension header enabled */ 
    gtp1->type = GTP_TPDU;
    gtp1->tid = pktinfo->hdr_creation->teid;
    gtp1->length = htons(payload_len);       /* Excluded the header length of gtpv1 */

    GTP5G_TRC(NULL, "QER Found GTP-U Flg(%u) GTPU-L(%u) SkbLen(%u)\n", 
        gtp1->flags, ntohs(gtp1->length), skb->len);
}

static inline void gtp5g_set_pktinfo_ipv4(struct gtp5g_pktinfo *pktinfo,
    struct sock *sk, struct iphdr *iph, struct outer_header_creation *hdr_creation,
    struct gtp5g_qer *qer, struct rtable *rt, struct flowi4 *fl4,
    struct net_device *dev)
{
    pktinfo->sk = sk;
    pktinfo->iph = iph;
    pktinfo->hdr_creation = hdr_creation;
    pktinfo->qer = qer;
    pktinfo->rt = rt;
    pktinfo->fl4 = *fl4;
    pktinfo->dev = dev;
}

static struct rtable *ip4_find_route(struct sk_buff *skb, struct iphdr *iph,
    struct sock *sk, struct net_device *gtp_dev, 
    __be32 saddr, __be32 daddr, struct flowi4 *fl4)
{
    struct rtable *rt;
    __be16 df;
    int mtu;

    memset(fl4, 0, sizeof(*fl4));
    fl4->flowi4_oif = sk->sk_bound_dev_if;
    fl4->daddr = daddr;
    fl4->saddr = (saddr ? saddr : inet_sk(sk)->inet_saddr);
    fl4->flowi4_tos = RT_CONN_FLAGS(sk);
    fl4->flowi4_proto = sk->sk_protocol;

    rt = ip_route_output_key(dev_net(gtp_dev), fl4);
    if (IS_ERR(rt)) {
        GTP5G_ERR(gtp_dev, "no route to %pI4\n", &iph->daddr);
        gtp_dev->stats.tx_carrier_errors++;
        goto err;
    }

    if (rt->dst.dev == gtp_dev) {
        GTP5G_ERR(gtp_dev, "circular route to %pI4\n", &iph->daddr);
        gtp_dev->stats.collisions++;
        goto err_rt;
    }

    skb_dst_drop(skb);

    /* This is similar to tnl_update_pmtu(). */
    df = iph->frag_off;
    if (df) {
        mtu = dst_mtu(&rt->dst) - gtp_dev->hard_header_len -
            sizeof(struct iphdr) - sizeof(struct udphdr);
        // GTPv1
        mtu -= sizeof(struct gtpv1_hdr);
    }
    else {
        mtu = dst_mtu(&rt->dst);
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0) || defined(RHEL8)
       rt->dst.ops->update_pmtu(&rt->dst, NULL, skb, mtu, false);
#else
       rt->dst.ops->update_pmtu(&rt->dst, NULL, skb, mtu);
#endif

    if (!skb_is_gso(skb) && (iph->frag_off & htons(IP_DF)) &&
        mtu < ntohs(iph->tot_len)) {
        GTP5G_ERR(gtp_dev, "packet too big, fragmentation needed\n");
        memset(IPCB(skb), 0, sizeof(*IPCB(skb)));
        icmp_send(skb, ICMP_DEST_UNREACH, ICMP_FRAG_NEEDED,
              htonl(mtu));
        goto err_rt;
    }

    return rt;
err_rt:
    ip_rt_put(rt);
err:
    return ERR_PTR(-ENOENT);
}

static struct rtable *ip4_find_route_simple(struct sk_buff *skb,
    struct sock *sk, struct net_device *gtp_dev, 
    __be32 saddr, __be32 daddr, struct flowi4 *fl4)
{
    struct rtable *rt;

    memset(fl4, 0, sizeof(*fl4));
    fl4->flowi4_oif = sk->sk_bound_dev_if;
    fl4->daddr = daddr;
    fl4->saddr = (saddr ? saddr : inet_sk(sk)->inet_saddr);
    fl4->flowi4_tos = RT_CONN_FLAGS(sk);
    fl4->flowi4_proto = sk->sk_protocol;

    rt = ip_route_output_key(dev_net(gtp_dev), fl4);
    if (IS_ERR(rt)) {
        GTP5G_ERR(gtp_dev, "no route from %#x to %#x\n", saddr, daddr);
        gtp_dev->stats.tx_carrier_errors++;
        goto err;
    }

    if (rt->dst.dev == gtp_dev) {
        GTP5G_ERR(gtp_dev, "Packet colllisions from %#x to %#x\n", 
            saddr, daddr);
        gtp_dev->stats.collisions++;
        goto err_rt;
    }

    skb_dst_drop(skb);

    return rt;

err_rt:
    ip_rt_put(rt);
err:
    return ERR_PTR(-ENOENT);
}

static int ip_xmit(struct sk_buff *skb, struct sock *sk, struct net_device *gtp_dev) 
{
    struct iphdr *iph = ip_hdr(skb);
    struct flowi4 fl4;
    struct rtable *rt;

    rt = ip4_find_route_simple(skb, sk, gtp_dev, 0, iph->daddr, &fl4);
    if (IS_ERR(rt)) {
        GTP5G_ERR(gtp_dev, "Failed to find route\n");
        return -EBADMSG;
    }

    skb_dst_set(skb, &rt->dst);
    /*
        Source address should be IP addr of the outgoing interface.
        Limitation: Not support multiple IP address configured on outgoing interface.
     */
    iph->saddr = rt->dst.dev->ip_ptr->ifa_list->ifa_address;

    if (ip_local_out(dev_net(gtp_dev), sk, skb) < 0) {
        GTP5G_ERR(gtp_dev, "Failed to send skb to ip layer\n");
        return -1;
    }
    return 0;
}

static int gtp5g_drop_skb_ipv4(struct sk_buff *skb, struct net_device *dev, 
    struct gtp5g_pdr *pdr)
{
    ++pdr->dl_drop_cnt;
    dev_kfree_skb(skb);
    return FAR_ACTION_DROP;
}

static void gtp5g_fwd_emark_skb_ipv4(struct sk_buff *skb,
    struct net_device *dev, struct gtp5g_emark_pktinfo *epkt_info) 
{
    struct rtable *rt;
    struct flowi4 fl4;
    struct gtpv1_hdr *gtp1;

    /* Reset all headers */
    skb_reset_transport_header(skb);
    skb_reset_network_header(skb);
    skb_reset_mac_header(skb);

    /* Fill GTP-U Header */
    gtp1 = skb_push(skb, sizeof(*gtp1));
    gtp1->flags = 0x30; /* v1, GTP-non-prime. */
    gtp1->type = GTP_EMARK;
    gtp1->tid = epkt_info->teid;

    rt = ip4_find_route_simple(skb, epkt_info->sk, dev, 
        epkt_info->role_addr /* Src Addr */ ,
        epkt_info->peer_addr /* Dst Addr*/, 
        &fl4);
    if (IS_ERR(rt)) {
        GTP5G_ERR(dev, "Failed to send GTP-U end-marker due to routing\n");
        dev_kfree_skb(skb);
        return;
    }

    udp_tunnel_xmit_skb(rt, 
        epkt_info->sk, 
        skb,
        fl4.saddr, 
        fl4.daddr,
        0,
        ip4_dst_hoplimit(&rt->dst),
        0,
        epkt_info->gtph_port, 
        epkt_info->gtph_port,
        true, 
        true);
}

static int gtp5g_fwd_skb_ipv4(struct sk_buff *skb, 
    struct net_device *dev, struct gtp5g_pktinfo *pktinfo, 
    struct gtp5g_pdr *pdr)
{
    struct rtable *rt;
    struct flowi4 fl4;
    struct iphdr *iph = ip_hdr(skb);
    struct outer_header_creation *hdr_creation;

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
    GTP5G_LOG(NULL, "PDR (%u) DL_PKT_CNT (%llu) DL_BYTE_CNT (%llu)", pdr->id, pdr->dl_pkt_cnt, pdr->dl_byte_cnt);

    gtp5g_push_header(skb, pktinfo);

    return FAR_ACTION_FORW;
err:
    return -EBADMSG;
}

static int gtp5g_buf_skb_ipv4(struct sk_buff *skb, struct net_device *dev,
    struct gtp5g_pdr *pdr)
{
    // TODO: handle nonlinear part
    if (unix_sock_send(pdr, skb->data, skb_headlen(skb)) < 0) {
        GTP5G_ERR(dev, "Failed to send skb to unix domain socket PDR(%u)", pdr->id);
        ++pdr->dl_drop_cnt;
    }

    dev_kfree_skb(skb);
    return FAR_ACTION_BUFF;
}

static int gtp5g_handle_skb_ipv4(struct sk_buff *skb, struct net_device *dev,
    struct gtp5g_pktinfo *pktinfo)
{
    struct gtp5g_dev *gtp = netdev_priv(dev);
    struct gtp5g_pdr *pdr;
    struct gtp5g_far *far;
    //struct gtp5g_qer *qer;
    struct iphdr *iph;

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

static void gtp5g_xmit_skb_ipv4(struct sk_buff *skb, struct gtp5g_pktinfo *pktinfo)
{
    //GTP5G_ERR(pktinfo->dev, "gtp -> IP src: %pI4 dst: %pI4\n",
    //           &pktinfo->iph->saddr, &pktinfo->iph->daddr);
    udp_tunnel_xmit_skb(pktinfo->rt, 
        pktinfo->sk,
        skb,
        pktinfo->fl4.saddr,
        pktinfo->fl4.daddr,
        pktinfo->iph->tos,
        ip4_dst_hoplimit(&pktinfo->rt->dst),
        0,
        pktinfo->gtph_port, 
        pktinfo->gtph_port,
        true, 
        true);
}

/**
 * Entry function for Downlink packets
 * */
static netdev_tx_t gtp5g_dev_xmit(struct sk_buff *skb, struct net_device *dev)
{
    unsigned int proto = ntohs(skb->protocol);
    struct gtp5g_pktinfo pktinfo;
    int ret = 0;

    /* Ensure there is sufficient headroom */
    if (skb_cow_head(skb, dev->needed_headroom)) {
        goto tx_err;
    }

    skb_reset_inner_headers(skb);

    /* PDR lookups in gtp5g_build_skb_*() need rcu read-side lock. 
     * */
    rcu_read_lock();
    switch (proto) {
    case ETH_P_IP:
        ret = gtp5g_handle_skb_ipv4(skb, dev, &pktinfo);
        break;
    default:
        ret = -EOPNOTSUPP;
    }
    rcu_read_unlock();

    if (ret < 0)
        goto tx_err;

    if (ret == FAR_ACTION_FORW)
        gtp5g_xmit_skb_ipv4(skb, &pktinfo);

    return NETDEV_TX_OK;

tx_err:
    dev->stats.tx_errors++;
    dev_kfree_skb(skb);
    return NETDEV_TX_OK;
}

static const struct net_device_ops gtp5g_netdev_ops = {
    .ndo_init           = gtp5g_dev_init,
    .ndo_uninit         = gtp5g_dev_uninit,
    .ndo_start_xmit     = gtp5g_dev_xmit,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
    .ndo_get_stats64    = dev_get_tstats64,
#else
    .ndo_get_stats64    = ip_tunnel_get_stats64,
#endif
};

static void pdr_context_free(struct rcu_head *head)
{
    struct gtp5g_pdr *pdr = container_of(head, struct gtp5g_pdr, rcu_head);
    struct gtp5g_pdi *pdi;
    struct sdf_filter *sdf;

    if (!pdr)
        return;

    sock_put(pdr->sk);

    if (pdr->outer_header_removal) 
        kfree(pdr->outer_header_removal);

    pdi = pdr->pdi;
    if (pdi) {
        if (pdi->ue_addr_ipv4)
            kfree(pdi->ue_addr_ipv4);
        if (pdi->f_teid)
            kfree(pdi->f_teid);
        if (pdr->far_id)
            kfree(pdr->far_id);
        if (pdr->qer_id)
            kfree(pdr->qer_id);

        sdf = pdi->sdf;
        if (sdf) {
            if (sdf->rule) {
                if (sdf->rule->sport)
                    kfree(sdf->rule->sport);
                if (sdf->rule->dport)
                    kfree(sdf->rule->dport);
                kfree(sdf->rule);
            }
            if (sdf->tos_traffic_class)
                kfree(sdf->tos_traffic_class);
            if (sdf->security_param_idx)
                kfree(sdf->security_param_idx);
            if (sdf->flow_label)
                kfree(sdf->flow_label);
            if (sdf->bi_id)
                kfree(sdf->bi_id);

            kfree(sdf);
        }
        kfree(pdi);
    }

    unix_sock_client_delete(pdr);
    kfree(pdr);
}

static void pdr_context_delete(struct gtp5g_pdr *pdr)
{
    if (!pdr)
        return;

    if (!hlist_unhashed(&pdr->hlist_id))
        hlist_del_rcu(&pdr->hlist_id);

    if (!hlist_unhashed(&pdr->hlist_i_teid))
        hlist_del_rcu(&pdr->hlist_i_teid);

    if (!hlist_unhashed(&pdr->hlist_addr))
        hlist_del_rcu(&pdr->hlist_addr);

    if (!hlist_unhashed(&pdr->hlist_related_far))
        hlist_del_rcu(&pdr->hlist_related_far);

    if (!hlist_unhashed(&pdr->hlist_related_qer))
        hlist_del_rcu(&pdr->hlist_related_qer);

    call_rcu(&pdr->rcu_head, pdr_context_free);
}

static void far_context_free(struct rcu_head *head)
{
    struct gtp5g_far *far = container_of(head, struct gtp5g_far, rcu_head);
    struct forwarding_parameter *fwd_param;

    if (!far)
        return;
    
    fwd_param = far->fwd_param;
    if (fwd_param) {
        if (fwd_param->hdr_creation) 
            kfree(fwd_param->hdr_creation);
        if (fwd_param->fwd_policy)
            kfree(fwd_param->fwd_policy);
        kfree(fwd_param);
    }

    kfree(far);
}

static void far_context_delete(struct gtp5g_far *far)
{
    struct gtp5g_dev *gtp;
    struct hlist_head *head;
    struct gtp5g_pdr *pdr;

    char seid_far_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};

    if (!far)
        return;

    gtp = netdev_priv(far->dev);

    if (!hlist_unhashed(&far->hlist_id))
        hlist_del_rcu(&far->hlist_id);

    seid_far_id_to_hex_str(far->seid, far->id, seid_far_id_hexstr);
    head = &gtp->related_far_hash[str_hashfn(seid_far_id_hexstr) % gtp->hash_size];
    hlist_for_each_entry_rcu(pdr, head, hlist_related_far) {
        if (pdr->seid == far->seid && *pdr->far_id == far->id) {
            pdr->far = NULL;
            unix_sock_client_delete(pdr);
        }
    }

    call_rcu(&far->rcu_head, far_context_free);
}

static int gtp5g_hashtable_new(struct gtp5g_dev *gtp, int hsize)
{
    int i;

    gtp->addr_hash = kmalloc_array(hsize, sizeof(struct hlist_head),
        GFP_KERNEL);
    if (gtp->addr_hash == NULL)
        return -ENOMEM;

    gtp->i_teid_hash = kmalloc_array(hsize, sizeof(struct hlist_head),
        GFP_KERNEL);
    if (gtp->i_teid_hash == NULL)
        goto err1;

    gtp->pdr_id_hash = kmalloc_array(hsize, sizeof(struct hlist_head),
        GFP_KERNEL);
    if (gtp->pdr_id_hash == NULL)
        goto err2;

    gtp->far_id_hash = kmalloc_array(hsize, sizeof(struct hlist_head),
        GFP_KERNEL);
    if (gtp->far_id_hash == NULL)
        goto err3;

    gtp->qer_id_hash = kmalloc_array(hsize, sizeof(struct hlist_head),
        GFP_KERNEL);
    if (gtp->qer_id_hash == NULL)
        goto err4;

    gtp->related_far_hash = kmalloc_array(hsize, sizeof(struct hlist_head),
        GFP_KERNEL);
    if (gtp->related_far_hash == NULL)
        goto err5;

    gtp->related_qer_hash = kmalloc_array(hsize, sizeof(struct hlist_head),
        GFP_KERNEL);
    if (gtp->related_qer_hash == NULL)
        goto err6;

    gtp->hash_size = hsize;

    for (i = 0; i < hsize; i++) {
        INIT_HLIST_HEAD(&gtp->addr_hash[i]);
        INIT_HLIST_HEAD(&gtp->i_teid_hash[i]);
        INIT_HLIST_HEAD(&gtp->pdr_id_hash[i]);
        INIT_HLIST_HEAD(&gtp->far_id_hash[i]);
        INIT_HLIST_HEAD(&gtp->qer_id_hash[i]);
        INIT_HLIST_HEAD(&gtp->related_far_hash[i]);
        INIT_HLIST_HEAD(&gtp->related_qer_hash[i]);
    }

    return 0;
err6:
    kfree(gtp->related_far_hash);
err5:
    kfree(gtp->qer_id_hash);
err4:
    kfree(gtp->far_id_hash);
err3:
    kfree(gtp->pdr_id_hash);
err2:
    kfree(gtp->i_teid_hash);
err1:
    kfree(gtp->addr_hash);
    return -ENOMEM;
}

static void gtp5g_hashtable_free(struct gtp5g_dev *gtp)
{
    struct gtp5g_pdr *pdr;
    struct gtp5g_far *far;
    struct gtp5g_qer *qer;
    int i;

    for (i = 0; i < gtp->hash_size; i++) {
        hlist_for_each_entry_rcu(far, &gtp->far_id_hash[i], hlist_id)
            far_context_delete(far);
        hlist_for_each_entry_rcu(qer, &gtp->qer_id_hash[i], hlist_id)
            qer_context_delete(qer);
        hlist_for_each_entry_rcu(pdr, &gtp->pdr_id_hash[i], hlist_id)
            pdr_context_delete(pdr);
    }

    synchronize_rcu();
    kfree(gtp->addr_hash);
    kfree(gtp->i_teid_hash);
    kfree(gtp->pdr_id_hash);
    kfree(gtp->far_id_hash);
    kfree(gtp->qer_id_hash);
    kfree(gtp->related_far_hash);
    kfree(gtp->related_qer_hash);
}

static void gtp5g_link_setup(struct net_device *dev)
{
    dev->netdev_ops = &gtp5g_netdev_ops;
    dev->needs_free_netdev = true;

    dev->hard_header_len = 0;
    dev->addr_len = 0;
    dev->mtu = ETH_DATA_LEN -
        (sizeof(struct iphdr) +
         sizeof(struct udphdr) +
         sizeof(struct gtpv1_hdr));

    /* Zero header length. */
    dev->type = ARPHRD_NONE;
    dev->flags = IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST;

    dev->priv_flags |= IFF_NO_QUEUE;
    dev->features |= NETIF_F_LLTX;
    netif_keep_dst(dev);

    /* TODO: Modify the headroom size based on
     * what are the extension header going to support
     * */
    dev->needed_headroom = LL_MAX_HEADER +
        sizeof(struct iphdr) +
        sizeof(struct udphdr) +
        sizeof(struct gtpv1_hdr) + 
        sizeof(struct gtp1_hdr_opt) +
        sizeof(struct gtp1_hdr_ext_pdu_sess_ctr);
}

static int gtp5g_validate(struct nlattr *tb[], struct nlattr *data[],
    struct netlink_ext_ack *extack)
{
    if (!data)
        return -EINVAL;

    return 0;
}

static struct gtp5g_dev *gtp5g_find_dev(struct net *src_net, struct nlattr *nla[])
{
    struct gtp5g_dev *gtp = NULL;
    struct net_device *dev;
    struct net *net;

    /* Examine the link attributes and figure out which network namespace
     * we are talking about.
     */
    if (nla[GTP5G_NET_NS_FD])
        net = get_net_ns_by_fd(nla_get_u32(nla[GTP5G_NET_NS_FD]));
    else
        net = get_net(src_net);

    if (IS_ERR(net))
        return NULL;

    /* Check if there's an existing gtp5g device to configure */
    dev = dev_get_by_index_rcu(net, nla_get_u32(nla[GTP5G_LINK]));
    if (dev && dev->netdev_ops == &gtp5g_netdev_ops)
        gtp = netdev_priv(dev);

    put_net(net);

    return gtp;
}

static struct gtp5g_pdr *gtp5g_find_pdr_by_link(struct net *net, struct nlattr *nla[])
{
    struct gtp5g_dev *gtp;
    u64 seid;

    if (nla[GTP5G_PDR_SEID])
        seid =  nla_get_u64(nla[GTP5G_PDR_SEID]);
    else
        seid = 0;

    gtp = gtp5g_find_dev(net, nla);
    if (!gtp) {
        GTP5G_ERR(NULL, "Failed to find gtp device for pdr\n");
        return ERR_PTR(-ENODEV);
    }

    if (nla[GTP5G_PDR_ID]) {
        u16 id = nla_get_u16(nla[GTP5G_PDR_ID]);
        return find_pdr_by_id(gtp, seid, id);
    }

    return ERR_PTR(-EINVAL);
}

static struct gtp5g_pdr *gtp5g_find_pdr(struct net *net, struct nlattr *nla[])
{
    struct gtp5g_pdr *pdr;

    if (nla[GTP5G_LINK])
        pdr = gtp5g_find_pdr_by_link(net, nla);
    else
        pdr = ERR_PTR(-EINVAL);

    if (!pdr)
        pdr = ERR_PTR(-ENOENT);

    return pdr;
}

static struct gtp5g_far *gtp5g_find_far_by_link(struct net *net, struct nlattr *nla[])
{
    struct gtp5g_dev *gtp;
    u64 seid;

    if (nla[GTP5G_FAR_SEID])
        seid = nla_get_u64(nla[GTP5G_FAR_SEID]);
    else
        seid = 0;

    gtp = gtp5g_find_dev(net, nla);
    if (!gtp) {
        GTP5G_ERR(NULL, "Failed to find gtp device for far\n");
        return ERR_PTR(-ENODEV);
    }

    if (nla[GTP5G_FAR_ID]) {
        u32 id = nla_get_u32(nla[GTP5G_FAR_ID]);
        return find_far_by_id(gtp, seid, id);
    }

    return ERR_PTR(-EINVAL);
}

static struct gtp5g_far *gtp5g_find_far(struct net *net, struct nlattr *nla[])
{
    struct gtp5g_far *far = NULL;

    if (nla[GTP5G_LINK])
        far = gtp5g_find_far_by_link(net, nla);
    else
        far = ERR_PTR(-EINVAL);

    if (!far)
        far = ERR_PTR(-ENOENT);

    return far;
}

static void gtp5g_encap_destroy(struct sock *sk)
{
    rtnl_lock();
    __gtp5g_encap_destroy(sk);
    rtnl_unlock();
}

static struct gtp5g_pdr *pdr_find_by_gtp1u(struct gtp5g_dev *gtp, struct sk_buff *skb,
    unsigned int hdrlen, u32 teid)
{
#ifdef MATCH_IP
    struct iphdr *outer_iph;
#endif
    struct iphdr *iph;
    __be32 *target_addr;
    struct hlist_head *head;
    struct gtp5g_pdr *pdr;
    struct gtp5g_pdi *pdi;

     if (!gtp) {
        GTP5G_ERR(NULL, "gtp5g_dev is null\n");
        return NULL;
    }

    if (ntohs(skb->protocol) != ETH_P_IP) {
        GTP5G_ERR(gtp->dev, "skb protocol is not IP: %#x\n", ntohs(skb->protocol));
        return NULL;
    }

    if (!pskb_may_pull(skb, hdrlen + sizeof(struct iphdr))) {
        GTP5G_ERR(gtp->dev, "Failed to pull skb len: %#x + IP header\n", hdrlen);
        return NULL;
    }

    iph = (struct iphdr *)(skb->data + hdrlen);
    target_addr = (gtp->role == GTP5G_ROLE_UPF ? &iph->saddr : &iph->daddr);

    head = &gtp->i_teid_hash[u32_hashfn(teid) % gtp->hash_size];
    hlist_for_each_entry_rcu(pdr, head, hlist_i_teid) {
        pdi = pdr->pdi;
        if (!pdi) {
            GTP5G_ERR(gtp->dev, "PDI is not present\n");
            continue;
        }
        
        // GTP-U packet must check teid
        if (!(pdi->f_teid && pdi->f_teid->teid == teid))
            continue;

#ifdef MATCH_IP
        // check outer IP dest addr to distinguish between N3 and N9 packet while act as i-upf
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
        outer_iph = (struct iphdr *)(skb->head + skb->network_header);
    #else
        outer_iph = (struct iphdr *)(skb->network_header);
    #endif

        if (!(pdi->f_teid && pdi->f_teid->gtpu_addr_ipv4.s_addr == outer_iph->daddr))
            continue;
#endif
        if (pdi->ue_addr_ipv4)
            if (!(pdr->af == AF_INET && *target_addr == pdi->ue_addr_ipv4->s_addr))
                continue;

        if (pdi->sdf)
            if (!sdf_filter_match(pdi->sdf, skb, hdrlen, GTP5G_SDF_FILTER_OUT))
                continue;

        return pdr;
    }

    return NULL;
}

static int gtp5g_drop_skb_encap(struct sk_buff *skb, struct net_device *dev, 
    struct gtp5g_pdr *pdr)
{
    pdr->ul_drop_cnt++;
    dev_kfree_skb(skb);
    return 0;
}

static int gtp5g_fwd_skb_encap(struct sk_buff *skb, struct net_device *dev,
    unsigned int hdrlen, struct gtp5g_pdr *pdr)
{
    struct gtp5g_far *far = pdr->far;
    struct forwarding_parameter *fwd_param = far->fwd_param;
    struct outer_header_creation *hdr_creation;
    struct forwarding_policy *fwd_policy;
    struct gtpv1_hdr *gtp1;
    struct iphdr *iph;
    struct udphdr *uh;
    struct pcpu_sw_netstats *stats;
    int ret;

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
    GTP5G_LOG(NULL, "PDR (%u) UL_PKT_CNT (%llu) UL_BYTE_CNT (%llu)", pdr->id, pdr->ul_pkt_cnt, pdr->ul_byte_cnt);

    ret = netif_rx(skb);
    if (ret != NET_RX_SUCCESS) {
        GTP5G_ERR(dev, "Uplink: Packet got dropped\n");
    }

    return 0;
}

static int gtp5g_buf_skb_encap(struct sk_buff *skb, struct net_device *dev, 
    struct gtp5g_pdr *pdr)
{
    if (unix_sock_send(pdr, skb->data, skb_headlen(skb)) < 0) {
        GTP5G_ERR(dev, "Failed to send skb to unix domain socket PDR(%u)", pdr->id);
        ++pdr->ul_drop_cnt;
    }

    dev_kfree_skb(skb);
    return 0;
}

static int gtp5g_rx(struct gtp5g_pdr *pdr, struct sk_buff *skb,
    unsigned int hdrlen, unsigned int role)
{
    int rt = -1;
    struct gtp5g_far *far = pdr->far;
    //struct gtp5g_qer *qer = pdr->qer;

    if (!far) {
        GTP5G_ERR(pdr->dev, "FAR not exists for PDR(%u)\n", pdr->id);
        goto out;
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
            rt = gtp5g_buf_skb_encap(skb, pdr->dev, pdr);
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

static int get_gtpu_header_len(struct gtpv1_hdr *gtpv1,  struct sk_buff *skb)
{
    u16 len = sizeof(*gtpv1);
    u16 pull_len = sizeof(struct udphdr);

    /** TS 29.281 Chapter 5.1 and Figure 5.1-1
     * GTP-U header at least 8 byte
     *
     * This field shall be present if and only if any one or more of the S, PN and E flags are set.
     * This field means seq number (2 Octect), N-PDU number (1 Octet) and  Next ext hdr type (1 Octet).
     *
     * TODO: Validate the Reserved flag set or not, if it is set then protocol error
     */
    if (gtpv1->flags & GTPV1_HDR_FLG_MASK) 
        len += 4;
    else
        return len;
    pull_len += len;

    /** TS 29.281 Chapter 5.2 and Figure 5.2.1-1
     * The length of the Extension header shall be defined in a variable length of 4 octets,
     * i.e. m+1 = n*4 octets, where n is a positive integer.
     */
    if (gtpv1->flags & GTPV1_HDR_FLG_EXTHDR) {
        __u8 next_ehdr_type = 0;
        gtpv1_hdr_opt_t *gtpv1_opt;
        
        if (!pskb_may_pull(skb, pull_len)) {
            GTP5G_ERR(NULL, "Failed to pull skb length %#x\n", pull_len);
            return -1;
        }
        // pskb_may_pull() is called, so gtpv1 may be invalidated here.
        gtpv1 = (struct gtpv1_hdr *)(skb->data + sizeof(struct udphdr));
        gtpv1_opt = (gtpv1_hdr_opt_t *) ((u8 *) gtpv1 + sizeof(*gtpv1));

        next_ehdr_type = gtpv1_opt->next_ehdr_type;
        while (next_ehdr_type) {
            switch (next_ehdr_type) {
            case GTPV1_NEXT_EXT_HDR_TYPE_85: {
                ext_pdu_sess_ctr_t *etype85; 
                // pdu_sess_ctr_t *pdu_sess_info = &etype85->pdu_sess_ctr;

                if (!pskb_may_pull(skb, (pull_len + 4))) {
                    GTP5G_ERR(NULL, "Failed to pull skb length %#x\n", pull_len);
                    return -1;
                }
                // pskb_may_pull() is called, so gtpv1 and gtpv1_opt may be invalidated here.
                gtpv1 = (struct gtpv1_hdr *)(skb->data + sizeof(struct udphdr));
                gtpv1_opt = (gtpv1_hdr_opt_t *) ((u8 *) gtpv1 + sizeof(*gtpv1));
                etype85 = (ext_pdu_sess_ctr_t *) ((u8 *) gtpv1_opt + sizeof(*gtpv1_opt)); 

                // Commented the below code due to support N9 packet downlink
                // if (pdu_sess_info->type_spare == PDU_SESSION_INFO_TYPE0)
                // return -1;
            
                //TODO: validate pdu_sess_ctr

                //Length should be multiple of 4
                len += (etype85->length * 4);
                pull_len += (etype85->length * 4);
                next_ehdr_type = etype85->next_ehdr_type;
                break;
            }
            default:
                /* Unknown/Unhandled Extension Header Type */
                 GTP5G_ERR(NULL, "%s: Invalid header type(%#x)\n", 
                    __func__, next_ehdr_type);
                return -1;
            }
        }
    }

    return len;
}

static int gtp1u_udp_encap_recv(struct gtp5g_dev *gtp, struct sk_buff *skb)
{
    unsigned int hdrlen = sizeof(struct udphdr) + sizeof(struct gtpv1_hdr);
    struct gtpv1_hdr *gtpv1;
    struct gtp5g_pdr *pdr;
    int gtpv1_hdr_len;

    if (!pskb_may_pull(skb, hdrlen)) {
        GTP5G_ERR(gtp->dev, "Failed to pull skb length %#x\n", hdrlen);
        return -1;
    }

    gtpv1 = (struct gtpv1_hdr *)(skb->data + sizeof(struct udphdr));
    if ((gtpv1->flags >> 5) != GTP_V1) {
        GTP5G_ERR(gtp->dev, "GTP-U version is not ver1: %#x\n",
            gtpv1->flags);
        return 1;
    }

    if (gtpv1->type != GTP_TPDU) {
        GTP5G_ERR(gtp->dev, "GTP-U message type is not a TPDU: %#x\n",
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

    //GTP5G_ERR(gtp->dev, "Total header len(%#x)\n", hdrlen);
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

/**
 * Entry function for Uplink packets
 *
 * UDP encapsulation receive handler. See net/ipv4/udp.c
 * Return codes: 
 *  =0 : if skb was successfully passed to the encap handler or
 *       was discarded by it
 *  >0 : if skb should be passed on to UDP
 *  <0 : if skb should be resubmitted as proto -N
 * */
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

static struct sock *gtp5g_encap_enable_socket(int fd, int type, 
    struct gtp5g_dev *gtp)
{
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
    tuncfg.encap_destroy = gtp5g_encap_destroy;

    setup_udp_tunnel_sock(sock_net(sock->sk), sock, &tuncfg);

out_sock:
    release_sock(sock->sk);
    sockfd_put(sock);
    return sk;
}

static int gtp5g_encap_enable(struct gtp5g_dev *gtp, struct nlattr *data[]) 
{
    struct sock *sk = NULL;
    unsigned int role = GTP5G_ROLE_UPF;

    if (data[IFLA_GTP5G_FD1]) {
        u32 fd1 = nla_get_u32(data[IFLA_GTP5G_FD1]);

        sk = gtp5g_encap_enable_socket(fd1, UDP_ENCAP_GTP1U, gtp);
        if (IS_ERR(sk))
            return PTR_ERR(sk);
    }

    if (data[IFLA_GTP5G_ROLE]) {
        role = nla_get_u32(data[IFLA_GTP5G_ROLE]);
        if (role > GTP5G_ROLE_RAN) {
            if (sk)
                gtp5g_encap_disable_sock(sk);
            return -EINVAL;
        }
    }

    gtp->sk1u = sk;
    gtp->role = role;

    return 0;
}

static int gtp5g_newlink(struct net *src_net, struct net_device *dev,
    struct nlattr *tb[], struct nlattr *data[],
    struct netlink_ext_ack *extack)
{
    struct gtp5g_dev *gtp;
    struct gtp5g_net *gn;
    int hashsize, err;

    if (!data[IFLA_GTP5G_FD1]) {
        GTP5G_ERR(NULL, "Failed to create a new link\n");
        return -EINVAL;
    }

    gtp = netdev_priv(dev);

    err = gtp5g_encap_enable(gtp, data);
    if (err < 0) {
        GTP5G_ERR(dev, "Failed to enable the encap rcv\n");
        return err;
    }

    if (!data[IFLA_GTP5G_PDR_HASHSIZE])
        hashsize = 1024;
    else
        hashsize = nla_get_u32(data[IFLA_GTP5G_PDR_HASHSIZE]);

    err = gtp5g_hashtable_new(gtp, hashsize);
    if (err < 0) {
        GTP5G_ERR(dev, "Failed to create a hash table\n");
        goto out_encap;
    }

    err = register_netdevice(dev);
    if (err < 0) {
        GTP5G_ERR(dev, "Failed to register new netdev err(%d)\n", err);
        goto out_hashtable;
    }

    gn = net_generic(dev_net(dev), gtp5g_net_id);
    list_add_rcu(&gtp->list, &gn->gtp5g_dev_list);
    list_add_rcu(&gtp->proc_list, &proc_gtp5g_dev);

    GTP5G_LOG(dev, "Registered a new 5G GTP interface\n");
    return 0;
out_hashtable:
    gtp5g_hashtable_free(gtp);
out_encap:
    gtp5g_encap_disable(gtp);
    return err;
}

static void gtp5g_dellink(struct net_device *dev, struct list_head *head)
{
    struct gtp5g_dev *gtp = netdev_priv(dev);

    gtp5g_hashtable_free(gtp);
    list_del_rcu(&gtp->list);
    list_del_rcu(&gtp->proc_list);
    unregister_netdevice_queue(dev, head);

    GTP5G_LOG(dev, "De-registered 5G GTP interface\n");
}

static size_t gtp5g_get_size(const struct net_device *dev)
{
    return nla_total_size(sizeof(__u32));    /* IFLA_GTP5G_PDR_HASHSIZE */
}

static int gtp5g_fill_info(struct sk_buff *skb, const struct net_device *dev)
{
    struct gtp5g_dev *gtp = netdev_priv(dev);

    if (nla_put_u32(skb, IFLA_GTP5G_PDR_HASHSIZE, gtp->hash_size))
        goto nla_put_failure;

    return 0;
nla_put_failure:
    return -EMSGSIZE;
}

static const struct nla_policy gtp5g_policy[IFLA_GTP5G_MAX + 1] = {
    [IFLA_GTP5G_FD1]             = { .type = NLA_U32 },
    [IFLA_GTP5G_PDR_HASHSIZE]    = { .type = NLA_U32 },
    [IFLA_GTP5G_ROLE]            = { .type = NLA_U32 },
};

static struct rtnl_link_ops gtp5g_link_ops __read_mostly = {
    .kind         = "gtp5g",
    .maxtype      = IFLA_GTP5G_MAX,
    .policy       = gtp5g_policy,
    .priv_size    = sizeof(struct gtp5g_dev),
    .setup        = gtp5g_link_setup,
    .validate     = gtp5g_validate,
    .newlink      = gtp5g_newlink,
    .dellink      = gtp5g_dellink,
    .get_size     = gtp5g_get_size,
    .fill_info    = gtp5g_fill_info,
};

static struct genl_family gtp5g_genl_family;

static int gtp5g_gnl_add_pdr(struct gtp5g_dev *gtp, struct genl_info *info)
{
    struct net_device *dev = gtp->dev;
    struct gtp5g_pdr *pdr;
    int err = 0;
    u32 pdr_id;
    u64 seid_int;
    char seid_pdr_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};
    
    if (info->attrs[GTP5G_PDR_SEID])
        seid_int = nla_get_u64(info->attrs[GTP5G_PDR_SEID]);
    else   
        seid_int = 0;

    pdr_id = nla_get_u16(info->attrs[GTP5G_PDR_ID]);
    pdr = find_pdr_by_id(gtp, seid_int, pdr_id);
    if (pdr) {
        if (info->nlhdr->nlmsg_flags & NLM_F_EXCL) {
            GTP5G_ERR(dev, "PDR-Up: Failed NLM_F_EXCL is set\n");
            err = -EEXIST;
            goto out;
        } else if (!(info->nlhdr->nlmsg_flags & NLM_F_REPLACE)) {
            GTP5G_ERR(dev, "PDR-Up: Failed NLM_F_REPLACE is not set\n");
            err = -EOPNOTSUPP;
            goto out;
        }

        err = pdr_fill(pdr, gtp, info);
        if (err < 0) {
            GTP5G_ERR(dev, "PDR-Up: update id(%u) fail\n", pdr_id);
            pdr_context_delete(pdr);
            return err;
        }
        
        GTP5G_INF(dev, "PDR-Up: update id(%u) success\n", pdr_id);
        return 0;
    }

    if (info->nlhdr->nlmsg_flags & NLM_F_REPLACE) {
        GTP5G_ERR(dev, "PDR-Add: Failed NLM_F_REPLACE is set\n");
        err = -ENOENT;
        goto out;
    }

    if (info->nlhdr->nlmsg_flags & NLM_F_APPEND) {
        GTP5G_ERR(dev, "PDR-Add: Failed NLM_F_APPEND is set\n");
        err = -EOPNOTSUPP;
        goto out;
    }

    // Check only at the creation part
    if (!info->attrs[GTP5G_PDR_PRECEDENCE]) {
        GTP5G_ERR(dev, "PDR-Add: Precedence is not given\n");
        err = -EINVAL;
        goto out;
    }

    pdr = kzalloc(sizeof(*pdr), GFP_ATOMIC);
    if (!pdr) {
        GTP5G_ERR(dev, "PDR-Add: Failed to allocate memory\n");
        err = -ENOMEM;
        goto out;
    }

    sock_hold(gtp->sk1u);
    pdr->sk = gtp->sk1u;
    pdr->dev = gtp->dev;

    err = pdr_fill(pdr, gtp, info);
    if (err < 0) {
        GTP5G_ERR(dev, "PDR-Add: failed to fill id(%u) err(%d)\n", pdr_id, err);
        pdr_context_delete(pdr);
        goto out;
    } 
    seid_pdr_id_to_hex_str(seid_int, pdr_id, seid_pdr_id_hexstr);
    hlist_add_head_rcu(&pdr->hlist_id, 
        &gtp->pdr_id_hash[str_hashfn(seid_pdr_id_hexstr) % gtp->hash_size]);

    GTP5G_INF(dev, "PDR-Add: id(%u) success\n", pdr_id);

out:
    return err;
}

static int gtp5g_genl_add_pdr(struct sk_buff *skb, struct genl_info *info)
{
    struct gtp5g_dev *gtp;
    int err = 0;

    if (!info->attrs[GTP5G_PDR_ID] ||
        !info->attrs[GTP5G_LINK]) {
        GTP5G_ERR(NULL, "PDR-Add: ID or LINK value is not exists\n");
        return -EINVAL;
    }

    rtnl_lock();
    rcu_read_lock();

    gtp = gtp5g_find_dev(sock_net(skb->sk), info->attrs);
    if (!gtp) {
        GTP5G_ERR(NULL, "PDR-Add: Failed to find the gtp5g dev\n");
        err = -ENODEV;
        goto unlock;
    }

    err = gtp5g_gnl_add_pdr(gtp, info);

unlock:
    rcu_read_unlock();
    rtnl_unlock();
    return err;
}

static int gtp5g_genl_del_pdr(struct sk_buff *skb, struct genl_info *info)
{
    u16 id;
    struct gtp5g_pdr *pdr;
    int err = 0;

    if (!info->attrs[GTP5G_PDR_ID] ||
        !info->attrs[GTP5G_LINK]) {
        GTP5G_ERR(NULL, "PDR-Del: ID or LINK is not present\n");
        return -EINVAL;
    }

    id = nla_get_u16(info->attrs[GTP5G_PDR_ID]);

    rcu_read_lock();

    pdr = gtp5g_find_pdr(sock_net(skb->sk), info->attrs);
    if (IS_ERR(pdr)) {
        GTP5G_ERR(NULL, "PDR-Del: failed to find id(%u)\n", id);
        err = PTR_ERR(pdr);
        goto unlock;
    }

    GTP5G_INF(pdr->dev, "PDR-Del: id(%u) success\n", id);
    pdr_context_delete(pdr);

unlock:
    rcu_read_unlock();
    return err;
}

static int gtp5g_genl_fill_pdr(struct sk_buff *skb, u32 snd_portid, u32 snd_seq,
    u32 type, struct gtp5g_pdr *pdr)
{
    void *genlh;
    struct nlattr *nest_pdi, *nest_f_teid, *nest_sdf, *nest_rule;
    struct gtp5g_pdi *pdi;
    struct local_f_teid *f_teid;
    struct sdf_filter *sdf;
    struct ip_filter_rule *rule;

    int i;
    u32 *u32_buf = kzalloc(0xff * sizeof(u32), GFP_KERNEL);
    if (!u32_buf) {
        GTP5G_ERR(NULL, "Failed to allocate memory\n");
        goto out;
    }

    genlh = genlmsg_put(skb, snd_portid, snd_seq, &gtp5g_genl_family, 0, type);
    if (!genlh) 
        goto genlmsg_fail;

    if (nla_put_u16(skb, GTP5G_PDR_ID, pdr->id) ||
        nla_put_u32(skb, GTP5G_PDR_PRECEDENCE, pdr->precedence))
        goto genlmsg_fail;

    if (pdr->seid) {
        if (nla_put_u64_64bit(skb, GTP5G_PDR_SEID, pdr->seid, 0))
            goto genlmsg_fail;
    }

    if (pdr->outer_header_removal) {
        if (nla_put_u8(skb, GTP5G_OUTER_HEADER_REMOVAL, *pdr->outer_header_removal))
            goto genlmsg_fail;
    }

    if (pdr->far_id) {
        if (nla_put_u32(skb, GTP5G_PDR_FAR_ID, *pdr->far_id))
            goto genlmsg_fail;
    }

    if (pdr->qer_id) {
        if (nla_put_u32(skb, GTP5G_PDR_QER_ID, *pdr->qer_id))
            goto genlmsg_fail;
    }

    if (pdr->role_addr_ipv4.s_addr) {
        if (nla_put_u32(skb, GTP5G_PDR_ROLE_ADDR_IPV4, pdr->role_addr_ipv4.s_addr))
            goto genlmsg_fail;
    }

    if (pdr->pdi) {
        if (!(nest_pdi = nla_nest_start(skb, GTP5G_PDR_PDI)))
            goto genlmsg_fail;

        pdi = pdr->pdi;
        if (pdi->ue_addr_ipv4) {
            if (nla_put_be32(skb, GTP5G_PDI_UE_ADDR_IPV4, pdi->ue_addr_ipv4->s_addr))
                goto genlmsg_fail;
        }

        if (pdi->f_teid) {
            if (!(nest_f_teid = nla_nest_start(skb, GTP5G_PDI_F_TEID)))
                goto genlmsg_fail;

            f_teid = pdi->f_teid;
            if (nla_put_u32(skb, GTP5G_F_TEID_I_TEID, ntohl(f_teid->teid)) ||
                nla_put_be32(skb, GTP5G_F_TEID_GTPU_ADDR_IPV4, f_teid->gtpu_addr_ipv4.s_addr))
                goto genlmsg_fail;

            nla_nest_end(skb, nest_f_teid);
        }

        if (pdi->sdf) {
            if (!(nest_sdf = nla_nest_start(skb, GTP5G_PDI_SDF_FILTER)))
                goto genlmsg_fail;

            sdf = pdi->sdf;
            if (sdf->rule) {
                if (!(nest_rule = nla_nest_start(skb, GTP5G_SDF_FILTER_FLOW_DESCRIPTION)))
                    goto genlmsg_fail;
                rule = sdf->rule;

                if (nla_put_u8(skb, GTP5G_FLOW_DESCRIPTION_ACTION, rule->action) ||
                    nla_put_u8(skb, GTP5G_FLOW_DESCRIPTION_DIRECTION, rule->direction) ||
                    nla_put_u8(skb, GTP5G_FLOW_DESCRIPTION_PROTOCOL, rule->proto) ||
                    nla_put_be32(skb, GTP5G_FLOW_DESCRIPTION_SRC_IPV4, rule->src.s_addr) ||
                    nla_put_be32(skb, GTP5G_FLOW_DESCRIPTION_DEST_IPV4, rule->dest.s_addr))
                    goto genlmsg_fail;

                if (rule->smask.s_addr != -1)
                    if (nla_put_be32(skb, GTP5G_FLOW_DESCRIPTION_SRC_MASK, rule->smask.s_addr))
                        goto genlmsg_fail;

                if (rule->dmask.s_addr != -1)
                    if (nla_put_be32(skb, GTP5G_FLOW_DESCRIPTION_DEST_MASK, rule->dmask.s_addr))
                        goto genlmsg_fail;

                if (rule->sport_num && rule->sport) {
                    for (i = 0; i < rule->sport_num; i++)
                        u32_buf[i] = rule->sport[i].start + (rule->sport[i].end << 16);
                    if (nla_put(skb, GTP5G_FLOW_DESCRIPTION_SRC_PORT,
                        rule->sport_num * sizeof(u32) / sizeof(char), u32_buf))
                        goto genlmsg_fail;
                }

                if (rule->dport_num && rule->dport) {
                    for (i = 0; i < rule->dport_num; i++)
                        u32_buf[i] = rule->dport[i].start + (rule->dport[i].end << 16);
                    if (nla_put(skb, GTP5G_FLOW_DESCRIPTION_DEST_PORT,
                        rule->dport_num * sizeof(u32) / sizeof(char), u32_buf))
                        goto genlmsg_fail;
                }

                nla_nest_end(skb, nest_rule);
            }

            if (sdf->tos_traffic_class)
                if (nla_put_u16(skb, GTP5G_SDF_FILTER_TOS_TRAFFIC_CLASS, *sdf->tos_traffic_class))
                    goto genlmsg_fail;

            if (sdf->security_param_idx)
                if (nla_put_u32(skb, GTP5G_SDF_FILTER_SECURITY_PARAMETER_INDEX, *sdf->security_param_idx))
                    goto genlmsg_fail;

            if (sdf->flow_label)
                if (nla_put_u32(skb, GTP5G_SDF_FILTER_FLOW_LABEL, *sdf->flow_label))
                    goto genlmsg_fail;

            if (sdf->bi_id)
                if (nla_put_u32(skb, GTP5G_SDF_FILTER_SDF_FILTER_ID, *sdf->bi_id))
                    goto genlmsg_fail;

            nla_nest_end(skb, nest_sdf);
        }

        nla_nest_end(skb, nest_pdi);
    }

    kfree(u32_buf);
    genlmsg_end(skb, genlh);

    return 0;
genlmsg_fail:
    kfree(u32_buf);
    genlmsg_cancel(skb, genlh);
out:
    return -EMSGSIZE;
}

static int gtp5g_genl_get_pdr(struct sk_buff *skb, struct genl_info *info)
{
    struct gtp5g_pdr *pdr;
    struct sk_buff *skb_ack;
    int err;

    if (!info->attrs[GTP5G_PDR_ID]) {
        GTP5G_ERR(NULL, "PDR ID is not given\n");
        return -EINVAL;
    }

    rcu_read_lock();
    pdr = gtp5g_find_pdr(sock_net(skb->sk), info->attrs);
    if (IS_ERR(pdr)) {
        GTP5G_ERR(NULL, "PDR record is not present\n");
        err = PTR_ERR(pdr);
        goto unlock;
    }

    skb_ack = genlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC);
    if (!skb_ack) {
        GTP5G_ERR(NULL, "Failed to allocate skb ack\n");
        err = -ENOMEM;
        goto unlock;
    }

    err = gtp5g_genl_fill_pdr(skb_ack, 
        NETLINK_CB(skb).portid,
        info->snd_seq, 
        info->nlhdr->nlmsg_type, 
        pdr);
    if (err < 0) {
        GTP5G_ERR(NULL, "Failed to fill PDR err(%d)\n", err);
        goto freebuf;
    }
    rcu_read_unlock();

    return genlmsg_unicast(genl_info_net(info), skb_ack, info->snd_portid);
freebuf:
    kfree_skb(skb_ack);
unlock:
    rcu_read_unlock();
    return err;
}

static int gtp5g_genl_dump_pdr(struct sk_buff *skb, struct netlink_callback *cb)
{
    /* netlink_callback->args
     * args[0] : index of gtp5g dev id
     * args[1] : index of gtp5g hash entry id in dev
     * args[2] : index of gtp5g pdr id
     * args[5] : set non-zero means it is finished
     */
    struct gtp5g_dev *gtp, *last_gtp = (struct gtp5g_dev *)cb->args[0];
    struct net *net = sock_net(skb->sk);
    struct gtp5g_net *gn = net_generic(net, gtp5g_net_id);
    int i, last_hash_entry_id = cb->args[1], ret;
    u16 pdr_id = cb->args[2];
    struct gtp5g_pdr *pdr;

    if (cb->args[5]) {
        GTP5G_ERR(NULL, "Failed to dump callback args[5] is present\n");
        return 0;
    }

    list_for_each_entry_rcu(gtp, &gn->gtp5g_dev_list, list) {
        if (last_gtp && last_gtp != gtp)
            continue;
        else
            last_gtp = NULL;

        for (i = last_hash_entry_id; i < gtp->hash_size; i++) {
            hlist_for_each_entry_rcu(pdr, &gtp->pdr_id_hash[i], hlist_id) {
                if (pdr_id && pdr_id != pdr->id)
                    continue;
                else
                    pdr_id = 0;

                ret = gtp5g_genl_fill_pdr(skb, 
                    NETLINK_CB(cb->skb).portid,
                    cb->nlh->nlmsg_seq, 
                    cb->nlh->nlmsg_type, 
                    pdr);
                if (ret < 0) {
                    cb->args[0] = (unsigned long) gtp;
                    cb->args[1] = i;
                    cb->args[2] = pdr->id;
                    goto out;
                }
            }
        }
    }
    cb->args[5] = 1;

out:
    return skb->len;
}

static int gtp5g_gnl_add_far(struct gtp5g_dev *gtp, struct genl_info *info)
{
    struct net_device *dev = gtp->dev;
    struct gtp5g_far *far;
    struct gtp5g_emark_pktinfo epkt_info;
    int err = 0;
    u32 far_id;
    u8  flag;

    u64 seid_int;
    char seid_far_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};

    if (info->attrs[GTP5G_FAR_SEID])
        seid_int = nla_get_u64(info->attrs[GTP5G_FAR_SEID]);
    else
        seid_int = 0;

    far_id = nla_get_u32(info->attrs[GTP5G_FAR_ID]);
    far = find_far_by_id(gtp, seid_int, far_id);
    if (far) {
        if (info->nlhdr->nlmsg_flags & NLM_F_EXCL) {
            GTP5G_ERR(dev, "FAR-Up: Failed NLM_F_EXCL is set\n");
            err = -EEXIST;
            goto out;
        } else if (!(info->nlhdr->nlmsg_flags & NLM_F_REPLACE)) {
            GTP5G_ERR(dev, "FAR-Up: Failed NLM_F_REPLACE is not set\n");
            err = -EOPNOTSUPP;
            goto out;
        }

        flag = 0;
        err = far_fill(far, gtp, info, &flag, &epkt_info);
        if (err < 0) {
            far_context_delete(far);
            GTP5G_ERR(dev,"FAR-Up: failed to update id(%u) err: %d\n", far_id, err);
            goto out;
        }

        // Send GTP-U End marker to gNB
        if (flag) {
            /* SKB size GTPU(8) + UDP(8) + IP(20) + Eth(14)  
             * + 2-Bytes align the IP header 
             * */
            struct sk_buff *skb = __netdev_alloc_skb(dev, 52, GFP_KERNEL);
            if (!skb) {
                GTP5G_ERR(dev, "FAR-Up: Failled to allocate EndMarker SKB with a size 52B\n");
                err = 0;
                goto out;
            }
            skb_reserve(skb, 2);
            skb->protocol = eth_type_trans(skb, dev);
            gtp5g_fwd_emark_skb_ipv4(skb, dev, &epkt_info);
       }
       GTP5G_INF(dev, "FAR-Up: update id(%u) success\n", far_id);
       return 0;
    }

    if (info->nlhdr->nlmsg_flags & NLM_F_REPLACE) {
        GTP5G_ERR(dev, "FAR-Add: Failed NLM_F_REPLACE is set\n");
        err = -ENOENT;
        goto out;
    }

    if (info->nlhdr->nlmsg_flags & NLM_F_APPEND) {
        GTP5G_ERR(dev, "FAR-Add: Failed NLM_F_APPEND is set\n");
        err = -EOPNOTSUPP;
        goto out;
    }

    // Check only at the creation part
    if (!info->attrs[GTP5G_FAR_APPLY_ACTION]) {
        GTP5G_ERR(dev, "FAR-Add: Failed apply action is not given\n");
        err = -EINVAL;
        goto out;
    }

    far = kzalloc(sizeof(*far), GFP_ATOMIC);
    if (!far) {
        GTP5G_ERR(dev, "FAR-Add: Failed to allocate memory\n");
        err = -ENOMEM;
        goto out;
    }
    far->dev = dev;

    err = far_fill(far, gtp, info, NULL, NULL);
    if (err < 0) {
        GTP5G_ERR(dev, "FAR-Add: id(%u) fail\n", far_id);
        far_context_delete(far);
        goto out;
    } 

    seid_far_id_to_hex_str(seid_int, far_id, seid_far_id_hexstr);
    hlist_add_head_rcu(&far->hlist_id,
        &gtp->far_id_hash[str_hashfn(seid_far_id_hexstr) % gtp->hash_size]);
    GTP5G_INF(dev, "FAR-Add: id(%u) success\n", far_id);

out:
    return err;
}

static int gtp5g_genl_add_far(struct sk_buff *skb, struct genl_info *info)
{
    struct gtp5g_dev *gtp;
    int err = 0;

    if (!info->attrs[GTP5G_FAR_ID] ||
        !info->attrs[GTP5G_LINK]) {
        GTP5G_ERR(NULL, "Failed to find FAR_ID or LINK in netlink\n");
        return -EINVAL;
    }

    rtnl_lock();
    rcu_read_lock();

    gtp = gtp5g_find_dev(sock_net(skb->sk), info->attrs);
    if (!gtp) {
        GTP5G_ERR(NULL, "Failed to find the gtp5g_dev\n");
        err = -ENODEV;
        goto unlock;
    }

    err = gtp5g_gnl_add_far(gtp, info);

unlock:
    rcu_read_unlock();
    rtnl_unlock();
    return err;
}

static int gtp5g_genl_del_far(struct sk_buff *skb, struct genl_info *info)
{
    u32 id;
    struct gtp5g_far *far;
    int err = 0;

    if (!info->attrs[GTP5G_FAR_ID] ||
        !info->attrs[GTP5G_LINK]) {
        GTP5G_ERR(NULL, "FAR-Del: Failed to find FAR_ID or LINK in netlink\n");
        return -EINVAL;
    }

    id = nla_get_u32(info->attrs[GTP5G_FAR_ID]);

    rcu_read_lock();

    far = gtp5g_find_far(sock_net(skb->sk), info->attrs);
    if (IS_ERR(far)) {
        GTP5G_ERR(NULL, "FAR-Del: Failed to find id (%u)\n", id);
        err = PTR_ERR(far);
        goto unlock;
    }

    GTP5G_INF(far->dev, "FAR-Del: id(%u) success\n", id);
    far_context_delete(far);

unlock:
    rcu_read_unlock();
    return err;
}

static int gtp5g_genl_fill_far(struct sk_buff *skb, u32 snd_portid, u32 snd_seq,
    u32 type, struct gtp5g_far *far)
{
    void *genlh;
    struct nlattr *nest_fwd_param, *nest_hdr_creation;
    struct forwarding_parameter *fwd_param;
    struct outer_header_creation *hdr_creation;
    struct forwarding_policy *fwd_policy;

    char seid_far_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};

    int cnt;
    struct gtp5g_dev *gtp = netdev_priv(far->dev);
    struct hlist_head *head;
    struct gtp5g_pdr *pdr;
    u16 *u16_buf = kzalloc(0xff * sizeof(u16), GFP_KERNEL);
    if (!u16_buf) {
        GTP5G_ERR(NULL, "Failed to allocate buf\n");
        goto out;
    }

    genlh = genlmsg_put(skb, snd_portid, snd_seq, &gtp5g_genl_family, 0, type);
    if (!genlh)
        goto genlmsg_fail;

    if (nla_put_u32(skb, GTP5G_FAR_ID, far->id) ||
        nla_put_u8(skb, GTP5G_FAR_APPLY_ACTION, far->action))
        goto genlmsg_fail;

    if (far->seid) {
        if (nla_put_u64_64bit(skb, GTP5G_FAR_SEID, far->seid, 0))
            goto genlmsg_fail;
    }
    if (far->fwd_param) {
        if (!(nest_fwd_param = nla_nest_start(skb, GTP5G_FAR_FORWARDING_PARAMETER)))
            goto genlmsg_fail;

        fwd_param = far->fwd_param;
        if (fwd_param->hdr_creation) {
            if (!(nest_hdr_creation = nla_nest_start(skb, GTP5G_FORWARDING_PARAMETER_OUTER_HEADER_CREATION)))
                goto genlmsg_fail;

            hdr_creation = fwd_param->hdr_creation;
            if (nla_put_u16(skb, GTP5G_OUTER_HEADER_CREATION_DESCRIPTION, hdr_creation->description) ||
                nla_put_u32(skb, GTP5G_OUTER_HEADER_CREATION_O_TEID, ntohl(hdr_creation->teid)) ||
                nla_put_be32(skb, GTP5G_OUTER_HEADER_CREATION_PEER_ADDR_IPV4, hdr_creation->peer_addr_ipv4.s_addr) ||
                nla_put_u16(skb, GTP5G_OUTER_HEADER_CREATION_PORT, ntohs(hdr_creation->port)))
                goto genlmsg_fail;

            nla_nest_end(skb, nest_hdr_creation);
        }

        if ((fwd_policy = fwd_param->fwd_policy))
            if (nla_put(skb, GTP5G_FORWARDING_PARAMETER_FORWARDING_POLICY, fwd_policy->len, fwd_policy->identifier))
                goto genlmsg_fail;

        nla_nest_end(skb, nest_fwd_param);
    }

    cnt = 0;    
    seid_far_id_to_hex_str(far->seid, far->id, seid_far_id_hexstr);
    head = &gtp->related_far_hash[str_hashfn(seid_far_id_hexstr) % gtp->hash_size];
    hlist_for_each_entry_rcu(pdr, head, hlist_related_far) {
        if (cnt >= 0xff)
            goto genlmsg_fail;

        if (pdr->seid == far->seid && *pdr->far_id == far->id)
            u16_buf[cnt++] = pdr->id;
    }

    if (cnt) {
        if (nla_put(skb, GTP5G_FAR_RELATED_TO_PDR, cnt * sizeof(u16) / sizeof(char), u16_buf))
            goto genlmsg_fail;
    }

    kfree(u16_buf);
    genlmsg_end(skb, genlh);
    return 0;
genlmsg_fail:
    kfree(u16_buf);
    genlmsg_cancel(skb, genlh);
out:
    return -EMSGSIZE;
}

static int gtp5g_genl_get_far(struct sk_buff *skb, struct genl_info *info)
{
    struct gtp5g_far *far;
    struct sk_buff *skb_ack;
    int err;

    if (!info->attrs[GTP5G_FAR_ID]) {
        GTP5G_ERR(NULL, "Failed to find FAR_ID in netlink msg\n");
        return -EINVAL;
    }
    rcu_read_lock();

    far = gtp5g_find_far(sock_net(skb->sk), info->attrs);
    if (IS_ERR(far)) {
        GTP5G_ERR(NULL, "Failed to find far\n");
        err = PTR_ERR(far);
        goto unlock;
    }

    skb_ack = genlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC);
    if (!skb_ack) {
        GTP5G_ERR(NULL, "Failed to allocate skb_ack\n");
        err = -ENOMEM;
        goto unlock;
    }

    err = gtp5g_genl_fill_far(skb_ack, 
        NETLINK_CB(skb).portid,
        info->snd_seq, 
        info->nlhdr->nlmsg_type, 
        far);
    if (err < 0) {
        GTP5G_ERR(NULL, "Failed to fill far\n");
        goto freebuf;
    }

    rcu_read_unlock();

    return genlmsg_unicast(genl_info_net(info), skb_ack, info->snd_portid);
freebuf:
    kfree_skb(skb_ack);
unlock:
    rcu_read_unlock();
    return err;
}

static int gtp5g_genl_dump_far(struct sk_buff *skb, struct netlink_callback *cb)
{
    /* netlink_callback->args
     * args[0] : index of gtp5g dev id
     * args[1] : index of gtp5g hash entry id in dev
     * args[2] : index of gtp5g far id
     * args[5] : set non-zero means it is finished
     */
    struct gtp5g_dev *gtp, *last_gtp = (struct gtp5g_dev *)cb->args[0];
    struct net *net = sock_net(skb->sk);
    struct gtp5g_net *gn = net_generic(net, gtp5g_net_id);
    int i, last_hash_entry_id = cb->args[1], ret;
    u32 far_id = cb->args[2];
    struct gtp5g_far *far;

    if (cb->args[5]) {
        GTP5G_ERR(NULL, "Failed to dump FAR arg5 present\n");
        return 0;
    }

    list_for_each_entry_rcu(gtp, &gn->gtp5g_dev_list, list) {
        if (last_gtp && last_gtp != gtp)
            continue;
        else
            last_gtp = NULL;

        for (i = last_hash_entry_id; i < gtp->hash_size; i++) {
            hlist_for_each_entry_rcu(far, &gtp->far_id_hash[i], hlist_id) {
                if (far_id && far_id != far->id)
                    continue;
                else
                    far_id = 0;

                ret = gtp5g_genl_fill_far(skb, 
                    NETLINK_CB(cb->skb).portid,
                    cb->nlh->nlmsg_seq, 
                    cb->nlh->nlmsg_type, 
                    far);
                if (ret < 0) {
                    cb->args[0] = (unsigned long) gtp;
                    cb->args[1] = i;
                    cb->args[2] = far->id;
                    goto out;
                }
            }
        }
    }
    cb->args[5] = 1;

out:
    return skb->len;
}

/** ---------------------------------------------------------------------
 *                                 QER
 *  ---------------------------------------------------------------------
 * */
static void qer_context_free(struct rcu_head *head)
{
    struct gtp5g_qer *qer = container_of(head, struct gtp5g_qer, rcu_head);

    if (!qer)
        return;

    kfree(qer);
}

static void qer_context_delete(struct gtp5g_qer *qer)
{
    struct gtp5g_dev *gtp;
    struct hlist_head *head;
    struct gtp5g_pdr *pdr;

    char seid_qer_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};

    if (!qer)
        return;

    gtp = netdev_priv(qer->dev);

    if (!hlist_unhashed(&qer->hlist_id))
        hlist_del_rcu(&qer->hlist_id);

    seid_qer_id_to_hex_str(qer->seid, qer->id, seid_qer_id_hexstr);
    head = &gtp->related_qer_hash[str_hashfn(seid_qer_id_hexstr) % gtp->hash_size];
    hlist_for_each_entry_rcu(pdr, head, hlist_related_qer) {
        if (*pdr->qer_id == qer->id) {
            pdr->qer = NULL;
            unix_sock_client_delete(pdr);
        }
    }

    call_rcu(&qer->rcu_head, qer_context_free);
}

static int qer_fill(struct gtp5g_qer *qer, struct gtp5g_dev *gtp, struct genl_info *info)
{
    struct nlattr *mbr_param_attrs[GTP5G_QER_MBR_ATTR_MAX + 1];
    struct nlattr *gbr_param_attrs[GTP5G_QER_GBR_ATTR_MAX + 1];
    struct gtp5g_pdr *pdr;
    struct hlist_head *head;

    char seid_qer_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};

    qer->id = nla_get_u32(info->attrs[GTP5G_QER_ID]);
    if (info->attrs[GTP5G_QER_SEID])
        qer->seid = nla_get_u64(info->attrs[GTP5G_QER_SEID]);
    else
        qer->seid = 0;
    

    if (info->attrs[GTP5G_QER_GATE]) {
        qer->ul_dl_gate = nla_get_u8(info->attrs[GTP5G_QER_GATE]);
    }

    /* MBR */
    if (info->attrs[GTP5G_QER_MBR] &&
        !nla_parse_nested(mbr_param_attrs, GTP5G_QER_MBR_ATTR_MAX, info->attrs[GTP5G_QER_MBR], NULL, NULL)) {
        qer->mbr.ul_high = nla_get_u32(mbr_param_attrs[GTP5G_QER_MBR_UL_HIGH32]);
        qer->mbr.ul_low  = nla_get_u8(mbr_param_attrs[GTP5G_QER_MBR_UL_LOW8]);
        qer->mbr.dl_high = nla_get_u32(mbr_param_attrs[GTP5G_QER_MBR_DL_HIGH32]);
        qer->mbr.dl_low  = nla_get_u8(mbr_param_attrs[GTP5G_QER_MBR_DL_LOW8]);
    }

    /* GBR */
    if (info->attrs[GTP5G_QER_GBR] &&
        !nla_parse_nested(gbr_param_attrs, GTP5G_QER_GBR_ATTR_MAX, info->attrs[GTP5G_QER_GBR], NULL, NULL)) {
        qer->gbr.ul_high = nla_get_u32(gbr_param_attrs[GTP5G_QER_GBR_UL_HIGH32]);
        qer->gbr.ul_low  = nla_get_u8(gbr_param_attrs[GTP5G_QER_GBR_UL_LOW8]);
        qer->gbr.dl_high = nla_get_u32(gbr_param_attrs[GTP5G_QER_GBR_DL_HIGH32]);
        qer->gbr.dl_low  = nla_get_u8(gbr_param_attrs[GTP5G_QER_GBR_DL_LOW8]);
    }

    if (info->attrs[GTP5G_QER_CORR_ID]) {
        qer->qer_corr_id = nla_get_u32(info->attrs[GTP5G_QER_CORR_ID]);
    }

    if (info->attrs[GTP5G_QER_RQI]) {
        qer->rqi = nla_get_u8(info->attrs[GTP5G_QER_RQI]);
    }

    if (info->attrs[GTP5G_QER_QFI]) {
        qer->qfi = nla_get_u8(info->attrs[GTP5G_QER_QFI]);
    }

    if (info->attrs[GTP5G_QER_PPI]) {
        qer->ppi = nla_get_u8(info->attrs[GTP5G_QER_PPI]);
    }

    if (info->attrs[GTP5G_QER_RCSR]) {
        qer->rcsr = nla_get_u8(info->attrs[GTP5G_QER_RCSR]);
    }

    /* Update PDRs which has not linked to this QER */
    seid_qer_id_to_hex_str(qer->seid, qer->id, seid_qer_id_hexstr);
    head = &gtp->related_qer_hash[str_hashfn(seid_qer_id_hexstr) % gtp->hash_size];
    hlist_for_each_entry_rcu(pdr, head, hlist_related_qer) {
        if (*pdr->qer_id == qer->id) {
            pdr->qer = qer;
            if (unix_sock_client_update(pdr) < 0)
                GTP5G_ERR(gtp->dev, "PDR(%u) update fail when QER(%u) apply action is changed\n",
                    pdr->id, qer->id);
        }
    }

    return 0;
}

static struct gtp5g_qer *find_qer_by_id(struct gtp5g_dev *gtp, u64 seid, u32 qer_id)
{
    struct hlist_head *head;
    struct gtp5g_qer *qer;
    char seid_qer_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};

    seid_qer_id_to_hex_str(seid, qer_id, seid_qer_id_hexstr);
    head = &gtp->qer_id_hash[str_hashfn(seid_qer_id_hexstr) % gtp->hash_size];
    hlist_for_each_entry_rcu(qer, head, hlist_id) {
        if (qer->seid == seid && qer->id == qer_id)
            return qer;
    }

    return NULL;
}

static int gtp5g_gnl_add_qer(struct gtp5g_dev *gtp, struct genl_info *info)
{
    struct net_device *dev = gtp->dev;
    struct gtp5g_qer *qer;
    int err = 0;
    u32 qer_id;

    u64 seid_int;
    char seid_qer_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};

    if (!dev) {
        GTP5G_ERR(NULL, "QER-Add: net_device of gtp is not set\n");
        return -EEXIST;
    }

    if (info->attrs[GTP5G_QER_SEID])
        seid_int = nla_get_u64(info->attrs[GTP5G_QER_SEID]);
    else
        seid_int = 0;
    
    qer_id = nla_get_u32(info->attrs[GTP5G_QER_ID]);
    qer = find_qer_by_id(gtp, seid_int, qer_id);
    if (qer) {
        if (info->nlhdr->nlmsg_flags & NLM_F_EXCL) {
            GTP5G_ERR(NULL, "QER-Up: Failed NLM_F_EXCL is set\n");
            return -EEXIST;
        } else if (!(info->nlhdr->nlmsg_flags & NLM_F_REPLACE)) {
            GTP5G_ERR(NULL, "QER-Up: Failed NLM_F_REPLACE is not set\n");
            return -EOPNOTSUPP;
        }

        err = qer_fill(qer, gtp, info);
        if (err < 0) {
            qer_context_delete(qer);
            GTP5G_ERR(dev, "QER-Up: failed to update id(%u) err: %d\n", qer_id, err);
            return err;
        } 

        GTP5G_INF(dev, "QER-Up: update id(%u) success\n", qer_id);
        return 0;
    }

    if (info->nlhdr->nlmsg_flags & NLM_F_REPLACE) {
        GTP5G_ERR(dev, "QER-Add: Invalid flag set NLM_F_REPLACE\n");
        return -ENOENT;
    }

    if (info->nlhdr->nlmsg_flags & NLM_F_APPEND) {
        GTP5G_ERR(dev, "QER-Add: Invalid flag set NLM_F_APPEND\n");
        return -EOPNOTSUPP;
    }

    qer = kzalloc(sizeof(*qer), GFP_ATOMIC);
    if (!qer) {
        GTP5G_ERR(dev, "QER-Add: Failed to allocate memory for QER_ID(%u)\n", qer_id);
        return -ENOMEM;
    }

    qer->dev = dev;

    err = qer_fill(qer, gtp, info);
    if (err < 0) {
        GTP5G_ERR(dev, "QER-Add: QER_ID(%u) fail\n", qer_id);
        qer_context_delete(qer);
        goto out;
    } 

    seid_qer_id_to_hex_str(seid_int, qer_id, seid_qer_id_hexstr);
    hlist_add_head_rcu(&qer->hlist_id, 
        &gtp->qer_id_hash[str_hashfn(seid_qer_id_hexstr) % gtp->hash_size]);
    GTP5G_INF(dev, "QER-Add: QER_ID(%u) success\n", qer_id);
out:
    return err;
}

static int gtp5g_genl_add_qer(struct sk_buff *skb, struct genl_info *info)
{
    struct gtp5g_dev *gtp;
    int err = 0;

    if (!info->attrs[GTP5G_QER_ID] ||
        !info->attrs[GTP5G_LINK]) {
        GTP5G_ERR(NULL, "QER-Add: QER_ID or GTP5G_LINK is not present\n");
        return -EINVAL;
    }

    rtnl_lock();
    rcu_read_lock();

    gtp = gtp5g_find_dev(sock_net(skb->sk), info->attrs);
    if (!gtp) {
        GTP5G_ERR(NULL, "QER-Add: Unable to find the gtp device\n");
        err = -ENODEV;
        goto unlock;
    }

    err = gtp5g_gnl_add_qer(gtp, info);

unlock:
    rcu_read_unlock();
    rtnl_unlock();
    return err;
}

static int gtp5g_genl_del_qer(struct sk_buff *skb, struct genl_info *info)
{
    u32 id;
    struct gtp5g_qer *qer;
    int err = 0;

    if (!info->attrs[GTP5G_QER_ID] ||
        !info->attrs[GTP5G_LINK]) {
        GTP5G_ERR(NULL, "QER-Del: QER_ID or GTP5G_LINK is not present\n");
        return -EINVAL;
    }

    id = nla_get_u32(info->attrs[GTP5G_QER_ID]);

    rcu_read_lock();
    qer = gtp5g_find_qer(sock_net(skb->sk), info->attrs);
    if (IS_ERR(qer)) {
        err = PTR_ERR(qer);
        GTP5G_ERR(qer->dev, "QER-Del: failed to find id(%u)\n", id);
        goto unlock;
    }

    GTP5G_INF(qer->dev, "QER-Del: id(%u) success\n", id);
    qer_context_delete(qer);

unlock:
    rcu_read_unlock();
    return err;
}

static int gtp5g_genl_fill_qer(struct sk_buff *skb, u32 snd_portid, u32 snd_seq,
    u32 type, struct gtp5g_qer *qer)
{
    void *genlh;
    int cnt;
    struct nlattr *nest_mbr_param, *nest_gbr_param;
    struct gtp5g_dev *gtp = netdev_priv(qer->dev);
    struct hlist_head *head;
    struct gtp5g_pdr *pdr;

    char seid_qer_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};

    u16 *u16_buf = kzalloc(0xff * sizeof(u16), GFP_KERNEL);
    if (!u16_buf) {
        GTP5G_ERR(NULL, "Failed to allocated mmeory\n");
        return -EMSGSIZE;
    }

    genlh = genlmsg_put(skb, snd_portid, snd_seq, 
        &gtp5g_genl_family, 0, type);
    if (!genlh) {
        GTP5G_ERR(NULL, "Failed to get genlh snd_port_id(%#x)"
            " \t snd_seq(%#x) type(%#x)\n", snd_portid, snd_seq, type);
        goto genlmsg_fail;
    }

    /* QER_ID & GATE */
    if (nla_put_u32(skb, GTP5G_QER_ID, qer->id) ||
        nla_put_u8(skb, GTP5G_QER_GATE, qer->ul_dl_gate))
        goto genlmsg_fail;

    if (qer->seid) {
        if (nla_put_u64_64bit(skb, GTP5G_QER_SEID, qer->seid, 0))
            goto genlmsg_fail;
    }

    /* MBR */
    if (!(nest_mbr_param = nla_nest_start(skb, GTP5G_QER_MBR)))
        goto genlmsg_fail;

    if (nla_put_u32(skb, GTP5G_QER_MBR_UL_HIGH32, qer->mbr.ul_high) ||
        nla_put_u8(skb, GTP5G_QER_MBR_UL_LOW8, qer->mbr.ul_low) ||
        nla_put_u32(skb, GTP5G_QER_MBR_DL_HIGH32, qer->mbr.dl_high) ||
        nla_put_u8(skb, GTP5G_QER_MBR_DL_LOW8, qer->mbr.dl_low))
        goto genlmsg_fail;

    nla_nest_end(skb, nest_mbr_param);

    /* GBR */
    if (!(nest_gbr_param = nla_nest_start(skb, GTP5G_QER_GBR)))
        goto genlmsg_fail;

    if (nla_put_u32(skb, GTP5G_QER_GBR_UL_HIGH32, qer->gbr.ul_high) ||
        nla_put_u8(skb, GTP5G_QER_GBR_UL_LOW8, qer->gbr.ul_low) ||
        nla_put_u32(skb, GTP5G_QER_GBR_DL_HIGH32, qer->gbr.dl_high) ||
        nla_put_u8(skb, GTP5G_QER_GBR_DL_LOW8, qer->gbr.dl_low))
        goto genlmsg_fail;

    nla_nest_end(skb, nest_gbr_param);

    /* CORR_ID, RQI, QFI, PPI, RCSR */
    if (nla_put_u32(skb, GTP5G_QER_CORR_ID, qer->qer_corr_id) ||
        nla_put_u8(skb, GTP5G_QER_RQI, qer->rqi) ||
        nla_put_u8(skb, GTP5G_QER_QFI, qer->qfi) ||
        nla_put_u8(skb, GTP5G_QER_PPI, qer->ppi) ||
        nla_put_u8(skb, GTP5G_QER_RCSR, qer->rcsr))
        goto genlmsg_fail;

    cnt = 0;
    seid_qer_id_to_hex_str(qer->seid, qer->id, seid_qer_id_hexstr);
    head = &gtp->related_qer_hash[str_hashfn(seid_qer_id_hexstr) % gtp->hash_size];        
    hlist_for_each_entry_rcu(pdr, head, hlist_related_qer) {
        if (cnt >= 0xff)
            goto genlmsg_fail;

        if (*pdr->qer_id == qer->id)
            u16_buf[cnt++] = pdr->id;
    }
    if (cnt) {
        if (nla_put(skb, 
            GTP5G_QER_RELATED_TO_PDR,
            (cnt * sizeof(u16) / sizeof(char)), 
            u16_buf))
            goto genlmsg_fail;
    }

    kfree(u16_buf);
    genlmsg_end(skb, genlh);
    return 0;

genlmsg_fail:
    kfree(u16_buf);
    genlmsg_cancel(skb, genlh);
    return -EMSGSIZE;
}

static struct gtp5g_qer *gtp5g_find_qer_by_link(struct net *net, struct nlattr *nla[])
{
    struct gtp5g_dev *gtp;
    u64 seid;

    if (nla[GTP5G_QER_SEID])
        seid =  nla_get_u64(nla[GTP5G_QER_SEID]);
    else
        seid = 0;
    

    gtp = gtp5g_find_dev(net, nla);
    if (!gtp)
        return ERR_PTR(-ENODEV);

    if (nla[GTP5G_QER_ID]) {
        u32 id = nla_get_u32(nla[GTP5G_QER_ID]);
        return find_qer_by_id(gtp, seid, id);
    }

    return ERR_PTR(-EINVAL);
}

static struct gtp5g_qer *gtp5g_find_qer(struct net *net, struct nlattr *nla[])
{
    struct gtp5g_qer *qer;

    if (nla[GTP5G_LINK])
        qer = gtp5g_find_qer_by_link(net, nla);
    else
        qer = ERR_PTR(-EINVAL);

    if (!qer)
        qer = ERR_PTR(-ENOENT);

    return qer;
}

static int gtp5g_genl_get_qer(struct sk_buff *skb, struct genl_info *info)
{
    struct gtp5g_qer *qer;
    struct sk_buff *skb_ack;
    int err;

    if (!info->attrs[GTP5G_QER_ID]) {
        GTP5G_ERR(NULL, "QER ID is not present\n");
        return -EINVAL;
    }

    rcu_read_lock();
    qer = gtp5g_find_qer(sock_net(skb->sk), info->attrs);
    if (IS_ERR(qer)) {
        GTP5G_ERR(NULL, "Failed to find QER\n");
        err = PTR_ERR(qer);
        goto unlock;
    }

    skb_ack = genlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC);
    if (!skb_ack) {
        err = -ENOMEM;
        goto unlock;
    }

    err = gtp5g_genl_fill_qer(skb_ack, 
        NETLINK_CB(skb).portid,
        info->snd_seq, 
        info->nlhdr->nlmsg_type, 
        qer);
    if (err < 0) {
        GTP5G_ERR(NULL, "Failed to fil the qer\n");
        goto freebuf;
    }
    rcu_read_unlock();

    return genlmsg_unicast(genl_info_net(info), skb_ack, info->snd_portid);
freebuf:
    kfree_skb(skb_ack);
unlock:
    rcu_read_unlock();
    return err;
}

static int gtp5g_genl_dump_qer(struct sk_buff *skb, struct netlink_callback *cb)
{
    /* netlink_callback->args
     * args[0] : index of gtp5g dev id
     * args[1] : index of gtp5g hash entry id in dev
     * args[2] : index of gtp5g qer id
     * args[5] : set non-zero means it is finished
     */
    struct gtp5g_dev *gtp, *last_gtp = (struct gtp5g_dev *)cb->args[0];
    struct net *net = sock_net(skb->sk);
    struct gtp5g_net *gn = net_generic(net, gtp5g_net_id);
    int i, last_hash_entry_id = cb->args[1], ret;
    u32 qer_id = cb->args[2];
    struct gtp5g_qer *qer;

    if (cb->args[5]) {
        GTP5G_ERR(NULL, "Invalid args\n");
        return 0;
    }

    list_for_each_entry_rcu(gtp, &gn->gtp5g_dev_list, list) {
        if (last_gtp && last_gtp != gtp)
            continue;
        else
            last_gtp = NULL;

        for (i = last_hash_entry_id; i < gtp->hash_size; i++) {
            hlist_for_each_entry_rcu(qer, &gtp->qer_id_hash[i], hlist_id) {
                if (qer_id && qer_id != qer->id)
                    continue;
                qer_id = 0;
                ret = gtp5g_genl_fill_qer(skb, 
                    NETLINK_CB(cb->skb).portid,
                    cb->nlh->nlmsg_seq, 
                    cb->nlh->nlmsg_type, 
                    qer);
                if (ret < 0) {
                    cb->args[0] = (unsigned long) gtp;
                    cb->args[1] = i;
                    cb->args[2] = qer->id;
                    goto out;
                }
            }
        }
    }

    cb->args[5] = 1;

out:
    return skb->len;
}

static int gtp5g_genl_add_urr(struct sk_buff *skb, struct genl_info *info)
{
    /* Not implemented yet */
    return -1; 
}

static int gtp5g_genl_del_urr(struct sk_buff *skb, struct genl_info *info)
{
    /* Not implemented yet */
    return -1; 
}

static int gtp5g_genl_get_urr(struct sk_buff *skb, struct genl_info *info)
{
    /* Not implemented yet */
    return -1; 
}

static int gtp5g_genl_dump_urr(struct sk_buff *skb, struct netlink_callback *cb)
{
    /* Not implemented yet */
    return -1; 
}

static int gtp5g_genl_add_bar(struct sk_buff *skb, struct genl_info *info)
{
    /* Not implemented yet */
    return -1; 
}

static int gtp5g_genl_del_bar(struct sk_buff *skb, struct genl_info *info)
{
    /* Not implemented yet */
    return -1; 
}

static int gtp5g_genl_get_bar(struct sk_buff *skb, struct genl_info *info)
{
    /* Not implemented yet */
    return -1; 
}

static int gtp5g_genl_dump_bar(struct sk_buff *skb, struct netlink_callback *cb)
{
    /* Not implemented yet */
    return -1; 
}

static const struct nla_policy gtp5g_genl_pdr_policy[GTP5G_PDR_ATTR_MAX + 1] = {
    [GTP5G_PDR_ID]                              = { .type = NLA_U32, },
    [GTP5G_PDR_PRECEDENCE]                      = { .type = NLA_U32, },
    [GTP5G_PDR_PDI]                             = { .type = NLA_NESTED, },
    [GTP5G_OUTER_HEADER_REMOVAL]                = { .type = NLA_U8, },
    [GTP5G_PDR_FAR_ID]                          = { .type = NLA_U32, },
    [GTP5G_PDR_QER_ID]                          = { .type = NLA_U32, },
};

static const struct nla_policy gtp5g_genl_far_policy[GTP5G_FAR_ATTR_MAX + 1] = {
    [GTP5G_FAR_ID]                              = { .type = NLA_U32, },
    [GTP5G_FAR_APPLY_ACTION]                    = { .type = NLA_U8, },
    [GTP5G_FAR_FORWARDING_PARAMETER]            = { .type = NLA_NESTED, },
};

static const struct nla_policy gtp5g_genl_qer_policy[GTP5G_QER_ATTR_MAX + 1] = {
    [GTP5G_QER_ID]                              = { .type = NLA_U32, },
    [GTP5G_QER_GATE]                            = { .type = NLA_U8, },
    [GTP5G_QER_MBR]                             = { .type = NLA_NESTED, },
    [GTP5G_QER_GBR]                             = { .type = NLA_NESTED, },
    [GTP5G_QER_CORR_ID]                         = { .type = NLA_U32, },
    [GTP5G_QER_RQI]                             = { .type = NLA_U8, },
    [GTP5G_QER_QFI]                             = { .type = NLA_U8, },
    [GTP5G_QER_PPI]                             = { .type = NLA_U8, },
    [GTP5G_QER_RCSR]                            = { .type = NLA_U8, },
};

static const struct genl_ops gtp5g_genl_ops[] = {
    {
        .cmd = GTP5G_CMD_ADD_PDR,
        // .validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
        .doit = gtp5g_genl_add_pdr,
        // .policy = gtp5g_genl_pdr_policy,
        .flags = GENL_ADMIN_PERM,
    },
    {
        .cmd = GTP5G_CMD_DEL_PDR,
        // .validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
        .doit = gtp5g_genl_del_pdr,
        // .policy = gtp5g_genl_pdr_policy,
        .flags = GENL_ADMIN_PERM,
    },
    {
        .cmd = GTP5G_CMD_GET_PDR,
        // .validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
        .doit = gtp5g_genl_get_pdr,
        .dumpit = gtp5g_genl_dump_pdr,
        // .policy = gtp5g_genl_pdr_policy,
        .flags = GENL_ADMIN_PERM,
    },
    {
        .cmd = GTP5G_CMD_ADD_FAR,
        // .validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
        .doit = gtp5g_genl_add_far,
        // .policy = gtp5g_genl_far_policy,
        .flags = GENL_ADMIN_PERM,
    },
    {
        .cmd = GTP5G_CMD_DEL_FAR,
        // .validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
        .doit = gtp5g_genl_del_far,
        // .policy = gtp5g_genl_far_policy,
        .flags = GENL_ADMIN_PERM,
    },
    {
        .cmd = GTP5G_CMD_GET_FAR,
        // .validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
        .doit = gtp5g_genl_get_far,
        .dumpit = gtp5g_genl_dump_far,
        // .policy = gtp5g_genl_far_policy,
        .flags = GENL_ADMIN_PERM,
    },
    {
        .cmd = GTP5G_CMD_ADD_QER,
        // .validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
        .doit = gtp5g_genl_add_qer,
        // .policy = gtp5g_genl_qer_policy,
        .flags = GENL_ADMIN_PERM,
    },
    {
        .cmd = GTP5G_CMD_DEL_QER,
        // .validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
        .doit = gtp5g_genl_del_qer,
        // .policy = gtp5g_genl_qer_policy,
        .flags = GENL_ADMIN_PERM,
    },
    {
        .cmd = GTP5G_CMD_GET_QER,
        // .validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
        .doit = gtp5g_genl_get_qer,
        .dumpit = gtp5g_genl_dump_qer,
        // .policy = gtp5g_genl_qer_policy,
        .flags = GENL_ADMIN_PERM,
    },
    {
        .cmd = GTP5G_CMD_ADD_URR,
        .doit = gtp5g_genl_add_urr,
        .flags = GENL_ADMIN_PERM,
    },
    {
        .cmd = GTP5G_CMD_DEL_URR,
        .doit = gtp5g_genl_del_urr,
        .flags = GENL_ADMIN_PERM,
    },
    {
        .cmd = GTP5G_CMD_GET_URR,
        .doit = gtp5g_genl_get_urr,
        .dumpit = gtp5g_genl_dump_urr,
        .flags = GENL_ADMIN_PERM,
    },
    {
        .cmd = GTP5G_CMD_ADD_BAR,
        .doit = gtp5g_genl_add_bar,
        .flags = GENL_ADMIN_PERM,
    },
    {
        .cmd = GTP5G_CMD_DEL_BAR,
        .doit = gtp5g_genl_del_bar,
        .flags = GENL_ADMIN_PERM,
    },
    {
        .cmd = GTP5G_CMD_GET_BAR,
        .doit = gtp5g_genl_get_bar,
        .dumpit = gtp5g_genl_dump_bar,
        .flags = GENL_ADMIN_PERM,
    }, 
};

static struct genl_family gtp5g_genl_family __ro_after_init = {
    .name       = "gtp5g",
    .version    = 0,
    .hdrsize    = 0,
    .maxattr    = GTP5G_ATTR_MAX,
    .netnsok    = true,
    .module     = THIS_MODULE,
    .ops        = gtp5g_genl_ops,
    .n_ops      = ARRAY_SIZE(gtp5g_genl_ops),
};

static int __net_init gtp5g_net_init(struct net *net)
{
    struct gtp5g_net *gn = net_generic(net, gtp5g_net_id);

    INIT_LIST_HEAD(&gn->gtp5g_dev_list);
    return 0;
}

static void __net_exit gtp5g_net_exit(struct net *net)
{
    struct gtp5g_net *gn = net_generic(net, gtp5g_net_id);
    struct gtp5g_dev *gtp;
    LIST_HEAD(list);

    rtnl_lock();
    list_for_each_entry(gtp, &gn->gtp5g_dev_list, list)
        gtp5g_dellink(gtp->dev, &list);

    unregister_netdevice_many(&list);
    rtnl_unlock();
}

static struct pernet_operations gtp5g_net_ops = {
    .init    = gtp5g_net_init,
    .exit    = gtp5g_net_exit,
    .id      = &gtp5g_net_id,
    .size    = sizeof(struct gtp5g_net),
};

struct proc_dir_entry *proc_gtp5g = NULL;
struct proc_dir_entry *proc_gtp5g_dbg = NULL;
struct proc_dir_entry *proc_gtp5g_pdr = NULL;
struct proc_dir_entry *proc_gtp5g_far = NULL;
struct proc_dir_entry *proc_gtp5g_qer = NULL;
struct proc_gtp5g_pdr proc_pdr;
struct proc_gtp5g_far proc_far;
struct proc_gtp5g_qer proc_qer;
u64 proc_seid = 0;
u16 proc_pdr_id = 0;
u32 proc_far_id = 0;
u32 proc_qer_id = 0;

static int gtp5g_dbg_read(struct seq_file *s, void *v) 
{
    seq_printf(s, "gtp5g kerenl debug level range: 0~4\n");
    seq_printf(s, "\t 0 -> Logging\n");
    seq_printf(s, "\t 1 -> Error(default)\n");
    seq_printf(s, "\t 2 -> Warning\n");
    seq_printf(s, "\t 3 -> Information\n");
    seq_printf(s, "\t 4 -> Trace\n");
    seq_printf(s, "Current: %d\n", dbg_trace_lvl);
    return 0;
}

static ssize_t proc_dbg_write(struct file *filp, const char __user *buffer,
    size_t len, loff_t *dptr) 
{
    char buf[16];
    unsigned long buf_len = min(len, sizeof(buf) - 1);
    int dbg;
    
    if (copy_from_user(buf, buffer, buf_len)) {
        GTP5G_ERR(NULL, "Failed to read buffer: %s\n", buffer);
        goto err;
    }
    
    buf[buf_len] = 0;
    if (sscanf(buf, "%d", &dbg) != 1) {
        GTP5G_ERR(NULL, "Failed to read debug level: %s\n", buffer);
        goto err;
    }
    
    if (dbg < 0 || dbg > 4) {
        GTP5G_ERR(NULL, "Failed to set debug level: %d <0 or >4\n", dbg);
        goto err;
    }
    
    dbg_trace_lvl = dbg;
    return strnlen(buf, buf_len);
err:
    return -1;
}

static int proc_dbg_read(struct inode *inode, struct file *file)
{
    return single_open(file, gtp5g_dbg_read, NULL);
}

static int gtp5g_pdr_read(struct seq_file *s, void *v) 
{
    if (!proc_pdr_id) {
        seq_printf(s, "Given PDR ID does not exists\n");
        return -1;
    }
    
    seq_printf(s, "PDR: \n");
    seq_printf(s, "\t SEID : %llu\n", proc_pdr.seid);
    seq_printf(s, "\t ID : %u\n", proc_pdr.id);
    seq_printf(s, "\t Precedence: %u\n", proc_pdr.precedence);
    seq_printf(s, "\t OHR: %u\n", proc_pdr.ohr);
    seq_printf(s, "\t Role Addr4: %#08x\n", ntohl(proc_pdr.role_addr4));
    seq_printf(s, "\t PDI UE Addr4: %#08x\n", ntohl(proc_pdr.pdi_ue_addr4));
    seq_printf(s, "\t PDI TEID: %#08x\n", ntohl(proc_pdr.pdi_fteid));
    seq_printf(s, "\t PDU GTPU Addr4: %#08x\n", ntohl(proc_pdr.pdi_gtpu_addr4));
    seq_printf(s, "\t FAR ID: %u\n", proc_pdr.far_id);
    seq_printf(s, "\t QER ID: %u\n", proc_pdr.qer_id);
    seq_printf(s, "\t UL Drop Count: %#llx\n", proc_pdr.ul_drop_cnt);
    seq_printf(s, "\t DL Drop Count: %#llx\n", proc_pdr.dl_drop_cnt);
    seq_printf(s, "\t UL Packet Count: %llu\n", proc_pdr.ul_pkt_cnt);
    seq_printf(s, "\t DL Packet Count: %llu\n", proc_pdr.dl_pkt_cnt);
    seq_printf(s, "\t UL Byte Count: %llu\n", proc_pdr.ul_byte_cnt);
    seq_printf(s, "\t DL Byte Count: %llu\n", proc_pdr.dl_byte_cnt);
    return 0;
}

static int gtp5g_far_read(struct seq_file *s, void *v) 
{
    if (!proc_far_id) {
        seq_printf(s, "Given FAR ID does not exists\n");
        return -1;
    }
    
    seq_printf(s, "FAR: \n");
    seq_printf(s, "\t SEID : %llu\n", proc_far.seid);
    seq_printf(s, "\t ID : %u\n", proc_far.id);
    seq_printf(s, "\t Apply Action: %#x\n", proc_far.action);
    seq_printf(s, "\t OHC Description: %#x\n", proc_far.description);
    seq_printf(s, "\t OHC TEID : %#08x\n", ntohl(proc_far.teid));
    seq_printf(s, "\t OHC Peer Addr4: %#08x\n", ntohl(proc_far.peer_addr4));
    return 0;
}

static int gtp5g_qer_read(struct seq_file *s, void *v) 
{
    if (!proc_qer_id) {
        seq_printf(s, "Given QER ID does not exists\n");
        return -1;
    }
    
    seq_printf(s, "QER: \n");
    seq_printf(s, "\t SEID : %llu\n", proc_qer.seid);
    seq_printf(s, "\t ID : %u\n", proc_qer.id);
    seq_printf(s, "\t QFI: %u\n", proc_qer.qfi);
    return 0;
}

static ssize_t proc_pdr_write(struct file *filp, const char __user *buffer,
    size_t len, loff_t *dptr) 
{
    char buf[128], dev_name[32];
    u8 found = 0;
    unsigned long buf_len = min(sizeof(buf) - 1, len);
    struct gtp5g_pdr *pdr;
    struct gtp5g_dev *gtp;
    
    if (copy_from_user(buf, buffer, buf_len)) {
        GTP5G_ERR(NULL, "Failed to read buffer: %s\n", buf);
        goto err;
    }
    
    buf[buf_len] = 0;
    if (sscanf(buf, "%s %llu %hu", dev_name, &proc_seid, &proc_pdr_id) != 3) {
        GTP5G_ERR(NULL, "proc write of PDR Dev & ID: %s is not valid\n", buf);
        goto err;
    }
    
    list_for_each_entry_rcu(gtp, &proc_gtp5g_dev, proc_list) {
        if (strcmp(dev_name, netdev_name(gtp->dev)) == 0) {
            found = 1;
            break;
        }
    }
    if (!found) {
        GTP5G_ERR(NULL, "Given dev: %s not exists\n", dev_name);
        goto err;
    }

    pdr = find_pdr_by_id(gtp, proc_seid, proc_pdr_id);
    if (!pdr) {
        GTP5G_ERR(NULL, "Given SEID : %llu PDR ID : %u not exists\n", proc_seid, proc_pdr_id);
        goto err;
    }
    
    memset(&proc_pdr, 0, sizeof(proc_pdr));
    proc_pdr.id = pdr->id;
    proc_pdr.seid = pdr->seid;
    proc_pdr.precedence = pdr->precedence;
    
    if (pdr->outer_header_removal) 
        proc_pdr.ohr = *pdr->outer_header_removal;
    
    if (pdr->role_addr_ipv4.s_addr)
        proc_pdr.role_addr4 = pdr->role_addr_ipv4.s_addr;
    
    if (pdr->pdi) {
        if (pdr->pdi->ue_addr_ipv4) 
            proc_pdr.pdi_ue_addr4 = pdr->pdi->ue_addr_ipv4->s_addr;
        if (pdr->pdi->f_teid) {
            proc_pdr.pdi_fteid = pdr->pdi->f_teid->teid;
            proc_pdr.pdi_gtpu_addr4 = pdr->pdi->f_teid->gtpu_addr_ipv4.s_addr;
        }
    }

    if (pdr->far_id)
        proc_pdr.far_id = *pdr->far_id;
    
    if (pdr->qer_id)
        proc_pdr.qer_id = *pdr->qer_id;

    proc_pdr.ul_drop_cnt = pdr->ul_drop_cnt;
    proc_pdr.dl_drop_cnt = pdr->dl_drop_cnt;

    proc_pdr.ul_pkt_cnt = pdr->ul_pkt_cnt;
    proc_pdr.dl_pkt_cnt = pdr->dl_pkt_cnt;
    proc_pdr.ul_byte_cnt = pdr->ul_byte_cnt;
    proc_pdr.dl_byte_cnt = pdr->dl_byte_cnt;

    return strnlen(buf, buf_len);
err:
    proc_pdr_id = 0;
    return -1;
}

static ssize_t proc_far_write(struct file *filp, const char __user *buffer,
    size_t len, loff_t *dptr) 
{
    char buf[128], dev_name[32];
    u8 found = 0;
    unsigned long buf_len = min(sizeof(buf) - 1, len);
    struct gtp5g_far *far;
    struct gtp5g_dev *gtp;
    
    if (copy_from_user(buf, buffer, buf_len)) {
        GTP5G_ERR(NULL, "Failed to read buffer: %s\n", buf);
        goto err;
    }
    
    buf[buf_len] = 0;
    if (sscanf(buf, "%s %llu %u", dev_name, &proc_seid, &proc_far_id) != 3) {
        GTP5G_ERR(NULL, "proc write of FAR Dev & ID: %s is not valid\n", buf);
        goto err;
    }
    
    list_for_each_entry_rcu(gtp, &proc_gtp5g_dev, proc_list) {
        if (strcmp(dev_name, netdev_name(gtp->dev)) == 0) {
            found = 1;
            break;
        }
    }
    if (!found) {
        GTP5G_ERR(NULL, "Given dev: %s not exists\n", dev_name);
        goto err;
    }

    far = find_far_by_id(gtp, proc_seid, proc_far_id);
    if (!far) {
        GTP5G_ERR(NULL, "Given FAR ID : %u not exists\n", proc_far_id);
        goto err;
    }
    
    memset(&proc_far, 0, sizeof(proc_far));
    proc_far.id = far->id;
    proc_far.seid = far->seid;
    proc_far.action = far->action;
   
    if (far->fwd_param) {
        if (far->fwd_param->hdr_creation) {
            proc_far.description = far->fwd_param->hdr_creation->description;
            proc_far.teid = far->fwd_param->hdr_creation->teid;
            proc_far.peer_addr4 = far->fwd_param->hdr_creation->peer_addr_ipv4.s_addr;
        }
    }

    return strnlen(buf, buf_len);
err:
    proc_far_id = 0;
    return -1;
}

static ssize_t proc_qer_write(struct file *filp, const char __user *buffer,
    size_t len, loff_t *dptr) 
{
    char buf[128], dev_name[32];
    u8 found = 0;
    unsigned long buf_len = min(sizeof(buf) - 1, len);
    struct gtp5g_qer *qer;
    struct gtp5g_dev *gtp;
    
    if (copy_from_user(buf, buffer, buf_len)) {
        GTP5G_ERR(NULL, "Failed to read buffer: %s\n", buf);
        goto err;
    }
    
    buf[buf_len] = 0;
    if (sscanf(buf, "%s %llu %u", dev_name, &proc_seid, &proc_qer_id) != 3) {
        GTP5G_ERR(NULL, "proc write of QER Dev & ID: %s is not valid\n", buf);
        goto err;
    }
    
    list_for_each_entry_rcu(gtp, &proc_gtp5g_dev, proc_list) {
        if (strcmp(dev_name, netdev_name(gtp->dev)) == 0) {
            found = 1;
            break;
        }
    }
    if (!found) {
        GTP5G_ERR(NULL, "Given dev: %s not exists\n", dev_name);
        goto err;
    }

    qer = find_qer_by_id(gtp, proc_seid, proc_qer_id);
    if (!qer) {
        GTP5G_ERR(NULL, "Given QER ID : %u not exists\n", proc_qer_id);
        goto err;
    }
    
    memset(&proc_qer, 0, sizeof(proc_qer));
    proc_qer.id = qer->id;
    proc_qer.seid = qer->seid;
    proc_qer.qfi = qer->qfi;

    return strnlen(buf, buf_len);
err:
    proc_qer_id = 0;
    return -1;
}

static int proc_pdr_read(struct inode *inode, struct file *file)
{
    return single_open(file, gtp5g_pdr_read, NULL);
}

static int proc_far_read(struct inode *inode, struct file *file)
{
    return single_open(file, gtp5g_far_read, NULL);
}

static int proc_qer_read(struct inode *inode, struct file *file)
{
    return single_open(file, gtp5g_qer_read, NULL);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops proc_gtp5g_dbg_ops = {
    .proc_open = proc_dbg_read,
    .proc_read = seq_read,
    .proc_write = proc_dbg_write,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};
#else
static const struct file_operations proc_gtp5g_dbg_ops = {
    .owner      = THIS_MODULE,
    .open       = proc_dbg_read,
    .read       = seq_read,
    .write      = proc_dbg_write,
    .llseek     = seq_lseek,
    .release    = single_release,
};
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops proc_gtp5g_pdr_ops = {
    .proc_open = proc_pdr_read,
    .proc_read = seq_read,
    .proc_write = proc_pdr_write,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};
#else
static const struct file_operations proc_gtp5g_pdr_ops = {
    .owner      = THIS_MODULE,
    .open       = proc_pdr_read,
    .read       = seq_read,
    .write      = proc_pdr_write,
    .llseek     = seq_lseek,
    .release    = single_release,
};
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops proc_gtp5g_far_ops = {
    .proc_open = proc_far_read,
    .proc_read = seq_read,
    .proc_write = proc_far_write,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};
#else
static const struct file_operations proc_gtp5g_far_ops = {
    .owner      = THIS_MODULE,
    .open       = proc_far_read,
    .read       = seq_read,
    .write      = proc_far_write,
    .llseek     = seq_lseek,
    .release    = single_release,
};
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops proc_gtp5g_qer_ops = {
    .proc_open = proc_qer_read,
    .proc_read = seq_read,
    .proc_write = proc_qer_write,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};
#else
static const struct file_operations proc_gtp5g_qer_ops = {
    .owner      = THIS_MODULE,
    .open       = proc_qer_read,
    .read       = seq_read,
    .write      = proc_qer_write,
    .llseek     = seq_lseek,
    .release    = single_release,
};
#endif

static int __init gtp5g_init(void)
{
    int err;

    GTP5G_LOG(NULL, "Gtp5g Module initialization Ver: %s\n", DRV_VERSION);

    INIT_LIST_HEAD(&proc_gtp5g_dev);

    get_random_bytes(&gtp5g_h_initval, sizeof(gtp5g_h_initval));

    err = rtnl_link_register(&gtp5g_link_ops);
    if (err < 0) {
        GTP5G_ERR(NULL, "Failed to register rtnl\n");
        goto error_out;
    }

    err = genl_register_family(&gtp5g_genl_family);
    if (err < 0) {
        GTP5G_ERR(NULL, "Failed to register generic\n");
        goto unreg_rtnl_link;
    }

    err = register_pernet_subsys(&gtp5g_net_ops);
    if (err < 0) {
        GTP5G_ERR(NULL, "Failed to register namespace\n");
        goto unreg_genl_family;
    }

    proc_gtp5g = proc_mkdir("gtp5g", NULL);
    if (!proc_gtp5g) {
        GTP5G_ERR(NULL, "Failed to create /proc/gtp5g\n");
        goto unreg_pernet;
    }

    proc_gtp5g_dbg = proc_create("dbg", (S_IFREG | S_IRUGO | S_IWUGO),
        proc_gtp5g, &proc_gtp5g_dbg_ops);
    if (!proc_gtp5g_dbg) {
        GTP5G_ERR(NULL, "Failed to create /proc/gtp5g/dbg\n");
        goto remove_gtp5g_proc;
    }

    proc_gtp5g_pdr = proc_create("pdr", (S_IFREG | S_IRUGO | S_IWUGO),
        proc_gtp5g, &proc_gtp5g_pdr_ops);
    if (!proc_gtp5g_pdr) {
        GTP5G_ERR(NULL, "Failed to create /proc/gtp5g/pdr\n");
        goto remove_dbg_proc;
    }

    proc_gtp5g_far = proc_create("far", (S_IFREG | S_IRUGO | S_IWUGO),
        proc_gtp5g, &proc_gtp5g_far_ops);
    if (!proc_gtp5g_far) {
        GTP5G_ERR(NULL, "Failed to create /proc/gtp5g/far\n");
        goto remove_pdr_proc;
    }

    proc_gtp5g_qer = proc_create("qer", (S_IFREG | S_IRUGO | S_IWUGO), 
        proc_gtp5g, &proc_gtp5g_qer_ops);
    if (!proc_gtp5g_qer) {
        GTP5G_ERR(NULL, "Failed to create /proc/gtp5g/qer\n");
        goto remove_far_proc;
    }

    GTP5G_LOG(NULL, "5G GTP module loaded\n");

    return 0;
remove_far_proc:
    remove_proc_entry("far", proc_gtp5g);
remove_pdr_proc:
    remove_proc_entry("pdr", proc_gtp5g);
remove_dbg_proc:
    remove_proc_entry("dbg", proc_gtp5g);
remove_gtp5g_proc:
    remove_proc_entry("gtp5g", NULL);
unreg_pernet:
    unregister_pernet_subsys(&gtp5g_net_ops);
unreg_genl_family:
    genl_unregister_family(&gtp5g_genl_family);
unreg_rtnl_link:
    rtnl_link_unregister(&gtp5g_link_ops);
error_out:
    return err;
}
late_initcall(gtp5g_init);

static void __exit gtp5g_fini(void)
{
    genl_unregister_family(&gtp5g_genl_family);
    rtnl_link_unregister(&gtp5g_link_ops);
    unregister_pernet_subsys(&gtp5g_net_ops);

    remove_proc_entry("qer", proc_gtp5g);
    remove_proc_entry("far", proc_gtp5g);
    remove_proc_entry("pdr", proc_gtp5g);
    remove_proc_entry("dbg", proc_gtp5g);
    remove_proc_entry("gtp5g", NULL);

    GTP5G_LOG(NULL, "5G GTP module unloaded\n");
}
module_exit(gtp5g_fini);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yao-Wen Chang <yaowenowo@gmail.com>");
MODULE_AUTHOR("Muthuraman <muthuramane.cs03g@g2.nctu.edu.tw>");
MODULE_DESCRIPTION("Interface for 5G GTP encapsulated traffic");
MODULE_VERSION(DRV_VERSION);
MODULE_ALIAS_RTNL_LINK("gtp5g");
MODULE_ALIAS_GENL_FAMILY("gtp5g");
