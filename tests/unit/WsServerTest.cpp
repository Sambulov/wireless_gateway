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
 * [x] Connect — ws_server_on_connect sets user_data on conn
 * [x] Disconnect — without prior connect: no crash
 * [x] Disconnect — after connect: no crash, user_data still held by stub
 *
 * [x] Frame PING  → replies with PONG (1 send call)
 * [x] Frame CLOSE → replies with CLOSE (1 send call)
 * [x] Frame TEXT malformed JSON       → no send
 * [x] Frame TEXT missing FLAGS        → sends BAD_REQ (1 send call)
 * [x] Frame TEXT without SID         → accepted (SID назначает клиент)
 * [x] Frame TEXT числовой FID        → отклонён (протокол только на именах)
 * [x] Frame TEXT имя ищется точно    → регистр и лишние символы не совпадают
 * [x] Frame — вложенный JSON отбрасывается без падения и без утечки
 * [x] Frame — в ответе то же имя FID, что прислал клиент
 * [x] Frame — фрагментированное сообщение собирается из частей
 * [x] Frame — продолжение без начала отбрасывается
 * [x] Frame — незавершённый сбор бросается при начале нового сообщения
 * [x] Снятие вызова — ждём FALSE обработчика, раньше не освобождаем
 * [x] Work cycle — обработчик получает такт без данных
 * [x] Work cycle — завершённый вызов снимается и больше тактов не получает
 * [x] Frame TEXT unregistered FID     → sends NO_HANDLER (1 send call)
 * [x] Frame TEXT registered handler   → call enqueued, no immediate send
 *
 * [x] bApiCallGetId — NULL call → fails
 * [x] bApiCallGetId — NULL out_id → fails
 *
 * Regression (built with ASan/UBSan, see Makefile):
 * [x] Ping send fails on last pending call → no write through NULL session
 * [x] TO_DELETE followed by same fd/FID/SID call → both freed, no UAF
 */

extern "C" {
#include "web_api.h"
#include "ws_transport.h"
#include "ws_hal.h"
}
#include "ws_hal_stub.h"
#include "ws_conn_stub.h"
#include "CppUTest/TestHarness.h"

#include <stdlib.h>
#include <string.h>

/* Forward declarations — these callbacks are not in any public header */
extern "C" {
    void ws_server_init(void);
    void ws_server_on_connect(ws_conn_t *conn);
    void ws_server_on_frame(ws_conn_t *conn, ws_frame_type_t type,
                            uint8_t *data, size_t len, uint8_t bFinal);
    void ws_server_on_disconnect(ws_conn_t *conn);
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
/* Живёт, пока такты идут: проверяем именно доставку тактов. */
static uint8_t long_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)call; (void)ctx; (void)data; (void)len;
    if (!data) long_handler_pulses++;
    return 1;
}

/* Завершается сам на третьем такте: проверяем, что после этого тактов нет. */
static int short_handler_pulses;
static uint8_t short_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)call; (void)ctx; (void)data; (void)len;
    if (!data) short_handler_pulses++;
    return short_handler_pulses < 3;
}

/* ws_server_on_text takes ownership of data — always pass malloc'd copy */
static uint8_t *heap_str(const char *s) {
    size_t n = strlen(s);
    uint8_t *buf = (uint8_t *)malloc(n + 1);
    memcpy(buf, s, n + 1);
    return buf;
}

static void send_text(ws_conn_t *conn, const char *json) {
    size_t n = strlen(json);
    ws_server_on_frame(conn, WS_FRAME_TEXT, heap_str(json), n, 1);
}

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
    LONGS_EQUAL(1, bApiCallRegister(dummy_handler, "reg.one", NULL));
}

TEST(WsServerRegister, NullHandlerFails) {
    LONGS_EQUAL(0, bApiCallRegister(NULL, "reg.one", NULL));
}

TEST(WsServerRegister, EmptyNameFails) {
    /* Пустое имя — это «FID не задан»: раньше таким признаком был нулевой код */
    LONGS_EQUAL(0, bApiCallRegister(dummy_handler, "", NULL));
    LONGS_EQUAL(0, bApiCallRegister(dummy_handler, NULL, NULL));
}

TEST(WsServerRegister, TooLongNameFails) {
    /* Имя длиннее буфера отвергается целиком, а не обрезается: обрезанное имя
     * увело бы вызов не тому обработчику. */
    char long_name[API_FID_NAME_MAX + 8];
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    LONGS_EQUAL(0, bApiCallRegister(dummy_handler, long_name, NULL));
}

TEST(WsServerRegister, DuplicateNameFails) {
    bApiCallRegister(dummy_handler, "reg.two", NULL);
    LONGS_EQUAL(0, bApiCallRegister(dummy_handler, "reg.two", NULL));
}

TEST(WsServerRegister, UnregisterKnownNameSucceeds) {
    bApiCallRegister(dummy_handler, "reg.three", NULL);
    LONGS_EQUAL(1, bApiCallUnregister("reg.three"));
}

TEST(WsServerRegister, UnregisterUnknownNameFails) {
    LONGS_EQUAL(0, bApiCallUnregister("reg.unknown"));
}

/* ── TEST_GROUP: Connect ──────────────────────────────────────────────── */

TEST_GROUP(WsServerConnect) {
    ws_conn_t *conn;
    void setup() {
        ws_hal_stub_reset();
        ws_server_init();
        conn = ws_conn_stub_make(42);
    }
    void teardown() {
        ws_server_on_disconnect(conn);  /* no-op if not connected */
        ws_server_test_reset();
        free(conn);
    }
};

TEST(WsServerConnect, ConnectSetsUserDataOnConn) {
    ws_server_on_connect(conn);
    CHECK(ws_conn_get_user_data(conn) != NULL);
}

TEST(WsServerConnect, DisconnectWithoutConnectNoCrash) {
    ws_server_on_disconnect(conn);
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
}

TEST(WsServerConnect, DisconnectAfterConnectNoCrash) {
    ws_server_on_connect(conn);
    ws_server_on_disconnect(conn);  /* frees ApiSession_t — no crash */
}

/* ── TEST_GROUP: Frame ────────────────────────────────────────────────── */

TEST_GROUP(WsServerFrame) {
    ws_conn_t *conn;
    void setup() {
        ws_hal_stub_reset();
        ws_server_init();
        conn = ws_conn_stub_make(1);
        ws_server_on_connect(conn);
    }
    void teardown() {
        ws_server_on_disconnect(conn);
        ws_server_test_reset();
        free(conn);
    }
};

TEST(WsServerFrame, PingRepliesWithPong) {
    uint8_t *data = heap_str("ping");
    ws_server_on_frame(conn, WS_FRAME_PING, data, 4, 1);
    LONGS_EQUAL(1, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFrame, CloseRepliesWithClose) {
    ws_server_on_frame(conn, WS_FRAME_CLOSE, NULL, 0, 1);
    LONGS_EQUAL(1, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFrame, TextMalformedJsonNoReply) {
    send_text(conn, "{bad json}");
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFrame, TextMissingFlagsSendsBadReq) {
    /* No FLAGS field — mandatory */
    send_text(conn, "{\"FID\":\"frm.flags\",\"SID\":1}");
    LONGS_EQUAL(1, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFrame, TextWithoutSidIsAccepted) {
    /* SID назначает клиент и он необязателен: сервер лишь повторяет его в
     * ответе, если тот был. Кадр без SID — полноценный запрос. */
    bApiCallRegister(dummy_handler, "frm.sid", NULL);
    send_text(conn, "{\"FID\":\"frm.sid\",\"FLAGS\":0}");
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFrame, TextUnregisteredFidSendsNoHandler) {
    send_text(conn, "{\"FID\":\"frm.unknown\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFrame, TextRegisteredHandlerEnqueuesCallNoImmediateSend) {
    /* Handler is registered: call goes into the work queue.
     * The worker task is stubbed out — no DELIVERED status yet. */
    bApiCallRegister(dummy_handler, "frm.one", NULL);
    send_text(conn, "{\"FID\":\"frm.one\",\"FLAGS\":0,\"SID\":5}");
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFrame, PongFrameNoReply) {
    /* PONG is handled silently — only alive-ts updated, no send */
    ws_server_on_frame(conn, WS_FRAME_PONG, NULL, 0, 1);
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFrame, NumericFidIsRejected) {
    /* Протокол работает только с именами: число в FID — не имя, кадр
     * отклоняется молча, как и любой другой кадр без корректного FID. */
    bApiCallRegister(dummy_handler, "frm.numeric", NULL);
    send_text(conn, "{\"FID\":0,\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
    send_text(conn, "{\"FID\":8194,\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
    /* Строка "0x2002" — уже имя, просто незарегистрированное: клиент получает
     * внятный NO_HANDLER, а не тишину. */
    send_text(conn, "{\"FID\":\"0x2002\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_conn_stub_send_calls(conn));
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
    bApiCallRegister(answer_handler, "uart2.modbus", NULL);
    send_text(conn, "{\"FID\":\"uart2.modbus\",\"FLAGS\":0,\"SID\":9}");
    ws_server_test_worker_step();
    STRCMP_CONTAINS("\"FID\":\"uart2.modbus\"", ws_conn_stub_last_payload(conn));
    STRCMP_CONTAINS("\"SID\":\"0x00000009\"", ws_conn_stub_last_payload(conn));
}

TEST(WsServerFrame, TooLongFidNameInFrameIsRejected) {
    /* Имя длиннее буфера отклоняется целиком, а не обрезается: иначе вызов мог
     * бы уйти обработчику с похожим именем. */
    std::string long_name(API_FID_NAME_MAX + 5, 'x');
    send_text(conn, ("{\"FID\":\"" + long_name + "\",\"FLAGS\":0,\"SID\":1}").c_str());
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFrame, NameMatchesExactly) {
    /* Имя ищется целиком: другой регистр или лишний символ — это другой вызов,
     * и сервер обязан ответить NO_HANDLER, а не отдать его этому обработчику. */
    bApiCallRegister(dummy_handler, "uart2.modbus", NULL);
    send_text(conn, "{\"FID\":\"UART2.MODBUS\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_conn_stub_send_calls(conn));       /* NO_HANDLER */
    send_text(conn, "{\"FID\":\"uart2.modbus2\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(2, ws_conn_stub_send_calls(conn));       /* NO_HANDLER */
    send_text(conn, "{\"FID\":\"uart2.modbu\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(3, ws_conn_stub_send_calls(conn));       /* NO_HANDLER */
    send_text(conn, "{\"FID\":\"uart2.modbus\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(3, ws_conn_stub_send_calls(conn));       /* обработан, ответа нет */
}

TEST(WsServerFrame, TextNoArgFieldStillEnqueues) {
    /* ARG is optional — missing ARG should not block enqueueing */
    bApiCallRegister(dummy_handler, "frm.noarg", NULL);
    send_text(conn, "{\"FID\":\"frm.noarg\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFrame, DisconnectedCallNoLongerReceivesBroadcast) {
    /* After disconnect, ws_server_test_reset clears enqueued call.
     * Verify bCallFidMatch skips nulled sessions: targets == 0. */
    bApiCallRegister(dummy_handler, "frm.bcast", NULL);
    send_text(conn, "{\"FID\":\"frm.bcast\",\"FLAGS\":0,\"SID\":1}");
    ws_server_on_disconnect(conn);
    uint8_t buf[] = "{}";
    LONGS_EQUAL(0, bApiCallSendJsonFidGroup("frm.bcast", buf, 2));
}

TEST(WsServerFrame, DeeplyNestedJsonIsDroppedSafely) {
    /* Ровно то, что роняло устройство: прежний разбор шёл рекурсивно и на
     * глубине ~400 заканчивал стек задачи httpd. Разбор на jsmn итеративный,
     * токенов на такую вложенность не хватает — кадр отбрасывается, память под
     * него освобождается (проверяет LeakSanitizer). */
    std::string deep = "{\"FID\":\"frm.deep\",\"FLAGS\":0,\"SID\":1,\"ARG\":";
    deep.append(500, '[');
    deep.append(500, ']');
    deep += "}";
    send_text(conn, deep.c_str());
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
}

/* ── TEST_GROUP: WsServerFragments ────────────────────────────────────── */

TEST_GROUP(WsServerFragments) {
    ws_conn_t *conn;
    void setup() {
        ws_hal_stub_reset();
        ws_server_init();
        conn = ws_conn_stub_make(1);
        ws_server_on_connect(conn);
        bApiCallRegister(dummy_handler, "frag.one", NULL);
    }
    void teardown() {
        ws_server_on_disconnect(conn);
        ws_server_test_reset();
        free(conn);
    }
};

TEST(WsServerFragments, MessageIsAssembledFromParts) {
    /* Сообщение, разрезанное на три кадра, должно дойти до обработчика целиком:
     * первый фрагмент идёт с FIN=0, дальше — продолжения (WS_FRAME_CONT). */
    const char *p1 = "{\"FID\":\"frag.one\",\"FLAGS\":0,";
    const char *p2 = "\"SID\":1,";
    const char *p3 = "\"ARG\":{}}";
    ws_server_on_frame(conn, WS_FRAME_TEXT, heap_str(p1), strlen(p1), 0);
    ws_server_on_frame(conn, WS_FRAME_CONT, heap_str(p2), strlen(p2), 0);
    ws_server_on_frame(conn, WS_FRAME_CONT, heap_str(p3), strlen(p3), 1);
    /* Вызов ушёл в очередь — немедленного ответа нет (как и у целого кадра) */
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFragments, ContinuationWithoutStartIsDropped) {
    ws_server_on_frame(conn, WS_FRAME_CONT, heap_str("{}"), 2, 1);
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFragments, UnfinishedAssemblyIsDroppedOnNewMessage) {
    /* Начали собирать, но пришло новое сообщение: недособранное выбрасываем
     * (иначе оно склеилось бы с чужими данными), а новое обрабатываем. */
    const char *part = "{\"FID\":\"frag.one\",";
    ws_server_on_frame(conn, WS_FRAME_TEXT, heap_str(part), strlen(part), 0);
    send_text(conn, "{\"FID\":\"frm.unknown\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_conn_stub_send_calls(conn));   /* ответ NO_HANDLER */
}

TEST(WsServerFragments, DisconnectReleasesPartialAssembly) {
    /* Незавершённый сбор не должен утечь при обрыве соединения (LeakSanitizer). */
    const char *part = "{\"FID\":\"frag.one\",";
    ws_server_on_frame(conn, WS_FRAME_TEXT, heap_str(part), strlen(part), 0);
    ws_server_on_disconnect(conn);
    ws_conn_set_user_data(conn, NULL);
}

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
    ws_conn_t *conn;
    void setup() {
        ws_hal_stub_reset();
        ws_server_init();
        conn = ws_conn_stub_make(1);
        ws_server_on_connect(conn);
        slow_finish_steps = 0;
        bApiCallRegister(slow_finish_handler, "regx.slow", NULL);
    }
    void teardown() {
        ws_server_on_disconnect(conn);
        ws_server_test_reset();
        free(conn);
    }
};

TEST(WsServerFinishRule, CallSurvivesUntilHandlerReportsFinished) {
    send_text(conn, "{\"FID\":\"regx.slow\",\"FLAGS\":0,\"SID\":1}");
    ws_server_test_worker_step();          /* первичная выдача: обработчик жив */

    ws_server_on_disconnect(conn);         /* клиент ушёл — вызов просят снять */
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
    ws_conn_t *conn;
    void setup() {
        ws_hal_stub_reset();
        ws_server_init();
        conn = ws_conn_stub_make(1);
        ws_server_on_connect(conn);
        long_handler_pulses = 0;
    }
    void teardown() {
        ws_server_on_disconnect(conn);
        ws_server_test_reset();
        free(conn);
    }
};

TEST(WsServerWorkCycle, HandlerGetsPulsesWithoutData) {
    /* Имя вызова и такт работы различаются только данными: такт приходит с
     * data == NULL и не несёт запроса клиента. */
    bApiCallRegister(long_handler, "cycle.pulse", NULL);
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
    bApiCallRegister(short_handler, "cycle.done", NULL);
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

/* ── TEST_GROUP: WsServerFidQueue ─────────────────────────────────────── */

TEST_GROUP(WsServerFidQueue) {
    ws_conn_t *conn;
    void setup() {
        ws_hal_stub_reset();
        ws_server_init();
        conn = ws_conn_stub_make(1);
        ws_server_on_connect(conn);
    }
    void teardown() {
        ws_server_on_disconnect(conn);
        ws_server_test_reset();
        free(conn);
    }
};

TEST(WsServerFidQueue, RegisterNullQueueFails) {
    LONGS_EQUAL(0, ws_server_register_fid_queue("q.null", NULL));
}

TEST(WsServerFidQueue, RegisterQueueSucceeds) {
    void *q = ws_hal_queue_create(4, sizeof(void *));
    LONGS_EQUAL(1, ws_server_register_fid_queue("q.one", q));
}

TEST(WsServerFidQueue, RegisterDuplicateQueueFails) {
    void *q = ws_hal_queue_create(4, sizeof(void *));
    ws_server_register_fid_queue("q.two", q);
    LONGS_EQUAL(0, ws_server_register_fid_queue("q.two", q));
}

TEST(WsServerFidQueue, TextWithQueuedFidEnqueuesNoImmediateSend) {
    /* Queue-routed FID: call enqueued for worker forwarding, no immediate reply */
    void *q = ws_hal_queue_create(4, sizeof(void *));
    ws_server_register_fid_queue("q.three", q);
    send_text(conn, "{\"FID\":\"q.three\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
}

/* ── TEST_GROUP: WsServerMultiClient ─────────────────────────────────── */

TEST_GROUP(WsServerMultiClient) {
    ws_conn_t *conn1;
    ws_conn_t *conn2;
    void setup() {
        ws_hal_stub_reset();
        ws_server_init();
        conn1 = ws_conn_stub_make(1);
        conn2 = ws_conn_stub_make(2);
        ws_server_on_connect(conn1);
        ws_server_on_connect(conn2);
    }
    void teardown() {
        ws_server_on_disconnect(conn1);
        ws_server_on_disconnect(conn2);
        ws_server_test_reset();
        free(conn1);
        free(conn2);
    }
};

TEST(WsServerMultiClient, ErrorReplyGoesOnlyToSender) {
    /* Unregistered FID → NO_HANDLER sent only to conn1, not conn2 */
    send_text(conn1, "{\"FID\":\"multi.unknown\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_conn_stub_send_calls(conn1));
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn2));
}

TEST(WsServerMultiClient, PingReplyGoesOnlyToSender) {
    ws_server_on_frame(conn1, WS_FRAME_PING, heap_str("p"), 1, 1);
    LONGS_EQUAL(1, ws_conn_stub_send_calls(conn1));
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn2));
}

TEST(WsServerMultiClient, DisconnectOneClientDoesNotBreakOther) {
    ws_server_on_disconnect(conn1);
    /* conn2 can still receive frames normally */
    ws_server_on_frame(conn2, WS_FRAME_PING, heap_str("p"), 1, 1);
    LONGS_EQUAL(1, ws_conn_stub_send_calls(conn2));
}

/* ── Additional WsServerRegister tests ───────────────────────────────── */

TEST(WsServerRegister, ReregisterAfterUnregisterSucceeds) {
    bApiCallRegister(dummy_handler, "reg.four", NULL);
    bApiCallUnregister("reg.four");
    LONGS_EQUAL(1, bApiCallRegister(dummy_handler, "reg.four", NULL));
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
    ws_conn_t *conn;
    void *periph_q;
    void setup() {
        ws_hal_stub_reset();
        ws_server_init();
        conn = ws_conn_stub_make(1);
        ws_server_on_connect(conn);
        periph_q = ws_hal_queue_create(4, sizeof(webapi_msg_t *));
        bApiCallRegister(dummy_handler, REG_FID_HANDLER, NULL);
        ws_server_register_fid_queue(REG_FID_QUEUE, periph_q);
    }
    void teardown() {
        webapi_msg_t *msg;
        while (ws_hal_queue_receive(periph_q, &msg, WS_HAL_WAIT_NONE)) {
            free(msg->data);
            free(msg);
        }
        ws_server_on_disconnect(conn);
        ws_server_test_reset();
        free(conn);
    }
};

TEST(WsServerRegression, PingFailOnLastPendingCallDoesNotTouchNullSession) {
    /* vServeApiCall: a failed ping calls vApiCallComplete(), which drops
     * ulCallPending 1 -> 0 and clears call->session. The code used to fall
     * through and store ulPingTs into NULL->ulPingTs (crash on target:
     * StoreProhibited, EXCVADDR=0x0000000c; on the 64-bit host ASan reports
     * the address as 0x10). */
    send_text(conn, "{\"FID\":\"regx.handler\",\"FLAGS\":2,\"SID\":1}");
    ws_conn_stub_set_send_result(conn, -1);
    ws_hal_stub_advance_tick(1000);   /* >= CONFIG_WEB_SOCKET_PING_DELAY */

    ws_server_test_worker_step();

    /* Only the failed ping was attempted. The call is dropped later in the
     * same step (FID has no queue -> INVALID), and that status is not sent
     * because the session is already gone. */
    LONGS_EQUAL(1, ws_conn_stub_send_calls(conn));
}

TEST(WsServerRegression, ToDeleteFreesSameCallQueuedRightAfterIt) {
    /* The worker cached `next` before handling TO_DELETE, then freed every
     * call with the same fd/FID/SID — including `next` when the client sent
     * the target call right after the delete. `item = next` then walked
     * freed memory. */
    send_text(conn, "{\"FID\":\"regx.queue\",\"FLAGS\":8,\"SID\":7}");
    send_text(conn, "{\"FID\":\"regx.queue\",\"FLAGS\":4,\"SID\":7}");

    ws_server_test_worker_step();

    /* Снимается ТОЛЬКО вызов с флагом удаления; второй (подписка) живёт и уходит
     * в очередь периферии — раньше по совпадению fd/FID/SID сносило оба. */
    LONGS_EQUAL(1, ws_hal_stub_queue_count(periph_q));
    LONGS_EQUAL(1, ws_conn_stub_send_calls(conn));   /* DELIVERED отданному запросу */

    /* Снятие подтверждается клиенту следующим тактом (такт работы идёт в начале
     * шага, а на первом шаге тик ещё нулевой). */
    ws_hal_stub_advance_tick(20);
    ws_server_test_worker_step();
    LONGS_EQUAL(2, ws_conn_stub_send_calls(conn));   /* DELIVERED о снятии */
    LONGS_EQUAL(1, ws_hal_stub_queue_count(periph_q));
}
