#include "trig_events.h"
#include "ptp.h"
#include "ptp_clock.h"
#include "main.h"

#include <stdio.h>

#define TRIG_PRINT_MIN_GAP_MS 200U

/* Edge-to-clock-read latency of the interrupt path, subtracted from every
 * timestamp. Measured on this board (550 MHz, I/D-cache on) by reading the
 * clock, setting the line's EXTI software-trigger bit and comparing with the
 * captured time: 240-285 ns, median 260 ns, over 200 runs. A real pin adds
 * its input synchroniser, a few ns more. Recalibrate if the clock tree,
 * cache setup or handler code changes. */
#define TRIG_IRQ_LATENCY_NS 250

typedef struct
{
  uint16_t pin;
  const char *name;
} channel_t;

/* EXTI lines are per pin number, so the pin alone identifies the channel. */
static const channel_t channels[] = {
    {TRIG_BTN_Pin, "btn"},
    {TRIG1_Pin, "trig1"},
    {TRIG2_Pin, "trig2"},
};
#define NUM_CHANNELS (sizeof(channels) / sizeof(channels[0]))

/* Single producer (the trigger EXTI handlers, which share priority 0 and so
 * never preempt each other), single consumer (main loop). */
static trig_event_t queue[TRIG_QUEUE_LEN];
static volatile uint32_t q_head, q_tail;
static volatile uint32_t dropped;
static volatile uint32_t next_seq;

/* Per-channel capture time, latched by whichever trigger handler ran first
 * while that channel's line was pending; valid while its bit is in
 * stamped_mask. */
static int64_t stamp[NUM_CHANNELS];
static uint32_t stamped_mask;

/* Latest capture, for the serial echo (which must not consume the queue). */
static volatile trig_event_t last_event;
static volatile uint32_t captured;
static uint32_t echoed;
static uint32_t last_echo_ms;

/* Trigger pins sit on different EXTI vectors (EXTI9_5, EXTI15_10). Edges
 * that arrive together would otherwise be stamped one handler apart (~0.65 us
 * measured), so the first handler to run stamps every trigger line that is
 * already pending, and later handlers reuse that stamp. All trigger handlers
 * share priority 0, so none of this is ever preempted by another of them. */
void TrigEvents_IrqEntry(void)
{
  int64_t now = PtpClock_Now() - TRIG_IRQ_LATENCY_NS;
  uint32_t pending = EXTI->PR1;
  for (uint32_t ch = 0; ch < NUM_CHANNELS; ch++)
  {
    uint32_t bit = 1UL << ch;
    if ((pending & channels[ch].pin) != 0U && (stamped_mask & bit) == 0U)
    {
      stamp[ch] = now;
      stamped_mask |= bit;
    }
  }
}

/* HAL_GPIO_EXTI_IRQHandler() calls this once per pending trigger pin. */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  uint8_t ch;
  for (ch = 0; ch < NUM_CHANNELS; ch++)
  {
    if (channels[ch].pin == GPIO_Pin)
    {
      break;
    }
  }
  if (ch == NUM_CHANNELS)
  {
    return;
  }

  if ((stamped_mask & (1UL << ch)) == 0U)
  {
    /* Edge arrived after this handler's entry stamp was taken (its line
     * wasn't pending yet); stamp it now rather than reuse a stale time. */
    stamp[ch] = PtpClock_Now() - TRIG_IRQ_LATENCY_NS;
  }

  trig_event_t ev = {
      .seq = next_seq++,
      .channel = ch,
      .locked = Ptp_IsLocked(),
      .rms_ns = Ptp_OffsetRms(),
      .time_ns = stamp[ch],
  };
  stamped_mask &= ~(1UL << ch);
  last_event = ev;
  captured++;

  uint32_t head = q_head;
  if (head - q_tail >= TRIG_QUEUE_LEN)
  {
    dropped++;
    return;
  }
  queue[head % TRIG_QUEUE_LEN] = ev;
  __DMB(); /* entry visible before the index that publishes it */
  q_head = head + 1U;
}

int TrigEvents_Pop(trig_event_t *ev)
{
  uint32_t tail = q_tail;
  if (tail == q_head)
  {
    return 0;
  }
  __DMB();
  *ev = queue[tail % TRIG_QUEUE_LEN];
  __DMB();
  q_tail = tail + 1U;
  return 1;
}

const char *TrigEvents_ChannelName(uint8_t channel)
{
  return channel < NUM_CHANNELS ? channels[channel].name : "?";
}

uint32_t TrigEvents_Dropped(void)
{
  return dropped;
}

void TrigEvents_Poll(void)
{
  /* Same reasoning as eth_log: a UART line blocks for ~5 ms, so a fast
   * trigger train is summarised rather than echoed edge by edge. The queue
   * (and so the command server) still gets every edge. */
  uint32_t now = HAL_GetTick();
  uint32_t n = captured;
  if (n == echoed || (now - last_echo_ms) < TRIG_PRINT_MIN_GAP_MS)
  {
    return;
  }
  last_echo_ms = now;

  __disable_irq();
  trig_event_t ev = last_event;
  __enable_irq();

  uint32_t burst = n - echoed;
  echoed = n;
  printf("[trig] %s #%lu at %lu.%09lu%s", TrigEvents_ChannelName(ev.channel),
         (unsigned long)ev.seq,
         (unsigned long)(ev.time_ns / PTP_NS_PER_SEC),
         (unsigned long)(ev.time_ns % PTP_NS_PER_SEC),
         ev.locked ? "" : " (ptp not locked)");
  if (burst > 1U)
  {
    printf(", %lu edges since last line", (unsigned long)burst);
  }
  printf("\r\n");
}
