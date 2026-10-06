#ifndef PTP_H
#define PTP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IEEE 1588-2008 (PTPv2) slave-only ordinary clock over UDP/IPv4 multicast,
 * end-to-end delay mechanism, domain 0 -- the defaults of linuxptp's ptp4l.
 * Disciplines the ETH MAC's hardware clock (ptp_clock.h) to the master with a
 * PI servo, continuously, so oscillator drift is tracked for as long as the
 * master keeps sending Sync messages. */

typedef enum
{
  PTP_STATE_LISTENING,    /* no master heard (yet, or it went quiet) */
  PTP_STATE_UNCALIBRATED, /* master selected, estimating initial drift */
  PTP_STATE_SLAVE,        /* clock stepped onto master time, servo running */
} ptp_state_t;

typedef struct
{
  ptp_state_t state;
  uint8_t locked;           /* SLAVE and recent offsets below PTP_LOCK_THRESHOLD_NS */
  int64_t offset_ns;        /* last measured offset from master (local - master) */
  uint32_t offset_rms_ns;   /* RMS of the last few offsets */
  int64_t path_delay_ns;    /* filtered mean one-way path delay */
  int32_t freq_ppb;         /* current frequency correction applied to the clock */
  int16_t utc_offset;       /* TAI - UTC in seconds, from the master's Announce */
  uint8_t utc_offset_valid;
  uint8_t master_id[8];     /* master's clockIdentity (valid unless LISTENING) */
  uint32_t sync_count;      /* Sync/Follow_Up pairs used */
  uint32_t delay_count;     /* Delay_Req/Delay_Resp exchanges completed */
  int8_t log_sync_interval;
} ptp_status_t;

/* A clock counts as locked once the RMS offset of the last few Syncs is
 * below this. Recorded with every trigger event. Sized for the stock board's
 * HSI oscillator, which tracks to ~1-2 us RMS at 64 Syncs/s; with a crystal
 * on HSE this could be tightened by an order of magnitude or more. */
#define PTP_LOCK_THRESHOLD_NS 10000

/* Cheap, interrupt-safe snapshot of status.offset_rms_ns. */
uint32_t Ptp_OffsetRms(void);

/* Brings up the hardware clock, opens UDP 319/320 and lets PTP multicast
 * through the MAC filter. Call once after MX_LWIP_Init(). */
void Ptp_Init(void);

/* Housekeeping (master timeout, periodic serial status). Main loop. */
void Ptp_Poll(void);

void Ptp_GetStatus(ptp_status_t *st);

/* Cheap, interrupt-safe snapshot of status.locked. */
uint8_t Ptp_IsLocked(void);

#ifdef __cplusplus
}
#endif

#endif /* PTP_H */
