#include "varint.h"
#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

// static int msb = (int)(~0ULL << 25);

typedef uint8_t varintBuf[16];

static size_t varintEncode(int value, uint8_t *vbuf) {
  unsigned pos = sizeof(varintBuf) - 1;
  vbuf[pos] = value & 127;
  while (value >>= 7) {
    vbuf[--pos] = 128 | (--value & 127);
  }
  return pos;
}

#define VARINT_BUF(buf, pos) ((buf) + pos)
#define VARINT_LEN(pos) (16 - (pos))

size_t WriteVarintRaw(int value, char *buf) {
  varintBuf varint;
  size_t pos = varintEncode(value, varint);
  memcpy(buf, VARINT_BUF(varint, pos), VARINT_LEN(pos));
  return VARINT_LEN(pos);
}

size_t WriteVarintBuffer(int value, Buffer *buf) {
  varintBuf varint;
  size_t pos = varintEncode(value, varint);
  size_t n = VARINT_LEN(pos);
  Buffer_Reserve(buf, n);
  memcpy(buf->data + buf->offset, VARINT_BUF(varint, pos), n);
  buf->offset += n;
  return n;
}

int WriteVarint(int value, BufferWriter *w) {
  // printf("writing %d bytes\n", 16 - pos);
  varintBuf varint;
  size_t pos = varintEncode(value, varint);
  size_t nw = VARINT_LEN(pos);

  if (Buffer_Reserve(w->buf, nw)) {
    w->pos = w->buf->data + w->buf->offset;
  }

  memcpy(w->pos, VARINT_BUF(varint, pos), nw);

  w->buf->offset += nw;
  w->pos += nw;

  return nw;
}

size_t varintSize(int value) {
  assert(value > 0);
  size_t outputSize = 0;
  do {
    ++outputSize;
  } while (value >>= 7);
  return outputSize;
}

void VVW_Free(VarintVectorWriter *w) {
  Buffer_Free(&w->buf);
  free(w);
}

VarintVectorWriter *NewVarintVectorWriter(size_t cap) {
  VarintVectorWriter *w = malloc(sizeof(VarintVectorWriter));
  VVW_Init(w, cap);
  return w;
}

void VVW_Init(VarintVectorWriter *w, size_t cap) {
  w->lastValue = 0;
  w->nmemb = 0;
  Buffer_Init(&w->buf, cap);
}

/**
Write an integer to the vector.
@param w a vector writer
@param i the integer we want to write
@retur 0 if we're out of capacity, the varint's actual size otherwise
*/
size_t VVW_Write(VarintVectorWriter *w, int i) {
  Buffer_Reserve(&w->buf, 16);
  size_t n = WriteVarintBuffer(i - w->lastValue, &w->buf);
  if (n != 0) {
    w->nmemb += 1;
    w->lastValue = i;
  }
  return n;
}

// Truncate the vector
size_t VVW_Truncate(VarintVectorWriter *w) {
  return Buffer_Truncate(&w->buf, 0);
}
