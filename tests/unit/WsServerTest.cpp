/* WsServer test list (delete each line as its TEST is written):
 * [x] Init — api_init() не создаёт ни задачи, ни очереди
 *
 * [x] Register — api_register_handler succeeds for valid handler + FID
 * [x] Register — NULL handler → fails
 * [x] Register — пустое/слишком длинное имя → fails
 * [x] Register — duplicate FID → fails
 *
 * Контракт подъёма/снятия ядра:
 * [x] api_init(NULL, system) → NULL
 * [x] api_init(transport, NULL) → NULL
 * [x] api_init с неполной таблицей методов транспорта → NULL
 * [x] api_init с неполной таблицей системы → NULL (каждая из пяти операций)
 * [x] api_init заводит свой дескриптор на каждый объект транспорта
 * [x] api_init заводит РОВНО ОДИН системный мьютекс на ядро
 * [x] api_deinit снимает подписку: транспорт не отдаёт ему соединения
 * [x] api_deinit(NULL) не падает
 *
 * Система (api_system_t: мьютекс, время, журнал):
 * [x] Ядро не работает без мьютекса системы: после такта счётчик взятий > 0
 * [x] Ядро не заводит ничего своего: ни задач, ни очередей
 * [x] api_worker — ровно один проход, без ожиданий
 *
 * Транспорт (стаб — реализация transport_iface_t):
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
 * Доставка запроса обработчику (единственный путь — вызов обработчика,
 * очередей периферии у ядра нет):
 * [x] Зарегистрированный обработчик получает запрос только в такте
 * [x] Обработчик, вернувший TRUE, получает вызов снова; FALSE — вызов закрыт
 * [x] Обработчик, ответивший сразу, закрывает вызов в том же такте
 *
 * Такт и ответ клиенту (ядро потоков и очередей не заводит):
 * [x] api_worker без данных ничего не делает и не падает
 * [x] Поздний ответ (api_reply из следующего такта) доходит до клиента
 * [x] Поздний ответ ушедшему клиенту не падает и никому не уходит
 *
 * Два ядра (точка доступа и станция), по одному на транспорт:
 * [x] Каждое ядро обслуживает свои соединения и свой контекст регистрации
 * [x] Сервис, зарегистрированный только на AP-ядре, станции не оказан
 * [x] Один FID на обоих ядрах разрешён, повтор имени на одном ядре — нет
 * [x] Рассылка по FID уходит подписчикам только своего ядра
 * [x] Рассылка на ядре без подписчиков — «некому»
 * [x] Поздний ответ остаётся на своём ядре (ответ идёт через своё ядро)
 * [x] Один обработчик может обслуживать оба ядра
 * [x] Стаб транспорта задаёт интерфейс соединения
 * [x] Интерфейс, который не обслуживает никто, → соединение закрывается
 *
 * Regression (built with ASan/UBSan, see Makefile):
 * [x] Отправка не прошла у снятого вызова → нет записи через NULL-сессию
 * [x] TO_DELETE followed by same fid/SID call → both freed, no UAF
 * [x] TO_DELETE снимает вызов с этим SID и не трогает соседний
 */

extern "C" {
#include "api.h"
#include "transport.h"
#include "platform.h"
#include "net_iface.h"
}
#include "system_stub.h"
#include "transport_stub.h"
#include "CppUTest/TestHarness.h"

#include <stdlib.h>
#include <string.h>

/* Отдельных тест-хуков у ядра нет: такт — это публичный api_worker (его зовёт
 * задача приложения), и тест зовёт ровно его, потому что в хосте задачи нет.
 * Проверяется то, что увидит прошивка.
 */

/* ── Helpers ──────────────────────────────────────────────────────────── */

/* Своё ядро у каждого теста: его заводит api_init в setup() и освобождает
 * api_deinit в teardown(). Ядро — экземпляр, поэтому тесты друг другу не мешают. */

/* Контракт обработчика: (ядро, вызов, контекст, данные, длина).
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
 * соединение. Ядро заводит api_init над объектом точки доступа, снимает его
 * api_deinit, а дескрипторы соединений освобождает ws_transport_stub_reset(). */
#define TRANSPORT_SETUP()                                    \
    do {                                                     \
        system_stub_reset();                               \
        ws_transport_stub_reset();                           \
        api = ws_transport_stub_init();                      \
    } while (0)

/* Порядок важен: сначала сбрасываем транспорт (он доводит до конца отложенные
 * отправки, чтобы ядро освободило свои буферы ответов), и только потом снимаем
 * само ядро. api_deinit отписывается от транспорта, поэтому висячей подписки не
 * остаётся. */
#define TRANSPORT_TEARDOWN()                                 \
    do {                                                     \
        ws_transport_stub_reset();                           \
        api_deinit(api);                                     \
        api = NULL;                                          \
    } while (0)

/* ── TEST_GROUP: Init ─────────────────────────────────────────────────── */

TEST_GROUP(WsServerInit) {
    api_t *api;
    void setup() {
        system_stub_reset();
        ws_transport_stub_reset();
        api = NULL;
    }
    void teardown() {
        if (api != NULL) {
            ws_transport_stub_reset();
            api_deinit(api);
            api = NULL;
        }
    }
};

TEST(WsServerInit, InitCreatesNoWorkerTask) {
    /* Ядро не заводит ни задачи, ни очереди: такт ему даёт вызывающий
     * (api_worker), а ответ собирает и отправляет сам обработчик. Поэтому подъём
     * ядра не создаёт в системе ни одного объекта: всё платформенное приходит
     * таблицей. */
    api = ws_transport_stub_init();
    CHECK(api != NULL);
    LONGS_EQUAL(0, system_stub_task_created_count());
}

/* ── TEST_GROUP: Register ─────────────────────────────────────────────── */

TEST_GROUP(WsServerRegister) {
    api_t *api;
    void setup() {
        TRANSPORT_SETUP();
    }
    void teardown() {
        TRANSPORT_TEARDOWN();
    }
};

TEST(WsServerRegister, ValidHandlerSucceeds) {
    LONGS_EQUAL(1, api_register_handler(api, dummy_handler, "reg.one", NULL));
}

TEST(WsServerRegister, NullHandlerFails) {
    LONGS_EQUAL(0, api_register_handler(api, NULL, "reg.one", NULL));
}

TEST(WsServerRegister, EmptyNameFails) {
    /* Пустое имя — это «FID не задан» */
    LONGS_EQUAL(0, api_register_handler(api, dummy_handler, "", NULL));
    LONGS_EQUAL(0, api_register_handler(api, dummy_handler, NULL, NULL));
}

TEST(WsServerRegister, TooLongNameFails) {
    /* Имя длиннее буфера отвергается целиком, а не обрезается: обрезанное имя
     * увело бы вызов не тому обработчику. */
    char long_name[API_FID_NAME_MAX + 8];
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    LONGS_EQUAL(0, api_register_handler(api, dummy_handler, long_name, NULL));
}

TEST(WsServerRegister, DuplicateNameFails) {
    api_register_handler(api, dummy_handler, "reg.two", NULL);
    LONGS_EQUAL(0, api_register_handler(api, dummy_handler, "reg.two", NULL));
}

/* ── TEST_GROUP: Подключение транспорта ───────────────────────────────── */

TEST_GROUP(WsServerTransportAttach) {
    api_t *api;
    void setup() {
        system_stub_reset();
        ws_transport_stub_reset();
        api = NULL;
    }
    void teardown() {
        if (api != NULL) {
            ws_transport_stub_reset();
            api_deinit(api);
            api = NULL;
        }
    }
};

TEST(WsServerTransportAttach, NullTransportIsRejected) {
    /* Ядру нечего обслуживать без транспорта: подъём возвращает NULL, а не
     * дескриптор, которым некуда отдавать события. */
    POINTERS_EQUAL(NULL, api_init(NULL, system_stub_api_system()));
}

TEST(WsServerTransportAttach, NullSystemIsRejected) {
    /* Система — второй обязательный объект: без мьютекса, времени и журнала ядро
     * не может работать ни в одном сценарии. Отказ честный: NULL, а не ядро,
     * которое упадёт на первом же вызове. */
    POINTERS_EQUAL(NULL, api_init(ws_transport_stub_object(GW_IFACE_AP), NULL));
}

TEST(WsServerTransportAttach, SystemWithoutRequiredOpsIsRejected) {
    /* Неполная таблица системы — не система. Проверяем КАЖДУЮ из пяти операций:
     * ядру нужны все, и другого способа получить их у него нет (ни очередей, ни
     * задержек, ни потоков оно не просит). Таблица «портится» копией, поэтому
     * видно, что отказ даёт именно отсутствующая операция. */
    api_system_t broken = *system_stub_api_system();

    broken.mutex_create = NULL;
    POINTERS_EQUAL(NULL, api_init(ws_transport_stub_object(GW_IFACE_AP), &broken));

    broken = *system_stub_api_system();
    broken.mutex_take = NULL;
    POINTERS_EQUAL(NULL, api_init(ws_transport_stub_object(GW_IFACE_AP), &broken));

    broken = *system_stub_api_system();
    broken.mutex_give = NULL;
    POINTERS_EQUAL(NULL, api_init(ws_transport_stub_object(GW_IFACE_AP), &broken));

    broken = *system_stub_api_system();
    broken.time_ms = NULL;
    POINTERS_EQUAL(NULL, api_init(ws_transport_stub_object(GW_IFACE_AP), &broken));

    broken = *system_stub_api_system();
    broken.log = NULL;
    POINTERS_EQUAL(NULL, api_init(ws_transport_stub_object(GW_IFACE_AP), &broken));
}

TEST(WsServerTransportAttach, TransportWithoutRequiredOpsIsRejected) {
    /* Без begin_send/close/subscribe ядру работать нечем: половина интерфейса
     * означала бы отказ уже в момент запроса клиента. Проверяем каждую из трёх
     * обязательных операций: нехватка любой — отказ. */
    void *module = ws_transport_stub_module(GW_IFACE_AP);
    for (int op = 0; op < 3; op++) {
        transport_iface_t broken = *ws_transport_stub_iface();
        switch (op) {
        case 0: broken.begin_send = NULL; break;
        case 1: broken.close      = NULL; break;
        default: broken.subscribe = NULL; break;
        }
        transport_t t = { &broken, module };
        POINTERS_EQUAL(NULL, api_init(&t, system_stub_api_system()));
    }

    /* Объект без таблицы методов — тоже не транспорт: подписываться не на что. */
    transport_t no_iface = { NULL, module };
    POINTERS_EQUAL(NULL, api_init(&no_iface, system_stub_api_system()));
}

TEST(WsServerTransportAttach, StubAttachSucceeds) {
    api = ws_transport_stub_init();
    CHECK(api != NULL);
}

/* ── TEST_GROUP: система (api_system_t) ───────────────────────────────── */

/* Ядро просит у системы РОВНО три вещи — мьютекс, чтение таймера и журнал, — и
 * живёт только на них: своих потоков, очередей и задержек у него нет. Здесь
 * проверяется, что без мьютекса системы ядро не работает вовсе, и что подъём
 * ядра не заводит в системе ничего лишнего. */
TEST_GROUP(WsServerSystem) {
    api_t *api;
    ws_transport_stub_conn_t *conn;
    void setup() {
        system_stub_reset();
        ws_transport_stub_reset();
        api = NULL;
        conn = NULL;
    }
    void teardown() {
        if (api != NULL) {
            ws_transport_stub_reset();
            api_deinit(api);
            api = NULL;
        }
    }
};

TEST(WsServerSystem, InitCreatesExactlyOneSystemMutex) {
    /* Мьютекс — единственная синхронизация ядра, и заводит его СИСТЕМА: подъём
     * ядра создаёт ровно один мьютекс (не по одному на список и не задачу). */
    api = ws_transport_stub_init();
    CHECK(api != NULL);
    LONGS_EQUAL(1, system_stub_mutex_creates());
    LONGS_EQUAL(1, system_stub_mutex_alive());
}

TEST(WsServerSystem, WorkerTakesSystemMutex) {
    /* Работа без единого вызова mutex_* со стороны системы невозможна: ядро не
     * имеет своей синхронизации, и любой такт — это взятие и отпускание
     * системного мьютекса (при создании мьютекса взятий ещё ноль). */
    api = ws_transport_stub_init();
    CHECK(api != NULL);
    LONGS_EQUAL(0, system_stub_mutex_takes());

    conn = ws_transport_stub_connect(1);
    send_text(conn, "{\"FID\":\"sys.mutex\",\"FLAGS\":0,\"SID\":1}");  /* NO_HANDLER */
    api_worker(api);

    CHECK(system_stub_mutex_takes() > 0);
}

TEST(WsServerSystem, WorkerWithoutWorkSendsNothingAndTakesMutex) {
    /* Пустой такт: работы нет — ядро ничего не отправляет и не падает, но
     * системным мьютексом всё равно пользуется (другого способа смотреть на
     * состояние у него нет). */
    api = ws_transport_stub_init();
    CHECK(api != NULL);
    conn = ws_transport_stub_connect(1);

    const uint32_t before = system_stub_mutex_takes();
    for (int i = 0; i < 3; i++) {
        system_stub_advance_time(20);
        api_worker(api);
    }
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
    CHECK(system_stub_mutex_takes() > before);
}

/* ── TEST_GROUP: жизнь дескриптора ядра ───────────────────────────────── */

/* Ядро создаёт дескриптор само (api_init) и само его освобождает (api_deinit).
 * Здесь проверяется именно этот контракт: сколько объектов транспорта — столько
 * независимых ядер, и снятое ядро не оставляет после себя подписки. */
TEST_GROUP(WsServerCoreLifecycle) {
    api_t *api;
    void setup() {
        system_stub_reset();
        ws_transport_stub_reset();
        api = NULL;
    }
    void teardown() {
        if (api != NULL) {
            ws_transport_stub_reset();
            api_deinit(api);
            api = NULL;
        }
    }
};

TEST(WsServerCoreLifecycle, EachTransportObjectGetsItsOwnDescriptor) {
    /* Два объекта транспорта — два независимых ядра: дескрипторы разные, и
     * регистрация на одном не видна другому. */
    api_t *ap  = api_init(ws_transport_stub_object(GW_IFACE_AP),  system_stub_api_system());
    api_t *sta = api_init(ws_transport_stub_object(GW_IFACE_STA), system_stub_api_system());
    CHECK(ap != NULL);
    CHECK(sta != NULL);
    CHECK(ap != sta);

    /* Регистрация на AP-ядре станции не видна: повтор того же имени на STA-ядре
     * проходит, а на AP — отвергается. Так видно, что списки регистраций у ядер
     * свои, без всякого «снять и проверить». */
    LONGS_EQUAL(1, api_register_handler(ap, dummy_handler, "life.one", NULL));
    LONGS_EQUAL(1, api_register_handler(sta, dummy_handler, "life.one", NULL));
    LONGS_EQUAL(0, api_register_handler(ap, dummy_handler, "life.one", NULL));
    LONGS_EQUAL(0, system_stub_task_created_count());    /* ядра задач не заводят */

    ws_transport_stub_reset();
    api_deinit(ap);
    api_deinit(sta);
}

TEST(WsServerCoreLifecycle, DeinitRemovesSubscription) {
    /* После api_deinit транспорт не должен помнить подписчика: соединение,
     * пришедшее позже, отдавать некому — оно закрывается, а не уходит в
     * освобождённое ядро (иначе это была бы UAF на первом же событии). */
    api = ws_transport_stub_init();
    CHECK(api != NULL);
    api_deinit(api);
    api = NULL;

    ws_transport_stub_conn_t *conn = ws_transport_stub_connect(3);
    LONGS_EQUAL(1, ws_transport_stub_close_calls(conn));
    LONGS_EQUAL(1, ws_transport_stub_closed(conn));
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerCoreLifecycle, DeinitNullIsSafe) {
    /* Снимать нечего — не ошибка: вызывающему не нужно помнить, поднялось ли
     * ядро, чтобы безопасно позвать api_deinit. */
    api_deinit(NULL);
}

TEST(WsServerCoreLifecycle, EventsAfterDeinitDoNotReachCore) {
    /* Отписка — свойство делегата, а не операция транспорта: api_deinit снимает
     * свой делегат из event_t транспорта. Соединение при этом живо, и его
     * события продолжают подниматься — но до освобождённого ядра не доходят
     * (иначе первый же кадр был бы обращением к освобождённой памяти). */
    api = ws_transport_stub_init();
    CHECK(api != NULL);
    ws_transport_stub_conn_t *conn = ws_transport_stub_connect(4);
    LONGS_EQUAL(0, ws_transport_stub_close_calls(conn));   /* соединение принято ядром */

    api_deinit(api);
    api = NULL;

    send_text(conn, "{\"FID\":\"life.after\",\"FLAGS\":0,\"SID\":1}");
    /* Ни ответа NO_HANDLER, ни других отправок: ядро уже не подписано. */
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
    LONGS_EQUAL(0, ws_transport_stub_close_calls(conn));
}

/* ── TEST_GROUP: Соединения ───────────────────────────────────────────── */

TEST_GROUP(WsServerConnect) {
    api_t *api;
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
    api_register_handler(api, dummy_handler, "conn.calls", NULL);
    send_text(conn, "{\"FID\":\"conn.calls\",\"FLAGS\":0,\"SID\":1}");

    ws_transport_stub_disconnect(conn);

    /* Вызовов у соединения больше нет: рассылка по FID не находит получателей */
    uint8_t buf[] = "{}";
    LONGS_EQUAL(0, api_broadcast(api, "conn.calls", buf, 2));
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
        system_stub_advance_time(10000);
        api_worker(api);
    }
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

/* ── TEST_GROUP: Frame ────────────────────────────────────────────────── */

TEST_GROUP(WsServerFrame) {
    api_t *api;
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
    api_register_handler(api, dummy_handler, "frm.sid", NULL);
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
    api_register_handler(api, dummy_handler, "frm.one", NULL);
    send_text(conn, "{\"FID\":\"frm.one\",\"FLAGS\":0,\"SID\":5}");
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerFrame, NumericFidIsRejected) {
    /* Протокол работает только с именами: число в FID — не имя, сообщение
     * отклоняется молча, как и любое другое без корректного FID. */
    api_register_handler(api, dummy_handler, "frm.numeric", NULL);
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
    api_reply(call, body, sizeof(body) - 1);
    return 0;
}

TEST(WsServerFrame, ReplyCarriesSameFidName) {
    /* Клиент присылает имя — сервер возвращает ровно его же, а не код. */
    api_register_handler(api, answer_handler, "uart2.modbus", NULL);
    send_text(conn, "{\"FID\":\"uart2.modbus\",\"FLAGS\":0,\"SID\":9}");
    api_worker(api);
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
    api_register_handler(api, dummy_handler, "uart2.modbus", NULL);
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
    api_register_handler(api, dummy_handler, "frm.noarg", NULL);
    send_text(conn, "{\"FID\":\"frm.noarg\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerFrame, DisconnectedCallNoLongerReceivesBroadcast) {
    /* After disconnect, the session is released and the call is unbound.
     * Verify call_fid_match skips nulled sessions: targets == 0. */
    api_register_handler(api, dummy_handler, "frm.bcast", NULL);
    send_text(conn, "{\"FID\":\"frm.bcast\",\"FLAGS\":0,\"SID\":1}");
    ws_transport_stub_disconnect(conn);
    uint8_t buf[] = "{}";
    LONGS_EQUAL(0, api_broadcast(api, "frm.bcast", buf, 2));
}

TEST(WsServerFrame, DeeplyNestedJsonIsDroppedSafely) {
    /* Глубоко вложенный пакет не должен съесть стек: разбор на jsmn
     * итеративный, токенов на такую вложенность не хватает — сообщение
     * отбрасывается, память под него освобождает транспорт (проверяет
     * LeakSanitizer). */
    std::string deep = "{\"FID\":\"frm.deep\",\"FLAGS\":0,\"SID\":1,\"ARG\":";
    deep.append(500, '[');
    deep.append(500, ']');
    deep += "}";
    send_text(conn, deep.c_str());
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

/* ── TEST_GROUP: Склейка фрагментов (делает транспорт) ────────────────── */

TEST_GROUP(WsServerFragments) {
    api_t *api;
    ws_transport_stub_conn_t *conn;
    void setup() {
        TRANSPORT_SETUP();
        conn = ws_transport_stub_connect(1);
        api_register_handler(api, dummy_handler, "frag.one", NULL);
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
    std::string big(TRANSPORT_STUB_FRAME_MAX_LEN + 1, 'a');
    ws_transport_stub_frame(conn, WS_STUB_FRAME_TEXT, big.data(), big.size(), 1);
    LONGS_EQUAL(1, ws_transport_stub_close_calls(conn));
    LONGS_EQUAL(1, ws_transport_stub_closed(conn));
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerFragments, OversizedAssembledMessageClosesConnection) {
    /* Предел собранного сообщения тот же, что у одиночного кадра: как только
     * суммарная длина его превышает, соединение закрывается. */
    std::string big(TRANSPORT_STUB_MESSAGE_MAX_LEN, 'a');
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
    api_t *api;
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
    api_register_handler(api, dummy_handler, "send.bcast", NULL);
    send_text(conn,  "{\"FID\":\"send.bcast\",\"FLAGS\":4,\"SID\":1}");
    send_text(conn2, "{\"FID\":\"send.bcast\",\"FLAGS\":4,\"SID\":2}");
    ws_transport_stub_set_deferred(conn, 1);
    ws_transport_stub_set_deferred(conn2, 1);

    api_worker(api);
    LONGS_EQUAL(1, ws_transport_stub_pending_count(conn));
    LONGS_EQUAL(1, ws_transport_stub_pending_count(conn2));
    STRCMP_CONTAINS("\"FID\":\"send.bcast\"", ws_transport_stub_last_sent(conn));
    STRCMP_CONTAINS("\"FID\":\"send.bcast\"", ws_transport_stub_last_sent(conn2));

    ws_transport_stub_complete_pending(conn, TRANSPORT_OK);
    LONGS_EQUAL(0, ws_transport_stub_pending_count(conn));
    /* Второй получатель ещё не завершил отправку — буфер жив */
    ws_transport_stub_complete_pending(conn2, TRANSPORT_OK);
    LONGS_EQUAL(0, ws_transport_stub_pending_count(conn2));
}

TEST(WsServerSend, TransferErrClosesConnectionAndReleasesCalls) {
    /* Отправка не прошла — соединение считаем мёртвым: ядро просит транспорт его
     * закрыть, а вызовы этого соединения снимает. */
    teardown_ask = 0;
    api_register_handler(api, teardown_ask_handler, "send.err", NULL);
    send_text(conn, "{\"FID\":\"send.err\",\"FLAGS\":4,\"SID\":1}");

    ws_transport_stub_set_deferred(conn, 1);
    api_worker(api);                   /* DELIVERED отложен транспортом */
    LONGS_EQUAL(1, ws_transport_stub_pending_count(conn));

    ws_transport_stub_complete_pending(conn, TRANSPORT_FAULT);   /* TRANSFER_ERR */
    LONGS_EQUAL(1, ws_transport_stub_close_calls(conn));

    /* Вызовы мёртвого соединения сняты: рассылать по этому FID больше некому */
    uint8_t buf[] = "{}";
    LONGS_EQUAL(0, api_broadcast(api, "send.err", buf, 2));

    /* Транспорт доводит закрытие до конца (у httpd — освобождение сессии) */
    ws_transport_stub_disconnect(conn);

    /* Вызов снимается тактом работы, и только после FALSE обработчика: первый
     * такт помечает вызов к снятию, второй — спрашивает обработчика. */
    system_stub_advance_time(20);
    api_worker(api);
    LONGS_EQUAL(0, teardown_ask);
    system_stub_advance_time(20);
    api_worker(api);
    LONGS_EQUAL(1, teardown_ask);
}

TEST(WsServerSend, SendRefusedByTransportReleasesBuffer) {
    /* Транспорт отказался принять отправку: события о завершении не будет,
     * буфер освобождает само ядро. */
    api_register_handler(api, dummy_handler, "send.refuse", NULL);
    ws_transport_stub_set_send_result(conn, TRANSPORT_FAULT);
    send_text(conn, "{\"FID\":\"send.refuse\",\"FLAGS\":0,\"SID\":1}");
    api_worker(api);                    /* DELIVERED не ушёл */

    LONGS_EQUAL(1, ws_transport_stub_send_attempts(conn));
    /* Вызов снят обработчиком (dummy_handler вернул FALSE) — и ничего не утекло */
    uint8_t buf[] = "{}";
    LONGS_EQUAL(0, api_broadcast(api, "send.refuse", buf, 2));
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
    api_t *api;
    ws_transport_stub_conn_t *conn;
    void setup() {
        TRANSPORT_SETUP();
        conn = ws_transport_stub_connect(1);
        slow_finish_steps = 0;
        api_register_handler(api, slow_finish_handler, "regx.slow", NULL);
    }
    void teardown() {
        TRANSPORT_TEARDOWN();
    }
};

TEST(WsServerFinishRule, CallSurvivesUntilHandlerReportsFinished) {
    send_text(conn, "{\"FID\":\"regx.slow\",\"FLAGS\":0,\"SID\":1}");
    api_worker(api);          /* первичная выдача: обработчик жив */

    ws_transport_stub_disconnect(conn);    /* клиент ушёл — вызов просят снять */
    slow_finish_steps = 0;

    /* Шаг 1 выставляет флаг снятия (GC идёт после такта), дальше каждый шаг —
     * один вопрос обработчику. */
    for (int i = 0; i < 3; i++) {
        system_stub_advance_time(20);
        api_worker(api);
    }
    LONGS_EQUAL(2, slow_finish_steps);     /* спросили дважды, вызов ещё жив */

    system_stub_advance_time(20);
    api_worker(api);          /* третий раз — обработчик вернул FALSE */
    LONGS_EQUAL(3, slow_finish_steps);

    for (int i = 0; i < 2; i++) {
        system_stub_advance_time(20);
        api_worker(api);      /* вызов снят: больше не спрашивают */
    }
    LONGS_EQUAL(3, slow_finish_steps);
}

/* ── TEST_GROUP: WsServerWorkCycle ────────────────────────────────────── */

TEST_GROUP(WsServerWorkCycle) {
    api_t *api;
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
    api_register_handler(api, long_handler, "cycle.pulse", NULL);
    send_text(conn, "{\"FID\":\"cycle.pulse\",\"FLAGS\":0,\"SID\":1}");
    api_worker(api);          /* первичный вызов — уже с данными */
    /* Один такт api_worker даёт обработчику ровно один такт работы, поэтому
     * считаем прирост, а не абсолютное значение. */
    const int base = long_handler_pulses;
    CHECK(base <= 1);
    for (int i = 0; i < 3; i++) {
        system_stub_advance_time(20);
        api_worker(api);
    }
    LONGS_EQUAL(base + 3, long_handler_pulses);
}

TEST(WsServerWorkCycle, FinishedCallGetsNoMorePulses) {
    /* Обработчик вернул 0 — вызов завершён и снят со списка: следующий такт
     * работы до него уже не доходит (иначе вызов с контекстом копился бы в
     * списке навсегда — замерено накопление 215 вызовов за минуту). */
    short_handler_pulses = 0;
    api_register_handler(api, short_handler, "cycle.done", NULL);
    send_text(conn, "{\"FID\":\"cycle.done\",\"FLAGS\":0,\"SID\":1}");
    api_worker(api);
    for (int i = 0; i < 3; i++) {
        system_stub_advance_time(20);
        api_worker(api);
    }
    LONGS_EQUAL(3, short_handler_pulses);
    for (int i = 0; i < 5; i++) {
        system_stub_advance_time(20);
        api_worker(api);
    }
    LONGS_EQUAL(3, short_handler_pulses);   /* вызов завершён — тактов больше нет */
}

/* ── TEST_GROUP: доставка запроса обработчику ─────────────────────────── */

/* Доставка у ядра ОДНА — вызов обработчика (очередей периферии у ядра нет).
 * Здесь проверяется именно она: запрос приходит в такте, а не сразу; обработчик,
 * вернувший TRUE, получает вызов снова (это и есть «я жду периферию»); TRUE на
 * первом вызове означает, что клиент получил подтверждение доставки. */

/* «Периферия», которая забирает вызов и отвечает сама, когда захочет: она
 * запоминает дескриптор вызова и возвращает TRUE — ядро оставит вызов живым. */
static void *waiting_call;
static int   waiting_calls;
static int   late_body_sent;

static uint8_t waiting_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)ctx; (void)data; (void)len;
    if (!call) return 0;                 /* снятие: держать нечего */
    waiting_call = call;
    waiting_calls++;
    return 1;                            /* «ответ будет позже» */
}

/* Та же «периферия», но отвечающая сама: на запрос — TRUE (вызов остаётся
 * живым), на следующем такте — ответ по дескриптору вызова и FALSE (вызов
 * закрыт). Так выглядит сервис со своим потоком/очередью. */
static uint8_t late_answer_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)ctx; (void)len;
    if (!call) return 0;                 /* снятие: держать нечего */
    if (data) {
        waiting_call = call;
        waiting_calls++;
        return 1;                        /* «ответ будет позже» */
    }
    const uint8_t body[] = "{\"V\":1}";
    api_reply(call, body, sizeof(body) - 1);
    late_body_sent++;
    return 0;                            /* ответ ушёл — вызов завершён */
}

/* Запрос клиента получает обработчик в такте: до такта — ни одного вызова и ни
 * одной отправки. Обработчику приходит ТОЛЬКО вызов: ядро он не получает (его
 * знает сам вызов, см. api_reply). */
static int delivered_calls;
static uint8_t delivered_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)ctx; (void)len;
    if (!call) return 0;
    if (data) delivered_calls++;
    return 0;                            /* ответили сразу — вызов закрыт */
}

TEST_GROUP(WsServerDelivery) {
    api_t *api;
    ws_transport_stub_conn_t *conn;
    void setup() {
        TRANSPORT_SETUP();
        conn = ws_transport_stub_connect(1);
        waiting_call = NULL;
        waiting_calls = 0;
        delivered_calls = 0;
    }
    void teardown() {
        TRANSPORT_TEARDOWN();
    }
};

TEST(WsServerDelivery, RequestReachesHandlerOnlyInWorker) {
    api_register_handler(api, delivered_handler, "dlv.one", NULL);
    send_text(conn, "{\"FID\":\"dlv.one\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(0, delivered_calls);                       /* такта ещё не было */
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));

    api_worker(api);
    LONGS_EQUAL(1, delivered_calls);
    /* Обработчик ответил сразу (FALSE) — вызов закрыт в том же такте. */
    LONGS_EQUAL(1, ws_transport_stub_send_attempts(conn));  /* только DELIVERED */
}

TEST(WsServerDelivery, HandlerReturningTrueGetsCallAgain) {
    api_register_handler(api, waiting_handler, "dlv.wait", NULL);
    send_text(conn, "{\"FID\":\"dlv.wait\",\"FLAGS\":0,\"SID\":7}");
    api_worker(api);
    CHECK(waiting_call != NULL);
    const int first = waiting_calls;

    /* Вызов остался жив: следующий такт снова зовёт обработчик (data == NULL —
     * это такт работы, а не новый запрос). */
    system_stub_advance_time(20);
    api_worker(api);
    CHECK(waiting_calls > first);
}

/* ── TEST_GROUP: Два ядра (точка доступа и станция) ────────────────────── */

/* В прошивке ядер два — по одному на транспорт: клиентов точки доступа
 * обслуживает AP-ядро, клиентов станционной сети — STA-ядро. Масок интерфейсов
 * в ядре нет, и «сервис только для точки доступа» — это регистрация на
 * AP-ядре. Здесь проверяется именно это.
 *
 * Два обработчика одного метода: по одному на ядро. В тестах видно, какой из них
 * вызван и с каким контекстом регистрации (по нему обработчик и понимает, через
 * какой транспорт его позвали). */
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

TEST_GROUP(WsServerTwoCores) {
    /* Два ядра теста (как app.api_ap и app.api_sta). Каждое поднято над СВОИМ
     * объектом транспорта — так же, как app.c поднимает их над транспортами,
     * порождёнными веб-серверами интерфейсов (web_server_open_websocket). */
    api_t *api_ap;
    api_t *api_sta;
    ws_transport_stub_conn_t *ap_conn;
    ws_transport_stub_conn_t *sta_conn;
    /* Контексты регистрации: у каждого ядра свой. */
    int ctx_ap, ctx_sta;
    void setup() {
        system_stub_reset();
        ws_transport_stub_reset();
        api_ap  = ws_transport_stub_init_only(GW_IFACE_AP);
        api_sta = ws_transport_stub_init_only(GW_IFACE_STA);
        ws_transport_stub_set_iface(GW_IFACE_AP);
        ap_conn = ws_transport_stub_connect(1);
        ws_transport_stub_set_iface(GW_IFACE_STA);
        sta_conn = ws_transport_stub_connect(2);
        iface_ap_calls = iface_other_calls = 0;
        iface_ap_ctx_seen = iface_other_ctx_seen = NULL;
    }
    void teardown() {
        ws_transport_stub_reset();
        api_deinit(api_ap);
        api_deinit(api_sta);
    }
};

TEST(WsServerTwoCores, EachCoreServesItsOwnConnectionAndContext) {
    /* Один метод зарегистрирован на обоих ядрах с разными контекстами: клиент
     * точки доступа получает AP-экземпляр, клиент станции — STA-экземпляр, и
     * обработчик узнаёт, через какой транспорт его позвали, по контексту. */
    LONGS_EQUAL(1, api_register_handler(api_ap,  iface_ap_handler,    "iface.fid", &ctx_ap));
    LONGS_EQUAL(1, api_register_handler(api_sta, iface_other_handler, "iface.fid", &ctx_sta));

    send_text(ap_conn, "{\"FID\":\"iface.fid\",\"FLAGS\":0,\"SID\":1}");
    api_worker(api_ap);
    LONGS_EQUAL(1, iface_ap_calls);
    LONGS_EQUAL(0, iface_other_calls);
    CHECK(iface_ap_ctx_seen == &ctx_ap);

    send_text(sta_conn, "{\"FID\":\"iface.fid\",\"FLAGS\":0,\"SID\":2}");
    api_worker(api_sta);
    LONGS_EQUAL(1, iface_ap_calls);
    LONGS_EQUAL(1, iface_other_calls);
    CHECK(iface_other_ctx_seen == &ctx_sta);
}

TEST(WsServerTwoCores, TransportIsIdentifiedByItsContext) {
    /* Транспорты порождены разными веб-серверами (AP и STA), и номера интерфейса
     * в объекте транспорта нет: экземпляры различает только контекст. */
    const transport_t *t_ap  = ws_transport_stub_object(GW_IFACE_AP);
    const transport_t *t_sta = ws_transport_stub_object(GW_IFACE_STA);
    CHECK(t_ap != NULL);
    CHECK(t_sta != NULL);
    CHECK(t_ap != t_sta);
    CHECK(t_ap->iface == t_sta->iface);          /* таблица методов у них общая */
    CHECK(t_ap->context != NULL);
    CHECK(t_sta->context != NULL);
    CHECK(t_ap->context != t_sta->context);      /* различает их контекст */

    /* Контекст и есть идентификатор: он принадлежит ровно одному экземпляру, и
     * другого объекта с этим контекстом быть не может. */
    POINTERS_EQUAL(ws_transport_stub_module(GW_IFACE_AP),  t_ap->context);
    POINTERS_EQUAL(ws_transport_stub_module(GW_IFACE_STA), t_sta->context);
    CHECK(ws_transport_stub_module(GW_IFACE_AP) != ws_transport_stub_module(GW_IFACE_STA));
}

TEST(WsServerTwoCores, TransportsAreIndependent) {
    /* Два транспорта независимы: подписка и ядро у каждого свои. Снятие AP-ядра
     * оставляет AP-соединения без сервиса, а STA-экземпляр продолжает принимать
     * свои — чужой контекст от этого ничего не теряет. */
    api_deinit(api_ap);
    api_ap = NULL;

    ws_transport_stub_set_iface(GW_IFACE_AP);
    ws_transport_stub_conn_t *ap_gone = ws_transport_stub_connect(5);
    LONGS_EQUAL(1, ws_transport_stub_close_calls(ap_gone));    /* обслуживать нечем */

    ws_transport_stub_set_iface(GW_IFACE_STA);
    ws_transport_stub_conn_t *sta_alive = ws_transport_stub_connect(6);
    LONGS_EQUAL(0, ws_transport_stub_close_calls(sta_alive));  /* своё ядро на месте */
}

TEST(WsServerTwoCores, ServiceOnApCoreIsNoHandlerForStation) {
    /* FID зарегистрирован только на AP-ядре: клиент станционной сети получает
     * внятный NO_HANDLER, а обработчик не вызывается вовсе. */
    api_register_handler(api_ap, iface_ap_handler, "iface.only.ap", &ctx_ap);

    send_text(sta_conn, "{\"FID\":\"iface.only.ap\",\"FLAGS\":0,\"SID\":1}");
    LONGS_EQUAL(1, ws_transport_stub_send_attempts(sta_conn));      /* NO_HANDLER */
    api_worker(api_sta);
    LONGS_EQUAL(0, iface_ap_calls);

    /* А клиенту точки доступа тот же сервис оказан: на его ядре он есть. */
    send_text(ap_conn, "{\"FID\":\"iface.only.ap\",\"FLAGS\":0,\"SID\":2}");
    api_worker(api_ap);
    LONGS_EQUAL(1, iface_ap_calls);
}

TEST(WsServerTwoCores, SameFidOnBothCoresAllowedButNotTwiceOnOne) {
    /* Ключ регистрации — имя FID, и он свой у каждого ядра: одно имя
     * регистрируется по разу на каждом, а повтор имени на одном ядре
     * неразличим и отвергается. */
    LONGS_EQUAL(1, api_register_handler(api_ap,  iface_ap_handler,    "iface.dup", &ctx_ap));
    LONGS_EQUAL(1, api_register_handler(api_sta, iface_other_handler, "iface.dup", &ctx_sta));
    LONGS_EQUAL(0, api_register_handler(api_ap,  iface_ap_handler,    "iface.dup", &ctx_ap));
    LONGS_EQUAL(0, api_register_handler(api_sta, iface_other_handler, "iface.dup", &ctx_sta));
}

TEST(WsServerTwoCores, BroadcastStaysOnItsCore) {
    /* Рассылка по FID адресована подписчикам этого имени на ЭТОМ ядре. Оба
     * клиента подписались на свой FID (каждый на своём ядре) — кадр, собранный
     * периферией точки доступа, получает только её подписчик. */
    api_register_handler(api_ap,  iface_ap_handler,    "iface.bcast", &ctx_ap);
    api_register_handler(api_sta, iface_other_handler, "iface.bcast", &ctx_sta);

    send_text(ap_conn,  "{\"FID\":\"iface.bcast\",\"FLAGS\":4,\"SID\":1}");
    send_text(sta_conn, "{\"FID\":\"iface.bcast\",\"FLAGS\":4,\"SID\":2}");

    const int ap_before  = ws_transport_stub_send_attempts(ap_conn);
    const int sta_before = ws_transport_stub_send_attempts(sta_conn);
    uint8_t buf[] = "{\"V\":1}";
    LONGS_EQUAL(1, api_broadcast(api_ap, "iface.bcast", buf, sizeof(buf) - 1));
    LONGS_EQUAL(ap_before + 1, ws_transport_stub_send_attempts(ap_conn));
    LONGS_EQUAL(sta_before,    ws_transport_stub_send_attempts(sta_conn));
    STRCMP_CONTAINS("\"FID\":\"iface.bcast\"", ws_transport_stub_last_sent(ap_conn));

    /* И наоборот: кадр станционного ядра не видит подписчик точки доступа. */
    LONGS_EQUAL(1, api_broadcast(api_sta, "iface.bcast", buf, sizeof(buf) - 1));
    LONGS_EQUAL(ap_before + 1, ws_transport_stub_send_attempts(ap_conn));
    LONGS_EQUAL(sta_before + 1, ws_transport_stub_send_attempts(sta_conn));
}

TEST(WsServerTwoCores, BroadcastOnCoreWithoutSubscribersHasNoRecipients) {
    /* Подписка на этом FID живёт на AP-ядре, а рассылка просит STA-ядро: там
     * подписчиков нет, и это честный отказ, а не тишина. */
    api_register_handler(api_ap, iface_ap_handler, "iface.bcast2", &ctx_ap);
    send_text(ap_conn, "{\"FID\":\"iface.bcast2\",\"FLAGS\":4,\"SID\":1}");

    uint8_t buf[] = "{\"V\":1}";
    LONGS_EQUAL(0, api_broadcast(api_sta, "iface.bcast2", buf, sizeof(buf) - 1));
}

/* «Периферия» двух ядер: забирает вызов и отвечает на следующем такте из того
 * же обработчика — своим дескриптором вызова и своим ядром (api_reply).
 * Другого пути у ответа нет: адресат — дескриптор вызова, а ядро — то, которое
 * вызвало обработчик. */
static int  iface_late_answered;
static uint8_t iface_late_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)ctx; (void)len;
    if (!call) return 0;                 /* снятие: держать нечего */
    if (data) return 1;                  /* запрос принят, ответ будет позже */
    /* Такт работы: отвечаем и завершаем вызов. */
    const uint8_t body[] = "\"LATE\"";
    api_reply(call, body, sizeof(body) - 1);
    iface_late_answered++;
    return 0;
}

TEST(WsServerTwoCores, LateAnswerStaysOnItsCore) {
    /* Поздний ответ адресован вызову и уходит через своё ядро, поэтому на чужое
     * соединение он не попадёт: вызов привязан к сессии своего ядра. Проверяем
     * именно это: каждый клиент получает ответ своего ядра, и по одному. */
    LONGS_EQUAL(1, api_register_handler(api_ap,  iface_late_handler, "iface.reply", NULL));
    LONGS_EQUAL(1, api_register_handler(api_sta, iface_late_handler, "iface.reply", NULL));
    iface_late_answered = 0;

    send_text(ap_conn,  "{\"FID\":\"iface.reply\",\"FLAGS\":0,\"SID\":7}");
    send_text(sta_conn, "{\"FID\":\"iface.reply\",\"FLAGS\":0,\"SID\":8}");
    api_worker(api_ap);
    api_worker(api_sta);
    LONGS_EQUAL(0, iface_late_answered);          /* ответа ещё не было */

    const int ap_before  = ws_transport_stub_send_attempts(ap_conn);
    const int sta_before = ws_transport_stub_send_attempts(sta_conn);

    system_stub_advance_time(20);
    api_worker(api_ap);                          /* ответ клиенту точки доступа */
    LONGS_EQUAL(1, iface_late_answered);
    LONGS_EQUAL(ap_before + 1, ws_transport_stub_send_attempts(ap_conn));
    LONGS_EQUAL(sta_before,    ws_transport_stub_send_attempts(sta_conn));
    STRCMP_CONTAINS("\"SID\":\"0x00000007\"", ws_transport_stub_last_sent(ap_conn));

    system_stub_advance_time(20);
    api_worker(api_sta);                         /* ответ клиенту станции */
    LONGS_EQUAL(2, iface_late_answered);
    LONGS_EQUAL(ap_before + 1, ws_transport_stub_send_attempts(ap_conn));
    LONGS_EQUAL(sta_before + 1, ws_transport_stub_send_attempts(sta_conn));
    STRCMP_CONTAINS("\"SID\":\"0x00000008\"", ws_transport_stub_last_sent(sta_conn));
}

TEST(WsServerTwoCores, OneHandlerCanServeBothCores) {
    /* Один и тот же обработчик регистрируется на обоих ядрах (так живёт UART:
     * порт один) — и каждый вызов обслуживается своим ядром, а ответ уходит в
     * соединение того ядра, откуда пришёл запрос. */
    api_register_handler(api_ap,  iface_ap_handler,    "iface.shared", &ctx_ap);
    api_register_handler(api_sta, iface_other_handler, "iface.shared", &ctx_sta);

    send_text(ap_conn,  "{\"FID\":\"iface.shared\",\"FLAGS\":0,\"SID\":1}");
    send_text(sta_conn, "{\"FID\":\"iface.shared\",\"FLAGS\":0,\"SID\":2}");
    api_worker(api_ap);
    api_worker(api_sta);

    LONGS_EQUAL(1, iface_ap_calls);
    LONGS_EQUAL(1, iface_other_calls);
    /* Каждый клиент получил подтверждение доставки от своего ядра. */
    LONGS_EQUAL(1, ws_transport_stub_send_attempts(ap_conn));
    LONGS_EQUAL(1, ws_transport_stub_send_attempts(sta_conn));
}

TEST(WsServerTwoCores, CoresCreateNoTasksOfTheirOwn) {
    /* Ядра не заводят ни потоков, ни очередей — даже когда их два. Такт им даёт
     * ОДНА задача приложения, и подъём ядра не должен добавить в систему ни
     * одного объекта. */
    LONGS_EQUAL(0, system_stub_task_created_count());
}

TEST(WsServerTwoCores, OneThreadDrivingBothCoresKeepsStateApart) {
    /* Приложение крутит один поток и зовёт api_worker по очереди для каждого
     * ядра (см. api_worker_task в main/app.c). Здесь проверяется, что такой
     * обход не путает состояния: кадр клиента точки доступа обрабатывается
     * AP-ядром, кадр станции — STA-ядром, и ответы уходят каждый в своё
     * соединение, хотя такт зовётся «через один». */
    api_register_handler(api_ap,  iface_ap_handler,    "iface.seq", &ctx_ap);
    api_register_handler(api_sta, iface_other_handler, "iface.seq", &ctx_sta);

    send_text(ap_conn,  "{\"FID\":\"iface.seq\",\"FLAGS\":0,\"SID\":1}");
    send_text(sta_conn, "{\"FID\":\"iface.seq\",\"FLAGS\":0,\"SID\":2}");

    /* Один проход «потока»: сначала AP, потом STA. */
    api_worker(api_ap);
    api_worker(api_sta);

    LONGS_EQUAL(1, iface_ap_calls);
    LONGS_EQUAL(1, iface_other_calls);
    CHECK(iface_ap_ctx_seen == &ctx_ap);
    CHECK(iface_other_ctx_seen == &ctx_sta);

    /* Второй проход ничего не добавляет: вызовы обеих сторон уже обслужены
     * своими ядрами и завершены обработчиками (они вернули FALSE). */
    api_worker(api_ap);
    api_worker(api_sta);
    LONGS_EQUAL(1, iface_ap_calls);
    LONGS_EQUAL(1, iface_other_calls);

    /* И рассылка каждого ядра по-прежнему не видна другому. */
    uint8_t buf[] = "{\"V\":1}";
    LONGS_EQUAL(0, api_broadcast(api_ap,  "iface.other.none", buf, sizeof(buf) - 1));
    LONGS_EQUAL(0, api_broadcast(api_sta, "iface.other.none", buf, sizeof(buf) - 1));
}

/* ── TEST_GROUP: сам стаб транспорта ──────────────────────────────────── */

TEST_GROUP(WsServerIfaceStub) {
    api_t *api;
    void setup() {
        TRANSPORT_SETUP();
    }
    void teardown() {
        TRANSPORT_TEARDOWN();
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

TEST(WsServerIfaceStub, UnservedInterfaceConnectionIsClosed) {
    /* Интерфейс, который не обслуживает ни один экземпляр транспорта (в
     * прошивке — ETH из сборки для QEMU): обслуживать соединение нечем, и оно
     * закрывается, а не висит без сервисов. */
    ws_transport_stub_set_iface(GW_IFACE_ETH);
    ws_transport_stub_conn_t *c = ws_transport_stub_connect(5);
    LONGS_EQUAL(1, ws_transport_stub_close_calls(c));
    LONGS_EQUAL(1, ws_transport_stub_closed(c));
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(c));
}

/* ── TEST_GROUP: WsServerMultiClient ─────────────────────────────────── */

TEST_GROUP(WsServerMultiClient) {
    api_t *api;
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

/* «Периферия», которая забирает вызов и НЕ отвечает из обработчика: ответит
 * тот, у кого есть только дескриптор вызова (так отвечает колбэк драйвера в
 * Modbus). */
static void *kept_call;
static uint8_t keep_call_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)ctx; (void)data; (void)len;
    if (!call) return 0;                 /* снятие: держать нечего */
    kept_call = call;
    return 1;                            /* ответ будет позже */
}

TEST_GROUP(WsServerApiCalls) {
    api_t *api;
    ws_transport_stub_conn_t *conn;
    void setup() {
        TRANSPORT_SETUP();
        conn = ws_transport_stub_connect(1);
        kept_call = NULL;
    }
    void teardown() {
        TRANSPORT_TEARDOWN();
    }
};

TEST(WsServerApiCalls, SendJsonNullCallReturnsFail) {
    /* Вызов не задан — отвечать нечему. Честный отказ, без падения: «подходящее»
     * ядро по имени не ищется, и нулевой дескриптор вызова — это отказ. */
    uint8_t buf[] = "{}";
    LONGS_EQUAL(0, api_reply(NULL, buf, 2));
}

TEST(WsServerApiCalls, ReplyThroughCallNeedsNoCore) {
    /* Ядро ответу не передаётся: его знает сам вызов (api_reply берёт его
     * оттуда). Тот, кто забрал вызов у обработчика — служба со своим потоком
     * или колбэк драйвера, — отвечает одним дескриптором вызова, и ответ уходит
     * в соединение того ядра, откуда пришёл запрос: FID и SID те же. */
    api_register_handler(api, keep_call_handler, "answer.nocore", NULL);
    send_text(conn, "{\"FID\":\"answer.nocore\",\"FLAGS\":0,\"SID\":4}");
    api_worker(api);                            /* обработчик забрал вызов */
    CHECK(kept_call != NULL);
    const int before = ws_transport_stub_send_attempts(conn);

    const uint8_t body[] = "\"NO_CORE\"";
    LONGS_EQUAL(1, api_reply(kept_call, body, sizeof(body) - 1));
    LONGS_EQUAL(before + 1, ws_transport_stub_send_attempts(conn));
    STRCMP_CONTAINS("\"FID\":\"answer.nocore\"", ws_transport_stub_last_sent(conn));
    STRCMP_CONTAINS("\"SID\":\"0x00000004\"", ws_transport_stub_last_sent(conn));
}

TEST(WsServerApiCalls, SendJsonFidGroupNoSubscribersReturnsFail) {
    uint8_t buf[] = "{}";
    LONGS_EQUAL(0, api_broadcast(api, "no.subscribers", buf, 2));
}

TEST(WsServerApiCalls, SendJsonFidGroupWithoutCoreReturnsFail) {
    /* Ядро у рассылки — обязательный аргумент: без него рассылать нечего и
     * некому — честный отказ, а не поиск «подходящего» ядра по имени. */
    uint8_t buf[] = "{}";
    LONGS_EQUAL(0, api_broadcast(NULL, "no.subscribers", buf, 2));
}

/* ── TEST_GROUP: такт и ответ клиенту ────────────────────────────────── */

/* Ядро не заводит ни потоков, ни очередей: такт ему даёт вызывающий
 * (api_worker), а ответ собирает и отправляет сам обработчик из своего вызова.
 * Здесь проверяется, что такт спокоен, когда делать нечего, и что поздний ответ
 * (отправленный следующим тактом) доходит до ждущего клиента — и не падает,
 * если клиент уже ушёл. */
TEST_GROUP(WsServerWorkerAndAnswer) {
    api_t *api;
    ws_transport_stub_conn_t *conn;
    void setup() {
        TRANSPORT_SETUP();
        conn = ws_transport_stub_connect(1);
        long_handler_pulses = 0;
        waiting_call  = NULL;
        waiting_calls = 0;
        late_body_sent = 0;
    }
    void teardown() {
        TRANSPORT_TEARDOWN();
    }
};

TEST(WsServerWorkerAndAnswer, WorkerWithoutDataDoesNothing) {
    /* Пустой такт: вызовов и регистраций нет. Ядро ничего не отправляет и
     * обработчиков не зовёт — проверяем и по транспорту, и по счётчику тактов. */
    for (int i = 0; i < 5; i++) {
        system_stub_advance_time(20);
        api_worker(api);
    }
    LONGS_EQUAL(0, ws_transport_stub_send_attempts(conn));
    LONGS_EQUAL(0, long_handler_pulses);

    /* NULL — не такт, а ничего: вызывающему не нужно помнить, поднялось ли
     * ядро, чтобы безопасно позвать такт. */
    api_worker(NULL);
}

TEST(WsServerWorkerAndAnswer, LateAnswerReachesWaitingClient) {
    /* Сервис, у которого свой поток или своя очередь (внутреннее дело сервиса),
     * забирает вызов обработчиком и отвечает на следующем такте — своим
     * дескриптором вызова. Клиент получает подтверждение доставки, а затем сам
     * ответ в том же конверте, что и запрос. */
    api_register_handler(api, late_answer_handler, "answer.late", NULL);
    send_text(conn, "{\"FID\":\"answer.late\",\"FLAGS\":0,\"SID\":7}");
    api_worker(api);
    CHECK(waiting_call != NULL);
    LONGS_EQUAL(0, late_body_sent);                 /* ответа ещё не было */
    const int before = ws_transport_stub_send_attempts(conn);

    system_stub_advance_time(20);
    api_worker(api);                                /* здесь обработчик отвечает */

    LONGS_EQUAL(1, late_body_sent);
    LONGS_EQUAL(before + 1, ws_transport_stub_send_attempts(conn));
    STRCMP_CONTAINS("\"FID\":\"answer.late\"", ws_transport_stub_last_sent(conn));
    STRCMP_CONTAINS("\"SID\":\"0x00000007\"", ws_transport_stub_last_sent(conn));

    /* Обработчик вернул FALSE — вызов закрыт, тактов больше нет. */
    system_stub_advance_time(20);
    api_worker(api);
    LONGS_EQUAL(1, late_body_sent);
    LONGS_EQUAL(before + 1, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerWorkerAndAnswer, LateAnswerToGoneClientDoesNotCrash) {
    /* Клиент ушёл, пока сервис думал. Ответ отправлять некуда, и ядро честно
     * отвечает отказом (0) вместо записи в освобождённую сессию; обработчик при
     * этом завершает вызов и освобождает свой контекст. */
    api_register_handler(api, late_answer_handler, "answer.gone", NULL);
    send_text(conn, "{\"FID\":\"answer.gone\",\"FLAGS\":0,\"SID\":7}");
    api_worker(api);
    CHECK(waiting_call != NULL);

    ws_transport_stub_disconnect(conn);
    const int send_before = ws_transport_stub_send_attempts(conn);

    system_stub_advance_time(20);
    api_worker(api);
    LONGS_EQUAL(1, late_body_sent);                 /* обработчик ответил... */
    LONGS_EQUAL(send_before, ws_transport_stub_send_attempts(conn));  /* ...и не отправил */

    /* Вызов снят: следующий такт обработчика уже не зовёт. */
    system_stub_advance_time(20);
    api_worker(api);
    LONGS_EQUAL(1, late_body_sent);
}

/* ── TEST_GROUP: WsServerRegression ───────────────────────────────────── */
/* One test per fixed bug. The failures here are memory errors, so they are
 * only reliable under ASan (enabled in the Makefile). */

#define REG_FID_HANDLER  "regx.handler"   /* обработчик, который отвечает сразу */
#define REG_FID_KEEP     "regx.keep"      /* обработчик, который держит вызов  */

/* Держит вызов живым (как сервис, ждущий периферию) и считает вызовы: по счёту
 * видно, что вызов жив, а после снятия — что его больше не зовут. */
static int reg_keep_calls;
static uint8_t reg_keep_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)ctx; (void)len;
    if (!call) return 0;
    if (data) reg_keep_calls++;
    return 1;
}

TEST_GROUP(WsServerRegression) {
    api_t *api;
    ws_transport_stub_conn_t *conn;
    void setup() {
        TRANSPORT_SETUP();
        conn = ws_transport_stub_connect(1);
        reg_keep_calls = 0;
        api_register_handler(api, dummy_handler, REG_FID_HANDLER, NULL);
        api_register_handler(api, reg_keep_handler, REG_FID_KEEP, NULL);
    }
    void teardown() {
        TRANSPORT_TEARDOWN();
    }
};

TEST(WsServerRegression, RefusedSendReleasesBufferWithoutKillingSession) {
    /* Об отказе сообщает транспорт (begin_send != TRANSPORT_OK): ядро
     * освобождает свой буфер ответа, но не трогает соединение — его судьбу
     * решают события TRANSFER_ERR и CLOSED. Иначе следующий такт мог бы
     * обратиться к уже освобождённой сессии (на устройстве это давало
     * StoreProhibited). */
    ws_transport_stub_set_send_result(conn, TRANSPORT_FAULT);
    send_text(conn, "{\"FID\":\"regx.handler\",\"FLAGS\":2,\"SID\":1}");
    api_worker(api);          /* DELIVERED не ушёл, вызов обработан */

    LONGS_EQUAL(1, ws_transport_stub_send_attempts(conn));

    /* Соединение живо: следующий запрос обслуживается как обычно */
    ws_transport_stub_set_send_result(conn, TRANSPORT_OK);
    send_text(conn, "{\"FID\":\"regx.unknown\",\"FLAGS\":0,\"SID\":2}");
    LONGS_EQUAL(2, ws_transport_stub_send_attempts(conn));
}

TEST(WsServerRegression, ToDeleteFreesSameCallQueuedRightAfterIt) {
    /* The worker cached `next` before handling TO_DELETE, then freed every
     * call with the same fid/SID — including `next` when the client sent
     * the target call right after the delete. `item = next` then walked
     * freed memory. */
    send_text(conn, "{\"FID\":\"regx.keep\",\"FLAGS\":8,\"SID\":7}");
    send_text(conn, "{\"FID\":\"regx.keep\",\"FLAGS\":4,\"SID\":7}");

    api_worker(api);

    /* Снятие — просьба, а не вызов: подтверждение уходит сразу, а подписка,
     * пришедшая следом, живёт и отдана обработчику. Снятие по совпадению
     * fid/SID не должно сносить соседний вызов. */
    LONGS_EQUAL(1, reg_keep_calls);                          /* подписка дошла */
    LONGS_EQUAL(2, ws_transport_stub_send_attempts(conn));   /* DELIVERED просьбе и подписке */

    system_stub_advance_time(20);
    api_worker(api);
    LONGS_EQUAL(2, ws_transport_stub_send_attempts(conn));   /* больше ничего не уходит */
}

TEST(WsServerRegression, ToDeleteRemovesCallWithThatSidAndKeepsNeighbour) {
    /* Один FID, две подписки одного клиента: SID 7 и SID 8. Просьба снять SID 7
     * обязана снять ИМЕННО его: соседний вызов того же FID остаётся в работе и
     * продолжает переспрашиваться. Иначе на устройстве был бы двойной приём
     * данных после смены канала в терминале. */
    removable_pulses = 0;
    LONGS_EQUAL(1, api_register_handler(api, removable_handler, "regx.pulse", NULL));

    send_text(conn, "{\"FID\":\"regx.pulse\",\"FLAGS\":4,\"SID\":7,\"ARG\":{\"RDL\":10}}");
    send_text(conn, "{\"FID\":\"regx.pulse\",\"FLAGS\":4,\"SID\":8,\"ARG\":{\"RDL\":10}}");
    api_worker(api);                        /* обе подписки отданы обработчику */
    system_stub_advance_time(50);
    api_worker(api);
    LONGS_EQUAL(2, removable_pulses);                    /* такт каждой из двух подписок */

    send_text(conn, "{\"FID\":\"regx.pulse\",\"FLAGS\":8,\"SID\":7}");
    api_worker(api);                        /* снятие доходит до конца */

    system_stub_advance_time(50);
    api_worker(api);
    const int one_alive = removable_pulses - 2;

    system_stub_advance_time(50);
    api_worker(api);
    const int still_one = removable_pulses - 2 - one_alive;

    /* Живая подписка ровно одна: каждый шаг даёт один такт, а не два. При старом
     * поведении снятие не работало, и тактов было бы по два на шаг. */
    LONGS_EQUAL(1, one_alive);
    LONGS_EQUAL(1, still_one);
}
TEST(WsServerRegression, BroadcastSendsOneSharedBufferToEverySubscriber) {
    /* Кадр периферии уходит подписчикам ОДНИМ буфером: на каждого клиента не
     * собирается своя копия той же портянки. Рассылка — api_broadcast
     * (единственный путь ответа). Проверяем по
     * адресу, который ядро отдало транспорту: у обоих подписчиков он один. */
    ws_transport_stub_set_deferred(conn, 1);
    ws_transport_stub_conn_t *conn2 = ws_transport_stub_connect(2);
    ws_transport_stub_set_deferred(conn2, 1);

    send_text(conn,  "{\"FID\":\"regx.keep\",\"FLAGS\":4,\"SID\":1,\"ARG\":{\"RDL\":1000}}");
    send_text(conn2, "{\"FID\":\"regx.keep\",\"FLAGS\":4,\"SID\":2,\"ARG\":{\"RDL\":1000}}");
    api_worker(api);

    uint8_t body[] = "\"AAAA\"";
    LONGS_EQUAL(1, api_broadcast(api, REG_FID_KEEP, body, sizeof(body) - 1));

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
    ws_transport_stub_complete_pending(conn, TRANSPORT_OK);
    ws_transport_stub_complete_pending(conn2, TRANSPORT_OK);
}
