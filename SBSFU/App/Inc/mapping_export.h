/**
  ******************************************************************************
  * @file    mapping_export.h
  * @brief   Exports memory mapping symbols defined in linker scripts to C code.
  ******************************************************************************
  */

#ifndef MAPPING_EXPORT_H
#define MAPPING_EXPORT_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined (__ICCARM__) || defined(__GNUC__)
extern uint32_t __ICFEDIT_intvec_start__;
#define INTVECT_START ((uint32_t)& __ICFEDIT_intvec_start__)
extern uint32_t __ICFEDIT_SE_Startup_region_ROM_start__;
#define SE_STARTUP_REGION_ROM_START ((uint32_t)& __ICFEDIT_SE_Startup_region_ROM_start__)
extern uint32_t __ICFEDIT_SE_Code_region_ROM_start__;
#define SE_CODE_REGION_ROM_START ((uint32_t)& __ICFEDIT_SE_Code_region_ROM_start__)
extern uint32_t __ICFEDIT_SE_Code_region_ROM_end__;
#define SE_CODE_REGION_ROM_END ((uint32_t)& __ICFEDIT_SE_Code_region_ROM_end__)
extern uint32_t __ICFEDIT_SE_IF_region_ROM_start__;
#define SE_IF_REGION_ROM_START ((uint32_t)& __ICFEDIT_SE_IF_region_ROM_start__)
extern uint32_t __ICFEDIT_SE_IF_region_ROM_end__;
#define SE_IF_REGION_ROM_END ((uint32_t)& __ICFEDIT_SE_IF_region_ROM_end__)
extern uint32_t __ICFEDIT_SE_Key_region_ROM_start__;
#define SE_KEY_REGION_ROM_START ((uint32_t)& __ICFEDIT_SE_Key_region_ROM_start__)
extern uint32_t __ICFEDIT_SE_Key_region_ROM_end__;
#define SE_KEY_REGION_ROM_END ((uint32_t)& __ICFEDIT_SE_Key_region_ROM_end__)
extern uint32_t __ICFEDIT_SE_CallGate_region_ROM_start__;
#define SE_CALLGATE_REGION_ROM_START ((uint32_t)& __ICFEDIT_SE_CallGate_region_ROM_start__)
extern uint32_t __ICFEDIT_SB_HDP_region_ROM_start__;
#define SB_HDP_REGION_ROM_START ((uint32_t)& __ICFEDIT_SB_HDP_region_ROM_start__)
extern uint32_t __ICFEDIT_SB_HDP_region_ROM_end__;
#define SB_HDP_REGION_ROM_END ((uint32_t)& __ICFEDIT_SB_HDP_region_ROM_end__)
extern uint32_t __ICFEDIT_SB_region_ROM_start__;
#define SB_REGION_ROM_START ((uint32_t)& __ICFEDIT_SB_region_ROM_start__)
extern uint32_t __ICFEDIT_SB_region_ROM_end__;
#define SB_REGION_ROM_END ((uint32_t)& __ICFEDIT_SB_region_ROM_end__)
extern uint32_t __ICFEDIT_SE_region_RAM_start__;
#define SE_REGION_RAM_START ((uint32_t)& __ICFEDIT_SE_region_RAM_start__)
extern uint32_t __ICFEDIT_SE_region_RAM_end__;
#define SE_REGION_RAM_END ((uint32_t)& __ICFEDIT_SE_region_RAM_end__)
extern uint32_t __ICFEDIT_SB_HDP_Code_region_RAM_start__;
#define SB_HDP_CODE_REGION_RAM_START ((uint32_t)& __ICFEDIT_SB_HDP_Code_region_RAM_start__)
extern uint32_t __ICFEDIT_SB_HDP_Code_region_RAM_end__;
#define SB_HDP_CODE_REGION_RAM_END ((uint32_t)& __ICFEDIT_SB_HDP_Code_region_RAM_end__)
extern uint32_t __ICFEDIT_SB_region_RAM_start__;
#define SB_REGION_RAM_START ((uint32_t)& __ICFEDIT_SB_region_RAM_start__)
extern uint32_t __ICFEDIT_SB_region_RAM_end__;
#define SB_REGION_RAM_END ((uint32_t)& __ICFEDIT_SB_region_RAM_end__)
extern uint32_t __ICFEDIT_SE_region_RAM_stack_top__;
#define SE_REGION_RAM_STACK_TOP ((uint32_t)& __ICFEDIT_SE_region_RAM_stack_top__)
#endif /* __ICCARM__ || __GNUC__ */

/* Firmware image slot symbols */
#if defined (__ICCARM__) || defined(__GNUC__)
extern uint32_t __ICFEDIT_SLOT_Active_1_header__;
#define SLOT_ACTIVE_1_HEADER ((uint32_t)& __ICFEDIT_SLOT_Active_1_header__)
extern uint32_t __ICFEDIT_SLOT_Active_1_start__;
#define SLOT_ACTIVE_1_START  ((uint32_t)& __ICFEDIT_SLOT_Active_1_start__)
extern uint32_t __ICFEDIT_SLOT_Active_1_end__;
#define SLOT_ACTIVE_1_END    ((uint32_t)& __ICFEDIT_SLOT_Active_1_end__)

#define SLOT_ACTIVE_2_HEADER (0U)
#define SLOT_ACTIVE_2_START  (0U)
#define SLOT_ACTIVE_2_END    (0U)
#define SLOT_ACTIVE_3_HEADER (0U)
#define SLOT_ACTIVE_3_START  (0U)
#define SLOT_ACTIVE_3_END    (0U)
#endif /* __ICCARM__ || __GNUC__ */

#ifdef __cplusplus
}
#endif

#endif /* MAPPING_EXPORT_H */
