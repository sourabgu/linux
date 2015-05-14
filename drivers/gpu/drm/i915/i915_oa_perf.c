#include <linux/perf_event.h>
#include <linux/sizes.h>

#include "i915_drv.h"
#include "intel_ringbuffer.h"

/* Must be a power of two */
#define OA_BUFFER_SIZE	     SZ_16M
#define OA_TAKEN(tail, head) ((tail - head) & (OA_BUFFER_SIZE - 1))

#define FREQUENCY 200
#define PERIOD max_t(u64, 10000, NSEC_PER_SEC / FREQUENCY)

static u32 i915_oa_event_paranoid = true;

static int hsw_perf_format_sizes[] = {
	64,  /* A13_HSW */
	128, /* A29_HSW */
	128, /* A13_B8_C8_HSW */
	-1,  /* Disallowed since 192 bytes doesn't factor into buffer size
		(A29_B8_C8_HSW) */
	64,  /* B4_C8_HSW */
	256, /* A45_B8_C8_HSW */
	128, /* B4_C8_A16_HSW */
	64   /* C4_B8_HSW */
};


/* A generated mux config to select counters useful for profiling 3D
 * workloads */
static struct i915_oa_reg hsw_profile_3d_mux_config[] = {

	{ 0x253A4, 0x01600000 },
	{ 0x25440, 0x00100000 },
	{ 0x25128, 0x00000000 },
	{ 0x2691C, 0x00000800 },
	{ 0x26AA0, 0x01500000 },
	{ 0x26B9C, 0x00006000 },
	{ 0x2791C, 0x00000800 },
	{ 0x27AA0, 0x01500000 },
	{ 0x27B9C, 0x00006000 },
	{ 0x2641C, 0x00000400 },
	{ 0x25380, 0x00000010 },
	{ 0x2538C, 0x00000000 },
	{ 0x25384, 0x0800AAAA },
	{ 0x25400, 0x00000004 },
	{ 0x2540C, 0x06029000 },
	{ 0x25410, 0x00000002 },
	{ 0x25404, 0x5C30FFFF },
	{ 0x25100, 0x00000016 },
	{ 0x25110, 0x00000400 },
	{ 0x25104, 0x00000000 },
	{ 0x26804, 0x00001211 },
	{ 0x26884, 0x00000100 },
	{ 0x26900, 0x00000002 },
	{ 0x26908, 0x00700000 },
	{ 0x26904, 0x00000000 },
	{ 0x26984, 0x00001022 },
	{ 0x26A04, 0x00000011 },
	{ 0x26A80, 0x00000006 },
	{ 0x26A88, 0x00000C02 },
	{ 0x26A84, 0x00000000 },
	{ 0x26B04, 0x00001000 },
	{ 0x26B80, 0x00000002 },
	{ 0x26B8C, 0x00000007 },
	{ 0x26B84, 0x00000000 },
	{ 0x27804, 0x00004844 },
	{ 0x27884, 0x00000400 },
	{ 0x27900, 0x00000002 },
	{ 0x27908, 0x0E000000 },
	{ 0x27904, 0x00000000 },
	{ 0x27984, 0x00004088 },
	{ 0x27A04, 0x00000044 },
	{ 0x27A80, 0x00000006 },
	{ 0x27A88, 0x00018040 },
	{ 0x27A84, 0x00000000 },
	{ 0x27B04, 0x00004000 },
	{ 0x27B80, 0x00000002 },
	{ 0x27B8C, 0x000000E0 },
	{ 0x27B84, 0x00000000 },
	{ 0x26104, 0x00002222 },
	{ 0x26184, 0x0C006666 },
	{ 0x26284, 0x04000000 },
	{ 0x26304, 0x04000000 },
	{ 0x26400, 0x00000002 },
	{ 0x26410, 0x000000A0 },
	{ 0x26404, 0x00000000 },
	{ 0x25420, 0x04108020 },
	{ 0x25424, 0x1284A420 },
	{ 0x2541C, 0x00000000 },
	{ 0x25428, 0x00042049 },
};

/* A corresponding B counter configuration for profiling 3D workloads */
static struct i915_oa_reg hsw_profile_3d_b_counter_config[] = {
	{ 0x2724, 0x00800000 },
	{ 0x2720, 0x00000000 },
	{ 0x2714, 0x00800000 },
	{ 0x2710, 0x00000000 },
};

static void forward_one_oa_snapshot_to_event(struct drm_i915_private *dev_priv,
					     u8 *snapshot,
					     struct perf_event *event)
{
	struct perf_sample_data data;
	int snapshot_size = dev_priv->oa_pmu.oa_buffer.format_size;
	struct perf_raw_record raw;

	WARN_ON(snapshot_size == 0);

	perf_sample_data_init(&data, 0, event->hw.last_period);

	/* Note: the combined u32 raw->size member + raw data itself must be 8
	 * byte aligned. (See note in init_oa_buffer for more details) */
	raw.size = snapshot_size + 4;
	raw.data = snapshot;

	data.raw = &raw;

	perf_event_overflow(event, &data, &dev_priv->oa_pmu.dummy_regs);
}

static u32 forward_oa_snapshots(struct drm_i915_private *dev_priv,
				u32 head,
				u32 tail)
{
	struct perf_event *exclusive_event = dev_priv->oa_pmu.exclusive_event;
	int snapshot_size = dev_priv->oa_pmu.oa_buffer.format_size;
	u8 *oa_buf_base = dev_priv->oa_pmu.oa_buffer.addr;
	u32 mask = (OA_BUFFER_SIZE - 1);
	u8 *snapshot;
	u32 taken;

	head -= dev_priv->oa_pmu.oa_buffer.gtt_offset;
	tail -= dev_priv->oa_pmu.oa_buffer.gtt_offset;

	/* Note: the gpu doesn't wrap the tail according to the OA buffer size
	 * so when we need to make sure our head/tail values are in-bounds we
	 * use the above mask.
	 */

	while ((taken = OA_TAKEN(tail, head))) {
		/* The tail increases in 64 byte increments, not in
		 * format_size steps. */
		if (taken < snapshot_size)
			break;

		snapshot = oa_buf_base + (head & mask);
		head += snapshot_size;

		/* We currently only allow exclusive access to the counters
		 * so only have one event to forward too... */
		if (dev_priv->oa_pmu.event_active)
			forward_one_oa_snapshot_to_event(dev_priv, snapshot,
							 exclusive_event);
	}

	return dev_priv->oa_pmu.oa_buffer.gtt_offset + head;
}

static void log_oa_status(struct drm_i915_private *dev_priv,
			  enum drm_i915_oa_event_type status)
{
	struct {
		struct perf_event_header header;
		drm_i915_oa_event_header_t i915_oa_header;
	} oa_event;
	struct perf_output_handle handle;
	struct perf_sample_data sample_data;
	struct perf_event *event = dev_priv->oa_pmu.exclusive_event;
	int ret;

	oa_event.header.size = sizeof(oa_event);
	oa_event.header.type = PERF_RECORD_DEVICE;
	oa_event.i915_oa_header.type = status;
	oa_event.i915_oa_header.__reserved_1 = 0;

	perf_event_header__init_id(&oa_event.header, &sample_data, event);

	ret = perf_output_begin(&handle, event, oa_event.header.size);
	if (ret)
		return;

	perf_output_put(&handle, oa_event);
	perf_event__output_id_sample(event, &handle, &sample_data);
	perf_output_end(&handle);
}

static void flush_oa_snapshots(struct drm_i915_private *dev_priv,
			       bool skip_if_flushing)
{
	unsigned long flags;
	u32 oastatus2;
	u32 oastatus1;
	u32 head;
	u32 tail;

	/* Can either flush via hrtimer callback or pmu methods/fops */
	if (skip_if_flushing) {

		/* If the hrtimer triggers at the same time that we are
		 * responding to a userspace initiated flush then we can
		 * just bail out...
		 */
		if (!spin_trylock_irqsave(&dev_priv->oa_pmu.oa_buffer.flush_lock,
					  flags))
			return;
	} else
		spin_lock_irqsave(&dev_priv->oa_pmu.oa_buffer.flush_lock, flags);

	WARN_ON(!dev_priv->oa_pmu.oa_buffer.addr);

	oastatus2 = I915_READ(GEN7_OASTATUS2);
	oastatus1 = I915_READ(GEN7_OASTATUS1);

	head = oastatus2 & GEN7_OASTATUS2_HEAD_MASK;
	tail = oastatus1 & GEN7_OASTATUS1_TAIL_MASK;

	if (unlikely(oastatus1 & (GEN7_OASTATUS1_OABUFFER_OVERFLOW |
				  GEN7_OASTATUS1_REPORT_LOST))) {

		if (oastatus1 & GEN7_OASTATUS1_OABUFFER_OVERFLOW)
			log_oa_status(dev_priv, I915_OA_RECORD_BUFFER_OVERFLOW);

		if (oastatus1 & GEN7_OASTATUS1_REPORT_LOST)
			log_oa_status(dev_priv, I915_OA_RECORD_REPORT_LOST);

		I915_WRITE(GEN7_OASTATUS1, oastatus1 &
			   ~(GEN7_OASTATUS1_OABUFFER_OVERFLOW |
			     GEN7_OASTATUS1_REPORT_LOST));
	}

	head = forward_oa_snapshots(dev_priv, head, tail);

	I915_WRITE(GEN7_OASTATUS2, (head & GEN7_OASTATUS2_HEAD_MASK) |
				    GEN7_OASTATUS2_GGTT);

	spin_unlock_irqrestore(&dev_priv->oa_pmu.oa_buffer.flush_lock, flags);
}

static void
oa_buffer_destroy(struct drm_i915_private *i915)
{
	mutex_lock(&i915->dev->struct_mutex);

	vunmap(i915->oa_pmu.oa_buffer.addr);
	i915_gem_object_ggtt_unpin(i915->oa_pmu.oa_buffer.obj);
	drm_gem_object_unreference(&i915->oa_pmu.oa_buffer.obj->base);

	i915->oa_pmu.oa_buffer.obj = NULL;
	i915->oa_pmu.oa_buffer.gtt_offset = 0;
	i915->oa_pmu.oa_buffer.addr = NULL;

	mutex_unlock(&i915->dev->struct_mutex);
}

static void i915_oa_event_destroy(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), oa_pmu.pmu);

	WARN_ON(event->parent);

	oa_buffer_destroy(i915);

	i915->oa_pmu.specific_ctx = NULL;

	BUG_ON(i915->oa_pmu.exclusive_event != event);
	i915->oa_pmu.exclusive_event = NULL;

	intel_uncore_forcewake_put(i915, FORCEWAKE_ALL);
	intel_runtime_pm_put(i915);
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

static int init_oa_buffer(struct perf_event *event)
{
	struct drm_i915_private *dev_priv =
		container_of(event->pmu, typeof(*dev_priv), oa_pmu.pmu);
	struct drm_i915_gem_object *bo;
	int ret;

	BUG_ON(!IS_HASWELL(dev_priv->dev));
	BUG_ON(!mutex_is_locked(&dev_priv->dev->struct_mutex));
	BUG_ON(dev_priv->oa_pmu.oa_buffer.obj);

	spin_lock_init(&dev_priv->oa_pmu.oa_buffer.flush_lock);

	/* NB: We over allocate the OA buffer due to the way raw sample data
	 * gets copied from the gpu mapped circular buffer into the perf
	 * circular buffer so that only one copy is required.
	 *
	 * For each perf sample (raw->size + 4) needs to be 8 byte aligned,
	 * where the 4 corresponds to the 32bit raw->size member that's
	 * added to the sample header that userspace sees.
	 *
	 * Due to the + 4 for the size member: when we copy a report to the
	 * userspace facing perf buffer we always copy an additional 4 bytes
	 * from the subsequent report to make up for the miss alignment, but
	 * when a report is at the end of the gpu mapped buffer we need to
	 * read 4 bytes past the end of the buffer.
	 */
	bo = i915_gem_alloc_object(dev_priv->dev, OA_BUFFER_SIZE + PAGE_SIZE);
	if (bo == NULL) {
		DRM_ERROR("Failed to allocate OA buffer\n");
		ret = -ENOMEM;
		goto err;
	}
	dev_priv->oa_pmu.oa_buffer.obj = bo;

	ret = i915_gem_object_set_cache_level(bo, I915_CACHE_LLC);
	if (ret)
		goto err_unref;

	/* PreHSW required 512K alignment, HSW requires 16M */
	ret = i915_gem_obj_ggtt_pin(bo, SZ_16M, 0);
	if (ret)
		goto err_unref;

	dev_priv->oa_pmu.oa_buffer.gtt_offset = i915_gem_obj_ggtt_offset(bo);
	dev_priv->oa_pmu.oa_buffer.addr = vmap_oa_buffer(bo);

	/* Pre-DevBDW: OABUFFER must be set with counters off,
	 * before OASTATUS1, but after OASTATUS2 */
	I915_WRITE(GEN7_OASTATUS2, dev_priv->oa_pmu.oa_buffer.gtt_offset |
		   GEN7_OASTATUS2_GGTT); /* head */
	I915_WRITE(GEN7_OABUFFER, dev_priv->oa_pmu.oa_buffer.gtt_offset);
	I915_WRITE(GEN7_OASTATUS1, dev_priv->oa_pmu.oa_buffer.gtt_offset |
		   GEN7_OASTATUS1_OABUFFER_SIZE_16M); /* tail */

	DRM_DEBUG_DRIVER("OA Buffer initialized, gtt offset = 0x%x, vaddr = %p",
			 dev_priv->oa_pmu.oa_buffer.gtt_offset,
			 dev_priv->oa_pmu.oa_buffer.addr);

	return 0;

err_unref:
	drm_gem_object_unreference(&bo->base);
err:
	return ret;
}

static enum hrtimer_restart hrtimer_sample(struct hrtimer *hrtimer)
{
	struct drm_i915_private *i915 =
		container_of(hrtimer, typeof(*i915), oa_pmu.timer);

	flush_oa_snapshots(i915, true);

	hrtimer_forward_now(hrtimer, ns_to_ktime(PERIOD));
	return HRTIMER_RESTART;
}

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
			mutex_unlock(&dev_priv->dev->struct_mutex);
			return ctx;
		}
	}
	mutex_unlock(&dev_priv->dev->struct_mutex);

	return NULL;
}

static int i915_oa_copy_attr(drm_i915_oa_attr_t __user *uattr,
			     drm_i915_oa_attr_t *attr)
{
	u32 size;
	int ret;

	if (!access_ok(VERIFY_WRITE, uattr, I915_OA_ATTR_SIZE_VER0))
		return -EFAULT;

	/*
	 * zero the full structure, so that a short copy will be nice.
	 */
	memset(attr, 0, sizeof(*attr));

	ret = get_user(size, &uattr->size);
	if (ret)
		return ret;

	if (size > PAGE_SIZE)	/* silly large */
		goto err_size;

	if (size < I915_OA_ATTR_SIZE_VER0)
		goto err_size;

	/*
	 * If we're handed a bigger struct than we know of,
	 * ensure all the unknown bits are 0 - i.e. new
	 * user-space does not rely on any kernel feature
	 * extensions we dont know about yet.
	 */
	if (size > sizeof(*attr)) {
		unsigned char __user *addr;
		unsigned char __user *end;
		unsigned char val;

		addr = (void __user *)uattr + sizeof(*attr);
		end  = (void __user *)uattr + size;

		for (; addr < end; addr++) {
			ret = get_user(val, addr);
			if (ret)
				return ret;
			if (val)
				goto err_size;
		}
		size = sizeof(*attr);
	}

	ret = copy_from_user(attr, uattr, size);
	if (ret)
		return -EFAULT;

	if (attr->__reserved_1)
		return -EINVAL;

out:
	return ret;

err_size:
	put_user(sizeof(*attr), &uattr->size);
	ret = -E2BIG;
	goto out;
}

static int i915_oa_event_init(struct perf_event *event)
{
	struct drm_i915_private *dev_priv =
		container_of(event->pmu, typeof(*dev_priv), oa_pmu.pmu);
	drm_i915_oa_attr_t oa_attr;
	u64 report_format;
	int ret = 0;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	ret = i915_oa_copy_attr(to_user_ptr(event->attr.config), &oa_attr);
	if (ret)
		return ret;

	/* To avoid the complexity of having to accurately filter
	 * counter snapshots and marshal to the appropriate client
	 * we currently only allow exclusive access */
	if (dev_priv->oa_pmu.oa_buffer.obj)
		return -EBUSY;

	report_format = oa_attr.format;
	dev_priv->oa_pmu.oa_buffer.format = report_format;
	dev_priv->oa_pmu.metrics_set = oa_attr.metrics_set;

	if (IS_HASWELL(dev_priv->dev)) {
		int snapshot_size;

		if (report_format >= ARRAY_SIZE(hsw_perf_format_sizes))
			return -EINVAL;

		snapshot_size = hsw_perf_format_sizes[report_format];
		if (snapshot_size < 0)
			return -EINVAL;

		dev_priv->oa_pmu.oa_buffer.format_size = snapshot_size;

		if (oa_attr.metrics_set > I915_OA_METRICS_SET_3D)
			return -EINVAL;
	} else {
		BUG(); /* pmu shouldn't have been registered */
		return -ENODEV;
	}

	/* Since we are limited to an exponential scale for
	 * programming the OA sampling period we don't allow userspace
	 * to pass a precise attr.sample_period. */
	if (event->attr.freq ||
	    (event->attr.sample_period != 0 &&
	     event->attr.sample_period != 1))
		return -EINVAL;

	dev_priv->oa_pmu.periodic = event->attr.sample_period;

	/* Instead of allowing userspace to configure the period via
	 * attr.sample_period we instead accept an exponent whereby
	 * the sample_period will be:
	 *
	 *   80ns * 2^(period_exponent + 1)
	 *
	 * Programming a period of 160 nanoseconds would not be very
	 * polite, so higher frequencies are reserved for root.
	 */
	if (dev_priv->oa_pmu.periodic) {
		u64 period_exponent = oa_attr.timer_exponent;

		if (period_exponent > 63)
			return -EINVAL;

		if (period_exponent < 15 && !capable(CAP_SYS_ADMIN))
			return -EACCES;

		dev_priv->oa_pmu.period_exponent = period_exponent;
	} else if (oa_attr.timer_exponent)
		return -EINVAL;

	/* We bypass the default perf core perf_paranoid_cpu() ||
	 * CAP_SYS_ADMIN check by using the PERF_PMU_CAP_IS_DEVICE
	 * flag and instead authenticate based on whether the current
	 * pid owns the specified context, or require CAP_SYS_ADMIN
	 * when collecting cross-context metrics.
	 */
	dev_priv->oa_pmu.specific_ctx = NULL;
	if (oa_attr.single_context) {
		u32 ctx_id = oa_attr.ctx_id;
		unsigned int drm_fd = oa_attr.drm_fd;
		struct fd fd = fdget(drm_fd);

		if (!fd.file)
			return -EBADF;

		dev_priv->oa_pmu.specific_ctx =
			lookup_context(dev_priv, fd.file, ctx_id);
		fdput(fd);

		if (!dev_priv->oa_pmu.specific_ctx)
			return -EINVAL;
	}

	if (!dev_priv->oa_pmu.specific_ctx &&
	    i915_oa_event_paranoid && !capable(CAP_SYS_ADMIN))
		return -EACCES;

	mutex_lock(&dev_priv->dev->struct_mutex);
	ret = init_oa_buffer(event);
	mutex_unlock(&dev_priv->dev->struct_mutex);

	if (ret)
		return ret;

	BUG_ON(dev_priv->oa_pmu.exclusive_event);
	dev_priv->oa_pmu.exclusive_event = event;

	event->destroy = i915_oa_event_destroy;

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
	 *   references will effectively disable RC6 and trunk clock
	 *   gating.
	 */
	intel_runtime_pm_get(dev_priv);
	intel_uncore_forcewake_get(dev_priv, FORCEWAKE_ALL);

	return 0;
}

static void update_oacontrol(struct drm_i915_private *dev_priv)
{
	BUG_ON(!spin_is_locked(&dev_priv->oa_pmu.lock));

	if (dev_priv->oa_pmu.event_active) {
		unsigned long ctx_id = 0;
		bool pinning_ok = false;

		if (dev_priv->oa_pmu.specific_ctx) {
			struct intel_context *ctx =
				dev_priv->oa_pmu.specific_ctx;
			struct drm_i915_gem_object *obj =
				ctx->legacy_hw_ctx.rcs_state;

			if (i915_gem_obj_is_pinned(obj)) {
				ctx_id = i915_gem_obj_ggtt_offset(obj);
				pinning_ok = true;
			}
		}

		if ((ctx_id == 0 || pinning_ok)) {
			bool periodic = dev_priv->oa_pmu.periodic;
			u32 period_exponent = dev_priv->oa_pmu.period_exponent;
			u32 report_format = dev_priv->oa_pmu.oa_buffer.format;

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

static void config_oa_regs(struct drm_i915_private *dev_priv,
			   struct i915_oa_reg *regs,
			   int n_regs)
{
	int i;

	for (i = 0; i < n_regs; i++) {
		struct i915_oa_reg *reg = regs + i;

		I915_WRITE(reg->addr, reg->value);
	}
}

static void i915_oa_event_start(struct perf_event *event, int flags)
{
	struct drm_i915_private *dev_priv =
		container_of(event->pmu, typeof(*dev_priv), oa_pmu.pmu);
	unsigned long lock_flags;
	u32 oastatus1, tail;

	/* PRM - observability performance counters:
	 *
	 *   OACONTROL, specific context enable:
	 *
	 *   "OA unit level clock gating must be ENABLED when using
	 *   specific ContextID feature."
	 *
	 * Assuming we don't ever disable OA unit level clock gating
	 * lets just assert that this condition is met...
	 */
	WARN_ONCE(I915_READ(GEN6_UCGCTL3) & GEN6_OACSUNIT_CLOCK_GATE_DISABLE,
		  "disabled OA unit level clock gating will result in incorrect per-context OA counters");

	I915_WRITE(GDT_CHICKEN_BITS, GT_NOA_ENABLE);

	if (dev_priv->oa_pmu.metrics_set == I915_OA_METRICS_SET_3D) {
		config_oa_regs(dev_priv, hsw_profile_3d_mux_config,
			       ARRAY_SIZE(hsw_profile_3d_mux_config));
		config_oa_regs(dev_priv, hsw_profile_3d_b_counter_config,
			       ARRAY_SIZE(hsw_profile_3d_b_counter_config));
	} else {
		/* XXX: On Haswell, when threshold disable mode is desired,
		 * instead of setting the threshold enable to '0', we need to
		 * program it to '1' and set OASTARTTRIG1 bits 15:0 to 0
		 * (threshold value of 0)
		 */
		I915_WRITE(OASTARTTRIG6, (OASTARTTRIG6_THRESHOLD_ENABLE |
					  OASTARTTRIG6_EVENT_SELECT_4));
		I915_WRITE(OASTARTTRIG5, 0); /* threshold value */

		I915_WRITE(OASTARTTRIG2, (OASTARTTRIG2_THRESHOLD_ENABLE |
					  OASTARTTRIG2_EVENT_SELECT_0));
		I915_WRITE(OASTARTTRIG1, 0); /* threshold value */

		/* Setup B0 as the gpu clock counter... */
		I915_WRITE(OACEC0_0, OACEC_COMPARE_GREATER_OR_EQUAL); /* to 0 */
		I915_WRITE(OACEC0_1, 0xfffe); /* Select NOA[0] */
	}

	spin_lock_irqsave(&dev_priv->oa_pmu.lock, lock_flags);

	dev_priv->oa_pmu.event_active = true;
	update_oacontrol(dev_priv);

	/* Reset the head ptr to ensure we don't forward reports relating
	 * to a previous perf event */
	oastatus1 = I915_READ(GEN7_OASTATUS1);
	tail = oastatus1 & GEN7_OASTATUS1_TAIL_MASK;
	I915_WRITE(GEN7_OASTATUS2, (tail & GEN7_OASTATUS2_HEAD_MASK) |
				    GEN7_OASTATUS2_GGTT);

	mmiowb();
	spin_unlock_irqrestore(&dev_priv->oa_pmu.lock, lock_flags);

	if (event->attr.sample_period)
		__hrtimer_start_range_ns(&dev_priv->oa_pmu.timer,
					 ns_to_ktime(PERIOD), 0,
					 HRTIMER_MODE_REL_PINNED, 0);

	event->hw.state = 0;
}

static void i915_oa_event_stop(struct perf_event *event, int flags)
{
	struct drm_i915_private *dev_priv =
		container_of(event->pmu, typeof(*dev_priv), oa_pmu.pmu);
	unsigned long lock_flags;

	spin_lock_irqsave(&dev_priv->oa_pmu.lock, lock_flags);

	dev_priv->oa_pmu.event_active = false;
	update_oacontrol(dev_priv);

	mmiowb();
	spin_unlock_irqrestore(&dev_priv->oa_pmu.lock, lock_flags);

	if (event->attr.sample_period) {
		hrtimer_cancel(&dev_priv->oa_pmu.timer);
		flush_oa_snapshots(dev_priv, false);
	}

	event->hw.state = PERF_HES_STOPPED;
}

static int i915_oa_event_add(struct perf_event *event, int flags)
{
	if (flags & PERF_EF_START)
		i915_oa_event_start(event, flags);

	return 0;
}

static void i915_oa_event_del(struct perf_event *event, int flags)
{
	i915_oa_event_stop(event, flags);
}

static void i915_oa_event_read(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), oa_pmu.pmu);

	/* XXX: What counter would be useful here? */
	local64_set(&event->count, 0);
}

static int i915_oa_event_flush(struct perf_event *event)
{
	if (event->attr.sample_period) {
		struct drm_i915_private *i915 =
			container_of(event->pmu, typeof(*i915), oa_pmu.pmu);

		flush_oa_snapshots(i915, true);
	}

	return 0;
}

static int i915_oa_event_event_idx(struct perf_event *event)
{
	return 0;
}

void i915_oa_context_pin_notify(struct drm_i915_private *dev_priv,
				struct intel_context *context)
{
	unsigned long flags;

	if (dev_priv->oa_pmu.pmu.event_init == NULL)
		return;

	spin_lock_irqsave(&dev_priv->oa_pmu.lock, flags);

	if (dev_priv->oa_pmu.specific_ctx == context)
		update_oacontrol(dev_priv);

	mmiowb();
	spin_unlock_irqrestore(&dev_priv->oa_pmu.lock, flags);
}

void i915_oa_context_unpin_notify(struct drm_i915_private *dev_priv,
				  struct intel_context *context)
{
	unsigned long flags;

	if (dev_priv->oa_pmu.pmu.event_init == NULL)
		return;

	spin_lock_irqsave(&dev_priv->oa_pmu.lock, flags);

	if (dev_priv->oa_pmu.specific_ctx == context)
		update_oacontrol(dev_priv);

	mmiowb();
	spin_unlock_irqrestore(&dev_priv->oa_pmu.lock, flags);
}

static struct ctl_table oa_table[] = {
	{
	 .procname = "oa_event_paranoid",
	 .data = &i915_oa_event_paranoid,
	 .maxlen = sizeof(i915_oa_event_paranoid),
	 .mode = 0644,
	 .proc_handler = proc_dointvec,
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

void i915_oa_pmu_register(struct drm_device *dev)
{
	struct drm_i915_private *i915 = to_i915(dev);

	if (!IS_HASWELL(dev))
		return;

	i915->oa_pmu.sysctl_header = register_sysctl_table(dev_root);

	/* We need to be careful about forwarding cpu metrics to
	 * userspace considering that PERF_PMU_CAP_IS_DEVICE bypasses
	 * the events/core security check that stops an unprivileged
	 * process collecting metrics for other processes.
	 */
	i915->oa_pmu.dummy_regs = *task_pt_regs(current);

	hrtimer_init(&i915->oa_pmu.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	i915->oa_pmu.timer.function = hrtimer_sample;

	spin_lock_init(&i915->oa_pmu.lock);

	i915->oa_pmu.pmu.capabilities  = PERF_PMU_CAP_IS_DEVICE;

	/* Effectively disallow opening an event with a specific pid
	 * since we aren't interested in processes running on the cpu...
	 */
	i915->oa_pmu.pmu.task_ctx_nr   = perf_invalid_context;

	i915->oa_pmu.pmu.event_init    = i915_oa_event_init;
	i915->oa_pmu.pmu.add	       = i915_oa_event_add;
	i915->oa_pmu.pmu.del	       = i915_oa_event_del;
	i915->oa_pmu.pmu.start	       = i915_oa_event_start;
	i915->oa_pmu.pmu.stop	       = i915_oa_event_stop;
	i915->oa_pmu.pmu.read	       = i915_oa_event_read;
	i915->oa_pmu.pmu.flush	       = i915_oa_event_flush;
	i915->oa_pmu.pmu.event_idx     = i915_oa_event_event_idx;

	if (perf_pmu_register(&i915->oa_pmu.pmu, "i915_oa", -1))
		i915->oa_pmu.pmu.event_init = NULL;
}

void i915_oa_pmu_unregister(struct drm_device *dev)
{
	struct drm_i915_private *i915 = to_i915(dev);

	if (i915->oa_pmu.pmu.event_init == NULL)
		return;

	unregister_sysctl_table(i915->oa_pmu.sysctl_header);

	perf_pmu_unregister(&i915->oa_pmu.pmu);
	i915->oa_pmu.pmu.event_init = NULL;
}
