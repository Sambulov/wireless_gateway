#include <string.h>

#include "seg_ring.h"

#include "CodeLib.h"

typedef struct {
    uint8_t *storage;
    uint16_t size;
    uint16_t length    : 15;
    uint16_t head_open : 1;
    uint16_t tail;
    uint16_t open_len  : 15;
    uint16_t open_lost : 1;
} gw_seg_ring_private_t;

_Static_assert(sizeof(gw_seg_ring_private_t) <= GW_SEG_RING_PRIVATE_SIZE,
               "GW_SEG_RING_PRIVATE_SIZE мал для состояния кольца");

/* Метка сегмента: 2 байта. */
#define SEG_RING_MARK_SIZE        2

/* Предел длины одного сегмента — он же предел размера кольца. */
#define SEG_LEN_MAX   0x7fffu

#define SEG_BREAK   0x8000u

static uint16_t seg_free(const gw_seg_ring_private_t *p) {
  return (p->size - p->length);
}

static uint16_t seg_index_move(gw_seg_ring_private_t *p, uint16_t index, int16_t offset) {
  int32_t ind = index + offset;
  while(ind < 0)
    ind += p->size;
  while(ind >= p->size)
    ind -= p->size;
  return (uint16_t)ind;
}

static void seg_mark_put(gw_seg_ring_private_t *p, uint16_t at, int16_t mark) {
  p->storage[at++] = (uint16_t)mark;
  if(at >= p->size) 
    at = 0;
  p->storage[at] = ((uint16_t)mark) >> 8;
}

static int16_t seg_mark_get(gw_seg_ring_private_t *p, uint16_t at) {
  uint16_t mark = p->storage[at++];
  if(at >= p->size) 
    at = 0;
  mark |= ((uint16_t)p->storage[at]) << 8;
  return (int16_t)mark;
}

static uint8_t *seg_get_linear_buf(gw_seg_ring_private_t *p, uint16_t *size) {
  if(!p->head_open || (p->length >= p->size))
    return NULL;
  uint16_t start_at = seg_index_move(p, p->tail, p->length);
  uint16_t sz = p->size - start_at;
  if(p->tail > start_at)
    sz = p->tail - start_at;
  *size = sz;
  return &p->storage[start_at];
}

static void gw_seg_open(gw_seg_ring_private_t *p) {
  if(seg_free(p) <= SEG_RING_MARK_SIZE)
    return;
  uint16_t head = seg_index_move(p, p->tail, p->length);
  seg_mark_put(p, head, 0);
  p->length += SEG_RING_MARK_SIZE;
  p->head_open = 1;
}

static void gw_seg_close(gw_seg_ring_private_t *p) {
  if(!p->head_open)
    return;
  int16_t mark = (int16_t)(p->open_len | (p->open_lost? SEG_BREAK: 0));
  uint16_t at = seg_index_move(p, p->tail, p->length - (p->open_len + SEG_RING_MARK_SIZE));
  seg_mark_put(p, at, mark);
  p->open_len = 0;
  p->open_lost = 0;
  p->head_open = 0;
}

uint8_t gw_seg_ring_init(void *desc, uint8_t *storage, uint16_t size) {
  gw_seg_ring_private_t *p = (gw_seg_ring_private_t *)desc;
  if ((p == NULL) || (storage == NULL) || (size < SEG_RING_MARK_SIZE))
    return CL_FALSE;
  if(size > SEG_LEN_MAX)
    size = SEG_LEN_MAX;
  mem_set(p, 0, sizeof(gw_seg_ring_private_t));
  p->storage = storage;
  p->size    = size;
  return CL_TRUE;
}

uint16_t gw_seg_ring_write(void *desc, const uint8_t *data, uint16_t len, uint8_t segment_end) {
  gw_seg_ring_private_t *p = (gw_seg_ring_private_t *)desc;
  if (p == NULL || p->storage == NULL || (data == NULL && len > 0))
      return 0;
  if(!p->head_open)
    gw_seg_open(p);
  if(!p->head_open)
    return 0;
  uint16_t done = 0;
  while (!p->open_lost && (len > done)) {
    uint16_t bl_len = 0;
    uint8_t *blk = seg_get_linear_buf(p, &bl_len);
    if(!blk)
      break;
    uint16_t wr = CL_MIN(bl_len, len - done);
    mem_cpy(blk, &data[done], wr);
    p->length += wr;
    done += wr;
  }
  p->open_len += done;
  p->open_lost = p->open_lost || (len > done);
  if (segment_end)
    gw_seg_close(p);
  return done;
}

uint8_t gw_seg_ring_read(void *desc, uint8_t *dst, uint16_t max, gw_seg_ring_chunk_t *chunk) {
  gw_seg_ring_private_t *p = (gw_seg_ring_private_t *)desc;
  if (p == NULL || p->storage == NULL)
    return 0;
  int16_t mark = 0;
  uint16_t done = 0;
  uint8_t is_open = 1;
  uint16_t available = 0;
  if(p->length) {
    mark = (int16_t)seg_mark_get(p, p->tail);
    is_open = (p->length == (p->open_len + SEG_RING_MARK_SIZE)) && p->head_open;
    if(is_open)
      mark = p->open_len;
    available = (((uint16_t)mark) & ~SEG_BREAK);
    uint16_t read_at = seg_index_move(p, p->tail, SEG_RING_MARK_SIZE);
    uint16_t amount = 0;
    do {
      amount = p->size - read_at;
      amount = CL_MIN(amount, available);
      if(max)
        amount = CL_MIN(amount, max);
      if(dst && amount)
        mem_cpy(&dst[done], &p->storage[read_at], amount);
      done += amount;
      available -= amount;
      read_at = seg_index_move(p, read_at, amount);
    } while (amount && (done < max));
  }

  if(chunk) {
    chunk->len = done;
    chunk->end = !is_open && (!available);
    chunk->lost = chunk->end && ((uint16_t)mark & SEG_BREAK);
  }

  if(max) {
    if(!is_open && !available)
      done += SEG_RING_MARK_SIZE;

    p->tail = seg_index_move(p, p->tail, done);
    p->length -= done;

    if(is_open) {
      p->open_len = available;
      available = 0;
    }

    if(is_open || available)
      seg_mark_put(p, p->tail, available | ((uint16_t)mark & SEG_BREAK));
  }
  return 1;
}

#ifdef SEG_RING_DEBUG
#include <stdio.h>
void gw_seg_ring_debug(const void *desc, const char *tag)
{
    const gw_seg_ring_private_t *p = (const gw_seg_ring_private_t *)desc;

    printf("  [%s] read=%u head=%u open_len=%d used=%u\n", tag, p->read, p->head,
           p->open_len, seg_used(p));
}
#endif

uint16_t gw_seg_ring_free(const void *desc) {
  const gw_seg_ring_private_t *p = (const gw_seg_ring_private_t *)desc;
  if((p == NULL) || (p->storage == NULL)) 
    return 0;
  return seg_free(p);
}
