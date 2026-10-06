#include "ptp.h"
#include "ptp_clock.h"
#include "main.h"

#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

extern ETH_HandleTypeDef heth;

#define PTP_EVENT_PORT   319
#define PTP_GENERAL_PORT 320
#define PTP_DOMAIN       0

#define MSG_SYNC       0x0
#define MSG_DELAY_REQ  0x1
#define MSG_FOLLOW_UP  0x8
#define MSG_DELAY_RESP 0x9
#define MSG_ANNOUNCE   0xB

#define HDR_LEN             34
#define SYNC_LEN            44 /* also Delay_Req, Follow_Up */
#define DELAY_RESP_LEN      54
#define ANNOUNCE_LEN        64
#define FLAG_TWO_STEP       0x0200 /* flagField octet 0, bit 1 */
#define FLAG_UTC_OFF_VALID  0x0004 /* flagField octet 1, bit 2 */

/* Beyond this the servo gives up slewing and re-steps the clock. */
#define STEP_THRESHOLD_NS   1000000LL
/* An Rx timestamp older than this when its Sync reaches us is stale (it
 * belongs to an earlier frame, or predates a clock step). */
#define MAX_RX_AGE_NS       50000000LL
#define MAX_PATH_DELAY_NS   1000000LL
#define MASTER_TIMEOUT_MS   5000U
#define DELAY_REQ_TIMEOUT_MS 2000U
#define STATUS_PERIOD_MS    10000U
#define RMS_WINDOW          8

/* Fixed PHY/MAC latencies between the timestamp point and the wire, like
 * ptp4l's ingressLatency/egressLatency. Any asymmetry they leave shows up as
 * a constant offset error that PTP itself cannot see; calibrate against an
 * external reference (e.g. a scope on a trigger input) if it matters. */
#define PTP_RX_LATENCY_NS   0
#define PTP_TX_LATENCY_NS   0

typedef struct
{
  uint8_t type;
  uint16_t length;
  uint8_t domain;
  uint16_t flags;
  int64_t correction_ns;
  uint8_t src[10]; /* sourcePortIdentity: clockIdentity[8] + portNumber[2] */
  uint16_t seq;
  int8_t log_interval;
} hdr_t;

static struct udp_pcb *event_pcb;
static ip_addr_t ptp_mcast;
static uint8_t my_port_id[10];

static ptp_status_t st;
static volatile uint8_t locked_flag;

static uint8_t have_master;
static uint8_t master_port_id[10];
static uint32_t last_sync_ms;
static uint32_t last_status_ms;

/* Two-step Sync waiting for its Follow_Up. */
static uint8_t sync_pending;
static uint16_t sync_seq;
static int64_t sync_t2;
static int64_t sync_corr;

/* t2 - t1 of the newest Sync, for pairing with the next Delay_Req. */
static uint8_t have_sync;
static int64_t last_ms_diff;

/* Delay_Req in flight. */
static uint8_t dreq_pending;
static uint16_t dreq_seq;
static int64_t dreq_t3;
static int64_t dreq_ms_diff;
static uint32_t dreq_sent_ms;
static uint32_t last_dreq_ms;
static uint32_t dreq_interval_ms = 1000U;
static uint8_t have_delay;

/* PI servo, modelled on linuxptp's pi.c. */
static int servo_state;
static int64_t servo_off0;
static int64_t servo_local0;
static double servo_integ; /* ppb */

static int64_t rms_buf[RMS_WINDOW];
static uint32_t rms_idx, rms_count;

/* ---- wire format ------------------------------------------------------- */

static uint16_t rd16(const uint8_t *b) { return (uint16_t)((b[0] << 8) | b[1]); }
static uint32_t rd32(const uint8_t *b)
{
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
static void wr16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }
static void wr32(uint8_t *b, uint32_t v)
{
  b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v;
}

/* PTP Timestamp: 48-bit seconds, 32-bit nanoseconds. */
static int64_t rd_ts(const uint8_t *b)
{
  uint64_t sec = ((uint64_t)rd16(b) << 32) | rd32(b + 2);
  return (int64_t)sec * PTP_NS_PER_SEC + (int64_t)rd32(b + 6);
}

static void wr_ts(uint8_t *b, int64_t t)
{
  uint64_t sec = (uint64_t)(t / PTP_NS_PER_SEC);
  wr16(b, (uint16_t)(sec >> 32));
  wr32(b + 2, (uint32_t)sec);
  wr32(b + 6, (uint32_t)(t % PTP_NS_PER_SEC));
}

static int parse_header(const uint8_t *b, uint16_t len, hdr_t *h)
{
  if (len < HDR_LEN || (b[1] & 0x0F) != 2) /* low nibble: versionPTP; ptp4l may send 0x12 (v2.1) */
  {
    return 0;
  }
  h->type = b[0] & 0x0F;
  h->length = rd16(b + 2);
  h->domain = b[4];
  h->flags = rd16(b + 6);
  /* correctionField: ns * 2^16, signed; drop the sub-ns part. */
  uint64_t corr = ((uint64_t)rd32(b + 8) << 32) | rd32(b + 12);
  h->correction_ns = (int64_t)corr >> 16;
  memcpy(h->src, b + 20, 10);
  h->seq = rd16(b + 30);
  h->log_interval = (int8_t)b[33];
  return h->length <= len;
}

/* ---- helpers ------------------------------------------------------------ */

/* newlib-nano's printf has no %lld, so anything printed goes through long. */
static long clamp_long(int64_t v)
{
  if (v > 2147483647LL) return 2147483647L;
  if (v < -2147483647LL) return -2147483647L;
  return (long)v;
}

static void print_clock_id(const char *what, const uint8_t *id)
{
  printf("[ptp] %s %02x%02x%02x.%02x%02x.%02x%02x%02x port %u\r\n", what,
         id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], rd16(id + 8));
}

static void set_locked(uint8_t locked)
{
  if (locked != st.locked)
  {
    printf("[ptp] %s (offset rms %lu ns)\r\n", locked ? "locked" : "lost lock",
           (unsigned long)st.offset_rms_ns);
  }
  st.locked = locked;
  locked_flag = locked;
}

/* Everything measured against the old timescale is meaningless after a step. */
static void forget_measurements(void)
{
  sync_pending = 0;
  have_sync = 0;
  dreq_pending = 0;
  rms_idx = 0;
  rms_count = 0;
}

static void drop_master(void)
{
  have_master = 0;
  st.state = PTP_STATE_LISTENING;
  servo_state = 0;
  forget_measurements();
  set_locked(0);
}

/* First master heard wins; there is no BMCA, this is meant for one master on
 * the wire. Returns whether src is the selected master. */
static int from_master(const uint8_t *src)
{
  if (!have_master)
  {
    memcpy(master_port_id, src, sizeof(master_port_id));
    memcpy(st.master_id, src, sizeof(st.master_id));
    have_master = 1;
    have_delay = 0;
    last_sync_ms = HAL_GetTick();
    st.state = PTP_STATE_UNCALIBRATED;
    print_clock_id("master", src);
  }
  return memcmp(src, master_port_id, sizeof(master_port_id)) == 0;
}

/* ---- servo -------------------------------------------------------------- */

static void update_rms(int64_t offset)
{
  rms_buf[rms_idx] = offset;
  rms_idx = (rms_idx + 1U) % RMS_WINDOW;
  if (rms_count < RMS_WINDOW)
  {
    rms_count++;
  }
  double sum = 0.0;
  for (uint32_t i = 0; i < rms_count; i++)
  {
    sum += (double)rms_buf[i] * (double)rms_buf[i];
  }
  st.offset_rms_ns = (uint32_t)fmin(sqrt(sum / rms_count), 4294967295.0);
}

static void servo_sample(int64_t offset, int64_t local_ts)
{
  switch (servo_state)
  {
  case 0:
    /* First sample: just remember it, so the next one gives a drift rate. */
    servo_off0 = offset;
    servo_local0 = local_ts;
    servo_state = 1;
    st.state = PTP_STATE_UNCALIBRATED;
    return;

  case 1:
  {
    int64_t dt = local_ts - servo_local0;
    if (dt <= 0)
    {
      servo_state = 0;
      return;
    }
    /* Offset growth per unit time is how much faster we run than the master
     * at the current correction; start the integrator there and jump onto
     * master time in one go. */
    double excess_ppb = (double)(offset - servo_off0) * 1e9 / (double)dt;
    servo_integ = PtpClock_GetFreq() - excess_ppb;
    PtpClock_SetFreq(servo_integ);
    servo_integ = PtpClock_GetFreq(); /* in case it clamped */
    PtpClock_Step(-offset);
    forget_measurements();
    servo_state = 2;
    st.state = PTP_STATE_SLAVE;
    printf("[ptp] clock stepped by %s%lu.%09lu s, frequency %+ld ppb\r\n",
           offset > 0 ? "-" : "+",
           (unsigned long)((offset < 0 ? -offset : offset) / PTP_NS_PER_SEC),
           (unsigned long)((offset < 0 ? -offset : offset) % PTP_NS_PER_SEC),
           (long)servo_integ);
    return;
  }

  default:
    break;
  }

  if (offset > STEP_THRESHOLD_NS || offset < -STEP_THRESHOLD_NS)
  {
    printf("[ptp] offset %ld ns beyond step threshold, re-acquiring\r\n", clamp_long(offset));
    servo_state = 0;
    st.state = PTP_STATE_UNCALIBRATED;
    set_locked(0);
    return;
  }

  /* Per-Sync loop gains of 0.7 (P) and 0.3 (I), i.e. linuxptp's kp/ki_norm_max
   * limits. linuxptp's defaults lower the gain at fast Sync rates to average
   * out timestamp noise, but our timestamps are good to a few ns while the
   * HSI wanders by tens of ppm within a second -- tracking speed is what
   * matters, so this settles within one or two Syncs. */
  double t = ldexp(1.0, st.log_sync_interval);
  double kp = 0.7 / t;
  double ki = 0.3 / t;

  double ki_term = ki * (double)offset;
  double ppb = servo_integ - kp * (double)offset - ki_term;
  if (ppb > -PTP_CLOCK_MAX_PPB && ppb < PTP_CLOCK_MAX_PPB)
  {
    servo_integ -= ki_term; /* no integrator wind-up while saturated */
  }
  PtpClock_SetFreq(ppb);

  update_rms(offset);
  set_locked(rms_count >= RMS_WINDOW / 2 && st.offset_rms_ns < PTP_LOCK_THRESHOLD_NS);
}

/* ---- protocol ----------------------------------------------------------- */

static void send_delay_req(void)
{
  struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, SYNC_LEN, PBUF_RAM);
  if (p == NULL)
  {
    return;
  }
  uint8_t *b = (uint8_t *)p->payload; /* PBUF_RAM: one contiguous buffer */
  memset(b, 0, SYNC_LEN);
  b[0] = MSG_DELAY_REQ;
  b[1] = 2;
  wr16(b + 2, SYNC_LEN);
  b[4] = PTP_DOMAIN;
  memcpy(b + 20, my_port_id, sizeof(my_port_id));
  wr16(b + 30, ++dreq_seq);
  b[32] = 1;    /* controlField: Delay_Req */
  b[33] = 0x7F; /* logMessageInterval: n/a */
  wr_ts(b + 34, PtpClock_Now()); /* originTimestamp is informational only */

  /* One pbuf with header room (PBUF_TRANSPORT) stays one Ethernet frame in
   * one DMA descriptor, which PtpClock_TakeTxTimestamp relies on. */
  PtpClock_ArmTxTimestamp();
  err_t err = udp_sendto(event_pcb, p, &ptp_mcast, PTP_EVENT_PORT);
  int64_t t3;
  int stamped = PtpClock_TakeTxTimestamp(&t3);
  pbuf_free(p);

  last_dreq_ms = HAL_GetTick();
  if (err == ERR_OK && stamped)
  {
    dreq_pending = 1;
    dreq_t3 = t3 + PTP_TX_LATENCY_NS;
    dreq_ms_diff = last_ms_diff;
    dreq_sent_ms = last_dreq_ms;
  }
}

static void maybe_send_delay_req(void)
{
  uint32_t now = HAL_GetTick();
  if (!have_sync)
  {
    return;
  }
  if (dreq_pending && (now - dreq_sent_ms) < DELAY_REQ_TIMEOUT_MS)
  {
    return;
  }
  /* Until the first measurement, ask after every Sync to converge fast. */
  if (have_delay && (now - last_dreq_ms) < dreq_interval_ms)
  {
    return;
  }
  send_delay_req();
}

static void process_sample(int64_t t1, int64_t t2)
{
  int64_t ms_diff = t2 - t1;
  int64_t offset = ms_diff - (have_delay ? st.path_delay_ns : 0);

  st.sync_count++;
  st.offset_ns = offset;
  have_sync = 1;
  last_ms_diff = ms_diff;

  servo_sample(offset, t2); /* may step the clock and forget have_sync */
  maybe_send_delay_req();
}

static void handle_sync(const hdr_t *h, const uint8_t *b, int64_t t2)
{
  if (!from_master(h->src) || h->length < SYNC_LEN)
  {
    return;
  }
  last_sync_ms = HAL_GetTick();
  st.log_sync_interval = h->log_interval;

  if (h->flags & FLAG_TWO_STEP)
  {
    sync_pending = 1;
    sync_seq = h->seq;
    sync_t2 = t2;
    sync_corr = h->correction_ns;
  }
  else
  {
    process_sample(rd_ts(b + HDR_LEN) + h->correction_ns, t2);
  }
}

static void handle_follow_up(const hdr_t *h, const uint8_t *b)
{
  if (!have_master || !from_master(h->src) || h->length < SYNC_LEN ||
      !sync_pending || h->seq != sync_seq)
  {
    return;
  }
  sync_pending = 0;
  process_sample(rd_ts(b + HDR_LEN) + sync_corr + h->correction_ns, sync_t2);
}

static void handle_delay_resp(const hdr_t *h, const uint8_t *b)
{
  if (!have_master || !from_master(h->src) || h->length < DELAY_RESP_LEN ||
      !dreq_pending || h->seq != dreq_seq ||
      memcmp(b + 44, my_port_id, sizeof(my_port_id)) != 0)
  {
    return;
  }
  dreq_pending = 0;

  int64_t t4 = rd_ts(b + HDR_LEN) - h->correction_ns;
  /* master->slave = d + o, slave->master = d - o */
  int64_t delay = (dreq_ms_diff + (t4 - dreq_t3)) / 2;
  if (delay > MAX_PATH_DELAY_NS || delay < -MAX_PATH_DELAY_NS)
  {
    return;
  }
  if (!have_delay)
  {
    st.path_delay_ns = delay;
    have_delay = 1;
  }
  else
  {
    st.path_delay_ns += (delay - st.path_delay_ns) / 8;
  }
  st.delay_count++;

  /* The master dictates the Delay_Req rate (logMinDelayReqInterval). */
  int8_t l = h->log_interval;
  if (l >= -7 && l <= 6)
  {
    dreq_interval_ms = l >= 0 ? (1000U << l) : (1000U >> -l);
  }
}

static void handle_announce(const hdr_t *h, const uint8_t *b)
{
  if (!from_master(h->src) || h->length < ANNOUNCE_LEN)
  {
    return;
  }
  st.utc_offset = (int16_t)rd16(b + 44);
  st.utc_offset_valid = (h->flags & FLAG_UTC_OFF_VALID) != 0;
}

static void event_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                       const ip_addr_t *addr, u16_t port)
{
  LWIP_UNUSED_ARG(arg);
  LWIP_UNUSED_ARG(pcb);
  LWIP_UNUSED_ARG(addr);
  LWIP_UNUSED_ARG(port);

  /* Always take it, even for frames we then ignore, so it can't leak onto a
   * later Sync whose own timestamp went missing. */
  int64_t t2;
  int have_ts = PtpClock_TakeRxTimestamp(&t2);

  uint8_t b[64];
  uint16_t len = pbuf_copy_partial(p, b, sizeof(b), 0);
  pbuf_free(p);

  hdr_t h;
  if (!parse_header(b, len, &h) || h.domain != PTP_DOMAIN || h.type != MSG_SYNC || !have_ts)
  {
    return;
  }
  int64_t age = PtpClock_Now() - t2;
  if (age < 0 || age > MAX_RX_AGE_NS)
  {
    return;
  }
  handle_sync(&h, b, t2 - PTP_RX_LATENCY_NS);
}

static void general_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr, u16_t port)
{
  LWIP_UNUSED_ARG(arg);
  LWIP_UNUSED_ARG(pcb);
  LWIP_UNUSED_ARG(addr);
  LWIP_UNUSED_ARG(port);

  uint8_t b[64];
  uint16_t len = pbuf_copy_partial(p, b, sizeof(b), 0);
  pbuf_free(p);

  hdr_t h;
  if (!parse_header(b, len, &h) || h.domain != PTP_DOMAIN)
  {
    return;
  }
  switch (h.type)
  {
  case MSG_FOLLOW_UP:  handle_follow_up(&h, b); break;
  case MSG_DELAY_RESP: handle_delay_resp(&h, b); break;
  case MSG_ANNOUNCE:   handle_announce(&h, b); break;
  default: break;
  }
}

/* ---- public ------------------------------------------------------------- */

/* Boot-time sanity check that the timestamp unit really ticks at the rate
 * PtpClock_Init() assumed (HCLK): compare it against the CPU cycle counter
 * over 10 ms. Anything but ~0 ppm means the reference clock is wrong. */
static void check_ref_clock(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  uint32_t c0 = DWT->CYCCNT;
  int64_t p0 = PtpClock_Now();
  while ((DWT->CYCCNT - c0) < SystemCoreClock / 100U)
  {
  }
  uint32_t c1 = DWT->CYCCNT;
  int64_t p1 = PtpClock_Now();

  double cpu_ns = (double)(c1 - c0) * 1e9 / (double)SystemCoreClock;
  long ppm = (long)(((double)(p1 - p0) / cpu_ns - 1.0) * 1e6);
  printf("[ptp] hw clock %lu ns resolution, rate vs CPU %+ld ppm\r\n",
         (unsigned long)PtpClock_ResolutionNs(), ppm);
}

void Ptp_Init(void)
{
  PtpClock_Init();
  check_ref_clock();

  /* PTP's 224.0.1.129 maps to 01:00:5e:00:01:81, which the MAC's default
   * perfect filter drops. LWIP_IGMP is off, so nothing else programs a hash
   * filter for it; pass all multicast instead. */
  ETH_MACFilterConfigTypeDef filter;
  HAL_ETH_GetMACFilterConfig(&heth, &filter);
  filter.PassAllMulticast = ENABLE;
  HAL_ETH_SetMACFilterConfig(&heth, &filter);

  /* clockIdentity = EUI-64 from the MAC address. */
  const uint8_t *mac = netif_default->hwaddr;
  uint8_t id[10] = {mac[0], mac[1], mac[2], 0xFF, 0xFE, mac[3], mac[4], mac[5], 0, 1};
  memcpy(my_port_id, id, sizeof(my_port_id));

  IP_ADDR4(&ptp_mcast, 224, 0, 1, 129);

  event_pcb = udp_new();
  struct udp_pcb *general_pcb = udp_new();
  if (event_pcb == NULL || general_pcb == NULL ||
      udp_bind(event_pcb, IP_ADDR_ANY, PTP_EVENT_PORT) != ERR_OK ||
      udp_bind(general_pcb, IP_ADDR_ANY, PTP_GENERAL_PORT) != ERR_OK)
  {
    printf("[ptp] failed to open udp/319,320\r\n");
    return;
  }
  udp_recv(event_pcb, event_recv, NULL);
  udp_recv(general_pcb, general_recv, NULL);

  st.state = PTP_STATE_LISTENING;
  print_clock_id("slave, clock id", my_port_id);
}

void Ptp_Poll(void)
{
  uint32_t now = HAL_GetTick();

  if (have_master && (now - last_sync_ms) > MASTER_TIMEOUT_MS)
  {
    printf("[ptp] master silent for %lu ms, holding frequency at %+ld ppb\r\n",
           (unsigned long)(now - last_sync_ms), (long)PtpClock_GetFreq());
    drop_master();
  }

  if (st.state == PTP_STATE_SLAVE && (now - last_status_ms) >= STATUS_PERIOD_MS)
  {
    last_status_ms = now;
    printf("[ptp] offset %+ld ns, rms %lu ns, path delay %ld ns, freq %+ld ppb\r\n",
           clamp_long(st.offset_ns), (unsigned long)st.offset_rms_ns,
           clamp_long(st.path_delay_ns), (long)PtpClock_GetFreq());
  }
}

void Ptp_GetStatus(ptp_status_t *out)
{
  *out = st;
  out->freq_ppb = (int32_t)PtpClock_GetFreq();
}

uint8_t Ptp_IsLocked(void)
{
  return locked_flag;
}

uint32_t Ptp_OffsetRms(void)
{
  return st.offset_rms_ns; /* one aligned 32-bit read */
}
