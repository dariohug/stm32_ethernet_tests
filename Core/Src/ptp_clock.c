#include "ptp_clock.h"
#include "main.h"

/* CubeMX's ethernetif.c owns the handle. */
extern ETH_HandleTypeDef heth;

/* This deliberately drives the timestamp registers directly instead of using
 * the HAL's HAL_ETH_PTP_* API (which needs HAL_ETH_USE_PTP, changing
 * ETH_HandleTypeDef's layout for every translation unit). In FW_H7 V1.13.0
 * that API is also wrong in ways that matter here: HAL_ETH_PTP_SetTime uses
 * TSUPDT (add) rather than TSINIT (set), and HAL_ETH_PTP_AddTimeOffset also
 * nudges the addend register by the offset in nanoseconds. */

#define TS_INVALID UINT32_MAX

static uint32_t ssinc_ns;    /* nanoseconds added per accumulator overflow */
static double base_addend;   /* addend giving exactly 1e9 ns per second */
static double cur_ppb;
static int32_t armed_tx_idx = -1;

/* The register bits below self-clear once the PTP clock domain has taken the
 * update, which takes a few PTP clock cycles. Bounded so a dead clock can't
 * hang the firmware. */
static void wait_tscr_clear(uint32_t bits)
{
  for (uint32_t i = 0; i < 100000U && (ETH->MACTSCR & bits) != 0U; i++)
  {
  }
}

void PtpClock_Init(void)
{
  /* The H7's PTP reference clock is HCLK (275 MHz here). In fine-update mode
   * a 32-bit accumulator adds MACTSAR every reference clock cycle and each
   * overflow advances the time by SSINC ns, so the effective update rate is
   * HCLK * addend / 2^32 and must equal 1e9 / SSINC. Picking SSINC so that
   * rate is ~80% of HCLK keeps the addend well below 2^32, leaving room to
   * trim the frequency both ways; it also sets the resolution (5 ns here). */
  uint32_t hclk = HAL_RCC_GetHCLKFreq();
  ssinc_ns = (uint32_t)((1250000000ULL + hclk - 1U) / hclk);
  base_addend = 4294967296.0 * (double)PTP_NS_PER_SEC / ((double)ssinc_ns * (double)hclk);
  cur_ppb = 0.0;

  CLEAR_BIT(ETH->MACIER, ETH_MACIER_TSIE);
  ETH->MACTSCR = ETH_MACTSCR_TSENA;
  ETH->MACSSIR = ssinc_ns << ETH_MACMACSSIR_SSINC_Pos;
  ETH->MACTSAR = (uint32_t)base_addend;
  SET_BIT(ETH->MACTSCR, ETH_MACTSCR_TSADDREG);
  wait_tscr_clear(ETH_MACTSCR_TSADDREG);

  /* Fine update; subseconds count ns (roll over at 1e9); PTPv2 over
   * UDP/IPv4; snapshot event messages only, and with TSMSTRENA clear that
   * means Sync only (slave). Restricting Rx snapshots to Sync is what makes
   * PtpClock_TakeRxTimestamp() trustworthy: nothing else overwrites it. Tx
   * snapshots are per-descriptor (TTSE) and unaffected by these filters. */
  ETH->MACTSCR = ETH_MACTSCR_TSENA | ETH_MACTSCR_TSCFUPDT | ETH_MACTSCR_TSCTRLSSR |
                 ETH_MACTSCR_TSVER2ENA | ETH_MACTSCR_TSIPV4ENA | ETH_MACTSCR_TSEVNTENA;

  ETH->MACSTSUR = 0U;
  ETH->MACSTNUR = 0U;
  SET_BIT(ETH->MACTSCR, ETH_MACTSCR_TSINIT);
  wait_tscr_clear(ETH_MACTSCR_TSINIT);

  heth.RxDescList.TimeStamp.TimeStampHigh = TS_INVALID;
  heth.RxDescList.TimeStamp.TimeStampLow = TS_INVALID;
}

int64_t PtpClock_Now(void)
{
  uint32_t sec, nsec;
  do
  {
    sec = ETH->MACSTSR;
    nsec = ETH->MACSTNR;
  } while (sec != ETH->MACSTSR);
  return (int64_t)sec * PTP_NS_PER_SEC + (int64_t)nsec;
}

void PtpClock_Step(int64_t delta_ns)
{
  /* Re-initialise rather than use the add/subtract update (TSUPDT): it
   * handles arbitrarily large jumps (the first lock moves the clock from ~0
   * to ~1.8e9 s of TAI) with one code path. The read-to-write gap is a few
   * register accesses with interrupts off, i.e. tens of ns, which the servo
   * then trims out like any other offset. */
  wait_tscr_clear(ETH_MACTSCR_TSINIT | ETH_MACTSCR_TSUPDT);

  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  int64_t t = PtpClock_Now() + delta_ns;
  if (t < 0)
  {
    t = 0;
  }
  ETH->MACSTSUR = (uint32_t)(t / PTP_NS_PER_SEC);
  ETH->MACSTNUR = (uint32_t)(t % PTP_NS_PER_SEC);
  SET_BIT(ETH->MACTSCR, ETH_MACTSCR_TSINIT);
  __set_PRIMASK(primask);

  wait_tscr_clear(ETH_MACTSCR_TSINIT);
}

void PtpClock_SetFreq(double ppb)
{
  if (ppb > PTP_CLOCK_MAX_PPB)
  {
    ppb = PTP_CLOCK_MAX_PPB;
  }
  else if (ppb < -PTP_CLOCK_MAX_PPB)
  {
    ppb = -PTP_CLOCK_MAX_PPB;
  }
  cur_ppb = ppb;

  wait_tscr_clear(ETH_MACTSCR_TSADDREG);
  ETH->MACTSAR = (uint32_t)(base_addend * (1.0 + ppb * 1e-9) + 0.5);
  SET_BIT(ETH->MACTSCR, ETH_MACTSCR_TSADDREG);
  wait_tscr_clear(ETH_MACTSCR_TSADDREG);
}

double PtpClock_GetFreq(void)
{
  return cur_ppb;
}

int PtpClock_TakeRxTimestamp(int64_t *ts)
{
  /* HAL_ETH_ReadData() copies the timestamp out of the frame's trailing
   * context descriptor into RxDescList.TimeStamp. */
  ETH_TimeStampTypeDef *rx = &heth.RxDescList.TimeStamp;
  if (rx->TimeStampHigh == TS_INVALID && rx->TimeStampLow == TS_INVALID)
  {
    return 0;
  }
  *ts = (int64_t)rx->TimeStampHigh * PTP_NS_PER_SEC + (int64_t)rx->TimeStampLow;
  rx->TimeStampHigh = TS_INVALID;
  rx->TimeStampLow = TS_INVALID;
  return 1;
}

void PtpClock_ArmTxTimestamp(void)
{
  /* TTSE lives in the frame's first descriptor, which is the next one the
   * HAL will fill. ETH_Prepare_Tx_Descriptors() only MODIFY_REGs other
   * DESC2 fields, so the bit survives until the DMA picks the frame up. */
  uint32_t idx = heth.TxDescList.CurTxDesc;
  ETH_DMADescTypeDef *d = (ETH_DMADescTypeDef *)heth.TxDescList.TxDesc[idx];
  SET_BIT(d->DESC2, ETH_DMATXNDESCRF_TTSE);
  armed_tx_idx = (int32_t)idx;
}

int PtpClock_TakeTxTimestamp(int64_t *ts)
{
  if (armed_tx_idx < 0)
  {
    return 0;
  }
  uint32_t idx = (uint32_t)armed_tx_idx;
  armed_tx_idx = -1;
  ETH_DMADescTypeDef *d = (ETH_DMADescTypeDef *)heth.TxDescList.TxDesc[idx];

  /* Nothing in this project calls HAL_ETH_ReleaseTxPacket(), which is where
   * the HAL would clear TTSE; do it here so the next frame to reuse this
   * descriptor doesn't get stamped too. */
  CLEAR_BIT(d->DESC2, ETH_DMATXNDESCRF_TTSE);

  /* The timestamp is written back into the frame's last descriptor. Our PTP
   * frames are one pbuf, i.e. one descriptor, so first == last; if the HAL
   * advanced by anything other than exactly one, the frame wasn't ours. */
  uint32_t desc3 = d->DESC3;
  if (heth.TxDescList.CurTxDesc != (idx + 1U) % ETH_TX_DESC_CNT ||
      (desc3 & ETH_DMATXNDESCWBF_OWN) != 0U ||
      (desc3 & ETH_DMATXNDESCWBF_LD) == 0U ||
      (desc3 & ETH_DMATXNDESCWBF_TTSS) == 0U)
  {
    return 0;
  }
  *ts = (int64_t)d->DESC1 * PTP_NS_PER_SEC + (int64_t)d->DESC0;
  return 1;
}

uint32_t PtpClock_ResolutionNs(void)
{
  return ssinc_ns;
}
