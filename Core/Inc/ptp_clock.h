#ifndef PTP_CLOCK_H
#define PTP_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The ETH MAC's IEEE 1588 system time: a free-running seconds+nanoseconds
 * counter in hardware that stamps PTP frames on the wire and can be read at
 * any moment (e.g. from an EXTI handler). Times are plain int64 nanoseconds on
 * whatever timescale the clock was last stepped to -- TAI once PTP has locked,
 * seconds-since-boot before that. */

#define PTP_NS_PER_SEC 1000000000LL

/* Configure the timestamp unit (fine-update mode, ns rollover, Sync-only Rx
 * snapshots for PTPv2 over UDP/IPv4) and start the clock at 0. Call once after
 * MX_LWIP_Init(), i.e. after HAL_ETH_Init() has clocked the MAC. */
void PtpClock_Init(void);

/* Current time. Safe from any context, including interrupts; two register
 * reads plus a seconds re-read to catch a rollover between them. */
int64_t PtpClock_Now(void);

/* Jump the clock by delta_ns (re-initialises it to now + delta). */
void PtpClock_Step(int64_t delta_ns);

/* Run the clock fast (+) or slow (-) by ppb parts per billion relative to the
 * nominal rate. Absolute, not cumulative. Clamped to +-PTP_CLOCK_MAX_PPB. */
#define PTP_CLOCK_MAX_PPB 20000000.0 /* 2%: covers the HSI's trim tolerance */
void PtpClock_SetFreq(double ppb);
double PtpClock_GetFreq(void);

/* Hardware Rx timestamp of the frame currently being processed by LwIP (we're
 * NO_SYS, so ethernetif_input() hands each frame up synchronously right after
 * HAL_ETH_ReadData() captured its timestamp). Returns 0 if the frame carried
 * none. Consumes the timestamp, so a later frame without one can't silently
 * reuse it. */
int PtpClock_TakeRxTimestamp(int64_t *ts);

/* Tx timestamping for one outgoing frame: Arm right before handing a
 * single-pbuf frame to LwIP, Take right after the send returns (the HAL's
 * transmit is blocking, so the descriptor has been written back by then).
 * Take returns 0 if no timestamp was captured. */
void PtpClock_ArmTxTimestamp(void);
int PtpClock_TakeTxTimestamp(int64_t *ts);

/* Nominal clock resolution, for reporting. */
uint32_t PtpClock_ResolutionNs(void);

#ifdef __cplusplus
}
#endif

#endif /* PTP_CLOCK_H */
