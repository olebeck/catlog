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

#include "ringbuf.h"

#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>

#define SCE_KERNEL_ATTR_THREAD_FIFO (0x00000000U)
#define RINGBUF_EVF_NON_EMPTY 0x00000001

static int idx(RingBuf *rb, char *ptr)
{
  return (unsigned int)(ptr - rb->base_ptr) % rb->buf_len;
}

static void inc(RingBuf *rb, char **ptr)
{
  *ptr = rb->base_ptr + idx(rb, *ptr + 1);
}

static int empty(RingBuf *rb)
{
  return rb->get_ptr == rb->put_ptr;
}

static int full(RingBuf *rb)
{
  return idx(rb, rb->put_ptr + 1) == idx(rb, rb->get_ptr);
}

static int put(RingBuf *rb, char c)
{
  if (full(rb))
  {
    return -1;
  }
  *rb->put_ptr = c;
  inc(rb, &rb->put_ptr);
  return 0;
}

static int put_clobber(RingBuf *rb, char c)
{
  if (full(rb))
  {
    inc(rb, &rb->get_ptr);
  }
  *rb->put_ptr = c;
  inc(rb, &rb->put_ptr);
  return 0;
}

static int get(RingBuf *rb, char *c)
{
  if (empty(rb))
  {
    return -1;
  }
  *c = *rb->get_ptr;
  inc(rb, &rb->get_ptr);
  return 0;
}

int ringbuf_init(RingBuf *rb, int size)
{
  int ret = 0;

  rb->evf_uid      = -1;
  rb->mtx_uid      = -1;
  rb->memblock_uid = -1;
  rb->buf_len      = 0;
  rb->base_ptr     = NULL;
  rb->get_ptr      = NULL;
  rb->put_ptr      = NULL;

  rb->evf_uid = ksceKernelCreateEventFlag("RingBufferEventFlag", SCE_KERNEL_ATTR_THREAD_FIFO | SCE_EVENT_WAITMULTIPLE,
                                          0x00000000, NULL);
  if (rb->evf_uid < 0)
  {
    ret = rb->evf_uid;
    goto fail_evf;
  }

  rb->mtx_uid = ksceKernelCreateMutex("RingBufferMutex", SCE_KERNEL_ATTR_THREAD_FIFO, 0, NULL);
  if (rb->mtx_uid < 0)
  {
    ret = rb->mtx_uid;
    goto fail_mtx;
  }

  rb->memblock_uid = ksceKernelAllocMemBlock("RingBufferMemBlock", 0x6020D006, size, NULL);
  if (rb->memblock_uid < 0)
  {
    ret = rb->memblock_uid;
    goto fail_memblock;
  }
  ksceKernelGetMemBlockBase(rb->memblock_uid, (void **)&rb->base_ptr);

  rb->buf_len = size;
  rb->get_ptr = rb->put_ptr = rb->base_ptr;
  return 0;

fail_memblock:
  ksceKernelDeleteMutex(rb->mtx_uid);
fail_mtx:
  ksceKernelDeleteEventFlag(rb->evf_uid);
fail_evf:
  return ret;
}

int ringbuf_term(RingBuf *rb)
{
  ksceKernelDeleteEventFlag(rb->evf_uid);
  ksceKernelDeleteMutex(rb->mtx_uid);
  ksceKernelFreeMemBlock(rb->memblock_uid);
  rb->mtx_uid = rb->memblock_uid = -1;
  rb->buf_len = 0;
  rb->base_ptr = rb->get_ptr = rb->put_ptr = NULL;
  return 0;
}

int ringbuf_put(RingBuf *rb, char *c, int size)
{
  int n_put = 0;
  ksceKernelLockMutex(rb->mtx_uid, 1, NULL);

  while (size-- > 0)
  {
    if (put(rb, *c++) == 0)
    {
      n_put++;
    }
    else
    {
      break;
    }
  }

  if (n_put > 0)
  {
    ksceKernelSetEventFlag(rb->evf_uid, RINGBUF_EVF_NON_EMPTY);
  }

  ksceKernelUnlockMutex(rb->mtx_uid, 1);
  return n_put;
}

int ringbuf_put_clobber(RingBuf *rb, char *c, int size)
{
  int n_put = 0;
  ksceKernelLockMutex(rb->mtx_uid, 1, NULL);

  while (size-- > 0)
  {
    put_clobber(rb, *c++);
    n_put++;
  }

  if (n_put > 0)
  {
    ksceKernelSetEventFlag(rb->evf_uid, RINGBUF_EVF_NON_EMPTY);
  }

  ksceKernelUnlockMutex(rb->mtx_uid, 1);
  return n_put;
}

int ringbuf_get(RingBuf *rb, char *c, int size)
{
  int n_get = 0;
  ksceKernelLockMutex(rb->mtx_uid, 1, NULL);

  while (size-- > 0)
  {
    if (get(rb, c++) == 0)
    {
      n_get++;
    }
    else
    {
      break;
    }
  }

  if (empty(rb))
  {
    ksceKernelClearEventFlag(rb->evf_uid, ~RINGBUF_EVF_NON_EMPTY);
  }

  ksceKernelUnlockMutex(rb->mtx_uid, 1);
  return n_get;
}

int ringbuf_get_wait(RingBuf *rb, char *c, int size, SceUInt *timeout)
{
  int n_get = 0;
  int ret   = ksceKernelWaitEventFlag(rb->evf_uid, RINGBUF_EVF_NON_EMPTY, SCE_EVENT_WAITAND, NULL, timeout);
  if (ret < 0)
  {
    goto done;
  }
  ksceKernelLockMutex(rb->mtx_uid, 1, NULL);

  while (size-- > 0)
  {
    if (get(rb, c++) == 0)
    {
      n_get++;
    }
    else
    {
      break;
    }
  }

  if (empty(rb))
  {
    ksceKernelClearEventFlag(rb->evf_uid, ~RINGBUF_EVF_NON_EMPTY);
  }

  ksceKernelUnlockMutex(rb->mtx_uid, 1);
done:
  return n_get;
}
