#ifndef TRIG_EVENTS_H
#define TRIG_EVENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rising edges on the trigger inputs, timestamped with the PTP hardware
 * clock at interrupt entry and queued for the command server.
 *
 *   channel 0  btn    PC13  blue user button B1
 *   channel 1  trig1  PE9   Zio D6 (CN10), pulled down
 *   channel 2  trig2  PE11  Zio D5 (CN10), pulled down
 *
 * Pins, pulls and edges are owned by the .ioc (TRIG_* labels); this module
 * only maps pins to channel numbers. */

#define TRIG_QUEUE_LEN 64U /* power of two */

typedef struct
{
  uint32_t seq;     /* global, increments per captured edge; gaps = drops */
  uint8_t channel;
  uint8_t locked;   /* PTP was locked when the edge was captured */
  uint32_t rms_ns;  /* PTP's recent RMS offset then: the timestamp's uncertainty */
  int64_t time_ns;  /* PTP time (TAI once locked) */
} trig_event_t;

/* Call first thing in each EXTI IRQ handler that serves a trigger pin
 * (stm32h7xx_it.c USER CODE ...IRQn 0): latches the clock before the HAL's
 * dispatch, so the timestamp doesn't include it. */
void TrigEvents_IrqEntry(void);

/* Pops the oldest queued event. Returns 0 if the queue is empty. Main loop
 * only (single consumer). */
int TrigEvents_Pop(trig_event_t *ev);

const char *TrigEvents_ChannelName(uint8_t channel);

/* Events lost because the queue was full. */
uint32_t TrigEvents_Dropped(void);

/* Rate-limited serial echo of captured edges. Main loop. */
void TrigEvents_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* TRIG_EVENTS_H */
