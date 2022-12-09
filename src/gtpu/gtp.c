#include <linux/udp.h>

#include "gtp.h"
#include "log.h"

int get_gtpu_header_len(struct gtpv1_hdr *gtpv1,  struct sk_buff *skb)
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
        gtpv1_hdr_opt_t *gtpv1_opt = (gtpv1_hdr_opt_t *) ((u8 *) gtpv1 + sizeof(*gtpv1));

        next_ehdr_type = gtpv1_opt->next_ehdr_type;
        while (next_ehdr_type) {
            switch (next_ehdr_type) {
            case GTPV1_NEXT_EXT_HDR_TYPE_85: {
                ext_pdu_sess_ctr_t *etype85; 
                // pdu_sess_ctr_t *pdu_sess_info = &etype85->pdu_sess_ctr;

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
