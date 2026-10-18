#include "serial_frame.h"

#include <limits.h>
#include <string.h>

bool sf_init(sf_parser *parser, uint32_t timeout_ms)
{
    if (parser == NULL || timeout_ms > INT32_MAX) {
        return false;
    }
    memset(parser, 0, sizeof(*parser));
    parser->timeout_ms = timeout_ms;
    return true;
}

void sf_reset(sf_parser *parser)
{
    if (parser != NULL) {
        uint32_t timeout_ms = parser->timeout_ms;
        (void)sf_init(parser, timeout_ms);
    }
}

uint16_t sf_crc16_modbus(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xffffu;
    size_t i;
    for (i = 0; i < length; ++i) {
        unsigned bit;
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit) {
            crc = (uint16_t)((crc >> 1) ^ ((crc & 1u) ? 0xa001u : 0u));
        }
    }
    return crc;
}

size_t sf_build_frame(const uint8_t *payload, size_t length,
                      uint8_t *output, size_t capacity)
{
    uint16_t crc;
    if (length > SF_MAX_PAYLOAD || output == NULL ||
        (payload == NULL && length != 0) || capacity < length + SF_OVERHEAD) {
        return 0;
    }
    output[0] = 0xaau;
    output[1] = 0x55u;
    output[2] = (uint8_t)length;
    if (length != 0) {
        memcpy(output + 3, payload, length);
    }
    crc = sf_crc16_modbus(output + 2, length + 1);
    output[length + 3] = (uint8_t)crc;
    output[length + 4] = (uint8_t)(crc >> 8);
    return length + SF_OVERHEAD;
}

size_t sf_tick(sf_parser *parser, uint32_t now_ms)
{
    size_t dropped;
    if (parser == NULL || parser->pending == 0 || parser->timeout_ms == 0 ||
        (uint32_t)(now_ms - parser->waiting_since_ms) < parser->timeout_ms) {
        return 0;
    }
    dropped = parser->pending;
    parser->discarded += (uint32_t)dropped;
    parser->timeouts++;
    parser->pending = 0;
    return dropped;
}

size_t sf_feed(sf_parser *parser, const uint8_t *data, size_t length,
               uint32_t now_ms, sf_frame_callback callback, void *user)
{
    size_t i;
    size_t emitted = 0;
    if (parser == NULL || (data == NULL && length != 0)) {
        return 0;
    }
    (void)sf_tick(parser, now_ms);
    for (i = 0; i < length; ++i) {
        uint8_t byte = data[i];
        size_t total;
        uint16_t received;
        if (parser->pending == 0) {
            if (byte == 0xaau) {
                parser->buffer[0] = byte;
                parser->pending = 1;
                parser->waiting_since_ms = now_ms;
            } else {
                parser->discarded++;
            }
            continue;
        }
        if (parser->pending == 1 && byte != 0x55u) {
            parser->discarded++;
            if (byte == 0xaau) {
                parser->waiting_since_ms = now_ms;
            } else {
                parser->discarded++;
                parser->pending = 0;
            }
            continue;
        }
        parser->buffer[parser->pending++] = byte;
        if (parser->pending < 3) {
            continue;
        }
        total = (size_t)parser->buffer[2] + SF_OVERHEAD;
        if (parser->pending < total) {
            continue;
        }
        received = (uint16_t)((uint16_t)parser->buffer[total - 2] |
                             ((uint16_t)parser->buffer[total - 1] << 8));
        if (sf_crc16_modbus(parser->buffer + 2, total - 4) == received) {
            parser->frames++;
            emitted++;
            if (callback != NULL) {
                callback(parser->buffer + 3, parser->buffer[2], user);
            }
        } else {
            parser->crc_errors++;
            parser->discarded += (uint32_t)total;
        }
        parser->pending = 0;
    }
    return emitted;
}
