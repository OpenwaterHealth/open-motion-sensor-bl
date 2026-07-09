/*
*****************************************************************************
**  File        : mapping_sbsfu.ld
**  Abstract    : SBSFU memory region mapping for STM32H7xx
**                Defines SE secure zone and SBSFU code/RAM regions.
*****************************************************************************
*/

VECTOR_SIZE = 0x400;

/* SE Code region protected by MPU isolation */
__ICFEDIT_SE_Code_region_ROM_start__       = 0x08000000 + VECTOR_SIZE;
__ICFEDIT_SE_CallGate_region_ROM_start__   = __ICFEDIT_SE_Code_region_ROM_start__;
__ICFEDIT_SE_CallGate_region_ROM_end__     = __ICFEDIT_SE_Code_region_ROM_start__ + 0x1FF;

/* SE key region protected by MPU isolation / PCROP */
__ICFEDIT_SE_Key_region_ROM_start__        = __ICFEDIT_SE_CallGate_region_ROM_end__ + 0x1;
__ICFEDIT_SE_Key_region_ROM_end__          = __ICFEDIT_SE_Key_region_ROM_start__ + 0x2FF;

/* SE Startup */
__ICFEDIT_SE_Startup_region_ROM_start__    = __ICFEDIT_SE_Key_region_ROM_end__ + 0x1;
__ICFEDIT_SE_Code_nokey_region_ROM_start__ = __ICFEDIT_SE_Startup_region_ROM_start__ + 0x100;
/* SE End aligned to the end of the 1st 32KBytes of flash, MPU isolation constraint */
__ICFEDIT_SE_Code_region_ROM_end__         = 0x08007FFF;

/* SE Interface ROM: code out of MPU isolation zone (accessible from SBSFU) */
__ICFEDIT_SE_IF_region_ROM_start__         = 0x08000000 + 0x8000;
__ICFEDIT_SE_IF_region_ROM_end__           = __ICFEDIT_SE_IF_region_ROM_start__ + 0x8FF;

/* Secure memory activation code (HDP enable) */
__ICFEDIT_SB_HDP_region_ROM_start__        = __ICFEDIT_SE_IF_region_ROM_end__ + 1;
__ICFEDIT_SB_HDP_region_ROM_end__          = __ICFEDIT_SB_HDP_region_ROM_start__ + 0xFF;

/* SBSFU Code region */
__ICFEDIT_SB_region_ROM_start__            = __ICFEDIT_SB_HDP_region_ROM_end__ + 1;
/* SBSFU end aligned to the end of the 1st 128KBytes of FLASH, MPU constraint */
__ICFEDIT_SB_region_ROM_end__              = 0x0801FFFF;

SE_Entry_Secure_ROM_Region_Length = __ICFEDIT_SE_CallGate_region_ROM_end__ - __ICFEDIT_SE_CallGate_region_ROM_start__ + 1;
SE_Key_region_ROM_Length          = __ICFEDIT_SE_Key_region_ROM_end__ - __ICFEDIT_SE_Key_region_ROM_start__ + 1;
SE_Startup_region_ROM_Length      = __ICFEDIT_SE_Code_nokey_region_ROM_start__ - __ICFEDIT_SE_Startup_region_ROM_start__;
SE_ROM_region_Length              = __ICFEDIT_SE_Code_region_ROM_end__ - __ICFEDIT_SE_Code_nokey_region_ROM_start__ + 1;
SE_IF_region_ROM_Length           = __ICFEDIT_SE_IF_region_ROM_end__ - __ICFEDIT_SE_IF_region_ROM_start__ + 1;
SB_HDP_ROM_region_Length          = __ICFEDIT_SB_HDP_region_ROM_end__ - __ICFEDIT_SB_HDP_region_ROM_start__ + 1;
SB_ROM_region_Length              = __ICFEDIT_SB_region_ROM_end__ - __ICFEDIT_SB_region_ROM_start__ + 1;

/* RAM section */
/* SE RAM region: SE stack placed first, stack overflow stays within SE RAM */
__ICFEDIT_SE_region_RAM_start__     = 0x20000000;
__ICFEDIT_SE_region_RAM_stack_top__ = 0x20000400;
__ICFEDIT_SE_region_RAM_end__       = 0x20000FFF;

/* Secure memory activation code area - execution in RAM */
__ICFEDIT_SB_HDP_Code_region_RAM_start__ = __ICFEDIT_SE_region_RAM_end__ + 1;
__ICFEDIT_SB_HDP_Code_region_RAM_end__   = __ICFEDIT_SB_HDP_Code_region_RAM_start__ + 0xFF;

/* SBSFU RAM region */
__ICFEDIT_SB_region_RAM_start__     = __ICFEDIT_SB_HDP_Code_region_RAM_end__ + 1;
__ICFEDIT_SB_region_RAM_end__       = 0x2001FFFF;

SE_RAM_region_Length          = __ICFEDIT_SE_region_RAM_end__ - __ICFEDIT_SE_region_RAM_stack_top__ + 1;
SB_HDP_CODE_RAM_region_Length = __ICFEDIT_SB_HDP_Code_region_RAM_end__ - __ICFEDIT_SB_HDP_Code_region_RAM_start__ + 1;
SB_RAM_region_Length          = __ICFEDIT_SB_region_RAM_end__ - __ICFEDIT_SB_region_RAM_start__ + 1;

MEMORY
{
 SE_Entry_Secure_ROM_Region (rx)  : ORIGIN = __ICFEDIT_SE_CallGate_region_ROM_start__,  LENGTH = SE_Entry_Secure_ROM_Region_Length
 SE_Key_region_ROM (rx)           : ORIGIN = __ICFEDIT_SE_Key_region_ROM_start__,        LENGTH = SE_Key_region_ROM_Length
 SE_Startup_region_ROM (rx)       : ORIGIN = __ICFEDIT_SE_Startup_region_ROM_start__,    LENGTH = SE_Startup_region_ROM_Length
 SE_ROM_region (rx)               : ORIGIN = __ICFEDIT_SE_Code_nokey_region_ROM_start__, LENGTH = SE_ROM_region_Length
 SE_IF_region_ROM (rx)            : ORIGIN = __ICFEDIT_SE_IF_region_ROM_start__,         LENGTH = SE_IF_region_ROM_Length
 SB_HDP_ROM_region (rx)           : ORIGIN = __ICFEDIT_SB_HDP_region_ROM_start__,        LENGTH = SB_HDP_ROM_region_Length
 SB_ROM_region (rx)               : ORIGIN = __ICFEDIT_SB_region_ROM_start__,            LENGTH = SB_ROM_region_Length
 SE_RAM_region (xrw)              : ORIGIN = __ICFEDIT_SE_region_RAM_stack_top__,        LENGTH = SE_RAM_region_Length
 SB_HDP_Code_RAM_region (xrw)     : ORIGIN = __ICFEDIT_SB_HDP_Code_region_RAM_start__,  LENGTH = SB_HDP_CODE_RAM_region_Length
 SB_RAM_region (xrw)              : ORIGIN = __ICFEDIT_SB_region_RAM_start__,            LENGTH = SB_RAM_region_Length
 /* AXI SRAM (D1 domain): DMA1/DMA2-accessible; used for DMA buffers.
    DTCM (0x20000000) is NOT on the AXI/AHB bus matrix and therefore
    cannot be accessed by DMA controllers. */
 RAM_D2 (xrw)                     : ORIGIN = 0x24000000,                                 LENGTH = 64K
}
