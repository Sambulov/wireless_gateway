/* Сборка входящего сообщения WebSocket из фрагментов (main/ws_msg.c).
 *
 * Главное в контракте — предел ВСЕГО сообщения: каждый кадр по отдельности
 * может быть в пределе, а их сумма — нет. Раньше проверялась только длина
 * кадра, и поток продолжений по 16 КБ растил буфер до исчерпания кучи. */
extern "C" {
#include "ws_msg.h"
}
#include "CppUTest/TestHarness.h"

#include <string.h>

static const uint32_t MAX = 64;

TEST_GROUP(WsMsg) {
    ws_msg_t *msg;
    void setup()    { msg = NULL; }
    void teardown() { ws_msg_free(&msg); }

    /* Кадр в сообщение — как это делает транспорт: место, чтение, учёт. */
    bool append(const char *s, uint32_t max = MAX) {
        uint32_t n = (uint32_t)strlen(s);
        uint8_t *dst = ws_msg_reserve(&msg, n, max);
        if (dst == NULL) return false;
        memcpy(dst, s, n);
        ws_msg_commit(msg, n);
        return true;
    }
};

TEST(WsMsg, FragmentsAreJoinedInOrder) {
    CHECK(append("{\"FID\":"));
    CHECK(append("\"x\"}"));
    LONGS_EQUAL(11, ws_msg_len(msg));
    MEMCMP_EQUAL("{\"FID\":\"x\"}", ws_msg_data(msg), 11);
}

TEST(WsMsg, MessageExactlyAtTheLimitIsAccepted) {
    char half[33];
    memset(half, 'a', 32); half[32] = '\0';
    CHECK(append(half));
    CHECK(append(half));
    LONGS_EQUAL(MAX, ws_msg_len(msg));
}

/* Регрессия: каждый кадр меньше предела, сумма — больше. */
TEST(WsMsg, FragmentsOverTheLimitInTotalAreRefused) {
    char part[41];
    memset(part, 'a', 40); part[40] = '\0';
    CHECK(append(part));
    CHECK_FALSE(append(part));                 /* 80 > 64 */
    /* Отказ не портит накопленное: транспорт сам решает, что с ним делать. */
    LONGS_EQUAL(40, ws_msg_len(msg));
}

TEST(WsMsg, SingleFrameOverTheLimitIsRefused) {
    POINTERS_EQUAL(NULL, ws_msg_reserve(&msg, MAX + 1, MAX));
    POINTERS_EQUAL(NULL, msg);                 /* ничего не заведено */
}

/* Длина кадра приходит из сети: сумма не должна переполнить uint32 и
 * «пролезть» под предел. */
TEST(WsMsg, HugeFrameDoesNotWrapAroundTheLimit) {
    CHECK(append("abcd"));
    POINTERS_EQUAL(NULL, ws_msg_reserve(&msg, 0xFFFFFFFFu, MAX));
    POINTERS_EQUAL(NULL, ws_msg_reserve(&msg, 0xFFFFFFFDu, 0xFFFFFFFFu));
    LONGS_EQUAL(4, ws_msg_len(msg));
}

/* Пустой первый кадр (FIN=0) — тоже начало сообщения: продолжение к нему
 * обязано найти, к чему пристыковаться. */
TEST(WsMsg, EmptyFirstFrameStartsTheMessage) {
    CHECK(ws_msg_reserve(&msg, 0, MAX) != NULL);
    CHECK(msg != NULL);
    LONGS_EQUAL(0, ws_msg_len(msg));
    CHECK(append("abc"));
    LONGS_EQUAL(3, ws_msg_len(msg));
}

TEST(WsMsg, CommitNeverCountsMoreThanReserved) {
    CHECK(ws_msg_reserve(&msg, 4, MAX) != NULL);
    ws_msg_commit(msg, 100);
    LONGS_EQUAL(4, ws_msg_len(msg));
}

TEST(WsMsg, FreeForgetsTheMessage) {
    CHECK(append("abc"));
    ws_msg_free(&msg);
    POINTERS_EQUAL(NULL, msg);
    LONGS_EQUAL(0, ws_msg_len(msg));
    POINTERS_EQUAL(NULL, ws_msg_data(msg));
}
