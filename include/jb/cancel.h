#ifndef JB_CANCEL_H
#define JB_CANCEL_H

/** @file cancel.h
 * Cancellation callback shared by synchronous host operations.
 */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Polled on the calling thread; true requests HAL_ECANCELED. Context may be
 * NULL. Return promptly without throwing; use atomics for cross-thread requests.
 * Individual operations document when completion takes priority over cancellation.
 */
typedef bool (*jb_cancel_fn)(void *context);

#ifdef __cplusplus
}
#endif
#endif
