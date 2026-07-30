// SPDX-License-Identifier: GPL-2.0
#include "fplayerdemo.h"

static volatile sig_atomic_t g_abort_requested;

uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void profile_add_us(uint64_t *total, uint64_t *max, uint64_t val)
{
	*total += val;
	if (max && val > *max)
		*max = val;
}

uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t timeval_to_us(const struct timeval *tv)
{
	return (uint64_t)tv->tv_sec * 1000000 + tv->tv_usec;
}

static bool read_system_cpu(uint64_t *total, uint64_t *idle)
{
	unsigned long long user, nice, system, idle_j, iowait;
	unsigned long long irq, softirq, steal, guest, guest_nice;
	FILE *fp;
	int n;

	fp = fopen("/proc/stat", "r");
	if (!fp)
		return false;

	n = fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
		   &user, &nice, &system, &idle_j, &iowait, &irq, &softirq,
		   &steal, &guest, &guest_nice);
	fclose(fp);
	if (n < 4)
		return false;

	if (n < 5)
		iowait = 0;
	if (n < 6)
		irq = 0;
	if (n < 7)
		softirq = 0;
	if (n < 8)
		steal = 0;
	if (n < 9)
		guest = 0;
	if (n < 10)
		guest_nice = 0;

	*idle = idle_j + iowait;
	*total = user + nice + system + idle_j + iowait + irq + softirq +
		 steal + guest + guest_nice;
	return true;
}

static uint64_t read_rss_kb(void)
{
	unsigned long pages = 0;
	long page_kb = sysconf(_SC_PAGESIZE) / 1024;
	FILE *fp;

	fp = fopen("/proc/self/statm", "r");
	if (!fp)
		return 0;

	if (fscanf(fp, "%*s %lu", &pages) != 1)
		pages = 0;
	fclose(fp);

	return (uint64_t)pages * (page_kb > 0 ? (uint64_t)page_kb : 4);
}

static void usage_sample_read(struct usage_sample *sample)
{
	struct rusage ru;

	memset(sample, 0, sizeof(*sample));
	sample->wall_us = now_us();
	if (!getrusage(RUSAGE_SELF, &ru))
		sample->proc_cpu_us = timeval_to_us(&ru.ru_utime) +
				      timeval_to_us(&ru.ru_stime);
	sample->rss_kb = read_rss_kb();
	sample->has_sys_cpu = read_system_cpu(&sample->sys_total,
					      &sample->sys_idle);
}

bool usage_timing_enabled(const struct options *opt)
{
	return opt->profile || opt->usage_report;
}

void usage_report_maybe(struct usage_report *report,
			       const struct options *opt,
			       uint32_t decoded, uint32_t displayed,
			       const struct lite_kms *kms,
			       uint64_t submit_us,
			       const struct submit_profile *submit,
			       bool force)
{
	struct usage_sample now;
	uint64_t wall_delta;
	uint32_t decoded_delta;
	uint32_t displayed_delta;
	double proc_cpu = 0.0;
	double decode_fps = 0.0;
	double display_fps = 0.0;

	if (!opt->usage_report)
		return;

	if (report->initialized && !force) {
		uint64_t wall_now = now_us();

		if (wall_now - report->last.wall_us < USAGE_REPORT_INTERVAL_US)
			return;
	}

	usage_sample_read(&now);
	if (!report->initialized) {
		report->initialized = true;
		report->last = now;
		report->first_wall_us = now.wall_us;
		report->last_decoded = decoded;
		report->last_displayed = displayed;
		return;
	}

	wall_delta = now.wall_us - report->last.wall_us;
	if (!force && wall_delta < USAGE_REPORT_INTERVAL_US)
		return;
	if (force && wall_delta < USAGE_REPORT_INTERVAL_US / 2)
		return;

	decoded_delta = decoded - report->last_decoded;
	displayed_delta = displayed - report->last_displayed;
	if (force && !decoded_delta && !displayed_delta)
		return;

	if (wall_delta)
		proc_cpu = (double)(now.proc_cpu_us - report->last.proc_cpu_us) *
			   100.0 / wall_delta;
	decode_fps = wall_delta ? (double)decoded_delta * 1000000.0 /
				  wall_delta : 0.0;
	display_fps = wall_delta ? (double)displayed_delta * 1000000.0 /
				   wall_delta : 0.0;

	fprintf(stderr, "fps %.1f/%.1f cpu %.1f%% frames %u/%u\n",
		decode_fps, display_fps, proc_cpu, decoded, displayed);

	report->last = now;
	report->last_decoded = decoded;
	report->last_displayed = displayed;
}

bool playback_aborted(const struct options *opt)
{
	return g_abort_requested ||
	       (opt->abort_playback &&
		__atomic_load_n(opt->abort_playback, __ATOMIC_RELAXED));
}

int sleep_until_us(uint64_t target_us, const struct options *opt)
{
	for (;;) {
		uint64_t t = now_us();
		uint64_t delta;

		if (playback_aborted(opt))
			return -ECANCELED;
		if (t >= target_us)
			return 0;

		delta = target_us - t;
		if (delta > 100000)
			delta = 100000;
		usleep(delta);
	}
}

int av_start_clock_init(struct av_start_clock *clock)
{
	memset(clock, 0, sizeof(*clock));
	if (pthread_mutex_init(&clock->lock, NULL))
		return -1;
	if (pthread_cond_init(&clock->cond, NULL)) {
		pthread_mutex_destroy(&clock->lock);
		return -1;
	}
	clock->initialized = true;
	return 0;
}

void av_start_clock_destroy(struct av_start_clock *clock)
{
	if (!clock->initialized)
		return;
	pthread_cond_destroy(&clock->cond);
	pthread_mutex_destroy(&clock->lock);
	memset(clock, 0, sizeof(*clock));
}

void av_start_clock_publish(const struct options *opt)
{
	struct av_start_clock *clock = opt->av_clock;

	if (!clock || !clock->initialized)
		return;
	pthread_mutex_lock(&clock->lock);
	if (!clock->ready) {
		clock->base_us = now_us() + AV_PLAYBACK_START_DELAY_US;
		clock->ready = true;
		pthread_cond_broadcast(&clock->cond);
	}
	pthread_mutex_unlock(&clock->lock);
}

uint64_t playback_base_get(const struct options *opt)
{
	struct av_start_clock *clock = opt->av_clock;
	uint64_t base = opt->playback_base_us;

	if (!clock || !clock->initialized)
		return base;
	pthread_mutex_lock(&clock->lock);
	if (clock->ready)
		base = clock->base_us;
	pthread_mutex_unlock(&clock->lock);
	return base;
}

void playback_abort(int *abort_playback)
{
	if (abort_playback)
		__atomic_store_n(abort_playback, 1, __ATOMIC_RELAXED);
}

static void handle_stop_signal(int sig)
{
	(void)sig;
	g_abort_requested = 1;
}

int install_signal_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_stop_signal;
	sigemptyset(&sa.sa_mask);

	if (sigaction(SIGINT, &sa, NULL) < 0)
		return -1;
	if (sigaction(SIGTERM, &sa, NULL) < 0)
		return -1;

	return 0;
}

int audio_wait_for_playback_base(const struct options *opt)
{
	uint64_t base;

	if (opt->no_pace)
		return 0;
	if (opt->av_clock && opt->av_clock->initialized) {
		struct av_start_clock *clock = opt->av_clock;

		pthread_mutex_lock(&clock->lock);
		while (!clock->ready && !playback_aborted(opt)) {
			struct timespec deadline;

			clock_gettime(CLOCK_REALTIME, &deadline);
			deadline.tv_nsec += 100000000;
			if (deadline.tv_nsec >= 1000000000) {
				deadline.tv_sec++;
				deadline.tv_nsec -= 1000000000;
			}
			pthread_cond_timedwait(&clock->cond, &clock->lock,
					       &deadline);
		}
		base = clock->ready ? clock->base_us : 0;
		pthread_mutex_unlock(&clock->lock);
		if (playback_aborted(opt))
			return -ECANCELED;
	} else {
		base = opt->playback_base_us;
	}
	if (!base)
		return 0;

	for (;;) {
		uint64_t t = now_us();
		uint64_t delta;

		if (playback_aborted(opt))
			return -ECANCELED;
		if (t >= base)
			return 0;

		delta = base - t;
		if (delta > 20000)
			delta = 20000;
		usleep(delta);
	}
}

void set_thread_name(const char *name)
{
	prctl(PR_SET_NAME, name, 0, 0, 0);
}

uint32_t align_up_u32(uint32_t val, uint32_t align)
{
	return (val + align - 1) & ~(align - 1);
}

uint32_t align_down_u32(uint32_t val, uint32_t align)
{
	return val & ~(align - 1);
}

static uint32_t st12_stride(uint32_t width)
{
	return align_up_u32(width, 32);
}

static uint32_t st12_coded_height(uint32_t height)
{
	return align_up_u32(height, 32);
}

static uint32_t st12_frame_size(uint32_t width, uint32_t height)
{
	uint32_t stride = st12_stride(width);
	uint32_t coded_height = st12_coded_height(height);

	return stride * coded_height +
	       stride * align_up_u32(coded_height, 64) / 2;
}

uint32_t h264_sps_coded_width(const struct v4l2_ctrl_h264_sps *sps)
{
	return (sps->pic_width_in_mbs_minus1 + 1) * 16;
}

uint32_t h264_sps_coded_height(const struct v4l2_ctrl_h264_sps *sps)
{
	uint32_t map_units = sps->pic_height_in_map_units_minus1 + 1;
	uint32_t frame_mbs_only =
		!!(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY);

	return map_units * 16 * (2 - frame_mbs_only);
}

uint32_t h264_info_coded_width(const struct h264_stream_info *info)
{
	return (info->width_mbs_minus1 + 1) * 16;
}

uint32_t h264_info_coded_height(const struct h264_stream_info *info)
{
	uint32_t map_units = info->height_map_units_minus1 + 1;

	return map_units * 16 * (2 - !!info->frame_mbs_only_flag);
}

uint32_t h264_info_display_width(const struct h264_stream_info *info)
{
	return info->crop_width ? info->crop_width : h264_info_coded_width(info);
}

uint32_t h264_info_display_height(const struct h264_stream_info *info)
{
	return info->crop_height ? info->crop_height :
	       h264_info_coded_height(info);
}

static uint32_t h264_reorder_delay(const struct h264_stream_info *info,
				   uint32_t max_num_ref_frames)
{
	uint32_t reorder = info->vui_bitstream_restriction ?
			   info->vui_num_reorder_frames : 2;

	if (info->vui_bitstream_restriction &&
	    info->vui_max_dec_frame_buffering > max_num_ref_frames)
		reorder = info->vui_max_dec_frame_buffering -
			  max_num_ref_frames;
	if (reorder < 1)
		reorder = 1;

	return reorder;
}

uint32_t min_capture_buffers(const struct h264_stream_info *info,
				    uint32_t max_num_ref_frames,
				    bool display)
{
	uint32_t hold = display ? LITE_SCANOUT_HOLD : 0;
	uint32_t min = max_num_ref_frames +
		       h264_reorder_delay(info, max_num_ref_frames) + hold + 2;

	return min > LITE_CAPTURE_BUFFERS ? LITE_CAPTURE_BUFFERS : min;
}

static uint64_t read_soft_cma_bytes(void)
{
	const char *env = getenv("F1C_LITE_CMA_MB");
	FILE *fp;
	char line[128];
	uint64_t mb;

	if (env && *env) {
		char *end = NULL;

		mb = strtoull(env, &end, 0);
		if (end && *end == '\0' && mb > 0)
			return mb * 1024ull * 1024ull;
	}

	fp = fopen("/proc/meminfo", "r");
	if (!fp)
		return LITE_FALLBACK_CMA_BYTES;

	while (fgets(line, sizeof(line), fp)) {
		unsigned long long kb;

		if (sscanf(line, "CmaTotal: %llu kB", &kb) == 1) {
			fclose(fp);
			return kb * 1024ull;
		}
	}

	fclose(fp);
	return LITE_FALLBACK_CMA_BYTES;
}

uint32_t read_capture_extra_buffers(void)
{
	const char *env = getenv("F1C_LITE_CAPTURE_EXTRA");
	char *end = NULL;
	unsigned long extra;

	if (!env || !*env)
		return 2;

	errno = 0;
	extra = strtoul(env, &end, 0);
	if (errno || !end || *end || extra > LITE_CAPTURE_BUFFERS)
		return 2;

	return extra;
}

int check_capture_memory_budget(uint32_t width, uint32_t height,
				       uint32_t min_capture,
				       uint32_t output_size)
{
	uint32_t frame_size = st12_frame_size(width, height);
	uint64_t need = (uint64_t)frame_size * min_capture + output_size;
	uint64_t soft_cma = read_soft_cma_bytes();

	if (need <= soft_cma)
		return 0;

	fprintf(stderr,
		"unsupported buffer budget: %ux%u ST12 frame=%u min_capture=%u output=%u need=%" PRIu64 " > soft_cma=%" PRIu64 "\n",
		width, height, frame_size, min_capture, output_size, need,
		soft_cma);
	fprintf(stderr,
		"hint: enlarge CMA or set F1C_LITE_CMA_MB for this test; lower steps include 854x480, 960x540, and 1280x720.\n");
	return -1;
}

uint32_t clamp_capture_request_to_cma(uint32_t width, uint32_t height,
					     uint32_t minimum,
					     uint32_t requested,
					     uint32_t output_size,
					     bool verbose)
{
	uint32_t frame_size = st12_frame_size(width, height);
	uint64_t soft_cma = read_soft_cma_bytes();
	uint32_t max_capture;

	if (requested <= minimum || soft_cma <= output_size || !frame_size)
		return requested;

	max_capture = (soft_cma - output_size) / frame_size;
	if (max_capture > minimum)
		max_capture--;
	if (max_capture < minimum)
		max_capture = minimum;

	if (requested > max_capture) {
		if (verbose)
			fprintf(stderr,
				"v4l2 capture request trimmed=%u->%u by soft_cma=%" PRIu64 "\n",
				requested, max_capture, soft_cma);
		return max_capture;
	}

	return requested;
}

uint16_t rd16(const uint8_t *p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

uint64_t rd64(const uint8_t *p)
{
	return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}

bool fourcc_is(const uint8_t *p, const char *s)
{
	return !memcmp(p, s, 4);
}

char *pixfmt_str(uint32_t pixfmt, char out[5])
{
	out[0] = pixfmt & 0xff;
	out[1] = (pixfmt >> 8) & 0xff;
	out[2] = (pixfmt >> 16) & 0xff;
	out[3] = (pixfmt >> 24) & 0xff;
	out[4] = 0;
	return out;
}

char *fourcc_str(const uint8_t *p, char out[5])
{
	memcpy(out, p, 4);
	out[4] = 0;
	return out;
}

int xioctl(int fd, unsigned long req, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, req, arg);
	} while (ret < 0 && errno == EINTR);

	return ret;
}

#ifdef HAVE_LIBAVCODEC
const char *ffmpeg_errstr(int err, char *buf, size_t len)
{
	if (av_strerror(err, buf, len) < 0)
		snprintf(buf, len, "ffmpeg error %d", err);
	return buf;
}
#endif
