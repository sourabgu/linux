/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <linux/anon_inodes.h>
#include <linux/sizes.h>

#include "i915_drv.h"
#include "intel_ringbuffer.h"
#include "intel_lrc.h"
#include "i915_oa_hsw.h"
#include "i915_oa_bdw.h"
#include "i915_oa_chv.h"
#include "i915_oa_skl.h"

/* Must be a power of two */
#define OA_BUFFER_SIZE	     SZ_16M
#define OA_TAKEN(tail, head) ((tail - head) & (OA_BUFFER_SIZE - 1))

/* frequency for forwarding samples from OA to perf buffer */
#define POLL_FREQUENCY 200
#define POLL_PERIOD max_t(u64, 10000, NSEC_PER_SEC / POLL_FREQUENCY)

static u32 i915_perf_stream_paranoid = true;

#define OA_EXPONENT_MAX 0x3f

#define GEN8_OAREPORT_REASON_TIMER          (1<<19)
#define GEN8_OAREPORT_REASON_TRIGGER1       (1<<20)
#define GEN8_OAREPORT_REASON_TRIGGER2       (1<<21)
#define GEN8_OAREPORT_REASON_CTX_SWITCH     (1<<22)
#define GEN8_OAREPORT_REASON_GO_TRANSITION  (1<<23)
#define GEN9_OAREPORT_REASON_CLK_RATIO      (1<<24)

/* Data common to periodic and RCS based samples */
struct oa_sample_data {
	u32 source;
	u32 ctx_id;
	u32 pid;
	u32 tag;
	const u8 *report;
};

/* for sysctl proc_dointvec_minmax of i915_oa_min_timer_exponent */
static int zero;
static int oa_exponent_max = OA_EXPONENT_MAX;

/* Theoretically we can program the OA unit to sample every 160ns but don't
 * allow that by default unless root...
 *
 * The period is derived from the exponent as:
 *
 *   period = 80ns * 2^(exponent + 1)
 *
 * Referring to perf's kernel.perf_event_max_sample_rate for a precedent
 * (100000 by default); with an OA exponent of 6 we get a period of 10.240
 * microseconds - just under 100000Hz
 */
static u32 i915_oa_min_timer_exponent = 6;

static struct i915_oa_format hsw_oa_formats[I915_OA_FORMAT_MAX] = {
	[I915_OA_FORMAT_A13]	    = { 0, 64 },
	[I915_OA_FORMAT_A29]	    = { 1, 128 },
	[I915_OA_FORMAT_A13_B8_C8]  = { 2, 128 },
	/* A29_B8_C8 Disallowed as 192 bytes doesn't factor into buffer size */
	[I915_OA_FORMAT_B4_C8]	    = { 4, 64 },
	[I915_OA_FORMAT_A45_B8_C8]  = { 5, 256 },
	[I915_OA_FORMAT_B4_C8_A16]  = { 6, 128 },
	[I915_OA_FORMAT_C4_B8]	    = { 7, 64 },
};

static struct i915_oa_format gen8_plus_oa_formats[I915_OA_FORMAT_MAX] = {
	[I915_OA_FORMAT_A12]		    = { 0, 64 },
	[I915_OA_FORMAT_A12_B8_C8]	    = { 2, 128 },
	[I915_OA_FORMAT_A32u40_A4u32_B8_C8] = { 5, 256 },
	[I915_OA_FORMAT_C4_B8]		    = { 7, 64 },
};

#define SAMPLE_OA_REPORT	(1<<0)
#define SAMPLE_OA_SOURCE_INFO	(1<<1)
#define SAMPLE_CTX_ID		(1<<2)
#define SAMPLE_PID		(1<<3)
#define SAMPLE_TAG		(1<<4)

struct perf_open_properties
{
	u32 sample_flags;

	u64 single_context:1;
	u64 ctx_handle;

	/* OA sampling state */
	int metrics_set;
	int oa_format;
	bool oa_periodic;
	u32 oa_period_exponent;

	/* Command stream mode */
	bool cs_mode;
	enum intel_ring_id ring_id;
};

/*
 * Emit the commands to capture metrics, into the command stream. This function
 * can be called concurrently with the stream operations and doesn't require
 * perf mutex lock.
 */

void i915_perf_command_stream_hook(struct drm_i915_gem_request *req, u32 tag)
{
	struct intel_engine_cs *ring = req->ring;
	struct drm_i915_private *dev_priv = ring->dev->dev_private;
	struct i915_perf_stream *stream;

	if (!dev_priv->perf.initialized)
		return;

	mutex_lock(&dev_priv->perf.streams_lock);
	list_for_each_entry(stream, &dev_priv->perf.streams, link) {
		if (stream->enabled && stream->command_stream_hook)
			stream->command_stream_hook(req, tag);
	}
	mutex_unlock(&dev_priv->perf.streams_lock);
}

/*
 * Release some perf entries to make space for a new entry data. We dereference
 * the associated request before deleting the entry. Also, no need to check for
 * gpu completion of commands, since, these entries are anyways going to be
 * replaced by a new entry, and gpu will overwrite the buffer contents
 * eventually, when the request associated with new entry completes.
 */
static void release_some_perf_entries(struct drm_i915_private *dev_priv,
					u32 target_size)
{
	struct i915_perf_cs_data_node *entry, *next;
	u32 entry_size = dev_priv->perf.oa.oa_buffer.format_size;
	u32 size = 0;

	list_for_each_entry_safe
		(entry, next, &dev_priv->perf.node_list, link) {

		size += entry_size;
		i915_gem_request_unreference(entry->request);
		list_del(&entry->link);
		kfree(entry);

		if (size >= target_size)
			break;
	}
}

/*
 * Insert the perf entry to the end of the list. This function never fails,
 * since it always manages to insert the entry. If the space is exhausted in
 * the buffer, it will remove the oldest entries in order to make space.
 */
static void insert_perf_entry(struct drm_i915_private *dev_priv,
				struct i915_perf_cs_data_node *entry)
{
	struct i915_perf_cs_data_node *first_entry, *last_entry;
	int max_offset = dev_priv->perf.command_stream_buf.obj->base.size;
	u32 entry_size = dev_priv->perf.oa.oa_buffer.format_size;

	spin_lock(&dev_priv->perf.node_list_lock);
	if (list_empty(&dev_priv->perf.node_list)) {
		entry->offset = 0;
		list_add_tail(&entry->link, &dev_priv->perf.node_list);
		spin_unlock(&dev_priv->perf.node_list_lock);
		return;
	}

	first_entry = list_first_entry(&dev_priv->perf.node_list,
				       typeof(*first_entry), link);
	last_entry = list_last_entry(&dev_priv->perf.node_list,
				     typeof(*first_entry), link);

	if (last_entry->offset >= first_entry->offset) {
		/* Sufficient space available at the end of buffer? */
		if (last_entry->offset + 2*entry_size < max_offset)
			entry->offset = last_entry->offset + entry_size;
		/*
		 * Wraparound condition. Is sufficient space available at
		 * beginning of buffer?
		 */
		else if (entry_size < first_entry->offset)
			entry->offset = 0;
		/* Insufficient space. Overwrite existing old entries */
		else {
			u32 target_size = entry_size - first_entry->offset;

			release_some_perf_entries(dev_priv, target_size);
			entry->offset = 0;
		}
	} else {
		/* Sufficient space available? */
		if (last_entry->offset + 2*entry_size < first_entry->offset)
			entry->offset = last_entry->offset + entry_size;
		/* Insufficient space. Overwrite existing old entries */
		else {
			u32 target_size = entry_size -
				(first_entry->offset - last_entry->offset -
				entry_size);

			release_some_perf_entries(dev_priv, target_size);
			entry->offset = last_entry->offset + entry_size;
		}
	}
	list_add_tail(&entry->link, &dev_priv->perf.node_list);
	spin_unlock(&dev_priv->perf.node_list_lock);
}

static void i915_perf_command_stream_hook_oa(struct drm_i915_gem_request *req,
						u32 tag)
{
	struct intel_engine_cs *ring = req->ring;
	struct intel_ringbuffer *ringbuf = req->ringbuf;
	struct intel_context *ctx = req->ctx;
	struct drm_i915_private *dev_priv = ring->dev->dev_private;
	struct i915_perf_cs_data_node *entry;
	u32 addr = 0;
	int ret;

	/* OA counters are only supported on the render ring */
	BUG_ON(ring->id != RCS);

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (entry == NULL) {
		DRM_ERROR("alloc failed\n");
		return;
	}

	if (i915.enable_execlists)
		ret = intel_logical_ring_begin(req, 4);
	else
		ret = intel_ring_begin(req, 4);

	if (ret) {
		kfree(entry);
		return;
	}

	entry->ctx_id = ctx->global_id;
	entry->pid = current->pid;
	entry->tag = tag;
	i915_gem_request_assign(&entry->request, req);

	insert_perf_entry(dev_priv, entry);

	addr = dev_priv->perf.command_stream_buf.vma->node.start +
		entry->offset;

	/* addr should be 64 byte aligned */
	BUG_ON(addr & 0x3f);

	if (i915.enable_execlists) {
		intel_logical_ring_emit(ringbuf, MI_REPORT_PERF_COUNT | (2<<0));
		intel_logical_ring_emit(ringbuf,
					addr | MI_REPORT_PERF_COUNT_GGTT);
		intel_logical_ring_emit(ringbuf, 0);
		intel_logical_ring_emit(ringbuf,
			i915_gem_request_get_seqno(req));
		intel_logical_ring_advance(ringbuf);
	} else {
		if (INTEL_INFO(ring->dev)->gen >= 8) {
			intel_ring_emit(ring, MI_REPORT_PERF_COUNT | (2<<0));
			intel_ring_emit(ring, addr | MI_REPORT_PERF_COUNT_GGTT);
			intel_ring_emit(ring, 0);
			intel_ring_emit(ring,
				i915_gem_request_get_seqno(req));
		} else {
			intel_ring_emit(ring, MI_REPORT_PERF_COUNT | (1<<0));
			intel_ring_emit(ring, addr | MI_REPORT_PERF_COUNT_GGTT);
			intel_ring_emit(ring, i915_gem_request_get_seqno(req));
			intel_ring_emit(ring, MI_NOOP);
		}
		intel_ring_advance(ring);
	}
	i915_vma_move_to_active(dev_priv->perf.command_stream_buf.vma, req);
}

static int i915_oa_rcs_wait_gpu(struct drm_i915_private *dev_priv)
{
	struct i915_perf_cs_data_node *last_entry = NULL;
	struct drm_i915_gem_request *req = NULL;
	int ret;

	/*
	 * Wait for the last scheduled request to complete. This would
	 * implicitly wait for the prior submitted requests. The refcount
	 * of the requests is not decremented here.
	 */
	spin_lock(&dev_priv->perf.node_list_lock);

	if (!list_empty(&dev_priv->perf.node_list)) {
		last_entry = list_last_entry(&dev_priv->perf.node_list,
			struct i915_perf_cs_data_node, link);
		req = last_entry->request;
	}
	spin_unlock(&dev_priv->perf.node_list_lock);

	if (!req)
		return 0;

	ret = __i915_wait_request(req, atomic_read(
				&dev_priv->gpu_error.reset_counter),
				true, NULL, NULL);
	if (ret) {
		DRM_ERROR("failed to wait\n");
		return ret;
	}
	return 0;
}

static void i915_oa_rcs_free_requests(struct drm_i915_private *dev_priv)
{
	struct i915_perf_cs_data_node *entry, *next;

	list_for_each_entry_safe
		(entry, next, &dev_priv->perf.node_list, link) {
		i915_gem_request_unreference__unlocked(entry->request);

		spin_lock(&dev_priv->perf.node_list_lock);
		list_del(&entry->link);
		spin_unlock(&dev_priv->perf.node_list_lock);
		kfree(entry);
	}
}

static bool gen8_oa_buffer_is_empty(struct drm_i915_private *dev_priv)
{
	u32 head = I915_READ(GEN8_OAHEADPTR);
	u32 tail = I915_READ(GEN8_OATAILPTR);

	return OA_TAKEN(tail, head) == 0;
}

static bool gen7_oa_buffer_is_empty(struct drm_i915_private *dev_priv)
{
	u32 oastatus2 = I915_READ(GEN7_OASTATUS2);
	u32 oastatus1 = I915_READ(GEN7_OASTATUS1);
	u32 head = oastatus2 & GEN7_OASTATUS2_HEAD_MASK;
	u32 tail = oastatus1 & GEN7_OASTATUS1_TAIL_MASK;

	return OA_TAKEN(tail, head) == 0;
}

static bool append_oa_status(struct i915_perf_stream *stream,
			     struct i915_perf_read_state *read_state,
			     enum drm_i915_perf_record_type type)
{
	struct drm_i915_perf_record_header header = { type, 0, sizeof(header) };

	if ((read_state->count - read_state->read) < header.size)
		return false;

	copy_to_user(read_state->buf, &header, sizeof(header));

	read_state->buf += sizeof(header);
	read_state->read += header.size;

	return true;
}

static bool append_oa_sample(struct i915_perf_stream *stream,
			     struct i915_perf_read_state *read_state,
			     struct oa_sample_data *data)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	int report_size = dev_priv->perf.oa.oa_buffer.format_size;
	struct drm_i915_perf_record_header header;
	u32 sample_flags = stream->sample_flags;

	header.type = DRM_I915_PERF_RECORD_SAMPLE;
	header.pad = 0;
	header.size = stream->sample_size;

	if ((read_state->count - read_state->read) < header.size)
		return false;

	copy_to_user(read_state->buf, &header, sizeof(header));
	read_state->buf += sizeof(header);

	if (sample_flags & SAMPLE_OA_SOURCE_INFO) {
		if (copy_to_user(read_state->buf, &data->source, 4))
			return false;
		read_state->buf += 4;
	}

	if (sample_flags & SAMPLE_CTX_ID) {
		if (copy_to_user(read_state->buf, &data->ctx_id, 4))
			return false;
		read_state->buf += 4;
	}

	if (sample_flags & SAMPLE_PID) {
		if (copy_to_user(read_state->buf, &data->pid, 4))
			return false;
		read_state->buf += 4;
	}

	if (sample_flags & SAMPLE_TAG) {
		if (copy_to_user(read_state->buf, &data->tag, 4))
			return false;
		read_state->buf += 4;
	}

	if (sample_flags & SAMPLE_OA_REPORT) {
		if (copy_to_user(read_state->buf, data->report, report_size))
			return false;
		read_state->buf += report_size;
	}

	read_state->read += header.size;

	return true;
}

static bool append_oa_buffer_sample(struct i915_perf_stream *stream,
				    struct i915_perf_read_state *read_state,
				    const u8 *report)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	u32 sample_flags = stream->sample_flags;
	struct oa_sample_data data = { 0 };

	if (sample_flags & SAMPLE_OA_SOURCE_INFO) {
		enum drm_i915_perf_oa_event_source source;

		if (INTEL_INFO(dev_priv)->gen >= 8) {
			u32 reason = *(u32 *)report;

			if (reason & GEN8_OAREPORT_REASON_CTX_SWITCH)
				source =
				I915_PERF_OA_EVENT_SOURCE_CONTEXT_SWITCH;
			else if (reason & GEN8_OAREPORT_REASON_TIMER)
				source = I915_PERF_OA_EVENT_SOURCE_PERIODIC;
			else
				source = I915_PERF_OA_EVENT_SOURCE_UNDEFINED;
		} else
			source = I915_PERF_OA_EVENT_SOURCE_PERIODIC;

		data.source = source;
	}
#warning "FIXME: append_oa_buffer_sample: read ctx ID from report and map that to an intel_context::global_id"
	if (sample_flags & SAMPLE_CTX_ID)
		data.ctx_id = 0;

#warning "FIXME: append_oa_buffer_sample: deduce pid for periodic samples based on most recent RCS pid for ctx"
	if (sample_flags & SAMPLE_PID)
		data.pid = 0;

#warning "FIXME: append_oa_buffer_sample: deduce tag for periodic samples based on most recent RCS tag for ctx"
	if (sample_flags & SAMPLE_TAG)
		data.tag = 0;

	if (sample_flags & SAMPLE_OA_REPORT)
		data.report = report;

	append_oa_sample(stream, read_state, &data);

	return true;
}

static u32 gen8_append_oa_reports(struct i915_perf_stream *stream,
				  struct i915_perf_read_state *read_state,
				  u32 head,
				  u32 tail, u32 ts)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	int report_size = dev_priv->perf.oa.oa_buffer.format_size;
	u8 *oa_buf_base = dev_priv->perf.oa.oa_buffer.addr;
	u32 mask = (OA_BUFFER_SIZE - 1);
	u8 *report;
	u32 report_ts, taken;

	head -= dev_priv->perf.oa.oa_buffer.gtt_offset;
	tail -= dev_priv->perf.oa.oa_buffer.gtt_offset;

	/* Note: the gpu doesn't wrap the tail according to the OA buffer size
	 * so when we need to make sure our head/tail values are in-bounds we
	 * use the above mask.
	 */

	while ((taken = OA_TAKEN(tail, head))) {
		u32 ctx_id;

		/* The tail increases in 64 byte increments, not in
		 * format_size steps. */
		if (taken < report_size)
			break;

		/* All the report sizes factor neatly into the buffer
		 * size so we never expect to see a report split
		 * between the beginning and end of the buffer... */
		BUG_ON((OA_BUFFER_SIZE - (head & mask)) < report_size);

		report = oa_buf_base + (head & mask);

		report_ts = *(u32 *)(report + 4);
		if (report_ts > ts)
			break;

		ctx_id = *(u32 *)(report + 12);
		if (i915.enable_execlists) {
			/* XXX: Just keep the lower 20 bits for now since I'm
			 * not entirely sure if the HW touches any of the higher
			 * bits */
			ctx_id &= 0xfffff;
		}

		if (dev_priv->perf.oa.exclusive_stream->enabled) {

			/* NB: For Gen 8 we handle per-context report filtering
			 * ourselves instead of programming the OA unit with a
			 * specific context id.
			 *
			 * NB: To allow userspace to calculate all counter
			 * deltas for a specific context we have to send the
			 * first report belonging to any subsequently
			 * switched-too context.
			 */
			if (!dev_priv->perf.oa.exclusive_stream->ctx ||
			    (dev_priv->perf.oa.specific_ctx_id == ctx_id ||
			     (dev_priv->perf.oa.specific_ctx_id !=
			      dev_priv->perf.oa.oa_buffer.last_ctx_id))) {

				if (!append_oa_buffer_sample(stream, read_state,
								report))
					break;
			}
		}

		/*
		 * If append_oa_buffer_sample() returns false we shouldn't
		 * progress head so we update it afterwards.
		 */
		dev_priv->perf.oa.oa_buffer.last_ctx_id = ctx_id;
		head += report_size;
	}

	return dev_priv->perf.oa.oa_buffer.gtt_offset + head;
}

static void gen8_oa_read(struct i915_perf_stream *stream,
			 struct i915_perf_read_state *read_state, u32 ts)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	u32 oastatus;
	u32 head;
	u32 tail;

	WARN_ON(!dev_priv->perf.oa.oa_buffer.addr);

	head = I915_READ(GEN8_OAHEADPTR);
	tail = I915_READ(GEN8_OATAILPTR);
	oastatus = I915_READ(GEN8_OASTATUS);

	if (unlikely(oastatus & (GEN8_OASTATUS_OABUFFER_OVERFLOW |
				 GEN8_OASTATUS_REPORT_LOST))) {

		if (oastatus & GEN8_OASTATUS_OABUFFER_OVERFLOW) {
			if (append_oa_status(stream, read_state,
					     DRM_I915_PERF_RECORD_OA_BUFFER_OVERFLOW))
				oastatus &= ~GEN8_OASTATUS_OABUFFER_OVERFLOW;
		}

		if (oastatus & GEN8_OASTATUS_REPORT_LOST) {
			if (append_oa_status(stream, read_state,
					     DRM_I915_PERF_RECORD_OA_REPORT_LOST))
				oastatus &= ~GEN8_OASTATUS_REPORT_LOST;
		}

		I915_WRITE(GEN8_OASTATUS, oastatus);
	}

	head = gen8_append_oa_reports(stream, read_state, head, tail, ts);

	I915_WRITE(GEN8_OAHEADPTR, head);
}

static u32 gen7_append_oa_reports(struct i915_perf_stream *stream,
				  struct i915_perf_read_state *read_state,
				  u32 head,
				  u32 tail, u32 ts)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	int report_size = dev_priv->perf.oa.oa_buffer.format_size;
	u8 *oa_buf_base = dev_priv->perf.oa.oa_buffer.addr;
	u32 mask = (OA_BUFFER_SIZE - 1);
	u8 *report;
	u32 report_ts, taken;

	head -= dev_priv->perf.oa.oa_buffer.gtt_offset;
	tail -= dev_priv->perf.oa.oa_buffer.gtt_offset;

	/* Note: the gpu doesn't wrap the tail according to the OA buffer size
	 * so when we need to make sure our head/tail values are in-bounds we
	 * use the above mask.
	 */

	while ((taken = OA_TAKEN(tail, head))) {
		/* The tail increases in 64 byte increments, not in
		 * format_size steps. */
		if (taken < report_size)
			break;

		report = oa_buf_base + (head & mask);

		report_ts = *(u32 *)(report + 4);
		if (report_ts > ts)
			break;

		if (dev_priv->perf.oa.exclusive_stream->enabled) {
			if (!append_oa_buffer_sample(stream, read_state,
							report))
				break;
		}

		/*
		 * If append_oa_buffer_sample() returns false we shouldn't
		 * progress head so we update it afterwards.
		 */
		head += report_size;
	}

	return dev_priv->perf.oa.oa_buffer.gtt_offset + head;
}

static void gen7_oa_read(struct i915_perf_stream *stream,
			 struct i915_perf_read_state *read_state, u32 ts)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	u32 oastatus2;
	u32 oastatus1;
	u32 head;
	u32 tail;

	WARN_ON(!dev_priv->perf.oa.oa_buffer.addr);

	oastatus2 = I915_READ(GEN7_OASTATUS2);
	oastatus1 = I915_READ(GEN7_OASTATUS1);

	head = oastatus2 & GEN7_OASTATUS2_HEAD_MASK;
	tail = oastatus1 & GEN7_OASTATUS1_TAIL_MASK;

	if (unlikely(oastatus1 & (GEN7_OASTATUS1_OABUFFER_OVERFLOW |
				  GEN7_OASTATUS1_REPORT_LOST))) {

		if (oastatus1 & GEN7_OASTATUS1_OABUFFER_OVERFLOW) {
			if (append_oa_status(stream, read_state,
					     DRM_I915_PERF_RECORD_OA_BUFFER_OVERFLOW))
				oastatus1 &= ~GEN7_OASTATUS1_OABUFFER_OVERFLOW;
		}

		if (oastatus1 & GEN7_OASTATUS1_REPORT_LOST) {
			if (append_oa_status(stream, read_state,
					     DRM_I915_PERF_RECORD_OA_REPORT_LOST))
				oastatus1 &= ~GEN7_OASTATUS1_REPORT_LOST;
		}

		I915_WRITE(GEN7_OASTATUS1, oastatus1);
	}

	head = gen7_append_oa_reports(stream, read_state, head, tail, ts);

	I915_WRITE(GEN7_OASTATUS2, (head & GEN7_OASTATUS2_HEAD_MASK) |
				    OA_MEM_SELECT_GGTT);
}

static bool append_oa_rcs_sample(struct i915_perf_stream *stream,
				 struct i915_perf_read_state *read_state,
				 struct i915_perf_cs_data_node *node)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	struct oa_sample_data data = { 0 };
	const u8 *report = dev_priv->perf.command_stream_buf.addr +
				node->offset;
	u32 sample_flags = stream->sample_flags;
	u32 report_ts;

	/*
	 * Forward the periodic OA samples which have the timestamp lower
	 * than timestamp of this sample, before forwarding this sample.
	 * This ensures samples read by user are order acc. to their timestamps
	 */
	report_ts = *(u32 *)(report + 4);
	dev_priv->perf.oa.ops.read(stream, read_state, report_ts);

	if (sample_flags & SAMPLE_OA_SOURCE_INFO)
		data.source = I915_PERF_OA_EVENT_SOURCE_RCS;

	if (sample_flags & SAMPLE_CTX_ID)
		data.ctx_id = node->ctx_id;

	if (sample_flags & SAMPLE_PID)
		data.pid = node->pid;

	if (sample_flags & SAMPLE_TAG)
		data.tag = node->tag;

	if (sample_flags & SAMPLE_OA_REPORT)
		data.report = report;

	append_oa_sample(stream, read_state, &data);

	return true;
}

static void oa_rcs_append_reports(struct i915_perf_stream *stream,
				  struct i915_perf_read_state *read_state)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	struct i915_perf_cs_data_node *entry, *next;

	list_for_each_entry_safe(entry, next,
				 &dev_priv->perf.node_list, link) {
		if (!i915_gem_request_completed(entry->request, true))
			break;

		if (!append_oa_rcs_sample(stream, read_state, entry))
			break;

		spin_lock(&dev_priv->perf.node_list_lock);
		list_del(&entry->link);
		spin_unlock(&dev_priv->perf.node_list_lock);

		i915_gem_request_unreference__unlocked(entry->request);
		kfree(entry);
	}

	/* Flush any remaining periodic reports */
	dev_priv->perf.oa.ops.read(stream, read_state, U32_MAX);
}

static bool command_stream_buf_is_empty(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	if (stream->cs_mode)
		return list_empty(&dev_priv->perf.node_list);
	else
		return true;
}

static bool stream_have_data__unlocked(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	/* Note: the oa_buffer_is_empty() condition is ok to run unlocked as it
	 * just performs mmio reads of the OA buffer head + tail pointers and
	 * it's assumed we're handling some operation that implies the stream
	 * can't be destroyed until completion (such as a read()) that ensures
	 * the device + OA buffer can't disappear
	 */
	return !(dev_priv->perf.oa.ops.oa_buffer_is_empty(dev_priv) &&
		 command_stream_buf_is_empty(stream));
}

static bool i915_oa_can_read(struct i915_perf_stream *stream)
{

	return stream_have_data__unlocked(stream);
}

static int i915_oa_wait_unlocked(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	int ret;

	if (stream->cs_mode) {
		ret = i915_oa_rcs_wait_gpu(dev_priv);
		if (ret)
			return ret;
	}

	return wait_event_interruptible(dev_priv->perf.oa.poll_wq,
					stream_have_data__unlocked(stream));
}

static void i915_oa_poll_wait(struct i915_perf_stream *stream,
			      struct file *file,
			      poll_table *wait)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	poll_wait(file, &dev_priv->perf.oa.poll_wq, wait);
}

static void i915_oa_read(struct i915_perf_stream *stream,
			 struct i915_perf_read_state *read_state)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	if (stream->cs_mode)
		oa_rcs_append_reports(stream, read_state);
	else
		dev_priv->perf.oa.ops.read(stream, read_state, U32_MAX);
}

static void
free_command_stream_buf(struct drm_i915_private *i915)
{
	mutex_lock(&i915->dev->struct_mutex);

	vunmap(i915->perf.command_stream_buf.addr);
	i915_gem_object_ggtt_unpin(i915->perf.command_stream_buf.obj);
	drm_gem_object_unreference(&i915->perf.command_stream_buf.obj->base);

	i915->perf.command_stream_buf.obj = NULL;
	i915->perf.command_stream_buf.vma = NULL;
	i915->perf.command_stream_buf.addr = NULL;

	mutex_unlock(&i915->dev->struct_mutex);
}

static void
free_oa_buffer(struct drm_i915_private *i915)
{
	mutex_lock(&i915->dev->struct_mutex);

	vunmap(i915->perf.oa.oa_buffer.addr);
	i915_gem_object_ggtt_unpin(i915->perf.oa.oa_buffer.obj);
	drm_gem_object_unreference(&i915->perf.oa.oa_buffer.obj->base);

	i915->perf.oa.oa_buffer.obj = NULL;
	i915->perf.oa.oa_buffer.gtt_offset = 0;
	i915->perf.oa.oa_buffer.addr = NULL;

	mutex_unlock(&i915->dev->struct_mutex);
}

static void i915_oa_stream_destroy(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	BUG_ON(stream != dev_priv->perf.oa.exclusive_stream);

	if (stream->cs_mode)
		free_command_stream_buf(dev_priv);

	if (dev_priv->perf.oa.oa_buffer.obj) {
		dev_priv->perf.oa.ops.disable_metric_set(dev_priv);

		free_oa_buffer(dev_priv);

		intel_uncore_forcewake_put(dev_priv, FORCEWAKE_ALL);
		intel_runtime_pm_put(dev_priv);
	}

	dev_priv->perf.oa.exclusive_stream = NULL;
}

static void *vmap_oa_buffer(struct drm_i915_gem_object *obj)
{
	int i;
	void *addr = NULL;
	struct sg_page_iter sg_iter;
	struct page **pages;

	pages = drm_malloc_ab(obj->base.size >> PAGE_SHIFT, sizeof(*pages));
	if (pages == NULL) {
		DRM_DEBUG_DRIVER("Failed to get space for pages\n");
		goto finish;
	}

	i = 0;
	for_each_sg_page(obj->pages->sgl, &sg_iter, obj->pages->nents, 0) {
		pages[i] = sg_page_iter_page(&sg_iter);
		i++;
	}

	addr = vmap(pages, i, 0, PAGE_KERNEL);
	if (addr == NULL) {
		DRM_DEBUG_DRIVER("Failed to vmap pages\n");
		goto finish;
	}

finish:
	if (pages)
		drm_free_large(pages);
	return addr;
}

static void gen7_init_oa_buffer(struct drm_i915_private *dev_priv)
{
	/* Pre-DevBDW: OABUFFER must be set with counters off,
	 * before OASTATUS1, but after OASTATUS2 */
	I915_WRITE(GEN7_OASTATUS2, dev_priv->perf.oa.oa_buffer.gtt_offset |
		   OA_MEM_SELECT_GGTT); /* head */
	I915_WRITE(GEN7_OABUFFER, dev_priv->perf.oa.oa_buffer.gtt_offset);
	I915_WRITE(GEN7_OASTATUS1, dev_priv->perf.oa.oa_buffer.gtt_offset |
		   OABUFFER_SIZE_16M); /* tail */
}

static void gen8_init_oa_buffer(struct drm_i915_private *dev_priv)
{
	I915_WRITE(GEN8_OAHEADPTR,
		   dev_priv->perf.oa.oa_buffer.gtt_offset);
	/* PRM says:
	 *
	 *  "This MMIO must be set before the OATAILPTR
	 *  register and after the OAHEADPTR register. This is
	 *  to enable proper functionality of the overflow
	 *  bit."
	 */
	I915_WRITE(GEN8_OABUFFER, dev_priv->perf.oa.oa_buffer.gtt_offset |
		   OABUFFER_SIZE_16M | OA_MEM_SELECT_GGTT);
	I915_WRITE(GEN8_OATAILPTR,
		   dev_priv->perf.oa.oa_buffer.gtt_offset);
}

static int alloc_obj(struct drm_i915_private *dev_priv,
				struct drm_i915_gem_object **obj)
{
	struct drm_i915_gem_object *bo;
	int ret;

	intel_runtime_pm_get(dev_priv);

	ret = i915_mutex_lock_interruptible(dev_priv->dev);
	if (ret)
		goto out;

	bo = i915_gem_alloc_object(dev_priv->dev, OA_BUFFER_SIZE);
	if (bo == NULL) {
		DRM_ERROR("Failed to allocate OA buffer\n");
		ret = -ENOMEM;
		goto unlock;
	}
	ret = i915_gem_object_set_cache_level(bo, I915_CACHE_LLC);
	if (ret)
		goto err_unref;

	/* PreHSW required 512K alignment, HSW requires 16M */
	ret = i915_gem_obj_ggtt_pin(bo, SZ_16M, 0);
	if (ret)
		goto err_unref;

	*obj = bo;
	goto unlock;

err_unref:
	drm_gem_object_unreference(&bo->base);
unlock:
	mutex_unlock(&dev_priv->dev->struct_mutex);
out:
	intel_runtime_pm_put(dev_priv);
	return ret;
}

static int alloc_oa_buffer(struct drm_i915_private *dev_priv)
{
	struct drm_i915_gem_object *bo;
	int ret;

	BUG_ON(dev_priv->perf.oa.oa_buffer.obj);

	ret = alloc_obj(dev_priv, &bo);
	if (ret)
		return ret;

	dev_priv->perf.oa.oa_buffer.obj = bo;

	dev_priv->perf.oa.oa_buffer.gtt_offset = i915_gem_obj_ggtt_offset(bo);
	dev_priv->perf.oa.oa_buffer.addr = vmap_oa_buffer(bo);

	dev_priv->perf.oa.ops.init_oa_buffer(dev_priv);

	DRM_DEBUG_DRIVER("OA Buffer initialized, gtt offset = 0x%x, vaddr = %p",
			 dev_priv->perf.oa.oa_buffer.gtt_offset,
			 dev_priv->perf.oa.oa_buffer.addr);

	return 0;
}

static int alloc_command_stream_buf(struct drm_i915_private *dev_priv)
{
	struct drm_i915_gem_object *bo;
	int ret;

	BUG_ON(dev_priv->perf.command_stream_buf.obj);

	ret = alloc_obj(dev_priv, &bo);
	if (ret)
		return ret;

	dev_priv->perf.command_stream_buf.obj = bo;
	dev_priv->perf.command_stream_buf.vma = i915_gem_obj_to_ggtt(bo);
	dev_priv->perf.command_stream_buf.addr = vmap_oa_buffer(bo);
	INIT_LIST_HEAD(&dev_priv->perf.node_list);

	DRM_DEBUG_DRIVER(
		"command stream buf initialized, gtt offset = 0x%x, vaddr = %p",
		 (unsigned int)
		 dev_priv->perf.command_stream_buf.vma->node.start,
		 dev_priv->perf.command_stream_buf.addr);

	return 0;
}

static void config_oa_regs(struct drm_i915_private *dev_priv,
			   const struct i915_oa_reg *regs,
			   int n_regs)
{
	int i;

	for (i = 0; i < n_regs; i++) {
		const struct i915_oa_reg *reg = regs + i;

		I915_WRITE(reg->addr, reg->value);
	}
}

static int hsw_enable_metric_set(struct drm_i915_private *dev_priv)
{
	int ret = i915_oa_select_metric_set_hsw(dev_priv);

	if (ret)
		return ret;

	I915_WRITE(GDT_CHICKEN_BITS, GT_NOA_ENABLE);

	/* PRM:
	 *
	 * OA unit is using “crclk” for its functionality. When trunk
	 * level clock gating takes place, OA clock would be gated,
	 * unable to count the events from non-render clock domain.
	 * Render clock gating must be disabled when OA is enabled to
	 * count the events from non-render domain. Unit level clock
	 * gating for RCS should also be disabled.
	 */
	I915_WRITE(GEN7_MISCCPCTL, (I915_READ(GEN7_MISCCPCTL) &
				    ~GEN7_DOP_CLOCK_GATE_ENABLE));
	I915_WRITE(GEN6_UCGCTL1, (I915_READ(GEN6_UCGCTL1) |
				  GEN6_CSUNIT_CLOCK_GATE_DISABLE));

	config_oa_regs(dev_priv, dev_priv->perf.oa.mux_regs,
		       dev_priv->perf.oa.mux_regs_len);
	config_oa_regs(dev_priv, dev_priv->perf.oa.b_counter_regs,
		       dev_priv->perf.oa.b_counter_regs_len);

	return 0;
}

static void hsw_disable_metric_set(struct drm_i915_private *dev_priv)
{
	I915_WRITE(GEN6_UCGCTL1, (I915_READ(GEN6_UCGCTL1) &
				  ~GEN6_CSUNIT_CLOCK_GATE_DISABLE));
	I915_WRITE(GEN7_MISCCPCTL, (I915_READ(GEN7_MISCCPCTL) |
				    GEN7_DOP_CLOCK_GATE_ENABLE));

	I915_WRITE(GDT_CHICKEN_BITS, (I915_READ(GDT_CHICKEN_BITS) &
				      ~GT_NOA_ENABLE));
}

/* Manages updating the per-context aspects of the OA stream
 * configuration across all contexts.
 *
 * The awkward consideration here is that OACTXCONTROL controls the
 * exponent for periodic sampling which is primarily used for system
 * wide profiling where we'd like a consistent sampling period even in
 * the face of context switches.
 *
 * Our approach of updating the register state context (as opposed to
 * say using a workaround batch buffer) ensures that the hardware
 * won't automatically reload an out-of-date timer exponent even
 * transiently before a WA BB could be parsed.
 */
static int configure_all_contexts(struct drm_i915_private *dev_priv)
{
	struct drm_device *dev = dev_priv->dev;
	struct intel_context *ctx;
	struct intel_engine_cs *ring;
	int ring_id;
	int ret;

	ret = mutex_lock_interruptible(&dev->struct_mutex);
	if (ret)
		return ret;

	list_for_each_entry(ctx, &dev_priv->context_list, link) {

		for_each_ring(ring, dev_priv, ring_id) {
			/* The actual update of the register state context
			 * will happen the next time this logical ring
			 * is submitted. (See i915_oa_update_reg_state()
			 * which hooks into execlists_update_context())
			 */
			atomic_set(&ring->oa_state_dirty, true);
		}
	}

	mutex_unlock(&dev->struct_mutex);

	/* Now update the current context.
	 *
	 * Note: Using MMIO to update per-context registers requires
	 * some extra care...
	 */
	ret = intel_uncore_begin_ctx_mmio(dev_priv);
	if (ret) {
		DRM_ERROR("Failed to bring RCS out of idle to update current ctx OA state");
		return ret;
	}

	I915_WRITE(GEN8_OACTXCONTROL, ((dev_priv->perf.oa.period_exponent <<
					GEN8_OA_TIMER_PERIOD_SHIFT) |
				      (dev_priv->perf.oa.periodic ?
				       GEN8_OA_TIMER_ENABLE : 0) |
				      GEN8_OA_COUNTER_RESUME));

	config_oa_regs(dev_priv, dev_priv->perf.oa.flex_regs,
			dev_priv->perf.oa.flex_regs_len);

	intel_uncore_end_ctx_mmio(dev_priv);

	return 0;
}

static int bdw_enable_metric_set(struct drm_i915_private *dev_priv)
{
	int ret = i915_oa_select_metric_set_bdw(dev_priv);

	if (ret)
		return ret;

	I915_WRITE(GDT_CHICKEN_BITS, 0xA0);
	config_oa_regs(dev_priv, dev_priv->perf.oa.mux_regs,
		       dev_priv->perf.oa.mux_regs_len);
	I915_WRITE(GDT_CHICKEN_BITS, 0x80);
	config_oa_regs(dev_priv, dev_priv->perf.oa.b_counter_regs,
		       dev_priv->perf.oa.b_counter_regs_len);

	configure_all_contexts(dev_priv);

	return 0;
}

static void bdw_disable_metric_set(struct drm_i915_private *dev_priv)
{
	I915_WRITE(GEN6_UCGCTL1, (I915_READ(GEN6_UCGCTL1) &
				  ~GEN6_CSUNIT_CLOCK_GATE_DISABLE));
	I915_WRITE(GEN7_MISCCPCTL, (I915_READ(GEN7_MISCCPCTL) |
				    GEN7_DOP_CLOCK_GATE_ENABLE));
#warning "BDW: Do we need to write to CHICKEN2 to disable DOP clock gating when idle? (vpg does this)"
}

static int chv_enable_metric_set(struct drm_i915_private *dev_priv)
{
	int ret = i915_oa_select_metric_set_chv(dev_priv);

	if (ret)
		return ret;

	I915_WRITE(GDT_CHICKEN_BITS, 0xA0);
	config_oa_regs(dev_priv, dev_priv->perf.oa.mux_regs,
		       dev_priv->perf.oa.mux_regs_len);
	I915_WRITE(GDT_CHICKEN_BITS, 0x80);
	config_oa_regs(dev_priv, dev_priv->perf.oa.b_counter_regs,
		       dev_priv->perf.oa.b_counter_regs_len);

	configure_all_contexts(dev_priv);

	return 0;
}

static void chv_disable_metric_set(struct drm_i915_private *dev_priv)
{
	I915_WRITE(GEN6_UCGCTL1, (I915_READ(GEN6_UCGCTL1) &
				  ~GEN6_CSUNIT_CLOCK_GATE_DISABLE));
	I915_WRITE(GEN7_MISCCPCTL, (I915_READ(GEN7_MISCCPCTL) |
				    GEN7_DOP_CLOCK_GATE_ENABLE));
#warning "CHV: Do we need to write to CHICKEN2 to disable DOP clock gating when idle? (vpg does this)"
}

static int skl_enable_metric_set(struct drm_i915_private *dev_priv)
{
	int ret = i915_oa_select_metric_set_skl(dev_priv);

	if (ret)
		return ret;

	I915_WRITE(GDT_CHICKEN_BITS, 0xA0);
	config_oa_regs(dev_priv, dev_priv->perf.oa.mux_regs,
		       dev_priv->perf.oa.mux_regs_len);
	I915_WRITE(GDT_CHICKEN_BITS, 0x80);
	config_oa_regs(dev_priv, dev_priv->perf.oa.b_counter_regs,
		       dev_priv->perf.oa.b_counter_regs_len);

	configure_all_contexts(dev_priv);

	return 0;
}

static void skl_disable_metric_set(struct drm_i915_private *dev_priv)
{
	I915_WRITE(GEN6_UCGCTL1, (I915_READ(GEN6_UCGCTL1) &
				  ~GEN6_CSUNIT_CLOCK_GATE_DISABLE));
	I915_WRITE(GEN7_MISCCPCTL, (I915_READ(GEN7_MISCCPCTL) |
				    GEN7_DOP_CLOCK_GATE_ENABLE));
#warning "SKL: Do we need to write to CHICKEN2 to disable DOP clock gating when idle? (vpg does this)"
}

static void gen7_update_oacontrol_locked(struct drm_i915_private *dev_priv)
{
	assert_spin_locked(&dev_priv->perf.hook_lock);

	if (dev_priv->perf.oa.exclusive_stream->enabled) {
		unsigned long ctx_id = 0;
		bool pinning_ok = false;

		if (dev_priv->perf.oa.exclusive_stream->ctx &&
		    dev_priv->perf.oa.specific_ctx_id) {
			ctx_id = dev_priv->perf.oa.specific_ctx_id;
			pinning_ok = true;
		}

		if (dev_priv->perf.oa.exclusive_stream->ctx == NULL ||
		    pinning_ok) {
			bool periodic = dev_priv->perf.oa.periodic;
			u32 period_exponent = dev_priv->perf.oa.period_exponent;
			u32 report_format = dev_priv->perf.oa.oa_buffer.format;

			I915_WRITE(GEN7_OACONTROL,
				   (ctx_id & GEN7_OACONTROL_CTX_MASK) |
				   (period_exponent <<
				    GEN7_OACONTROL_TIMER_PERIOD_SHIFT) |
				   (periodic ?
				    GEN7_OACONTROL_TIMER_ENABLE : 0) |
				   (report_format <<
				    GEN7_OACONTROL_FORMAT_SHIFT) |
				   (ctx_id ?
				    GEN7_OACONTROL_PER_CTX_ENABLE : 0) |
				   GEN7_OACONTROL_ENABLE);
			return;
		}
	}

	I915_WRITE(GEN7_OACONTROL, 0);
}

static void gen7_oa_enable(struct drm_i915_private *dev_priv)
{
	unsigned long flags;
	u32 oastatus1, tail;

	spin_lock_irqsave(&dev_priv->perf.hook_lock, flags);
	gen7_update_oacontrol_locked(dev_priv);
	spin_unlock_irqrestore(&dev_priv->perf.hook_lock, flags);

	/* Reset the head ptr so we don't forward reports from before now. */
	oastatus1 = I915_READ(GEN7_OASTATUS1);
	tail = oastatus1 & GEN7_OASTATUS1_TAIL_MASK;
	I915_WRITE(GEN7_OASTATUS2, (tail & GEN7_OASTATUS2_HEAD_MASK) |
				    OA_MEM_SELECT_GGTT);
}

static void gen8_oa_enable(struct drm_i915_private *dev_priv)
{
	u32 report_format = dev_priv->perf.oa.oa_buffer.format;
	u32 tail;

	/* Note: we don't rely on the hardware to perform single context
	 * filtering and instead filter on the cpu based on the context-id
	 * field of reports */
	I915_WRITE(GEN8_OACONTROL, (report_format <<
				    GEN8_OA_REPORT_FORMAT_SHIFT) |
				   GEN8_OA_COUNTER_ENABLE);

	/* Reset the head ptr so we don't forward reports from before now. */
	tail = I915_READ(GEN8_OATAILPTR);
	I915_WRITE(GEN8_OAHEADPTR, tail);
}

static void i915_oa_stream_enable(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	dev_priv->perf.oa.ops.oa_enable(dev_priv);

	if (stream->cs_mode)
		stream->command_stream_hook = i915_perf_command_stream_hook_oa;

	if (dev_priv->perf.oa.periodic)
		hrtimer_start(&dev_priv->perf.oa.poll_check_timer,
			      ns_to_ktime(POLL_PERIOD),
			      HRTIMER_MODE_REL_PINNED);
}

static void gen7_oa_disable(struct drm_i915_private *dev_priv)
{
	I915_WRITE(GEN7_OACONTROL, 0);
}

static void gen8_oa_disable(struct drm_i915_private *dev_priv)
{
	I915_WRITE(GEN8_OACONTROL, 0);
}

static void i915_oa_stream_disable(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	if (dev_priv->perf.oa.periodic)
		hrtimer_cancel(&dev_priv->perf.oa.poll_check_timer);

	if (stream->cs_mode) {
		stream->command_stream_hook = NULL;
		i915_oa_rcs_wait_gpu(dev_priv);
		i915_oa_rcs_free_requests(dev_priv);
	}

	dev_priv->perf.oa.ops.oa_disable(dev_priv);
}

static int i915_oa_stream_init(struct i915_perf_stream *stream,
			       struct drm_i915_perf_open_param *param,
			       struct perf_open_properties *props)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	bool require_oa_unit = props->sample_flags & (SAMPLE_OA_REPORT |
						      SAMPLE_OA_SOURCE_INFO);
	bool require_cs_mode = props->sample_flags & (SAMPLE_PID |
						      SAMPLE_TAG);
	int format_size;
	int ret;

	/* To avoid the complexity of having to accurately filter
	 * counter reports and marshal to the appropriate client
	 * we currently only allow exclusive access */
	if (dev_priv->perf.oa.exclusive_stream) {
		DRM_ERROR("OA unit already in use\n");
		return -EBUSY;
	}

	/* Ctx Id can be sampled in HSW only through command streamer mode */
	if (IS_HASWELL(dev_priv->dev) &&
	    (props->sample_flags & SAMPLE_CTX_ID) && !props->cs_mode) {
		DRM_ERROR(
			"Context ID sampling only supported via command stream on Haswell");
		return -EINVAL;
	}

	stream->sample_size = sizeof(struct drm_i915_perf_record_header);

	if (require_oa_unit) {
		if (!dev_priv->perf.oa.ops.init_oa_buffer) {
			DRM_ERROR("OA unit not supported\n");
			return -ENODEV;
		}

		if (!props->metrics_set) {
			DRM_ERROR("OA metric set not specified\n");
			return -EINVAL;
		}

		if (props->cs_mode  && (props->ring_id != RCS)) {
			DRM_ERROR(
				"Command stream OA metrics only available via Render CS\n");
			return -EINVAL;
		}
		stream->ring_id = RCS;

		if (!props->oa_format) {
			DRM_ERROR("OA report format not specified\n");
			return -EINVAL;
		}

		format_size =
			dev_priv->perf.oa.oa_formats[props->oa_format].size;

		if (props->sample_flags & SAMPLE_OA_REPORT) {
			stream->sample_flags |= SAMPLE_OA_REPORT;
			stream->sample_size += format_size;
		}

		if (props->sample_flags & SAMPLE_OA_SOURCE_INFO) {
			stream->sample_flags |= SAMPLE_OA_SOURCE_INFO;
			stream->sample_size += 4;
		}

		dev_priv->perf.oa.oa_buffer.format_size = format_size;
		BUG_ON(dev_priv->perf.oa.oa_buffer.format_size == 0);

		dev_priv->perf.oa.oa_buffer.format =
			dev_priv->perf.oa.oa_formats[props->oa_format].format;

		dev_priv->perf.oa.metrics_set = props->metrics_set;

		dev_priv->perf.oa.periodic = props->oa_periodic;
		if (dev_priv->perf.oa.periodic)
			dev_priv->perf.oa.period_exponent =
					props->oa_period_exponent;

		if (i915.enable_execlists && stream->ctx)
			dev_priv->perf.oa.specific_ctx_id =
				intel_execlists_ctx_id(stream->ctx);

		ret = alloc_oa_buffer(dev_priv);
		if (ret)
			return ret;

		/* PRM - observability performance counters:
		 *
		 *   OACONTROL, performance counter enable, note:
		 *
		 *   "When this bit is set, in order to have coherent counts,
		 *   RC6 power state and trunk clock gating must be disabled.
		 *   This can be achieved by programming MMIO registers as
		 *   0xA094=0 and 0xA090[31]=1"
		 *
		 *   In our case we are expected that taking pm + FORCEWAKE
		 *   references will effectively disable RC6.
		 */
		intel_runtime_pm_get(dev_priv);
		intel_uncore_forcewake_get(dev_priv, FORCEWAKE_ALL);

		dev_priv->perf.oa.ops.enable_metric_set(dev_priv);
	}

	if (props->sample_flags & SAMPLE_CTX_ID) {
		stream->sample_flags |= SAMPLE_CTX_ID;
		stream->sample_size += 4;

		/*
		 * NB: it's meaningful to request SAMPLE_CTX_ID with just CS
		 * mode or periodic OA mode sampling but we don't allow
		 * SAMPLE_CTX_ID without either mode
		 */
		if (!require_oa_unit)
			require_cs_mode = true;
	}

	if (require_cs_mode && !props->cs_mode) {
		DRM_ERROR("PID or TAG sampling require a ring to be specified");
		ret = -EINVAL;
		goto cs_error;
	}

	if (props->cs_mode) {
		/*
		 * The only time we should allow enabling CS mode if it's not
		 * strictly required, is if SAMPLE_CTX_ID has been requested
		 * as it's usable with periodic OA or CS sampling.
		 */
		if (!require_cs_mode &&
		    !(props->sample_flags & SAMPLE_CTX_ID)) {
			DRM_ERROR(
				"Ring given without requesting any CS data to sample");
			ret = -EINVAL;
			goto cs_error;
		}

		stream->cs_mode = true;

		if (props->sample_flags & SAMPLE_PID) {
			stream->sample_flags |= SAMPLE_PID;
			stream->sample_size += 4;
		}

		if (props->sample_flags & SAMPLE_TAG) {
			stream->sample_flags |= SAMPLE_TAG;
			stream->sample_size += 4;
		}

		ret = alloc_command_stream_buf(dev_priv);
		if (ret)
			goto cs_error;
	}

	dev_priv->perf.oa.exclusive_stream = stream;

	stream->destroy = i915_oa_stream_destroy;
	stream->enable = i915_oa_stream_enable;
	stream->disable = i915_oa_stream_disable;
	stream->can_read = i915_oa_can_read;
	stream->wait_unlocked = i915_oa_wait_unlocked;
	stream->poll_wait = i915_oa_poll_wait;
	stream->read = i915_oa_read;

	return 0;

cs_error:
	if (require_oa_unit) {
		dev_priv->perf.oa.ops.disable_metric_set(dev_priv);

		intel_uncore_forcewake_put(dev_priv, FORCEWAKE_ALL);
		intel_runtime_pm_put(dev_priv);

		free_oa_buffer(dev_priv);
	}
	return ret;
}

static void gen7_update_hw_ctx_id_locked(struct drm_i915_private *dev_priv,
					 u32 ctx_id)
{
	assert_spin_locked(&dev_priv->perf.hook_lock);

	dev_priv->perf.oa.specific_ctx_id = ctx_id;
	gen7_update_oacontrol_locked(dev_priv);
}

static void i915_oa_context_pin_notify_locked(struct drm_i915_private *dev_priv,
					      struct intel_context *context)
{
	assert_spin_locked(&dev_priv->perf.hook_lock);

	if (i915.enable_execlists ||
	    dev_priv->perf.oa.ops.update_hw_ctx_id_locked == NULL)
		return;

	if (dev_priv->perf.oa.exclusive_stream &&
	    dev_priv->perf.oa.exclusive_stream->ctx == context) {
		struct drm_i915_gem_object *obj =
			context->legacy_hw_ctx.rcs_state;
		u32 ctx_id = i915_gem_obj_ggtt_offset(obj);

		dev_priv->perf.oa.ops.update_hw_ctx_id_locked(dev_priv, ctx_id);
	}
}

void i915_oa_context_pin_notify(struct drm_i915_private *dev_priv,
				struct intel_context *context)
{
	unsigned long flags;

	if (!dev_priv->perf.initialized)
		return;

	spin_lock_irqsave(&dev_priv->perf.hook_lock, flags);
	i915_oa_context_pin_notify_locked(dev_priv, context);
	spin_unlock_irqrestore(&dev_priv->perf.hook_lock, flags);
}

static void gen8_legacy_ctx_switch_unlocked(struct drm_i915_gem_request *req)
{
	struct drm_i915_private *dev_priv = req->i915;
	struct intel_engine_cs *ring = req->ring;
	const struct i915_oa_reg *flex_regs = dev_priv->perf.oa.flex_regs;
	int n_flex_regs = dev_priv->perf.oa.flex_regs_len;
	int ret;
	int i;

	if (!atomic_read(&ring->oa_state_dirty))
		return;

	ret = intel_ring_begin(req, n_flex_regs * 2 + 4);
	if (ret)
		return;

	intel_ring_emit(ring, MI_LOAD_REGISTER_IMM(n_flex_regs + 1));

	intel_ring_emit(ring, GEN8_OACTXCONTROL);
	intel_ring_emit(ring,
			(dev_priv->perf.oa.period_exponent <<
			 GEN8_OA_TIMER_PERIOD_SHIFT) |
			(dev_priv->perf.oa.periodic ?
			 GEN8_OA_TIMER_ENABLE : 0) |
			GEN8_OA_COUNTER_RESUME);

	for (i = 0; i < n_flex_regs; i++) {
		intel_ring_emit(ring, flex_regs[i].addr);
		intel_ring_emit(ring, flex_regs[i].value);
	}
	intel_ring_emit(ring, MI_NOOP);
	intel_ring_advance(ring);

	atomic_set(&ring->oa_state_dirty, false);
}

void i915_oa_legacy_ctx_switch_notify(struct drm_i915_gem_request *req)
{
	struct drm_i915_private *dev_priv = req->i915;

	if (!dev_priv->perf.initialized)
		return;

	if (dev_priv->perf.oa.ops.legacy_ctx_switch_unlocked == NULL)
		return;

	if (dev_priv->perf.oa.exclusive_stream &&
	    dev_priv->perf.oa.exclusive_stream->enabled) {

		/* XXX: We don't take a lock here and this may run
		 * async with respect to stream methods. Notably we
		 * don't want to block context switches by long i915
		 * perf read() operations.
		 *
		 * It's expect to always be safe to read the
		 * dev_priv->perf state needed here, and expected to
		 * be benign to redundantly update the state if the OA
		 * unit has been disabled since oa_state_dirty was
		 * last set.
		 */
		dev_priv->perf.oa.ops.legacy_ctx_switch_unlocked(req);
	}
}

static void gen8_update_reg_state_unlocked(struct intel_engine_cs *ring,
					   uint32_t *reg_state)
{
	struct drm_i915_private *dev_priv = ring->dev->dev_private;
	const struct i915_oa_reg *flex_regs = dev_priv->perf.oa.flex_regs;
	int n_flex_regs = dev_priv->perf.oa.flex_regs_len;
	int ctx_oactxctrl = dev_priv->perf.oa.ctx_oactxctrl_off;
	int ctx_flexeu0 = dev_priv->perf.oa.ctx_flexeu0_off;
	int i;

	if (!atomic_read(&ring->oa_state_dirty))
		return;

	reg_state[ctx_oactxctrl] = GEN8_OACTXCONTROL;
	reg_state[ctx_oactxctrl+1] = (dev_priv->perf.oa.period_exponent <<
				      GEN8_OA_TIMER_PERIOD_SHIFT) |
				     (dev_priv->perf.oa.periodic ?
				      GEN8_OA_TIMER_ENABLE : 0) |
				     GEN8_OA_COUNTER_RESUME;

	for (i = 0; i < n_flex_regs; i++) {
		uint32_t offset = flex_regs[i].addr;

		/* Map from mmio address to register state context
		 * offset... */

		offset -= EU_PERF_CNTL0;

		offset >>= 5; /* Flex EU mmio registers are separated by 256
			       * bytes, here they are separated by 8 bytes */

		/* EU_PERF_CNTL0 offset in register state context... */
		offset += ctx_flexeu0;

		reg_state[offset] = flex_regs[i].addr;
		reg_state[offset+1] = flex_regs[i].value;
	}

	atomic_set(&ring->oa_state_dirty, false);
}

void i915_oa_update_reg_state(struct intel_engine_cs *ring, uint32_t *reg_state)
{
	struct drm_i915_private *dev_priv = ring->dev->dev_private;

	if (!dev_priv->perf.initialized)
		return;

	/* XXX: We don't take a lock here and this may run async with
	 * respect to stream methods. Notably we don't want to block
	 * context switches by long i915 perf read() operations.
	 *
	 * It's expect to always be safe to read the dev_priv->perf
	 * state needed here, and expected to be benign to redundantly
	 * update the state if the OA unit has been disabled since
	 * oa_state_dirty was last set.
	 */

	gen8_update_reg_state_unlocked(ring, reg_state);
}

static ssize_t i915_perf_read_locked(struct i915_perf_stream *stream,
				     struct file *file,
				     char __user *buf,
				     size_t count,
				     loff_t *ppos)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	struct i915_perf_read_state state = { count, 0, buf };
	int ret;

	if (file->f_flags & O_NONBLOCK) {
		if (!stream->can_read(stream))
			return -EAGAIN;
	} else {
		mutex_unlock(&dev_priv->perf.lock);
		ret = stream->wait_unlocked(stream);
		mutex_lock(&dev_priv->perf.lock);

		if (ret)
			return ret;
	}

	stream->read(stream, &state);
	if (state.read == 0)
		return -ENOSPC;

	return state.read;
}

static ssize_t i915_perf_read(struct file *file,
			      char __user *buf,
			      size_t count,
			      loff_t *ppos)
{
	struct i915_perf_stream *stream = file->private_data;
	struct drm_i915_private *dev_priv = stream->dev_priv;
	ssize_t ret;

	mutex_lock(&dev_priv->perf.lock);
	ret = i915_perf_read_locked(stream, file, buf, count, ppos);
	mutex_unlock(&dev_priv->perf.lock);

	return ret;
}

static enum hrtimer_restart poll_check_timer_cb(struct hrtimer *hrtimer)
{
	struct i915_perf_stream *stream;

	struct drm_i915_private *dev_priv =
		container_of(hrtimer, typeof(*dev_priv),
			     perf.oa.poll_check_timer);

	/* No need to protect the streams list here, since the hrtimer is
	 * disabled before the stream is removed from list, and currently a
	 * single exclusive_stream is supported.
	 * XXX: revisit this when multiple concurrent streams are supported.
	 */
	list_for_each_entry(stream, &dev_priv->perf.streams, link) {
		if (stream_have_data__unlocked(stream))
			wake_up(&dev_priv->perf.oa.poll_wq);
	}

	hrtimer_forward_now(hrtimer, ns_to_ktime(POLL_PERIOD));

	return HRTIMER_RESTART;
}

static unsigned int i915_perf_poll_locked(struct i915_perf_stream *stream,
					  struct file *file,
					  poll_table *wait)
{
	unsigned int streams = 0;

	stream->poll_wait(stream, file, wait);

	if (stream->can_read(stream))
		streams |= POLLIN;

	return streams;
}

static unsigned int i915_perf_poll(struct file *file, poll_table *wait)
{
	struct i915_perf_stream *stream = file->private_data;
	struct drm_i915_private *dev_priv = stream->dev_priv;
	int ret;

	mutex_lock(&dev_priv->perf.lock);
	ret = i915_perf_poll_locked(stream, file, wait);
	mutex_unlock(&dev_priv->perf.lock);

	return ret;
}

static void i915_perf_enable_locked(struct i915_perf_stream *stream)
{
	if (stream->enabled)
		return;

	/* Allow stream->enable() to refer to this */
	stream->enabled = true;

	if (stream->enable)
		stream->enable(stream);
}

static void i915_perf_disable_locked(struct i915_perf_stream *stream)
{
	if (!stream->enabled)
		return;

	/* Allow stream->disable() to refer to this */
	stream->enabled = false;

	if (stream->disable)
		stream->disable(stream);
}

static long i915_perf_ioctl_locked(struct i915_perf_stream *stream,
				   unsigned int cmd,
				   unsigned long arg)
{
	switch (cmd) {
	case I915_PERF_IOCTL_ENABLE:
		i915_perf_enable_locked(stream);
		return 0;
	case I915_PERF_IOCTL_DISABLE:
		i915_perf_disable_locked(stream);
		return 0;
	}

	return -EINVAL;
}

static long i915_perf_ioctl(struct file *file,
			    unsigned int cmd,
			    unsigned long arg)
{
	struct i915_perf_stream *stream = file->private_data;
	struct drm_i915_private *dev_priv = stream->dev_priv;
	long ret;

	mutex_lock(&dev_priv->perf.lock);
	ret = i915_perf_ioctl_locked(stream, cmd, arg);
	mutex_unlock(&dev_priv->perf.lock);

	return ret;
}

static void i915_perf_destroy_locked(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	mutex_lock(&dev_priv->perf.streams_lock);
	list_del(&stream->link);
	mutex_unlock(&dev_priv->perf.streams_lock);

	if (stream->enabled)
		i915_perf_disable_locked(stream);

	if (stream->destroy)
		stream->destroy(stream);

	if (stream->ctx) {
		mutex_lock(&dev_priv->dev->struct_mutex);
		i915_gem_context_unreference(stream->ctx);
		mutex_unlock(&dev_priv->dev->struct_mutex);
	}

	kfree(stream);
}

static int i915_perf_release(struct inode *inode, struct file *file)
{
	struct i915_perf_stream *stream = file->private_data;
	struct drm_i915_private *dev_priv = stream->dev_priv;

	mutex_lock(&dev_priv->perf.lock);
	i915_perf_destroy_locked(stream);
	mutex_unlock(&dev_priv->perf.lock);

	return 0;
}


static const struct file_operations fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
	.release	= i915_perf_release,
	.poll		= i915_perf_poll,
	.read		= i915_perf_read,
	.unlocked_ioctl	= i915_perf_ioctl,
};

static struct intel_context *
lookup_context(struct drm_i915_private *dev_priv,
	       struct file *user_filp,
	       u32 ctx_user_handle)
{
	struct intel_context *ctx;

	mutex_lock(&dev_priv->dev->struct_mutex);
	list_for_each_entry(ctx, &dev_priv->context_list, link) {
		struct drm_file *drm_file;

		if (!ctx->file_priv)
			continue;

		drm_file = ctx->file_priv->file;

		if (user_filp->private_data == drm_file &&
		    ctx->user_handle == ctx_user_handle) {
			i915_gem_context_reference(ctx);
			mutex_unlock(&dev_priv->dev->struct_mutex);

			return ctx;
		}
	}
	mutex_unlock(&dev_priv->dev->struct_mutex);

	return NULL;
}

int i915_perf_open_ioctl_locked(struct drm_device *dev,
				struct drm_i915_perf_open_param *param,
				struct perf_open_properties *props,
				struct drm_file *file)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_context *specific_ctx = NULL;
	struct i915_perf_stream *stream = NULL;
	unsigned long f_flags = 0;
	int stream_fd;
	int ret = 0;

	if (props->single_context) {
		u32 ctx_handle = props->ctx_handle;

		specific_ctx = lookup_context(dev_priv, file->filp, ctx_handle);
		if (!specific_ctx) {
			DRM_ERROR("Failed to look up context with ID %u for opening perf stream\n",
				  ctx_handle);
			ret = -EINVAL;
			goto err;
		}
	}

	/* Similar to perf's kernel.perf_paranoid_cpu sysctl option
	 * we check a dev.i915.perf_stream_paranoid sysctl option
	 * to determine if it's ok to access system wide OA counters
	 * without CAP_SYS_ADMIN privileges.
	 */
	if (!specific_ctx &&
	    i915_perf_stream_paranoid && !capable(CAP_SYS_ADMIN)) {
		DRM_ERROR("Insufficient privileges to open system-wide i915 perf stream\n");
		ret = -EACCES;
		goto err_ctx;
	}

	stream = kzalloc(sizeof(*stream), GFP_KERNEL);
	if (!stream) {
		ret = -ENOMEM;
		goto err_ctx;
	}

	stream->dev_priv = dev_priv;
	stream->ctx = specific_ctx;

	ret = i915_oa_stream_init(stream, param, props);
	if (ret)
		goto err_alloc;

	/* we avoid simply assigning stream->sample_flags = props->sample_flags
	 * to have _stream_init check the combination of sample flags more
	 * thoroughly, but still this is the expected result at this point. */
	BUG_ON(stream->sample_flags != props->sample_flags);

	mutex_lock(&dev_priv->perf.streams_lock);
	list_add(&stream->link, &dev_priv->perf.streams);
	mutex_unlock(&dev_priv->perf.streams_lock);

	if (param->flags & I915_PERF_FLAG_FD_CLOEXEC)
		f_flags |= O_CLOEXEC;
	if (param->flags & I915_PERF_FLAG_FD_NONBLOCK)
		f_flags |= O_NONBLOCK;

	stream_fd = anon_inode_getfd("[i915_perf]", &fops, stream, f_flags);
	if (stream_fd < 0) {
		ret = stream_fd;
		goto err_open;
	}

	if (!(param->flags & I915_PERF_FLAG_DISABLED))
		i915_perf_enable_locked(stream);

	return stream_fd;

err_open:
	list_del(&stream->link);
	if (stream->destroy)
		stream->destroy(stream);
err_alloc:
	kfree(stream);
err_ctx:
	if (specific_ctx) {
		mutex_lock(&dev_priv->dev->struct_mutex);
		i915_gem_context_unreference(specific_ctx);
		mutex_unlock(&dev_priv->dev->struct_mutex);
	}
err:
	return ret;
}

/* Note we copy the properties from userspace outside of the i915 perf
 * mutex to avoid an awkward lockdep with mmap_sem.
 *
 * Note this function only validates properties in isolation it doesn't
 * validate that the combination of properties makes sense or that all
 * properties necessary for a particular kind of stream have be set.
 */
static int read_properties_unlocked(struct drm_i915_private *dev_priv,
				    u64 __user *uprops,
				    u32 n_props,
				    struct perf_open_properties *props)
{
	u64 __user *uprop = uprops;
	int i;

	memset(props, 0, sizeof(struct perf_open_properties));

	if (!n_props) {
		DRM_ERROR("No i915 perf properties given");
		return -EINVAL;
	}

	if (n_props > DRM_I915_PERF_PROP_MAX) {
		DRM_ERROR("More i915 perf properties specified than exist");
		return -EINVAL;
	}

	for (i = 0; i < n_props; i++) {
		u64 id, value;
		int ret;

		ret = get_user(id, (u64 __user *)uprop);
		if (ret)
			return ret;

		if (id == 0 || id >= DRM_I915_PERF_PROP_MAX) {
			DRM_ERROR("Unknown i915 perf property ID");
			return -EINVAL;
		}

		ret = get_user(value, (u64 __user *)uprop + 1);
		if (ret)
			return ret;

		switch ((enum drm_i915_perf_property_id)id) {
		case DRM_I915_PERF_CTX_HANDLE_PROP:
			props->single_context = 1;
			props->ctx_handle = value;
			break;
		case DRM_I915_PERF_SAMPLE_OA_PROP:
			props->sample_flags |= SAMPLE_OA_REPORT;
			break;
		case DRM_I915_PERF_OA_METRICS_SET_PROP:
			if (value == 0 || value > dev_priv->perf.oa.n_builtin_sets) {
				DRM_ERROR("Unknown OA metric set ID");
				return -EINVAL;
			}
			props->metrics_set = value;
			break;
		case DRM_I915_PERF_OA_FORMAT_PROP:
			if (value == 0 || value >= I915_OA_FORMAT_MAX) {
				DRM_ERROR("Invalid OA report format\n");
				return -EINVAL;
			}
			if (!dev_priv->perf.oa.oa_formats[value].size) {
				DRM_ERROR("Invalid OA report format\n");
				return -EINVAL;
			}
			props->oa_format = value;
			break;
		case DRM_I915_PERF_OA_EXPONENT_PROP:
			if (value > OA_EXPONENT_MAX)
				return -EINVAL;

			/* Theoretically we can program the OA unit to sample
			 * every 160ns but don't allow that by default unless
			 * root.
			 */
			if (value < i915_oa_min_timer_exponent &&
			    !capable(CAP_SYS_ADMIN)) {
				DRM_ERROR("Sampling period too high without root privileges\n");
				return -EACCES;
			}

			props->oa_periodic = true;
			props->oa_period_exponent = value;
			break;
		case DRM_I915_PERF_SAMPLE_OA_SOURCE_PROP:
			props->sample_flags |= SAMPLE_OA_SOURCE_INFO;
			break;
		case DRM_I915_PERF_RING_PROP: {
				u8 ring_id =
					(value & I915_EXEC_RING_MASK) - 1;
				if (ring_id >= LAST_USER_RING)
					return -EINVAL;

				/* XXX: Currently only RCS is supported.
				 * Remove this check when support for other
				 * rings is added
				 */
				if (ring_id != RCS)
					return -EINVAL;

				props->cs_mode = true;
				props->ring_id = ring_id;
			}
			break;
		case DRM_I915_PERF_SAMPLE_CTX_ID_PROP:
			props->sample_flags |= SAMPLE_CTX_ID;
			break;
		case DRM_I915_PERF_SAMPLE_PID_PROP:
			props->sample_flags |= SAMPLE_PID;
			break;
		case DRM_I915_PERF_SAMPLE_TAG_PROP:
			props->sample_flags |= SAMPLE_TAG;
			break;
		case DRM_I915_PERF_PROP_MAX:
			BUG();
		}

		uprop += 2;
	}

	return 0;
}

int i915_perf_open_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_perf_open_param *param = data;
	struct perf_open_properties props;
	u32 known_open_flags = 0;
	int ret;

	known_open_flags = I915_PERF_FLAG_FD_CLOEXEC |
			   I915_PERF_FLAG_FD_NONBLOCK |
			   I915_PERF_FLAG_DISABLED;
	if (param->flags & ~known_open_flags) {
		DRM_ERROR("Unknown drm_i915_perf_open_param flag\n");
		return -EINVAL;
	}

	ret = read_properties_unlocked(dev_priv,
				       to_user_ptr(param->properties),
				       param->n_properties,
				       &props);
	if (ret)
		return ret;

	mutex_lock(&dev_priv->perf.lock);
	ret = i915_perf_open_ioctl_locked(dev, param, &props, file);
	mutex_unlock(&dev_priv->perf.lock);

	return ret;
}


static struct ctl_table oa_table[] = {
	{
	 .procname = "perf_stream_paranoid",
	 .data = &i915_perf_stream_paranoid,
	 .maxlen = sizeof(i915_perf_stream_paranoid),
	 .mode = 0644,
	 .proc_handler = proc_dointvec,
	 },
	{
	 .procname = "oa_min_timer_exponent",
	 .data = &i915_oa_min_timer_exponent,
	 .maxlen = sizeof(i915_oa_min_timer_exponent),
	 .mode = 0644,
	 .proc_handler = proc_dointvec_minmax,
	 .extra1 = &zero,
	 .extra2 = &oa_exponent_max,
	 },
	{}
};

static struct ctl_table i915_root[] = {
	{
	 .procname = "i915",
	 .maxlen = 0,
	 .mode = 0555,
	 .child = oa_table,
	 },
	{}
};

static struct ctl_table dev_root[] = {
	{
	 .procname = "dev",
	 .maxlen = 0,
	 .mode = 0555,
	 .child = i915_root,
	 },
	{}
};

void i915_perf_init(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);

	if (!(IS_HASWELL(dev) ||
	      IS_BROADWELL(dev) || IS_CHERRYVIEW(dev) ||
	      IS_SKYLAKE(dev)))
		return;

	dev_priv->perf.metrics_kobj =
		kobject_create_and_add("metrics", &dev->primary->kdev->kobj);
	if (!dev_priv->perf.metrics_kobj)
		return;

	hrtimer_init(&dev_priv->perf.oa.poll_check_timer,
		     CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	dev_priv->perf.oa.poll_check_timer.function = poll_check_timer_cb;
	init_waitqueue_head(&dev_priv->perf.oa.poll_wq);

	INIT_LIST_HEAD(&dev_priv->perf.streams);
	mutex_init(&dev_priv->perf.lock);
	mutex_init(&dev_priv->perf.streams_lock);
	spin_lock_init(&dev_priv->perf.hook_lock);
	spin_lock_init(&dev_priv->perf.node_list_lock);

	if (IS_HASWELL(dev)) {
		dev_priv->perf.oa.ops.init_oa_buffer = gen7_init_oa_buffer;
		dev_priv->perf.oa.ops.enable_metric_set = hsw_enable_metric_set;
		dev_priv->perf.oa.ops.disable_metric_set = hsw_disable_metric_set;
		dev_priv->perf.oa.ops.oa_enable = gen7_oa_enable;
		dev_priv->perf.oa.ops.oa_disable = gen7_oa_disable;
		dev_priv->perf.oa.ops.update_hw_ctx_id_locked = gen7_update_hw_ctx_id_locked;
		dev_priv->perf.oa.ops.read = gen7_oa_read;
		dev_priv->perf.oa.ops.oa_buffer_is_empty = gen7_oa_buffer_is_empty;

		dev_priv->perf.oa.oa_formats = hsw_oa_formats;

		dev_priv->perf.oa.n_builtin_sets =
			i915_oa_n_builtin_metric_sets_hsw;

		if (i915_perf_init_sysfs_hsw(dev_priv))
			goto sysfs_error;
	} else {
		dev_priv->perf.oa.ops.init_oa_buffer = gen8_init_oa_buffer;
		dev_priv->perf.oa.ops.oa_enable = gen8_oa_enable;
		dev_priv->perf.oa.ops.oa_disable = gen8_oa_disable;
		dev_priv->perf.oa.ops.read = gen8_oa_read;
		dev_priv->perf.oa.ops.oa_buffer_is_empty = gen8_oa_buffer_is_empty;

		dev_priv->perf.oa.oa_formats = gen8_plus_oa_formats;

		if (!i915.enable_execlists) {
			dev_priv->perf.oa.ops.legacy_ctx_switch_unlocked =
				gen8_legacy_ctx_switch_unlocked;
		}

		if (IS_BROADWELL(dev)) {
			dev_priv->perf.oa.ops.enable_metric_set =
				bdw_enable_metric_set;
			dev_priv->perf.oa.ops.disable_metric_set =
				bdw_disable_metric_set;
			dev_priv->perf.oa.ctx_oactxctrl_off = 0x120;
			dev_priv->perf.oa.ctx_flexeu0_off = 0x2ce;
			dev_priv->perf.oa.n_builtin_sets =
				i915_oa_n_builtin_metric_sets_bdw;

			if (i915_perf_init_sysfs_bdw(dev_priv))
				goto sysfs_error;
		} else if (IS_CHERRYVIEW(dev)) {
			dev_priv->perf.oa.ops.enable_metric_set =
				chv_enable_metric_set;
			dev_priv->perf.oa.ops.disable_metric_set =
				chv_disable_metric_set;
			dev_priv->perf.oa.ctx_oactxctrl_off = 0x120;
			dev_priv->perf.oa.ctx_flexeu0_off = 0x2ce;
			dev_priv->perf.oa.n_builtin_sets =
				i915_oa_n_builtin_metric_sets_chv;

			if (i915_perf_init_sysfs_chv(dev_priv))
				goto sysfs_error;
		} else if (IS_SKYLAKE(dev)) {
			dev_priv->perf.oa.ops.enable_metric_set =
				skl_enable_metric_set;
			dev_priv->perf.oa.ops.disable_metric_set =
				skl_disable_metric_set;
			dev_priv->perf.oa.ctx_oactxctrl_off = 0x128;
			dev_priv->perf.oa.ctx_flexeu0_off = 0x3de;
			dev_priv->perf.oa.n_builtin_sets =
				i915_oa_n_builtin_metric_sets_skl;

			if (i915_perf_init_sysfs_skl(dev_priv))
				goto sysfs_error;
		}
	}

	dev_priv->perf.sysctl_header = register_sysctl_table(dev_root);

	dev_priv->perf.initialized = true;

	return;

sysfs_error:
	kobject_put(dev_priv->perf.metrics_kobj);
	dev_priv->perf.metrics_kobj = NULL;

	return;
}

void i915_perf_fini(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);

	if (!dev_priv->perf.initialized)
		return;

	unregister_sysctl_table(dev_priv->perf.sysctl_header);

        if (IS_HASWELL(dev))
                i915_perf_deinit_sysfs_hsw(dev_priv);
        else if (IS_BROADWELL(dev))
                i915_perf_deinit_sysfs_bdw(dev_priv);
        else if (IS_CHERRYVIEW(dev))
                i915_perf_deinit_sysfs_chv(dev_priv);
        else if (IS_SKYLAKE(dev))
                i915_perf_deinit_sysfs_skl(dev_priv);

	kobject_put(dev_priv->perf.metrics_kobj);
	dev_priv->perf.metrics_kobj = NULL;

	dev_priv->perf.oa.ops.init_oa_buffer = NULL;

	dev_priv->perf.initialized = false;
}
