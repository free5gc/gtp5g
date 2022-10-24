#ifndef __REPORT_H__
#define __REPORT_H__

#include <linux/kernel.h>
#include <linux/rculist.h>
#include <linux/net.h>

#include "dev.h"

#define TRIGGER_EVEQU  (1 << 0)
#define TRIGGER_TEBUR  (1 << 1)
#define TRIGGER_IPMJL  (1 << 2)
#define TRIGGER_QUVTI  (1 << 3)
#define TRIGGER_EMRRE  (1 << 4)
#define TRIGGER_VOLQU  (1 << 8)
#define TRIGGER_TIMQU  (1 << 9)
#define TRIGGER_LIUSA (1 << 10)
#define TRIGGER_TERMR (1 << 11)
#define TRIGGER_MONIT (1 << 12)
#define TRIGGER_ENVCL (1 << 13)
#define TRIGGER_MACAR (1 << 14)
#define TRIGGER_EVETH (1 << 15)
#define TRIGGER_PERIO (1 << 16)
#define TRIGGER_VOLTH (1 << 17)
#define TRIGGER_TIMTH (1 << 18)
#define TRIGGER_QUHTI (1 << 19)
#define TRIGGER_START (1 << 20)
#define TRIGGER_STOPT (1 << 21)
#define TRIGGER_DROTH (1 << 22)
#define TRIGGER_IMMER (1 << 23)

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
