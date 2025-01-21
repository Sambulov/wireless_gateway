#include <stdio.h>
#include "esp_log.h"
#include "ws_api.h"

#define TAG 	"ws_api"

void ws_api_inc_test(void)
{
	const unsigned int MAX = 100;
	static unsigned int cnt;
	unsigned int i;

	for (i = 0; i < MAX; ++i) {
        	ESP_LOGI(TAG, "conter = %u\n", cnt);
		cnt++;
	}
}
