/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _GTP_H_
#define _GTP_H_

/** -----------------------------------------------------------
 *		BEGIN OF General GTP protocol related definitions
 *  ----------------------------------------------------------- 
 */

#define GTP1U_PORT	2152

#define GTP_TPDU	255
#define GTP_EMARK	254

/* According to 3GPP TS 29.060. */
struct gtpv1_hdr {
	__u8	flags;
#define GTPV1_HDR_FLG_NPDU		0x01
#define GTPV1_HDR_FLG_SEQ		0x02
#define GTPV1_HDR_FLG_EXTHDR	0x04
#define GTPV1_HDR_FLG_MASK		0x07

	__u8	type;
	__be16	length;
	__be32	tid;
} __attribute__((packed)) gtpv1_hdr_t;

typedef struct gtp1_hdr_opt {
	__be16 	seq_number;
	__u8	NPDU;
	__u8 	next_ehdr_type;
/** 3GPP TS 29.281
 * From Figure 5.2.1-2 Definition of Extension Header Type 
 */
#define GTPV1_NEXT_EXT_HDR_TYPE_00	0x00 /* No More extension */
#define GTPV1_NEXT_EXT_HDR_TYPE_03	0x03 /* Long PDCP PDU Number */
#define GTPV1_NEXT_EXT_HDR_TYPE_20	0x20 /* Service Class Indicator */
#define GTPV1_NEXT_EXT_HDR_TYPE_40	0x40 /* UDP Port */
#define GTPV1_NEXT_EXT_HDR_TYPE_81	0x81 /* RAN Container */
#define GTPV1_NEXT_EXT_HDR_TYPE_82	0x82 /* Long PDCP PDU Number */
#define GTPV1_NEXT_EXT_HDR_TYPE_83	0x83 /* Xw RAN Container */
#define GTPV1_NEXT_EXT_HDR_TYPE_84	0x84 /* NR RAN Container */
#define GTPV1_NEXT_EXT_HDR_TYPE_85	0x85 /* PDU Session Container */
#define GTPV1_NEXT_EXT_HDR_TYPE_C0	0xc0 /* PDCP PDU Number */

} __attribute__((packed)) gtpv1_hdr_opt_t;


/** 3GPP TS 29.281
 * Section 5.2.1 General format of the GTP-U Extension Header
 *
 *           +-----------------------------+	
 *	Octets 1 | 	Extension Header Length    |
 * 			 +-----------------------------+
 *	2 – m	 |	Extension Header Content   |
 * 			 +-----------------------------+
 *	m+1		 |	Next Extension Header Type |
 *			 +-----------------------------+
 * 
 * Note: If no more header type then the value of Next Extension Header Type
 *		 is ZERO.
 * 
 *
 * Section 5.2.2 Extension Header types 
 */

/** TS 29.281
 * 5.2.2.1 UDP Port
 *
 * */
struct gtp1_hdr_ext_udp_port {
	__u8	length;
	__be16	udp_port_num;
	__u8	next_ehdr_type;	
} __attribute__((packed)) ext_dup_port_t;

/** TS 29.281
 * 5.2.2.2 PDCP PDU Number
 *
 * This value shall be used by a source eNB or gNB 
 * */
struct gtp1_hdr_ext_pdcp_pdu_num {
	__u8	length;
	__u8	pdcp_pdu_num0;
	__u8	pdcp_pdu_num1;
	__u8	next_ehdr_type;	
} __attribute__((packed)) ext_pdcp_pdu_num_t;

/** TS 29.281
 * 5.2.2.2A Long PDU Session container
 *
 */
struct gtp1_hdr_ext_lpdcp_pdu_num {
	__u8	length;
	__u8	spare0_pdcp_pdu_num0;
	__u8	pdcp_pdu_num1;
	__u8	pdcp_pdu_num2;
	__u8	spare1;
	__u8	spare2;
	__u8	spare3;
	__u8	next_ehdr_type;	
} __attribute__((packed)) ext_lpdcp_pdu_num_t;

/** TS 29.281
 * 5.2.2.3 Service Class Indicator
 *
 */
struct gtp1_hdr_ext_service_cls {
	__u8	length;
	__u8	service_cls_indicator;
	__u8	spare;
	__u8	next_ehdr_type;	
} __attribute__((packed)) ext_service_cls_t;

/** TS 29.281
 * 5.2.2.4 RAN container
 *
 */
struct gtp1_hdr_ext_ran_container {
	__u8	length;
	__u8	ran_ctr[0];
	__u8	next_ehdr_type;	
} __attribute__((packed)) ext_ran_ctr_t;

/** TS 29.281
 * 5.2.2.5 Xw RAN container
 *
 */
struct gtp1_hdr_ext_xw_ran_ctr {
	__u8	length;
	__u8	xw_ran_ctr[0];
	__u8	next_ehdr_type;	
} __attribute__((packed)) ext_xw_ran_ctr_t;


/** TS 29.281
 * 5.2.2.7 NR RAN container
 *
 */
typedef struct gtp1_hdr_ext_nr_ran_ctr {
	__u8	length;
	__u8	nr_ran_ctr[0];
	__u8	next_ehdr_type;	
} __attribute__((packed)) ext_nr_ran_ctr_t;

/** TS 29.281
 * 5.2.2.7 PDU Session container
 *		> Extension Type 133(0x85)
 *		> Transmitted in a G-PDU over N3 and N9
 *
 *		@pdu_sess_container 
 *			- variable length field
 *			- Specified in TS 38.415 (Refer. v15.2.0 (2018-12)) in Section 5.5
 *				-> DL PDU Session Information (PDU Type 0)
 *					<- TODO: if ppp is set to 1 then ppi SHOULD set
 *				-> UL PDU Session Information (PDU Type 1)
 *
 * Note: For a G-PDU with serveral Extension Headers, the PDU Session
 * container SHOULD BE the FIRST Extension Header
 */
typedef struct ul_pdu_sess_info {
	__u8	spare_qfi; 			/* Spare(2b) + qfi(6b)*/
} __attribute__((packed)) ul_pdu_sess_info_t;

typedef struct dl_pdu_sess_info {
	__u8	ppp_rqi_qfi;		/* ppp(1b) + rqi(1b) + qfi(6) */
} __attribute__((packed)) dl_pdu_sess_info_t;

typedef struct dl_pdu_sess_info_ppi {
	__u8 	ppi_spare;			/* ppi(3b) + spare(5b) */
	__u8	padding[3];
} __attribute__((packed)) dl_pdu_sess_info_ppi_t;

typedef struct pdu_sess_ctr {
	__u8 type_spare; 			/* type(4b) + spare(4b) */
#define PDU_SESSION_INFO_TYPE0	0x00
#define PDU_SESSION_INFO_TYPE1	0x10
	union {
		ul_pdu_sess_info_t ul;
		dl_pdu_sess_info_t dl;
	} u;	
	//dl_pdu_sess_info_ppi_t dl_ppi[0]; 
} __attribute__((packed)) pdu_sess_ctr_t;

typedef struct gtp1_hdr_ext_pdu_sess_ctr {
	__u8			length;
	pdu_sess_ctr_t 	pdu_sess_ctr;
	__u8			next_ehdr_type;	
} __attribute__((packed)) ext_pdu_sess_ctr_t;


/** -----------------------------------------------------------
 *			END OF General GTP protocol related definitions
 *  ----------------------------------------------------------- 
 */

/* Maybe add this part to if_link.h */
enum ifla_gtp5g_role {
    GTP5G_ROLE_UPF = 0,
    GTP5G_ROLE_RAN,
};

enum {
    IFLA_GTP5G_UNSPEC,

    IFLA_GTP5G_FD1,
    IFLA_GTP5G_PDR_HASHSIZE,
    IFLA_GTP5G_ROLE,

    __IFLA_GTP5G_MAX,
};
#define IFLA_GTP5G_MAX (__IFLA_GTP5G_MAX - 1)
/* end of part */

enum gtp5g_cmd {
    GTP5G_CMD_UNSPEC = 0,

    GTP5G_CMD_ADD_PDR,
    GTP5G_CMD_ADD_FAR,
    GTP5G_CMD_ADD_QER,

    GTP5G_CMD_DEL_PDR,
    GTP5G_CMD_DEL_FAR,
    GTP5G_CMD_DEL_QER,

    GTP5G_CMD_GET_PDR,
    GTP5G_CMD_GET_FAR,
    GTP5G_CMD_GET_QER,

    GTP5G_CMD_ADD_URR,
    GTP5G_CMD_ADD_BAR,
    GTP5G_CMD_DEL_URR,
    GTP5G_CMD_DEL_BAR,
    GTP5G_CMD_GET_URR,
    GTP5G_CMD_GET_BAR,
    /* Add newly supported feature ON ABOVE
     * for compatability with older version of
     * free5GC's UPF or libgtp5gnl
     * */

    __GTP5G_CMD_MAX,
};
#define GTP5G_CMD_MAX (__GTP5G_CMD_MAX - 1)

/* This const value need to bigger than the Layer 1 attr size,
 * like GTP5G_PDR_ATTR_MAX and GTP5G_FAR_ATTR_MAX
 */
#define GTP5G_ATTR_MAX 0x10

enum gtp5g_device_attrs {
    GTP5G_LINK = 1,
    GTP5G_NET_NS_FD,
};

enum gtp5g_pdr_attrs {
    /* gtp5g_device_attrs in this part */

    GTP5G_PDR_ID = 3,
    GTP5G_PDR_PRECEDENCE,
    GTP5G_PDR_PDI,
    GTP5G_OUTER_HEADER_REMOVAL,
    GTP5G_PDR_FAR_ID,

    /* Not in 3GPP spec, just used for routing */
    GTP5G_PDR_ROLE_ADDR_IPV4,

    /* Not in 3GPP spec, just used for buffering */
    GTP5G_PDR_UNIX_SOCKET_PATH,

    GTP5G_PDR_QER_ID,

    GTP5G_PDR_SEID,
    GTP5G_PDR_URR_ID,
    /* Add newly supported feature ON ABOVE
     * for compatability with older version of
     * free5GC's UPF or libgtp5gnl
     * */

    __GTP5G_PDR_ATTR_MAX,
};
#define GTP5G_PDR_ATTR_MAX (__GTP5G_PDR_ATTR_MAX - 1)

/* Nest in GTP5G_PDR_PDI */
enum gtp5g_pdi_attrs {
    GTP5G_PDI_UE_ADDR_IPV4 = 1,
    GTP5G_PDI_F_TEID,
    GTP5G_PDI_SDF_FILTER,

    __GTP5G_PDI_ATTR_MAX,
};
#define GTP5G_PDI_ATTR_MAX (__GTP5G_PDI_ATTR_MAX - 1)

/* Nest in GTP5G_PDI_F_TEID */
enum gtp5g_f_teid_attrs {
    GTP5G_F_TEID_I_TEID = 1,
    GTP5G_F_TEID_GTPU_ADDR_IPV4,

    __GTP5G_F_TEID_ATTR_MAX,
};
#define GTP5G_F_TEID_ATTR_MAX (__GTP5G_F_TEID_ATTR_MAX - 1)

/* Nest in GTP5G_PDI_SDF_FILTER */
enum gtp5g_sdf_filter_attrs {
    GTP5G_SDF_FILTER_FLOW_DESCRIPTION = 1,
    GTP5G_SDF_FILTER_TOS_TRAFFIC_CLASS,
    GTP5G_SDF_FILTER_SECURITY_PARAMETER_INDEX,
    GTP5G_SDF_FILTER_FLOW_LABEL,
    GTP5G_SDF_FILTER_SDF_FILTER_ID,

    __GTP5G_SDF_FILTER_ATTR_MAX,
};
#define GTP5G_SDF_FILTER_ATTR_MAX (__GTP5G_SDF_FILTER_ATTR_MAX - 1)

/* Nest in GTP5G_SDF_FILTER_FLOW_DESCRIPTION */
enum gtp5g_flow_description_attrs {
    GTP5G_FLOW_DESCRIPTION_ACTION = 1, // Only "permit"
    GTP5G_FLOW_DESCRIPTION_DIRECTION,
    GTP5G_FLOW_DESCRIPTION_PROTOCOL,
    GTP5G_FLOW_DESCRIPTION_SRC_IPV4,
    GTP5G_FLOW_DESCRIPTION_SRC_MASK,
    GTP5G_FLOW_DESCRIPTION_DEST_IPV4,
    GTP5G_FLOW_DESCRIPTION_DEST_MASK,
    GTP5G_FLOW_DESCRIPTION_SRC_PORT,
    GTP5G_FLOW_DESCRIPTION_DEST_PORT,

    __GTP5G_FLOW_DESCRIPTION_ATTR_MAX,
};
#define GTP5G_FLOW_DESCRIPTION_ATTR_MAX (__GTP5G_FLOW_DESCRIPTION_ATTR_MAX - 1)

enum gtp5g_far_attrs {
    /* gtp5g_device_attrs in this part */

    GTP5G_FAR_ID = 3,
    GTP5G_FAR_APPLY_ACTION,
    GTP5G_FAR_FORWARDING_PARAMETER,

    /* Not IEs in 3GPP Spec, for other purpose */
    GTP5G_FAR_RELATED_TO_PDR,

    GTP5G_FAR_SEID,
    GTP5G_FAR_BAR_ID,
    __GTP5G_FAR_ATTR_MAX,
};
#define GTP5G_FAR_ATTR_MAX (__GTP5G_FAR_ATTR_MAX - 1)

#define FAR_ACTION_UPSPEC 0x00
#define FAR_ACTION_DROP   0x01
#define FAR_ACTION_FORW   0x02
#define FAR_ACTION_BUFF   0x04
#define FAR_ACTION_MASK   0x07
#define FAR_ACTION_NOCP   0x08
#define FAR_ACTION_DUPL   0x10

/* Nest in GTP5G_FAR_FORWARDING_PARAMETER */
enum gtp5g_forwarding_parameter_attrs {
    GTP5G_FORWARDING_PARAMETER_OUTER_HEADER_CREATION = 1,
    GTP5G_FORWARDING_PARAMETER_FORWARDING_POLICY,

    __GTP5G_FORWARDING_PARAMETER_ATTR_MAX,
};
#define GTP5G_FORWARDING_PARAMETER_ATTR_MAX (__GTP5G_FORWARDING_PARAMETER_ATTR_MAX - 1)

/* Nest in GTP5G_FORWARDING_PARAMETER_OUTER_HEADER_CREATION */
enum gtp5g_outer_header_creation_attrs {
    GTP5G_OUTER_HEADER_CREATION_DESCRIPTION = 1,
    GTP5G_OUTER_HEADER_CREATION_O_TEID,
    GTP5G_OUTER_HEADER_CREATION_PEER_ADDR_IPV4,
    GTP5G_OUTER_HEADER_CREATION_PORT,

    __GTP5G_OUTER_HEADER_CREATION_ATTR_MAX,
};
#define GTP5G_OUTER_HEADER_CREATION_ATTR_MAX (__GTP5G_OUTER_HEADER_CREATION_ATTR_MAX - 1)

enum {
    GTP5G_SDF_FILTER_ACTION_UNSPEC = 0,

    GTP5G_SDF_FILTER_PERMIT,

    __GTP5G_SDF_FILTER_ACTION_MAX,
};
#define GTP5G_SDF_FILTER_ACTION_MAX (__GTP5G_SDF_FILTER_ACTION_MAX - 1)

enum {
    GTP5G_SDF_FILTER_DIRECTION_UNSPEC = 0,

    GTP5G_SDF_FILTER_IN,
    GTP5G_SDF_FILTER_OUT,

    __GTP5G_SDF_FILTER_DIRECTION_MAX,
};
#define GTP5G_SDF_FILTER_DIRECTION_MAX (__GTP5G_SDF_FILTER_DIRECTION_MAX - 1)


/* ------------------------------------------------------------------
 *								QER
 * ------------------------------------------------------------------
 * */
enum gtp5g_qer_attrs {
    /* gtp5g_device_attrs in this part */

    GTP5G_QER_ID = 3,
    GTP5G_QER_GATE,
    GTP5G_QER_MBR,
    GTP5G_QER_GBR,
    GTP5G_QER_CORR_ID,
    GTP5G_QER_RQI,
    GTP5G_QER_QFI,
    GTP5G_QER_PPI,
    GTP5G_QER_RCSR,
	

    /* Not IEs in 3GPP Spec, for other purpose */
    GTP5G_QER_RELATED_TO_PDR,

    GTP5G_QER_SEID,
    __GTP5G_QER_ATTR_MAX,
};
#define GTP5G_QER_ATTR_MAX (__GTP5G_QER_ATTR_MAX - 1)

/* Nest in GTP5G_QER_MBR */
enum gtp5g_mbr_attrs {
    GTP5G_QER_MBR_UL_HIGH32 = 1,
    GTP5G_QER_MBR_UL_LOW8,
    GTP5G_QER_MBR_DL_HIGH32,
    GTP5G_QER_MBR_DL_LOW8,

    __GTP5G_QER_MBR_ATTR_MAX,
};
#define GTP5G_QER_MBR_ATTR_MAX (__GTP5G_QER_MBR_ATTR_MAX - 1)

/* Nest in GTP5G_QER_QBR */
enum gtp5g_qer_gbr_attrs {
    GTP5G_QER_GBR_UL_HIGH32 = 1,
    GTP5G_QER_GBR_UL_LOW8,
    GTP5G_QER_GBR_DL_HIGH32,
    GTP5G_QER_GBR_DL_LOW8,

    __GTP5G_QER_GBR_ATTR_MAX,
};
#define GTP5G_QER_GBR_ATTR_MAX (__GTP5G_QER_GBR_ATTR_MAX - 1)

/* ------------------------------------------------------------------
 *                              URR
 * ------------------------------------------------------------------
 * */
enum gtp5g_urr_attrs {
    GTP5G_URR_ID = 3,
    GTP5G_URR_MEASUREMENT_METHOD,
    GTP5G_URR_REPORTING_TRIGGER,
    GTP5G_URR_MEASUREMENT_PERIOD,
    GTP5G_URR_MEASUREMENT_INFO,
    GTP5G_URR_SEQ, // 3GPP TS 29.244 table 7.5.8.3-1 UR-SEQN
    GTP5G_URR_SEID,

    __GTP5G_URR_ATTR_MAX,
};
#define GTP5G_URR_ATTR_MAX (__GTP5G_URR_ATTR_MAX - 1)

/* ------------------------------------------------------------------
 *	                            BAR
 * ------------------------------------------------------------------
 * */
enum gtp5g_bar_attrs {
    GTP5G_BAR_ID = 3,
    GTP5G_DOWNLINK_DATA_NOTIFICATION_DELAY,
    GTP5G_BUFFERING_PACKETS_COUNT,
    GTP5G_BAR_SEID,

    __GTP5G_BAR_ATTR_MAX,
};
#define GTP5G_BAR_ATTR_MAX (__GTP5G_BAR_ATTR_MAX - 1)

#endif
