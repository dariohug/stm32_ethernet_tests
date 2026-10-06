#ifndef ETH_LOG_H
#define ETH_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Only forward declarations here: LwIP pulls this header in via
 * LWIP_HOOK_FILENAME from inside its own sources, so including lwip headers
 * back would be circular. */
struct pbuf;
struct netif;

/* Retargets printf() to USART3 (ST-LINK VCP, 115200 8N1) and announces
 * startup. Call once after MX_USART3_UART_Init(). */
void EthLog_Init(void);

/* LwIP LWIP_HOOK_IP4_INPUT hook: observes every inbound IPv4 packet.
 * Always returns 0, i.e. never consumes the packet. */
int EthLog_Ip4Input(struct pbuf *p, struct netif *inp);

/* Emits the periodic traffic summary. Call from the main loop. */
void EthLog_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* ETH_LOG_H */
