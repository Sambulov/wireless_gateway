#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>

#include "GwJson.h"
#include "CodeLib.h"
#include "api.h"

static const char *TAG = "api";

/* ── Механика ядра ──────────────────────────────────────────────────────────
 *
 * Дескриптор вызова ходит по спискам free → incoming → executing → free.
 * Пир — одно соединение транспорта: своя очередь кадров, живёт, пока на него
 * ссылается вызов. Буфер кадра разделяется получателями рассылки и
 * освобождается последним. */

#define API_CALL_POOL_SIZE   32      /* дескрипторов вызовов на ядро */
#define API_TX_QUEUE_MAX      8      /* кадров в очереди одного пира (голова в работе) */
#define API_FRAME_OVERHEAD   64      /* FID, SID, скобки ARG/STA и терминатор */
#define API_ARG_PROP        "\"ARG\":"
#define API_ARG_PROP_LEN      6      /* без открывающей скобки: её пишет кадр */
#define API_FRAME_MAX_TOKENS 48

/* Скопировать имя FID в буфер. 0 — пустое или слишком длинное имя. */
static uint8_t api_fid_set(char *dst, const char *name) {
    if(!dst || !name || !name[0]) return 0;
    size_t n = 0;
    while(name[n] && n < API_FID_NAME_MAX - 1) n++;
    if(name[n] != '\0') return 0;
    for(size_t i = 0; i < n; i++) dst[i] = name[i];
    dst[n] = '\0';
    return 1;
}

typedef struct api_peer api_peer_t;

/* Готовый кадр со счётчиком владельцев: один буфер уходит нескольким пирам,
 * освобождает его последний. */
typedef struct {
    uint32_t holders;
    int32_t  len;
    char     buf[];
} api_frame_buffer_t;

typedef struct api_frame_t {
    struct api_frame_t *next;
    api_frame_buffer_t *data;
} api_frame_t;

typedef struct {
    __linked_list_object__
    api_handler_t handler;
    char fid[API_FID_NAME_MAX];
    void *context;                    /* родной контекст, приходит первому вызову */
} api_handler_item_t;

typedef struct api_call_s {
    __linked_list_object__
    api_t *api;
    api_peer_t *peer;                 /* соединение, которому принадлежит вызов */
    char fid[API_FID_NAME_MAX];
    api_handler_t handler;
    uint32_t sid;
    uint8_t  has_sid;
    void *context;                    /* per-call контекст, владеет обработчик */
    uint8_t *arg_data;                /* копия ARG, живёт до исполнения */
    uint32_t arg_len;
    uint8_t  teardown;                /* просили снять: зов обработчика с NULL */
} api_call_t;

/* Пир — соединение транспорта. Дескриптор соединения для ядра непрозрачен. */
struct api_peer {
    __linked_list_object__
    const void *conn;
    api_frame_t *tx_frames;
    uint32_t calls;                   /* сколько дескрипторов ссылается на пир */
    uint8_t  tx_busy;                 /* голова очереди у транспорта */
    uint8_t  closed;                  /* соединения уже нет: ждёт уборки */
};

/* ── Состояние ядра ──────────────────────────────────────────────────────── */
typedef struct api_private_s {
    const transport_t *transport;     /* объект экземпляра (api_init) */
    delegate_t delegate;              /* подписка ядра на события транспорта */
    const api_system_t *system;
    void *mutex;

    linked_list_t handlers;
    linked_list_t peers;               /* живые соединения */
    linked_list_t dead_peers;          /* закрытые, ждут уборки */
    linked_list_t free_calls;
    linked_list_t incoming;
    linked_list_t executing;

    api_call_t pool[API_CALL_POOL_SIZE];
    uint32_t pulse_timer;
} api_private_t;

struct api_s {
    api_private_t core;
};

static api_private_t *api_private(const api_t *api) {
    return (api_private_t *)(void *)api;
}

/* ── Система ──────────────────────────────────────────────────────────────── */
static void api_log(const api_private_t *core, int level, const char *fmt, ...) {
    if((core == NULL) || (core->system == NULL) || (core->system->log == NULL)) return;
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    core->system->log(core->system->context, level, TAG, "%s", buf);
}

static void api_lock(api_private_t *core) {
    core->system->mutex_take(core->system->context, core->mutex);
}

static void api_unlock(api_private_t *core) {
    core->system->mutex_give(core->system->context, core->mutex);
}

static uint32_t api_time(const api_private_t *core) {
    return core->system->time_ms(core->system->context);
}

/* Методы берутся из объекта экземпляра, а адресат — соединение: для этого
 * копия объекта с подменённым контекстом (см. transport.h). */
static transport_t api_transport_of(const api_private_t *core, const void *self) {
    if(core->transport == NULL) {
        const transport_t empty = { NULL, NULL };
        return empty;
    }
    transport_t tr = *core->transport;
    tr.context = (void *)self;
    return tr;
}

/* ── Кадры ────────────────────────────────────────────────────────────────── */

static api_frame_buffer_t *api_frame_buffer_new(uint32_t cap) {
    api_frame_buffer_t *fb = malloc(sizeof(api_frame_buffer_t) + cap);
    if(fb != NULL) {
        fb->holders = 0;
        fb->len     = 0;
    }
    return fb;
}

static api_frame_t *api_frame_release(api_frame_t *frame) {
    if(frame == NULL) return NULL;
    api_frame_t *next = frame->next;
    api_frame_buffer_t *fb = frame->data;
    free(frame);
    if(fb != NULL) {
        if(fb->holders) fb->holders--;
        if(!fb->holders) free(fb);
    }
    return next;
}

/* Освободить одну ссылку на буфер (без кадра): так владелец отдаёт свою. */
static void api_buffer_release(api_frame_buffer_t *fb) {
    if(fb == NULL) return;
    if(fb->holders) fb->holders--;
    if(!fb->holders) free(fb);
}

/* Статус: {"FID":"…","SID":"0x…","STA":"0x…"} — SID печатается только у
 * названного вызова; статус и данные в одном кадре не совмещаются. */
static api_frame_buffer_t *api_frame_buffer_status(const char *fid, uint8_t has_sid,
                                                   uint32_t sid, uint32_t sta) {
    if(fid == NULL) fid = "";
    uint32_t cap = (uint32_t)strlen(fid) + API_FRAME_OVERHEAD + 32;
    api_frame_buffer_t *fb = api_frame_buffer_new(cap);
    if(fb != NULL) {
        fb->len = has_sid
                ? sprintf(fb->buf, "{\"FID\":\"%s\",\"SID\":\"0x%08lx\",\"STA\":\"0x%08lX\"}",
                          fid, (unsigned long)sid, (unsigned long)sta)
                : sprintf(fb->buf, "{\"FID\":\"%s\",\"STA\":\"0x%08lX\"}", fid, (unsigned long)sta);
    }
    return fb;
}

/* Данные: {"FID":"…","SID":"0x…","ARG":<json>}. Пустое тело уходит как null. */
static api_frame_buffer_t *api_frame_buffer_json(const char *fid, uint8_t has_sid,
                                                 uint32_t sid, const char *arg, uint32_t arg_len) {
    if((arg == NULL) || (arg_len == 0)) {
        arg     = "null";
        arg_len = 4;
    }
    uint32_t cap = (uint32_t)strlen(fid) + arg_len + API_FRAME_OVERHEAD + 32;
    api_frame_buffer_t *fb = api_frame_buffer_new(cap);
    if(fb != NULL) {
        int n = has_sid
              ? sprintf(fb->buf, "{\"FID\":\"%s\",\"SID\":\"0x%08lx\",\"ARG\":", fid, (unsigned long)sid)
              : sprintf(fb->buf, "{\"FID\":\"%s\",\"ARG\":", fid);
        memcpy(fb->buf + n, arg, arg_len);
        fb->buf[n + arg_len] = '}';
        fb->len = n + (int32_t)arg_len + 1;
    }
    return fb;
}

/* Переход от области ARG (её отдаёт api_call_begin_response) к буферу кадра:
 * перед ней лежит открывающая скобка и префикс. */
#define API_RESPONSE_TO_FB(resp) \
    (api_frame_buffer_t *)(void *)((char *)(resp) - API_ARG_PROP_LEN - 1 \
                                   - offsetof(api_frame_buffer_t, buf))

/* ── Очередь пира ─────────────────────────────────────────────────────────── */

static void api_tx_drop_head(api_peer_t *peer) {
    if(peer->tx_frames == NULL) return;
    peer->tx_frames = api_frame_release(peer->tx_frames);
}

static void api_tx_clear(api_peer_t *peer) {
    while(peer->tx_frames != NULL) api_tx_drop_head(peer);
    peer->tx_busy = 0;
}

/* Отдать голову очереди транспорту. С этого момента кадр принадлежит транспорту
 * и освобождается только по завершению отправки. */
static void api_peer_tx_action(api_private_t *core, api_peer_t *peer) {
    while(!peer->tx_busy && (peer->tx_frames != NULL)) {
        /* Признак «кадр в работе» ставим ДО вызова: транспорт вправе завершить
         * отправку синхронно, и событие застало бы пустую голову. */
        peer->tx_busy = 1;
        api_frame_t *frame = peer->tx_frames;
        transport_t tr = api_transport_of(core, peer->conn);
        transport_result_t res = transport_send_zero_copy(&tr, frame->data->buf,
                                                          (uint32_t)frame->data->len);
        if(res == TRANSPORT_OK) return;
        peer->tx_busy = 0;
        if(res == TRANSPORT_BUSY) return;         /* остаётся в очереди, повтор тактом */
        api_log(core, API_LOG_WARN, "Peer %p send failed (%d), frame dropped",
                peer->conn, (int)res);
        api_tx_drop_head(peer);
    }
}

/* Поставить кадр в очередь пира. FALSE — пира нет или очередь полна. Владение
 * буфером переходит очереди: каждая удачная постановка держит свою ссылку. */
static uint8_t api_send_frame(api_private_t *core, api_peer_t *peer, api_frame_buffer_t *fb) {
    if(fb == NULL) return HDL_FALSE;
    if((peer == NULL) || peer->closed) {
        api_buffer_release(fb);
        return HDL_FALSE;
    }
    api_frame_t *frame = malloc(sizeof(api_frame_t));
    if(frame == NULL) {
        api_buffer_release(fb);
        return HDL_FALSE;
    }
    frame->next = NULL;
    frame->data = fb;
    fb->holders++;

    api_frame_t **slot = &peer->tx_frames;
    uint32_t queued = 0;
    while((*slot != NULL) && (queued < API_TX_QUEUE_MAX)) {
        slot = &(*slot)->next;
        queued++;
    }
    if(queued >= API_TX_QUEUE_MAX) {
        api_log(core, API_LOG_WARN, "Peer %p tx queue full, frame dropped", peer->conn);
        api_frame_release(frame);
        return HDL_FALSE;
    }
    *slot = frame;
    api_peer_tx_action(core, peer);               /* голова уходит сразу */
    return HDL_TRUE;
}

/* ── Пул вызовов ──────────────────────────────────────────────────────────── */

static void api_call_release(api_private_t *core, api_call_t *call) {
    if(call == NULL) return;
    free(call->arg_data);
    call->arg_data = NULL;
    if(call->peer != NULL) {
        /* Последний ссылающийся вызов позволяет пиру быть снятым. */
        call->peer->calls--;
        call->peer = NULL;
    }
    memset(call, 0, sizeof(*call));
    linked_list_insert_last(&core->free_calls, linked_list_item(call));
}

static api_call_t *api_call_alloc(api_private_t *core) {
    linked_list_item_t *item = linked_list_find_first(core->free_calls, NULL, NULL);
    if(item == NULL) return NULL;
    api_call_t *call = linked_list_get_object(api_call_t, item);
    linked_list_unlink(item);
    memset(call, 0, sizeof(*call));
    return call;
}

/* ── Пиры ─────────────────────────────────────────────────────────────────── */

static api_peer_t *api_peer_find(api_private_t *core, const void *conn) {
    linked_list_item_t *item = linked_list_find_first(core->peers, NULL, NULL);
    while(item != NULL) {
        api_peer_t *peer = linked_list_get_object(api_peer_t, item);
        if(peer->conn == conn) return peer;
        item = linked_list_find_next_no_overlap(item, NULL, NULL);
    }
    return NULL;
}

static api_peer_t *api_peer_create(api_private_t *core, const void *conn) {
    api_peer_t *peer = malloc(sizeof(api_peer_t));
    if(peer == NULL) return NULL;
    memset(peer, 0, sizeof(*peer));
    peer->conn = conn;
    linked_list_insert_last(&core->peers, linked_list_item(peer));
    return peer;
}

/* Пир снимается только когда его вызовы уже завершены: соединения нет, а
 * обработчику ещё нужно освободить своё состояние. */
static void api_peer_destroy(api_private_t *core, api_peer_t *peer) {
    api_tx_clear(peer);
    linked_list_unlink(linked_list_item(peer));
    free(peer);
    (void)core;
}

static void api_peers_reap(api_private_t *core) {
    api_lock(core);
    linked_list_item_t *item = linked_list_find_first(core->dead_peers, NULL, NULL);
    api_peer_t *peer = NULL;
    while(item != NULL) {
        api_peer_t *candidate = linked_list_get_object(api_peer_t, item);
        if(candidate->calls == 0) { peer = candidate; break; }
        item = linked_list_find_next_no_overlap(item, NULL, NULL);
    }
    if(peer != NULL) api_peer_destroy(core, peer);
    api_unlock(core);
}

/* ── Регистрация методов ──────────────────────────────────────────────────── */

static uint8_t handler_fid_match(linked_list_item_t *item, void *arg) {
    const char *name = (const char *)arg;
    api_handler_item_t *hd = linked_list_get_object(api_handler_item_t, item);
    return (name != NULL) && (strcmp(hd->fid, name) == 0);
}

static api_handler_item_t *api_handler_find(api_private_t *core, const char *name) {
    linked_list_item_t *item = linked_list_find_first(core->handlers, handler_fid_match, (void *)name);
    return item ? linked_list_get_object(api_handler_item_t, item) : NULL;
}

uint8_t api_call_register(api_t *api, api_handler_t handler, const char *fid, void *context) {
    if(api == NULL) return 0;
    api_private_t *core = api_private(api);
    char name[API_FID_NAME_MAX];
    if(!api_fid_set(name, fid)) return 0;

    api_lock(core);
    api_handler_item_t *existing = api_handler_find(core, name);
    if(handler == NULL) {
        /* Снятие метода: активные вызовы доживают со своим обработчиком. */
        if(existing != NULL) {
            linked_list_unlink(linked_list_item(existing));
            free(existing);
        }
        api_unlock(core);
        api_log(core, API_LOG_INFO, "Api method %s unregistered", name);
        return 1;
    }
    if(existing != NULL) {
        existing->handler = handler;
        existing->context = context;
        api_unlock(core);
        api_log(core, API_LOG_INFO, "Api method %s updated", name);
        return 1;
    }
    api_handler_item_t *hd = malloc(sizeof(api_handler_item_t));
    if(hd == NULL) {
        api_unlock(core);
        return 0;
    }
    hd->handler = handler;
    mem_cpy(hd->fid, name, sizeof(hd->fid));
    hd->context = context;
    linked_list_insert_last(&core->handlers, linked_list_item(hd));
    api_unlock(core);
    api_log(core, API_LOG_INFO, "Api method %s registered", name);
    return 1;
}

/* ── Диспетчеризация ──────────────────────────────────────────────────────── */

/* ARG клиента обработчику: при отсутствии ARG — указатель на пустую нагрузку,
 * но не NULL: NULL отдан рабочему циклу (пульсу). */
static uint8_t api_empty_arg[1] = { 0 };

/* Вызов дескриптора. FALSE от обработчика — вызов завершён, дескриптор
 * освобождается сразу, повторного вызова не будет. */
static void api_invoke(api_private_t *core, api_call_t *call, uint8_t *arg, uint32_t arg_len) {
    uint8_t *data = (arg != NULL) ? arg : api_empty_arg;
    api_unlock(core);
    uint8_t active = call->handler(call, &call->context, data, arg_len);
    api_lock(core);
    if(!active) {
        linked_list_unlink(linked_list_item(call));
        api_call_release(core, call);
    }
}

/* Кадр называют живой вызов парой (FID, SID): только она и ищется. Номер сам по
 * себе ничего не значит — не нашлось пары, значит это новый вызов. */
static uint8_t call_pair_match(linked_list_item_t *item, void *arg) {
    const api_call_t *want = (const api_call_t *)arg;
    const api_call_t *call = linked_list_get_object(api_call_t, item);
    return !call->teardown && call->has_sid && want->has_sid
           && (call->peer == want->peer) && (call->sid == want->sid)
           && (strcmp(call->fid, want->fid) == 0);
}

static void api_dispatch(api_private_t *core, api_call_t *call) {
    api_lock(core);
    api_call_t *active = NULL;
    if(call->has_sid) {
        linked_list_item_t *found = linked_list_find_first(core->executing, call_pair_match, call);
        if(found != NULL) active = linked_list_get_object(api_call_t, found);
    }

    if(active != NULL) {
        /* Продолжение: ARG уходит обработчику живого вызова, а дескриптор
         * повтора освобождается сразу. */
        uint8_t *arg = call->arg_data;
        uint32_t arg_len = call->arg_len;
        call->arg_data = NULL;
        api_call_release(core, call);
        api_invoke(core, active, arg, arg_len);
        api_unlock(core);
        free(arg);
        return;
    }

    api_handler_item_t *reg = api_handler_find(core, call->fid);
    if(reg == NULL) {
        api_peer_t *peer = call->peer;
        uint8_t has_sid = call->has_sid;
        uint32_t sid = call->sid;
        char fid[API_FID_NAME_MAX];
        mem_cpy(fid, call->fid, sizeof(fid));
        api_call_release(core, call);
        api_unlock(core);
        api_log(core, API_LOG_WARN, "No method for FID %s, replied NO_HANDLER", fid);
        api_send_frame(core, peer, api_frame_buffer_status(fid, has_sid, sid,
                                                           API_CALL_ERROR_STATUS_NO_HANDLER));
        return;
    }

    call->handler = reg->handler;
    call->context = reg->context;
    linked_list_insert_last(&core->executing, linked_list_item(call));

    /* ARG живёт до исполнения; дескриптор может исчезнуть внутри api_invoke,
     * поэтому освобождаем копию отдельным указателем — ровно один раз. */
    uint8_t *arg = call->arg_data;
    uint32_t arg_len = call->arg_len;
    call->arg_data = NULL;
    api_invoke(core, call, arg, arg_len);
    api_unlock(core);
    free(arg);
}

/* ── Снятие и пульс ───────────────────────────────────────────────────────── */

static uint8_t api_teardown_match(linked_list_item_t *item, void *arg) {
    (void)arg;
    return linked_list_get_object(api_call_t, item)->teardown;
}

/* Одно снятие за такт: обработчик получает call == NULL и возвращает FALSE,
 * когда освободил контекст; до этого вызов остаётся в списке. */
static void api_housekeeping(api_private_t *core) {
    api_lock(core);
    linked_list_item_t *item = linked_list_find_first(core->executing, api_teardown_match, NULL);
    if(item != NULL) {
        api_call_t *call = linked_list_get_object(api_call_t, item);
        api_unlock(core);
        uint8_t alive = call->handler(NULL, &call->context, NULL, 0);
        api_lock(core);
        if(!alive) {
            linked_list_unlink(linked_list_item(call));
            api_call_release(core, call);
        }
    }
    api_unlock(core);
}

/* Пульс: каждый активный вызов получает data == NULL — там метод делает своё
 * периодическое дело. FALSE означает «закончил». */
static void api_pulse(api_private_t *core) {
    api_lock(core);
    linked_list_item_t *item = linked_list_find_first(core->executing, NULL, NULL);
    while(item != NULL) {
        linked_list_item_t *next = linked_list_find_next_no_overlap(item, NULL, NULL);
        api_call_t *call = linked_list_get_object(api_call_t, item);
        if(!call->teardown) {
            uint8_t alive;
            api_unlock(core);
            alive = call->handler(call, &call->context, NULL, 0);
            api_lock(core);
            if(!alive) {
                linked_list_unlink(linked_list_item(call));
                api_call_release(core, call);
            }
        }
        item = next;
    }
    api_unlock(core);
}

/* Кадры, застрявшие из-за занятого транспорта, повторяются тактом. */
static void api_tx_retry(api_private_t *core) {
    api_lock(core);
    linked_list_item_t *item = linked_list_find_first(core->peers, NULL, NULL);
    while(item != NULL) {
        linked_list_item_t *next = linked_list_find_next_no_overlap(item, NULL, NULL);
        api_peer_t *peer = linked_list_get_object(api_peer_t, item);
        if(!peer->tx_busy && (peer->tx_frames != NULL)) api_peer_tx_action(core, peer);
        item = next;
    }
    api_unlock(core);
}

/* Входящие кадры: разбираются и отдаются обработчикам. */
static void api_incoming_drain(api_private_t *core) {
    for(;;) {
        api_lock(core);
        linked_list_item_t *item = linked_list_find_first(core->incoming, NULL, NULL);
        if(item == NULL) {
            api_unlock(core);
            return;
        }
        api_call_t *call = linked_list_get_object(api_call_t, item);
        linked_list_unlink(item);
        api_unlock(core);
        api_dispatch(core, call);
    }
}

/* ── Отправка кадров: контракт §3.7 ───────────────────────────────────────── */

uint8_t api_call_send_status(void *call_handle, uint32_t sta) {
    api_call_t *call = (api_call_t *)call_handle;
    if((call == NULL) || (call->peer == NULL)) return HDL_FALSE;
    api_private_t *core = api_private(call->api);
    return api_send_frame(core, call->peer,
                          api_frame_buffer_status(call->fid, call->has_sid, call->sid, sta));
}

uint8_t api_call_send_arg(void *call_handle, const char *arg, uint32_t len) {
    api_call_t *call = (api_call_t *)call_handle;
    if((call == NULL) || (call->peer == NULL)) return HDL_FALSE;
    api_private_t *core = api_private(call->api);
    return api_send_frame(core, call->peer,
                          api_frame_buffer_json(call->fid, call->has_sid, call->sid, arg, len));
}

char *api_call_begin_response(uint32_t arg_len) {
    if(arg_len == 0) return NULL;
    api_frame_buffer_t *fb = api_frame_buffer_new(arg_len + API_FRAME_OVERHEAD + API_FID_NAME_MAX);
    if(fb == NULL) return NULL;
    fb->len = sprintf(fb->buf, "{" API_ARG_PROP);
    return fb->buf + fb->len;
}

void api_call_cancel_response(char *resp) {
    if(resp != NULL) free(API_RESPONSE_TO_FB(resp));
}

uint8_t api_call_send_response(void *call_handle, char *resp, uint32_t arg_len) {
    api_call_t *call = (api_call_t *)call_handle;
    if((call == NULL) || (resp == NULL) || (arg_len == 0) || (call->peer == NULL))
        return HDL_FALSE;
    api_frame_buffer_t *fb = API_RESPONSE_TO_FB(resp);
    fb->len += (int32_t)arg_len;
    if(call->has_sid)
        fb->len += sprintf(fb->buf + fb->len, ",\"FID\":\"%s\",\"SID\":\"0x%08lx\"}",
                           call->fid, (unsigned long)call->sid);
    else
        fb->len += sprintf(fb->buf + fb->len, ",\"FID\":\"%s\"}", call->fid);
    api_private_t *core = api_private(call->api);
    return api_send_frame(core, call->peer, fb);
}

/* Рассылка: один буфер уходит всем активным вызовам имени, без SID. */
typedef struct {
    api_private_t *core;
    const char *fid;
    api_frame_buffer_t *fb;
    uint32_t sent;                    /* сколько получателей приняло кадр */
} group_send_t;

static void api_group_send_action(linked_list_item_t *item, void *arg) {
    group_send_t *g = (group_send_t *)arg;
    api_call_t *call = linked_list_get_object(api_call_t, item);
    if(call->teardown || (strcmp(call->fid, g->fid) != 0)) return;
    if(api_send_frame(g->core, call->peer, g->fb)) g->sent++;
}

static uint8_t api_group_send(api_private_t *core, const char *fid, api_frame_buffer_t *fb) {
    if(fb == NULL) return HDL_FALSE;
    group_send_t g = { core, fid, fb, 0 };
    fb->holders++;                        /* наша ссылка: отдадим её в конце */
    api_lock(core);
    linked_list_item_t *item = linked_list_find_first(core->executing, NULL, NULL);
    while(item != NULL) {
        linked_list_item_t *next = linked_list_find_next_no_overlap(item, NULL, NULL);
        api_group_send_action(item, &g);
        item = next;
    }
    api_unlock(core);
    api_buffer_release(fb);               /* получатели держат свои ссылки */
    return g.sent ? HDL_TRUE : HDL_FALSE;
}

uint8_t api_call_group_fid_data(api_t *api, const char *fid, const char *data, uint32_t len) {
    if((api == NULL) || (fid == NULL) || !fid[0]) return HDL_FALSE;
    api_private_t *core = api_private(api);
    return api_group_send(core, fid, api_frame_buffer_json(fid, 0, 0, data, len));
}

uint8_t api_call_group_response(api_t *api, const char *fid, void *resp, uint32_t arg_len) {
    if((api == NULL) || (fid == NULL) || !fid[0] || (resp == NULL) || (arg_len == 0))
        return HDL_FALSE;
    api_private_t *core = api_private(api);
    api_frame_buffer_t *fb = API_RESPONSE_TO_FB(resp);
    fb->len += (int32_t)arg_len;
    fb->len += sprintf(fb->buf + fb->len, ",\"FID\":\"%s\"}", fid);
    return api_group_send(core, fid, fb);
}

/* ── События транспорта ───────────────────────────────────────────────────── */

static void api_calls_mark_teardown_action(linked_list_item_t *item, void *arg) {
    api_call_t *call = linked_list_get_object(api_call_t, item);
    if(call->peer == arg) call->teardown = 1;
}

static void api_incoming_drop_action(linked_list_item_t *item, void *arg) {
    api_call_t *call = linked_list_get_object(api_call_t, item);
    if(call->peer != arg) return;
    linked_list_unlink(item);
    api_call_release(api_private(call->api), call);
}

/* Соединения нет: пир помечается и передаётся уборке (обработчиков здесь не
 * зовём — событие приходит из чужой задачи). Вызовы помечаются на снятие,
 * неразобранные кадры освобождаются сразу. */
static void api_peer_close(api_private_t *core, const void *conn) {
    api_lock(core);
    api_peer_t *peer = api_peer_find(core, conn);
    if(peer != NULL) {
        peer->closed = 1;
        linked_list_unlink(linked_list_item(peer));
        linked_list_insert_last(&core->dead_peers, linked_list_item(peer));
        linked_list_do_foreach(core->executing, api_calls_mark_teardown_action, peer);
        linked_list_do_foreach(core->incoming, api_incoming_drop_action, peer);
    }
    api_unlock(core);
    if(peer == NULL) api_log(core, API_LOG_DEBUG, "Closed unknown connection %p", conn);
}

static void api_transfer_done(api_private_t *core, const void *conn) {
    api_lock(core);
    api_peer_t *peer = api_peer_find(core, conn);
    if((peer != NULL) && peer->tx_busy) {
        peer->tx_busy = 0;
        api_tx_drop_head(peer);
        api_peer_tx_action(core, peer);
    }
    api_unlock(core);
}

/* Разбор кадра клиента: конверта FID/SID/ARG, без дерева значений. */
static void api_receive(api_private_t *core, api_peer_t *peer, const void *data, uint32_t len) {
    if((data == NULL) || (len == 0)) {
        api_log(core, API_LOG_INFO, "Empty message dropped");
        return;
    }

    GW_JSON_TOKENS(toks, API_FRAME_MAX_TOKENS);
    gw_json_t json;
    if(!gw_json_parse(&json, (const char *)data, len, toks, API_FRAME_MAX_TOKENS)) {
        api_log(core, API_LOG_WARN, "Frame is not JSON, replied BAD_REQ");
        api_send_frame(core, peer, api_frame_buffer_status("", 0, 0,
                                                           API_CALL_ERROR_STATUS_BAD_REQ));
        return;
    }

    char fid[API_FID_NAME_MAX * 2];
    if(!gw_json_str(&json, "FID", fid, sizeof(fid)) || !fid[0]) {
        api_log(core, API_LOG_WARN, "Frame has no FID, replied NO_FID");
        api_send_frame(core, peer, api_frame_buffer_status("", 0, 0,
                                                           API_CALL_ERROR_STATUS_NO_FID));
        return;
    }
    if(strlen(fid) >= API_FID_NAME_MAX) {
        api_log(core, API_LOG_WARN, "FID too long, replied NO_FID");
        api_send_frame(core, peer, api_frame_buffer_status("", 0, 0,
                                                           API_CALL_ERROR_STATUS_NO_FID));
        return;
    }

    int32_t sid = 0;
    uint8_t has_sid = gw_json_int(&json, "SID", &sid) ? 1 : 0;

    const char *arg = NULL;
    size_t arg_len = 0;
    gw_json_raw(&json, "ARG", &arg, &arg_len);

    api_lock(core);
    api_call_t *call = api_call_alloc(core);
    if(call == NULL) {
        api_unlock(core);
        api_log(core, API_LOG_WARN, "No free call descriptors, replied NO_FREE_DESCRIPTORS");
        api_send_frame(core, peer, api_frame_buffer_status(fid, has_sid, (uint32_t)sid,
                                                           API_CALL_ERROR_STATUS_NO_FREE_DESCRIPTORS));
        return;
    }
    call->api     = (api_t *)(void *)core;
    call->peer    = peer;
    call->has_sid = has_sid;
    call->sid     = (uint32_t)sid;
    peer->calls++;
    mem_cpy(call->fid, fid, strlen(fid) + 1);

    if((arg != NULL) && (arg_len > 0)) {
        uint8_t *copy = malloc(arg_len);
        if(copy == NULL) {
            api_call_release(core, call);
            api_unlock(core);
            api_send_frame(core, peer, api_frame_buffer_status(fid, has_sid, (uint32_t)sid,
                                                               API_CALL_ERROR_STATUS_NO_MEM));
            return;
        }
        mem_cpy(copy, arg, arg_len);
        call->arg_data = copy;
        call->arg_len  = (uint32_t)arg_len;
    }
    linked_list_insert_last(&core->incoming, linked_list_item(call));
    api_unlock(core);
}

static void api_transport_event(void *event_trigger, void *sender, void *context) {
    if((event_trigger == NULL) || (sender == NULL) || (context == NULL)) return;
    api_private_t *core = api_private((const api_t *)context);
    const transport_event_trigger_t *ev = (const transport_event_trigger_t *)event_trigger;

    switch(ev->event) {
    case TRANSPORT_ESTABLISHED:
        api_lock(core);
        if(api_peer_find(core, sender) == NULL) {
            if(api_peer_create(core, sender) == NULL) {
                api_unlock(core);
                api_log(core, API_LOG_WARN, "No memory for a peer");
                transport_t tr = api_transport_of(core, sender);
                transport_close(&tr);
                return;
            }
        }
        api_unlock(core);
        api_log(core, API_LOG_INFO, "Peer up, conn=%p", sender);
        return;

    case TRANSPORT_CLOSED:
        api_peer_close(core, sender);
        return;

    case TRANSPORT_RECEIVED: {
        api_lock(core);
        api_peer_t *peer = api_peer_find(core, sender);
        api_unlock(core);
        if(peer == NULL) {
            api_log(core, API_LOG_WARN, "Message for unknown connection %p, dropped", sender);
            return;
        }
        api_receive(core, peer, ev->data.ptr, ev->data.length);
        return;
    }

    case TRANSPORT_TRANSFER_COMPLETE:
    case TRANSPORT_TRANSFER_ERR:
        api_transfer_done(core, sender);
        return;

    default:
        return;
    }
}

/* ── Такт работы ──────────────────────────────────────────────────────────── */

void api_worker(api_t *api) {
    if(api == NULL) return;
    api_private_t *core = api_private(api);

    api_lock(core);                       /* состояние читается под замком всегда */
    api_unlock(core);

    api_peers_reap(core);
    api_housekeeping(core);

    uint32_t now = api_time(core);
    if((now - core->pulse_timer) >= API_PULSE_INTERVAL_MS) {
        core->pulse_timer = now;
        api_pulse(core);
    }
    api_tx_retry(core);
    api_incoming_drain(core);
}

/* ── Подъём и снятие ─────────────────────────────────────────────────────── */

api_t *api_init(const transport_t *transport, void *transport_config,
                const api_system_t *system) {
    const transport_iface_t *iface = (transport != NULL) ? transport->iface : NULL;
    if((iface == NULL) || (iface->begin_send == NULL) ||
       (iface->close == NULL) || (iface->subscribe == NULL) ||
       (iface->open == NULL)) return NULL;
    if((system == NULL) || (system->mutex_create == NULL) ||
       (system->mutex_take == NULL) || (system->mutex_give == NULL) ||
       (system->log == NULL) || (system->time_ms == NULL)) return NULL;

    api_t *api = calloc(1, sizeof(*api));
    if(api == NULL) return NULL;
    api_private_t *core = api_private(api);
    core->transport = transport;
    core->system    = system;

    core->mutex = system->mutex_create(system->context);
    if(core->mutex == NULL) {
        free(api);
        return NULL;
    }

    for(uint32_t i = 0; i < API_CALL_POOL_SIZE; i++)
        linked_list_insert_last(&core->free_calls, linked_list_item(&core->pool[i]));

    core->delegate.handler = api_transport_event;
    core->delegate.context = api;
    transport_result_t res = transport_subscribe(transport, &core->delegate);
    if(res != TRANSPORT_OK) {
        api_log(core, API_LOG_WARN, "Transport subscribe failed: %d", (int)res);
        free(api);
        return NULL;
    }

    /* Транспорт открывает ядро: подписка уже на месте, первое соединение будет
     * кому отдать. */
    res = transport_open(transport, transport_config);
    if(res != TRANSPORT_OK) {
        api_log(core, API_LOG_WARN, "Transport open failed: %d", (int)res);
        event_unsubscribe(&core->delegate);
        free(api);
        return NULL;
    }

    api_log(core, API_LOG_INFO, "API core up, transport=%p", transport->context);
    return api;
}

static void api_free_list(api_private_t *core, linked_list_t *list) {
    linked_list_item_t *item;
    while((item = linked_list_find_first(*list, NULL, NULL)) != NULL) {
        api_call_t *call = linked_list_get_object(api_call_t, item);
        linked_list_unlink(item);
        if(core->system != NULL && call->handler != NULL && call->peer != NULL)
            call->handler(NULL, &call->context, NULL, 0);
        free(call->arg_data);
        call->arg_data = NULL;
    }
}

void api_deinit(api_t *api) {
    if(api == NULL) return;
    api_private_t *core = api_private(api);

    /* Сначала отписка: транспорт не должен остаться с обработчиком, смотрящим
     * на освобождаемое ядро. */
    event_unsubscribe(&core->delegate);

    /* Обработчикам даём шанс освободить состояние: вызов исчезает вместе с
     * ядром, и «call == NULL» — единственный способ об этом сказать. */
    linked_list_item_t *item = linked_list_find_first(core->executing, NULL, NULL);
    while(item != NULL) {
        linked_list_item_t *next = linked_list_find_next_no_overlap(item, NULL, NULL);
        api_call_t *call = linked_list_get_object(api_call_t, item);
        if(call->handler != NULL) call->handler(NULL, &call->context, NULL, 0);
        linked_list_unlink(item);
        free(call->arg_data);
        call->arg_data = NULL;
        item = next;
    }
    api_free_list(core, &core->incoming);
    api_free_list(core, &core->free_calls);

    while((item = linked_list_find_first(core->handlers, NULL, NULL)) != NULL) {
        api_handler_item_t *h = linked_list_get_object(api_handler_item_t, item);
        linked_list_unlink(item);
        free(h);
    }
    while((item = linked_list_find_first(core->peers, NULL, NULL)) != NULL) {
        api_peer_destroy(core, linked_list_get_object(api_peer_t, item));
    }
    while((item = linked_list_find_first(core->dead_peers, NULL, NULL)) != NULL) {
        api_peer_destroy(core, linked_list_get_object(api_peer_t, item));
    }
    free(api);
}
