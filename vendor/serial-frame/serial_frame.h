#ifndef SERIAL_FRAME_H
#define SERIAL_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SF_MAX_PAYLOAD 255u
#define SF_OVERHEAD 5u
#define SF_MAX_FRAME (SF_MAX_PAYLOAD + SF_OVERHEAD)

/* Payload is valid only during callback. Copy it if it must be retained.
 * Do not re-enter feed/tick/reset on this parser from the callback. */
typedef void (*sf_frame_callback)(const uint8_t *payload, size_t length, void *user);

typedef struct {
    uint8_t buffer[SF_MAX_FRAME];
    size_t pending;
    uint32_t timeout_ms;
    uint32_t waiting_since_ms;
    uint32_t frames;
    uint32_t crc_errors;
    uint32_t discarded;
    uint32_t timeouts;
} sf_parser;

/* Initialize before use. Treat buffer/pending/configuration as read-only after
 * initialization; use sf_reset to clear state. Statistics may wrap at 2^32. */

/* timeout_ms=0 disables timeout; otherwise must be <= INT32_MAX.
 * now_ms is a monotonic uint32 millisecond tick (wrap is supported).
 * Call feed/tick at least once per 2^31 ms; serialized use only, no locking. */
bool sf_init(sf_parser *parser, uint32_t timeout_ms);
void sf_reset(sf_parser *parser); /* keeps timeout configuration, clears counters */
/* data must reference length readable bytes; NULL is allowed only for length=0. */
uint16_t sf_crc16_modbus(const uint8_t *data, size_t length);
/* Returns frame size, or 0 for invalid pointers, length or capacity.
 * NULL payload is allowed for length=0; output must not overlap payload. */
size_t sf_build_frame(const uint8_t *payload, size_t length,
                      uint8_t *output, size_t capacity);
size_t sf_tick(sf_parser *parser, uint32_t now_ms);
/* Returns number of valid frames; NULL callback discards decoded payloads.
 * NULL data is allowed for length=0. Invalid input leaves state unchanged. */
size_t sf_feed(sf_parser *parser, const uint8_t *data, size_t length,
               uint32_t now_ms, sf_frame_callback callback, void *user);

#ifdef __cplusplus
}
#endif
#endif
