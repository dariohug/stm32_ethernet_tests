#include "cmd_server.h"
#include "main.h"
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

static void send_reply(struct tcp_pcb *pcb, const char *reply)
{
  u16_t len = (u16_t)strlen(reply);
  if (tcp_sndbuf(pcb) < len)
  {
    return; /* client isn't reading; drop rather than block */
  }
  tcp_write(pcb, reply, len, TCP_WRITE_FLAG_COPY);
  tcp_output(pcb);
}

/* Reply contract: "ok" on success, "err <reason>" on a bad command, one
 * line of text for status/help. Always CRLF-terminated. */
static void handle_line(struct tcp_pcb *pcb, char *line)
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
    send_reply(pcb, "commands: led1|led2|led3 on|off|toggle, status, help\r\n");
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
        handle_line(pcb, cs->buf);
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
