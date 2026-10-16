/* WsServer test list (delete each line as its TEST is written):
 * [x] Init — ws_server_init() creates exactly one worker task
 *
 * [x] Register — bApiCallRegister succeeds for valid handler + FID
 * [x] Register — NULL handler → fails
 * [x] Register — пустое/слишком длинное имя → fails
 * [x] Register — duplicate FID → fails
 * [x] Unregister — registered FID → succeeds
 * [x] Unregister — unknown FID → fails
 *
 * Транспорт (стаб — реализация ws_transport_iface_t):
 * [x] Attach — NULL iface / iface без обязательных операций → BAD_CALL
 * [x] Connect — ESTABLISHED заводит сессию, привязанную к дескриптору соединения
 * [x] Disconnect — CLOSED освобождает сессию и её вызовы
 * [x] Disconnect — повторный CLOSED не ломает ядро (нет двойного освобождения)
 * [x] CLOSED неизвестного соединения — не падаем
 * [x] PING/PONG/CLOSE обрабатывает транспорт: ядро их не видит и не отвечает
 * [x] PONG ядру не доходит (транспорт отвечает только на PING)
 * [x] Ядро не пингует: за долгий простой ни одной отправки
 *
 * [x] Frame TEXT malformed JSON       → no send
 * [x] Frame TEXT missing FLAGS        → sends BAD_REQ (1 send call)
 * [x] Frame TEXT without SID         → accepted (SID назначает клиент)
 * [x] Frame TEXT числовой FID        → отклонён (протокол только на именах)
 * [x] Frame TEXT имя ищется точно    → регистр и лишние символы не совпадают
 * [x] Frame — вложенный JSON отбрасывается без падения и без утечки
 * [x] Frame — в ответе то же имя FID, что прислал клиент
 * [x] Frame — собранное сообщение разбирается ядром как обычное
 * [x] Frame TEXT unregistered FID     → sends NO_HANDLER (1 send call)
 * [x] Frame TEXT registered handler   → call enqueued, no immediate send
 *
 * Склейка фрагментов в транспорте:
 * [x] Сообщение из трёх фрагментов доходит до ядра целиком
 * [x] Продолжение без начала отбрасывается
 * [x] Незавершённый сбор бросается при начале нового сообщения
 * [x] Одиночный кадр больше 16 КБ → соединение закрывается
 * [x] Собранное сообщение больше 16 КБ → соединение закрывается
 *
 * Отправка:
 * [x] TRANSFER_COMPLETE освобождает буфер ответа (и при рассылке — по счётчику)
 * [x] TRANSFER_ERR закрывает соединение и снимает его вызовы
 * [x] Отказ begin_send: буфер освобождает ядро, сессия не тронута
 *
 * [x] Снятие вызова — ждём FALSE обработчика, раньше не освобождаем
 * [x] Work cycle — обработчик получает такт без данных
 * [x] Work cycle — завершённый вызов снимается и больше тактов не получает
 *
 * [x] bApiCallGetId — NULL call → fails
 * [x] bApiCallGetId — NULL out_id → fails
 *
 * Сервисы по сетевым интерфейсам (регистрация с маской):
 * [x] Один FID на два интерфейса → два разных обработчика по контексту
 * [x] Точная маска выигрывает у «любой»
 * [x] Интерфейс без регистрации → NO_HANDLER (обработчик не зовётся)
 * [x] Интерфейс неизвестен → подходит только регистрация «любой»
 * [x] Транспорт без peer_iface → интерфейс неизвестен (только «любой»)
 * [x] Рассылка по FID не уходит сессии с чужим интерфейсом
 * [x] Очередь периферии подчиняется маске регистрации
 * [x] Один FID с разными масками разрешён, повтор пары (имя, маска) — нет
 * [x] Маска 0 отвергается (сервис не нужен никому)
 * [x] Снятие регистрации снимает только свою пару (имя, маска)
 * [x] Стаб транспорта задаёт интерфейс соединения
 *
 * Regression (built with ASan/UBSan, see Makefile):
 * [x] Отправка не прошла у снятого вызова → нет записи через NULL-сессию
 * [x] TO_DELETE followed by same fid/SID call → both freed, no UAF
 * [x] TO_DELETE снимает вызов с этим SID и не трогает соседний
 */

extern "C" {
#include "web_api.h"
#include "ws_transport.h"
#include "ws_hal.h"
/* Очередь ядра для ответов периферии: в устройстве объявлена в app.h, тесту
 * достаточно самого символа (он определён в ws_server.c). */
void *get_ws_worker_queue(void);
}
#include "ws_hal_stub.h"
#include "ws_transport_stub.h"
#include "CppUTest/TestHarness.h"

#include <stdlib.h>
#include <string.h>

/* Forward declarations — these callbacks are not in any public header */
extern "C" {
    void ws_server_init(void);
    void ws_server_test_reset(void);
    void ws_server_test_worker_step(void);
    void ws_server_test_work_step(void);
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

/* Контракт обработчика: (вызов, контекст, данные, длина).
 * Зов без данных — такт работы; возврат 0 означает «вызов завершён». */
static uint8_t dummy_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)call; (void)ctx; (void)data; (void)len;
    return 0;
}

/* Обработчик, который считает такты работы и держит вызов живым, пока не
 * получит запрос и ни одного такта после него. */
static int long_handler_pulses;
static uint8_t long_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)call; (void)ctx; (void)data; (void)len;
    if (!data) long_handler_pulses++;
    return 1;
}

/* Подписка, которая живёт тактами, но честно завершается, когда вызов снимают:
 * по контракту обработчик получает call == NULL и обязан вернуть FALSE. Так
 * ведут себя настоящие обработчики (например Modbus). */
static int removable_pulses;
static uint8_t removable_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)ctx; (void)len;
    if (!call) return 0;
    if (!data) removable_pulses++;
    return 1;
}

/* Завершается сам на третьем такте: проверяем, что после этого тактов нет. */
static int short_handler_pulses;
static uint8_t short_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)call; (void)ctx; (void)data; (void)len;
    if (!data) short_handler_pulses++;
    return short_handler_pulses < 3;
}

/* Сообщение «из сети»: транспорт принимает кадр, сам склеивает фрагменты и
 * отдаёт ядру готовое сообщение. */
static void send_text(ws_transport_stub_conn_t *conn, const char *json) {
    ws_transport_stub_frame(conn, WS_STUB_FRAME_TEXT, json, strlen(json), 1);
}

static void send_part(ws_transport_stub_conn_t *conn, const char *json, int final) {
    ws_transport_stub_frame(conn, WS_STUB_FRAME_TEXT, json, strlen(json), final);
}

static void send_cont(ws_transport_stub_conn_t *conn, const char *json, int final) {
    ws_transport_stub_frame(conn, WS_STUB_FRAME_CONT, json, strlen(json), final);
}

/* Установка для тестов, которым нужны ядро, подключённый транспорт и одно
 * соединение. teardown снимает состояние ядра, а дескрипторы соединений
 * освобождает ws_transport_stub_reset(). */
#define TRANSPORT_SETUP()                                    \
    do {                                                     \
        ws_hal_stub_reset();                                 \
        ws_server_init();                                    \
        ws_transport_stub_reset();                           \
        ws_transport_stub_attach();                          \
    } while (0)

#define TRANSPORT_TEARDOWN()                                 \
    do {                                                     \
        ws_server_test_reset();                              \
        ws_transport_stub_reset();                           \
    } while (0)

/* ── TEST_GROUP: Init ─────────────────────────────────────────────────── */

TEST_GROUP(WsServerInit) {
    void setup()    { ws_hal_stub_reset(); }
    void teardown() {}
};

TEST(WsServerInit, InitCreatesOneWorkerTask) {
    ws_server_init();
    LONGS_EQUAL(1, ws_hal_stub_task_created_count());
}

/* ── TEST_GROUP: Register ─────────────────────────────────────────────── */

TEST_GROUP(WsServerRegister) {
    void setup() {
        ws_hal_stub_reset();
        ws_server_init();
    }
    void teardown() {
        ws_server_test_reset();
    }
};

TEST(WsServerRegister, ValidHandlerSucceeds) {
    LONGS_EQUAL(1, bApiCallRegister(dummy_handler, "reg.one", GW_IFACE_MASK_ANY, NULL));
}

TEST(WsServerRegister, NullHandlerFails) {
    LONGS_EQUAL(0, bApiCallRegister(NULL, "reg.one", GW_IFACE_MASK_ANY, NULL));
}

TEST(WsServerRegister, EmptyNameFails) {
    /* Пустое имя — это «FID не задан»: раньше таким признаком был нулевой код */
    LONGS_EQUAL(0, bApiCallRegister(dummy_handler, "", GW_IFACE_MASK_ANY, NULL));
    LONGS_EQUAL(0, bApiCallRegister(dummy_handler, NULL, GW_IFACE_MASK_ANY, NULL));
}

TEST(WsServerRegister, TooLongNameFails) {
    /* Имя длиннее буфера отвергается целиком, а не обрезается: обрезанное имя
     * увело бы вызов не тому обработчику. */
    char long_name[API_FID_NAME_MAX + 8];
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    LONGS_EQUAL(0, bApiCallRegister(dummy_handler, long_name, GW_IFACE_MASK_ANY, NULL));
}

TEST(WsServerRegister, DuplicateNameFails) {
    bApiCallRegister(dummy_handler, "reg.two", GW_IFACE_MASK_ANY, NULL);
    LONGS_EQUAL(0, bApiCallRegister(dummy_handler, "reg.two", GW_IFACE_MASK_ANY, NULL));
}

TEST(WsServerRegister, UnregisterKnownNameSucceeds) {
    bApiCallRegister(dummy_handler, "reg.three", GW_IFACE_MASK_ANY, NULL);
    LONGS_EQUAL(1, bApiCallUnregister("reg.three", GW_IFACE_MASK_ANY));
}

TEST(WsServerRegister, UnregisterUnknownNameFails) {
    LONGS_EQUAL(0, bApiCallUnregister("reg.unknown", GW_IFACE_MASK_ANY));
}

TEST(WsServerRegister, ReregisterAfterUnregisterSucceeds) {
    bApiCallRegister(dummy_handler, "reg.four", GW_IFACE_MASK_ANY, NULL);
    bApiCallUnregister("reg.four", GW_IFACE_MASK_ANY);
    LONGS_EQUAL(1, bApiCallRegister(dummy_handler, "reg.four", GW_IFACE_MASK_ANY, NULL));
}

/* ── TEST_GROUP: Подключение транспорта ───────────────────────────────── */

TEST_GROUP(WsServerTransportAttach) {
    void setup() {
        ws_hal_stub_reset();
        ws_server_init();
        ws_transport_stub_reset();
    }
    void teardown() {
        ws_server_test_reset();
        ws_transport_stub_reset();
    }
};

TEST(WsServerTransportAttach, NullIfaceIsRejected) {
    LONGS_EQUAL(WS_TRANSPORT_BAD_CALL, ws_server_transport_attach(NULL, NULL));
}

TEST(WsServerTransportAttach, IfaceWithoutRequiredOpsIsRejected) {
    /* Без begin_send/close/subscribe ядру работать нечем: половина интерфейса
     * означала бы отказ уже в момент запроса клиента. */
    ws_transport_iface_t broken = *ws_transport_stub_iface();
    broken.begin_send = NULL;
    LONGS_EQUAL(WS_TRANSPORT_BAD_CALL, ws_server_transport_attach(&broken, ws_transport_stub_module()));
}

TEST(WsServerTransportAttach, StubAttachSucceeds) {
    LONGS_EQUAL(WS_TRANSPORT_OK, ws_transport_stub_attach());
}

/* ── TEST_GROUP: Соединения ───────────────────────────────────────────── */

TEST_GROUP(WsServerConnect) {
    ws_transport_stub_conn_t *conn;
    void setup() {
        TRANSPORT_SETUP();
        conn = ws_transport_stub_connect(42);
    }
    void teardown() {
        TRANSPORT_TEARDOWN();
    }
};

TEST(WsServerConnect, EstablishedBindsSessionToDescriptor) {
    /* Соединение установлено — сессия ядра есть, и ядро отвечает на сообщение
     * из него (незарегистрированный FID → NO_HANDLER). */
    send_text(conn, "{\"FID\":\"conn.unknown\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerConnect, ClosedReleasesSessionAndItsCalls) {
    bApiCallRegister(dummy_handler, "conn.calls", GW_IFACE_MASK_ANY, NULL);
    send_text(conn, "{\"FID\":\"conn.calls\",\"FLAGS\":0,\"SID\":1}");

    ws_transport_stub_disconnect(conn);

    /* Вызовов у соединения больше нет: рассылка по FID не находит получателей */
    uint8_t buf[] = "{}";
    LONGS_EQUAL(0, bApiCallSendJsonFidGroup("conn.calls", buf, 2));
}

TEST(WsServerConnect, RepeatedClosedIsSurvived) {
    /* Транспорт вправе сообщить о разрыве дважды: второй CLOSED приходит для
     * соединения, сессии которого у ядра уже нет. */
    ws_transport_stub_disconnect(conn);
    ws_transport_stub_emit_closed(conn);
    /* Ни падения, ни двойного освобождения (проверяют ASan/UBSan/LeakSanitizer) */
}

TEST(WsServerConnect, MessageFromUnknownConnectionIsDropped) {
    /* Сообщение из соединения, о котором ядро не знает (ESTABLISHED до него не
     * дошёл): адресовать его некуда, но и падать нельзя. */
    ws_transport_stub_conn_t *ghost = ws_transport_stub_connect_silent(7);
    send_text(ghost, "{\"FID\":\"conn.ghost\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(ghost));
    ws_transport_stub_emit_closed(ghost);   /* и CLOSED для ядра незнаком */
}

TEST(WsServerConnect, PingIsAnsweredByTransportOnly) {
    /* Управляющие кадры — уровень транспорта: ядро PING не видит и PONG не шлёт */
    ws_transport_stub_frame(conn, WS_STUB_FRAME_PING, "ping", 4, 1);
    LONGS_EQUAL(1, ws_transport_stub_reply_calls(conn));
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerConnect, PongIsIgnoredByTransport) {
    ws_transport_stub_frame(conn, WS_STUB_FRAME_PONG, "pong", 4, 1);
    LONGS_EQUAL(0, ws_transport_stub_reply_calls(conn));
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerConnect, CloseFrameIsAnsweredByTransportOnly) {
    /* CLOSE подтверждает транспорт; ядру кадр не отдаётся и ответа от него нет */
    ws_transport_stub_frame(conn, WS_STUB_FRAME_CLOSE, NULL, 0, 1);
    LONGS_EQUAL(1, ws_transport_stub_reply_calls(conn));
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerConnect, CoreDoesNotPingIdleConnection) {
    /* Пингование — не дело ядра: за долгий простой оно не отправляет ничего.
     * Живость соединения видит транспорт (TRANSFER_ERR, CLOSED). */
    for (int i = 0; i < 5; i++) {
        ws_hal_stub_advance_tick(10000);
        ws_server_test_worker_step();
    }
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

/* ── TEST_GROUP: Frame ────────────────────────────────────────────────── */

TEST_GROUP(WsServerFrame) {
    ws_transport_stub_conn_t *conn;
    void setup() {
        TRANSPORT_SETUP();
        conn = ws_transport_stub_connect(1);
    }
    void teardown() {
        TRANSPORT_TEARDOWN();
    }
};

TEST(WsServerFrame, TextMalformedJsonNoReply) {
    send_text(conn, "{bad json}");
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerFrame, TextMissingFlagsSendsBadReq) {
    /* No FLAGS field — mandatory */
    send_text(conn, "{\"FID\":\"frm.flags\",\"SID\":1}");
    LONGS_EQUAL(1, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerFrame, TextWithoutSidIsAccepted) {
    /* SID назначает клиент и он необязателен: сервер лишь повторяет его в
     * ответе, если тот был. Сообщение без SID — полноценный запрос. */
    bApiCallRegister(dummy_handler, "frm.sid", GW_IFACE_MASK_ANY, NULL);
    send_text(conn, "{\"FID\":\"frm.sid\",\"FLAGS\":0}");
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerFrame, TextUnregisteredFidSendsNoHandler) {
    send_text(conn, "{\"FID\":\"frm.unknown\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerFrame, TextRegisteredHandlerEnqueuesCallNoImmediateSend) {
    /* Handler is registered: call goes into the work queue.
     * The worker task is stubbed out — no DELIVERED status yet. */
    bApiCallRegister(dummy_handler, "frm.one", GW_IFACE_MASK_ANY, NULL);
    send_text(conn, "{\"FID\":\"frm.one\",\"FLAGS\":0,\"SID\":5}");
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerFrame, NumericFidIsRejected) {
    /* Протокол работает только с именами: число в FID — не имя, сообщение
     * отклоняется молча, как и любое другое без корректного FID. */
    bApiCallRegister(dummy_handler, "frm.numeric", GW_IFACE_MASK_ANY, NULL);
    send_text(conn, "{\"FID\":0,\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
    send_text(conn, "{\"FID\":8194,\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
    /* Строка "0x2002" — уже имя, просто незарегистрированное: клиент получает
     * внятный NO_HANDLER, а не тишину. */
    send_text(conn, "{\"FID\":\"0x2002\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_transport_stub_send_attempts(conn));
}

/* Обработчик, который сразу отвечает и завершает вызов. */
static uint8_t answer_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)ctx; (void)data; (void)len;
    const uint8_t body[] = "{\"OK\":1}";
    bApiCallSendJson(call, body, sizeof(body) - 1);
    return 0;
}

TEST(WsServerFrame, ReplyCarriesSameFidName) {
    /* Клиент присылает имя — сервер возвращает ровно его же, а не код. */
    bApiCallRegister(answer_handler, "uart2.modbus", GW_IFACE_MASK_ANY, NULL);
    send_text(conn, "{\"FID\":\"uart2.modbus\",\"FLAGS\":0,\"SID\":9}");
    ws_server_test_worker_step();
    STRCMP_CONTAINS("\"FID\":\"uart2.modbus\"", ws_transport_stub_last_sent(conn));
    STRCMP_CONTAINS("\"SID\":\"0x00000009\"", ws_transport_stub_last_sent(conn));
}

TEST(WsServerFrame, TooLongFidNameInFrameIsRejected) {
    /* Имя длиннее буфера отклоняется целиком, а не обрезается: иначе вызов мог
     * бы уйти обработчику с похожим именем. */
    std::string long_name(API_FID_NAME_MAX + 5, 'x');
    send_text(conn, ("{\"FID\":\"" + long_name + "\",\"FLAGS\":0,\"SID\":1}").c_str());
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerFrame, NameMatchesExactly) {
    /* Имя ищется целиком: другой регистр или лишний символ — это другой вызов,
     * и сервер обязан ответить NO_HANDLER, а не отдать его этому обработчику. */
    bApiCallRegister(dummy_handler, "uart2.modbus", GW_IFACE_MASK_ANY, NULL);
    send_text(conn, "{\"FID\":\"UART2.MODBUS\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_transport_stub_send_attempts(conn));       /* NO_HANDLER */
    send_text(conn, "{\"FID\":\"uart2.modbus2\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(2, ws_transport_stub_send_attempts(conn));       /* NO_HANDLER */
    send_text(conn, "{\"FID\":\"uart2.modbu\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(3, ws_transport_stub_send_attempts(conn));       /* NO_HANDLER */
    send_text(conn, "{\"FID\":\"uart2.modbus\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(3, ws_transport_stub_send_attempts(conn));       /* обработан, ответа нет */
}

TEST(WsServerFrame, TextNoArgFieldStillEnqueues) {
    /* ARG is optional — missing ARG should not block enqueueing */
    bApiCallRegister(dummy_handler, "frm.noarg", GW_IFACE_MASK_ANY, NULL);
    send_text(conn, "{\"FID\":\"frm.noarg\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerFrame, DisconnectedCallNoLongerReceivesBroadcast) {
    /* After disconnect, the session is released and the call is unbound.
     * Verify bCallFidMatch skips nulled sessions: targets == 0. */
    bApiCallRegister(dummy_handler, "frm.bcast", GW_IFACE_MASK_ANY, NULL);
    send_text(conn, "{\"FID\":\"frm.bcast\",\"FLAGS\":0,\"SID\":1}");
    ws_transport_stub_disconnect(conn);
    uint8_t buf[] = "{}";
    LONGS_EQUAL(0, bApiCallSendJsonFidGroup("frm.bcast", buf, 2));
}

TEST(WsServerFrame, DeeplyNestedJsonIsDroppedSafely) {
    /* Ровно то, что роняло устройство: прежний разбор шёл рекурсивно и на
     * глубине ~400 заканчивал стек задачи httpd. Разбор на jsmn итеративный,
     * токенов на такую вложенность не хватает — сообщение отбрасывается, память
     * под него освобождает транспорт (проверяет LeakSanitizer). */
    std::string deep = "{\"FID\":\"frm.deep\",\"FLAGS\":0,\"SID\":1,\"ARG\":";
    deep.append(500, '[');
    deep.append(500, ']');
    deep += "}";
    send_text(conn, deep.c_str());
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

/* ── TEST_GROUP: Склейка фрагментов (делает транспорт) ────────────────── */

TEST_GROUP(WsServerFragments) {
    ws_transport_stub_conn_t *conn;
    void setup() {
        TRANSPORT_SETUP();
        conn = ws_transport_stub_connect(1);
        bApiCallRegister(dummy_handler, "frag.one", GW_IFACE_MASK_ANY, NULL);
    }
    void teardown() {
        TRANSPORT_TEARDOWN();
    }
};

TEST(WsServerFragments, MessageIsAssembledFromParts) {
    /* Сообщение, разрезанное на три кадра, доходит до ядра целиком: первый
     * фрагмент идёт с FIN=0, дальше — продолжения. Ядро видит одно событие
     * RECEIVED и разбирает готовый JSON. */
    send_part(conn, "{\"FID\":\"frag.unknown\",\"FLAGS\":0,", 0);
    send_cont(conn, "\"SID\":1,", 0);
    send_cont(conn, "\"ARG\":{}}", 1);
    /* Разбор дошёл до имени: незарегистрированный FID → один NO_HANDLER */
    LONGS_EQUAL(1, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerFragments, AssembledMessageEnqueuesCallOnce) {
    send_part(conn, "{\"FID\":\"frag.one\",\"FLAGS\":0,", 0);
    send_cont(conn, "\"SID\":1,\"ARG\":{}}", 1);
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));   /* вызов ушёл в очередь */
}

TEST(WsServerFragments, ContinuationWithoutStartIsDropped) {
    send_cont(conn, "{}", 1);
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerFragments, UnfinishedAssemblyIsDroppedOnNewMessage) {
    /* Начали собирать, но пришло новое сообщение: недособранное выбрасываем
     * (иначе оно склеилось бы с чужими данными), а новое обрабатываем. */
    send_part(conn, "{\"FID\":\"frag.one\",", 0);
    send_text(conn, "{\"FID\":\"frm.unknown\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_transport_stub_send_attempts(conn));   /* ответ NO_HANDLER */
}

TEST(WsServerFragments, OversizedSingleFrameClosesConnection) {
    /* Одиночный кадр больше 16 КБ не читаем и не разбираем: сессию закрываем. */
    std::string big(WS_TRANSPORT_STUB_FRAME_MAX_LEN + 1, 'a');
    ws_transport_stub_frame(conn, WS_STUB_FRAME_TEXT, big.data(), big.size(), 1);
    LONGS_EQUAL(1, ws_transport_stub_close_calls(conn));
    LONGS_EQUAL(1, ws_transport_stub_closed(conn));
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerFragments, OversizedAssembledMessageClosesConnection) {
    /* Предел собранного сообщения тот же, что у одиночного кадра: как только
     * суммарная длина его превышает, соединение закрывается. */
    std::string big(WS_TRANSPORT_STUB_MESSAGE_MAX_LEN, 'a');
    send_part(conn, big.c_str(), 0);                     /* ровно предел — ещё можно */
    ws_transport_stub_frame(conn, WS_STUB_FRAME_CONT, "b", 1, 1);   /* +1 → предел превышен */
    LONGS_EQUAL(1, ws_transport_stub_close_calls(conn));
    LONGS_EQUAL(1, ws_transport_stub_closed(conn));
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

/* Обработчик подписки, считающий вопросы о завершении: ядро снимает вызов
 * только после того, как он вернёт FALSE. */
static int teardown_ask;
static uint8_t teardown_ask_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)ctx; (void)data; (void)len;
    if (!call) { teardown_ask++; return 0; }   /* завершение: контекст освобождён */
    return 1;                                  /* запрос принят, работаем */
}

/* ── TEST_GROUP: Отправка ─────────────────────────────────────────────── */

TEST_GROUP(WsServerSend) {
    ws_transport_stub_conn_t *conn;
    void setup() {
        TRANSPORT_SETUP();
        conn = ws_transport_stub_connect(1);
    }
    void teardown() {
        TRANSPORT_TEARDOWN();
    }
};

TEST(WsServerSend, TransferCompleteReleasesBufferForEverySubscriber) {
    /* Рассылка: один буфер на всех получателей. Освобождается по счётчику, когда
     * завершились все отправки (иначе — утечка или двойное освобождение). */
    ws_transport_stub_conn_t *conn2 = ws_transport_stub_connect(2);
    bApiCallRegister(dummy_handler, "send.bcast", GW_IFACE_MASK_ANY, NULL);
    send_text(conn,  "{\"FID\":\"send.bcast\",\"FLAGS\":4,\"SID\":1}");
    send_text(conn2, "{\"FID\":\"send.bcast\",\"FLAGS\":4,\"SID\":2}");
    ws_transport_stub_set_deferred(conn, 1);
    ws_transport_stub_set_deferred(conn2, 1);

    ws_server_test_worker_step();
    LONGS_EQUAL(1, ws_transport_stub_pending_count(conn));
    LONGS_EQUAL(1, ws_transport_stub_pending_count(conn2));
    STRCMP_CONTAINS("\"FID\":\"send.bcast\"", ws_transport_stub_last_sent(conn));
    STRCMP_CONTAINS("\"FID\":\"send.bcast\"", ws_transport_stub_last_sent(conn2));

    ws_transport_stub_complete_pending(conn, WS_TRANSPORT_OK);
    LONGS_EQUAL(0, ws_transport_stub_pending_count(conn));
    /* Второй получатель ещё не завершил отправку — буфер жив */
    ws_transport_stub_complete_pending(conn2, WS_TRANSPORT_OK);
    LONGS_EQUAL(0, ws_transport_stub_pending_count(conn2));
}

TEST(WsServerSend, TransferErrClosesConnectionAndReleasesCalls) {
    /* Отправка не прошла — соединение считаем мёртвым: ядро просит транспорт его
     * закрыть, а вызовы этого соединения снимает. */
    teardown_ask = 0;
    bApiCallRegister(teardown_ask_handler, "send.err", GW_IFACE_MASK_ANY, NULL);
    send_text(conn, "{\"FID\":\"send.err\",\"FLAGS\":4,\"SID\":1}");

    ws_transport_stub_set_deferred(conn, 1);
    ws_server_test_worker_step();                   /* DELIVERED отложен транспортом */
    LONGS_EQUAL(1, ws_transport_stub_pending_count(conn));

    ws_transport_stub_complete_pending(conn, WS_TRANSPORT_FAULT);   /* TRANSFER_ERR */
    LONGS_EQUAL(1, ws_transport_stub_close_calls(conn));

    /* Вызовы мёртвого соединения сняты: рассылать по этому FID больше некому */
    uint8_t buf[] = "{}";
    LONGS_EQUAL(0, bApiCallSendJsonFidGroup("send.err", buf, 2));

    /* Транспорт доводит закрытие до конца (у httpd — освобождение сессии) */
    ws_transport_stub_disconnect(conn);

    /* Вызов снимается тактом работы, и только после FALSE обработчика: первый
     * такт помечает вызов к снятию, второй — спрашивает обработчика. */
    ws_hal_stub_advance_tick(20);
    ws_server_test_worker_step();
    LONGS_EQUAL(0, teardown_ask);
    ws_hal_stub_advance_tick(20);
    ws_server_test_worker_step();
    LONGS_EQUAL(1, teardown_ask);
}

TEST(WsServerSend, SendRefusedByTransportReleasesBuffer) {
    /* Транспорт отказался принять отправку: события о завершении не будет,
     * буфер освобождает само ядро. */
    bApiCallRegister(dummy_handler, "send.refuse", GW_IFACE_MASK_ANY, NULL);
    ws_transport_stub_set_send_result(conn, WS_TRANSPORT_FAULT);
    send_text(conn, "{\"FID\":\"send.refuse\",\"FLAGS\":0,\"SID\":1}");
    ws_server_test_worker_step();                    /* DELIVERED не ушёл */

    LONGS_EQUAL(1, ws_transport_stub_send_attempts(conn));
    /* Вызов снят обработчиком (dummy_handler вернул FALSE) — и ничего не утекло */
    uint8_t buf[] = "{}";
    LONGS_EQUAL(0, bApiCallSendJsonFidGroup("send.refuse", buf, 2));
}

/* ── TEST_GROUP: WsServerFinishRule ───────────────────────────────────── */

/* Обработчик, который завершается не сразу: на call == NULL отвечает TRUE
 * дважды и только потом FALSE. Ядро обязано спрашивать до результата. */
static int slow_finish_steps;
static uint8_t slow_finish_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)ctx; (void)data; (void)len;
    if (!call) {
        slow_finish_steps++;
        return slow_finish_steps < 3;      /* TRUE — «ещё не завершился» */
    }
    return 1;                              /* запрос принят, работаем */
}

TEST_GROUP(WsServerFinishRule) {
    ws_transport_stub_conn_t *conn;
    void setup() {
        TRANSPORT_SETUP();
        conn = ws_transport_stub_connect(1);
        slow_finish_steps = 0;
        bApiCallRegister(slow_finish_handler, "regx.slow", GW_IFACE_MASK_ANY, NULL);
    }
    void teardown() {
        TRANSPORT_TEARDOWN();
    }
};

TEST(WsServerFinishRule, CallSurvivesUntilHandlerReportsFinished) {
    send_text(conn, "{\"FID\":\"regx.slow\",\"FLAGS\":0,\"SID\":1}");
    ws_server_test_worker_step();          /* первичная выдача: обработчик жив */

    ws_transport_stub_disconnect(conn);    /* клиент ушёл — вызов просят снять */
    slow_finish_steps = 0;

    /* Шаг 1 выставляет флаг снятия (GC идёт после такта), дальше каждый шаг —
     * один вопрос обработчику. */
    for (int i = 0; i < 3; i++) {
        ws_hal_stub_advance_tick(20);
        ws_server_test_worker_step();
    }
    LONGS_EQUAL(2, slow_finish_steps);     /* спросили дважды, вызов ещё жив */

    ws_hal_stub_advance_tick(20);
    ws_server_test_worker_step();          /* третий раз — обработчик вернул FALSE */
    LONGS_EQUAL(3, slow_finish_steps);

    for (int i = 0; i < 2; i++) {
        ws_hal_stub_advance_tick(20);
        ws_server_test_worker_step();      /* вызов снят: больше не спрашивают */
    }
    LONGS_EQUAL(3, slow_finish_steps);
}

/* ── TEST_GROUP: WsServerWorkCycle ────────────────────────────────────── */

TEST_GROUP(WsServerWorkCycle) {
    ws_transport_stub_conn_t *conn;
    void setup() {
        TRANSPORT_SETUP();
        conn = ws_transport_stub_connect(1);
        long_handler_pulses = 0;
    }
    void teardown() {
        TRANSPORT_TEARDOWN();
    }
};

TEST(WsServerWorkCycle, HandlerGetsPulsesWithoutData) {
    /* Имя вызова и такт работы различаются только данными: такт приходит с
     * data == NULL и не несёт запроса клиента. */
    bApiCallRegister(long_handler, "cycle.pulse", GW_IFACE_MASK_ANY, NULL);
    send_text(conn, "{\"FID\":\"cycle.pulse\",\"FLAGS\":0,\"SID\":1}");
    ws_server_test_worker_step();          /* первичный вызов — уже с данными */
    /* Шаг задачи сам состоит из такта работы, поэтому считаем прирост, а не
     * абсолютное значение. */
    const int base = long_handler_pulses;
    CHECK(base <= 1);
    for (int i = 0; i < 3; i++) {
        ws_hal_stub_advance_tick(20);
        ws_server_test_work_step();
    }
    LONGS_EQUAL(base + 3, long_handler_pulses);
}

TEST(WsServerWorkCycle, FinishedCallGetsNoMorePulses) {
    /* Обработчик вернул 0 — вызов завершён и снят со списка: следующий такт
     * работы до него уже не доходит (иначе вызов с контекстом копился бы в
     * списке навсегда — замерено накопление 215 вызовов за минуту). */
    short_handler_pulses = 0;
    bApiCallRegister(short_handler, "cycle.done", GW_IFACE_MASK_ANY, NULL);
    send_text(conn, "{\"FID\":\"cycle.done\",\"FLAGS\":0,\"SID\":1}");
    ws_server_test_worker_step();
    for (int i = 0; i < 3; i++) {
        ws_hal_stub_advance_tick(20);
        ws_server_test_work_step();
    }
    LONGS_EQUAL(3, short_handler_pulses);
    for (int i = 0; i < 5; i++) {
        ws_hal_stub_advance_tick(20);
        ws_server_test_work_step();
    }
    LONGS_EQUAL(3, short_handler_pulses);   /* вызов завершён — тактов больше нет */
}

/* ── TEST_GROUP: FID → очередь периферии ─────────────────────────────── */

TEST_GROUP(WsServerFidQueue) {
    ws_transport_stub_conn_t *conn;
    void setup() {
        TRANSPORT_SETUP();
        conn = ws_transport_stub_connect(1);
    }
    void teardown() {
        TRANSPORT_TEARDOWN();
    }
};

TEST(WsServerFidQueue, RegisterNullQueueFails) {
    LONGS_EQUAL(0, ws_server_register_fid_queue("q.null", GW_IFACE_MASK_ANY, NULL));
}

TEST(WsServerFidQueue, RegisterQueueSucceeds) {
    void *q = ws_hal_queue_create(4, sizeof(void *));
    LONGS_EQUAL(1, ws_server_register_fid_queue("q.one", GW_IFACE_MASK_ANY, q));
}

TEST(WsServerFidQueue, RegisterDuplicateQueueFails) {
    void *q = ws_hal_queue_create(4, sizeof(void *));
    ws_server_register_fid_queue("q.two", GW_IFACE_MASK_ANY, q);
    LONGS_EQUAL(0, ws_server_register_fid_queue("q.two", GW_IFACE_MASK_ANY, q));
}

TEST(WsServerFidQueue, TextWithQueuedFidEnqueuesNoImmediateSend) {
    /* Queue-routed FID: call enqueued for worker forwarding, no immediate reply */
    void *q = ws_hal_queue_create(4, sizeof(void *));
    ws_server_register_fid_queue("q.three", GW_IFACE_MASK_ANY, q);
    send_text(conn, "{\"FID\":\"q.three\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerFidQueue, PeerIdReachesPeripheralMessage) {
    /* Обработчик периферии отличает запрос из softAP по webapi_msg_t.fd: ядро
     * берёт этот номер у транспорта (peer_id), а не из своего состояния. */
    void *q = ws_hal_queue_create(4, sizeof(webapi_msg_t *));
    ws_server_register_fid_queue("q.fd", GW_IFACE_MASK_ANY, q);
    send_text(conn, "{\"FID\":\"q.fd\",\"FLAGS\":0,\"SID\":1}");
    ws_server_test_worker_step();

    webapi_msg_t *msg = NULL;
    LONGS_EQUAL(1, ws_hal_queue_receive(q, &msg, WS_HAL_WAIT_NONE));
    CHECK(msg != NULL);
    LONGS_EQUAL(1, msg->fd);
    free(msg->data);
    free(msg);
}

/* ── TEST_GROUP: Сервисы по сетевым интерфейсам ────────────────────────── */

/* Два обработчика одного метода: по одному на интерфейс. В тестах видно, какой
 * из них вызван и с каким контекстом регистрации (по нему обработчик и понимает,
 * через какой интерфейс его позвали). */
static int  iface_ap_calls, iface_other_calls;
static void *iface_ap_ctx_seen, *iface_other_ctx_seen;

static uint8_t iface_ap_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)call; (void)data; (void)len;
    iface_ap_calls++;
    iface_ap_ctx_seen = *ctx;
    return 0;
}

static uint8_t iface_other_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)call; (void)data; (void)len;
    iface_other_calls++;
    iface_other_ctx_seen = *ctx;
    return 0;
}

TEST_GROUP(WsServerIface) {
    ws_transport_stub_conn_t *ap_conn;
    ws_transport_stub_conn_t *sta_conn;
    /* Контексты регистрации: у каждого экземпляра сервиса свой. */
    int ctx_ap, ctx_other;
    void setup() {
        TRANSPORT_SETUP();
        ws_transport_stub_set_iface(GW_IFACE_AP);
        ap_conn = ws_transport_stub_connect(1);
        ws_transport_stub_set_iface(GW_IFACE_STA);
        sta_conn = ws_transport_stub_connect(2);
        iface_ap_calls = iface_other_calls = 0;
        iface_ap_ctx_seen = iface_other_ctx_seen = NULL;
    }
    void teardown() {
        TRANSPORT_TEARDOWN();
    }
};

TEST(WsServerIface, SameFidServesEachInterfaceByItsOwnRegistration) {
    /* Один метод зарегистрирован дважды: для точки доступа и для любого другого
     * интерфейса. Клиент точки доступа получает свой экземпляр сервиса, клиент
     * станционной сети — свой, и обработчик узнаёт интерфейс по контексту. */
    LONGS_EQUAL(1, bApiCallRegister(iface_ap_handler, "iface.fid", GW_IFACE_MASK(GW_IFACE_AP), &ctx_ap));
    LONGS_EQUAL(1, bApiCallRegister(iface_other_handler, "iface.fid", GW_IFACE_MASK_ANY, &ctx_other));

    send_text(ap_conn, "{\"FID\":\"iface.fid\",\"FLAGS\":0,\"SID\":1}");
    ws_server_test_worker_step();
    LONGS_EQUAL(1, iface_ap_calls);
    LONGS_EQUAL(0, iface_other_calls);
    CHECK(iface_ap_ctx_seen == &ctx_ap);

    send_text(sta_conn, "{\"FID\":\"iface.fid\",\"FLAGS\":0,\"SID\":2}");
    ws_server_test_worker_step();
    LONGS_EQUAL(1, iface_ap_calls);
    LONGS_EQUAL(1, iface_other_calls);
    CHECK(iface_other_ctx_seen == &ctx_other);
}

TEST(WsServerIface, ExactMaskWinsOverAnyMask) {
    /* Точная маска точнее «любой»: клиент точки доступа получает экземпляр,
     * зарегистрированный именно на AP, даже если «любой» зарегистрирован
     * раньше (порядок регистрации решает только между равными по точности). */
    LONGS_EQUAL(1, bApiCallRegister(iface_other_handler, "iface.exact", GW_IFACE_MASK_ANY, &ctx_other));
    LONGS_EQUAL(1, bApiCallRegister(iface_ap_handler, "iface.exact", GW_IFACE_MASK(GW_IFACE_AP), &ctx_ap));

    send_text(ap_conn, "{\"FID\":\"iface.exact\",\"FLAGS\":0,\"SID\":1}");
    ws_server_test_worker_step();
    LONGS_EQUAL(1, iface_ap_calls);
    LONGS_EQUAL(0, iface_other_calls);

    /* А клиенту станционной сети точной регистрации нет — подходит «любой». */
    send_text(sta_conn, "{\"FID\":\"iface.exact\",\"FLAGS\":0,\"SID\":2}");
    ws_server_test_worker_step();
    LONGS_EQUAL(1, iface_other_calls);
}

TEST(WsServerIface, CallFromUnservedInterfaceGetsNoHandler) {
    /* FID зарегистрирован только для точки доступа: клиенту станционной сети
     * сервис не оказан, и он получает внятный NO_HANDLER (отправка есть), а
     * обработчик не вызывается вовсе. */
    bApiCallRegister(iface_ap_handler, "iface.only.ap", GW_IFACE_MASK(GW_IFACE_AP), &ctx_ap);

    send_text(sta_conn, "{\"FID\":\"iface.only.ap\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_transport_stub_send_attempts(sta_conn));
    ws_server_test_worker_step();
    LONGS_EQUAL(0, iface_ap_calls);
    LONGS_EQUAL(0, iface_other_calls);
}

TEST(WsServerIface, UnknownInterfaceGetsOnlyAnyRegistration) {
    /* Интерфейс соединения неизвестен: неизвестно и то, что клиенту можно —
     * подходит только регистрация «любой интерфейс». */
    ws_transport_stub_set_iface(GW_IFACE_NONE);
    ws_transport_stub_conn_t *c = ws_transport_stub_connect(3);
    LONGS_EQUAL(GW_IFACE_NONE, ws_transport_stub_conn_iface(c));

    bApiCallRegister(iface_ap_handler, "iface.unknown", GW_IFACE_MASK(GW_IFACE_AP), &ctx_ap);
    send_text(c, "{\"FID\":\"iface.unknown\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_transport_stub_send_attempts(c));      /* NO_HANDLER */

    bApiCallRegister(iface_other_handler, "iface.unknown", GW_IFACE_MASK_ANY, &ctx_other);
    send_text(c, "{\"FID\":\"iface.unknown\",\"FLAGS\":0,\"SID\":2}");
    ws_server_test_worker_step();
    LONGS_EQUAL(1, iface_other_calls);
    LONGS_EQUAL(0, iface_ap_calls);
}

TEST(WsServerIface, TransportWithoutPeerIfaceMeansUnknownInterface) {
    /* peer_iface может быть NULL: транспорт не умеет отвечать, через какой
     * интерфейс пришло соединение. Ядро считает интерфейс неизвестным и
     * обслуживает только регистрации с маской «любой интерфейс». */
    ws_transport_iface_t plain = *ws_transport_stub_iface();
    plain.peer_iface = NULL;
    LONGS_EQUAL(WS_TRANSPORT_OK, ws_server_transport_attach(&plain, ws_transport_stub_module()));

    bApiCallRegister(iface_ap_handler, "iface.plain", GW_IFACE_MASK(GW_IFACE_AP), &ctx_ap);
    ws_transport_stub_set_iface(GW_IFACE_AP);
    ws_transport_stub_conn_t *c = ws_transport_stub_connect(4);
    send_text(c, "{\"FID\":\"iface.plain\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_transport_stub_send_attempts(c));      /* NO_HANDLER */

    bApiCallRegister(iface_other_handler, "iface.plain", GW_IFACE_MASK_ANY, &ctx_other);
    send_text(c, "{\"FID\":\"iface.plain\",\"FLAGS\":0,\"SID\":2}");
    ws_server_test_worker_step();
    LONGS_EQUAL(1, iface_other_calls);
}

TEST(WsServerIface, BroadcastReachesOnlyServicedInterfaces) {
    /* Рассылка по FID идёт только тем сессиям, чей интерфейс разрешён маской
     * регистрации: подписка клиента станционной сети на этот FID вообще не
     * принимается (NO_HANDLER), а ответ получает только точка доступа. */
    bApiCallRegister(iface_ap_handler, "iface.bcast", GW_IFACE_MASK(GW_IFACE_AP), &ctx_ap);

    send_text(ap_conn, "{\"FID\":\"iface.bcast\",\"FLAGS\":4,\"SID\":1}");
    send_text(sta_conn, "{\"FID\":\"iface.bcast\",\"FLAGS\":4,\"SID\":2}");
    LONGS_EQUAL(1, ws_transport_stub_send_attempts(sta_conn));   /* NO_HANDLER */

    const int ap_before  = ws_transport_stub_send_attempts(ap_conn);
    const int sta_before = ws_transport_stub_send_attempts(sta_conn);
    uint8_t buf[] = "{\"V\":1}";
    LONGS_EQUAL(1, bApiCallSendJsonFidGroup("iface.bcast", buf, sizeof(buf) - 1));
    LONGS_EQUAL(ap_before + 1, ws_transport_stub_send_attempts(ap_conn));
    LONGS_EQUAL(sta_before, ws_transport_stub_send_attempts(sta_conn));
    STRCMP_CONTAINS("\"FID\":\"iface.bcast\"", ws_transport_stub_last_sent(ap_conn));
}

TEST(WsServerIface, BroadcastWithoutServicedSessionHasNoRecipients) {
    /* На FID, зарегистрированный только для точки доступа, подписан клиент
     * станционной сети (подписка не принята) — рассылать некому. */
    bApiCallRegister(iface_ap_handler, "iface.bcast2", GW_IFACE_MASK(GW_IFACE_AP), &ctx_ap);
    send_text(sta_conn, "{\"FID\":\"iface.bcast2\",\"FLAGS\":4,\"SID\":1}");

    uint8_t buf[] = "{\"V\":1}";
    LONGS_EQUAL(0, bApiCallSendJsonFidGroup("iface.bcast2", buf, sizeof(buf) - 1));
}

TEST(WsServerIface, FidQueueRouteRespectsMask) {
    /* Очередь периферии регистрируется с маской так же, как обработчик:
     * вызов уходит в неё только с обслуживаемого интерфейса. */
    void *q = ws_hal_queue_create(4, sizeof(webapi_msg_t *));
    LONGS_EQUAL(1, ws_server_register_fid_queue("iface.q", GW_IFACE_MASK(GW_IFACE_AP), q));

    send_text(sta_conn, "{\"FID\":\"iface.q\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_transport_stub_send_attempts(sta_conn));   /* NO_HANDLER */
    ws_server_test_worker_step();
    LONGS_EQUAL(0, ws_hal_stub_queue_count(q));

    send_text(ap_conn, "{\"FID\":\"iface.q\",\"FLAGS\":0,\"SID\":2}");
    ws_server_test_worker_step();
    LONGS_EQUAL(1, ws_hal_stub_queue_count(q));

    webapi_msg_t *msg = NULL;
    while (ws_hal_queue_receive(q, &msg, WS_HAL_WAIT_NONE)) {
        free(msg->data);
        free(msg);
    }
}

TEST(WsServerIface, SameFidDifferentMasksIsAllowed) {
    /* Один FID регистрируется по разу на интерфейс; повтор пары (имя, маска) —
     * неразличимая регистрация, её отвергаем. */
    LONGS_EQUAL(1, bApiCallRegister(iface_ap_handler, "iface.dup", GW_IFACE_MASK(GW_IFACE_AP), &ctx_ap));
    LONGS_EQUAL(1, bApiCallRegister(iface_other_handler, "iface.dup", GW_IFACE_MASK_ANY, &ctx_other));
    LONGS_EQUAL(0, bApiCallRegister(iface_other_handler, "iface.dup", GW_IFACE_MASK_ANY, &ctx_other));
    LONGS_EQUAL(0, bApiCallRegister(iface_ap_handler, "iface.dup", GW_IFACE_MASK(GW_IFACE_AP), &ctx_ap));
}

TEST(WsServerIface, ZeroMaskRegistrationIsRejected) {
    /* Маска 0 не обслуживает никого: это ошибка регистрации, а не «сервис для
     * всех» — для всех есть GW_IFACE_MASK_ANY. */
    void *q = ws_hal_queue_create(4, sizeof(webapi_msg_t *));
    LONGS_EQUAL(0, bApiCallRegister(iface_ap_handler, "iface.zero", GW_IFACE_MASK_NONE, NULL));
    LONGS_EQUAL(0, ws_server_register_fid_queue("iface.zero", GW_IFACE_MASK_NONE, q));
}

TEST(WsServerIface, UnregisterRemovesOnlyItsOwnRegistration) {
    /* Снятие регистрации касается только своей пары (имя, маска): тот же FID,
     * зарегистрированный на другой интерфейс, продолжает работать. */
    bApiCallRegister(iface_ap_handler, "iface.unreg", GW_IFACE_MASK(GW_IFACE_AP), &ctx_ap);
    bApiCallRegister(iface_other_handler, "iface.unreg", GW_IFACE_MASK_ANY, &ctx_other);

    LONGS_EQUAL(0, bApiCallUnregister("iface.unreg", GW_IFACE_MASK(GW_IFACE_STA)));  /* такой пары нет */
    LONGS_EQUAL(1, bApiCallUnregister("iface.unreg", GW_IFACE_MASK(GW_IFACE_AP)));

    /* Клиента точки доступа теперь обслуживает экземпляр «любой интерфейс». */
    send_text(ap_conn, "{\"FID\":\"iface.unreg\",\"FLAGS\":0,\"SID\":1}");
    ws_server_test_worker_step();
    LONGS_EQUAL(0, iface_ap_calls);
    LONGS_EQUAL(1, iface_other_calls);
    CHECK(iface_other_ctx_seen == &ctx_other);
}

TEST_GROUP(WsServerIfaceStub) {
    void setup() {
        ws_hal_stub_reset();
        ws_server_init();
        ws_transport_stub_reset();
        ws_transport_stub_attach();
    }
    void teardown() {
        ws_server_test_reset();
        ws_transport_stub_reset();
    }
};

TEST(WsServerIfaceStub, StubReportsInterfaceSetBeforeConnect) {
    /* Стаб транспорта умеет задавать интерфейс соединения — так же, как
     * настоящий транспорт определяет его при установлении. */
    ws_transport_stub_set_iface(GW_IFACE_AP);
    ws_transport_stub_conn_t *ap = ws_transport_stub_connect(1);
    ws_transport_stub_set_iface(GW_IFACE_STA);
    ws_transport_stub_conn_t *sta = ws_transport_stub_connect(2);
    LONGS_EQUAL(GW_IFACE_AP,  ws_transport_stub_conn_iface(ap));
    LONGS_EQUAL(GW_IFACE_STA, ws_transport_stub_conn_iface(sta));
}

/* ── TEST_GROUP: WsServerMultiClient ─────────────────────────────────── */

TEST_GROUP(WsServerMultiClient) {
    ws_transport_stub_conn_t *conn1;
    ws_transport_stub_conn_t *conn2;
    void setup() {
        TRANSPORT_SETUP();
        conn1 = ws_transport_stub_connect(1);
        conn2 = ws_transport_stub_connect(2);
    }
    void teardown() {
        TRANSPORT_TEARDOWN();
    }
};

TEST(WsServerMultiClient, ErrorReplyGoesOnlyToSender) {
    /* Unregistered FID → NO_HANDLER sent only to conn1, not conn2 */
    send_text(conn1, "{\"FID\":\"multi.unknown\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_transport_stub_send_attempts(conn1));
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn2));
}

TEST(WsServerMultiClient, ControlFramesGoToTheSendersTransportOnly) {
    ws_transport_stub_frame(conn1, WS_STUB_FRAME_PING, "p", 1, 1);
    LONGS_EQUAL(1, ws_transport_stub_reply_calls(conn1));
    LONGS_EQUAL(0, ws_transport_stub_reply_calls(conn2));
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn1));
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn2));
}

TEST(WsServerMultiClient, DisconnectOneClientDoesNotBreakOther) {
    ws_transport_stub_disconnect(conn1);
    /* conn2 продолжает работать: сообщение разбирается и получает ответ */
    send_text(conn2, "{\"FID\":\"multi.unknown\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_transport_stub_send_attempts(conn2));
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn1));
}

/* ── TEST_GROUP: WsServerApiCalls ────────────────────────────────────── */

TEST_GROUP(WsServerApiCalls) {
    void setup() {
        ws_hal_stub_reset();
        ws_server_init();
    }
    void teardown() {
        ws_server_test_reset();
    }
};

TEST(WsServerApiCalls, SendStatusNullCallReturnsFail) {
    /* Вызов не задан и имя не задано — рассылать некому, возвращаем 0 */
    LONGS_EQUAL(0, bApiCallSendStatus(NULL, 0));
}

TEST(WsServerApiCalls, SendJsonNullCallReturnsFail) {
    uint8_t buf[] = "{}";
    LONGS_EQUAL(0, bApiCallSendJson(NULL, buf, 2));
}

TEST(WsServerApiCalls, SendJsonFidGroupNoSubscribersReturnsFail) {
    uint8_t buf[] = "{}";
    LONGS_EQUAL(0, bApiCallSendJsonFidGroup("no.subscribers", buf, 2));
}

/* ── TEST_GROUP: ApiCallGetId ─────────────────────────────────────────── */

TEST_GROUP(WsServerApiCallGetId) {
    void setup() {
        ws_hal_stub_reset();
        ws_server_init();
    }
    void teardown() {
        ws_server_test_reset();
    }
};

TEST(WsServerApiCallGetId, NullCallReturnsFail) {
    uint32_t id = 0;
    LONGS_EQUAL(0, bApiCallGetId(NULL, &id));
}

TEST(WsServerApiCallGetId, NullOutIdReturnsFail) {
    uint32_t dummy = 0;
    LONGS_EQUAL(0, bApiCallGetId(&dummy, NULL));
}

/* ── TEST_GROUP: WsServerRegression ───────────────────────────────────── */
/* One test per fixed bug. The failures here are memory errors, so they are
 * only reliable under ASan (enabled in the Makefile). */

#define REG_FID_HANDLER  "regx.handler"   /* handler only, no peripheral queue */
#define REG_FID_QUEUE    "regx.queue"     /* routed to periph_q                */

TEST_GROUP(WsServerRegression) {
    ws_transport_stub_conn_t *conn;
    void *periph_q;
    void setup() {
        TRANSPORT_SETUP();
        conn = ws_transport_stub_connect(1);
        periph_q = ws_hal_queue_create(4, sizeof(webapi_msg_t *));
        bApiCallRegister(dummy_handler, REG_FID_HANDLER, GW_IFACE_MASK_ANY, NULL);
        ws_server_register_fid_queue(REG_FID_QUEUE, GW_IFACE_MASK_ANY, periph_q);
    }
    void teardown() {
        webapi_msg_t *msg;
        while (ws_hal_queue_receive(periph_q, &msg, WS_HAL_WAIT_NONE)) {
            free(msg->data);
            free(msg);
        }
        TRANSPORT_TEARDOWN();
    }
};

TEST(WsServerRegression, RefusedSendReleasesBufferWithoutKillingSession) {
    /* Раньше неудачная отправка снимала у вызова соединение, а следующий такт
     * лез в уже освобождённую сессию — на устройстве это давало StoreProhibited
     * (EXCVADDR=0x0000000c; на 64-битном хосте адрес 0x10). Теперь об отказе
     * сообщает транспорт (begin_send != WS_TRANSPORT_OK): ядро освобождает свой
     * буфер ответа, а судьбу соединения решают события TRANSFER_ERR и CLOSED. */
    ws_transport_stub_set_send_result(conn, WS_TRANSPORT_FAULT);
    send_text(conn, "{\"FID\":\"regx.handler\",\"FLAGS\":2,\"SID\":1}");
    ws_server_test_worker_step();          /* DELIVERED не ушёл, вызов обработан */

    LONGS_EQUAL(1, ws_transport_stub_send_attempts(conn));

    /* Соединение живо: следующий запрос обслуживается как обычно */
    ws_transport_stub_set_send_result(conn, WS_TRANSPORT_OK);
    send_text(conn, "{\"FID\":\"regx.unknown\",\"FLAGS\":0,\"SID\":2}");
    LONGS_EQUAL(2, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerRegression, ToDeleteFreesSameCallQueuedRightAfterIt) {
    /* The worker cached `next` before handling TO_DELETE, then freed every
     * call with the same fid/SID — including `next` when the client sent
     * the target call right after the delete. `item = next` then walked
     * freed memory. */
    send_text(conn, "{\"FID\":\"regx.queue\",\"FLAGS\":8,\"SID\":7}");
    send_text(conn, "{\"FID\":\"regx.queue\",\"FLAGS\":4,\"SID\":7}");

    ws_server_test_worker_step();

    /* Снятие — просьба, а не вызов: подтверждение уходит сразу, а подписка,
     * пришедшая следом, живёт и уходит в очередь периферии. Раньше на просьбу
     * создавался свой вызов (и второй такт тратился на его подтверждение), а
     * снятие по совпадению fid/SID сносило соседний вызов. */
    LONGS_EQUAL(1, ws_hal_stub_queue_count(periph_q));
    LONGS_EQUAL(2, ws_transport_stub_send_attempts(conn));   /* DELIVERED просьбе и подписке */

    ws_hal_stub_advance_tick(20);
    ws_server_test_worker_step();
    LONGS_EQUAL(2, ws_transport_stub_send_attempts(conn));   /* больше ничего не уходит */
    LONGS_EQUAL(1, ws_hal_stub_queue_count(periph_q));
}

TEST(WsServerRegression, ToDeleteRemovesCallWithThatSidAndKeepsNeighbour) {
    /* Один FID, две подписки одного клиента: SID 7 и SID 8. Просьба снять SID 7
     * обязана снять ИМЕННО его: соседний вызов того же FID остаётся в работе и
     * продолжает переспрашиваться. Раньше такая просьба не снимала ничего —
     * создавался вызов, удалявший сам себя, а подписка жила дальше (на устройстве
     * это давало двойной приём данных после смены канала в терминале). */
    removable_pulses = 0;
    LONGS_EQUAL(1, bApiCallRegister(removable_handler, "regx.pulse", GW_IFACE_MASK_ANY, NULL));

    send_text(conn, "{\"FID\":\"regx.pulse\",\"FLAGS\":4,\"SID\":7,\"ARG\":{\"RDL\":10}}");
    send_text(conn, "{\"FID\":\"regx.pulse\",\"FLAGS\":4,\"SID\":8,\"ARG\":{\"RDL\":10}}");
    ws_server_test_worker_step();                        /* обе подписки отданы обработчику */
    ws_hal_stub_advance_tick(50);
    ws_server_test_worker_step();
    LONGS_EQUAL(2, removable_pulses);                    /* такт каждой из двух подписок */

    send_text(conn, "{\"FID\":\"regx.pulse\",\"FLAGS\":8,\"SID\":7}");
    ws_server_test_worker_step();                        /* снятие доходит до конца */

    ws_hal_stub_advance_tick(50);
    ws_server_test_worker_step();
    const int one_alive = removable_pulses - 2;

    ws_hal_stub_advance_tick(50);
    ws_server_test_worker_step();
    const int still_one = removable_pulses - 2 - one_alive;

    /* Живая подписка ровно одна: каждый шаг даёт один такт, а не два. При старом
     * поведении снятие не работало, и тактов было бы по два на шаг. */
    LONGS_EQUAL(1, one_alive);
    LONGS_EQUAL(1, still_one);
}
TEST(WsServerRegression, BroadcastSendsOneSharedBufferToEverySubscriber) {
    /* Данные периферии уходят подписчикам ОДНИМ буфером: на каждого клиента не
     * собирается своя копия той же портянки. Проверяем по адресу, который ядро
     * отдало транспорту: у обоих подписчиков он один и тот же. */
    ws_transport_stub_set_deferred(conn, 1);
    ws_transport_stub_conn_t *conn2 = ws_transport_stub_connect(2);
    ws_transport_stub_set_deferred(conn2, 1);

    send_text(conn,  "{\"FID\":\"regx.queue\",\"FLAGS\":4,\"SID\":1,\"ARG\":{\"RDL\":1000}}");
    send_text(conn2, "{\"FID\":\"regx.queue\",\"FLAGS\":4,\"SID\":2,\"ARG\":{\"RDL\":1000}}");
    ws_server_test_worker_step();
    /* Оба опроса уже отданы периферии — очередь опустошаем: дальше она нужна
     * только для ответа-рассылки. */
    webapi_msg_t *msg;
    while (ws_hal_queue_receive(periph_q, &msg, WS_HAL_WAIT_NONE)) {
        free(msg->data);
        free(msg);
    }
    webapi_msg_t resp = { .fd = -1 };
    resp.id = 0;
    bApiFidSet(resp.fid, REG_FID_QUEUE);
    resp.data = (uint8_t *)malloc(6);
    memcpy(resp.data, "\"AAAA\"", 6);
    resp.len  = 6;
    ws_hal_queue_send(get_ws_worker_queue(), &resp, WS_HAL_WAIT_NONE);

    ws_server_test_worker_step();

    /* Кадр ушёл обоим подписчикам, хотя опрашивал только первый. Перед кадром
     * каждому ушло подтверждение (DELIVERED), поэтому смотрим ПОСЛЕДНЮЮ
     * отправку: у обоих это данные, и это ОДИН И ТОТ ЖЕ буфер. */
    const int n1 = ws_transport_stub_pending_count(conn);
    const int n2 = ws_transport_stub_pending_count(conn2);
    CHECK(n1 >= 2);
    CHECK(n2 >= 2);
    POINTERS_EQUAL(ws_transport_stub_pending_data(conn,  n1 - 1),
                   ws_transport_stub_pending_data(conn2, n2 - 1));
    STRCMP_CONTAINS("\"AAAA\"", ws_transport_stub_last_sent(conn));

    /* Завершение отправок освобождает общий блок ровно один раз (ASan поймал бы
     * двойное освобождение) */
    ws_transport_stub_complete_pending(conn, WS_TRANSPORT_OK);
    ws_transport_stub_complete_pending(conn2, WS_TRANSPORT_OK);
}
