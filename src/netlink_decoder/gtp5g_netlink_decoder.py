#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
GTP5G Netlink Decoder 

Parses strace -xx output to decode GTP5G Generic Netlink messages.
Usage: sudo strace -f -e trace=sendmsg,recvmsg -xx -s 65535 -p <UPF_PID> 2>&1 | python3 gtp5g_netlink_decoder.py
"""

import sys
import re
import struct
import subprocess

# ==========================================
# 1. GTP5G Protocol Constants
# ==========================================

# --- Commands ---
GTP5G_CMDS = {
    0: "GTP5G_CMD_UNSPEC",
    1: "GTP5G_CMD_ADD_PDR",
    2: "GTP5G_CMD_ADD_FAR",
    3: "GTP5G_CMD_ADD_QER",
    4: "GTP5G_CMD_DEL_PDR",
    5: "GTP5G_CMD_DEL_FAR",
    6: "GTP5G_CMD_DEL_QER",
    7: "GTP5G_CMD_GET_PDR",
    8: "GTP5G_CMD_GET_FAR",
    9: "GTP5G_CMD_GET_QER",
    10: "GTP5G_CMD_ADD_URR",
    11: "GTP5G_CMD_ADD_BAR",
    12: "GTP5G_CMD_DEL_URR",
    13: "GTP5G_CMD_DEL_BAR",
    14: "GTP5G_CMD_GET_URR",
    15: "GTP5G_CMD_GET_BAR",
    16: "GTP5G_CMD_GET_VERSION",
    17: "GTP5G_CMD_GET_REPORT",
    18: "GTP5G_CMD_BUFFER_GTPU",
    19: "GTP5G_CMD_GET_MULTI_REPORTS",
    20: "GTP5G_CMD_GET_USAGE_STATISTIC",
}

# --- Common Attributes ---
GTP5G_COMMON_ATTRS = {
    1: "GTP5G_LINK",
    2: "GTP5G_NET_NS_FD",
}

# --- PDR Attributes ---
GTP5G_PDR_ATTRS = {
    **GTP5G_COMMON_ATTRS,
    3: "GTP5G_PDR_ID",
    4: "GTP5G_PDR_PRECEDENCE",
    5: "GTP5G_PDR_PDI",
    6: "GTP5G_OUTER_HEADER_REMOVAL",
    7: "GTP5G_PDR_FAR_ID",
    8: "GTP5G_PDR_ROLE_ADDR_IPV4",
    9: "GTP5G_PDR_UNIX_SOCKET_PATH",
    10: "GTP5G_PDR_QER_ID",
    11: "GTP5G_PDR_SEID",
    12: "GTP5G_PDR_URR_ID",
}

# --- FAR Attributes ---
GTP5G_FAR_ATTRS = {
    **GTP5G_COMMON_ATTRS,
    3: "GTP5G_FAR_ID",
    4: "GTP5G_FAR_APPLY_ACTION",
    5: "GTP5G_FAR_FORWARDING_PARAMETER",
    6: "GTP5G_FAR_RELATED_TO_PDR",
    7: "GTP5G_FAR_SEID",
    8: "GTP5G_FAR_BAR_ID",
}

# --- QER Attributes ---
GTP5G_QER_ATTRS = {
    **GTP5G_COMMON_ATTRS,
    3: "GTP5G_QER_ID",
    4: "GTP5G_QER_GATE",
    5: "GTP5G_QER_MBR",
    6: "GTP5G_QER_GBR",
    7: "GTP5G_QER_CORR_ID",
    8: "GTP5G_QER_RQI",
    9: "GTP5G_QER_QFI",
    10: "GTP5G_QER_PPI",
    11: "GTP5G_QER_RCSR",
    12: "GTP5G_QER_RELATED_TO_PDR",
    13: "GTP5G_QER_SEID",
}

# --- URR Attributes ---
GTP5G_URR_ATTRS = {
    **GTP5G_COMMON_ATTRS,
    3: "GTP5G_URR_ID",
    4: "GTP5G_URR_MEASUREMENT_METHOD",
    5: "GTP5G_URR_REPORTING_TRIGGER",
    6: "GTP5G_URR_MEASUREMENT_PERIOD",
    7: "GTP5G_URR_MEASUREMENT_INFO",
    8: "GTP5G_URR_SEID",
    9: "GTP5G_URR_VOLUME_THRESHOLD",
    10: "GTP5G_URR_VOLUME_QUOTA",
    11: "GTP5G_URR_MULTI_SEID_URRID",
    12: "GTP5G_URR_NUM",
    13: "GTP5G_URR_RELATED_TO_PDR",
}

# --- BAR Attributes ---
GTP5G_BAR_ATTRS = {
    **GTP5G_COMMON_ATTRS,
    3: "GTP5G_BAR_ID",
    4: "GTP5G_DOWNLINK_DATA_NOTIFICATION_DELAY",
    5: "GTP5G_BUFFERING_PACKETS_COUNT",
    6: "GTP5G_BAR_SEID",
}

# --- Report Attributes ---
GTP5G_REPORT_ATTRS = {
    **GTP5G_COMMON_ATTRS,
    3: "GTP5G_UR_URRID",
    4: "GTP5G_UR_USAGE_REPORT_TRIGGER",
    5: "GTP5G_UR_URSEQN",
    6: "GTP5G_UR_VOLUME_MEASUREMENT",
    7: "GTP5G_UR_QUERY_URR_REFERENCE",
    8: "GTP5G_UR_START_TIME",
    9: "GTP5G_UR_END_TIME",
    10: "GTP5G_UR_SEID",
}

# --- PDI Nested Attributes ---
GTP5G_PDI_ATTRS = {
    1: "GTP5G_PDI_UE_ADDR_IPV4",
    2: "GTP5G_PDI_F_TEID",
    3: "GTP5G_PDI_SDF_FILTER",
    4: "GTP5G_PDI_SRC_INTF",
}

# --- F-TEID Nested Attributes ---
GTP5G_F_TEID_ATTRS = {
    1: "GTP5G_F_TEID_I_TEID",
    2: "GTP5G_F_TEID_GTPU_ADDR_IPV4",
}

# --- SDF Filter Nested Attributes ---
GTP5G_SDF_FILTER_ATTRS = {
    1: "GTP5G_SDF_FILTER_FLOW_DESCRIPTION",
    2: "GTP5G_SDF_FILTER_TOS_TRAFFIC_CLASS",
    3: "GTP5G_SDF_FILTER_SECURITY_PARAMETER_INDEX",
    4: "GTP5G_SDF_FILTER_FLOW_LABEL",
    5: "GTP5G_SDF_FILTER_SDF_FILTER_ID",
}

# --- FAR Forwarding Parameter Nested Attributes ---
GTP5G_FAR_FP_ATTRS = {
    1: "GTP5G_FORWARDING_PARAMETER_OUTER_HEADER_CREATION",
    2: "GTP5G_FORWARDING_PARAMETER_FORWARDING_POLICY",
    3: "GTP5G_FORWARDING_PARAMETER_PFCPSM_REQ_FLAGS",
    4: "GTP5G_FORWARDING_PARAMETER_TOS_TC",
}

# --- Outer Header Creation Attributes ---
GTP5G_OHC_ATTRS = {
    1: "GTP5G_OUTER_HEADER_CREATION_DESCRIPTION",
    2: "GTP5G_OUTER_HEADER_CREATION_O_TEID",
    3: "GTP5G_OUTER_HEADER_CREATION_PEER_ADDR_IPV4",
    4: "GTP5G_OUTER_HEADER_CREATION_PORT",
}

# --- QER MBR Nested Attributes ---
GTP5G_QER_MBR_ATTRS = {
    1: "GTP5G_QER_MBR_UL_HIGH32",
    2: "GTP5G_QER_MBR_UL_LOW8",
    3: "GTP5G_QER_MBR_DL_HIGH32",
    4: "GTP5G_QER_MBR_DL_LOW8",
}

# --- QER GBR Nested Attributes ---
GTP5G_QER_GBR_ATTRS = {
    1: "GTP5G_QER_GBR_UL_HIGH32",
    2: "GTP5G_QER_GBR_UL_LOW8",
    3: "GTP5G_QER_GBR_DL_HIGH32",
    4: "GTP5G_QER_GBR_DL_LOW8",
}

# --- URR Volume Threshold Nested Attributes ---
GTP5G_URR_VOLUME_THRESHOLD_ATTRS = {
    1: "GTP5G_URR_VOLUME_THRESHOLD_FLAG",
    2: "GTP5G_URR_VOLUME_THRESHOLD_TOVOL",
    3: "GTP5G_URR_VOLUME_THRESHOLD_UVOL",
    4: "GTP5G_URR_VOLUME_THRESHOLD_DVOL",
}

# --- URR Volume Quota Nested Attributes ---
GTP5G_URR_VOLUME_QUOTA_ATTRS = {
    1: "GTP5G_URR_VOLUME_QUOTA_FLAG",
    2: "GTP5G_URR_VOLUME_QUOTA_TOVOL",
    3: "GTP5G_URR_VOLUME_QUOTA_UVOL",
    4: "GTP5G_URR_VOLUME_QUOTA_DVOL",
}

# --- UR Volume Measurement Nested Attributes ---
GTP5G_UR_VOLUME_MEASUREMENT_ATTRS = {
    1: "GTP5G_UR_VOLUME_MEASUREMENT_FLAGS",
    2: "GTP5G_UR_VOLUME_MEASUREMENT_TOVOL",
    3: "GTP5G_UR_VOLUME_MEASUREMENT_UVOL",
    4: "GTP5G_UR_VOLUME_MEASUREMENT_DVOL",
    5: "GTP5G_UR_VOLUME_MEASUREMENT_TOPACKET",
    6: "GTP5G_UR_VOLUME_MEASUREMENT_UPACKET",
    7: "GTP5G_UR_VOLUME_MEASUREMENT_DPACKET",
}

# --- Multi Reports Nested Attributes ---
GTP5G_MULTI_REPORT_ATTRS = {
    **GTP5G_COMMON_ATTRS,
    5: "GTP5G_UR",
    11: "GTP5G_URR_MULTI_SEID_URRID",
    12: "GTP5G_URR_NUM",
}

# --- Usage Statistic Attributes ---
GTP5G_USAGE_STATISTIC_ATTRS = {
    **GTP5G_COMMON_ATTRS,
    1: "GTP5G_USTAT_UL_VOL_RX",
    2: "GTP5G_USTAT_UL_VOL_TX",
    3: "GTP5G_USTAT_DL_VOL_RX",
    4: "GTP5G_USTAT_DL_VOL_TX",
    5: "GTP5G_USTAT_UL_PKT_RX",
    6: "GTP5G_USTAT_UL_PKT_TX",
    7: "GTP5G_USTAT_DL_PKT_RX",
    8: "GTP5G_USTAT_DL_PKT_TX",
}

# --- URR Multi SEID URRID Nested Attributes (reuses URR attrs) ---
GTP5G_URR_MULTI_SEID_URRID_ATTRS = {
    3: "GTP5G_URR_ID",
    8: "GTP5G_URR_SEID",
}

# --- Flow Description Nested Attributes ---
GTP5G_FLOW_DESCRIPTION_ATTRS = {
    1: "GTP5G_FLOW_DESCRIPTION_ACTION",
    2: "GTP5G_FLOW_DESCRIPTION_DIRECTION",
    3: "GTP5G_FLOW_DESCRIPTION_PROTOCOL",
    4: "GTP5G_FLOW_DESCRIPTION_SRC_IPV4",
    5: "GTP5G_FLOW_DESCRIPTION_SRC_MASK",
    6: "GTP5G_FLOW_DESCRIPTION_DEST_IPV4",
    7: "GTP5G_FLOW_DESCRIPTION_DEST_MASK",
    8: "GTP5G_FLOW_DESCRIPTION_SRC_PORT",
    9: "GTP5G_FLOW_DESCRIPTION_DEST_PORT",
}

CURRENT_GTP5G_FAMILY_ID = None

# ==========================================
# 2. Utility Functions
# ==========================================

def get_gtp5g_family_id():
    """Get gtp5g Generic Netlink family ID using 'genl' command."""
    try:
        result = subprocess.run(
            ["genl", "ctrl", "list", "name", "gtp5g"], 
            capture_output=True, 
            text=True
        )
        
        if result.returncode != 0:
            return None

        lines = result.stdout.split('\n')
        found_name = False
        for line in lines:
            if "Name: gtp5g" in line:
                found_name = True
            elif found_name and "ID:" in line:
                match = re.search(r'ID:\s+(0x[0-9a-fA-F]+)', line)
                if match:
                    hex_id = match.group(1)
                    fam_id = int(hex_id, 16)
                    print(f"[Init] Detected gtp5g Family ID: {fam_id} ({hex_id})")
                    return fam_id
            if found_name and "Name:" in line:
                break

        match = re.search(r'Name:\s+gtp5g\s+ID:\s+(0x[0-9a-fA-F]+)', result.stdout)
        if match:
            hex_id = match.group(1)
            fam_id = int(hex_id, 16)
            print(f"[Init] Detected gtp5g Family ID: {fam_id} ({hex_id})")
            return fam_id

    except FileNotFoundError:
        print("[Init] Error: 'genl' command not found")
    except Exception as e:
        print(f"[Init] Warning: Could not detect gtp5g family: {e}")

    return None

def decode_value(attr_name, data):
    """Decode attribute value based on attribute name and data type.
    
    Returns the decoded value (int, string, or IP address).
    """
    try:
        # IPv4 addresses (network byte order / big-endian)
        if "IPV4" in attr_name or "ADDR_IPV4" in attr_name:
            if len(data) >= 4:
                return f"{data[0]}.{data[1]}.{data[2]}.{data[3]}"
        
        # SEID and timestamps (U64)
        if "SEID" in attr_name or "TIME" in attr_name:
            if len(data) >= 8:
                return struct.unpack("=Q", data[:8])[0]
        
        # U64 Volume values (URR thresholds, quotas, measurements)
        if attr_name in ["GTP5G_URR_VOLUME_THRESHOLD_TOVOL", "GTP5G_URR_VOLUME_THRESHOLD_UVOL",
                         "GTP5G_URR_VOLUME_THRESHOLD_DVOL",
                         "GTP5G_URR_VOLUME_QUOTA_TOVOL", "GTP5G_URR_VOLUME_QUOTA_UVOL",
                         "GTP5G_URR_VOLUME_QUOTA_DVOL",
                         "GTP5G_UR_VOLUME_MEASUREMENT_TOVOL", "GTP5G_UR_VOLUME_MEASUREMENT_UVOL",
                         "GTP5G_UR_VOLUME_MEASUREMENT_DVOL",
                         "GTP5G_UR_VOLUME_MEASUREMENT_TOPACKET", "GTP5G_UR_VOLUME_MEASUREMENT_UPACKET",
                         "GTP5G_UR_VOLUME_MEASUREMENT_DPACKET"]:
            if len(data) >= 8:
                return struct.unpack("=Q", data[:8])[0]
            elif len(data) >= 4:
                return struct.unpack("=I", data[:4])[0]
        
        # U32 values (IDs, TEIDs, counters, etc.)
        if attr_name in ["GTP5G_LINK", "GTP5G_NET_NS_FD",
                         "GTP5G_FAR_ID", "GTP5G_QER_ID", "GTP5G_QER_CORR_ID",
                         "GTP5G_PDR_FAR_ID", "GTP5G_PDR_QER_ID", "GTP5G_PDR_URR_ID",
                         "GTP5G_URR_ID", "GTP5G_URR_MEASUREMENT_METHOD",
                         "GTP5G_URR_REPORTING_TRIGGER", "GTP5G_URR_MEASUREMENT_PERIOD",
                         "GTP5G_URR_NUM",
                         "GTP5G_PDR_PRECEDENCE", "GTP5G_F_TEID_I_TEID",
                         "GTP5G_OUTER_HEADER_CREATION_O_TEID",
                         "GTP5G_SDF_FILTER_SECURITY_PARAMETER_INDEX",
                         "GTP5G_SDF_FILTER_FLOW_LABEL", "GTP5G_SDF_FILTER_SDF_FILTER_ID",
                         "GTP5G_UR_URRID", "GTP5G_UR_URSEQN", "GTP5G_UR_QUERY_URR_REFERENCE",
                         "GTP5G_UR_USAGE_REPORT_TRIGGER",
                         "GTP5G_QER_MBR_UL_HIGH32", "GTP5G_QER_MBR_DL_HIGH32",
                         "GTP5G_QER_GBR_UL_HIGH32", "GTP5G_QER_GBR_DL_HIGH32"]:
            if len(data) >= 4:
                return struct.unpack("=I", data[:4])[0]
        
        # Flow Description MASK (network byte order, display as dotted decimal)
        if attr_name in ["GTP5G_FLOW_DESCRIPTION_SRC_MASK", "GTP5G_FLOW_DESCRIPTION_DEST_MASK"]:
            if len(data) >= 4:
                mask_val = struct.unpack(">I", data[:4])[0]
                mask_ip = f"{(mask_val >> 24) & 0xff}.{(mask_val >> 16) & 0xff}.{(mask_val >> 8) & 0xff}.{mask_val & 0xff}"
                return mask_ip
        
        # Flow Description PORT (array of U32, each contains start:end port range)
        if attr_name in ["GTP5G_FLOW_DESCRIPTION_SRC_PORT", "GTP5G_FLOW_DESCRIPTION_DEST_PORT"]:
            if len(data) == 0:
                return "(none)"
            port_ranges = []
            for i in range(0, len(data), 4):
                if i + 4 <= len(data):
                    port_val = struct.unpack("=I", data[i:i+4])[0]
                    port1 = port_val & 0xFFFF
                    port2 = (port_val >> 16) & 0xFFFF
                    if port1 == port2:
                        port_ranges.append(str(port1))
                    else:
                        port_ranges.append(f"{min(port1,port2)}-{max(port1,port2)}")
            return ",".join(port_ranges) if port_ranges else "(none)"
        
        # PDR_ID (U16)
        if attr_name == "GTP5G_PDR_ID":
            if len(data) >= 2:
                return struct.unpack("=H", data[:2])[0]
        
        # U16 values (ports, related PDR IDs, action flags)
        if attr_name in ["GTP5G_FAR_RELATED_TO_PDR", "GTP5G_QER_RELATED_TO_PDR",
                         "GTP5G_URR_RELATED_TO_PDR", "GTP5G_OUTER_HEADER_CREATION_PORT",
                         "GTP5G_OUTER_HEADER_CREATION_DESCRIPTION",
                         "GTP5G_SDF_FILTER_TOS_TRAFFIC_CLASS",
                         "GTP5G_BUFFERING_PACKETS_COUNT",
                         "GTP5G_FAR_APPLY_ACTION"]:
            if len(data) >= 2:
                return struct.unpack("=H", data[:2])[0]
        
        # U8 values (flags, QFI, gate status, protocol numbers)
        if attr_name in ["GTP5G_OUTER_HEADER_REMOVAL",
                         "GTP5G_PDI_SRC_INTF", "GTP5G_QER_GATE", "GTP5G_BAR_ID",
                         "GTP5G_QER_RQI", "GTP5G_QER_QFI", "GTP5G_QER_PPI", "GTP5G_QER_RCSR",
                         "GTP5G_URR_MEASUREMENT_INFO", "GTP5G_DOWNLINK_DATA_NOTIFICATION_DELAY",
                         "GTP5G_QER_MBR_UL_LOW8", "GTP5G_QER_MBR_DL_LOW8",
                         "GTP5G_QER_GBR_UL_LOW8", "GTP5G_QER_GBR_DL_LOW8",
                         "GTP5G_FORWARDING_PARAMETER_PFCPSM_REQ_FLAGS",
                         "GTP5G_FORWARDING_PARAMETER_TOS_TC",
                         "GTP5G_UR_VOLUME_MEASUREMENT_FLAGS",
                         "GTP5G_URR_VOLUME_THRESHOLD_FLAG", "GTP5G_URR_VOLUME_QUOTA_FLAG",
                         "GTP5G_FLOW_DESCRIPTION_ACTION", "GTP5G_FLOW_DESCRIPTION_DIRECTION",
                         "GTP5G_FLOW_DESCRIPTION_PROTOCOL"]:
            if len(data) >= 1:
                return struct.unpack("=B", data[:1])[0]
        
        # String values (paths, policies)
        if attr_name in ["GTP5G_PDR_UNIX_SOCKET_PATH",
                         "GTP5G_FORWARDING_PARAMETER_FORWARDING_POLICY",
                         "GTP5G_SDF_FILTER_FLOW_DESCRIPTION"]:
            return data.decode('utf-8', errors='ignore').rstrip('\x00')
    
    except Exception:
        pass
    
    # Fallback: return hex string for unknown types
    return "0x" + data.hex() if data else "(empty)"

def parse_attributes(data, mapping):
    """Parse Netlink attributes from binary data.
    
    Args:
        data: Raw bytes containing Netlink attributes
        mapping: Dictionary mapping attribute type IDs to names
    
    Returns:
        Dictionary of parsed attributes with decoded values
    """
    attrs = {}
    offset = 0
    length = len(data)
    
    # Nested attribute type to sub-mapping lookup
    nested_mappings = {
        "GTP5G_PDR_PDI": GTP5G_PDI_ATTRS,
        "GTP5G_PDI_F_TEID": GTP5G_F_TEID_ATTRS,
        "GTP5G_PDI_SDF_FILTER": GTP5G_SDF_FILTER_ATTRS,
        "GTP5G_SDF_FILTER_FLOW_DESCRIPTION": GTP5G_FLOW_DESCRIPTION_ATTRS,
        "GTP5G_FAR_FORWARDING_PARAMETER": GTP5G_FAR_FP_ATTRS,
        "GTP5G_FORWARDING_PARAMETER_OUTER_HEADER_CREATION": GTP5G_OHC_ATTRS,
        "GTP5G_QER_MBR": GTP5G_QER_MBR_ATTRS,
        "GTP5G_QER_GBR": GTP5G_QER_GBR_ATTRS,
        "GTP5G_URR_VOLUME_THRESHOLD": GTP5G_URR_VOLUME_THRESHOLD_ATTRS,
        "GTP5G_URR_VOLUME_QUOTA": GTP5G_URR_VOLUME_QUOTA_ATTRS,
        "GTP5G_UR_VOLUME_MEASUREMENT": GTP5G_UR_VOLUME_MEASUREMENT_ATTRS,
        "GTP5G_UR": GTP5G_REPORT_ATTRS,
        "GTP5G_URR_MULTI_SEID_URRID": GTP5G_URR_MULTI_SEID_URRID_ATTRS,
    }
    
    while offset < length:
        # Need at least 4 bytes for NLA header (len + type)
        if length - offset < 4:
            break
        
        nla_len, nla_type = struct.unpack("=HH", data[offset:offset+4])
        
        # Skip invalid attributes
        if nla_len == 0:
            offset += 4
            continue
        if nla_len < 4 or nla_len > length - offset:
            break
        
        # Extract type ID (mask out NLA_F_NESTED and NLA_F_NET_BYTEORDER flags)
        type_id = nla_type & 0x3FFF
        attr_name = mapping.get(type_id, f"UNKNOWN_ATTR_{type_id}")
        
        # Type 0 is a container: expand its contents with same mapping
        if type_id == 0:
            container_data = data[offset+4:offset+nla_len]
            nested_attrs = parse_attributes(container_data, mapping)
            attrs.update(nested_attrs)
            aligned_len = (nla_len + 3) & ~3
            offset += aligned_len
            continue
        
        # Known nested attributes: recursively parse with appropriate sub-mapping
        if attr_name in nested_mappings:
            nested_data = data[offset+4:offset+nla_len]
            sub_mapping = nested_mappings[attr_name]
            nested_attrs = parse_attributes(nested_data, sub_mapping)
            attrs[attr_name] = nested_attrs
            aligned_len = (nla_len + 3) & ~3
            offset += aligned_len
            continue
        
        # Regular attribute: decode value based on type
        val_data = data[offset+4:offset+nla_len]
        attrs[attr_name] = decode_value(attr_name, val_data)
        
        aligned_len = (nla_len + 3) & ~3
        offset += aligned_len
    
    return attrs

def format_attrs(attrs, indent=0):
    """Format parsed attributes for display output."""
    if not attrs:
        return "  (empty)"
    
    lines = []
    prefix = "  " * (indent + 1)
    for key, value in attrs.items():
        if isinstance(value, dict):
            # Nested attribute group
            lines.append(f"{prefix}{key}:")
            lines.append(format_attrs(value, indent + 1))
        else:
            # Simple value
            lines.append(f"{prefix}{key}: {value}")
    return "\n".join(lines)

# ==========================================
# 3. Main Processing Logic
# ==========================================

def process_line(line):
    """Process a single line of strace output.
    
    Extracts Netlink message from strace sendmsg/recvmsg output,
    parses the GTP5G Generic Netlink payload, and prints decoded attributes.
    """
    # Skip error responses and incomplete lines
    if 'NLMSG_ERROR' in line or 'unfinished' in line.lower():
        return
    
    if 'sendmsg' not in line and 'recvmsg' not in line:
        return
    
    # Extract Netlink message header fields
    header_match = re.search(
        r'iov_base=\{len=(\d+),\s*type=([^,]+),\s*flags=([^,]+),\s*seq=(\d+),\s*pid=(\d+)\}',
        line
    )
    
    if not header_match:
        return
    
    msg_len = int(header_match.group(1))
    msg_type_str = header_match.group(2).strip()
    flags_str = header_match.group(3).strip()
    seq = int(header_match.group(4))
    pid = int(header_match.group(5))
    
    # Parse message type (Generic Netlink family ID)
    if 'gtp5g' in msg_type_str:
        msg_type = CURRENT_GTP5G_FAMILY_ID if CURRENT_GTP5G_FAMILY_ID else 31
    else:
        type_match = re.search(r'0x([0-9a-fA-F]+)', msg_type_str)
        msg_type = int(type_match.group(1), 16) if type_match else 0
    
    # Filter out non-gtp5g messages
    if CURRENT_GTP5G_FAMILY_ID and msg_type != CURRENT_GTP5G_FAMILY_ID:
        return
    
    # Parse Netlink message flags
    flags = 0
    if 'NLM_F_REQUEST' in flags_str:
        flags |= 0x0001
    if 'NLM_F_ACK' in flags_str:
        flags |= 0x0004
    if '0x200' in flags_str:
        flags |= 0x0200
    if '0x100' in flags_str:
        flags |= 0x0100
    
    # Extract payload data from iov_base fields in order of appearance
    # strace may output iov data in different formats depending on content
    iov_patterns = [
        # Pattern 1: Simple hex string iov_base="\x..."
        (r'iov_base="((?:\\x[0-9a-fA-F]{2})+)"', 'hex'),
        # Pattern 2: Nested structure where strace decoded first 16 bytes as nlmsghdr
        # Format: iov_base={{len=N, type=X, flags=N, seq=N, pid=N}, "\x..."}
        (r'iov_base=\{\{len=(\d+),\s*type=([^,]+),\s*flags=(\d+),\s*seq=(\d+),\s*pid=(\d+)\},\s*"((?:\\x[0-9a-fA-F]{2})+)"\}', 'nested_full'),
        # Pattern 3: Standalone decoded nlmsghdr without trailing hex data
        (r'iov_base=\{len=(\d+),\s*type=([^,]+),\s*flags=(\d+),\s*seq=(\d+),\s*pid=(\d+)\}(?!,\s*")', 'fake_header'),
    ]
    
    # Collect all iov data segments with their positions
    iov_items = []
    
    for pattern, ptype in iov_patterns:
        for match in re.finditer(pattern, line):
            # Skip the real Netlink header (contains 'gtp5g' family name)
            if ptype in ('fake_header', 'nested_full') and 'gtp5g' in match.group(0):
                continue
            iov_items.append((match.start(), ptype, match))
    
    # Sort by position to maintain correct byte order
    iov_items.sort(key=lambda x: x[0])
    
    # Assemble payload from collected iov segments
    payload_bytes = b''
    for pos, ptype, match in iov_items:
        try:
            if ptype == 'hex':
                hex_str = match.group(1)
                payload_bytes += bytes.fromhex(hex_str.replace('\\x', ''))
            elif ptype == 'nested_full':
                # Handle nested structure: rebuild the 16-byte header then append hex data
                len_val = int(match.group(1))
                type_str = match.group(2).strip()
                flags_val = int(match.group(3))
                seq_val = int(match.group(4))
                pid_val = int(match.group(5))
                hex_str = match.group(6)
                
                # Parse type value from various strace output formats
                type_val = 0
                if 'NLMSG_???' in type_str or 'GENERIC_FAMILY_???' in type_str:
                    hex_match = re.match(r'(0x[0-9a-fA-F]+|\d+)', type_str)
                    if hex_match:
                        val_str = hex_match.group(1)
                        type_val = int(val_str, 16) if val_str.startswith('0x') else int(val_str)
                elif type_str.startswith('0x'):
                    hex_match = re.match(r'0x([0-9a-fA-F]+)', type_str)
                    if hex_match:
                        type_val = int(hex_match.group(1), 16)
                elif type_str.isdigit():
                    type_val = int(type_str)
                
                # Rebuild 16-byte nlmsghdr structure
                rebuilt = struct.pack("=I", len_val)
                rebuilt += struct.pack("=HH", type_val, flags_val)
                rebuilt += struct.pack("=I", seq_val)
                rebuilt += struct.pack("=I", pid_val)
                payload_bytes += rebuilt
                
                # Append the trailing hex data
                payload_bytes += bytes.fromhex(hex_str.replace('\\x', ''))
            elif ptype == 'fake_header':
                len_val = int(match.group(1))
                type_str = match.group(2).strip()
                flags_val = int(match.group(3))
                seq_val = int(match.group(4))
                pid_val = int(match.group(5))
                
                # Parse type value from strace output
                type_val = 0
                if type_str == 'NLMSG_OVERRUN':
                    type_val = 4  # NLMSG_OVERRUN = 4
                elif 'NLMSG_???' in type_str or 'GENERIC_FAMILY_???' in type_str:
                    hex_match = re.match(r'(0x[0-9a-fA-F]+|\d+)', type_str)
                    if hex_match:
                        val_str = hex_match.group(1)
                        type_val = int(val_str, 16) if val_str.startswith('0x') else int(val_str)
                elif type_str.startswith('0x'):
                    hex_match = re.match(r'0x([0-9a-fA-F]+)', type_str)
                    if hex_match:
                        type_val = int(hex_match.group(1), 16)
                elif type_str.isdigit():
                    type_val = int(type_str)
                
                # Rebuild original 16-byte nlmsghdr structure
                rebuilt = struct.pack("=I", len_val)
                rebuilt += struct.pack("=HH", type_val, flags_val)
                rebuilt += struct.pack("=I", seq_val)
                rebuilt += struct.pack("=I", pid_val)
                payload_bytes += rebuilt
        except (ValueError, struct.error):
            continue
    
    if len(payload_bytes) < 4:
        return
    
    # Parse Generic Netlink header (4 bytes: cmd, version, reserved)
    cmd, version, reserved = struct.unpack("=BBH", payload_bytes[:4])
    cmd_str = GTP5G_CMDS.get(cmd, f"UNKNOWN_CMD_{cmd}")
    
    # Select attribute mapping based on command type
    # Note: GET_REPORT (17) requests use URR_ATTRS (URR_ID=3, URR_SEID=8)
    # Responses would use REPORT_ATTRS, but we primarily parse requests
    attr_mappings = {
        1: GTP5G_PDR_ATTRS, 4: GTP5G_PDR_ATTRS, 7: GTP5G_PDR_ATTRS,
        2: GTP5G_FAR_ATTRS, 5: GTP5G_FAR_ATTRS, 8: GTP5G_FAR_ATTRS,
        3: GTP5G_QER_ATTRS, 6: GTP5G_QER_ATTRS, 9: GTP5G_QER_ATTRS,
        10: GTP5G_URR_ATTRS, 12: GTP5G_URR_ATTRS, 14: GTP5G_URR_ATTRS,
        11: GTP5G_BAR_ATTRS, 13: GTP5G_BAR_ATTRS, 15: GTP5G_BAR_ATTRS,
        17: GTP5G_URR_ATTRS,  # GET_REPORT 請求使用 URR attrs (URR_ID, URR_SEID)
        19: GTP5G_MULTI_REPORT_ATTRS,
        20: GTP5G_USAGE_STATISTIC_ATTRS,
    }
    attr_mapping = attr_mappings.get(cmd, GTP5G_COMMON_ATTRS)
    
    # Parse attributes from payload (skip 4-byte GenL header)
    attrs_data = payload_bytes[4:]
    attributes = parse_attributes(attrs_data, attr_mapping)
    
    # Output decoded message
    print("-" * 60)
    print(f"GTP5G MESSAGE")
    print(f"Len: {msg_len}, FamilyID: {msg_type}, Seq: {seq}")
    print(f"Command: {cmd_str} (v{version})")
    print("Attributes:")
    print(format_attrs(attributes))
    print("-" * 60)
    sys.stdout.flush()

# ==========================================
# 4. Entry Point
# ==========================================

if __name__ == "__main__":
    sys.stderr = sys.stdout
    sys.stdout.reconfigure(line_buffering=True)
    sys.stdin.reconfigure(line_buffering=True)
    
    CURRENT_GTP5G_FAMILY_ID = get_gtp5g_family_id()
    if CURRENT_GTP5G_FAMILY_ID is None:
        print("[Warning] Could not detect gtp5g family ID, defaulting to 31")
        CURRENT_GTP5G_FAMILY_ID = 31
    
    print(f"[Init] Decoder started. Target Family ID: {CURRENT_GTP5G_FAMILY_ID}")
    print("[Init] Waiting for strace input...")
    
    try:
        for line in sys.stdin:
            process_line(line)
    except KeyboardInterrupt:
        print("\n[Exit] Decoder stopped")
    except Exception as e:
        print(f"[Error] {e}")