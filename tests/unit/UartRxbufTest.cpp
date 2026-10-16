/* Тесты кольцевого буфера приёма порта (uart_rxbuf).
 *
 * Библиотека без платформенных зависимостей: проверяется всё, что раньше было
 * не проверить — границы передач, излом кольца, порог частичной выгрузки,
 * потеря данных без перепрыгивания читателя. */
extern "C" {
#include "uart_rxbuf.h"
}

#include "CppUTest/TestHarness.h"

#include <string.h>

TEST_GROUP(UartRxbuf) {
    /* Кольцо небольшое: излом и переполнение на нём достаются быстро. */
    static const uint16_t RING = 32;
    uint8_t storage[32];
    gw_uart_rxbuf_t buf;

    void setup() {
        memset(storage, 0xA5, sizeof(storage));
        CHECK(gw_uart_rxbuf_init(&buf, storage, RING) == 1);
    }

    /* Собирает всё готовое в одну строку, попутно запоминая, где были концы
     * кадров (по возрастанию смещений в строке). */
    uint16_t drain(char *out, uint16_t *ends, uint8_t *lost, uint16_t max_ends) {
        uint8_t piece_buf[64];
        gw_uart_rxbuf_piece_t piece;
        uint16_t total = 0;
        uint8_t  n_ends = 0;
        if (lost) *lost = 0;
        while (gw_uart_rxbuf_take(&buf, piece_buf, sizeof(piece_buf), &piece)) {
            memcpy(&out[total], piece_buf, piece.len);
            total = (uint16_t)(total + piece.len);
            if (piece.lost_before && lost) *lost = 1;
            if (piece.frame_end && ends && n_ends < max_ends)
                ends[n_ends++] = total;
        }
        out[total] = '\0';
        return total;
    }
};

/* Один кадр: данные и конец передачи. Метка пишется при закрытии куска, поэтому
 * до этого момента читателю не отдаётся ничего. */
TEST(UartRxbuf, SingleFrameGoesOutAsOnePiece) {
    char out[64];
    uint16_t ends[8];
    uint8_t lost = 0;

    LONGS_EQUAL(0, drain(out, ends, &lost, 8));          /* пусто */
    LONGS_EQUAL(4, gw_uart_rxbuf_push(&buf, (const uint8_t *)"AAAA", 4, 1));
    LONGS_EQUAL(4, drain(out, ends, &lost, 8));
    STRCMP_EQUAL("AAAA", out);
    CHECK(ends[0] == 4);                                  /* конец кадра отмечен */
    LONGS_EQUAL(0, lost);
}

TEST(UartRxbuf, UnfinishedFrameIsNotGivenAway) {
    char out[64];
    uint16_t ends[8];
    uint8_t lost = 0;

    LONGS_EQUAL(3, gw_uart_rxbuf_push(&buf, (const uint8_t *)"abc", 3, 0));
    LONGS_EQUAL(0, drain(out, ends, &lost, 8));          /* передача не кончилась */
    LONGS_EQUAL(2, gw_uart_rxbuf_push(&buf, (const uint8_t *)"de", 2, 1));
    LONGS_EQUAL(5, drain(out, ends, &lost, 8));
    STRCMP_EQUAL("abcde", out);
    CHECK(ends[0] == 5);
}

/* Несколько передач в одном кольце: каждая со своей границей, порядок сохранён. */
TEST(UartRxbuf, SeveralFramesKeepTheirBoundaries) {
    char out[64];
    uint16_t ends[8];
    uint8_t lost = 0;

    gw_uart_rxbuf_push(&buf, (const uint8_t *)"one", 3, 1);
    gw_uart_rxbuf_push(&buf, (const uint8_t *)"two", 3, 1);
    gw_uart_rxbuf_push(&buf, (const uint8_t *)"3", 1, 1);

    LONGS_EQUAL(7, drain(out, ends, &lost, 8));
    STRCMP_EQUAL("onetwo3", out);
    CHECK(ends[0] == 3);
    CHECK(ends[1] == 6);
    CHECK(ends[2] == 7);
}

/* Длинный незаконченный кадр выгружается частями, и каждая часть, кроме
 * последней, помечена «продолжение» — клиент склеивает их в один кадр. */
TEST(UartRxbuf, LongFrameIsFlushedWithContinuationFlag) {
    /* Кольцо как в прошивке: порог частичной выгрузки должен в него влезать. */
    static uint8_t big_storage[4096];
    gw_uart_rxbuf_t big;
    CHECK(gw_uart_rxbuf_init(&big, big_storage, sizeof(big_storage)) == 1);

    static uint8_t chunk[1200];
    memset(chunk, 'x', sizeof(chunk));
    LONGS_EQUAL(1200, gw_uart_rxbuf_push(&big, chunk, sizeof(chunk), 0));

    static uint8_t piece_buf[2048];
    gw_uart_rxbuf_piece_t piece;

    /* Кусок дорос до порога и закрыт признаком «продолжение». */
    CHECK(gw_uart_rxbuf_take(&big, piece_buf, sizeof(piece_buf), &piece) == 1);
    LONGS_EQUAL(GW_UART_RXBUF_PART_THRESHOLD, piece.len);
    LONGS_EQUAL(0, piece.frame_end);
    for (uint16_t i = 0; i < piece.len; i++) CHECK(piece_buf[i] == 'x');

    /* Остаток передачи: закрывается уже как конец кадра. */
    LONGS_EQUAL(20, gw_uart_rxbuf_push(&big, (const uint8_t *)"01234567890123456789", 20, 1));
    CHECK(gw_uart_rxbuf_take(&big, piece_buf, sizeof(piece_buf), &piece) == 1);
    LONGS_EQUAL(1200 + 20 - GW_UART_RXBUF_PART_THRESHOLD, piece.len);
    LONGS_EQUAL(1, piece.frame_end);              /* конец кадра — здесь */
}

/* Излом кольца: кусок не переносится через границу массива, а закрывается
 * признаком «продолжение» — данные при этом не теряются. */
TEST(UartRxbuf, PieceDoesNotStraddleTheWrap) {
    char out[64];
    uint16_t ends[8];
    uint8_t lost = 0;

    /* Набиваем кольцо и забираем — голова уходит к концу массива. */
    for (int i = 0; i < 6; i++)
        gw_uart_rxbuf_push(&buf, (const uint8_t *)"zz", 2, 1);
    drain(out, ends, &lost, 8);

    /* Теперь пишем кадр, который физически не влезает до конца: он должен
     * разложиться на два куска. */
    uint8_t data[12];
    memset(data, 'w', sizeof(data));
    LONGS_EQUAL(12, gw_uart_rxbuf_push(&buf, data, sizeof(data), 1));
    uint16_t got = drain(out, ends, &lost, 8);
    LONGS_EQUAL(12, got);
    STRCMP_EQUAL("wwwwwwwwwwww", out);
    CHECK(ends[0] == 12);                 /* конец кадра — один, на конце */
    LONGS_EQUAL(0, lost);
}

/* Перенос в начало кольца «на середине» запрещён: читатель идёт по меткам подряд
 * и дыру не перепрыгнет. В такой ситуации новые данные теряются с признаком, а
 * незабранное остаётся целым и читается до конца. */
TEST(UartRxbuf, NoWrapInTheMiddleOfTheRing) {
    uint8_t piece_buf[64];
    gw_uart_rxbuf_piece_t piece;
    uint8_t ten[10], sixteen[16], four[4];
    memset(ten, 'a', sizeof(ten));
    memset(sixteen, 'b', sizeof(sixteen));
    memset(four, 'c', sizeof(four));

    /* Кольцо 32: 10 + 2 и 16 + 2 байт укладываются, голове остаётся 2 байта —
     * ровно под метку, но не под метку с данными. */
    LONGS_EQUAL(10, gw_uart_rxbuf_push(&buf, ten, sizeof(ten), 1));
    LONGS_EQUAL(16, gw_uart_rxbuf_push(&buf, sixteen, sizeof(sixteen), 1));
    LONGS_EQUAL(0, gw_uart_rxbuf_push(&buf, four, sizeof(four), 1));
    CHECK(gw_uart_rxbuf_lost(&buf) == 1);

    /* Оба куска читаются целиком, порядок и содержимое не пострадали. */
    CHECK(gw_uart_rxbuf_take(&buf, piece_buf, sizeof(piece_buf), &piece) == 1);
    LONGS_EQUAL(10, piece.len);
    MEMCMP_EQUAL("aaaaaaaaaa", piece_buf, 10);
    CHECK(gw_uart_rxbuf_take(&buf, piece_buf, sizeof(piece_buf), &piece) == 1);
    LONGS_EQUAL(16, piece.len);
    MEMCMP_EQUAL("bbbbbbbbbbbbbbbb", piece_buf, 16);
    LONGS_EQUAL(0, gw_uart_rxbuf_take(&buf, piece_buf, sizeof(piece_buf), &piece));
}

/* Переполнение: незабранное читателю остаётся целым, новые данные теряются, и об
 * этом сообщается признаком — молчаливой склейки двух передач не бывает. */
TEST(UartRxbuf, OverflowKeepsUnreadDataAndReportsLoss) {
    uint8_t data[16];
    memset(data, 'q', sizeof(data));

    /* Пишем кадры, пока места хватит; считаем принятое. */
    uint16_t accepted = 0;
    for (int i = 0; i < 6; i++)
        accepted = (uint16_t)(accepted + gw_uart_rxbuf_push(&buf, data, sizeof(data) - 2, 1));
    CHECK(accepted < 6 * 14);             /* часть не влезла */

    char out[128];
    uint16_t ends[16];
    uint8_t lost = 0;
    uint16_t got = drain(out, ends, &lost, 16);
    LONGS_EQUAL(accepted, got);           /* всё принятое читатель получил целиком */
    LONGS_EQUAL(14, ends[0]);             /* и границы кадров на месте */
}

/* О потерянном сообщается признаком на том куске, который идёт следом за потерей;
 * сама потеря видна сразу. */
TEST(UartRxbuf, LossIsVisibleBeforeItIsReported) {
    uint8_t data[16];
    memset(data, 'q', sizeof(data));
    while (gw_uart_rxbuf_push(&buf, data, 14, 1) == 14)
        ;
    LONGS_EQUAL(1, gw_uart_rxbuf_lost(&buf));      /* потеря есть */
}

/* Признак потери стоит ТАМ, ГДЕ потеряли: он достаётся куску, который
 * начинается ровно в точке потери, а данные, записанные раньше, идут без него. */
TEST(UartRxbuf, LostMarkSitsAtTheLossPoint) {
    uint8_t piece_buf[64];
    gw_uart_rxbuf_piece_t piece;
    uint8_t twenty[20];
    memset(twenty, 'a', sizeof(twenty));

    /* Кольцо 32: 20 байт проходят целиком, из следующих 20 влезает только часть —
     * здесь и теряем. */
    LONGS_EQUAL(20, gw_uart_rxbuf_push(&buf, twenty, sizeof(twenty), 1));
    CHECK(gw_uart_rxbuf_push(&buf, twenty, sizeof(twenty), 1) < 20);
    CHECK(gw_uart_rxbuf_lost(&buf) == 1);

    /* Забранное до потери — без признака. */
    CHECK(gw_uart_rxbuf_take(&buf, piece_buf, sizeof(piece_buf), &piece) == 1);
    LONGS_EQUAL(20, piece.len);
    LONGS_EQUAL(0, piece.lost_before);
    CHECK(gw_uart_rxbuf_take(&buf, piece_buf, sizeof(piece_buf), &piece) == 1);
    LONGS_EQUAL(0, piece.lost_before);

    /* Место освободилось: новые данные начинаются ровно в точке потери — вот у
     * этого куска признак и стоит. */
    LONGS_EQUAL(2, gw_uart_rxbuf_push(&buf, (const uint8_t *)"EF", 2, 1));
    CHECK(gw_uart_rxbuf_take(&buf, piece_buf, sizeof(piece_buf), &piece) == 1);
    LONGS_EQUAL(2, piece.len);
    LONGS_EQUAL(1, piece.lost_before);
    LONGS_EQUAL(1, piece.frame_end);
}

/* Кусок не влезает в приёмник читателя — говорим, сколько нужно, и ничего не
 * теряем: кусок остаётся в кольце. */
TEST(UartRxbuf, TooSmallSinkKeepsPiece) {
    uint8_t small[8];
    uint8_t big[16];
    gw_uart_rxbuf_piece_t piece;

    LONGS_EQUAL(12, gw_uart_rxbuf_push(&buf, (const uint8_t *)"0123456789AB", 12, 1));
    /* Приёмник мал — кусок остаётся в кольце, наружу уходит только его длина. */
    LONGS_EQUAL(0, gw_uart_rxbuf_take(&buf, small, sizeof(small), &piece));
    LONGS_EQUAL(12, piece.len);
    CHECK(gw_uart_rxbuf_take(&buf, big, sizeof(big), &piece) == 1);
    LONGS_EQUAL(12, piece.len);
    MEMCMP_EQUAL("0123456789AB", big, 12);
    LONGS_EQUAL(1, piece.frame_end);
}

/* Занятое место считается вместе с метками: две метки на кадр — это норма. */
TEST(UartRxbuf, UsedCountsMarks) {
    LONGS_EQUAL(0, gw_uart_rxbuf_used(&buf));
    gw_uart_rxbuf_push(&buf, (const uint8_t *)"abcd", 4, 1);
    LONGS_EQUAL(2 + 4, gw_uart_rxbuf_used(&buf));
    LONGS_EQUAL(32 - 6, gw_uart_rxbuf_free(&buf));
}
