#ifndef CMD_SERVER_H
#define CMD_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/* TCP command server, port 5000. Call once after MX_LWIP_Init(). */
void CmdServer_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* CMD_SERVER_H */
