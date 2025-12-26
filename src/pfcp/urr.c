#include <linux/rculist.h>

#include "common.h"
#include "dev.h"
#include "urr.h"
#include "pdr.h"
#include "far.h"
#include "seid.h"
#include "hash.h"
#include "log.h"

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
    struct pdr_node *pdr_node;
    char seid_urr_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};
    if (!urr)
        return;

    // If URR is being deleted while quota is still exhausted,
    // restore FAR actions to prevent permanent DROP state
    if (urr->quota_exhausted) {
        GTP5G_ERR(NULL, "URR (%u) deleted while quota exhausted, force restoring FAR actions", urr->id);
        urr_reverse_quota_exhaust_action(urr, gtp);
    }

    if (!hlist_unhashed(&urr->hlist_id))
        hlist_del_rcu(&urr->hlist_id);

    seid_urr_id_to_hex_str(urr->seid, urr->id, seid_urr_id_hexstr);
    head = &gtp->related_urr_hash[str_hashfn(seid_urr_id_hexstr) % gtp->hash_size];
    hlist_for_each_entry_rcu(pdr_node, head, hlist) {
        if (pdr_node->pdr != NULL &&
            find_urr_id_in_pdr(pdr_node->pdr, urr->id)) {
            unix_sock_client_delete(pdr_node->pdr);
        }
    }

    // Free allocated memory for quota exhausted state
    if (urr->pdrids)
        kfree(urr->pdrids);
    if (urr->farids)
        kfree(urr->farids);
    if (urr->actions)
        kfree(urr->actions);

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
    struct pdr_node *pdr_node;
    struct hlist_head *head;
    char seid_urr_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};

    seid_urr_id_to_hex_str(urr->seid, urr->id, seid_urr_id_hexstr);
    head = &gtp->related_urr_hash[str_hashfn(seid_urr_id_hexstr) % gtp->hash_size];
    hlist_for_each_entry_rcu(pdr_node, head, hlist) {
        if (pdr_node->pdr != NULL &&
            find_urr_id_in_pdr(pdr_node->pdr, urr->id)) {
            unix_sock_client_update(pdr_node->pdr, rcu_dereference(pdr_node->pdr->far));
        }
    }
}

// TODO: FAR ID for Quota Action IE for indicating the action while no quota is granted
void urr_quota_exhaust_action(struct urr *urr, struct gtp5g_dev *gtp)
{
    struct hlist_head *head;
    struct pdr_node *pdr_node;
    char seid_urr_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};
    u16 *actions = NULL, *pdrids = NULL;
    u32 *farids = NULL;
    struct far *far;

    if (urr->quota_exhausted) {
        GTP5G_WAR(NULL, "URR (%u) quota was already exhausted\n", urr->id);
        return;
    }

    // urr stop measurement
    urr->pdr_num = 0;

    pdrids = kzalloc(MAX_PDR_PER_SESSION * sizeof(u16), GFP_KERNEL);
    farids = kzalloc(MAX_PDR_PER_SESSION * sizeof(u32), GFP_KERNEL);
    actions = kzalloc(MAX_PDR_PER_SESSION * sizeof(u16), GFP_KERNEL);
    if (!pdrids || !farids || !actions)
        goto fail;

    seid_urr_id_to_hex_str(urr->seid, urr->id, seid_urr_id_hexstr);
    head = &gtp->related_urr_hash[str_hashfn(seid_urr_id_hexstr) % gtp->hash_size];
    
    rcu_read_lock();
    //each pdr that associate with the urr drop pkt
    hlist_for_each_entry_rcu(pdr_node, head, hlist) {
        if (find_urr_id_in_pdr(pdr_node->pdr, urr->id)) {
            if (urr->pdr_num >= MAX_PDR_PER_SESSION) {
                GTP5G_ERR(NULL, "URR (%u) exceeds MAX_PDR_PER_SESSION\n", urr->id);
                break;
            }
            
            pdrids[urr->pdr_num] = pdr_node->pdr->id;
            far = rcu_dereference(pdr_node->pdr->far);
            if (far != NULL) {
                farids[urr->pdr_num] = far->id;     // Save FAR ID
                actions[urr->pdr_num] = far->action; // Save original action
                urr->pdr_num++;
                
                far->action = FAR_ACTION_DROP;
                GTP5G_ERR(NULL, "URR (%u) quota exhausted, set FAR (%u) action to drop\n", urr->id, far->id);
            }
        }
    }
    rcu_read_unlock();

    if (urr->pdr_num > 0) {
        urr->pdrids = kzalloc(urr->pdr_num * sizeof(u16), GFP_KERNEL);
        urr->farids = kzalloc(urr->pdr_num * sizeof(u32), GFP_KERNEL);
        urr->actions = kzalloc(urr->pdr_num * sizeof(u16), GFP_KERNEL);
        
        if (!urr->pdrids || !urr->farids || !urr->actions) {
            GTP5G_ERR(NULL, "URR (%u) failed to allocate memory for saving FAR states\n", urr->id);
            if (urr->pdrids)
                kfree(urr->pdrids);
            if (urr->farids)
                kfree(urr->farids);
            if (urr->actions)
                kfree(urr->actions);
            urr->pdrids = NULL;
            urr->farids = NULL;
            urr->actions = NULL;
            urr->pdr_num = 0;
            goto fail;
        }

        memcpy(urr->pdrids, pdrids, urr->pdr_num * sizeof(u16));
        memcpy(urr->farids, farids, urr->pdr_num * sizeof(u32));
        memcpy(urr->actions, actions, urr->pdr_num * sizeof(u16));
    }

    urr->info |= URR_INFO_INAM;
    urr->quota_exhausted = true;

fail:
    if (pdrids)
        kfree(pdrids);
    if (farids)
        kfree(farids);
    if (actions)
        kfree(actions);
}

void urr_reverse_quota_exhaust_action(struct urr *urr, struct gtp5g_dev *gtp)
{
    int i;
    struct far *far;

    if (!urr->quota_exhausted) {
        GTP5G_WAR(NULL, "URR (%u) quota is not exhausted; should not reverse\n", urr->id);
        return;
    }

    rcu_read_lock();
    // Restore each FAR action by FAR ID directly, not through PDR
    for (i = 0; i < urr->pdr_num; i++) {
        far = find_far_by_id(gtp, urr->seid, urr->farids[i]);
        if (far != NULL) {
            far->action = urr->actions[i];
            GTP5G_INF(NULL, "URR (%u) restore FAR (%u) action to %u\n", urr->id, far->id, far->action);
        } else {
            GTP5G_WAR(NULL, "URR (%u) cannot find FAR (%u) to restore action\n", urr->id, urr->farids[i]);
        }
    }
    rcu_read_unlock();

    urr->quota_exhausted = false;
    urr->info &= ~URR_INFO_INAM;

    if (urr->pdrids) {
        kfree(urr->pdrids);
        urr->pdrids = NULL;
    }
    if (urr->farids) {
        kfree(urr->farids);
        urr->farids = NULL;
    }
    if (urr->actions) {
        kfree(urr->actions);
        urr->actions = NULL;
    }
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
    struct pdr_node *pdr_node;
    char seid_urr_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};
    int i;

    seid_urr_id_to_hex_str(urr->seid, urr->id, seid_urr_id_hexstr);
    head = &gtp->related_urr_hash[str_hashfn(seid_urr_id_hexstr) % gtp->hash_size];
    i = 0;
    hlist_for_each_entry_rcu(pdr_node, head, hlist) {
        if (i >= n)
            break;
        if (pdr_node->pdr != NULL && find_urr_id_in_pdr(pdr_node->pdr, urr->id)) {
            ids[i++] = pdr_node->pdr->id;
        }
    }
    return i;
}

void del_related_urr_hash(struct gtp5g_dev *gtp, struct pdr *pdr)
{
    u32 i, j;
    struct pdr_node *pdr_node = NULL ;
    struct pdr_node *to_be_del = NULL ;
    char seid_urr_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};

    for (j = 0; j < pdr->urr_num; j++) {
        to_be_del = NULL;
        seid_urr_id_to_hex_str(pdr->seid, pdr->urr_ids[j], seid_urr_id_hexstr);
        i = str_hashfn(seid_urr_id_hexstr) % gtp->hash_size;
        hlist_for_each_entry_rcu(pdr_node, &gtp->related_urr_hash[i], hlist) {
            if (pdr_node->pdr != NULL &&
                pdr_node->pdr->seid == pdr->seid &&
                pdr_node->pdr->id == pdr->id) {
                to_be_del = pdr_node;
                break;
            }
        }
        if (to_be_del){
            hlist_del(&to_be_del->hlist);
            kfree(to_be_del);
        }
    }
}

int urr_set_pdr(struct pdr *pdr, struct gtp5g_dev *gtp)
{
    char seid_urr_id_hexstr[SEID_U32ID_HEX_STR_LEN] = {0};
    u32 i, j;
    struct pdr_node *pdr_node = NULL;

    if (!pdr)
        return -1;

    // clean old pdr_node
    del_related_urr_hash(gtp, pdr);

    for (j = 0; j < pdr->urr_num; j++) {
        seid_urr_id_to_hex_str(pdr->seid, pdr->urr_ids[j], seid_urr_id_hexstr);
        i = str_hashfn(seid_urr_id_hexstr) % gtp->hash_size;

        pdr_node = kzalloc(sizeof(*pdr_node), GFP_ATOMIC);
        if (!pdr_node) {
            return -ENOMEM;
        }
        pdr_node->pdr = pdr;
        hlist_add_head_rcu(&pdr_node->hlist, &gtp->related_urr_hash[i]);
    }
    return 0;
}

/* 
 `get_period_vol_counter` will return one of the two counters.
 
 To avoid sending incorrect reports, there are two counters (vol1, vol2) for each period.
 These counters will take turns recording the packet count.
 
 For usage report => counter of the previous period
 For packet counting => counter of the current period 
*/  
struct VolumeMeasurement *get_period_vol_counter(struct urr *urr, bool use_vol2)
{
    if (use_vol2) {
        return &urr->vol2;
    }
    return &urr->vol1;
}

struct VolumeMeasurement *get_and_switch_period_vol_counter(struct urr *urr)
{
    unsigned int start;

    // Reader: use retry loop to safely switch which buffer the writer should use
    // and read from the buffer that writer is NOT currently using
    do {
        start = u64_stats_fetch_begin(&urr->period_vol_counter_sync);
        urr->use_vol2 = !urr->use_vol2; // Combine read and switch in one operation
    } while (u64_stats_fetch_retry(&urr->period_vol_counter_sync, start));

    // Return the buffer that writer was NOT using when we read vol_to_read
    return get_period_vol_counter(urr, !urr->use_vol2);
}
