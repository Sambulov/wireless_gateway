#pragma once

#include <stdint.h>
#include <stddef.h>

/* Контракт транспорта.
 *
 * Ядро API (ws_server.c) работает только через этот интерфейс: оно не знает ни
 * IDF, ни WebSocket. Транспорт принимает соединения, собирает СООБЩЕНИЕ из
 * фрагментов (WebSocket разрешает резать одно сообщение на кадры: первый с
 * FIN=0, дальше продолжения), сам обрабатывает управляющие кадры PING/PONG/CLOSE
 * и отдаёт ядру готовое сообщение одним событием RECEIVED. Ядру транспорт
 * отдаёт УРОВЕНЬ СООБЩЕНИЙ, а не уровень кадров.
 *
 * Роли `self` в iface:
 *   init / open / subscribe  — операции транспорта как модуля: self — дескриптор
 *                              модуля (у нас это статическая структура транспорта);
 *   close / begin_send / break_sending — операции соединения: self — дескриптор
 *                              соединения, он же приходит в событиях. Ядру этот
 *                              указатель непрозрачен: оно использует его только
 *                              как ключ своей сессии. */

/* Длина потока неизвестна. Транспорт нашего уровня сообщений всегда отдаёт
 * сообщение целиком, поэтому в событиях RECEIVED/COMPLETE/ERR поле left == 0. */
#define WS_TRANSPORT_STREAM_LENGTH  0xffffffffu

typedef enum {
    WS_TRANSPORT_OPEN              = 0x01, /* начали слушать / пытаемся подключиться */
    WS_TRANSPORT_ESTABLISHED       = 0x02, /* принято новое соединение */
    WS_TRANSPORT_CLOSED            = 0x04, /* соединение разорвано: освобождай ресурсы */
    WS_TRANSPORT_TRANSFER_BUFFER   = 0x10, /* транспорт даёт буфер под отправку */
    WS_TRANSPORT_TRANSFER_COMPLETE = 0x20, /* отправка завершена */
    WS_TRANSPORT_TRANSFER_ERR      = 0x40, /* отправка не удалась */
    WS_TRANSPORT_RECEIVED          = 0x80, /* приняты данные */
} ws_transport_event_type_t;

typedef enum {
    WS_TRANSPORT_INVALID  = 0x00,
    WS_TRANSPORT_OK       = 0x01,
    WS_TRANSPORT_BUSY     = 0x02,
    WS_TRANSPORT_BAD_CALL = 0x04,
    WS_TRANSPORT_FAULT    = 0x08,
} ws_transport_result_t;

typedef struct {
    ws_transport_event_type_t event;
    union {
        /* RECEIVED: ptr — собранное сообщение целиком (length — его длина),
         *           left == 0 (см. WS_TRANSPORT_STREAM_LENGTH).
         * TRANSFER_COMPLETE/ERR: ptr — ровно тот указатель, который ядро отдало
         *           в begin_send, length — его длина. По нему ядро и находит свой
         *           блок ответа, поэтому указатель должен вернуться неизменным.
         * Данные принадлежат транспорту: ядро их не освобождает и не изменяет,
         * они живут до возврата из обработчика события. */
        struct { const void *ptr; uint32_t length; uint32_t left; } data;
        /* TRANSFER_BUFFER — «вот буфер, пиши в него». Наш транспорт буферов не
         * выдаёт: буфер держит сам транспорт и сам же отправляет. Ядру это
         * событие не приходит; тип оставлен, чтобы интерфейс совпадал с
         * образцом. */
        struct { void *ptr; uint32_t size; uint32_t fill; } buffer;
    };
} ws_transport_event_trigger_t;

/* Обработчик событий транспорта. Возвращает WS_TRANSPORT_OK, если событие
 * принято; на ESTABLISHED отказ означает «соединение обслуживать нечем» —
 * транспорт вправе его закрыть. */
typedef ws_transport_result_t (*ws_transport_event_handler_t)(const void *self,
                                                             const ws_transport_event_trigger_t *ev);

typedef struct {
    /* Подготовить модуль к работе (нашему транспорту — запомнить адрес, на
     * котором он обслуживает соединения). config — настройки транспорта, вид
     * структуры задаёт реализация. */
    ws_transport_result_t (*init)(const void *self, const void *config);
    /* Начать слушать. Отдельного «начать слушать» у нашего транспорта нет:
     * сервер поднимает другой модуль, а транспорт лишь принимает соединения,
     * поэтому open означает «я готов принимать соединения». Событие OPEN наш
     * транспорт не выдаёт. */
    ws_transport_result_t (*open)(const void *self, const void *config);
    /* Разорвать соединение и освободить его ресурсы. */
    ws_transport_result_t (*close)(const void *self);
    /* Отправить СООБЩЕНИЕ (транспорт сам строит кадры). Асинхронно: результат
     * приходит событием TRANSFER_COMPLETE/TRANSFER_ERR, а данные должны жить до
     * него. Возврат != WS_TRANSPORT_OK означает, что отправка не принята и
     * события не будет — освобождай буфер сам. */
    ws_transport_result_t (*begin_send)(const void *self, const void *data, uint32_t length);
    /* Прервать начатую отправку. Наш транспорт отправляет сообщение одним
     * куском, частичной отмены у него нет (см. реализацию). */
    ws_transport_result_t (*break_sending)(const void *self);
    /* Подписаться на события. context транспорт запоминает сам: в сигнатуре
     * события его нет, потому что дескриптор соединения уже говорит, о ком речь. */
    ws_transport_result_t (*subscribe)(const void *self, ws_transport_event_handler_t handler,
                                      void *context);
    /* Числовой идентификатор соединения для внешних подсистем. Необязателен
     * (может быть NULL). Ядру он нужен только чтобы заполнить webapi_msg_t.fd:
     * обработчики по нему отличают запрос из softAP (request_from_softap).
     * Для сокетного транспорта это номер сокета; при NULL ядро подставляет -1. */
    int32_t (*peer_id)(const void *self);
} ws_transport_iface_t;
