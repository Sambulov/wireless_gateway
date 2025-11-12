#ifndef LOW_SPEED_PERIPH_H_
#define LOW_SPEED_PERIPH_H_

#include <stdint.h>

struct lsp_periph_bus {
    const char *name;
    size_t (*read)(uint8_t *data, size_t len);
    size_t (*write)(const uint8_t *data, size_t len);
};

#endif /* LOW_SPEED_PERIPH_H_ */
