/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk_internal/accel_engine.h"

#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/crc32.h"

/* Accelerator Engine Framework: The following provides a top level
 * generic API for the accelerator functions defined here. Modules,
 * such as the one in /module/accel/ioat, supply the implemention of
 * with the exception of the pure software implemention contained
 * later in this file.
 */

#define ALIGN_4K 0x1000

/* Largest context size for all accel modules */
static size_t g_max_accel_module_size = 0;

static struct spdk_accel_engine *g_hw_accel_engine = NULL;
static struct spdk_accel_engine *g_sw_accel_engine = NULL;
static struct spdk_accel_module_if *g_accel_engine_module = NULL;
static spdk_accel_fini_cb g_fini_cb_fn = NULL;
static void *g_fini_cb_arg = NULL;

/* Global list of registered accelerator modules */
static TAILQ_HEAD(, spdk_accel_module_if) spdk_accel_module_list =
	TAILQ_HEAD_INITIALIZER(spdk_accel_module_list);

struct accel_io_channel {
	struct spdk_accel_engine	*engine;
	struct spdk_io_channel		*ch;
};

/* Registration of hw modules (currently supports only 1 at a time) */
void
spdk_accel_hw_engine_register(struct spdk_accel_engine *accel_engine)
{
	if (g_hw_accel_engine == NULL) {
		g_hw_accel_engine = accel_engine;
	} else {
		SPDK_NOTICELOG("Hardware offload engine already enabled\n");
	}
}

/* Registration of sw modules (currently supports only 1) */
static void
accel_sw_register(struct spdk_accel_engine *accel_engine)
{
	assert(g_sw_accel_engine == NULL);
	g_sw_accel_engine = accel_engine;
}

static void
accel_sw_unregister(void)
{
	g_sw_accel_engine = NULL;
}

/* Common completion routine, called only by the accel framework */
static void
_accel_engine_done(void *ref, int status)
{
	struct spdk_accel_task *req = (struct spdk_accel_task *)ref;

	req->cb(req, status);
}

uint64_t
spdk_accel_get_capabilities(struct spdk_io_channel *ch)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

	return accel_ch->engine->get_capabilities();
}

/* Accel framework public API for copy function */
int
spdk_accel_submit_copy(struct spdk_accel_task *accel_req, struct spdk_io_channel *ch,
		       void *dst, void *src, uint64_t nbytes, spdk_accel_completion_cb cb)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

	accel_req->cb = cb;
	return accel_ch->engine->copy(accel_req->offload_ctx, accel_ch->ch, dst, src, nbytes,
				      _accel_engine_done);
}

/* Accel framework public API for dual cast copy function */
int
spdk_accel_submit_dualcast(struct spdk_accel_task *accel_req, struct spdk_io_channel *ch,
			   void *dst1, void *dst2, void *src, uint64_t nbytes,
			   spdk_accel_completion_cb cb)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

	if ((uintptr_t)dst1 & (ALIGN_4K - 1) || (uintptr_t)dst2 & (ALIGN_4K - 1)) {
		SPDK_ERRLOG("Dualcast requires 4K alignment on dst addresses\n");
		return -EINVAL;
	}

	accel_req->cb = cb;
	return accel_ch->engine->dualcast(accel_req->offload_ctx, accel_ch->ch, dst1, dst2, src, nbytes,
					  _accel_engine_done);
}

/* Accel framework public API for batch_create function */
struct spdk_accel_batch *
spdk_accel_batch_create(struct spdk_io_channel *ch)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

	return accel_ch->engine->batch_create(accel_ch->ch);
}

/* Accel framework public API for batch_submit function */
int
spdk_accel_batch_submit(struct spdk_accel_task *accel_req, struct spdk_io_channel *ch,
			struct spdk_accel_batch *batch, spdk_accel_completion_cb cb)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

	accel_req->cb = cb;
	return accel_ch->engine->batch_submit(accel_req->offload_ctx, accel_ch->ch, batch,
					      _accel_engine_done);
}

/* Accel framework public API for getting max batch */
uint32_t
spdk_accel_batch_get_max(struct spdk_io_channel *ch)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

	return accel_ch->engine->batch_get_max();
}

/* Accel framework public API for batch prep_copy function */
int
spdk_accel_batch_prep_copy(struct spdk_accel_task *accel_req, struct spdk_io_channel *ch,
			   struct spdk_accel_batch *batch, void *dst, void *src, uint64_t nbytes,
			   spdk_accel_completion_cb cb)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

	accel_req->cb = cb;
	return accel_ch->engine->batch_prep_copy(accel_req->offload_ctx, accel_ch->ch, batch, dst, src,
			nbytes,
			_accel_engine_done);
}

/* Accel framework public API for compare function */
int
spdk_accel_submit_compare(struct spdk_accel_task *accel_req, struct spdk_io_channel *ch,
			  void *src1, void *src2, uint64_t nbytes, spdk_accel_completion_cb cb)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

	accel_req->cb = cb;
	return accel_ch->engine->compare(accel_req->offload_ctx, accel_ch->ch, src1, src2, nbytes,
					 _accel_engine_done);
}

/* Accel framework public API for fill function */
int
spdk_accel_submit_fill(struct spdk_accel_task *accel_req, struct spdk_io_channel *ch,
		       void *dst, uint8_t fill, uint64_t nbytes, spdk_accel_completion_cb cb)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

	accel_req->cb = cb;
	return accel_ch->engine->fill(accel_req->offload_ctx, accel_ch->ch, dst, fill, nbytes,
				      _accel_engine_done);
}

/* Accel framework public API for CRC-32C function */
int
spdk_accel_submit_crc32c(struct spdk_accel_task *accel_req, struct spdk_io_channel *ch,
			 uint32_t *dst, void *src, uint32_t seed, uint64_t nbytes, spdk_accel_completion_cb cb)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

	accel_req->cb = cb;
	return accel_ch->engine->crc32c(accel_req->offload_ctx, accel_ch->ch, dst, src,
					seed, nbytes, _accel_engine_done);
}


/* Returns the largest context size of the accel modules. */
size_t
spdk_accel_task_size(void)
{
	return g_max_accel_module_size;
}

/* Helper function when when accel modules register with the framework. */
void spdk_accel_module_list_add(struct spdk_accel_module_if *accel_module)
{
	TAILQ_INSERT_TAIL(&spdk_accel_module_list, accel_module, tailq);
	if (accel_module->get_ctx_size && accel_module->get_ctx_size() > g_max_accel_module_size) {
		g_max_accel_module_size = accel_module->get_ctx_size();
	}
}

/* Framework level channel create callback. */
static int
accel_engine_create_cb(void *io_device, void *ctx_buf)
{
	struct accel_io_channel	*accel_ch = ctx_buf;

	if (g_hw_accel_engine != NULL) {
		accel_ch->ch = g_hw_accel_engine->get_io_channel();
		if (accel_ch->ch != NULL) {
			accel_ch->engine = g_hw_accel_engine;
			return 0;
		}
	}

	/* No hw engine enabled, use sw. */
	accel_ch->ch = g_sw_accel_engine->get_io_channel();
	assert(accel_ch->ch != NULL);
	accel_ch->engine = g_sw_accel_engine;
	return 0;
}

/* Framework level channel destroy callback. */
static void
accel_engine_destroy_cb(void *io_device, void *ctx_buf)
{
	struct accel_io_channel	*accel_ch = ctx_buf;

	spdk_put_io_channel(accel_ch->ch);
}

struct spdk_io_channel *
spdk_accel_engine_get_io_channel(void)
{
	return spdk_get_io_channel(&spdk_accel_module_list);
}

static void
accel_engine_module_initialize(void)
{
	struct spdk_accel_module_if *accel_engine_module;

	TAILQ_FOREACH(accel_engine_module, &spdk_accel_module_list, tailq) {
		accel_engine_module->module_init();
	}
}

int
spdk_accel_engine_initialize(void)
{
	SPDK_NOTICELOG("Accel engine initialized to use software engine.\n");
	accel_engine_module_initialize();
	/*
	 * We need a unique identifier for the accel engine framework, so use the
	 *  spdk_accel_module_list address for this purpose.
	 */
	spdk_io_device_register(&spdk_accel_module_list, accel_engine_create_cb, accel_engine_destroy_cb,
				sizeof(struct accel_io_channel), "accel_module");

	return 0;
}

static void
accel_engine_module_finish_cb(void)
{
	spdk_accel_fini_cb cb_fn = g_fini_cb_fn;

	cb_fn(g_fini_cb_arg);
	g_fini_cb_fn = NULL;
	g_fini_cb_arg = NULL;
}

void
spdk_accel_write_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_accel_module_if *accel_engine_module;

	/*
	 * The accel engine has no config, there may be some in
	 * the modules though.
	 */
	spdk_json_write_array_begin(w);
	TAILQ_FOREACH(accel_engine_module, &spdk_accel_module_list, tailq) {
		if (accel_engine_module->write_config_json) {
			accel_engine_module->write_config_json(w);
		}
	}
	spdk_json_write_array_end(w);
}

void
spdk_accel_engine_module_finish(void)
{
	if (!g_accel_engine_module) {
		g_accel_engine_module = TAILQ_FIRST(&spdk_accel_module_list);
	} else {
		g_accel_engine_module = TAILQ_NEXT(g_accel_engine_module, tailq);
	}

	if (!g_accel_engine_module) {
		accel_engine_module_finish_cb();
		return;
	}

	if (g_accel_engine_module->module_fini) {
		spdk_thread_send_msg(spdk_get_thread(), g_accel_engine_module->module_fini, NULL);
	} else {
		spdk_accel_engine_module_finish();
	}
}

void
spdk_accel_engine_finish(spdk_accel_fini_cb cb_fn, void *cb_arg)
{
	assert(cb_fn != NULL);

	g_fini_cb_fn = cb_fn;
	g_fini_cb_arg = cb_arg;

	spdk_io_device_unregister(&spdk_accel_module_list, NULL);
	spdk_accel_engine_module_finish();
}

void
spdk_accel_engine_config_text(FILE *fp)
{
	struct spdk_accel_module_if *accel_engine_module;

	TAILQ_FOREACH(accel_engine_module, &spdk_accel_module_list, tailq) {
		if (accel_engine_module->config_text) {
			accel_engine_module->config_text(fp);
		}
	}
}

/* The SW Accelerator module is "built in" here (rest of file) */

static uint64_t
sw_accel_get_capabilities(void)
{
	return ACCEL_COPY | ACCEL_FILL | ACCEL_CRC32C | ACCEL_COMPARE |
	       ACCEL_DUALCAST;
}

static int
sw_accel_submit_copy(void *cb_arg, struct spdk_io_channel *ch, void *dst, void *src,
		     uint64_t nbytes,
		     spdk_accel_completion_cb cb)
{
	struct spdk_accel_task *accel_req;

	memcpy(dst, src, (size_t)nbytes);

	accel_req = (struct spdk_accel_task *)((uintptr_t)cb_arg -
					       offsetof(struct spdk_accel_task, offload_ctx));
	cb(accel_req, 0);
	return 0;
}

static int
sw_accel_submit_dualcast(void *cb_arg, struct spdk_io_channel *ch, void *dst1, void *dst2,
			 void *src, uint64_t nbytes, spdk_accel_completion_cb cb)
{
	struct spdk_accel_task *accel_req;

	memcpy(dst1, src, (size_t)nbytes);
	memcpy(dst2, src, (size_t)nbytes);

	accel_req = (struct spdk_accel_task *)((uintptr_t)cb_arg -
					       offsetof(struct spdk_accel_task, offload_ctx));
	cb(accel_req, 0);
	return 0;
}

static int
sw_accel_submit_compare(void *cb_arg, struct spdk_io_channel *ch, void *src1, void *src2,
			uint64_t nbytes,
			spdk_accel_completion_cb cb)
{
	struct spdk_accel_task *accel_req;
	int result;

	result = memcmp(src1, src2, (size_t)nbytes);

	accel_req = (struct spdk_accel_task *)((uintptr_t)cb_arg -
					       offsetof(struct spdk_accel_task, offload_ctx));
	cb(accel_req, result);

	return 0;
}

static int
sw_accel_submit_fill(void *cb_arg, struct spdk_io_channel *ch, void *dst, uint8_t fill,
		     uint64_t nbytes,
		     spdk_accel_completion_cb cb)
{
	struct spdk_accel_task *accel_req;

	memset(dst, fill, nbytes);
	accel_req = (struct spdk_accel_task *)((uintptr_t)cb_arg -
					       offsetof(struct spdk_accel_task, offload_ctx));
	cb(accel_req, 0);

	return 0;
}

static int
sw_accel_submit_crc32c(void *cb_arg, struct spdk_io_channel *ch, uint32_t *dst, void *src,
		       uint32_t seed, uint64_t nbytes,
		       spdk_accel_completion_cb cb)
{
	struct spdk_accel_task *accel_req;

	*dst = spdk_crc32c_update(src, nbytes, ~seed);
	accel_req = (struct spdk_accel_task *)((uintptr_t)cb_arg -
					       offsetof(struct spdk_accel_task, offload_ctx));
	cb(accel_req, 0);

	return 0;
}

static struct spdk_io_channel *sw_accel_get_io_channel(void);

static struct spdk_accel_engine sw_accel_engine = {
	.get_capabilities	= sw_accel_get_capabilities,
	.copy			= sw_accel_submit_copy,
	.dualcast		= sw_accel_submit_dualcast,
	.batch_get_max		= NULL, /* TODO */
	.batch_create		= NULL, /* TODO */
	.batch_prep_copy	= NULL, /* TODO */
	.batch_submit		= NULL, /* TODO */
	.compare		= sw_accel_submit_compare,
	.fill			= sw_accel_submit_fill,
	.crc32c			= sw_accel_submit_crc32c,
	.get_io_channel		= sw_accel_get_io_channel,
};

static int
sw_accel_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
sw_accel_destroy_cb(void *io_device, void *ctx_buf)
{
}

static struct spdk_io_channel *sw_accel_get_io_channel(void)
{
	return spdk_get_io_channel(&sw_accel_engine);
}

static size_t
sw_accel_engine_get_ctx_size(void)
{
	return sizeof(struct spdk_accel_task);
}

static int
sw_accel_engine_init(void)
{
	accel_sw_register(&sw_accel_engine);
	spdk_io_device_register(&sw_accel_engine, sw_accel_create_cb, sw_accel_destroy_cb, 0,
				"sw_accel_engine");

	return 0;
}

static void
sw_accel_engine_fini(void *ctxt)
{
	spdk_io_device_unregister(&sw_accel_engine, NULL);
	accel_sw_unregister();

	spdk_accel_engine_module_finish();
}

SPDK_ACCEL_MODULE_REGISTER(sw_accel_engine_init, sw_accel_engine_fini,
			   NULL, NULL, sw_accel_engine_get_ctx_size)
