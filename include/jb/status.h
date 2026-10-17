#ifndef JB_STATUS_H
#define JB_STATUS_H

/** @file status.h
 * Shared operation results, borrowed from HAL until the first integration pin.
 * HAL_ETYPE is supplied by that same header. No HAL runtime is linked.
 */
#include "jb/abi.h"
#include <hal/core/hal_status.h>

JB_STATIC_ASSERT(sizeof(hal_status_t) == 4 && JB_ALIGNOF(hal_status_t) == 4,
                 "jb requires a naturally aligned 32-bit hal_status_t");

#endif
