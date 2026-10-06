#ifndef CMD_SERVER_H
#define CMD_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/* TCP command server, port 5000. Call once after MX_LWIP_Init(). */
void CmdServer_Init(void);

/* Pushes queued trigger events to a subscribed connection. Main loop. */
void CmdServer_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* CMD_SERVER_H */
