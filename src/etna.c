/*
 * Copyright (c) 2012-2013 Etnaviv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include <etna.h>
#include <etna_bo.h>
#include <viv.h>
#include <etna_queue.h>
#include <state.xml.h>

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "gc_abi.h"

#include "viv_internal.h"

//#define DEBUG
//#define DEBUG_CMDBUF

/* Maximum number of flushes without queuing a signal (per command buffer).
   If this amount is reached, we roll to the next command buffer,
   which automatically queues a signal.
   XXX works around driver bug on (at least) cubox, for which drivers
       is this not needed? Not urgent as this does not result in a
       deducible performance impact.
*/
#define ETNA_MAX_UNSIGNALED_FLUSHES (40)

static int gpu_context_initialize(struct etna_ctx *ctx)
{
    /* attach to GPU */
    int err;
    gcsHAL_INTERFACE id = {};
    id.command = gcvHAL_ATTACH;
    if((err=viv_invoke(ctx->conn, &id)) != gcvSTATUS_OK)
    {
#ifdef DEBUG
        fprintf(stderr, "Error attaching to GPU\n");
#endif
        return ETNA_INTERNAL_ERROR;
    }

#ifdef DEBUG
    fprintf(stderr, "Context 0x%08x\n", (int)id.u.Attach.context);
#endif

    ctx->ctx = VIV_TO_HANDLE(id.u.Attach.context);
    return ETNA_OK;
}

static int gpu_context_free(struct etna_ctx *ctx)
{
    /* attach to GPU */
    int err;
    gcsHAL_INTERFACE id = {};
    id.command = gcvHAL_DETACH;
    id.u.Detach.context = HANDLE_TO_VIV(ctx->ctx);
    
    if((err=viv_invoke(ctx->conn, &id)) != gcvSTATUS_OK)
    {
#ifdef DEBUG
        fprintf(stderr, "Error detaching from the GPU\n");
#endif
        return ETNA_INTERNAL_ERROR;
    }

    return ETNA_OK;
}

int etna_create(struct viv_conn *conn, struct etna_ctx **ctx_out)
{
    int rv;
    if(ctx_out == NULL) return ETNA_INVALID_ADDR;
    struct etna_ctx *ctx = ETNA_CALLOC_STRUCT(etna_ctx);
    if(ctx == NULL) return ETNA_OUT_OF_MEMORY;
    ctx->conn = conn;

    if(gpu_context_initialize(ctx) != ETNA_OK)
    {
        ETNA_FREE(ctx);
        return ETNA_INTERNAL_ERROR;
    }

    /* Create synchronization signal */
    if(viv_user_signal_create(conn, 0, &ctx->sig_id) != 0) /* automatic resetting signal */
    {
#ifdef DEBUG
        fprintf(stderr, "Cannot create user signal\n");
#endif
        return ETNA_INTERNAL_ERROR;
    }
#ifdef DEBUG
    fprintf(stderr, "Created user signal %i\n", ctx->sig_id);
#endif

    /* Allocate command buffers, and create a synchronization signal for each.
     * Also signal the synchronization signal for the buffers to tell that the buffers are ready for use.
     */
    for(int x=0; x<NUM_COMMAND_BUFFERS; ++x)
    {
        ctx->cmdbuf[x] = ETNA_CALLOC_STRUCT(_gcoCMDBUF);
        if((ctx->cmdbufi[x].bo = etna_bo_new(conn, COMMAND_BUFFER_SIZE, DRM_ETNA_GEM_TYPE_CMD))==NULL)
        {
#ifdef DEBUG
            fprintf(stderr, "Error allocating host memory for command buffer\n");
#endif
            return ETNA_OUT_OF_MEMORY;
        }
        ctx->cmdbuf[x]->object.type = gcvOBJ_COMMANDBUFFER;
#ifdef GCABI_CMDBUF_HAS_PHYSICAL
        ctx->cmdbuf[x]->physical = PTR_TO_VIV((void*)etna_bo_gpu_address(ctx->cmdbufi[x].bo));
        ctx->cmdbuf[x]->bytes = ctx->cmdbufi[x].bytes;
#endif
        ctx->cmdbuf[x]->logical = PTR_TO_VIV((void*)etna_bo_map(ctx->cmdbufi[x].bo));

        if(viv_user_signal_create(conn, 0, &ctx->cmdbufi[x].sig_id) != 0 ||
           viv_user_signal_signal(conn, ctx->cmdbufi[x].sig_id, 1) != 0)
        {
#ifdef DEBUG
            fprintf(stderr, "Cannot create user signal\n");
#endif
            return ETNA_INTERNAL_ERROR;
        }
#ifdef DEBUG
        fprintf(stderr, "Allocated buffer %i: phys=%08x log=%08x bytes=%08x [signal %i]\n", x,
                (uint32_t)ctx->cmdbuf[x]->physical, (uint32_t)ctx->cmdbuf[x]->logical, ctx->cmdbuf[x]->bytes, ctx->cmdbufi[x].sig_id);
#endif
    }

    /* Allocate command queue */
    if((rv = etna_queue_create(ctx, &ctx->queue)) != ETNA_OK)
    {
#ifdef DEBUG
        fprintf(stderr, "Error allocating kernel command queue\n");
#endif
        return rv;
    }

    /* Set current buffer to ETNA_NO_BUFFER, to signify that we need to switch to buffer 0 before
     * queueing of commands can be started.
     */
    ctx->cur_buf = ETNA_NO_BUFFER;

    *ctx_out = ctx;
    return ETNA_OK;
}

/* Clear a command buffer */
static void clear_buffer(gcoCMDBUF cmdbuf)
{
    /* Prepare command buffer for use */
    cmdbuf->startOffset = 0x0;
    cmdbuf->offset = BEGIN_COMMIT_CLEARANCE;
}

/* Switch to next buffer, optionally wait for it to be available */
static int switch_next_buffer(struct etna_ctx *ctx)
{
    int next_buf_id = (ctx->cur_buf + 1) % NUM_COMMAND_BUFFERS;
#if 0
    fprintf(stderr, "Switching to new buffer %i\n", next_buf_id);
#endif
    if(viv_user_signal_wait(ctx->conn, ctx->cmdbufi[next_buf_id].sig_id, VIV_WAIT_INDEFINITE) != 0)
    {
#ifdef DEBUG
        fprintf(stderr, "Error waiting for command buffer sync signal\n");
#endif
        return ETNA_INTERNAL_ERROR;
    }
    clear_buffer(ctx->cmdbuf[next_buf_id]);
    ctx->cur_buf = next_buf_id;
    ctx->buf = VIV_TO_PTR(ctx->cmdbuf[next_buf_id]->logical);
    ctx->offset = ctx->cmdbuf[next_buf_id]->offset / 4;
#ifdef DEBUG
    fprintf(stderr, "Switched to command buffer %i\n", ctx->cur_buf);
#endif
    return ETNA_OK;
}

int etna_free(struct etna_ctx *ctx)
{
    if(ctx == NULL)
        return ETNA_INVALID_ADDR;
    /* Free kernel command queue */
    etna_queue_free(ctx->queue);
    /* Free command buffers */
    for(int x=0; x<NUM_COMMAND_BUFFERS; ++x)
    {
        viv_user_signal_destroy(ctx->conn, ctx->cmdbufi[x].sig_id);
        etna_bo_del(ctx->conn, ctx->cmdbufi[x].bo, NULL);
        ETNA_FREE(ctx->cmdbuf[x]);
    }
    viv_user_signal_destroy(ctx->conn, ctx->sig_id);
    gpu_context_free(ctx);

    ETNA_FREE(ctx);
    return ETNA_OK;
}

/* internal (non-inline) part of etna_reserve
 * - commit current command buffer (if there is a current command buffer)
 * - signify when current command buffer becomes available using a signal
 * - switch to next command buffer
 */
int _etna_reserve_internal(struct etna_ctx *ctx, size_t n)
{
    int status;
#ifdef DEBUG
    fprintf(stderr, "Buffer full\n");
#endif
    if((ctx->offset*4 + END_COMMIT_CLEARANCE) > COMMAND_BUFFER_SIZE)
    {
        fprintf(stderr, "%s: Command buffer overflow! This is likely a programming error in the GPU driver.\n", __func__);
        abort();
    }
    if(ctx->cur_buf != ETNA_NO_BUFFER)
    {
#if 0
        fprintf(stderr, "Submitting old buffer %i\n", ctx->cur_buf);
#endif
        /* Queue signal to signify when buffer is available again */
        if((status = etna_queue_signal(ctx->queue, ctx->cmdbufi[ctx->cur_buf].sig_id, VIV_WHERE_COMMAND)) != ETNA_OK)
        {
            fprintf(stderr, "%s: queue signal for old buffer failed: %i\n", __func__, status);
            abort(); /* buffer is in invalid state XXX need some kind of recovery */
        }
        /* Otherwise, if there is something to be committed left in the current command buffer, commit it */
        if((status = etna_flush(ctx, NULL)) != ETNA_OK)
        {
            fprintf(stderr, "%s: reserve failed: %i\n", __func__, status);
            abort(); /* buffer is in invalid state XXX need some kind of recovery */
        }
    }

    /* Move on to next buffer if not enough free in current one */
    if((status = switch_next_buffer(ctx)) != ETNA_OK)
    {
        fprintf(stderr, "%s: can't switch to next command buffer: %i\n", __func__, status);
        abort(); /* Buffer is in invalid state XXX need some kind of recovery.
                    This could involve waiting and re-uploading the context state. */
    }
    return status;
}

int etna_flush(struct etna_ctx *ctx, uint32_t *fence_out)
{
    int status = ETNA_OK;
    if(ctx == NULL)
        return ETNA_INVALID_ADDR;
    if(ctx->cur_buf == ETNA_CTX_BUFFER)
        /* Can never flush while building context buffer */
        return ETNA_INTERNAL_ERROR;

    if(fence_out) /* is a fence handle requested? */
    {
        uint32_t fence;
        int signal;
        /* Need to lock the fence mutex to make sure submits are ordered by
         * fence number.
         */
        pthread_mutex_lock(&ctx->conn->fence_mutex);
        do {
            /*   Get next fence ID */
            if((status = _viv_fence_new(ctx->conn, &fence, &signal)) != VIV_STATUS_OK)
            {
                fprintf(stderr, "%s: could not request fence\n", __func__);
                goto unlock_and_return_status;
            }
        } while(fence == 0); /* don't return fence handle 0 as it is interpreted as error value downstream */
        /*   Queue the signal. This can call in turn call this function (but
         * without fence) if the queue was full, so we should be able to handle
         * that. In that case, we will exit from this function with only
         * this fence in the queue and an empty command buffer.
         */
        if((status = etna_queue_signal(ctx->queue, signal, VIV_WHERE_PIXEL)) != ETNA_OK)
        {
            fprintf(stderr, "%s: error %i queueing fence signal %i\n", __func__, status, signal);
            goto unlock_and_return_status;
        }
        *fence_out = fence;
    }
    /***** Start fence mutex locked */
    /* Make sure to unlock the mutex before returning */
    struct _gcsQUEUE *queue_first = _etna_queue_first(ctx->queue);
    gcoCMDBUF cur_buf = (ctx->cur_buf != ETNA_NO_BUFFER) ? ctx->cmdbuf[ctx->cur_buf] : NULL;

    if(cur_buf == NULL || (ctx->offset*4 <= (cur_buf->startOffset + BEGIN_COMMIT_CLEARANCE)))
    {
        /* Nothing in command buffer; but if we end up here there may be kernel commands to submit. Do this seperately. */
        if(queue_first != NULL)
        {
            ctx->flushes = 0;
            if((status = viv_event_commit(ctx->conn, queue_first)) != 0)
            {
#ifdef DEBUG
                fprintf(stderr, "Error committing kernel commands\n");
#endif
                goto unlock_and_return_status;
            }
            if(fence_out) /* mark fence as submitted to kernel */
                _viv_fence_mark_pending(ctx->conn, *fence_out);
        }
        goto unlock_and_return_status;
    }

    cur_buf->offset = ctx->offset*4; /* Copy over current end offset into CMDBUF, for kernel */
#ifdef DEBUG
    fprintf(stderr, "Committing command buffer %i startOffset=%x offset=%x\n", ctx->cur_buf,
            cur_buf->startOffset, ctx->offset*4);
#endif
#ifdef DEBUG_CMDBUF
    etna_dump_cmd_buffer(ctx);
#endif
    if(!queue_first)
        ctx->flushes += 1;
    else
        ctx->flushes = 0;
    if((status = viv_commit(ctx->conn, cur_buf, ctx->ctx, queue_first)) != 0)
    {
#ifdef DEBUG
        fprintf(stderr, "Error committing command buffer\n");
#endif
        goto unlock_and_return_status;
    }
    if(fence_out)
    {
        _viv_fence_mark_pending(ctx->conn, *fence_out);
        pthread_mutex_unlock(&ctx->conn->fence_mutex);
    }
    /***** End fence mutex locked */
    cur_buf->startOffset = cur_buf->offset + END_COMMIT_CLEARANCE;
    cur_buf->offset = cur_buf->startOffset + BEGIN_COMMIT_CLEARANCE;

    if((cur_buf->offset + END_COMMIT_CLEARANCE) >= COMMAND_BUFFER_SIZE ||
       ctx->flushes > ETNA_MAX_UNSIGNALED_FLUSHES)
    {
        /* nothing more fits in buffer, prevent warning about buffer overflow
           on next etna_reserve.
         */
        cur_buf->startOffset = cur_buf->offset = COMMAND_BUFFER_SIZE - END_COMMIT_CLEARANCE;
    }

    /* Set writing offset for next etna_reserve. For convenience this is
       stored as an index instead of a byte offset.  */
    ctx->offset = cur_buf->offset / 4;
#ifdef DEBUG
    fprintf(stderr, "  New start offset: %x New offset: %x\n", cur_buf->startOffset, cur_buf->offset);
#endif
    return ETNA_OK;

unlock_and_return_status: /* Unlock fence mutex (if necessary) and return status */
    if(fence_out)
        pthread_mutex_unlock(&ctx->conn->fence_mutex);
    return status;
}

int etna_finish(struct etna_ctx *ctx)
{
    int status;
    if(ctx == NULL)
        return ETNA_INVALID_ADDR;
    /* Submit event queue with SIGNAL, fromWhere=gcvKERNEL_PIXEL (wait for pixel engine to finish) */
    if(etna_queue_signal(ctx->queue, ctx->sig_id, VIV_WHERE_PIXEL) != 0)
    {
        return ETNA_INTERNAL_ERROR;
    }
    if((status = etna_flush(ctx, NULL)) != ETNA_OK)
        return status;
#ifdef DEBUG
    fprintf(stderr, "finish: Waiting for signal...\n");
#endif
    /* Wait for signal */
    if(viv_user_signal_wait(ctx->conn, ctx->sig_id, VIV_WAIT_INDEFINITE) != 0)
    {
        return ETNA_INTERNAL_ERROR;
    }

    return ETNA_OK;
}

int etna_set_pipe(struct etna_ctx *ctx, enum etna_pipe pipe)
{
    int status;
    if(ctx == NULL)
        return ETNA_INVALID_ADDR;

    if((status = etna_reserve(ctx, 2)) != ETNA_OK)
        return status;
    ETNA_EMIT_LOAD_STATE(ctx, VIVS_GL_FLUSH_CACHE>>2, 1, 0);
    switch(pipe)
    {
    case ETNA_PIPE_2D: ETNA_EMIT(ctx, VIVS_GL_FLUSH_CACHE_PE2D); break;
    case ETNA_PIPE_3D: ETNA_EMIT(ctx, VIVS_GL_FLUSH_CACHE_DEPTH | VIVS_GL_FLUSH_CACHE_COLOR); break;
    default: return ETNA_INVALID_VALUE;
    }

    etna_stall(ctx, SYNC_RECIPIENT_FE, SYNC_RECIPIENT_PE);

    if((status = etna_reserve(ctx, 2)) != ETNA_OK)
        return status;
    ETNA_EMIT_LOAD_STATE(ctx, VIVS_GL_PIPE_SELECT>>2, 1, 0);
    ETNA_EMIT(ctx, pipe);

    return ETNA_OK;
}

int etna_semaphore(struct etna_ctx *ctx, uint32_t from, uint32_t to)
{
    int status;
    if(ctx == NULL)
        return ETNA_INVALID_ADDR;
    if((status = etna_reserve(ctx, 2)) != ETNA_OK)
        return status;
    ETNA_EMIT_LOAD_STATE(ctx, VIVS_GL_SEMAPHORE_TOKEN>>2, 1, 0);
    ETNA_EMIT(ctx, VIVS_GL_SEMAPHORE_TOKEN_FROM(from) | VIVS_GL_SEMAPHORE_TOKEN_TO(to));
    return ETNA_OK;
}

int etna_stall(struct etna_ctx *ctx, uint32_t from, uint32_t to)
{
    int status;
    if(ctx == NULL)
        return ETNA_INVALID_ADDR;
    if((status = etna_reserve(ctx, 4)) != ETNA_OK)
        return status;
    ETNA_EMIT_LOAD_STATE(ctx, VIVS_GL_SEMAPHORE_TOKEN>>2, 1, 0);
    ETNA_EMIT(ctx, VIVS_GL_SEMAPHORE_TOKEN_FROM(from) | VIVS_GL_SEMAPHORE_TOKEN_TO(to));
    if(from == SYNC_RECIPIENT_FE)
    {
        /* if the frontend is to be stalled, queue a STALL frontend command */
        ETNA_EMIT_STALL(ctx, from, to);
    } else {
        /* otherwise, load the STALL token state */
        ETNA_EMIT_LOAD_STATE(ctx, VIVS_GL_STALL_TOKEN>>2, 1, 0);
        ETNA_EMIT(ctx, VIVS_GL_STALL_TOKEN_FROM(from) | VIVS_GL_STALL_TOKEN_TO(to));
    }
    return ETNA_OK;
}

int etna_set_context_cb(struct etna_ctx *ctx, etna_context_snapshot_cb_t snapshot_cb, void *data)
{
    ctx->ctx_cb = snapshot_cb;
    ctx->ctx_cb_data = data;
    return ETNA_OK;
}

void etna_dump_cmd_buffer(struct etna_ctx *ctx)
{
    uint32_t start_offset = ctx->cmdbuf[ctx->cur_buf]->startOffset/4 + 8;
    uint32_t *buf = &ctx->buf[start_offset];
    size_t size = ctx->offset - start_offset;
    fprintf(stderr, "cmdbuf %u offset %u:\n", ctx->cur_buf, start_offset);
    for(unsigned idx=0; idx<size; idx+=8)
    {
        for (unsigned i=idx; i<size && i<idx+8; ++i)
        {
            fprintf(stderr, "%08x ", buf[i]);
        }
        fprintf(stderr, "\n");
    }
}

