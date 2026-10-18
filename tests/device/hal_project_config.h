#pragma once

/* Targets and wiring come from the assemblies in device_tree.json. */

/* Boards whose status LED is an addressable pixel. */
#define HAL_ENABLE_RGB_LED

/* The stack measurement paints 2 KiB below the measuring frame; 4 KiB is
 * also the largest RP2040 core 0 stack (the SCRATCH_Y bank). */
#define HAL_RP_CORE0_STACK_SIZE 4096u
#define HAL_STM32_MAIN_STACK_SIZE 4096u
#define HAL_FREERTOS_TASK0_STACK 4096u
