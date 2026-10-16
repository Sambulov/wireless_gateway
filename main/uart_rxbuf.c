#include <string.h>

#include "uart_rxbuf.h"

/* ── Состояние одного кольца ─────────────────────────────────────────────────
 *
 *   head      — куда писать следующий байт данных;
 *   open_mark — где лежит метка ОТКРЫТОГО куска (head == open_mark, пока кусок
 *               пуст: метка заводится вместе с первым байтом данных);
 *   read      — где метка самого старого незабранного куска;
 *   lost      — приём терялся (места не хватило), читателю это сообщается.
 *
 * Занятое место — [read, head): закрытые куски и, если открытый кусок уже
 * накопил данные, его метка с данными. Меток со значением 0 в занятом месте не
 * бывает, кроме метки текущего открытого куска: пустой кусок метки не заводит,
 * а при изломе кольца пустой кусок просто переносится в начало.
 */
typedef struct {
    uint8_t *storage;
    uint16_t size;
    uint16_t head;
    uint16_t open_mark;
    uint16_t read;
    uint8_t  lost;
    /* Счётчики байт данных: сколько принято всего и сколько уже отдано. По ним
     * видно, ГДЕ именно теряли: потеря случается у головы записи, то есть ПОСЛЕ
     * всего уже записанного, и признак должен достаться куску, который начинается
     * ровно в точке потери, а не первому попавшемуся. */
    uint32_t seq_written;
    uint32_t seq_read;
    uint32_t lost_at_seq;
} gw_uart_rxbuf_private_t;

_Static_assert(sizeof(gw_uart_rxbuf_private_t) <= GW_UART_RXBUF_PRIVATE_SIZE,
               "GW_UART_RXBUF_PRIVATE_SIZE мал для состояния кольца");

static uint16_t rxbuf_used(const gw_uart_rxbuf_private_t *p)
{
    return (uint16_t)((uint16_t)(p->head - p->read + p->size) % p->size);
}

static uint16_t rxbuf_mark_get(const gw_uart_rxbuf_private_t *p, uint16_t at)
{
    return (uint16_t)(p->storage[at] | ((uint16_t)p->storage[(uint16_t)((at + 1) % p->size)] << 8));
}

static void rxbuf_mark_put(gw_uart_rxbuf_private_t *p, uint16_t at, uint16_t mark)
{
    p->storage[at] = (uint8_t)(mark & 0xFF);
    p->storage[(uint16_t)((at + 1) % p->size)] = (uint8_t)(mark >> 8);
}

/* Закрыть открытый кусок: в его метку кладём длину и признак «кадр продолжается».
 * Пустой кусок закрывать нечем и незачем. */
static void rxbuf_seal(gw_uart_rxbuf_private_t *p, uint8_t more)
{
    uint16_t len = (uint16_t)(p->head - p->open_mark - GW_UART_RXBUF_MARK_SIZE);

    if (len == 0)
        return;
    if (len > GW_UART_RXBUF_MAX_PIECE)
        len = GW_UART_RXBUF_MAX_PIECE;
    rxbuf_mark_put(p, p->open_mark, (uint16_t)(len | (more ? GW_UART_RXBUF_FLAG_MORE : 0)));
}

uint8_t gw_uart_rxbuf_init(void *desc, uint8_t *storage, uint16_t size)
{
    gw_uart_rxbuf_private_t *p = (gw_uart_rxbuf_private_t *)desc;

    if (p == NULL || storage == NULL || size < GW_UART_RXBUF_MARK_SIZE * 4)
        return 0;

    memset(p, 0, sizeof(*p));
    p->storage   = storage;
    p->size      = size;
    p->head      = 0;
    p->open_mark = 0;
    p->read      = 0;
    return 1;
}

uint16_t gw_uart_rxbuf_push(void *desc, const uint8_t *data, uint16_t len,
                            uint8_t frame_end)
{
    gw_uart_rxbuf_private_t *p = (gw_uart_rxbuf_private_t *)desc;
    uint16_t done = 0;

    if (p == NULL || p->storage == NULL || data == NULL || len == 0)
        return 0;

    while (done < len) {
        uint8_t  empty = (p->head == p->open_mark) ? 1 : 0;
        /* Метка этого куска (если он ещё пуст) и метка следующего куска. */
        uint16_t reserve = (uint16_t)(GW_UART_RXBUF_MARK_SIZE
                                      + (empty ? GW_UART_RXBUF_MARK_SIZE : 0));
        uint16_t free_total = (uint16_t)(p->size - rxbuf_used(p));

        if (free_total <= reserve) {
            /* Места нет. НЕ перепрыгиваем читателя: то, что он ещё не забрал,
             * остаётся целым, а новые данные теряются с явным признаком. */
            p->lost = 1;
            p->lost_at_seq = p->seq_written;
            break;
        }

        /* Сколько байт можно записать физически подряд от головы, не задев
         * незабранное (кусок через излом кольца не переносим — он закрывается
         * признаком «продолжение»). */
        uint16_t phys = (p->read > p->head)
                      ? (uint16_t)(p->read - p->head)
                      : (uint16_t)(p->size - p->head);

        if (phys <= (empty ? GW_UART_RXBUF_MARK_SIZE : 0)) {
            /* Дальше писать нельзя. Перенос в начало кольца разрешён только в двух
             * случаях, иначе читатель останется перед дырой: он идёт по меткам
             * подряд и перепрыгнуть пустое место не может.
             *
             *   1) незабранного нет (читатель догнал голову) — переносим и его;
             *   2) голова стоит ровно на физическом конце кольца — тогда читатель
             *      дойдёт до нуля сам, по своему счёту по модулю.
             */
            if (p->read == p->head) {
                p->read      = 0;
                p->open_mark = 0;
                p->head      = 0;
                continue;
            }
            if (p->head != p->size) {
                /* Середина кольца: писать некуда, а обойти нельзя — теряем новые
                 * данные, незабранное читателю остаётся целым. */
                p->lost = 1;
                p->lost_at_seq = p->seq_written;
                break;
            }
            if (!empty)
                rxbuf_seal(p, 1);
            p->open_mark = 0;
            p->head      = 0;
            continue;
        }

        uint16_t can = (uint16_t)(free_total - reserve);                 /* по месту */
        uint16_t phys_can = (uint16_t)(phys - (empty ? GW_UART_RXBUF_MARK_SIZE : 0));
        if (can > phys_can)
            can = phys_can;
        if (can > (uint16_t)(len - done))
            can = (uint16_t)(len - done);
        /* Куску не даём перерасти порог: тогда частичная выгрузка предсказуема —
         * ровно по порогу, а не «сколько влезло за раз». */
        uint16_t piece_data = empty ? 0
                             : (uint16_t)(p->head - p->open_mark - GW_UART_RXBUF_MARK_SIZE);
        if (piece_data < GW_UART_RXBUF_PART_THRESHOLD
            && can > (uint16_t)(GW_UART_RXBUF_PART_THRESHOLD - piece_data))
            can = (uint16_t)(GW_UART_RXBUF_PART_THRESHOLD - piece_data);
        if (can == 0) {
            p->lost = 1;
            p->lost_at_seq = p->seq_written;
            break;
        }

        if (empty) {
            /* Метка заводится вместе с первым байтом куска — нулём: читатель по
             * ней видит, что кусок ещё открыт, и останавливается на нём. */
            rxbuf_mark_put(p, p->open_mark, 0);
            p->head = (uint16_t)(p->open_mark + GW_UART_RXBUF_MARK_SIZE);
        }

        memcpy(&p->storage[p->head], &data[done], can);
        p->head = (uint16_t)(p->head + can);
        done    = (uint16_t)(done + can);
        p->seq_written += can;

        /* Порог частичной выгрузки: длинный кусок закрываем, не дожидаясь конца
         * передачи на линии, — но с признаком «продолжение». */
        if ((uint16_t)(p->head - p->open_mark - GW_UART_RXBUF_MARK_SIZE)
            >= GW_UART_RXBUF_PART_THRESHOLD) {
            rxbuf_seal(p, 1);
            p->open_mark = p->head;
        }

        if (done == len && frame_end) {
            /* Линия замолчала — передача закончилась: метка без признака. */
            rxbuf_seal(p, 0);
            p->open_mark = p->head;
        }
    }

    if (done < len) {
        /* Места не хватило: принятое закрываем с признаком «продолжение», а не
         * держим открытым куском до лучших времён — иначе читатель не увидит его
         * вовсе, а потеря останется незамеченной. */
        rxbuf_seal(p, 1);
        p->open_mark = p->head;
    }

    return done;
}

/* Разбор метки очередного куска: 0 — готовых нет (метка открытого куска либо
 * читатель догнал голову). */
/* Потеря случилась в этой точке потока (или раньше) — значит она стоит перед
 * этим куском. Сравнение по разности: счётчики 32-битные и переполняются. */
static uint8_t rxbuf_lost_before(const gw_uart_rxbuf_private_t *p)
{
    return (p->lost && (int32_t)(p->seq_read - p->lost_at_seq) >= 0) ? 1 : 0;
}

static uint16_t rxbuf_next_mark(const gw_uart_rxbuf_private_t *p)
{
    uint16_t mark;

    if (p == NULL || p->storage == NULL || p->read == p->head)
        return 0;
    mark = rxbuf_mark_get(p, p->read);
    if ((uint16_t)(mark & ~GW_UART_RXBUF_FLAG_MORE) == 0)
        return 0;
    return mark;
}

uint8_t gw_uart_rxbuf_peek(const void *desc, uint16_t *len, uint8_t *frame_end,
                           uint8_t *lost_before)
{
    const gw_uart_rxbuf_private_t *p = (const gw_uart_rxbuf_private_t *)desc;
    uint16_t mark = rxbuf_next_mark(p);

    if (mark == 0)
        return 0;
    if (len)          *len         = (uint16_t)(mark & ~GW_UART_RXBUF_FLAG_MORE);
    if (frame_end)    *frame_end   = (mark & GW_UART_RXBUF_FLAG_MORE) ? 0 : 1;
    if (lost_before)  *lost_before = rxbuf_lost_before(p);
    return 1;
}

uint8_t gw_uart_rxbuf_take(void *desc, uint8_t *dst, uint16_t max,
                           gw_uart_rxbuf_piece_t *piece)
{
    gw_uart_rxbuf_private_t *p = (gw_uart_rxbuf_private_t *)desc;
    uint16_t mark, len;

    if (p == NULL || p->storage == NULL || piece == NULL)
        return 0;

    piece->len         = 0;
    piece->frame_end   = 0;
    piece->lost_before = 0;

    if (dst == NULL)
        return 0;

    mark = rxbuf_next_mark(p);
    if (mark == 0)
        return 0;                                   /* готовых кусков нет */

    len = (uint16_t)(mark & ~GW_UART_RXBUF_FLAG_MORE);

    if (len > max) {
        piece->len = len;                           /* скажем, сколько нужно места */
        return 0;
    }

    memcpy(dst, &p->storage[(uint16_t)((p->read + GW_UART_RXBUF_MARK_SIZE) % p->size)], len);
    piece->len         = len;
    piece->frame_end   = (mark & GW_UART_RXBUF_FLAG_MORE) ? 0 : 1;
    piece->lost_before = rxbuf_lost_before(p);
    if (piece->lost_before)
        p->lost = 0;                                /* о потере сообщаем один раз */

    p->read = (uint16_t)((p->read + GW_UART_RXBUF_MARK_SIZE + len) % p->size);
    p->seq_read += len;
    return 1;
}

uint8_t gw_uart_rxbuf_lost(const void *desc)
{
    const gw_uart_rxbuf_private_t *p = (const gw_uart_rxbuf_private_t *)desc;
    return (p != NULL) ? p->lost : 0;
}

void gw_uart_rxbuf_lost_clear(void *desc)
{
    gw_uart_rxbuf_private_t *p = (gw_uart_rxbuf_private_t *)desc;
    if (p != NULL)
        p->lost = 0;
}

uint16_t gw_uart_rxbuf_used(const void *desc)
{
    const gw_uart_rxbuf_private_t *p = (const gw_uart_rxbuf_private_t *)desc;
    return (p != NULL && p->storage != NULL) ? rxbuf_used(p) : 0;
}

uint16_t gw_uart_rxbuf_free(const void *desc)
{
    const gw_uart_rxbuf_private_t *p = (const gw_uart_rxbuf_private_t *)desc;
    return (p != NULL && p->storage != NULL)
         ? (uint16_t)(p->size - rxbuf_used(p)) : 0;
}
