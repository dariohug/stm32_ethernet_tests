/* Places ETH DMA descriptors and the zero-copy Rx pbuf pool in D2 SRAM
 * (RAM_D2, defined by the CubeMX-generated linker script), which is the
 * only RAM the Ethernet DMA can reach. See Core/Inc/lwipopts.h USER CODE
 * for why the LwIP heap pointer is derived from _eth_ram_end instead of
 * CubeMX's fixed 0x30004000: with 12 Rx pool buffers the descriptors and
 * pool no longer fit in the first 16 KB CubeMX assumes.
 */
SECTIONS
{
  .RxDescripSection (NOLOAD) :
  {
    . = ALIGN(4);
    *(.RxDescripSection)
  } >RAM_D2

  .TxDescripSection (NOLOAD) :
  {
    . = ALIGN(4);
    *(.TxDescripSection)
  } >RAM_D2

  .Rx_PoolSection (NOLOAD) :
  {
    . = ALIGN(4);
    *(.Rx_PoolSection)
    . = ALIGN(4);
    _eth_ram_end = .;
  } >RAM_D2
}
