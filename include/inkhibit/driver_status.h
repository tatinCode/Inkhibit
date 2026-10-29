#ifndef INKHIBIT_DRIVER_STATUS_H
#define INKHIBIT_DRIVER_STATUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns 0 on success, otherwise a positive errno-style code.
 * The first error remains available through cleanup.
 * Starting a new, closed hardware session clears the previous error.
 */
int inkhibit_driver_error(void);
const char *inkhibit_driver_error_message(void);

/* Records the first error only. Copies the supplied message. */
void inkhibit_driver_fail(int code, const char *message);

/*
 * Records the first error only. Copies the supplied message.
 */
int inkhibit_driver_wait_ready(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
