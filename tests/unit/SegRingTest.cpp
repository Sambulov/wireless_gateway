/* Тесты сегментированного кольцевого буфера (seg_ring).
 *
 * Библиотека без платформенных зависимостей, поэтому проверяется сам контракт:
 * читатель получает не больше, чем просит, никогда не переступает границу
 * сегмента, признаки конца и потери приходят ПОСЛЕ своих байт, а без приёмника
 * буфер только рассказывает о текущем сегменте и ничего не снимает. */
extern "C" {
#include "seg_ring.h"
}

#include "CppUTest/TestHarness.h"

#include <string.h>

TEST_GROUP(SegRing) {
    /* Кольцо небольшое: излом и переполнение на нём достаются быстро. */
    static const uint16_t RING = 32;
    uint8_t storage[32];
    gw_seg_ring_t ring;

    void setup() {
        memset(storage, 0xA5, sizeof(storage));
        CHECK(gw_seg_ring_init(&ring, storage, RING) == 1);
    }

    /* Читает всё, что есть, кусками не больше max; отмечает смещения в потоке,
     * на которых пришли конец сегмента и признак потери. */
    uint16_t drain(char *out, uint16_t *ends, uint8_t *n_ends,
                   uint16_t *losts, uint8_t *n_losts, uint16_t max = 96) {
        uint8_t buf[96];
        gw_seg_ring_chunk_t chunk;
        uint16_t total = 0;

        *n_ends = 0;
        *n_losts = 0;
        while (gw_seg_ring_read(&ring, buf, max, &chunk)) {
            if (chunk.len) {
                memcpy(&out[total], buf, chunk.len);
                total = (uint16_t)(total + chunk.len);
            }
            if (chunk.end && *n_ends < 8)
                ends[(*n_ends)++] = total;
            if (chunk.lost && *n_losts < 8)
                losts[(*n_losts)++] = total;
        }
        out[total] = '\0';
        return total;
    }
};

/* ── базовое чтение ──────────────────────────────────────────────────────── */

TEST(SegRing, EmptyRingGivesNothing) {
    char out[96];
    uint16_t ends[8], losts[8];
    uint8_t n_ends, n_losts;
    gw_seg_ring_chunk_t chunk;

    LONGS_EQUAL(0, drain(out, ends, &n_ends, losts, &n_losts));
    CHECK(gw_seg_ring_read(&ring, NULL, 0, &chunk) == 0);
    LONGS_EQUAL(0, gw_seg_ring_used(&ring));
    LONGS_EQUAL(32, gw_seg_ring_free(&ring));
}

TEST(SegRing, ReadsOnlyWhatWasAsked) {
    char out[96];
    uint16_t ends[8], losts[8];
    uint8_t n_ends, n_losts;
    gw_seg_ring_chunk_t chunk;
    uint8_t buf[8];

    LONGS_EQUAL(10, gw_seg_ring_write(&ring, (const uint8_t *)"0123456789", 10, 1));

    /* Просят меньше сегмента — получают ровно столько и без признаков. */
    CHECK(gw_seg_ring_read(&ring, buf, 4, &chunk) == 1);
    LONGS_EQUAL(4, chunk.len);
    LONGS_EQUAL(0, chunk.end);
    LONGS_EQUAL(0, chunk.lost);
    MEMCMP_EQUAL("0123", buf, 4);

    CHECK(gw_seg_ring_read(&ring, buf, 4, &chunk) == 1);
    MEMCMP_EQUAL("4567", buf, 4);
    LONGS_EQUAL(0, chunk.end);

    /* Остаток сегмента — с признаком конца, даже если места было больше. */
    CHECK(gw_seg_ring_read(&ring, buf, 8, &chunk) == 1);
    LONGS_EQUAL(2, chunk.len);
    LONGS_EQUAL(1, chunk.end);
    MEMCMP_EQUAL("89", buf, 2);

    LONGS_EQUAL(0, drain(out, ends, &n_ends, losts, &n_losts));
}

TEST(SegRing, SegmentBoundaryStopsTheRead) {
    char out[96];
    uint16_t ends[8], losts[8];
    uint8_t n_ends, n_losts;

    /* Два сегмента подряд: одним запросом читатель получает только первый. */
    gw_seg_ring_write(&ring, (const uint8_t *)"abc", 3, 1);
    gw_seg_ring_write(&ring, (const uint8_t *)"de", 2, 1);

    gw_seg_ring_chunk_t chunk;
    uint8_t buf[96];
    CHECK(gw_seg_ring_read(&ring, buf, 96, &chunk) == 1);
    LONGS_EQUAL(3, chunk.len);
    LONGS_EQUAL(1, chunk.end);
    MEMCMP_EQUAL("abc", buf, 3);

    LONGS_EQUAL(2, drain(out, ends, &n_ends, losts, &n_losts));
    STRCMP_EQUAL("de", out);
    LONGS_EQUAL(1, n_ends);
    CHECK(ends[0] == 2);            /* конец второго сегмента */
    LONGS_EQUAL(0, n_losts);
}

/* ── открытый сегмент и разметка отдельным куском ────────────────────────── */

TEST(SegRing, OpenSegmentGivesDataWithoutEnd) {
    char out[96];
    uint16_t ends[8], losts[8];
    uint8_t n_ends, n_losts;
    gw_seg_ring_chunk_t chunk;
    uint8_t buf[8];

    /* Сегмент ещё не закончен — данные уже можно забирать, признака конца нет. */
    LONGS_EQUAL(4, gw_seg_ring_write(&ring, (const uint8_t *)"abcd", 4, 0));
    CHECK(gw_seg_ring_read(&ring, buf, 8, &chunk) == 1);
    LONGS_EQUAL(4, chunk.len);
    LONGS_EQUAL(0, chunk.end);

    /* Дописали и закрыли — данные идут дальше, конец приходит с ними. */
    LONGS_EQUAL(2, gw_seg_ring_write(&ring, (const uint8_t *)"ef", 2, 1));
    LONGS_EQUAL(2, drain(out, ends, &n_ends, losts, &n_losts));
    STRCMP_EQUAL("ef", out);
    LONGS_EQUAL(1, n_ends);
    CHECK(ends[0] == 2);
}

TEST(SegRing, EndMarkerArrivesAlone) {
    char out[96];
    uint16_t ends[8], losts[8];
    uint8_t n_ends, n_losts;
    gw_seg_ring_chunk_t chunk;
    uint8_t buf[8];

    /* Данные забрали до того, как линия замолчала: конец приходит отдельным
     * куском без данных — и ровно один раз. */
    gw_seg_ring_write(&ring, (const uint8_t *)"abcd", 4, 0);
    CHECK(gw_seg_ring_read(&ring, buf, 8, &chunk) == 1);
    LONGS_EQUAL(4, chunk.len);
    LONGS_EQUAL(0, chunk.end);

    gw_seg_ring_write(&ring, NULL, 0, 1);          /* пустая запись закрывает сегмент */

    CHECK(gw_seg_ring_read(&ring, buf, 8, &chunk) == 1);
    LONGS_EQUAL(0, chunk.len);
    LONGS_EQUAL(1, chunk.end);
    CHECK(gw_seg_ring_read(&ring, buf, 8, &chunk) == 0);

    LONGS_EQUAL(0, drain(out, ends, &n_ends, losts, &n_losts));
}

/* ── справка без приёмника ──────────────────────────────────────────────── */

TEST(SegRing, InfoModeDoesNotConsume) {
    gw_seg_ring_chunk_t chunk;
    uint8_t buf[8];
    uint16_t before;

    gw_seg_ring_write(&ring, (const uint8_t *)"abcd", 4, 1);
    before = gw_seg_ring_used(&ring);

    /* Дважды подряд — одно и то же: справка ничего не снимает. */
    CHECK(gw_seg_ring_read(&ring, NULL, 0, &chunk) == 1);
    LONGS_EQUAL(4, chunk.len);
    LONGS_EQUAL(1, chunk.end);
    LONGS_EQUAL(before, gw_seg_ring_used(&ring));

    CHECK(gw_seg_ring_read(&ring, NULL, 0, &chunk) == 1);
    LONGS_EQUAL(4, chunk.len);

    /* И только настоящее чтение снимает данные. */
    CHECK(gw_seg_ring_read(&ring, buf, 8, &chunk) == 1);
    LONGS_EQUAL(4, chunk.len);
    LONGS_EQUAL(0, gw_seg_ring_used(&ring));
}

TEST(SegRing, InfoModeOfOpenSegmentHasNoEnd) {
    gw_seg_ring_chunk_t chunk;

    gw_seg_ring_write(&ring, (const uint8_t *)"abc", 3, 0);
    CHECK(gw_seg_ring_read(&ring, NULL, 0, &chunk) == 1);
    LONGS_EQUAL(3, chunk.len);
    LONGS_EQUAL(0, chunk.end);
}

/* ── потеря данных ───────────────────────────────────────────────────────── */

TEST(SegRing, LossIsReportedAfterTheBytes) {
    char out[96];
    uint16_t ends[8], losts[8];
    uint8_t n_ends, n_losts;

    /* Первый сегмент влезает, второй — уже нет: принятое остаётся читателю
     * целым, а признак потери встаёт ПОСЛЕ него. */
    uint16_t first  = gw_seg_ring_write(&ring, (const uint8_t *)"01234567890123456789", 20, 1);
    uint16_t second = gw_seg_ring_write(&ring, (const uint8_t *)"abcdefghijklmnopqrst", 20, 1);

    LONGS_EQUAL(20, first);
    CHECK(second < 20);               /* остаток не влез — это и есть потеря */

    char expected[64];
    memcpy(expected, "01234567890123456789", 20);
    memcpy(&expected[20], "abcdefghijklmnopqrst", second);

    LONGS_EQUAL(20 + second, drain(out, ends, &n_ends, losts, &n_losts));
    MEMCMP_EQUAL(expected, out, 20 + second);
    /* Два конца кадра: первый сегмент закрылся сам, второй — по признаку
     * потери (метка пишется, значит и кадр на этом кончился). */
    LONGS_EQUAL(2, n_ends);
    CHECK(ends[0] == 20);
    CHECK(ends[1] == 20 + second);
    LONGS_EQUAL(1, n_losts);
    /* Потеря — после этих байт, а не перед ними. */
    CHECK(losts[0] == 20 + second);
}

TEST(SegRing, LossEndsTheUnfinishedSegment) {
    char out[96];
    uint16_t ends[8], losts[8];
    uint8_t n_ends, n_losts;
    gw_seg_ring_chunk_t chunk;
    uint8_t buf[96];

    /* Один длинный незаконченный сегмент заполняет кольцо: часть байт влезает,
     * остальные уже нет — сегмент закрывается с признаком потери. */
    uint16_t written = gw_seg_ring_write(&ring,
                                         (const uint8_t *)"0123456789abcdefghijklmnopqr", 28, 0);
    CHECK(written > 0);
    CHECK(written < 28);               /* остаток не влез — это и есть потеря */
    while (gw_seg_ring_write(&ring, (const uint8_t *)"XYZ", 3, 1) > 0)
        ;                              /* добиваем кольцо до отказа */

    CHECK(gw_seg_ring_read(&ring, buf, 96, &chunk) == 1);
    LONGS_EQUAL(written, chunk.len);
    /* Метка записана — значит и кадр на этом кончился; вместе с ней приходит и
     * признак потери: данные после этих байт не приняты. */
    LONGS_EQUAL(1, chunk.end);
    LONGS_EQUAL(1, chunk.lost);

    LONGS_EQUAL(0, drain(out, ends, &n_ends, losts, &n_losts));
}

TEST(SegRing, LossKeepsUnreadDataIntact) {
    char out[96];
    uint16_t ends[8], losts[8];
    uint8_t n_ends, n_losts;
    gw_seg_ring_chunk_t chunk;
    uint8_t buf[96];

    /* Читатель отстал: кольцо забито, новые байты уже не влезают. То, что
     * успело записаться, обязано дойти до читателя целиком и без склейки с
     * продолжением, а признак потери — стоять на границе последнего сегмента. */
    gw_seg_ring_write(&ring, (const uint8_t *)"AAAA", 4, 1);
    gw_seg_ring_write(&ring, (const uint8_t *)"BBBB", 4, 1);
    CHECK(gw_seg_ring_write(&ring, (const uint8_t *)"CCCCCCCCCCCCCCCCCCCCCCCCCCCCCC", 30, 1) < 30);
    while (gw_seg_ring_write(&ring, (const uint8_t *)"D", 1, 1) > 0)
        ;                              /* добиваем до отказа */

    CHECK(gw_seg_ring_read(&ring, buf, 96, &chunk) == 1);
    LONGS_EQUAL(4, chunk.len);
    LONGS_EQUAL(1, chunk.end);
    MEMCMP_EQUAL("AAAA", buf, 4);

    /* Дальше — всё принятое до потери, и признак потери в самом конце. */
    uint16_t rest = drain(out, ends, &n_ends, losts, &n_losts);
    CHECK(rest > 4);
    MEMCMP_EQUAL("BBBB", out, 4);
    CHECK(n_ends >= 1);
    CHECK(ends[0] == 4);              /* конец второго сегмента на месте */
    CHECK(n_losts >= 1);
    /* Потери отмечены там, где они случились: последняя — на конце потока,
     * а не в начале. */
    CHECK(losts[n_losts - 1] == rest);
}

/* ── излом кольца ────────────────────────────────────────────────────────── */

TEST(SegRing, WrapKeepsSegmentsApart) {
    char out[96];
    uint16_t ends[8], losts[8];
    uint8_t n_ends, n_losts;

    /* Первый сегмент сдвигает голову почти к концу кольца, второй пишется
     * через излом: читатель получает два сегмента, а не один склеенный. */
    gw_seg_ring_write(&ring, (const uint8_t *)"0123456789", 10, 1);
    LONGS_EQUAL(10, drain(out, ends, &n_ends, losts, &n_losts));
    STRCMP_EQUAL("0123456789", out);

    gw_seg_ring_write(&ring, (const uint8_t *)"abcdefghij", 10, 1);
    LONGS_EQUAL(10, drain(out, ends, &n_ends, losts, &n_losts));
    STRCMP_EQUAL("abcdefghij", out);
    LONGS_EQUAL(1, n_ends);
    CHECK(ends[0] == 10);
}

TEST(SegRing, LongSegmentAcrossWrapKeepsOneFrame) {
    char out[96];
    uint16_t ends[8], losts[8];
    uint8_t n_ends, n_losts;

    /* Сегмент, которому не хватило места до конца кольца, закрывается с
     * признаком «продолжение»: конец передачи придёт только на последней части. */
    gw_seg_ring_write(&ring, (const uint8_t *)"0123456789", 10, 1);   /* сдвиг головы */
    LONGS_EQUAL(10, drain(out, ends, &n_ends, losts, &n_losts));

    gw_seg_ring_write(&ring, (const uint8_t *)"abcdefghijklmnopqrst", 20, 1);
    LONGS_EQUAL(20, drain(out, ends, &n_ends, losts, &n_losts));
    STRCMP_EQUAL("abcdefghijklmnopqrst", out);
    LONGS_EQUAL(1, n_ends);          /* один кадр, а не два */
    CHECK(ends[0] == 20);
}

/* Непрерывный поток: линия не замолкает, сегмент так и не закрывается, а
 * читатель исправно забирает. Кольцо обязано возвращать себе уже отданное —
 * иначе место кончается там, где на самом деле пусто, и писатель начинает
 * отказывать на ровном месте (именно так терялись данные на приборе: чтение
 * успевало, а занятость не убывала). */
TEST(SegRing, ContinuousStreamReturnsDeliveredSpace) {
    uint8_t chunk[120];
    uint8_t out[512];
    uint16_t written = 0;
    uint16_t received = 0;
    uint16_t lost_marks = 0;
    uint16_t refuses = 0;

    memset(chunk, 'x', sizeof(chunk));

    /* Кольцо 32 Б не подходит: нужен размер побольше, чтобы поток шёл долго. */
    static uint8_t big_storage[512];
    gw_seg_ring_t big;
    CHECK(gw_seg_ring_init(&big, big_storage, sizeof(big_storage)) == 1);

    for (int i = 0; i < 200; i++) {
        uint16_t w = gw_seg_ring_write(&big, chunk, 100, 0);
        written = (uint16_t)(written + w);
        if (w < 100)
            refuses++;

        gw_seg_ring_chunk_t c;
        while (gw_seg_ring_read(&big, out, sizeof(out), &c)) {
            received = (uint16_t)(received + c.len);
            lost_marks = (uint16_t)(lost_marks + (c.lost ? 1 : 0));
        }
    }

    LONGS_EQUAL(0, refuses);          /* место возвращается, отказов нет */
    LONGS_EQUAL(0, lost_marks);       /* и терять нечего */
    LONGS_EQUAL(written, received);   /* всё записанное дошло */
}

/* ── занято/свободно ─────────────────────────────────────────────────────── */

TEST(SegRing, UsedCountsMarksAndData) {
    LONGS_EQUAL(0, gw_seg_ring_used(&ring));

    gw_seg_ring_write(&ring, (const uint8_t *)"abcd", 4, 1);
    LONGS_EQUAL(2 + 4, gw_seg_ring_used(&ring));      /* метка + данные */
    LONGS_EQUAL(32 - 6, gw_seg_ring_free(&ring));

    gw_seg_ring_chunk_t chunk;
    uint8_t buf[8];
    gw_seg_ring_read(&ring, buf, 8, &chunk);
    LONGS_EQUAL(0, gw_seg_ring_used(&ring));
    LONGS_EQUAL(32, gw_seg_ring_free(&ring));
}

/* ── поток целиком ───────────────────────────────────────────────────────── */

TEST(SegRing, LongStreamThroughSmallRingStaysIntact) {
    char out[64];
    char got[400];
    char expected[400];
    uint16_t ends[8], losts[8];
    uint8_t  n_ends, n_losts;
    uint16_t produced = 0;
    uint16_t received = 0;
    uint16_t pos = 0;
    uint8_t  byte = 0;

    /* Пишем сегментами по 7 байт, читаем по 5: суммарно втрое больше кольца. */
    while (produced < 300) {
        uint8_t chunk[7];

        for (uint8_t i = 0; i < 7; i++)
            chunk[i] = (uint8_t)('a' + (byte++ % 26));
        LONGS_EQUAL(7, gw_seg_ring_write(&ring, chunk, 7, 1));
        memcpy(&expected[pos], chunk, 7);
        pos      = (uint16_t)(pos + 7);
        produced = (uint16_t)(produced + 7);

        uint16_t piece = drain(out, ends, &n_ends, losts, &n_losts, 5);
        memcpy(&got[received], out, piece);
        received = (uint16_t)(received + piece);
        LONGS_EQUAL(0, n_losts);        /* читатель не отставал — терять нечего */
    }

    LONGS_EQUAL(0, gw_seg_ring_used(&ring));
    LONGS_EQUAL(pos, received);
    MEMCMP_EQUAL(expected, got, pos);   /* и порядок байт не поехал на изломах */
}
