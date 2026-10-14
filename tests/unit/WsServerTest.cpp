/* WsServer test list (delete each line as its TEST is written):
 * [x] Init — ws_server_init() creates exactly one worker task
 *
 * [x] Register — bApiCallRegister succeeds for valid handler + FID
 * [x] Register — NULL handler → fails
 * [x] Register — FID 0 → fails
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
 * [x] Frame — вложенный JSON отбрасывается без падения и без утечки
 * [x] Frame — фрагментированное сообщение собирается из частей
 * [x] Frame — продолжение без начала отбрасывается
 * [x] Frame — незавершённый сбор бросается при начале нового сообщения
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
static uint8_t long_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)call; (void)ctx; (void)data; (void)len;
    if (!data) long_handler_pulses++;
    return long_handler_pulses < 3;
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
    LONGS_EQUAL(1, bApiCallRegister(dummy_handler, 0x1001, NULL));
}

TEST(WsServerRegister, NullHandlerFails) {
    LONGS_EQUAL(0, bApiCallRegister(NULL, 0x1001, NULL));
}

TEST(WsServerRegister, FidZeroFails) {
    LONGS_EQUAL(0, bApiCallRegister(dummy_handler, 0, NULL));
}

TEST(WsServerRegister, DuplicateFidFails) {
    bApiCallRegister(dummy_handler, 0x1002, NULL);
    LONGS_EQUAL(0, bApiCallRegister(dummy_handler, 0x1002, NULL));
}

TEST(WsServerRegister, UnregisterKnownFidSucceeds) {
    bApiCallRegister(dummy_handler, 0x1003, NULL);
    LONGS_EQUAL(1, bApiCallUnregister(0x1003));
}

TEST(WsServerRegister, UnregisterUnknownFidFails) {
    LONGS_EQUAL(0, bApiCallUnregister(0x9999));
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
    send_text(conn, "{\"FID\":1000,\"SID\":1}");
    LONGS_EQUAL(1, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFrame, TextWithoutSidIsAccepted) {
    /* SID назначает клиент и он необязателен: сервер лишь повторяет его в
     * ответе, если тот был. Кадр без SID — полноценный запрос. */
    bApiCallRegister(dummy_handler, 0x2005, NULL);
    send_text(conn, "{\"FID\":8197,\"FLAGS\":0}");  /* 0x2005 */
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFrame, TextUnregisteredFidSendsNoHandler) {
    send_text(conn, "{\"FID\":9999,\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFrame, TextRegisteredHandlerEnqueuesCallNoImmediateSend) {
    /* Handler is registered: call goes into the work queue.
     * The worker task is stubbed out — no DELIVERED status yet. */
    bApiCallRegister(dummy_handler, 0x2001, NULL);
    send_text(conn, "{\"FID\":8193,\"FLAGS\":0,\"SID\":5}");  /* 0x2001 */
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFrame, PongFrameNoReply) {
    /* PONG is handled silently — only alive-ts updated, no send */
    ws_server_on_frame(conn, WS_FRAME_PONG, NULL, 0, 1);
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFrame, TextFidZeroDroppedSilently) {
    /* FID=0 is API_HANDLER_ID_GENERAL — dropped without any reply */
    send_text(conn, "{\"FID\":0,\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFrame, TextFidAsHexStringRecognized) {
    /* "FID":"0x2002" — hex string path, same as decimal for routing */
    bApiCallRegister(dummy_handler, 0x2002, NULL);
    send_text(conn, "{\"FID\":\"0x2002\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFrame, TextNoArgFieldStillEnqueues) {
    /* ARG is optional — missing ARG should not block enqueueing */
    bApiCallRegister(dummy_handler, 0x2003, NULL);
    send_text(conn, "{\"FID\":8195,\"FLAGS\":0,\"SID\":1}");  /* 0x2003 */
    LONGS_EQUAL(0, ws_conn_stub_send_calls(conn));
}

TEST(WsServerFrame, DisconnectedCallNoLongerReceivesBroadcast) {
    /* After disconnect, ws_server_test_reset clears enqueued call.
     * Verify bCallFidMatch skips nulled sessions: targets == 0. */
    bApiCallRegister(dummy_handler, 0x2004, NULL);
    send_text(conn, "{\"FID\":8196,\"FLAGS\":0,\"SID\":1}");  /* 0x2004 */
    ws_server_on_disconnect(conn);
    uint8_t buf[] = "{}";
    LONGS_EQUAL(0, bApiCallSendJsonFidGroup(0x2004, buf, 2));
}

TEST(WsServerFrame, DeeplyNestedJsonIsDroppedSafely) {
    /* Ровно то, что роняло устройство: cJSON разбирал это рекурсивно и на
     * глубине ~400 заканчивал стек задачи httpd. Разбор на jsmn итеративный,
     * токенов на такую вложенность не хватает — кадр отбрасывается, память под
     * него освобождается (проверяет LeakSanitizer). */
    std::string deep = "{\"FID\":1,\"FLAGS\":0,\"SID\":1,\"ARG\":";
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
        bApiCallRegister(dummy_handler, 0x2008, NULL);
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
    const char *p1 = "{\"FID\":8200,\"FLAGS\":0,";
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
    ws_server_on_frame(conn, WS_FRAME_TEXT, heap_str("{\"FID\":8200,"), 12, 0);
    send_text(conn, "{\"FID\":9999,\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_conn_stub_send_calls(conn));   /* ответ NO_HANDLER */
}

TEST(WsServerFragments, DisconnectReleasesPartialAssembly) {
    /* Незавершённый сбор не должен утечь при обрыве соединения (LeakSanitizer). */
    ws_server_on_frame(conn, WS_FRAME_TEXT, heap_str("{\"FID\":8200,"), 12, 0);
    ws_server_on_disconnect(conn);
    ws_conn_set_user_data(conn, NULL);
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
    bApiCallRegister(long_handler, 0x2006, NULL);
    send_text(conn, "{\"FID\":8198,\"FLAGS\":0,\"SID\":1}");  /* 0x2006 */
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
    bApiCallRegister(long_handler, 0x2007, NULL);
    send_text(conn, "{\"FID\":8199,\"FLAGS\":0,\"SID\":1}");  /* 0x2007 */
    ws_server_test_worker_step();
    const int base = long_handler_pulses;
    for (int i = 0; i < 3; i++) {
        ws_hal_stub_advance_tick(20);
        ws_server_test_work_step();
    }
    LONGS_EQUAL(base + 3, long_handler_pulses);
    for (int i = 0; i < 5; i++) {
        ws_hal_stub_advance_tick(20);
        ws_server_test_work_step();
    }
    LONGS_EQUAL(base + 3, long_handler_pulses);
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

TEST(WsServerFidQueue, RegisterQueueSucceeds) {
    void *q = ws_hal_queue_create(4, sizeof(void *));
    LONGS_EQUAL(1, ws_server_register_fid_queue(0x3001, q));
}

TEST(WsServerFidQueue, RegisterDuplicateQueueFails) {
    void *q = ws_hal_queue_create(4, sizeof(void *));
    ws_server_register_fid_queue(0x3002, q);
    LONGS_EQUAL(0, ws_server_register_fid_queue(0x3002, q));
}

TEST(WsServerFidQueue, TextWithQueuedFidEnqueuesNoImmediateSend) {
    /* Queue-routed FID: call enqueued for worker forwarding, no immediate reply */
    void *q = ws_hal_queue_create(4, sizeof(void *));
    ws_server_register_fid_queue(0x3003, q);
    send_text(conn, "{\"FID\":12291,\"FLAGS\":0,\"SID\":1}");  /* 0x3003 */
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
    send_text(conn1, "{\"FID\":9998,\"FLAGS\":0,\"SID\":1}");
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
    bApiCallRegister(dummy_handler, 0x4001, NULL);
    bApiCallUnregister(0x4001);
    LONGS_EQUAL(1, bApiCallRegister(dummy_handler, 0x4001, NULL));
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
    /* pxApiCall=NULL and ulFid=0 → invalid combination, returns 0 */
    LONGS_EQUAL(0, bApiCallSendStatus(NULL, 0));
}

TEST(WsServerApiCalls, SendJsonNullCallReturnsFail) {
    uint8_t buf[] = "{}";
    LONGS_EQUAL(0, bApiCallSendJson(NULL, buf, 2));
}

TEST(WsServerApiCalls, SendJsonFidGroupNoSubscribersReturnsFail) {
    uint8_t buf[] = "{}";
    LONGS_EQUAL(0, bApiCallSendJsonFidGroup(0xDEAD, buf, 2));
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

#define REG_FID_HANDLER  0x6001   /* handler only, no peripheral queue */
#define REG_FID_QUEUE    0x6002   /* routed to periph_q                */

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
    send_text(conn, "{\"FID\":24577,\"FLAGS\":2,\"SID\":1}");  /* 0x6001 */
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
    send_text(conn, "{\"FID\":24578,\"FLAGS\":8,\"SID\":7}");  /* 0x6002 */
    send_text(conn, "{\"FID\":24578,\"FLAGS\":4,\"SID\":7}");

    ws_server_test_worker_step();

    /* Both calls are gone: nothing forwarded, one DELIVERED for the delete */
    LONGS_EQUAL(0, ws_hal_stub_queue_count(periph_q));
    LONGS_EQUAL(1, ws_conn_stub_send_calls(conn));

    ws_server_test_worker_step();
    LONGS_EQUAL(0, ws_hal_stub_queue_count(periph_q));
    LONGS_EQUAL(1, ws_conn_stub_send_calls(conn));
}
