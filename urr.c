#include <linux/rculist.h>

#include "common.h"
#include "dev.h"
#include "urr.h"
#include "pdr.h"
#include "seid.h"
#include "hash.h"

void seid_urr_id_to_hex_str(u64 seid_int, u32 urr_id, char *buff)
{
    seid_and_u32id_to_hex_str(seid_int, urr_id, buff);
}

static void urr_context_free(struct rcu_head *head)
{
    struct urr *urr = container_of(head, struct urr, rcu_head);

    if (!urr)
        return;

    kfree(urr);
}

void urr_context_delete(struct urr *urr)
{
    struct gtp5g_dev *gtp = netdev_priv(urr->dev);
    struct hlist_head *head;
    struct pdr *pdr;
    char seid_urr_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};
    if (!urr)
        return;

    if (!hlist_unhashed(&urr->hlist_id))
        hlist_del_rcu(&urr->hlist_id);

    seid_urr_id_to_hex_str(urr->seid, urr->id, seid_urr_id_hexstr);
    head = &gtp->related_urr_hash[str_hashfn(seid_urr_id_hexstr) % gtp->hash_size];
    hlist_for_each_entry_rcu(pdr, head, hlist_related_urr) {
        if (find_urr_id_in_pdr(pdr, urr->id)) {
            unix_sock_client_delete(pdr);
        }
    }

    call_rcu(&urr->rcu_head, urr_context_free);
}

struct urr *find_urr_by_id(struct gtp5g_dev *gtp, u64 seid, u32 urr_id)
{
    struct hlist_head *head;
    struct urr *urr;
    char seid_urr_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};

    seid_urr_id_to_hex_str(seid, urr_id, seid_urr_id_hexstr);
    head = &gtp->urr_id_hash[str_hashfn(seid_urr_id_hexstr) % gtp->hash_size];
    hlist_for_each_entry_rcu(urr, head, hlist_id) {
        if (urr->seid == seid && urr->id == urr_id)
            return urr;
    }

    return NULL;
}

void urr_update(struct urr *urr, struct gtp5g_dev *gtp)
{
    struct pdr *pdr;
    struct hlist_head *head;
    char seid_urr_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};

    seid_urr_id_to_hex_str(urr->seid, urr->id, seid_urr_id_hexstr);
    head = &gtp->related_urr_hash[str_hashfn(seid_urr_id_hexstr) % gtp->hash_size];
    hlist_for_each_entry_rcu(pdr, head, hlist_related_urr) {
        if (find_urr_id_in_pdr(pdr, urr->id)) {
            unix_sock_client_update(pdr);
        }
    }
}

void urr_quota_exhaust_action(struct urr *urr, struct gtp5g_dev *gtp)
{
    struct hlist_head *head;
    struct pdr *pdr;
    char seid_urr_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};
    u16 *actions = NULL, *pdrids = NULL;

    // urr stop measurement
    urr->pdr_num = 0;
    urr->info |= URR_INFO_INAM;
    urr->quota_exhausted = true;

    pdrids = kzalloc(MAX_PDR_PER_SESSION * sizeof(u16), GFP_KERNEL);
    actions = kzalloc(MAX_PDR_PER_SESSION * sizeof(u16), GFP_KERNEL);
    if (!pdrids || !actions)
        goto fail;

    seid_urr_id_to_hex_str(urr->seid, urr->id, seid_urr_id_hexstr);
    head = &gtp->related_urr_hash[str_hashfn(seid_urr_id_hexstr) % gtp->hash_size];
    //each pdr that associate with the urr drop pkt
    hlist_for_each_entry_rcu(pdr, head, hlist_related_urr) {
        if (find_urr_id_in_pdr(pdr, urr->id)) {
            pdrids[urr->pdr_num] = pdr->id;
            actions[urr->pdr_num++] = pdr->far->action;

            pdr->far->action = FAR_ACTION_DROP;
        }
    }

    if (urr->pdr_num > 0) {
        urr->pdrids = kzalloc(urr->pdr_num * sizeof(u16), GFP_KERNEL);
        urr->actions = kzalloc(urr->pdr_num * sizeof(u16), GFP_KERNEL);

        memcpy(urr->pdrids, pdrids, urr->pdr_num * sizeof(u16));
        memcpy(urr->actions, actions, urr->pdr_num * sizeof(u16));
    }

fail:
    if (pdrids)
        kfree(pdrids);
    if (actions)
        kfree(actions);
}

void urr_reverse_quota_exhaust_action(struct urr *urr, struct gtp5g_dev *gtp)
{
    struct hlist_head *head;
    struct pdr *pdr;
    char seid_urr_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};
    int i;
    urr->quota_exhausted = false;
    seid_urr_id_to_hex_str(urr->seid, urr->id, seid_urr_id_hexstr);
    head = &gtp->related_urr_hash[str_hashfn(seid_urr_id_hexstr) % gtp->hash_size];

    //each pdr that associate with the urr resume it's normal action
    hlist_for_each_entry_rcu(pdr, head, hlist_related_urr) {
        if (find_urr_id_in_pdr(pdr, urr->id)) {
            for (i = 0; i < urr->pdr_num; i++) {
                if (urr->pdrids[i] == pdr->id)
                    pdr->far->action = urr->actions[i];
            }
        }
    }

    if (urr->pdrids)
        kfree(urr->pdrids);
    if (urr->actions)
        kfree(urr->actions);
}

void urr_append(u64 seid, u32 urr_id, struct urr *urr, struct gtp5g_dev *gtp)
{
    char seid_urr_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};
    u32 i;

    seid_urr_id_to_hex_str(seid, urr_id, seid_urr_id_hexstr);
    i = str_hashfn(seid_urr_id_hexstr) % gtp->hash_size;
    hlist_add_head_rcu(&urr->hlist_id, &gtp->urr_id_hash[i]);
}

int urr_get_pdr_ids(u16 *ids, int n, struct urr *urr, struct gtp5g_dev *gtp)
{
    struct hlist_head *head;
    struct pdr *pdr;
    char seid_urr_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};
    int i;

    seid_urr_id_to_hex_str(urr->seid, urr->id, seid_urr_id_hexstr);
    head = &gtp->related_urr_hash[str_hashfn(seid_urr_id_hexstr) % gtp->hash_size];
    i = 0;
    hlist_for_each_entry_rcu(pdr, head, hlist_related_urr) {
        if (i >= n)
            break;
        if (find_urr_id_in_pdr(pdr, urr->id)) {
            ids[i++] = pdr->id;
        }
    }
    return i;
}

void urr_set_pdr(u64 seid, u32 urr_id, struct hlist_node *node, struct gtp5g_dev *gtp)
{
    char seid_urr_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};
    u32 i;

    seid_urr_id_to_hex_str(seid, urr_id, seid_urr_id_hexstr);
    i = str_hashfn(seid_urr_id_hexstr) % gtp->hash_size;
    hlist_add_head_rcu(node, &gtp->related_urr_hash[i]);
}
