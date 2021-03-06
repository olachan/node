/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "uv-common.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct poll_ctx {
  uv_fs_poll_t* parent_handle; /* NULL if parent has been stopped or closed */
  int busy_polling;
  unsigned int interval;
  uint64_t start_time;
  uv_loop_t* loop;
  uv_fs_poll_cb poll_cb;
  uv_timer_t timer_handle;
  uv_fs_t fs_req; /* TODO(bnoordhuis) mark fs_req internal */
  uv_statbuf_t statbuf;
  char path[1]; /* variable length */
};

static int statbuf_eq(const uv_statbuf_t* a, const uv_statbuf_t* b);
static void poll_cb(uv_fs_t* req);
static void timer_cb(uv_timer_t* timer, int status);
static void timer_close_cb(uv_handle_t* handle);

static uv_statbuf_t zero_statbuf;


int uv_fs_poll_init(uv_loop_t* loop, uv_fs_poll_t* handle) {
  uv__handle_init(loop, (uv_handle_t*)handle, UV_FS_POLL);
  return 0;
}


int uv_fs_poll_start(uv_fs_poll_t* handle,
                     uv_fs_poll_cb cb,
                     const char* path,
                     unsigned int interval) {
  struct poll_ctx* ctx;
  uv_loop_t* loop;
  size_t len;

  if (uv__is_active(handle))
    return 0;

  loop = handle->loop;
  len = strlen(path);
  ctx = calloc(1, sizeof(*ctx) + len);

  if (ctx == NULL)
    return uv__set_artificial_error(loop, UV_ENOMEM);

  ctx->loop = loop;
  ctx->poll_cb = cb;
  ctx->interval = interval ? interval : 1;
  ctx->start_time = uv_now(loop);
  ctx->parent_handle = handle;
  memcpy(ctx->path, path, len + 1);

  if (uv_timer_init(loop, &ctx->timer_handle))
    abort();

  ctx->timer_handle.flags |= UV__HANDLE_INTERNAL;
  uv__handle_unref(&ctx->timer_handle);

  if (uv_fs_stat(loop, &ctx->fs_req, ctx->path, poll_cb))
    abort();

  handle->poll_ctx = ctx;
  uv__handle_start(handle);

  return 0;
}


int uv_fs_poll_stop(uv_fs_poll_t* handle) {
  struct poll_ctx* ctx;

  if (!uv__is_active(handle))
    return 0;

  ctx = handle->poll_ctx;
  assert(ctx != NULL);
  assert(ctx->parent_handle != NULL);

  ctx->parent_handle = NULL;
  uv_timer_stop(&ctx->timer_handle);

  handle->poll_ctx = NULL;
  uv__handle_stop(handle);

  return 0;
}


void uv__fs_poll_close(uv_fs_poll_t* handle) {
  uv_fs_poll_stop(handle);
}


static void timer_cb(uv_timer_t* timer, int status) {
  struct poll_ctx* ctx;

  ctx = container_of(timer, struct poll_ctx, timer_handle);

  if (ctx->parent_handle == NULL) { /* handle has been stopped or closed */
    uv_close((uv_handle_t*)&ctx->timer_handle, timer_close_cb);
    return;
  }

  assert(ctx->parent_handle->poll_ctx == ctx);
  ctx->start_time = uv_now(ctx->loop);

  if (uv_fs_stat(ctx->loop, &ctx->fs_req, ctx->path, poll_cb))
    abort();
}


static void poll_cb(uv_fs_t* req) {
  uv_statbuf_t* statbuf;
  struct poll_ctx* ctx;
  uint64_t interval;

  ctx = container_of(req, struct poll_ctx, fs_req);

  if (ctx->parent_handle == NULL) { /* handle has been stopped or closed */
    uv_close((uv_handle_t*)&ctx->timer_handle, timer_close_cb);
    uv_fs_req_cleanup(req);
    return;
  }

  if (req->result != 0) {
    if (ctx->busy_polling != -req->errorno) {
      uv__set_artificial_error(ctx->loop, req->errorno);
      ctx->poll_cb(ctx->parent_handle, -1, &ctx->statbuf, &zero_statbuf);
      ctx->busy_polling = -req->errorno;
    }
    goto out;
  }

  statbuf = req->ptr;

  if (ctx->busy_polling != 0)
    if (ctx->busy_polling < 0 || !statbuf_eq(&ctx->statbuf, statbuf))
      ctx->poll_cb(ctx->parent_handle, 0, &ctx->statbuf, statbuf);

  ctx->statbuf = *statbuf;
  ctx->busy_polling = 1;

out:
  uv_fs_req_cleanup(req);

  /* Reschedule timer, subtract the delay from doing the stat(). */
  interval = ctx->interval;
  interval -= (uv_now(ctx->loop) - ctx->start_time) % interval;

  if (uv_timer_start(&ctx->timer_handle, timer_cb, interval, 0))
    abort();
}


static void timer_close_cb(uv_handle_t* handle) {
  free(container_of(handle, struct poll_ctx, timer_handle));
}


static int statbuf_eq(const uv_statbuf_t* a, const uv_statbuf_t* b) {
#ifdef _WIN32
  return a->st_mtime == b->st_mtime
      && a->st_size == b->st_size
      && a->st_mode == b->st_mode;
#else

  /* Jump through a few hoops to get sub-second granularity on Linux. */
# if __linux__
#  if __USE_MISC /* _BSD_SOURCE || _SVID_SOURCE */
  if (a->st_ctim.tv_nsec != b->st_ctim.tv_nsec) return 0;
  if (a->st_mtim.tv_nsec != b->st_mtim.tv_nsec) return 0;
#  else
  if (a->st_ctimensec != b->st_ctimensec) return 0;
  if (a->st_mtimensec != b->st_mtimensec) return 0;
#  endif
# endif

  /* Jump through different hoops on OS X. */
# if __APPLE__
#  if !defined(_POSIX_C_SOURCE) || defined(_DARWIN_C_SOURCE)
  if (a->st_ctimespec.tv_nsec != b->st_ctimespec.tv_nsec) return 0;
  if (a->st_mtimespec.tv_nsec != b->st_mtimespec.tv_nsec) return 0;
#  else
  if (a->st_ctimensec != b->st_ctimensec) return 0;
  if (a->st_mtimensec != b->st_mtimensec) return 0;
#  endif
# endif

  /* TODO(bnoordhuis) Other Unices have st_ctim and friends too, provided
   * the stars and compiler flags are right...
   */

  return a->st_ctime == b->st_ctime
      && a->st_mtime == b->st_mtime
      && a->st_size == b->st_size
      && a->st_mode == b->st_mode
      && a->st_uid == b->st_uid
      && a->st_gid == b->st_gid
      && a->st_ino == b->st_ino
      && a->st_dev == b->st_dev;
#endif
}


#ifdef _WIN32

#include "win/internal.h"
#include "win/handle-inl.h"

void uv__fs_poll_endgame(uv_loop_t* loop, uv_fs_poll_t* handle) {
  assert(handle->flags & UV_HANDLE_CLOSING);
  assert(!(handle->flags & UV_HANDLE_CLOSED));
  uv__handle_close(handle);
}

#endif /* _WIN32 */
