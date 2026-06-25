#include "ws_frame_parser.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

#define WS_FID_RESERVED 0u   /* mirrors API_HANDLER_ID_GENEGAL in web_api.h */

ws_parse_result_t WsFrame_Parse(const char *payload, uint32_t len,
                                ws_parsed_frame_t *out)
{
    if (!payload || len == 0)
        return WS_PARSE_NO_PAYLOAD;

    cJSON *json = cJSON_ParseWithLengthOpts(payload, (size_t)len, NULL, 0);
    if (!json)
        return WS_PARSE_BAD_JSON;

    /* FID: accept decimal integer or hex string (e.g. "0x1012") */
    uint32_t fid = WS_FID_RESERVED;
    cJSON *fid_j = cJSON_GetObjectItem(json, "FID");
    if (cJSON_IsNumber(fid_j))
        fid = (uint32_t)fid_j->valueint;
    else if (cJSON_IsString(fid_j))
        fid = (uint32_t)strtol(fid_j->valuestring, NULL, 16);

    if (fid == WS_FID_RESERVED) {
        cJSON_Delete(json);
        return WS_PARSE_BAD_FID;
    }

    cJSON *flags_j = cJSON_GetObjectItem(json, "FLAGS");
    if (!cJSON_IsNumber(flags_j)) {
        cJSON_Delete(json);
        return WS_PARSE_MISSING_FLAGS;
    }

    cJSON *sid_j = cJSON_GetObjectItem(json, "SID");
    if (!cJSON_IsNumber(sid_j)) {
        cJSON_Delete(json);
        return WS_PARSE_MISSING_SID;
    }

    if (out) {
        out->fid   = fid;
        out->sid   = (uint32_t)sid_j->valueint & 0xffffu;
        out->flags = (uint32_t)flags_j->valueint;
    }

    cJSON_Delete(json);
    return WS_PARSE_OK;
}
