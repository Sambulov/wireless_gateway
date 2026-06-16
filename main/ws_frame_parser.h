#pragma once
#include <stdint.h>

typedef enum {
    WS_PARSE_OK            = 0,
    WS_PARSE_NO_PAYLOAD    = 1,  /* NULL or zero-length — caller skips the frame */
    WS_PARSE_BAD_JSON      = 2,  /* cJSON rejected the payload */
    WS_PARSE_BAD_FID       = 3,  /* FID missing or equals reserved value 0 */
    WS_PARSE_MISSING_FLAGS = 4,  /* FLAGS field absent or not a number */
    WS_PARSE_MISSING_SID   = 5,  /* SID field absent or not a number */
} ws_parse_result_t;

typedef struct {
    uint32_t fid;    /* function ID, non-zero */
    uint32_t sid;    /* session/call ID, masked to lower 16 bits */
    uint32_t flags;  /* caller-supplied flags bitmask */
} ws_parsed_frame_t;

/*
 * Parse and validate a WebSocket text-frame payload.
 * Returns WS_PARSE_OK and fills *out on success.
 * out may be NULL if only the validation result is needed.
 */
ws_parse_result_t WsFrame_Parse(const char *payload, uint32_t len,
                                ws_parsed_frame_t *out);
