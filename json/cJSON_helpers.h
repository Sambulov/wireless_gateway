#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "cJSON.h"
#include "stdint.h"

uint8_t cJSON_ParseInt(cJSON *pxJson, const char *pcName, uint32_t *pulOutVal);
uint8_t cJSON_StringValue(cJSON *pxJson, const char *pcName, char **pcOutVal);

/*
	Snake notation
*/

typedef cJSON json_t;
typedef cJSON_bool json_bool_t;

uint8_t json_parse_int(json_t *json, const char *name, uint32_t *out_val);
uint8_t json_string_value(json_t *json, const char *name, char **out_val);

static inline json_t *json_parse_with_length_opts(const char *value, size_t buffer_length, const char **return_parse_end, json_bool_t require_null_terminated) {
    return cJSON_ParseWithLengthOpts(value, buffer_length, return_parse_end, require_null_terminated);
}

static inline void json_delete(json_t *item) { cJSON_Delete(item); }

#ifdef __cplusplus
}
#endif
