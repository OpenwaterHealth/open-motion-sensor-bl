/* Entry Point */
ENTRY(Reset_Handler)

/* Highest address of the SBSFU stack (end of DTCMRAM) */
_estack = 0x20020000;

_Min_Heap_Size  = 0;
_Min_Stack_Size = 0x2000;

INCLUDE mapping_fwimg.ld
INCLUDE mapping_sbsfu.ld

__ICFEDIT_intvec_start__    = 0x08000000;
SE_Binary_region_ROM_Length = __ICFEDIT_SE_Code_region_ROM_end__ - __ICFEDIT_SE_CallGate_region_ROM_start__ + 1;

MEMORY
{
  ISR_VECTOR           (rx) : ORIGIN = __ICFEDIT_intvec_start__,                 LENGTH = VECTOR_SIZE
  SE_Binary_region_ROM (rx) : ORIGIN = __ICFEDIT_SE_CallGate_region_ROM_start__, LENGTH = SE_Binary_region_ROM_Length
}

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >ISR_VECTOR

  .text.SE_CORE_Bin :
  {
    . = ALIGN(4);
    KEEP(*(.SE_CORE_Bin))
    FILL(0xFF);
    . = ALIGN(4);
  } >SE_Binary_region_ROM

  .SE_IF_Code :
  {
    . = ALIGN(4);
    *(.SE_Entry_Secure_Code)
    *(.SE_IF_Code)
    /* The se_interface_*.c wrappers tag their code with the .SE_IF_Code section
     * only for IAR (#pragma default_function_attributes) and Keil; GCC has no
     * equivalent per-file pragma, so under GCC those functions land in plain
     * .text. They MUST live in this region: the SE call gate's IS_CALLER_SE_IF()
     * check issues NVIC_SystemReset() if its return address (inside SE_Init etc.)
     * is outside [SE_IF_region_ROM]. Place them here by object name (wildcard
     * matches both "se_interface_*.o" and CMake's "se_interface_*.c.obj"). */
    *se_interface_bootloader*(.text .text*)
    *se_interface_common*(.text .text*)
    *se_interface_application*(.text .text*)
    FILL(0xFF);
    . = ALIGN(4);
  } >SE_IF_region_ROM

  .SB_HDP_Code :
  {
    . = ALIGN(4);
    *(.SB_HDP_Code)
    FILL(0xFF);
    . = ALIGN(4);
  } >SB_HDP_ROM_region

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >SB_ROM_region

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >SB_ROM_region

  .ARM.extab :
  {
    . = ALIGN(4);
    *(.ARM.extab* .gnu.linkonce.armextab.*)
    . = ALIGN(4);
  } >SB_ROM_region

  .ARM :
  {
    . = ALIGN(4);
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
    . = ALIGN(4);
  } >SB_ROM_region

  .preinit_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
    . = ALIGN(4);
  } >SB_ROM_region

  .init_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
    . = ALIGN(4);
  } >SB_ROM_region

  .fini_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
    . = ALIGN(4);
  } >SB_ROM_region

  _siRamFunc = LOADADDR(.RamFunc);
  .RamFunc :
  {
    . = ALIGN(4);
    _sRamFunc = .;
    *(.RamFunc)
    *(.RamFunc*)
    . = ALIGN(4);
    _eRamFunc = .;
  } >SB_HDP_Code_RAM_region AT>SB_HDP_ROM_region

  _sidata = LOADADDR(.data);
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >SB_RAM_region AT>SB_ROM_region

  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >SB_RAM_region

  /* DMA buffers in RAM_D2 (AXI SRAM) - accessible by DMA */
  .dma_buffer (NOLOAD) : ALIGN(4)
  {
    *(.dma_buffer)
    *(.dma_buffer*)
    . = ALIGN(4);
  } >RAM_D2
  
  ._user_heap_stack (NOLOAD) :
  {
    . = ALIGN(8);
    PROVIDE (end = .);
    PROVIDE (_end = .);
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >SB_RAM_region

  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }
}
