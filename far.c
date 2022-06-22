#include "dev.h"
#include "far.h"
#include "pdr.h"
#include "seid.h"
#include "hash.h"

static void seid_far_id_to_hex_str(u64 seid_int, u32 far_id, char *buff)
{
    seid_and_u32id_to_hex_str(seid_int, far_id, buff);
}

static void far_context_free(struct rcu_head *head)
{
    struct far *far = container_of(head, struct far, rcu_head);
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

void far_context_delete(struct far *far)
{
    struct gtp5g_dev *gtp = netdev_priv(far->dev);
    struct hlist_head *head;
    struct pdr *pdr;

    char seid_far_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};;

    if (!far)
        return;

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

struct far *find_far_by_id(struct gtp5g_dev *gtp, u64 seid, u32 far_id)
{
    struct hlist_head *head;
    struct far *far;
    char seid_far_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};

    seid_far_id_to_hex_str(seid, far_id, seid_far_id_hexstr);
    head = &gtp->far_id_hash[str_hashfn(seid_far_id_hexstr) % gtp->hash_size];
    hlist_for_each_entry_rcu(far, head, hlist_id) {
        if (far->seid == seid && far->id == far_id)
            return far;
    }

    return NULL;
}

void far_update(struct far *far, struct gtp5g_dev *gtp, u8 *flag,
        struct gtp5g_emark_pktinfo *epkt_info)
{
    struct pdr *pdr;
    struct hlist_head *head;
    char seid_far_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};

    seid_far_id_to_hex_str(far->seid, far->id, seid_far_id_hexstr);
    head = &gtp->related_far_hash[str_hashfn(seid_far_id_hexstr) % gtp->hash_size];
    hlist_for_each_entry_rcu(pdr, head, hlist_related_far) {
        if (pdr->seid == far->seid && *pdr->far_id == far->id) {
            if (flag != NULL && *flag == 1) {
                epkt_info->role_addr = pdr->role_addr_ipv4.s_addr;
                epkt_info->sk = pdr->sk;
            }
            pdr->far = far;
            unix_sock_client_update(pdr);
        }
    }
}

void far_append(u64 seid, u32 far_id, struct far *far, struct gtp5g_dev *gtp)
{
    char seid_far_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};
    u32 i;

    seid_far_id_to_hex_str(seid, far_id, seid_far_id_hexstr);
    i = str_hashfn(seid_far_id_hexstr) % gtp->hash_size;
    hlist_add_head_rcu(&far->hlist_id, &gtp->far_id_hash[i]);
}

int far_get_pdr_ids(u16 *ids, int n, struct far *far, struct gtp5g_dev *gtp)
{
    struct hlist_head *head;
    struct pdr *pdr;
    char seid_far_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};
    int i;

    seid_far_id_to_hex_str(far->seid, far->id, seid_far_id_hexstr);
    head = &gtp->related_far_hash[str_hashfn(seid_far_id_hexstr) % gtp->hash_size];
    i = 0;
    hlist_for_each_entry_rcu(pdr, head, hlist_related_far) {
        if (i >= n)
            break;
        if (pdr->seid == far->seid && *pdr->far_id == far->id)
            ids[i++] = pdr->id;
    }
    return i;
}

void far_set_pdr(u64 seid, u32 far_id, struct hlist_node *node, struct gtp5g_dev *gtp)
{
    char seid_far_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};
    u32 i;

    if (!hlist_unhashed(node))
        hlist_del_rcu(node);

    seid_far_id_to_hex_str(seid, far_id, seid_far_id_hexstr);
    i = str_hashfn(seid_far_id_hexstr) % gtp->hash_size;
    hlist_add_head_rcu(node, &gtp->related_far_hash[i]);
}
