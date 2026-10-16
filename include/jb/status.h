#ifndef JB_STATUS_H
#define JB_STATUS_H

/** @file status.h
 * Shared operation results, borrowed from HAL until the first integration pin.
 * The prototype adds no result codes and links no HAL runtime.
 */
#include "jb/abi.h"
#include <hal/core/hal_status.h>

JB_STATIC_ASSERT(sizeof(hal_status_t) == 4 && JB_ALIGNOF(hal_status_t) == 4,
                 "jb requires a naturally aligned 32-bit hal_status_t");

#endif
