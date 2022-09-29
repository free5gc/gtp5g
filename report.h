#ifndef __REPORT_H__
#define __REPORT_H__

#include <linux/kernel.h>
#include <linux/rculist.h>
#include <linux/net.h>

#include "dev.h"

#define  TRIGGER_PERIO 0x10000
#define  TRIGGER_VOLTH 0x20000
#define  TRIGGER_TIMTH 0x40000
#define  TRIGGER_QUHTI 0x80000
#define  TRIGGER_START 0x100000
#define  TRIGGER_STOPT 0x200000
#define  TRIGGER_DROTH 0x400000
#define  TRIGGER_IMMER 0x800000
#define  TRIGGER_VOLQU 0x100 
#define  TRIGGER_TIMQU 0x200
#define  TRIGGER_LIUSA 0x400
#define  TRIGGER_TERMR 0x800
#define  TRIGGER_MONIT 0x1000 
#define  TRIGGER_ENVCL 0x2000 
#define  TRIGGER_MACAR 0x4000
#define  TRIGGER_EVETH 0x8000 
#define  TRIGGER_EVEQU 0x1 
#define  TRIGGER_TEBUR 0x2
#define  TRIGGER_IPMJL 0x4 
#define  TRIGGER_QUVTI 0x8 
#define  TRIGGER_EMRRE 0x10 

struct VolumeMeasurement{
    u64 totalVolume;
    u64 uplinkVolume;
    u64 downlinkVolume;
    u64 totalPktNum;
    u64 uplinkPktNum;
    u64 downlinkPktNum;
}__attribute__((packed));

struct user_report {
	uint32_t urrid; 					/* 8.2.54 URR_ID */
    
    uint64_t trigger ;
    struct VolumeMeasurement volmeasurement;

    uint32_t queryUrrRef;
    ktime_t start_time;
    ktime_t end_time;
} __attribute__((packed));

#endif // __REPORT_H__