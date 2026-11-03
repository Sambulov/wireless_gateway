/* Сборка входящего сообщения WebSocket из фрагментов (первый кадр с FIN=0,
 * дальше продолжения). Политика без платформы: сколько можно накопить решает
 * предел ВСЕГО сообщения, а не одного кадра — иначе поток продолжений растит
 * буфер, пока не кончится куча. Транспорт только читает кадры в отданное место.
 */
#ifndef WS_MSG_H
#define WS_MSG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ws_msg ws_msg_t;

/* Место под ещё add байт в конце сообщения *msg (*msg == NULL — новое
 * сообщение, заводится и при add == 0). Возвращает, куда читать кадр; принятое
 * учитывает ws_msg_commit. NULL — сообщение с этими байтами превысит max или
 * нет памяти; накопленное при этом не тронуто. Предел урезается так, чтобы
 * блок «заголовок + данные» помещался в size_t. */
uint8_t *ws_msg_reserve(ws_msg_t **msg, uint32_t add, uint32_t max);

/* Учесть len байт, прочитанных в место от ws_msg_reserve (не больше add). */
void ws_msg_commit(ws_msg_t *msg, uint32_t len);

const uint8_t *ws_msg_data(const ws_msg_t *msg);
uint32_t       ws_msg_len(const ws_msg_t *msg);   /* 0 для NULL */

/* Выбросить сообщение; *msg становится NULL. */
void ws_msg_free(ws_msg_t **msg);

#ifdef __cplusplus
}
#endif

#endif /* WS_MSG_H */
