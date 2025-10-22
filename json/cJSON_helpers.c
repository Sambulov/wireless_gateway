
#include "cJSON_helpers.h"
#include "cJSON.h"
#include "string.h"

uint8_t cJSON_ParseInt(cJSON *pxJson, const char *pcName, uint32_t *pulOutVal) {
    uint32_t val = 0;
    if(pcName != NULL) pxJson = cJSON_GetObjectItem(pxJson, pcName);
    if (cJSON_IsNumber(pxJson)) val = pxJson->valueint;
    else if (cJSON_IsString(pxJson)) val = (uint32_t)strtol(pxJson->valuestring, NULL, 16);
    else return 0;
    *pulOutVal = val;
    return 1;
}

uint8_t json_parse_int(json_t *, const char *, uint32_t *) __attribute__ ((alias ("cJSON_ParseInt")));
