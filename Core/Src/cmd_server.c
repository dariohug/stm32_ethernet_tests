#include "cmd_server.h"
#include "main.h"
#include "ptp.h"
#include "ptp_clock.h"
#include "trig_events.h"
#include "lwip/tcp.h"
#include <string.h>
#include <stdio.h>

#define CMD_PORT 5000
#define CMD_LINE_MAX 32

typedef struct
{
  GPIO_TypeDef *port;
  uint16_t pin;
  uint8_t state; /* 1 = on, mirrors the pin so "status" doesn't need to read it back */
} led_t;

static led_t leds[3] = {
    {LED_GREEN_GPIO_Port, LED_GREEN_Pin, 0},
    {LED_YELLOW_GPIO_Port, LED_YELLOW_Pin, 0},
    {LED_RED_GPIO_Port, LED_RED_Pin, 0},
};

/* Per-connection line buffer. One malloc per accepted connection, freed on
 * close/error; the buffer itself is fixed-size, no allocation in the parser. */
typedef struct
{
  char buf[CMD_LINE_MAX];
  uint16_t len;
  uint8_t overflow;
} conn_t;

static void set_led(int idx, const char *action)
{
  uint8_t state = leds[idx].state;
  if (strcmp(action, "on") == 0)
  {
    state = 1;
  }
  else if (strcmp(action, "off") == 0)
  {
    state = 0;
  }
  else if (strcmp(action, "toggle") == 0)
  {
    state = !state;
  }
  else
  {
    return;
  }
  leds[idx].state = state;
  HAL_GPIO_WritePin(leds[idx].port, leds[idx].pin, state ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* The one connection that gets trigger events pushed as they happen. */
static struct tcp_pcb *sub_pcb;
static conn_t *sub_cs;

/* Queues a reply without flushing. Returns 0 (and writes nothing) if it
 * doesn't fit in the send buffer right now. */
static int queue_reply(struct tcp_pcb *pcb, const char *reply)
{
  u16_t len = (u16_t)strlen(reply);
  if (tcp_sndbuf(pcb) < len || tcp_sndqueuelen(pcb) >= TCP_SND_QUEUELEN - 1)
  {
    return 0;
  }
  return tcp_write(pcb, reply, len, TCP_WRITE_FLAG_COPY) == ERR_OK;
}

static void send_reply(struct tcp_pcb *pcb, const char *reply)
{
  queue_reply(pcb, reply); /* client isn't reading if it fails; drop rather than block */
  tcp_output(pcb);
}

static const char *state_name(const ptp_status_t *st)
{
  switch (st->state)
  {
  case PTP_STATE_SLAVE: return st->locked ? "locked" : "slave";
  case PTP_STATE_UNCALIBRATED: return "uncalibrated";
  default: return "listening";
  }
}

/* "<sec>.<nsec>" -- by hand, since newlib-nano's printf has no %lld. */
static void fmt_time(char *out, size_t n, int64_t t)
{
  snprintf(out, n, "%lu.%09lu", (unsigned long)(t / PTP_NS_PER_SEC),
           (unsigned long)(t % PTP_NS_PER_SEC));
}

static long clamp_long(int64_t v)
{
  if (v > 2147483647LL) return 2147483647L;
  if (v < -2147483647LL) return -2147483647L;
  return (long)v;
}

static void format_event(char *out, size_t n, const trig_event_t *ev)
{
  char t[24];
  fmt_time(t, sizeof(t), ev->time_ns);
  snprintf(out, n, "ev %lu %s %s %s %lu\r\n", (unsigned long)ev->seq,
           TrigEvents_ChannelName(ev->channel), t, ev->locked ? "locked" : "unlocked",
           (unsigned long)ev->rms_ns);
}

/* Writes queued events while they fit; the rest stay queued for next time. */
static uint32_t drain_events(struct tcp_pcb *pcb)
{
  uint32_t sent = 0;
  trig_event_t ev;
  char line[64];
  /* Check for room before popping, so an event is never dequeued and then
   * dropped; the margin also leaves room for the events command's "end". */
  while (tcp_sndbuf(pcb) >= 2 * sizeof(line) &&
         tcp_sndqueuelen(pcb) < TCP_SND_QUEUELEN - 2 && TrigEvents_Pop(&ev))
  {
    format_event(line, sizeof(line), &ev);
    queue_reply(pcb, line);
    sent++;
  }
  return sent;
}

/* Reply contract: "ok" on success, "err <reason>" on a bad command, one
 * line of text for status/help/time/ptp; "events" is zero or more "ev" lines
 * followed by "end <n>". Always CRLF-terminated. A subscribed connection also
 * receives "ev" lines unprompted, interleaved with replies. */
static void handle_line(struct tcp_pcb *pcb, conn_t *cs, char *line)
{
  char *cmd = line;
  char *arg = strchr(line, ' ');
  if (arg != NULL)
  {
    *arg = '\0';
    arg++;
  }

  if (strcmp(cmd, "help") == 0)
  {
    send_reply(pcb, "commands: led1|led2|led3 on|off|toggle, status, time, ptp, "
                    "events, subscribe, unsubscribe, help\r\n");
    return;
  }

  if (strcmp(cmd, "time") == 0)
  {
    /* Read the clock first so formatting doesn't add to the latency. */
    int64_t now = PtpClock_Now();
    ptp_status_t st;
    Ptp_GetStatus(&st);
    char t[24], reply[80], utc[8];
    fmt_time(t, sizeof(t), now);
    if (st.utc_offset_valid)
    {
      snprintf(utc, sizeof(utc), "%d", st.utc_offset);
    }
    else
    {
      snprintf(utc, sizeof(utc), "?");
    }
    snprintf(reply, sizeof(reply), "time %s tai utc_offset=%s %s\r\n", t, utc, state_name(&st));
    send_reply(pcb, reply);
    return;
  }

  if (strcmp(cmd, "uptime") == 0)
  {
    /* Milliseconds since boot from SysTick, i.e. the raw (undisciplined)
     * oscillator -- for measuring it against a host clock. */
    char reply[32];
    snprintf(reply, sizeof(reply), "uptime %lu\r\n", (unsigned long)HAL_GetTick());
    send_reply(pcb, reply);
    return;
  }

  if (strcmp(cmd, "ptp") == 0)
  {
    ptp_status_t st;
    Ptp_GetStatus(&st);
    char reply[200];
    const uint8_t *m = st.master_id;
    snprintf(reply, sizeof(reply),
             "ptp state=%s offset=%ld rms=%lu delay=%ld freq=%ld syncs=%lu delays=%lu "
             "log_sync=%d utc_offset=%d%s master=%02x%02x%02x.%02x%02x.%02x%02x%02x "
             "dropped_events=%lu\r\n",
             state_name(&st), clamp_long(st.offset_ns), (unsigned long)st.offset_rms_ns,
             clamp_long(st.path_delay_ns), (long)st.freq_ppb, (unsigned long)st.sync_count,
             (unsigned long)st.delay_count, st.log_sync_interval, st.utc_offset,
             st.utc_offset_valid ? "" : "(unset)",
             m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
             (unsigned long)TrigEvents_Dropped());
    send_reply(pcb, reply);
    return;
  }

  if (strcmp(cmd, "events") == 0)
  {
    if (sub_pcb != NULL)
    {
      send_reply(pcb, "err events are being pushed to a subscriber\r\n");
      return;
    }
    char end[24];
    snprintf(end, sizeof(end), "end %lu\r\n", (unsigned long)drain_events(pcb));
    send_reply(pcb, end);
    return;
  }

  if (strcmp(cmd, "subscribe") == 0)
  {
    sub_pcb = pcb;
    sub_cs = cs;
    send_reply(pcb, "ok\r\n");
    return;
  }

  if (strcmp(cmd, "unsubscribe") == 0)
  {
    if (sub_cs == cs)
    {
      sub_pcb = NULL;
      sub_cs = NULL;
    }
    send_reply(pcb, "ok\r\n");
    return;
  }

  if (strcmp(cmd, "status") == 0)
  {
    char reply[48];
    snprintf(reply, sizeof(reply), "led1=%s led2=%s led3=%s\r\n",
             leds[0].state ? "on" : "off",
             leds[1].state ? "on" : "off",
             leds[2].state ? "on" : "off");
    send_reply(pcb, reply);
    return;
  }

  int idx = -1;
  if (strcmp(cmd, "led1") == 0) idx = 0;
  else if (strcmp(cmd, "led2") == 0) idx = 1;
  else if (strcmp(cmd, "led3") == 0) idx = 2;

  if (idx < 0)
  {
    send_reply(pcb, "err unknown command\r\n");
    return;
  }
  if (arg == NULL || (strcmp(arg, "on") != 0 && strcmp(arg, "off") != 0 && strcmp(arg, "toggle") != 0))
  {
    send_reply(pcb, "err bad arg\r\n");
    return;
  }

  set_led(idx, arg);
  send_reply(pcb, "ok\r\n");
}

static void feed_byte(struct tcp_pcb *pcb, conn_t *cs, uint8_t byte)
{
  if (byte == '\n')
  {
    if (cs->overflow)
    {
      send_reply(pcb, "err line too long\r\n");
      cs->overflow = 0;
    }
    else
    {
      if (cs->len > 0 && cs->buf[cs->len - 1] == '\r')
      {
        cs->len--;
      }
      cs->buf[cs->len] = '\0';
      if (cs->len > 0)
      {
        handle_line(pcb, cs, cs->buf);
      }
    }
    cs->len = 0;
    return;
  }

  if (cs->len < sizeof(cs->buf) - 1)
  {
    cs->buf[cs->len++] = (char)byte;
  }
  else
  {
    cs->overflow = 1;
  }
}

static void conn_close(struct tcp_pcb *pcb, conn_t *cs)
{
  if (sub_cs == cs)
  {
    sub_pcb = NULL;
    sub_cs = NULL;
  }
  tcp_arg(pcb, NULL);
  tcp_recv(pcb, NULL);
  tcp_err(pcb, NULL);
  mem_free(cs);
  tcp_close(pcb);
}

static err_t recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
  conn_t *cs = (conn_t *)arg;

  if (err != ERR_OK)
  {
    if (p != NULL)
    {
      pbuf_free(p);
    }
    return err;
  }

  if (p == NULL)
  {
    conn_close(pcb, cs);
    return ERR_OK;
  }

  tcp_recved(pcb, p->tot_len);
  for (struct pbuf *q = p; q != NULL; q = q->next)
  {
    const uint8_t *data = (const uint8_t *)q->payload;
    for (u16_t i = 0; i < q->len; i++)
    {
      feed_byte(pcb, cs, data[i]);
    }
  }
  pbuf_free(p);
  return ERR_OK;
}

static void err_cb(void *arg, err_t err)
{
  LWIP_UNUSED_ARG(err);
  if (arg != NULL && arg == sub_cs)
  {
    sub_pcb = NULL; /* LwIP has already freed the pcb */
    sub_cs = NULL;
  }
  if (arg != NULL)
  {
    mem_free(arg);
  }
}

static err_t accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
  LWIP_UNUSED_ARG(arg);
  if (err != ERR_OK || newpcb == NULL)
  {
    return ERR_VAL;
  }

  conn_t *cs = (conn_t *)mem_malloc(sizeof(conn_t));
  if (cs == NULL)
  {
    return ERR_MEM;
  }
  cs->len = 0;
  cs->overflow = 0;

  tcp_arg(newpcb, cs);
  tcp_recv(newpcb, recv_cb);
  tcp_err(newpcb, err_cb);
  return ERR_OK;
}

void CmdServer_Init(void)
{
  struct tcp_pcb *pcb = tcp_new();
  if (pcb == NULL)
  {
    return;
  }
  if (tcp_bind(pcb, IP_ADDR_ANY, CMD_PORT) != ERR_OK)
  {
    return;
  }
  pcb = tcp_listen(pcb);
  if (pcb == NULL)
  {
    return;
  }
  tcp_accept(pcb, accept_cb);
}

void CmdServer_Poll(void)
{
  if (sub_pcb != NULL && drain_events(sub_pcb) > 0)
  {
    tcp_output(sub_pcb);
  }
}
