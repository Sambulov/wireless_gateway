#include <stdint.h>
#include <stdlib.h>

#include "ws_msg.h"

/* Заголовок и данные — один блок: realloc растит их вместе. */
struct ws_msg {
    uint32_t len;
    uint32_t cap;
    uint8_t  data[];
};

uint8_t *ws_msg_reserve(ws_msg_t **msg, uint32_t add, uint32_t max) {
    if(msg == NULL) return NULL;
    /* Заголовок и данные — один блок: предел обязан оставить место заголовку,
     * иначе sizeof + cap переполнит size_t на 32-битной цели. */
#if SIZE_MAX <= UINT32_MAX                 /* на 64 битах не бывает */
    if((size_t)max > SIZE_MAX - sizeof(ws_msg_t))
        max = (uint32_t)(SIZE_MAX - sizeof(ws_msg_t));
#endif
    uint32_t len = (*msg != NULL) ? (*msg)->len : 0;
    /* Сравнение без сложения: len + add может переполнить uint32. */
    if((len > max) || (add > max - len)) return NULL;

    if((*msg == NULL) || ((*msg)->cap - len < add)) {
        uint32_t cap = len + add;
        ws_msg_t *grown = realloc(*msg, sizeof(ws_msg_t) + cap);
        if(grown == NULL) return NULL;
        grown->len = len;
        grown->cap = cap;
        *msg = grown;
    }
    return (*msg)->data + len;
}

void ws_msg_commit(ws_msg_t *msg, uint32_t len) {
    if(msg == NULL) return;
    if(len > msg->cap - msg->len) len = msg->cap - msg->len;
    msg->len += len;
}

const uint8_t *ws_msg_data(const ws_msg_t *msg) {
    return (msg != NULL) ? msg->data : NULL;
}

uint32_t ws_msg_len(const ws_msg_t *msg) {
    return (msg != NULL) ? msg->len : 0;
}

void ws_msg_free(ws_msg_t **msg) {
    if(msg == NULL) return;
    free(*msg);
    *msg = NULL;
}
