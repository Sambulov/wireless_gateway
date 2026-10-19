#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

#include "GwJson.h"
#include "CodeLib.h"
#include "api.h"

static const char *TAG = "api";

/* Скопировать имя FID в буфер вызова. 0 — имя пустое или длиннее предела: такое
 * имя обработчику отдавать нельзя, иначе оно молча обрежется и вызов уйдёт «не
 * тому», кто его ждал. Хелпер живёт здесь, а не в api.h: снаружи он не нужен
 * никому — имя FID проверяет само ядро при регистрации. */
static uint8_t api_fid_set(char *dst, const char *name) {
    if(!dst || !name || !name[0]) return 0;
    size_t n = 0;
    while(name[n] && n < API_FID_NAME_MAX - 1) n++;
    if(name[n] != '\0') return 0;
    for(size_t i = 0; i < n; i++) dst[i] = name[i];
    dst[n] = '\0';
    return 1;
}

typedef struct {
    __LinkedListObject__
    api_handler_t handler;
    char fid[API_FID_NAME_MAX];
    void *handler_ctx;
} api_handler_item_t;

/* Сессия ядра — соединение транспорта, к которому привязаны вызовы. Дескриптор
 * соединения (self) для ядра непрозрачен: это ключ, который транспорт вернёт в
 * событиях, и больше ничего — тип соединения транспорта ядру неизвестен. */
typedef struct {
    __LinkedListObject__
    const void *self;
    /* Числовой идентификатор соединения, который даёт транспорт (для сокетного
     * транспорта — номер сокета). Ядро берёт его только ради журнала: по нему в
     * строках ядра видно, о каком соединении речь. Адресатом ответа он не
     * служит — адресат это дескриптор вызова. */
    int peer_id;
} api_session_t;

typedef enum
{
    CALL_FLAG_NEW       = (1 << 0),
    CALL_FLAG_LONG_TERM = (1 << 2),
    CALL_FLAG_TO_DELETE = (1 << 3)
} call_flags_t;

/* Вызов клиента. Ядро помнит СВОЁ ядро: ответ и статус адресованы вызову, а
 * берутся из него же (api_reply не передают дескриптор ядра). */
typedef struct {
    __LinkedListObject__
    api_t *api;
    api_session_t *session;
    uint8_t *req_data;
    uint32_t req_data_len;
    uint32_t id;
    /* SID назначает клиент: он необязателен, и сервер лишь возвращает его в
     * ответе. Без него ответ уходит без поля SID (публичные подписки). */
    uint8_t  has_sid;
    void *handler_ctx;
    api_handler_t handler;
    char fid[API_FID_NAME_MAX];
    /* Обработчику уже отдали первичный запрос: только после этого его можно
     * дёргать тактом работы, иначе он получит data == NULL раньше запроса. */
    uint8_t  handler_started;
    call_flags_t flags;
    /* Темп переспроса подписки: ARG.RDL клиента (мс) и момент, раньше которого
     * вызов не отдают обработчику снова. У вызовов без RDL оба нуля — опрос
     * идёт сразу. */
    uint32_t poll_interval_ms;
    uint32_t next_poll_ms;
} api_call_t;

/* Блок ответа. Счётчик получателей лежит ПЕРЕД полезной нагрузкой не случайно:
 * транспорт в событии завершения возвращает ровно тот указатель, который получил
 * в begin_send (то есть payload), — по нему и находится счётчик. Кадр ответа
 * собирает транспорт, ядру о кадрах знать нечего. */
typedef struct {
    uint32_t counter;
    uint32_t len;
    uint8_t  payload[1];
} api_data_t;

/* Из указателя, вернувшегося в событии завершения, — к своему блоку ответа. */
static api_data_t *api_data_from_payload(const void *payload) {
    return (api_data_t *)(uintptr_t)((const uint8_t *)payload - offsetof(api_data_t, payload));
}

/* Размер сборки обработчиков такта.
 *
 * Обработчиков нельзя звать под мьютексом (см. api_work_cycle), поэтому их
 * указатели сначала собираются в пачку, а дёргаются уже без мьютекса. Пачка —
 * состояние ядра: освобождать вызовы может лишь та же задача, что их собрала,
 * так что собранные указатели остаются валидными. */
#define API_WORK_BATCH 16

/* ── Состояние одного ядра ──────────────────────────────────────────────────
 *
 * Всё состояние ядра живёт в дескрипторе, который выделяет и освобождает само
 * ядро (api_init / api_deinit). Наружу дескриптор отдаётся неполным типом api_t
 * (см. api.h), а настоящая раскладка — здесь.
 *
 * Всё состояние под ОДНИМ мьютексом: списки вызовов, регистраций и сессий
 * связаны ссылками (вызов → сессия), и трогать их порознь нельзя. Мьютекс —
 * единственная синхронизация ядра: ни очередей, ни ожиданий у него нет. */
typedef struct api_private_s {
    /* Объект транспорта, к которому подключено ядро (api_init): методы и контекст
     * реализации — он же идентификатор экземпляра. Дальше ядро работает только
     * через него — обращений к платформе в ядре нет, всё платформенное живёт в
     * транспорте. Один объект на ядро: «методы от одного экземпляра, контекст от
     * другого» невозможны. */
    const transport_t *transport;
    /* Подписка ядра — делегат CodeLibrary, а не пара «обработчик + контекст» в
     * транспорте. Живёт в дескрипторе, потому что принадлежит ЯДРУ: context
     * делегата — сам дескриптор, handler — api_transport_event, а снимает подписку
     * ядро само (event_unsubscribe в api_deinit). Транспорту для этого ничего не
     * нужно, и «висячей» подписки на освобождённое ядро не остаётся. */
    delegate_t delegate;

    /* Система — три операции и её контекст, которые дал вызывающий (api_init).
     * Своей задачи, своей очереди и своих задержек у ядра нет: их отсутствие и
     * есть причина, по которой таблица нужна целиком, а не по частям. */
    const api_system_t *system;
    /* Мьютекс состояния. Создаёт его система (api_system_t.mutex_create), тип для
     * ядра непрозрачен: ядро только берёт и отпускает. */
    void *mutex;

    /* Регистрации обработчиков; вызовы, отданные обработчику (он же и есть
     * периферия); вызовы, которые он ещё не завершил, — тот же список; сессии
     * соединений. Доставка запроса идёт только вызовом обработчика, отдельного
     * списка очередей у ядра нет. */
    LinkedList_t handlers;
    LinkedList_t calls;
    LinkedList_t waiting;
    LinkedList_t sessions;

    /* Момент последнего такта работы: такт ограничен по частоте (см.
     * API_WORK_PERIOD_MS в api.h) — вызывающий вправе звать api_worker чаще,
     * чем обработчику есть что делать. */
    uint32_t last_work_tick;

    /* Сбор вызовов на такт работы (см. api_collect_handled / api_collect_finish):
     * обработчиков нельзя звать под мьютексом, поэтому сначала собираем их
     * указатели, а дёргаем уже без мьютекса. */
    api_call_t *work_batch[API_WORK_BATCH];
    api_call_t *finish_batch[API_WORK_BATCH];
} api_private_t;

/* Дескриптор ядра — ровно это состояние и ничего сверх него. Наружу он отдаётся
 * как api_t (неполный тип), поэтому «сколько байт» и «какая раскладка» остаются
 * внутренним делом ядра. */
struct api_s {
    api_private_t core;
};

/* Приведение дескриптора к настоящему состоянию. Поле core — единственное и
 * лежит в начале, поэтому указатели совпадают: вызов обработчика события
 * получает ровно тот api_t, который вернул api_init. Обратный переход (состояние
 * → дескриптор) — тот же адрес, и он нужен ровно в одном месте: при создании
 * вызова (см. api_session_on_message). */
static api_private_t *api_private(const api_t *api) {
    return (api_private_t *)(void *)api;
}

/* Методы и контекст транспорта ВСЕГДА берутся из одного объекта — транспорт
 * приходит в api_init одним объектом, и половины от разных экземпляров
 * разойтись не могут. */
static const transport_iface_t *api_transport_iface(const api_private_t *core) {
    return (core->transport != NULL) ? core->transport->iface : NULL;
}

static void *transport_self(const api_private_t *core) {
    return (core->transport != NULL) ? core->transport->context : NULL;
}

/* ── Система ────────────────────────────────────────────────────────────────
 *
 * Ядро берёт системное ТОЛЬКО из таблицы, и ниже — все места, где оно к ней
 * обращается. Так видно, что ядру на самом деле нужно от системы (мьютекс,
 * время, журнал), и не расползаются системные вызовы: ни в этом файле, ни в
 * api.h нет ни FreeRTOS, ни ESP-IDF, ни platform.h.
 *
 * Строка журнала собирается ЗДЕСЬ, а таблица принимает varargs — поэтому
 * реализация системы не обязана уметь работать с va_list. Длинный текст
 * режется по размеру буфера. */
static void api_log(const api_private_t *core, int level, const char *fmt, ...) {
    if((core == NULL) || (core->system == NULL) || (core->system->log == NULL)) return;
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    core->system->log(core->system->context, level, TAG, "%s", buf);
}

/* Мьютекс ядра. Система отдаёт управление уже с взятым мьютексом (в прошивке —
 * без предела ожидания), поэтому спрашивать «взялся ли» нечего: «не взялся» —
 * ошибка системы, разбирать её ядру нечем. */
static void api_lock(api_private_t *core) {
    core->system->mutex_take(core->system->context, core->mutex);
}

static void api_unlock(api_private_t *core) {
    core->system->mutex_give(core->system->context, core->mutex);
}

/* Время — в миллисекундах, как и ARG.RDL у подписки: пересчитывать тики не
 * нужно, система отдаёт их сама. Чтение времени — не ожидание: ядро только
 * сравнивает разности, а «подождать» для него значит вернуть управление. */
static uint32_t api_time(const api_private_t *core) {
    return core->system->time_ms(core->system->context);
}

/* У ядра нет файловых переменных состояния: всё, что ядро помнит, лежит в его
 * дескрипторе. Стоит здесь завести любую static-переменную с состоянием — и два
 * ядра (точка доступа и станция) начнут её делить, то есть перестанут быть
 * независимыми. */

/* --- Соединения ---------------------------------------------------------- */

/* Поиск сессии по дескриптору соединения. Дескриптор сравнивается как ключ:
 * ядро в него не заглядывает, тип соединения транспорта ему неизвестен. */
static api_session_t *api_session_find(api_private_t *core, const void *self) {
    LinkedListItem_t *item = pxLinkedListFindFirst(core->sessions, NULL, NULL);
    while(item != NULL) {
        api_session_t *sess = LinkedListGetObject(api_session_t, item);
        if(sess->self == self) return sess;
        item = pxLinkedListFindNextNoOverlap(item, NULL, NULL);
    }
    return NULL;
}

static void api_break_call_by_session(LinkedListItem_t *item, void *arg) {
    api_session_t *session = (api_session_t *)arg;
    api_call_t *call = LinkedListGetObject(api_call_t, item);
    if(call->session == session) {
        api_log(api_private(call->api), API_LOG_INFO, "Break call id:%lu", call->id);
        call->session = NULL;
    }
}

/* Вызовы снятого соединения: клиент ушёл — отвечать некому. Сами вызовы
 * освободит такт работы, и лишь после FALSE обработчика. */
static void api_break_calls_by_session(api_private_t *core, api_session_t *sess) {
    ulLinkedListDoForeach(core->calls, api_break_call_by_session, sess);
    ulLinkedListDoForeach(core->waiting, api_break_call_by_session, sess);
}

static uint8_t handler_fid_match(LinkedListItem_t *item, void *arg) {
    const char *name = (const char *)arg;
    api_handler_item_t *hd = LinkedListGetObject(api_handler_item_t, item);
    return (name != NULL) && (strcmp(hd->fid, name) == 0);
}

/* Регистрация этого имени на этом ядре. NULL — сервис на этом ядре не
 * зарегистрирован, и вызов получит NO_HANDLER: на другом ядре то же имя может
 * быть зарегистрировано свободно (оно обслуживает другой транспорт). */
static api_handler_item_t *api_handler_find(api_private_t *core, const char *name) {
    LinkedListItem_t *item = pxLinkedListFindFirst(core->handlers, handler_fid_match, (void *)name);
    return item ? LinkedListGetObject(api_handler_item_t, item) : NULL;
}

/* Ключ снятия вызова: соединение + FID + SID. Совпадения только по FID и SID
 * мало — у одного клиента бывает несколько вызовов одного FID с разными SID, и
 * снятие одного не должно задевать соседний. */
typedef struct {
    api_session_t *session;
    const char   *fid;
    uint32_t      id;
} delete_key_t;

static uint8_t call_delete_match(LinkedListItem_t *item, void *arg) {
    const delete_key_t *key = (const delete_key_t *)arg;
    api_call_t *call = LinkedListGetObject(api_call_t, item);
    return (call->session == key->session)
        && (call->id == key->id)
        && (strcmp(call->fid, key->fid) == 0);
}

/* Ищем вызов в обоих списках: он бывает и в работе (ждёт такта переспроса), и в
 * ожидании ответа периферии. */
static api_call_t *api_call_find_to_delete(api_private_t *core, api_session_t *sess,
                                           const char *fid, uint32_t id) {
    delete_key_t key = { sess, fid, id };
    LinkedListItem_t *item = pxLinkedListFindFirst(core->calls, call_delete_match, &key);
    if(!item)
        item = pxLinkedListFindFirst(core->waiting, call_delete_match, &key);
    return item ? LinkedListGetObject(api_call_t, item) : NULL;
}

/* Рассылка по имени FID: получатель — живой (с соединением) вызов этого FID
 * на этом ядре. Сервис зарегистрирован на конкретном ядре, и подписчик с этого
 * ядра получает данные этого ядра.
 *
 * Вызов и не может появиться у соединения, которому сервис не оказан (такой
 * запрос получил бы NO_HANDLER), — проверка держит это свойство явным и не
 * полагается на то, что вызов уже проверили при создании. */
static uint8_t call_fid_match(LinkedListItem_t *item, void *arg) {
    const char *name = (const char *)arg;
    api_call_t *call = LinkedListGetObject(api_call_t, item);
    if((name == NULL) || (call->session == NULL)) return 0;
    return strcmp(call->fid, name) == 0;
}

/* Объявлено заранее: завершение вызова зовётся из обслуживания, описано ниже. */
static uint8_t api_call_teardown(api_call_t *call);
/* Отправка статуса — внутренняя: снаружи статус отправлять нечем и незачем
 * (см. api.h), а ядро само закрывает приём, разбор и отсутствие обработчика. */
static uint8_t api_send_status(api_call_t *call, uint32_t status);

/* Такт обслуживания вызова. Ядро не пингует клиента: управляющие кадры — дело
 * транспорта, а живость соединения видна по событиям TRANSFER_ERR (отправка не
 * прошла) и CLOSED (соединение разорвано). */
static void api_serve_call(LinkedListItem_t *item, void *arg) {
    (void)arg;
    api_call_t *call = LinkedListGetObject(api_call_t, item);
    if(call->session) return;   /* соединение живо — делать нечего */

    /* Клиента нет (отключился или ответ уже отправлен): просим снять вызов.
     * Сам вызов освободит такт работы — и только после того, как обработчик
     * вернёт FALSE. */
    api_log(api_private(call->api), API_LOG_DEBUG, "Api call complete, id: %lu", call->id);
    call->flags |= CALL_FLAG_TO_DELETE;
}

/* Принудительное завершение вызова: по контракту обработчик получает call == NULL
 * и обязан освободить свой контекст, возвращая FALSE.
 *
 * ЯДРО НЕ СНИМАЕТ ВЫЗОВ, ПОКА ОБРАБОТЧИК НЕ ВЕРНУЛ РЕЗУЛЬТАТ. Одна попытка за
 * такт: если обработчик завершается пошагово (вернул TRUE), вызов остаётся в
 * списках и его спросят снова следующим тактом — освобождать память вызова,
 * пока обработчик ещё держит своё состояние, нельзя. */
static uint8_t api_call_teardown(api_call_t *call) {
    if(!call || !call->handler || !call->handler_started) return 1;  /* завершать нечего */
    return call->handler(NULL, &call->handler_ctx, NULL, 0) ? 0 : 1;
    /* handler_ctx НЕ обнуляем: обработчик сам решает, что там оставить.
     * Для подписки (FLAGS:4) вызов после завершения живёт дальше и при
     * следующем переопросе должен снова получить свой контекст регистрации —
     * иначе он не знает, какой периферией управляет. */
}

uint8_t api_register_handler(api_t *api, api_handler_t handler, const char *name,
                             void *context) {
    if(api == NULL) return 0;
    api_private_t *core = api_private(api);
    char fid[API_FID_NAME_MAX];
    /* Имя обязательно: «нет FID» — это пустая строка или NULL, и оба случая
     * регистрацией не считаются. */
    if((handler == NULL) || !api_fid_set(fid, name)) return 0;
    api_lock(core);

    /* Имя на этом ядре уже занято — вторая такая же регистрация неразличима.
     * На ДРУГОМ ядре то же имя регистрируется свободно: там свой транспорт и
     * свой контекст (см. api.h). */
    api_handler_item_t *registered = api_handler_find(core, fid);
    if(registered != NULL) {
        api_unlock(core);
        return 0;
    }
    api_handler_item_t *hd = malloc(sizeof(api_handler_item_t));
    if(hd == NULL) {
        api_unlock(core);
        return 0;
    }
    hd->handler = handler;
    mem_cpy(hd->fid, fid, sizeof(hd->fid));
    hd->handler_ctx = context;
    vLinkedListInsertLast(&core->handlers, LinkedListItem(hd));
    api_log(core, API_LOG_INFO, "Api handler registered %s", hd->fid);
    api_unlock(core);
    return 1;
}

/* Отправка через транспорт: ядро отдаёт СООБЩЕНИЕ, кадры строит транспорт.
 * Результат отправки приходит событием TRANSFER_COMPLETE/TRANSFER_ERR, поэтому
 * блок ответа живёт до него (в событии транспорт вернёт тот же указатель). */
static void send_to_session(api_private_t *core, api_call_t *sub, api_data_t *resp,
                            transport_result_t *res) {
    /* Операции СОЕДИНЕНИЯ получают дескриптор соединения (self), а методы — из
     * объекта транспорта этого ядра (см. api_transport_iface): «методы одного
     * экземпляра, соединение другого» невозможно. */
    *res = api_transport_iface(core)->begin_send(sub->session->self, resp->payload, resp->len);
    api_log(core, API_LOG_DEBUG, "Api call %lu json sending with result: %d", sub->id, *res);
    if(*res != TRANSPORT_OK) {
        /* Транспорт отправку не принял — события не будет, освобождаем сами. */
        if((!resp->counter) || !(--resp->counter))
            free(resp);
    }
}

/* Ответ конкретному вызову (call != NULL) или рассылка всем вызовам с этим
 * именем (call == NULL, задано имя). Смешивать нельзя: вызов уже знает своё имя,
 * а рассылка — нет. */
static uint8_t api_dispatch(api_private_t *core, api_call_t *call,
                                const char *group_name, const uint8_t *json,
                                uint32_t data_len) {
    if(((call == NULL) && ((group_name == NULL) || !group_name[0])) ||
       ((call != NULL) && (group_name != NULL))) return 0;

    if(core == NULL) return 0;

    transport_result_t res = TRANSPORT_INVALID;
    const char *fid_out = (call != NULL) ? call->fid : group_name;
    uint32_t sid = (call != NULL) ? call->id : 0;

    api_lock(core);

    uint32_t targets;
    if(call != NULL) {
        targets = (call->session != NULL) ? 1 : 0;
    } else {
        targets = ulLinkedListCount(core->waiting, call_fid_match, (void *)group_name)
                + ulLinkedListCount(core->calls, call_fid_match, (void *)group_name);
    }

    if(targets == 0) {
        /* Рассылать некому: подписчиков этого FID сейчас нет (вызов снят,
         * соединение закрыто или подписка ещё не зарегистрирована). Молчать об
         * этом нельзя — снаружи это выглядит как «данные пропали». */
        api_log(core, API_LOG_INFO, "broadcast %s: подписчиков нет, кадр (%lu Б) не отправлен",
                group_name ? group_name : "-", (unsigned long)data_len);
        api_unlock(core);
        return 0;
    }

    /* SID в ответе — только если клиент его назначил. Публичные подписки
     * (рассылка по FID без конкретного вызова) идут без SID всегда. */
    uint8_t has_sid = (call != NULL) && call->has_sid;
    /* В ответе — то же имя, что прислал клиент. Шаблоны с запасом под имя и
     * SID, длину берём по факту ниже. */
    char template[] = "{\"FID\":\"%s\",\"SID\":\"0x%08lx\",\"ARG\":";
    char template_no_sid[] = "{\"FID\":\"%s\",\"ARG\":";
    uint32_t cap = data_len + sizeof(template) + sizeof(template_no_sid)
                   + 2 * API_FID_NAME_MAX + 16;

    api_data_t *resp = malloc(sizeof(api_data_t) + cap);
    if(resp != NULL) {
        int offset = has_sid
                   ? sprintf((char *)resp->payload, template, fid_out, (unsigned long)sid)
                   : sprintf((char *)resp->payload, template_no_sid, fid_out);
        mem_cpy(&resp->payload[offset], json, data_len);
        offset += data_len;
        resp->payload[offset] = '}';
        resp->counter        = targets;
        /* Длину берём по факту: шаблон без SID короче, и объявлять сообщение по
         * длинному шаблону нельзя — клиент получит мусор в хвосте. */
        resp->len            = (uint32_t)offset + 1;

        if(call != NULL) {
            send_to_session(core, call, resp, &res);
        } else {
            LinkedListItem_t *item = pxLinkedListFindFirst(core->waiting, call_fid_match, (void *)group_name);
            while(item != NULL) {
                api_call_t *sub = LinkedListGetObject(api_call_t, item);
                item = pxLinkedListFindNextNoOverlap(item, call_fid_match, (void *)group_name);
                send_to_session(core, sub, resp, &res);
            }
            item = pxLinkedListFindFirst(core->calls, call_fid_match, (void *)group_name);
            while(item != NULL) {
                api_call_t *sub = LinkedListGetObject(api_call_t, item);
                item = pxLinkedListFindNextNoOverlap(item, call_fid_match, (void *)group_name);
                send_to_session(core, sub, resp, &res);
            }
        }
    } else {
        res = TRANSPORT_FAULT;
    }

    api_unlock(core);
    return (res == TRANSPORT_OK);
}

/* Ответ конкретному вызову. Ядро берётся из самого вызова: отвечающему (в том
 * числе тому, кто ответил позже из своего потока) знать его не нужно. */
uint8_t api_reply(void *call_handle, const uint8_t *json, uint32_t len) {
    api_call_t *call = (api_call_t *)call_handle;
    if((call == NULL) || (call->api == NULL)) return 0;
    return api_dispatch(api_private(call->api), call, NULL, json, len);
}

/* Статус клиенту: короткий фрагмент ARG, который собирает само ядро. Наружу
 * эта функция не выходит — снаружи статус отправлять нечем (см. api.h), и
 * вызывают её только места ядра, закрывающие приём, разбор и отмену вызова. */
static uint8_t api_send_status(api_call_t *call, uint32_t status) {
    uint8_t sta[21];
    uint32_t len = sprintf((char *)sta, "{\"STA\":\"0x%08lx\"}", (unsigned long)status);
    return api_dispatch(api_private(call->api), call, NULL, sta, len);
}

/* Рассылка по имени FID адресована всем подписчикам этого имени на ОДНОМ ядре,
 * поэтому ядро — аргумент, как и у регистрации: чья периферия собрала кадр, то
 * ядро его и рассылает (в прошивке задач периферии столько же, сколько ядер, и
 * каждая знает своё). */
uint8_t api_broadcast(api_t *api, const char *name, const uint8_t *json,
                            uint32_t len) {
    return api_dispatch((api != NULL) ? api_private(api) : NULL, NULL, name, json, len);
}

/* --- События транспорта --------------------------------------------------- */

/* Сколько токенов отдаём разбору входящего сообщения и разбору ARG. Токен — это
 * ключ, значение или скобка структуры; конверт протокола укладывается в
 * единицы, ARG запроса регистров — в десятки. Выход за предел означает отказ
 * разбора, а не рост памяти: токены лежат на стеке задачи, jsmn не рекурсивен. */
#define API_FRAME_MAX_TOKENS 48
#define API_ARG_MAX_TOKENS   48

static void api_session_on_message(api_private_t *core, api_session_t *sess,
                                   const void *data, uint32_t len);

/* Принято новое соединение: заводим сессию ядра, привязанную к дескриптору
 * транспорта. Больше про соединение ядро ничего не знает — ни интерфейса, ни
 * сокета: интерфейс уже выражен тем, ЧЕЙ это транспорт (у каждого ядра свой).
 *
 * Обработчик события — обработчик делегата, а тот ничего не возвращает, поэтому
 * отказать транспорту ядро не может. Не хватило памяти на сессию — это отказ
 * уже в самом ядре: соединение остаётся без сервиса, а каждое его сообщение
 * отбивается как «неизвестное соединение» (см. TRANSPORT_RECEIVED). */
static void api_session_open(api_private_t *core, const void *self) {
    api_session_t *sess = malloc(sizeof(api_session_t));
    if(sess == NULL) {
        api_log(core, API_LOG_WARN, "No memory for a session, connection %p left unserved", self);
        return;
    }
    sess->self = self;
    /* Номер соединения спрашиваем у транспорта — только ради журнала.
     * peer_id необязателен, поэтому -1 — законное «номера нет». */
    sess->peer_id = (api_transport_iface(core)->peer_id != NULL)
                  ? (int)api_transport_iface(core)->peer_id(self) : -1;

    api_lock(core);
    vLinkedListInsertLast(&core->sessions, LinkedListItem(sess));
    api_unlock(core);

    api_log(core, API_LOG_INFO, "Connection established (id=%d), session=%p", sess->peer_id, (void *)sess);
}

/* Соединение разорвано: сессию освобождаем, вызовы этого соединения снимаем —
 * отвечать некому. Сами вызовы освободит такт работы, и лишь после того, как
 * обработчик вернёт FALSE. */
static void api_session_close(api_private_t *core, const void *self) {
    api_lock(core);
    api_session_t *sess = api_session_find(core, self);
    if(sess != NULL) {
        vLinkedListUnlink(LinkedListItem(sess));
        api_break_calls_by_session(core, sess);
    }
    api_unlock(core);

    if(sess == NULL) {
        /* Транспорт вправе сообщить о закрытии соединения, которого ядро не
         * принимало (например, закрытие после неудачной отправки). */
        api_log(core, API_LOG_DEBUG, "Closed unknown connection %p", self);
        return;
    }
    api_log(core, API_LOG_INFO, "Connection closed (id=%d), session=%p released", sess->peer_id, (void *)sess);
    free(sess);
}

/* Отправка завершилась — успешно или нет. Транспорт вернул ровно тот указатель,
 * который получил в begin_send, поэтому по нему находим блок ответа и
 * освобождаем его, когда последний получатель рассылки закончил отправку. */
static void api_transfer_done(api_private_t *core, const void *self,
                              const transport_event_trigger_t *ev) {
    if(ev->data.ptr == NULL) {
        api_log(core, API_LOG_WARN, "Transfer event without data, connection %p", self);
        return;
    }

    if(ev->event == TRANSPORT_TRANSFER_ERR) {
        /* Отправка не прошла — соединение считаем мёртвым: вызовы снимаем, а
         * соединение закрываем. Живость соединения определяется именно так:
         * ядро не пингует клиента, это дело транспорта. */
        api_log(core, API_LOG_WARN, "Send failed, closing connection %p", self);
        api_lock(core);
        api_session_t *sess = api_session_find(core, self);
        if(sess != NULL) api_break_calls_by_session(core, sess);
        api_unlock(core);
        if(api_transport_iface(core)->close != NULL)
            api_transport_iface(core)->close(self);
        /* self транспорту не принадлежит: дальше им не пользуемся */
    } else {
        api_log(core, API_LOG_DEBUG, "Transfer complete, connection %p", self);
    }

    api_data_t *resp = api_data_from_payload(ev->data.ptr);
    if((resp->counter == 0) || (--resp->counter == 0))
        free(resp);
}

/* Единственная точка входа транспорта в ядро: ядро не знает ни платформы, ни
 * WebSocket — только этот интерфейс.
 *
 * Аргументы делегатные (event_handler_t, см. transport.h): event_trigger —
 * событие транспорта, sender — дескриптор соединения, context — то, что ядро
 * положило в СВОЙ delegate_t (см. api_init), то есть сам дескриптор ядра. Так у
 * каждого поднятого ядра свой обработчик событий, хотя функция одна: чужие
 * соединения и отправки оно не увидит. */
static void api_transport_event(void *event_trigger, void *sender, void *context) {
    if((event_trigger == NULL) || (sender == NULL) || (context == NULL)) return;
    api_private_t *core = api_private((const api_t *)context);
    const transport_event_trigger_t *ev = (const transport_event_trigger_t *)event_trigger;
    const void *self = sender;   /* дескриптор соединения — ключ сессии, не более */

    switch(ev->event) {
    case TRANSPORT_ESTABLISHED:
        api_session_open(core, self);
        return;

    case TRANSPORT_CLOSED:
        api_session_close(core, self);
        return;

    case TRANSPORT_RECEIVED: {
        /* Транспорт отдаёт УРОВЕНЬ СООБЩЕНИЙ: фрагменты он уже склеил сам, и
         * буфер принадлежит ему — ядро только читает его до возврата отсюда. */
        api_lock(core);
        api_session_t *sess = api_session_find(core, self);
        api_unlock(core);
        if(sess == NULL) {
            api_log(core, API_LOG_WARN, "Message for unknown connection %p, dropped", self);
            return;
        }
        api_session_on_message(core, sess, ev->data.ptr, ev->data.length);
        return;
    }

    case TRANSPORT_TRANSFER_COMPLETE:
    case TRANSPORT_TRANSFER_ERR:
        api_transfer_done(core, self, ev);
        return;

    default:
        /* OPEN и TRANSFER_BUFFER ядру не нужны: слушает транспорт, буферов он
         * ядру не выдаёт. */
        api_log(core, API_LOG_DEBUG, "Unhandled transport event 0x%x", (unsigned)ev->event);
        return;
    }
}

/* Разбор принятого СООБЩЕНИЯ (JSON). Транспорт уже склеил фрагменты, так что
 * здесь всегда целое сообщение; буфер принадлежит транспорту и живёт до возврата
 * отсюда — токены jsmn указывают внутрь него, поэтому ядро его не освобождает. */
static void api_session_on_message(api_private_t *core, api_session_t *sess,
                                   const void *data, uint32_t len) {
    if(!data || !len) {
        api_log(core, API_LOG_INFO, "Got packet with empty message");
        return;
    }
    api_log(core, API_LOG_DEBUG, "Got message (%lu bytes): %.*s", len, (int)len, (const char *)data);

    api_call_t *call = malloc(sizeof(api_call_t));
    if(!call) {
        api_log(core, API_LOG_WARN, "Failed to malloc memory for ws api call");
        return;
    }
    memset(call, 0, sizeof(api_call_t));
    call->flags |= CALL_FLAG_NEW;
    /* Ядро вызова: ответ и статус адресованы вызову, и берут его отсюда, а не из
     * аргументов. Дескриптор ядра и его состояние — один и тот же адрес. */
    call->api = (api_t *)(void *)core;

    /* Разбор конверта: jsmn, без выделения памяти на каждый объект. Токены —
     * на стеке задачи, поэтому вложенный или огромный пакет не может ни съесть
     * кучу, ни переполнить стек разбором (jsmn не рекурсивен). */
    GW_JSON_TOKENS(toks, API_FRAME_MAX_TOKENS);
    gw_json_t json;
    if (!gw_json_parse(&json, (const char *)data, len, toks, API_FRAME_MAX_TOKENS)) {
        api_log(core, API_LOG_WARN, "Сообщение не разобрано как JSON или слишком сложно (%u токенов)",
                (unsigned)API_FRAME_MAX_TOKENS);
        free(call);
        return;
    }

    /* FID — только строка: имя вида "<подсистема>.<действие>". Числовой код
     * отклоняем: протокол задан именами целиком, и принимать оба вида значило бы
     * держать два протокола в одном месте. */
    if (!gw_json_str(&json, "FID", call->fid, sizeof(call->fid))) {
        api_log(core, API_LOG_WARN, "Api call bad FID property (нужно имя строкой, например \"uart2.modbus\")");
        free(call);
        return;
    }

    /* ARG отдаём обработчику как есть — срезом исходного сообщения, без дерева и
     * без печати в строку. */
    const char *arg_s = NULL;
    size_t      arg_n = 0;
    if (gw_json_raw(&json, "ARG", &arg_s, &arg_n)) {
        call->req_data = (uint8_t *)malloc(arg_n + 1);
        if (!call->req_data) {
            free(call);
            return;
        }
        mem_cpy(call->req_data, arg_s, arg_n);
        call->req_data[arg_n] = '\0';
        call->req_data_len = (uint32_t)arg_n;

        /* ARG.RDL (задержка повторного опроса, мс) — общее поле каркаса для
         * вызовов с FLAGS:4; смотрим тем же разбором, что и обработчики. */
        GW_JSON_TOKENS(arg_toks, API_ARG_MAX_TOKENS);
        gw_json_t arg;
        if (gw_json_parse(&arg, (const char *)call->req_data, arg_n,
                          arg_toks, API_ARG_MAX_TOKENS)) {
            int32_t rdl;
            if (gw_json_int(&arg, "RDL", &rdl) && rdl > 0)
                call->poll_interval_ms = (uint32_t)rdl;
        }
    }

    /* Сессия нужна до первой проверки: ответ об ошибке уходит именно в неё, а
     * не «в это ядро». */
    call->session = sess;

    /* FLAGS остаётся обязательным полем протокола */
    int32_t flags;
    if (!gw_json_int(&json, "FLAGS", &flags)) {
        api_log(core, API_LOG_WARN, "Missing FLAGS, dropping call fid:%s", call->fid);
        api_send_status(call, API_CALL_ERROR_STATUS_BAD_REQ);
        free(call->req_data);
        free(call);
        return;
    }
    call->flags |= (uint32_t)flags;

    /* SID назначает клиент, чтобы отличить ответ на свой запрос. Сервер его не
     * требует и не придумывает: нет SID — ответ уйдёт без этого поля. */
    int32_t sid;
    if (gw_json_int(&json, "SID", &sid)) {
        call->has_sid = 1;
        call->id      = (uint32_t)sid;
    }

    /* FLAGS с битом удаления — это ПРОСЬБА снять раньше созданный вызов этого же
     * клиента (тот же FID и SID), а не новый вызов. Снимает вызов такт работы —
     * и только после того, как обработчик вернёт FALSE (см. api_call_teardown),
     * поэтому здесь только ставим флаг. */
    if(call->flags & CALL_FLAG_TO_DELETE) {
        api_call_t *victim = NULL;
        if(call->has_sid) {
            api_lock(core);
            victim = api_call_find_to_delete(core, sess, call->fid, call->id);
            if(victim)
                victim->flags |= CALL_FLAG_TO_DELETE;
            api_unlock(core);
        } else {
            api_log(core, API_LOG_WARN, "Снятие вызова %s без SID: нечего искать", call->fid);
        }

        api_log(core, API_LOG_INFO, "Снятие вызова %s id:%lu %s", call->fid, (unsigned long)call->id,
                victim ? "принято" : "— такого вызова нет");
        /* Клиент ждёт подтверждения, что просьба дошла. */
        api_send_status(call, API_CALL_STATUS_DELIVERED);
        free(call->req_data);
        free(call);
        return;
    }

    api_lock(core);

    /* Сервис ищется по имени FID в таблице обработчиков ЭТОГО ядра: на каком
     * ядре он зарегистрирован, через такой транспорт и приходит (см. api.h).
     * Запрос доставляется только вызовом обработчика. Нет обработчика — вызов
     * получает NO_HANDLER (а не «молча ничего»). */
    api_handler_item_t *handler = api_handler_find(core, call->fid);
    if(handler == NULL) {
        api_log(core, API_LOG_WARN, "No handler for FID %s, dropping call id:%lu",
                call->fid, call->id);
        api_send_status(call, API_CALL_ERROR_STATUS_NO_HANDLER);
        free(call->req_data);
        free(call);
    } else {
        call->handler     = handler->handler;
        call->handler_ctx = handler->handler_ctx;
        vLinkedListInsertLast(&core->calls, LinkedListItem(call));
        api_log(core, API_LOG_DEBUG, "New api call %s enqueued with id:%lu", call->fid, call->id);
    }

    api_unlock(core);
}

/* --- Такт работы ---------------------------------------------------------- */

/* Один такт ядра: выдача запросов обработчикам, их такт работы, снятие вызовов
 * без соединения. Ответ клиенту сюда не входит: его отправляет сам обработчик
 * (api_reply) из своего вызова. Функцию зовёт api_worker; она НЕ ЖДЁТ:
 * всё, что не успела, остаётся на следующий вызов.
 *
 * Такт ограничен по частоте, а обработчик, которому нечего делать, возвращается
 * сразу.
 *
 * Такт 10 мс стоил обработчику периферии двух лишних тактов на транзакцию
 * (старт и съём ответа), а при нескольких запросах в работе — ещё и ожидания:
 * замерено на приборе 9600 бод: ожидание такта 52 мс, транзакция 70 мс при
 * физических 20 мс на шине, оборот 123 мс. С тактом 1 мс оборот падает до
 * времени самой шины, а обработчик пульса остаётся дешёвым: проверка состояния
 * и возврат.
 *
 * Период объявлен в api.h: его же держит задача приложения, которая зовёт
 * api_worker, — иначе «такт ядра» и «пауза вызывающего» разошлись бы. */

/* Обработчики нельзя звать под core->mutex так, как зовут остальное: обработчик
 * отвечает через api_reply, а тот берёт тот же мьютекс повторно. Мьютекс
 * рекурсивный (это требование к системе, см. api.h), поэтому самоблокировки нет,
 * но критическая секция всё равно не должна зависеть от того, чем занят
 * обработчик. Поэтому под мьютексом только собираем список вызовов, а дёргаем их
 * после освобождения. Освобождать вызовы может лишь эта же задача, так что
 * собранные указатели остаются валидными. */
static int api_collect_handled(api_private_t *core, LinkedList_t list, int from) {
    int n = from;
    LinkedListItem_t *item = pxLinkedListFindFirst(list, NULL, NULL);
    while(item != NULL && n < API_WORK_BATCH) {
        LinkedListItem_t *next = pxLinkedListFindNextNoOverlap(item, NULL, NULL);
        api_call_t *call = LinkedListGetObject(api_call_t, item);
        if(call->handler && call->handler_started && !(call->flags & CALL_FLAG_TO_DELETE))
            core->work_batch[n++] = call;
        item = next;
    }
    return n;
}

/* Вызовы, которые просят снять (клиент ушёл, пришёл FLAGS с флагом удаления):
 * их надо довести до конца — спросить обработчика, и только после FALSE
 * освобождать память вызова. */
static int api_collect_finish(api_private_t *core, LinkedList_t list, int from) {
    int n = from;
    LinkedListItem_t *item = pxLinkedListFindFirst(list, NULL, NULL);
    while(item != NULL && n < API_WORK_BATCH) {
        LinkedListItem_t *next = pxLinkedListFindNextNoOverlap(item, NULL, NULL);
        api_call_t *call = LinkedListGetObject(api_call_t, item);
        if(call->flags & CALL_FLAG_TO_DELETE)
            core->finish_batch[n++] = call;
        item = next;
    }
    return n;
}

static void api_finish_calls(api_private_t *core) {
    api_lock(core);
    int n = api_collect_finish(core, core->calls, 0);
    n = api_collect_finish(core, core->waiting, n);
    api_unlock(core);

    for(int i = 0; i < n; i++) {
        api_call_t *call = core->finish_batch[i];
        /* Флаг мог поставить клиент (FLAGS с битом удаления) — тогда он ждёт
         * подтверждения. Если клиента уже нет, отправлять некуда. */
        if(call->session)
            api_send_status(call, API_CALL_STATUS_DELIVERED);
        if(!api_call_teardown(call))
            continue;            /* обработчик ещё не завершился — спросим тактом позже */

        api_lock(core);
        LinkedListItem_t *li = LinkedListItem(call);
        if(bLinkedListContains(core->calls, li)) vLinkedListUnlink(li);
        if(bLinkedListContains(core->waiting, li)) vLinkedListUnlink(li);
        free(call->req_data);
        free(call);
        api_unlock(core);
    }
}

static void api_work_cycle(api_private_t *core) {
    uint32_t now = api_time(core);
    if((now - core->last_work_tick) < API_WORK_PERIOD_MS) return;
    core->last_work_tick = now;

    /* Сначала те, кого просят снять: пока обработчик не вернул результат,
     * вызов остаётся жить в списках. */
    api_finish_calls(core);

    api_lock(core);
    int n = api_collect_handled(core, core->calls, 0);
    n = api_collect_handled(core, core->waiting, n);
    api_unlock(core);

    for(int i = 0; i < n; i++) {
        api_call_t *call = core->work_batch[i];
        /* data == NULL — такт работы, а не запрос клиента */
        if(call->handler(call, &call->handler_ctx, NULL, 0)) continue;

        /* Обработчик вернул FALSE — это и есть результат: вызов завершён
         * (контекст он освободил сам), можно снимать. */
        api_lock(core);
        LinkedListItem_t *li = LinkedListItem(call);
        if(bLinkedListContains(core->calls, li)) vLinkedListUnlink(li);
        if(bLinkedListContains(core->waiting, li)) vLinkedListUnlink(li);
        if(bLinkedListContains(core->calls, li) || bLinkedListContains(core->waiting, li)) {
            api_unlock(core);
            continue;                      /* ещё где-то в списках — не трогаем */
        }
        if(call->flags & CALL_FLAG_LONG_TERM) {
            /* Подписка: вызов не закрываем, а возвращаем в работу — ядро само
             * повторит запрос (по ARG.RDL или сразу), и обработчик получит его
             * заново как обычный запрос клиента. Данные запроса сохраняем. */
            call->next_poll_ms = api_time(core) + call->poll_interval_ms;
            vLinkedListInsertLast(&core->calls, li);
            api_unlock(core);
            continue;
        }
        free(call->req_data);
        free(call);
        api_unlock(core);
    }
}

/* ARG запроса для обработчика. Клиент мог не прислать ARG — тогда это ПУСТОЙ
 * срез, но НЕ NULL: NULL у живого вызова означает такт работы, и обработчик не
 * должен путать «клиент ничего не прислал» с «ядро дало такт» (см. api.h). */
static uint8_t *api_call_arg(const api_call_t *call) {
    static uint8_t empty_arg[1] = { 0 };   /* общий срез нулевой длины: не пишется */
    return (call->req_data != NULL) ? call->req_data : empty_arg;
}

static void api_worker_step(api_private_t *core) {
        /* 0. Такт работы: обработчики получают возможность сделать своё
         * периодическое дело, не блокируя ядро */
        api_work_cycle(core);

        /* 1. У вызовов без соединения просим завершения (клиент ушёл или ответ
         * уже отправлен): живость соединения видит транспорт, а не ядро. */
        api_lock(core);
        ulLinkedListDoForeach(core->calls, api_serve_call, NULL);
        ulLinkedListDoForeach(core->waiting, api_serve_call, NULL);
        api_unlock(core);

        /* 2. Отдать обработчику новые вызовы. Доставка запроса сервису —
         * ровно один путь: вызов его обработчика. Обработчик сам решает,
         * ответить сразу (вернув FALSE) или попросить ещё такт (вернув TRUE). */
        api_lock(core);
        LinkedListItem_t *item = pxLinkedListFindFirst(core->calls, NULL, NULL);
        while(item != NULL) {
            LinkedListItem_t *next = pxLinkedListFindNextNoOverlap(item, NULL, NULL);
            api_call_t *call = LinkedListGetObject(api_call_t, item);

            if(call->flags & CALL_FLAG_TO_DELETE) {
                /* Вызов просят снять. Ничего не раздаём и не освобождаем здесь:
                 * завершением займётся такт работы, и лишь после того, как
                 * обработчик вернёт FALSE (см. api_call_teardown). */
                item = next;
                continue;
            }

            /* Подписка, которая ещё не заработала новый опрос (ARG.RDL),
             * остаётся в списке до истечения next_poll_ms и обработчику не
             * отдаётся. */
            if((call->flags & CALL_FLAG_LONG_TERM) && !(call->flags & CALL_FLAG_NEW)
               && ((int32_t)(api_time(core) - call->next_poll_ms) < 0)) {
                item = next;
                continue;
            }

            if(call->handler) {
                /* Обработчик зарегистрирован — отдаём ему запрос. Дальше вызов
                 * живёт по такту работы: обработчик получает data == NULL и
                 * завершается, вернув FALSE. */
                if(call->flags & CALL_FLAG_NEW) {
                    bool sent = api_send_status(call, API_CALL_STATUS_DELIVERED);
                    if(sent)
                        call->flags &= ~CALL_FLAG_NEW;
                }
                call->handler_started = 1;
                /* Переносим в список «в работе»: иначе следующий проход снова
                 * отдаст обработчику тот же запрос. */
                vLinkedListInsertLast(&core->waiting, item);
                /* Зовём обработчик без мьютекса: он отправляет ответ через API.
                 * Мьютекс отпускаем и берём заново — такт работы и удаление
                 * вызовов всё равно в этой же задаче. */
                api_unlock(core);
                uint8_t alive = call->handler(call, &call->handler_ctx,
                                              api_call_arg(call), call->req_data_len);
                api_lock(core);
                if(!alive) {
                    /* false от обработчика — вызов завершён (его результат) */
                    if(call->flags & CALL_FLAG_LONG_TERM) {
                        /* Подписка живёт дальше: вызов и так лежит в списке
                         * работы, ядро повторит запрос по таймеру переопроса.
                         * Переставлять его в конец списка нельзя: проход шёл бы
                         * по одному и тому же вызову бесконечно (клиент вправе
                         * подписаться на любой FID, в том числе на обработчик,
                         * который отвечает сразу и возвращает FALSE). */
                        call->next_poll_ms = api_time(core) + call->poll_interval_ms;
                    } else {
                        vLinkedListUnlink(item);
                        free(call->req_data);
                        free(call);
                    }
                }
            } else {
                /* Обработчика нет — сервис снял регистрацию между разбором
                 * запроса и тактом. Отвечаем внятным отказом, а не молчанием. */
                api_log(core, API_LOG_INFO, "no handler for FID %s, send status INVALID", call->fid);
                api_send_status(call, API_CALL_STATUS_INVALID);
                vLinkedListUnlink(item);
                free(call->req_data);
                free(call);
            }
            item = next;
        }
        api_unlock(core);
}

/* Один такт работы ядра. Задачу ядро не заводит: такт даёт тот, кто его поднял
 * (в прошивке — одна задача приложения на все ядра, на хосте — тест).
 * NULL — не такт, а ничего: вызывающему не нужно помнить, поднялось ли ядро. */
void api_worker(api_t *api) {
    if(api == NULL) return;
    api_worker_step(api_private(api));
}

api_t *api_init(const transport_t *transport, const api_system_t *system) {
    const transport_iface_t *iface = (transport != NULL) ? transport->iface : NULL;
    /* Полуинтерфейс ядру не годится: без отправки, закрытия и подписки оно
     * узнает об отказе уже в момент запроса клиента. Транспорт без объекта
     * (transport == NULL) — тоже: ядру не к чему подписываться. */
    if((iface == NULL) || (iface->begin_send == NULL) ||
       (iface->close == NULL) || (iface->subscribe == NULL)) return NULL;
    /* Система без этих операций — не система: мьютекс, время и журнал ядру нужны
     * в любом сценарии, а других системных вызовов у него нет вовсе (ни
     * очередей, ни задержек, ни потоков). Отказываем сразу и честно: собрать
     * ядро, которое упадёт на первом же вызове, хуже, чем вернуть NULL. */
    if((system == NULL) || (system->mutex_create == NULL) ||
       (system->mutex_take == NULL) || (system->mutex_give == NULL) ||
       (system->log == NULL) || (system->time_ms == NULL)) return NULL;

    /* Дескриптор — собственность ядра: его выделяет ядро и освобождает
     * api_deinit. Вызывающий только хранит указатель, поэтому ни «статический
     * дескриптор», ни размер состояния снаружи не нужны. Нулевое состояние —
     * это и есть «ядро только что поднято»: списки пусты, задача не запущена. */
    api_t *api = calloc(1, sizeof(*api));
    if(api == NULL) return NULL;
    api_private_t *core = api_private(api);
    core->transport = transport;

    /* Система — до подписки: событие транспорта может прийти сразу после неё, а
     * обработчику уже нужен мьютекс. Ядро заводит только мьютекс, и то через
     * таблицу: своей задачи, своей очереди и своих задержек у него нет. Если
     * мьютекс не создался, освобождать пока нечего — списки пусты, а сам
     * мьютекс принадлежит системе (в тестах его снимает system_stub_reset). */
    core->system = system;
    core->mutex = system->mutex_create(system->context);
    if(core->mutex == NULL) {
        api_log(core, API_LOG_WARN, "No memory for the API core");
        free(api);
        return NULL;
    }

    /* Подписка на события транспорта. Подписчик — делегат в самом дескрипторе:
     * handler — обработчик событий ядра, context — дескриптор (по нему обработчик
     * и находит своё ядро: обработчиков может быть несколько). self подписки —
     * контекст реализации из ОБЪЕКТА транспорта. Отписка транспорту не нужна:
     * делегат принадлежит ядру, и снимает его само ядро (api_deinit). */
    core->delegate.handler = api_transport_event;
    core->delegate.context = api;
    transport_result_t res = iface->subscribe(transport_self(core), &core->delegate);
    if(res != TRANSPORT_OK) {
        api_log(core, API_LOG_WARN, "Transport subscribe failed: %d", (int)res);
        free(api);
        return NULL;
    }

    /* Поток заводит и держит приложение (одна задача на все ядра), а такт ядру
     * даёт api_worker. Ядро не создаёт ни потоков, ни очередей, ни задержек — в
     * этом и состоит его кооперативный контракт. */
    api_log(core, API_LOG_INFO, "API core up, transport=%p", transport->context);
    return api;
}

/* Освободить вызовы одного списка вместе с их данными запроса. Список берётся
 * по указателю: LinkedList_t — это голова списка, и снятие элемента меняет
 * именно её, а копия головы после первого unlink указывала бы на освобождённый
 * элемент. */
static void api_free_calls(LinkedList_t *list) {
    LinkedListItem_t *item;
    while((item = pxLinkedListFindFirst(*list, NULL, NULL)) != NULL) {
        api_call_t *c = LinkedListGetObject(api_call_t, item);
        vLinkedListUnlink(item);
        free(c->req_data);
        free(c);
    }
}

void api_deinit(api_t *api) {
    if(api == NULL) return;
    api_private_t *core = api_private(api);

    /* Сначала отписка: транспорт не должен остаться с обработчиком, который
     * смотрит на освобождаемое ядро, — иначе первое же событие было бы
     * обращением к освобождённой памяти. Отписка — операция ДЕЛЕГАТА, а не
     * транспорта: ядро снимает свою подписку само, и транспорту (какой бы он ни
     * был) для этого ничего не нужно. После этого его событий у ядра уже не
     * будет: делегата в event_t транспорта нет. */
    event_unsubscribe(&core->delegate);

    /* «Хвостов» в ядре не остаётся: кадр клиенту собирается и отдаётся
     * транспорту прямо из обработчика, очередей у ядра нет вовсе. Хвосты
     * отправок в самом транспорте ядру неизвестны: их доводит до конца транспорт
     * (в тестах — ws_transport_stub_reset), и только пока ядро живо. */

    /* Вызовы — раньше сессий: вызов ссылается на сессию, снятие вызова её не
     * трогает. */
    api_free_calls(&core->calls);
    api_free_calls(&core->waiting);

    LinkedListItem_t *item;
    while((item = pxLinkedListFindFirst(core->handlers, NULL, NULL)) != NULL) {
        api_handler_item_t *h = LinkedListGetObject(api_handler_item_t, item);
        vLinkedListUnlink(item);
        free(h);
    }
    while((item = pxLinkedListFindFirst(core->sessions, NULL, NULL)) != NULL) {
        api_session_t *s = LinkedListGetObject(api_session_t, item);
        vLinkedListUnlink(item);
        free(s);
    }
    /* Мьютекс ядра здесь не освобождается: операции удаления в таблице системы
     * нет, а на цели ядро живёт до перезагрузки. Тот, кто поднимает и снимает
     * ядра на хосте (тесты), освобождает его своей системой —
     * system_stub_reset. */
    free(api);
}
