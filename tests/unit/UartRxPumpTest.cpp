/* Выборка принятого UART (main/uart_rx_pump.c).
 *
 * Главное — кросс-ток: задачи приёма портов идут параллельно на двух ядрах.
 * Буфер куска был один на все порты (static в uart_event_task), и, пока порт A
 * держал свой кусок между чтением и раздачей, порт B читал в тот же буфер —
 * подписчик A получал байты B («шум на втором порту при передаче на первом»).
 * Тест воспроизводит это чередование детерминированно: чтение порта A
 * запускает полный цикл порта B, как это сделало бы второе ядро. */
extern "C" {
#include "uart_rx_pump.h"
}

#include "CppUTest/TestHarness.h"

#include <string.h>
#include <string>
#include <vector>

namespace {

struct FakePort {
    std::string line;             /* что ещё лежит в FIFO драйвера */
    std::string got;              /* что дошло до подписчика */
    std::string echoed;           /* что вернули в линию эхом */
    std::vector<uint32_t> chunks; /* размеры доставленных кусков */
    std::vector<uint8_t> ends;
    uint32_t echo_pending = 0;    /* сколько байт своего эха отбросить */
    uint8_t echo_on = 0;
    int reads = 0;
    /* Куда и сколько просили прочитать — чтение не должно выходить за буфер. */
    std::vector<const uint8_t *> read_bufs;
    std::vector<uint32_t> read_sizes;
    /* Чередование «второе ядро»: после чтения этого порта успевает отработать
     * другой порт целиком. */
    uart_rx_pump_t *other = nullptr;
    size_t other_size = 0;
};

int32_t fake_read(void *ctx, uint8_t *buf, uint32_t size) {
    FakePort *p = (FakePort *)ctx;
    p->reads++;
    p->read_bufs.push_back(buf);
    p->read_sizes.push_back(size);
    uint32_t n = (uint32_t)std::min<size_t>(size, p->line.size());
    memcpy(buf, p->line.data(), n);
    p->line.erase(0, n);
    if (p->other) {
        uart_rx_pump_t *o = p->other;
        p->other = nullptr;          /* один раз */
        uart_rx_pump_run(o, p->other_size, 1);
    }
    return (int32_t)n;
}

uint32_t fake_gap(void *) { return 0; }

uint32_t fake_echo_skip(void *ctx, uint32_t size) {
    FakePort *p = (FakePort *)ctx;
    uint32_t drop = std::min(size, p->echo_pending);
    p->echo_pending -= drop;
    return drop;
}

void fake_echo_write(void *ctx, const uint8_t *buf, uint32_t size) {
    FakePort *p = (FakePort *)ctx;
    if (p->echo_on) p->echoed.append((const char *)buf, size);
}

void fake_deliver(void *ctx, const uint8_t *buf, uint32_t size, uint8_t b_end, uint32_t) {
    FakePort *p = (FakePort *)ctx;
    p->got.append((const char *)buf, size);
    p->chunks.push_back(size);
    p->ends.push_back(b_end);
}

const uart_rx_pump_ops_t ops = {
    fake_read, fake_gap, fake_echo_skip, fake_echo_write, fake_deliver,
};

} // namespace

TEST_GROUP(UartRxPump) {
    FakePort a, b;
    uart_rx_pump_t pa, pb;

    void setup() {
        uart_rx_pump_init(&pa, &ops, &a);
        uart_rx_pump_init(&pb, &ops, &b);
    }
};

/* Регрессия кросс-тока: порт B читает и раздаёт, пока кусок порта A ещё не
 * отдан. Каждый подписчик получает только свои байты. */
TEST(UartRxPump, ParallelPortsDoNotMixData) {
    a.line = "AAAAAAAAAAAAAAAA";
    b.line = "bbbbbbbbbbbbbbbb";
    a.other = &pb;
    a.other_size = b.line.size();

    uart_rx_pump_run(&pa, a.line.size(), 1);

    STRCMP_EQUAL("AAAAAAAAAAAAAAAA", a.got.c_str());
    STRCMP_EQUAL("bbbbbbbbbbbbbbbb", b.got.c_str());
}

/* Объявленный событием объём забирается кусками не больше буфера. */
TEST(UartRxPump, SplitsIntoChunks) {
    a.line = std::string(300, 'x');
    uart_rx_pump_run(&pa, 300, 1);
    LONGS_EQUAL(3, a.chunks.size());
    LONGS_EQUAL(UART_RX_PUMP_CHUNK, a.chunks[0]);
    LONGS_EQUAL(UART_RX_PUMP_CHUNK, a.chunks[1]);
    LONGS_EQUAL(300 - 2 * UART_RX_PUMP_CHUNK, a.chunks[2]);
    LONGS_EQUAL(300, a.got.size());
}

/* FIFO пуст, хотя событие обещало данные: выходим, а не крутимся вечно. */
TEST(UartRxPump, EmptyReadStops) {
    a.line = "abc";
    uart_rx_pump_run(&pa, 100, 0);
    STRCMP_EQUAL("abc", a.got.c_str());
    LONGS_EQUAL(2, a.reads);              /* 3 байта, затем 0 — и выход */
}

/* Признак конца передачи доходит до подписчика. */
TEST(UartRxPump, PassesEndFlag) {
    a.line = "q";
    uart_rx_pump_run(&pa, 1, 1);
    LONGS_EQUAL(1, a.ends.size());
    LONGS_EQUAL(1, a.ends[0]);
}

/* Однопроводная линия: начало — своё эхо, оно не доставляется и не
 * возвращается в линию; кусок целиком из эха не доставляется вовсе. */
TEST(UartRxPump, DropsOwnEcho) {
    a.echo_on = 1;
    a.echo_pending = 3;
    a.line = "ECHreply";
    uart_rx_pump_run(&pa, a.line.size(), 1);
    STRCMP_EQUAL("reply", a.got.c_str());
    STRCMP_EQUAL("reply", a.echoed.c_str());

    a.got.clear();
    a.chunks.clear();
    a.echo_pending = 2;
    a.line = "EC";
    uart_rx_pump_run(&pa, 2, 1);
    LONGS_EQUAL(0, a.chunks.size());
}

/* Регрессия старого цикла: после частичного отброса эха указатель куска не
 * сбрасывался, и следующее чтение на 128 байт шло в buf+skip — за конец буфера.
 * Каждое чтение обязано начинаться с начала буфера объекта и в нём умещаться. */
TEST(UartRxPump, PartialEchoSkipDoesNotShiftNextRead) {
    a.echo_pending = 4;
    a.line = "ECHO" + std::string(296, 'r');
    uart_rx_pump_run(&pa, 300, 1);

    STRCMP_EQUAL(std::string(296, 'r').c_str(), a.got.c_str());
    LONGS_EQUAL(3, a.read_bufs.size());
    for (size_t i = 0; i < a.read_bufs.size(); i++) {
        POINTERS_EQUAL(pa.buf, a.read_bufs[i]);
        CHECK(a.read_sizes[i] <= UART_RX_PUMP_CHUNK);
    }
}
