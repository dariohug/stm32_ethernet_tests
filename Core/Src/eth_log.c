#include "eth_log.h"
#include "main.h"

#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"

#include <stdio.h>

/* CubeMX generates this in main.c for the ST-LINK virtual COM port. */
extern UART_HandleTypeDef huart3;

/* One log line at 115200 baud costs roughly 5 ms of blocking transmit, which
 * dwarfs a main-loop iteration. So per-packet printing is rate limited: pings
 * are printed as they arrive (they're rare and they're what you want to watch),
 * while bulk traffic is only counted and reported once per window. Without
 * this, a throughput test would spend all its time in the UART. */
#define ETH_LOG_PING_MIN_GAP_MS 200U
#define ETH_LOG_SUMMARY_MS      1000U

static uint32_t icmp_pkts, tcp_pkts, udp_pkts, ptp_pkts, other_pkts, rx_bytes;
static uint32_t window_start_ms, last_ping_ms;
static uint8_t have_activity;

/* printf() -> USART3. syscalls.c's _write() calls this per character and
 * declares it weak, so defining it here overrides the stub. */
int __io_putchar(int ch)
{
  uint8_t c = (uint8_t)ch;
  HAL_UART_Transmit(&huart3, &c, 1, 100);
  return ch;
}

void EthLog_Init(void)
{
  /* Unbuffered, so a line reaches the wire when it's printed rather than when
   * newlib's buffer happens to fill. */
  setvbuf(stdout, NULL, _IONBF, 0);
  window_start_ms = HAL_GetTick();
  printf("\r\n[eth] up: 192.168.1.10, command server on tcp/5000\r\n");
}

int EthLog_Ip4Input(struct pbuf *p, struct netif *inp)
{
  LWIP_UNUSED_ARG(inp);

  if (p == NULL || p->len < sizeof(struct ip_hdr))
  {
    return 0; /* runt header; leave it to the stack's own checks */
  }

  const struct ip_hdr *hdr = (const struct ip_hdr *)p->payload;
  ip4_addr_t src;
  ip4_addr_copy(src, hdr->src);

  rx_bytes += p->tot_len;

  /* PTP (udp/319,320) arrives several times a second forever once a master
   * runs; it's counted but doesn't by itself make the link "active", or the
   * summary line would print every second for good. */
  uint16_t hlen = IPH_HL_BYTES(hdr);
  if (IPH_PROTO(hdr) == IP_PROTO_UDP && p->len >= hlen + 4U)
  {
    const uint8_t *udp = (const uint8_t *)p->payload + hlen;
    uint16_t dport = (uint16_t)((udp[2] << 8) | udp[3]);
    if (dport == 319U || dport == 320U)
    {
      ptp_pkts++;
      return 0;
    }
  }
  have_activity = 1;

  switch (IPH_PROTO(hdr))
  {
  case IP_PROTO_ICMP:
  {
    uint32_t now = HAL_GetTick();
    icmp_pkts++;
    if ((uint32_t)(now - last_ping_ms) >= ETH_LOG_PING_MIN_GAP_MS)
    {
      last_ping_ms = now;
      printf("[eth] ping from %s\r\n", ip4addr_ntoa(&src));
    }
    break;
  }
  case IP_PROTO_TCP:
    tcp_pkts++;
    break;
  case IP_PROTO_UDP:
    udp_pkts++;
    break;
  default:
    other_pkts++;
    break;
  }

  return 0; /* observe only, never eat the packet */
}

void EthLog_Poll(void)
{
  uint32_t now = HAL_GetTick();
  uint32_t elapsed = now - window_start_ms;

  if (elapsed < ETH_LOG_SUMMARY_MS)
  {
    return;
  }
  window_start_ms = now;

  if (!have_activity)
  {
    /* Stay silent on an idle link, but restart the window so background PTP
     * doesn't pile up and inflate the next summary. */
    ptp_pkts = rx_bytes = 0;
    return;
  }
  have_activity = 0;

  uint32_t total = icmp_pkts + tcp_pkts + udp_pkts + ptp_pkts + other_pkts;
  printf("[eth] %lu pkt/s (icmp %lu tcp %lu udp %lu ptp %lu other %lu), %lu B/s\r\n",
         (unsigned long)(total * 1000U / elapsed),
         (unsigned long)icmp_pkts, (unsigned long)tcp_pkts,
         (unsigned long)udp_pkts, (unsigned long)ptp_pkts, (unsigned long)other_pkts,
         (unsigned long)(rx_bytes * 1000U / elapsed));

  icmp_pkts = tcp_pkts = udp_pkts = ptp_pkts = other_pkts = rx_bytes = 0;
}
