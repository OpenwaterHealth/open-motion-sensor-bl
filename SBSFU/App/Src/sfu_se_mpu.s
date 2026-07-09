/**
  ******************************************************************************
  * @file      sfu_se_mpu.s
  * @brief     SE interface assembly stubs for MPU-based isolation (STM32H7).
  *            Provides:
  *              SVC_Handler  - Redirects SVC calls to MPU_SVC_Handler
  *              jump_to_function - Initialise SP and jump via PC
  *              launch_application - Exception-return into user application
  ******************************************************************************
  */

  .section  .text,"ax",%progbits
  .syntax unified
  .cpu cortex-m7
  .thumb

/**
  * @brief  SVC_Handler - forward to MPU_SVC_Handler with the caller's stacked
  *         frame as first argument.
  * @note   EXC_RETURN bit 2 (in LR on exception entry) tells which stack the
  *         exception frame was pushed onto: 0 = MSP (privileged caller),
  *         1 = PSP (unprivileged caller). Selecting it dynamically lets
  *         SFU_MPU_SysCall() work both when SBSFU runs unprivileged on PSP
  *         (MPU isolation enabled) and when it stays privileged on MSP
  *         (development mode, SFU_MPU_PROTECT_ENABLE undefined).
  */
  .weak   MPU_SVC_Handler
  .global SVC_Handler
  .type   SVC_Handler, %function
SVC_Handler:
  TST   lr, #4
  ITE   EQ
  MRSEQ r0, MSP
  MRSNE r0, PSP
  B MPU_SVC_Handler
  .size SVC_Handler, .-SVC_Handler

/**
  * @brief  jump_to_function - Load SP and PC from a vector table entry
  *         R0 = pointer to {SP, PC} pair
  */
  .section  .RamFunc,"ax",%progbits
  .global jump_to_function
  .type   jump_to_function, %function
jump_to_function:
  LDR SP, [R0]
  LDR PC, [R0,#4]
  .size jump_to_function, .-jump_to_function

/**
  * @brief  launch_application - Use an exception return to branch into the user
  *         application in Thread mode.
  *         R0 = application vector-table base address (e.g. 0x08020400)
  *         R1 = jump_to_function address (kept for call-site compatibility; unused)
  * @note   This runs in Handler mode (called from the SVC handler), so the only
  *         way back to Thread mode is an exception return. We build a basic
  *         exception frame on the CURRENT stack (MSP) and return with
  *         EXC_RETURN=0xFFFFFFF9 (Thread mode, MSP). The frame's stacked PC is
  *         the application reset handler read from *(Address+4); the application
  *         startup then sets its own stack pointer (ldr sp,=_estack) and VTOR.
  *         The frame MUST be unstacked from the same stack it was pushed on:
  *         the previous implementation pushed on MSP but returned via PSP, which
  *         unstacked garbage and faulted before reaching the application.
  */
  /* launch_application runs from flash. It MUST NOT stay in the .RamFunc
     section opened above for jump_to_function: this port's startup does not
     copy .RamFunc into RAM, so anything left there is uninitialised garbage.
     Switch back to .text so launch_application is executed in place from flash. */
  .section  .text,"ax",%progbits
  .global launch_application
  .type   launch_application, %function
launch_application:
  /* R0 = application vector-table address; R1 = jump_to_function (unused here).
     The reference design returns into jump_to_function (in .RamFunc) which loads
     SP from vector[0] then branches to vector[1]. This port's startup does NOT
     copy .RamFunc into RAM, so jump_to_function would be garbage. Instead we
     exception-return DIRECTLY to the application reset handler (vector[1], in
     flash); the application's GCC Reset_Handler sets its own SP (ldr sp,=_estack)
     as its first instruction, so the SP-from-vector step is not required. */
  LDR R2, [R0, #4]     /* R2 = application reset handler = vector[1] */
  ORR R2, R2, #1       /* ensure Thumb bit set */
  MOV R3, #0x01000000  /* xPSR: Thumb bit */
  PUSH {R3}            /* xPSR */
  PUSH {R2}            /* PC  = application reset handler */
  MOV R3, #0           /* zero the remaining context registers */
  PUSH {R3}            /* LR  */
  PUSH {R3}            /* R12 */
  PUSH {R3}            /* R3  */
  PUSH {R3}            /* R2  */
  PUSH {R3}            /* R1  */
  PUSH {R3}            /* R0  */
  MOV LR, #0xFFFFFFF9  /* Return to Thread mode, Main stack (MSP), basic frame */
  BX LR                /* exception return into the application */
  .size launch_application, .-launch_application

  .end
