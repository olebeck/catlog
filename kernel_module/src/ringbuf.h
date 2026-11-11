/*
CatLog
Copyright (C) 2024 Cat
Copyright (C) 2020 Princess of Sleeping
Copyright (C) 2020 Asakura Reiko

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, version 3 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#ifndef RINGBUF_H
#define RINGBUF_H

#include <psp2kern/types.h>

typedef struct RingBuf {
  SceUID evf_uid;
  SceUID mtx_uid;
  SceUID memblock_uid;

  int buf_len;
  char *base_ptr;
  char *get_ptr;
  char *put_ptr;
} RingBuf;

int ringbuf_init(RingBuf *rb, int size);
int ringbuf_term(RingBuf *rb);

int ringbuf_put(RingBuf *rb, char *c, int size);
int ringbuf_put_clobber(RingBuf *rb, char *c, int size);
int ringbuf_get(RingBuf *rb, char *c, int size);
int ringbuf_get_wait(RingBuf *rb, char *c, int size, SceUInt *timeout);

#endif
