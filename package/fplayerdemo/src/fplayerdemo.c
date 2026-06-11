// SPDX-License-Identifier: GPL-2.0
/*
 * F1C200S Cedrus/KMS player and diagnostics tool.
 *
 * This tool bypasses GStreamer pipelines and appsink. It can parse simple
 * MP4/AVC files directly or use FFmpeg/libavformat to demux common containers,
 * walks H.264 NAL units with gst-codecparsers, and can probe the direct V4L2
 * stateless decoder setup used by the custom request/KMS path.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <gst/gst.h>
#include <gst/codecparsers/gsth264parser.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <linux/media.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#ifdef HAVE_LIBAVCODEC
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define H264_OUTPUT_SIZE_MIN (256 * 1024)
#define LITE_CAPTURE_BUFFERS 16
#define LITE_FALLBACK_CMA_BYTES (16u * 1024u * 1024u)
#define LITE_MAX_DPB 16
#define LITE_REORDER_QUEUE 16
#define LITE_SCANOUT_HOLD 4
#define LITE_NV12_PLANES 2
#define TILE_W 32
#define TILE_H 32
#define FILE_READAHEAD_BYTES (1024u * 1024u)
#define FILE_KEEP_BEHIND_BYTES (512u * 1024u)
#define FILE_DISCARD_STEP_BYTES (256u * 1024u)
#define AV_PLAYBACK_START_DELAY_US 500000ULL
#define USAGE_REPORT_INTERVAL_US 1000000ULL
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct playlist {
	char **items;
	size_t count;
	size_t capacity;
};

struct blob {
	const uint8_t *data;
	uint32_t size;
};

enum audio_backend {
	AUDIO_BACKEND_AUTO,
	AUDIO_BACKEND_FFMPEG,
	AUDIO_BACKEND_FFMPEG_FIXED,
};

struct stts_entry {
	uint32_t count;
	uint32_t delta;
};

struct ctts_entry {
	uint32_t count;
	int64_t offset;
};

struct stsc_entry {
	uint32_t first_chunk;
	uint32_t samples_per_chunk;
	uint32_t sample_description_index;
};

struct sample_info {
	uint64_t offset;
	uint64_t dts_ticks;
	int64_t pts_ticks;
	uint64_t pts_us;
	uint32_t size;
	uint32_t duration;
	bool has_time;
	bool sync;
};

enum h264_bitstream_format {
	H264_BITSTREAM_AVCC,
	H264_BITSTREAM_ANNEXB,
};

struct video_track {
	bool is_video;
	bool is_audio;
	bool owns_sample_data;
	uint32_t timescale;
	uint16_t width;
	uint16_t height;
	enum AVCodecID codec;
	enum h264_bitstream_format h264_format;
	uint8_t nal_length_size;

	struct blob sps[32];
	unsigned int num_sps;
	struct blob pps[256];
	unsigned int num_pps;

	char audio_format[5];
	uint16_t audio_channels;
	uint16_t audio_sample_size;
	uint32_t audio_sample_rate;
	uint8_t esds_object_type;
	uint8_t esds_stream_type;
	uint32_t esds_buffer_size;
	uint32_t esds_max_bitrate;
	uint32_t esds_avg_bitrate;
	struct blob audio_specific_config;
	uint8_t aac_object_type;
	uint8_t aac_ext_object_type;
	uint8_t aac_sample_rate_index;
	uint8_t aac_channel_config;
	uint32_t aac_sample_rate;
	uint32_t aac_ext_sample_rate;
	bool has_esds;

	struct stts_entry *stts;
	uint32_t stts_count;
	struct ctts_entry *ctts;
	uint32_t ctts_count;
	struct stsc_entry *stsc;
	uint32_t stsc_count;
	uint32_t *sample_sizes;
	uint32_t default_sample_size;
	uint32_t sample_count;
	uint64_t *chunk_offsets;
	uint32_t chunk_count;
	uint32_t *sync_samples;
	uint32_t sync_count;

	struct sample_info *samples;
	uint8_t **sample_data;
	uint8_t *extra_data;
	size_t extra_size;
	int ffmpeg_codec_id;
	int64_t min_pts_ticks;
};

struct options {
	const char *input;
	const char *video_dev;
	const char *media_dev;
	const char *drm_card;
	const char *pcm_dev;
	enum audio_backend audio_backend;
	uint32_t plane_id;
	bool profile;
	bool usage_report;
	bool auto_play;
	bool display;
	bool audio_play;
	bool no_pace;
	bool atomic;
	bool crop_no_scale;
	bool loop;
	uint64_t playback_base_us;
	int *abort_playback;
};

struct stats {
	uint32_t samples;
	uint32_t nalus;
	uint32_t sps;
	uint32_t pps;
	uint32_t sei;
	uint32_t aud;
	uint32_t slices;
	uint32_t idr;
	uint32_t p_slices;
	uint32_t b_slices;
	uint32_t i_slices;
	uint32_t ref_slices;
	uint32_t nonref_slices;
	uint32_t field_slices;
	uint32_t ref_list_mod_l0;
	uint32_t ref_list_mod_l1;
	uint32_t adaptive_marking;
	uint32_t pred_weight;
	uint32_t max_ref_idx_l0;
	uint32_t max_ref_idx_l1;
	uint32_t max_frame_num_seen;
	uint32_t max_poc_lsb_seen;
	uint32_t parse_errors;
	uint64_t bytes;
	uint64_t identify_us;
	uint64_t parse_us;
	uint64_t max_sample_us;
};

struct h264_stream_info {
	bool have_sps;
	uint8_t profile_idc;
	uint8_t level_idc;
	uint32_t max_frame_num;
	uint32_t width_mbs_minus1;
	uint32_t height_map_units_minus1;
	uint32_t crop_x;
	uint32_t crop_y;
	uint32_t crop_width;
	uint32_t crop_height;
	uint8_t frame_cropping_flag;
	uint32_t num_ref_frames;
	uint8_t pic_order_cnt_type;
	uint8_t log2_max_pic_order_cnt_lsb_minus4;
	uint8_t frame_mbs_only_flag;
	uint8_t vui_timing;
	uint32_t vui_num_units_in_tick;
	uint32_t vui_time_scale;
	uint8_t vui_fixed_frame_rate;
	uint8_t vui_bitstream_restriction;
	uint32_t vui_num_reorder_frames;
	uint32_t vui_max_dec_frame_buffering;
	bool have_pps;
	uint8_t pps_id;
	uint8_t sps_id;
	uint8_t entropy_coding_mode;
	uint8_t num_ref_idx_l0_default_active_minus1;
	uint8_t num_ref_idx_l1_default_active_minus1;
	uint8_t weighted_pred_flag;
	uint8_t weighted_bipred_idc;
};

struct kms_ids {
	uint32_t connector_id;
	uint32_t crtc_id;
	uint32_t plane_id;
	uint32_t crtc_index;
	struct drm_mode_modeinfo mode;
};

struct plane_props {
	uint32_t crtc_id;
	uint32_t fb_id;
	uint32_t crtc_x;
	uint32_t crtc_y;
	uint32_t crtc_w;
	uint32_t crtc_h;
	uint32_t src_x;
	uint32_t src_y;
	uint32_t src_w;
	uint32_t src_h;
	uint32_t rotation;
};

struct kms_fb {
	int prime_fd;
	uint32_t handle;
	uint32_t fb_id;
};

struct lite_kms {
	int fd;
	struct kms_ids ids;
	struct plane_props plane_props;
	struct kms_fb fb[LITE_CAPTURE_BUFFERS];
	uint32_t scanout_hold[LITE_SCANOUT_HOLD];
	bool scanout_valid[LITE_SCANOUT_HOLD];
	bool atomic_ready;
	bool atomic_pending;
	bool atomic_plane_configured;
	bool crop_no_scale;
	bool printed_atomic_fallback;
	bool printed_rotation_fallback;
	uint64_t atomic_seq;
	uint32_t scanout_pos;
	uint32_t atomic_width;
	uint32_t atomic_height;
	uint32_t atomic_rotation;
	uint32_t rotation;
	uint32_t src_x;
	uint32_t src_y;
	uint32_t src_width;
	uint32_t src_height;
	uint32_t src_stride;
	uint32_t src_coded_height;
	uint32_t src_frame_size;
	uint32_t fb_width;
	uint32_t fb_height;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t coded_height;
	uint32_t frame_size;
	uint32_t delay_ms;
	uint32_t frames;
	uint64_t base_us;
	uint64_t first_ms;
	uint64_t last_ms;
	uint64_t last_report_ms;
	uint32_t last_report_frame;
	uint64_t sleep_us;
	uint64_t commit_us;
	uint64_t max_sleep_us;
	uint64_t max_commit_us;
};

struct display_frame {
	bool valid;
	uint32_t capture_index;
	uint32_t frame_id;
	uint64_t pts_us;
	bool has_time;
	int32_t poc;
};

struct display_queue {
	struct display_frame frames[LITE_REORDER_QUEUE];
	uint32_t count;
	uint32_t displayed;
	uint32_t reorder_delay;
};

struct usage_sample {
	uint64_t wall_us;
	uint64_t proc_cpu_us;
	uint64_t sys_total;
	uint64_t sys_idle;
	uint64_t rss_kb;
	bool has_sys_cpu;
};

struct usage_report {
	bool initialized;
	struct usage_sample last;
	uint64_t first_wall_us;
	uint32_t last_decoded;
	uint32_t last_displayed;
};

struct submit_profile {
	uint64_t request_us;
	uint64_t controls_us;
	uint64_t copy_us;
	uint64_t queue_us;
	uint64_t wait_us;
	uint64_t dq_us;
	uint64_t max_request_us;
	uint64_t max_controls_us;
	uint64_t max_copy_us;
	uint64_t max_queue_us;
	uint64_t max_wait_us;
	uint64_t max_dq_us;
};

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void profile_add_us(uint64_t *total, uint64_t *max, uint64_t val)
{
	*total += val;
	if (max && val > *max)
		*max = val;
}

static uint64_t now_ms(void)
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

static bool usage_timing_enabled(const struct options *opt)
{
	return opt->profile || opt->usage_report;
}

static void usage_report_maybe(struct usage_report *report,
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

static void sleep_until_us(uint64_t target_us)
{
	for (;;) {
		uint64_t t = now_us();
		uint64_t delta;

		if (t >= target_us)
			return;

		delta = target_us - t;
		if (delta > 20000)
			delta = 20000;
		usleep(delta);
	}
}

static bool playback_aborted(const struct options *opt)
{
	return opt->abort_playback &&
	       __atomic_load_n(opt->abort_playback, __ATOMIC_RELAXED);
}

static void playback_abort(int *abort_playback)
{
	if (abort_playback)
		__atomic_store_n(abort_playback, 1, __ATOMIC_RELAXED);
}

static int audio_wait_for_playback_base(const struct options *opt)
{
	if (opt->no_pace || !opt->playback_base_us)
		return 0;

	for (;;) {
		uint64_t t = now_us();
		uint64_t delta;

		if (playback_aborted(opt))
			return -ECANCELED;
		if (t >= opt->playback_base_us)
			return 0;

		delta = opt->playback_base_us - t;
		if (delta > 20000)
			delta = 20000;
		usleep(delta);
	}
}

static void set_thread_name(const char *name)
{
	prctl(PR_SET_NAME, name, 0, 0, 0);
}

static uint32_t align_up_u32(uint32_t val, uint32_t align)
{
	return (val + align - 1) & ~(align - 1);
}

static uint32_t align_down_u32(uint32_t val, uint32_t align)
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

static uint32_t h264_sps_coded_width(const struct v4l2_ctrl_h264_sps *sps)
{
	return (sps->pic_width_in_mbs_minus1 + 1) * 16;
}

static uint32_t h264_sps_coded_height(const struct v4l2_ctrl_h264_sps *sps)
{
	uint32_t map_units = sps->pic_height_in_map_units_minus1 + 1;
	uint32_t frame_mbs_only =
		!!(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY);

	return map_units * 16 * (2 - frame_mbs_only);
}

static uint32_t h264_info_coded_width(const struct h264_stream_info *info)
{
	return (info->width_mbs_minus1 + 1) * 16;
}

static uint32_t h264_info_coded_height(const struct h264_stream_info *info)
{
	uint32_t map_units = info->height_map_units_minus1 + 1;

	return map_units * 16 * (2 - !!info->frame_mbs_only_flag);
}

static uint32_t h264_info_display_width(const struct h264_stream_info *info)
{
	return info->crop_width ? info->crop_width : h264_info_coded_width(info);
}

static uint32_t h264_info_display_height(const struct h264_stream_info *info)
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

static uint32_t min_capture_buffers(const struct h264_stream_info *info,
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

static uint32_t read_capture_extra_buffers(void)
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

static int check_capture_memory_budget(uint32_t width, uint32_t height,
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

static uint32_t clamp_capture_request_to_cma(uint32_t width, uint32_t height,
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

static uint16_t rd16(const uint8_t *p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t rd64(const uint8_t *p)
{
	return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}

static bool fourcc_is(const uint8_t *p, const char *s)
{
	return !memcmp(p, s, 4);
}

static char *pixfmt_str(uint32_t pixfmt, char out[5])
{
	out[0] = pixfmt & 0xff;
	out[1] = (pixfmt >> 8) & 0xff;
	out[2] = (pixfmt >> 16) & 0xff;
	out[3] = (pixfmt >> 24) & 0xff;
	out[4] = 0;
	return out;
}

static char *fourcc_str(const uint8_t *p, char out[5])
{
	memcpy(out, p, 4);
	out[4] = 0;
	return out;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, req, arg);
	} while (ret < 0 && errno == EINTR);

	return ret;
}

static uint64_t ptr_to_u64(const void *ptr)
{
	return (uint64_t)(uintptr_t)ptr;
}

static int drm_ioctl(int fd, unsigned long request, void *arg,
		     const char *name)
{
	if (xioctl(fd, request, arg) < 0) {
		fprintf(stderr, "%s: %s\n", name, strerror(errno));
		return -1;
	}

	return 0;
}

static int set_client_cap(int fd, uint64_t capability, uint64_t value)
{
	struct drm_set_client_cap cap = {
		.capability = capability,
		.value = value,
	};

	return drm_ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap,
			 "DRM_IOCTL_SET_CLIENT_CAP");
}

static int get_resources(int fd, struct drm_mode_card_res *res,
			 uint32_t **connectors, uint32_t **crtcs)
{
	memset(res, 0, sizeof(*res));
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, res,
		      "DRM_IOCTL_MODE_GETRESOURCES(count)"))
		return -1;

	*connectors = calloc(res->count_connectors, sizeof(uint32_t));
	*crtcs = calloc(res->count_crtcs, sizeof(uint32_t));
	if ((!*connectors && res->count_connectors) ||
	    (!*crtcs && res->count_crtcs))
		return -1;

	res->count_fbs = 0;
	res->count_encoders = 0;
	res->connector_id_ptr = ptr_to_u64(*connectors);
	res->crtc_id_ptr = ptr_to_u64(*crtcs);
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, res,
		      "DRM_IOCTL_MODE_GETRESOURCES(data)"))
		return -1;

	return 0;
}

static int get_connector(int fd, uint32_t connector_id,
			 struct drm_mode_get_connector *conn,
			 struct drm_mode_modeinfo **modes,
			 uint32_t **encoders)
{
	memset(conn, 0, sizeof(*conn));
	conn->connector_id = connector_id;
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, conn,
		      "DRM_IOCTL_MODE_GETCONNECTOR(count)"))
		return -1;

	*modes = calloc(conn->count_modes, sizeof(**modes));
	*encoders = calloc(conn->count_encoders, sizeof(**encoders));
	if ((!*modes && conn->count_modes) ||
	    (!*encoders && conn->count_encoders))
		return -1;

	conn->count_props = 0;
	conn->modes_ptr = ptr_to_u64(*modes);
	conn->encoders_ptr = ptr_to_u64(*encoders);
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, conn,
		      "DRM_IOCTL_MODE_GETCONNECTOR(data)"))
		return -1;

	return 0;
}

static int find_display(int fd, struct kms_ids *ids)
{
	struct drm_mode_card_res res;
	uint32_t *connectors = NULL, *crtcs = NULL;
	struct drm_mode_get_connector conn;
	struct drm_mode_modeinfo *modes = NULL;
	uint32_t *encoders = NULL;
	struct drm_mode_get_encoder enc;
	unsigned int i, j;
	int ret = -1;

	if (get_resources(fd, &res, &connectors, &crtcs))
		goto out;

	for (i = 0; i < res.count_connectors; i++) {
		free(modes);
		free(encoders);
		modes = NULL;
		encoders = NULL;

		if (get_connector(fd, connectors[i], &conn, &modes, &encoders))
			goto out;

		if (conn.connection != 1 || !conn.count_modes)
			continue;

		ids->connector_id = conn.connector_id;
		ids->mode = modes[0];
		for (j = 0; j < conn.count_modes; j++) {
			if (modes[j].type & DRM_MODE_TYPE_PREFERRED) {
				ids->mode = modes[j];
				break;
			}
		}

		memset(&enc, 0, sizeof(enc));
		enc.encoder_id = conn.encoder_id;
		if (!enc.encoder_id && conn.count_encoders)
			enc.encoder_id = encoders[0];
		if (!enc.encoder_id)
			continue;

		if (drm_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc,
			      "DRM_IOCTL_MODE_GETENCODER"))
			goto out;

		ids->crtc_id = enc.crtc_id;
		if (!ids->crtc_id) {
			for (j = 0; j < res.count_crtcs; j++) {
				if (enc.possible_crtcs & (1U << j)) {
					ids->crtc_id = crtcs[j];
					break;
				}
			}
		}

		for (j = 0; j < res.count_crtcs; j++) {
			if (crtcs[j] == ids->crtc_id) {
				ids->crtc_index = j;
				ret = 0;
				goto out;
			}
		}
	}

	fprintf(stderr, "no connected display/CRTC found\n");

out:
	free(encoders);
	free(modes);
	free(crtcs);
	free(connectors);
	return ret;
}

static bool format_list_has(uint32_t *formats, uint32_t count, uint32_t fmt)
{
	uint32_t i;

	for (i = 0; i < count; i++)
		if (formats[i] == fmt)
			return true;

	return false;
}

static int get_planes(int fd, struct drm_mode_get_plane_res *pres,
		      uint32_t **planes)
{
	memset(pres, 0, sizeof(*pres));
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, pres,
		      "DRM_IOCTL_MODE_GETPLANERESOURCES(count)"))
		return -1;

	*planes = calloc(pres->count_planes, sizeof(uint32_t));
	if (!*planes && pres->count_planes)
		return -1;

	pres->plane_id_ptr = ptr_to_u64(*planes);
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, pres,
		      "DRM_IOCTL_MODE_GETPLANERESOURCES(data)")) {
		free(*planes);
		*planes = NULL;
		return -1;
	}

	return 0;
}

static int get_plane_formats(int fd, uint32_t plane_id,
			     struct drm_mode_get_plane *plane,
			     uint32_t **formats)
{
	memset(plane, 0, sizeof(*plane));
	plane->plane_id = plane_id;
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPLANE, plane,
		      "DRM_IOCTL_MODE_GETPLANE(count)"))
		return -1;

	*formats = calloc(plane->count_format_types, sizeof(uint32_t));
	if (!*formats && plane->count_format_types)
		return -1;

	plane->format_type_ptr = ptr_to_u64(*formats);
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPLANE, plane,
		      "DRM_IOCTL_MODE_GETPLANE(data)")) {
		free(*formats);
		*formats = NULL;
		return -1;
	}

	return 0;
}

static int find_plane(int fd, struct kms_ids *ids, uint32_t force_plane_id)
{
	struct drm_mode_get_plane_res pres;
	uint32_t *planes = NULL;
	unsigned int i;
	int ret = -1;

	if (get_planes(fd, &pres, &planes))
		return -1;

	for (i = 0; i < pres.count_planes; i++) {
		struct drm_mode_get_plane plane;
		uint32_t *formats = NULL;

		if (force_plane_id && planes[i] != force_plane_id)
			continue;

		if (get_plane_formats(fd, planes[i], &plane, &formats))
			goto out;

		if ((plane.possible_crtcs & (1U << ids->crtc_index)) &&
		    format_list_has(formats, plane.count_format_types,
				    DRM_FORMAT_NV12)) {
			ids->plane_id = plane.plane_id;
			free(formats);
			ret = 0;
			goto out;
		}

		free(formats);
	}

	if (force_plane_id)
		fprintf(stderr, "plane %u is not an NV12 plane for CRTC %u\n",
			force_plane_id, ids->crtc_id);
	else
		fprintf(stderr, "no NV12 plane found for CRTC %u\n",
			ids->crtc_id);

out:
	free(planes);
	return ret;
}

static void close_gem_handle(int fd, uint32_t handle)
{
	struct drm_gem_close close = { .handle = handle };

	if (handle)
		ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close);
}

static int prime_fd_to_handle(int drm_fd, int prime_fd, uint32_t *handle)
{
	struct drm_prime_handle prime = {
		.fd = prime_fd,
	};

	if (ioctl(drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) < 0)
		return -errno;

	*handle = prime.handle;
	return 0;
}

static int get_property_name(int fd, uint32_t prop_id, char *name,
			     size_t name_size)
{
	struct drm_mode_get_property prop = {
		.prop_id = prop_id,
	};

	if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) < 0)
		return -1;

	snprintf(name, name_size, "%s", prop.name);
	return 0;
}

static int get_object_property_id(int fd, uint32_t obj_id,
				  uint32_t obj_type, const char *name,
				  uint32_t *prop_id)
{
	struct drm_mode_obj_get_properties props;
	uint32_t *ids = NULL;
	uint64_t *values = NULL;
	uint32_t i;
	int ret = -1;

	memset(&props, 0, sizeof(props));
	props.obj_id = obj_id;
	props.obj_type = obj_type;
	if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props) < 0)
		return -1;

	ids = calloc(props.count_props, sizeof(*ids));
	values = calloc(props.count_props, sizeof(*values));
	if ((!ids && props.count_props) || (!values && props.count_props))
		goto out;

	props.props_ptr = ptr_to_u64(ids);
	props.prop_values_ptr = ptr_to_u64(values);
	if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props) < 0)
		goto out;

	for (i = 0; i < props.count_props; i++) {
		char prop_name[DRM_PROP_NAME_LEN];

		if (get_property_name(fd, ids[i], prop_name,
				      sizeof(prop_name)) < 0)
			continue;

		if (!strcmp(prop_name, name)) {
			*prop_id = ids[i];
			ret = 0;
			goto out;
		}
	}

	errno = ENOENT;

out:
	free(values);
	free(ids);
	return ret;
}

static int get_plane_atomic_props(int fd, uint32_t plane_id,
				  struct plane_props *props)
{
	struct {
		const char *name;
		uint32_t *id;
	} needed[] = {
		{ "CRTC_ID", &props->crtc_id },
		{ "FB_ID", &props->fb_id },
		{ "CRTC_X", &props->crtc_x },
		{ "CRTC_Y", &props->crtc_y },
		{ "CRTC_W", &props->crtc_w },
		{ "CRTC_H", &props->crtc_h },
		{ "SRC_X", &props->src_x },
		{ "SRC_Y", &props->src_y },
		{ "SRC_W", &props->src_w },
		{ "SRC_H", &props->src_h },
	};
	unsigned int i;

	memset(props, 0, sizeof(*props));
	for (i = 0; i < ARRAY_SIZE(needed); i++) {
		if (get_object_property_id(fd, plane_id,
					   DRM_MODE_OBJECT_PLANE,
					   needed[i].name,
					   needed[i].id) < 0) {
			fprintf(stderr, "atomic plane property %s missing\n",
				needed[i].name);
			return -1;
		}
	}

	if (get_object_property_id(fd, plane_id, DRM_MODE_OBJECT_PLANE,
				   "rotation", &props->rotation) < 0)
		props->rotation = 0;

	return 0;
}

static int wait_atomic_event(struct lite_kms *kms, int timeout_ms)
{
	struct pollfd pfd = {
		.fd = kms->fd,
		.events = POLLIN,
	};
	uint64_t pending_seq = kms->atomic_seq;

	while (kms->atomic_pending) {
		char buf[256];
		ssize_t len;
		char *pos;
		int ret;

		ret = poll(&pfd, 1, timeout_ms);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (!ret)
			return -ETIMEDOUT;

		len = read(kms->fd, buf, sizeof(buf));
		if (len < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -errno;
		}

		pos = buf;
		while (len >= (ssize_t)sizeof(struct drm_event)) {
			struct drm_event *event = (struct drm_event *)pos;

			if (!event->length || event->length > (uint32_t)len)
				return -EINVAL;

			if (event->type == DRM_EVENT_FLIP_COMPLETE &&
			    event->length >= sizeof(struct drm_event_vblank)) {
				struct drm_event_vblank *vblank =
					(struct drm_event_vblank *)event;

				if (vblank->user_data == pending_seq)
					kms->atomic_pending = false;
			}

			pos += event->length;
			len -= event->length;
		}
	}

	return 0;
}

static void calc_plane_rect(const struct drm_mode_modeinfo *mode,
			    uint32_t width, uint32_t height,
			    uint32_t *dst_x, uint32_t *dst_y,
			    uint32_t *dst_w, uint32_t *dst_h)
{
	*dst_w = mode->hdisplay;
	*dst_h = (uint64_t)height * *dst_w / width;

	if (*dst_h > mode->vdisplay) {
		*dst_h = mode->vdisplay;
		*dst_w = (uint64_t)width * *dst_h / height;
	}

	*dst_x = (mode->hdisplay - *dst_w) / 2;
	*dst_y = (mode->vdisplay - *dst_h) / 2;
}

static bool rotation_swaps_axes(uint32_t rotation)
{
	return rotation == DRM_MODE_ROTATE_90 ||
	       rotation == DRM_MODE_ROTATE_270;
}

static void calc_kms_plane_rect(const struct lite_kms *kms,
				uint32_t *dst_x, uint32_t *dst_y,
				uint32_t *dst_w, uint32_t *dst_h)
{
	uint32_t width = kms->width;
	uint32_t height = kms->height;

	if (rotation_swaps_axes(kms->rotation)) {
		width = kms->height;
		height = kms->width;
	}

	calc_plane_rect(&kms->ids.mode, width, height,
			dst_x, dst_y, dst_w, dst_h);
}

static bool crop_src_center_even(uint32_t base_w, uint32_t base_h,
				 uint32_t crop_w, uint32_t crop_h,
				 uint32_t *src_x, uint32_t *src_y,
				 uint32_t *src_w, uint32_t *src_h)
{
	uint32_t crop_x, crop_y;
	uint32_t max_x, max_y;

	if (base_w < crop_w || base_h < crop_h)
		return false;

	max_x = base_w - crop_w;
	max_y = base_h - crop_h;
	crop_x = align_up_u32(max_x / 2, TILE_W);
	crop_y = align_up_u32(max_y / 2, TILE_H);
	if (crop_x > max_x)
		crop_x = align_down_u32(max_x, TILE_W);
	if (crop_y > max_y)
		crop_y = align_down_u32(max_y, TILE_H);

	*src_x += align_down_u32(crop_x, 2);
	*src_y += align_down_u32(crop_y, 2);
	*src_w = crop_w;
	*src_h = crop_h;
	return true;
}

static void calc_kms_src_dst_rect(const struct lite_kms *kms,
				  uint32_t *src_x, uint32_t *src_y,
				  uint32_t *src_w, uint32_t *src_h,
				  uint32_t *dst_x, uint32_t *dst_y,
				  uint32_t *dst_w, uint32_t *dst_h)
{
	if (!kms->crop_no_scale) {
		calc_kms_plane_rect(kms, dst_x, dst_y, dst_w, dst_h);
		*src_x = kms->src_x;
		*src_y = kms->src_y;
		*src_w = kms->src_width;
		*src_h = kms->src_height;
		return;
	}

	*dst_w = kms->ids.mode.hdisplay;
	*dst_h = kms->ids.mode.vdisplay;
	if (*dst_w > kms->src_width)
		*dst_w = kms->src_width;
	if (*dst_h > kms->src_height)
		*dst_h = kms->src_height;

	*dst_x = (kms->ids.mode.hdisplay - *dst_w) / 2;
	*dst_y = (kms->ids.mode.vdisplay - *dst_h) / 2;
	*src_x = kms->src_x;
	*src_y = kms->src_y;
	*src_w = *dst_w;
	*src_h = *dst_h;
}

static uint32_t auto_plane_rotation(const struct drm_mode_modeinfo *mode,
				    uint32_t width, uint32_t height,
				    bool atomic_ready,
				    uint32_t rotation_prop)
{
	bool video_landscape = width > height;
	bool mode_landscape = mode->hdisplay > mode->vdisplay;

	if (!atomic_ready || !rotation_prop)
		return DRM_MODE_ROTATE_0;
	if (video_landscape == mode_landscape)
		return DRM_MODE_ROTATE_0;

	return DRM_MODE_ROTATE_90;
}

static const char *rotation_name(uint32_t rotation)
{
	switch (rotation) {
	case DRM_MODE_ROTATE_90:
		return "rotate-90";
	case DRM_MODE_ROTATE_180:
		return "rotate-180";
	case DRM_MODE_ROTATE_270:
		return "rotate-270";
	default:
		return "none";
	}
}

static int parse_ulong_range(const char *arg, unsigned long min,
			     unsigned long max, unsigned long *out)
{
	char *end = NULL;
	unsigned long val;

	errno = 0;
	val = strtoul(arg, &end, 0);
	if (errno || !end || *end || val < min || val > max)
		return -1;

	*out = val;
	return 0;
}

static int atomic_set_plane(struct lite_kms *kms, uint32_t fb_id)
{
	uint64_t seq = kms->atomic_seq + 1;
	uint32_t objs[1] = { kms->ids.plane_id };
	bool full_update = !kms->atomic_plane_configured ||
			   kms->atomic_width != kms->width ||
			   kms->atomic_height != kms->height ||
			   kms->atomic_rotation != kms->rotation;
	uint32_t count_props[1] = { full_update ? 10 : 1 };
	uint32_t props[11];
	uint64_t values[11];
	struct drm_mode_atomic atomic = {
		.flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT,
		.count_objs = ARRAY_SIZE(objs),
		.objs_ptr = ptr_to_u64(objs),
		.count_props_ptr = ptr_to_u64(count_props),
		.props_ptr = ptr_to_u64(props),
		.prop_values_ptr = ptr_to_u64(values),
		.user_data = seq,
	};
	uint32_t src_x, src_y, src_w, src_h;
	uint32_t dst_x, dst_y, dst_w, dst_h;
	uint32_t idx = 0;
	int ret;

	if (kms->atomic_pending) {
		ret = wait_atomic_event(kms, 100);
		if (ret)
			return ret;
	}

	if (full_update) {
		calc_kms_src_dst_rect(kms, &src_x, &src_y, &src_w, &src_h,
				      &dst_x, &dst_y, &dst_w, &dst_h);

		props[idx] = kms->plane_props.crtc_id;
		values[idx++] = kms->ids.crtc_id;
		props[idx] = kms->plane_props.fb_id;
		values[idx++] = fb_id;
		props[idx] = kms->plane_props.crtc_x;
		values[idx++] = dst_x;
		props[idx] = kms->plane_props.crtc_y;
		values[idx++] = dst_y;
		props[idx] = kms->plane_props.crtc_w;
		values[idx++] = dst_w;
		props[idx] = kms->plane_props.crtc_h;
		values[idx++] = dst_h;
		props[idx] = kms->plane_props.src_x;
		values[idx++] = (uint64_t)src_x << 16;
		props[idx] = kms->plane_props.src_y;
		values[idx++] = (uint64_t)src_y << 16;
		props[idx] = kms->plane_props.src_w;
		values[idx++] = (uint64_t)src_w << 16;
		props[idx] = kms->plane_props.src_h;
		values[idx++] = (uint64_t)src_h << 16;
		if (kms->plane_props.rotation) {
			props[idx] = kms->plane_props.rotation;
			values[idx++] = kms->rotation;
		}
		count_props[0] = idx;
	} else {
		props[0] = kms->plane_props.fb_id;
		values[0] = fb_id;
	}

	if (ioctl(kms->fd, DRM_IOCTL_MODE_ATOMIC, &atomic) < 0)
		return -errno;

	kms->atomic_seq = seq;
	kms->atomic_pending = true;
	kms->atomic_plane_configured = true;
	kms->atomic_width = kms->width;
	kms->atomic_height = kms->height;
	kms->atomic_rotation = kms->rotation;
	return 0;
}

static int set_plane(struct lite_kms *kms, uint32_t fb_id)
{
	uint32_t src_x, src_y, src_w, src_h;
	uint32_t dst_x, dst_y, dst_w, dst_h;
	struct drm_mode_set_plane plane;

	calc_kms_src_dst_rect(kms, &src_x, &src_y, &src_w, &src_h,
			      &dst_x, &dst_y, &dst_w, &dst_h);

	memset(&plane, 0, sizeof(plane));
	plane.plane_id = kms->ids.plane_id;
	plane.crtc_id = kms->ids.crtc_id;
	plane.fb_id = fb_id;
	plane.crtc_x = dst_x;
	plane.crtc_y = dst_y;
	plane.crtc_w = dst_w;
	plane.crtc_h = dst_h;
	plane.src_x = src_x << 16;
	plane.src_y = src_y << 16;
	plane.src_w = src_w << 16;
	plane.src_h = src_h << 16;

	return drm_ioctl(kms->fd, DRM_IOCTL_MODE_SETPLANE, &plane,
			 "DRM_IOCTL_MODE_SETPLANE");
}

static int commit_plane(struct lite_kms *kms, uint32_t fb_id)
{
	if (kms->atomic_ready) {
		int ret = atomic_set_plane(kms, fb_id);

		if (!ret)
			return 0;

		if (!kms->printed_atomic_fallback) {
			fprintf(stderr,
				"atomic commit failed, using legacy SETPLANE: %s\n",
				strerror(-ret));
			kms->printed_atomic_fallback = true;
		}
		kms->atomic_ready = false;
		kms->rotation = DRM_MODE_ROTATE_0;
	}

	return set_plane(kms, fb_id);
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [OPTIONS] FILE_OR_DIR\n", prog);
	fprintf(stderr, "Default playback auto-selects video, audio, or A/V from the input.\n");
	fprintf(stderr, "\nPlayback:\n");
	fprintf(stderr, "  -K  video only through KMS/Cedrus\n");
	fprintf(stderr, "  -E  audio only; combine with -K for explicit A/V playback\n");
	fprintf(stderr, "  -l  loop the selected file or directory playlist\n");
	fprintf(stderr, "  -B  benchmark/no frame pacing; with audio, decode only without ALSA output\n");
	fprintf(stderr, "  -Z  crop source to panel size and disable DE scaling\n");
	fprintf(stderr, "  -U  print runtime FPS/CPU/RSS/timing once per second\n");
	fprintf(stderr, "\nAdvanced options:\n");
	fprintf(stderr, "  -P -L -X backend\n");
	fprintf(stderr, "  -p plane -o pcm\n");
}

static int parse_audio_backend(const char *arg, enum audio_backend *out)
{
	if (!strcmp(arg, "auto") || !strcmp(arg, "default")) {
		*out = AUDIO_BACKEND_AUTO;
		return 0;
	}
	if (!strcmp(arg, "ffmpeg") || !strcmp(arg, "avcodec") ||
	    !strcmp(arg, "ffmpeg-fixed") || !strcmp(arg, "aac_fixed") ||
	    !strcmp(arg, "avcodec-fixed")) {
		*out = AUDIO_BACKEND_FFMPEG_FIXED;
		return 0;
	}
	if (!strcmp(arg, "ffmpeg-float") || !strcmp(arg, "avcodec-float")) {
		*out = AUDIO_BACKEND_FFMPEG;
		return 0;
	}

	fprintf(stderr, "unknown audio backend: %s\n", arg);
	return -1;
}

static int parse_args(int argc, char **argv, struct options *opt)
{
	bool explicit_mode = false;
	int c;

	opt->video_dev = "/dev/video0";
	opt->drm_card = "/dev/dri/card0";
	opt->pcm_dev = "hw:0,0";
	opt->audio_backend = AUDIO_BACKEND_FFMPEG_FIXED;
	opt->atomic = true;

	while ((c = getopt(argc, argv, "PUKBEZLo:X:p:lh")) != -1) {
		switch (c) {
		case 'P':
			opt->profile = true;
			break;
		case 'U':
			opt->usage_report = true;
			break;
		case 'K':
			explicit_mode = true;
			opt->display = true;
			break;
		case 'E':
			explicit_mode = true;
			opt->audio_play = true;
			break;
		case 'B':
			opt->no_pace = true;
			break;
		case 'l':
			opt->loop = true;
			break;
		case 'Z':
			opt->crop_no_scale = true;
			opt->display = true;
			break;
		case 'L':
			opt->atomic = false;
			break;
		case 'o':
			opt->pcm_dev = optarg;
			break;
		case 'X':
			if (parse_audio_backend(optarg, &opt->audio_backend))
				return -1;
			break;
		case 'p': {
			unsigned long plane_id;

			if (parse_ulong_range(optarg, 0, UINT32_MAX, &plane_id))
				return -1;
			opt->plane_id = (uint32_t)plane_id;
			break;
		}
		case 'h':
		default:
			return -1;
		}
	}

	if (optind + 1 != argc)
		return -1;

	if (!explicit_mode)
		opt->auto_play = true;

	opt->input = argv[optind];
	return 0;
}

static bool media_path_supported(const char *path)
{
	static const char * const suffixes[] = {
		".avi", ".mkv", ".flv", ".ts", ".mp4", ".m4v", ".webm",
		".asf", ".mov", ".mp1", ".mp2", ".mp3", ".ogg", ".flac",
		".ape", ".wav", ".m4a", ".amr", ".aac", ".opus",
	};
	const char *dot = strrchr(path, '.');
	unsigned int i;

	if (!dot)
		return false;

	for (i = 0; i < ARRAY_SIZE(suffixes); i++)
		if (!strcasecmp(dot, suffixes[i]))
			return true;

	return false;
}

static void playlist_free(struct playlist *list)
{
	size_t i;

	for (i = 0; i < list->count; i++)
		free(list->items[i]);
	free(list->items);
	memset(list, 0, sizeof(*list));
}

static int playlist_add(struct playlist *list, const char *path)
{
	char **items;
	char *copy;

	if (list->count == list->capacity) {
		size_t capacity = list->capacity ? list->capacity * 2 : 16;

		items = realloc(list->items, capacity * sizeof(*items));
		if (!items)
			return -1;
		list->items = items;
		list->capacity = capacity;
	}

	copy = strdup(path);
	if (!copy)
		return -1;

	list->items[list->count++] = copy;
	return 0;
}

static int playlist_cmp(const void *a, const void *b)
{
	const char * const *pa = a;
	const char * const *pb = b;

	return strcmp(*pa, *pb);
}

static int playlist_build_file(struct playlist *list, const char *path)
{
	if (!media_path_supported(path))
		fprintf(stderr, "warning: trying unsupported suffix: %s\n", path);

	return playlist_add(list, path);
}

static int playlist_build_dir(struct playlist *list, const char *path)
{
	DIR *dir;
	struct dirent *de;

	dir = opendir(path);
	if (!dir) {
		fprintf(stderr, "opendir %s failed: %s\n", path, strerror(errno));
		return -1;
	}

	while ((de = readdir(dir))) {
		struct stat st;
		char full[PATH_MAX];
		int n;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (!media_path_supported(de->d_name))
			continue;

		n = snprintf(full, sizeof(full), "%s%s%s", path,
			     path[0] && path[strlen(path) - 1] == '/' ? "" : "/",
			     de->d_name);
		if (n < 0 || (size_t)n >= sizeof(full)) {
			fprintf(stderr, "playlist path too long: %s/%s\n",
				path, de->d_name);
			closedir(dir);
			return -1;
		}

		if (stat(full, &st) || !S_ISREG(st.st_mode))
			continue;
		if (playlist_add(list, full)) {
			closedir(dir);
			return -1;
		}
	}

	closedir(dir);
	qsort(list->items, list->count, sizeof(*list->items), playlist_cmp);
	return list->count ? 0 : -1;
}

static int playlist_build(struct playlist *list, const char *path)
{
	struct stat st;

	if (stat(path, &st)) {
		fprintf(stderr, "stat %s failed: %s\n", path, strerror(errno));
		return -1;
	}

	if (S_ISDIR(st.st_mode))
		return playlist_build_dir(list, path);
	if (S_ISREG(st.st_mode))
		return playlist_build_file(list, path);

	fprintf(stderr, "unsupported input type: %s\n", path);
	return -1;
}

static int map_file_ro(const char *path, uint8_t **data, size_t *size)
{
	struct stat st;
	void *map;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror(path);
		return -1;
	}

	if (fstat(fd, &st) || st.st_size <= 0) {
		perror("fstat");
		close(fd);
		return -1;
	}

	if ((uintmax_t)st.st_size > SIZE_MAX) {
		fprintf(stderr, "%s too large to map on this target: %ju bytes\n",
			path, (uintmax_t)st.st_size);
		close(fd);
		return -1;
	}

	map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap %s size=%ju failed: %s\n",
			path, (uintmax_t)st.st_size, strerror(errno));
		return -1;
	}

	*data = map;
	*size = (size_t)st.st_size;
	return 0;
}

static void warm_file_pages(const uint8_t *file, size_t size)
{
	volatile uint8_t sink = 0;
	uint64_t start = now_us();
	size_t page = 4096;
	size_t off;

	madvise((void *)file, size, MADV_SEQUENTIAL);
	madvise((void *)file, size, MADV_WILLNEED);

	for (off = 0; off < size; off += page)
		sink ^= file[off];
	if (size)
		sink ^= file[size - 1];

	fprintf(stderr, "warm file pages size=%zu elapsed_us=%" PRIu64 "\n",
		size, now_us() - start);
	(void)sink;
}

static uint64_t readahead_file_window(const uint8_t *file, size_t file_size,
				      uint64_t offset, size_t window)
{
	static bool warned;
	long page = sysconf(_SC_PAGESIZE);
	uint64_t start, end;
	size_t len;

	if (page <= 0)
		page = 4096;
	if (offset >= file_size || !window)
		return offset;

	start = offset / (uint64_t)page * (uint64_t)page;
	end = offset + window;
	if (end < offset || end > file_size)
		end = file_size;
	len = (size_t)(end - start);
	if (!len)
		return end;

	if (madvise((void *)(file + start), len, MADV_WILLNEED) && !warned) {
		fprintf(stderr, "readahead madvise failed: %s\n",
			strerror(errno));
		warned = true;
	}

	return end;
}

static uint64_t discard_file_window(const uint8_t *file, size_t file_size,
				    uint64_t start, uint64_t end)
{
	static bool warned;
	long page = sysconf(_SC_PAGESIZE);
	size_t len;

	if (page <= 0)
		page = 4096;
	if (start >= end || start >= file_size)
		return start;
	if (end > file_size)
		end = file_size;

	start = (start + (uint64_t)page - 1) / (uint64_t)page *
		(uint64_t)page;
	end = end / (uint64_t)page * (uint64_t)page;
	if (start >= end)
		return start;

	len = (size_t)(end - start);
	if (madvise((void *)(file + start), len, MADV_DONTNEED) && !warned) {
		fprintf(stderr, "discard madvise failed: %s\n",
			strerror(errno));
		warned = true;
	}

	return end;
}

static bool atom_payload(const uint8_t *file, size_t file_size, uint64_t off,
			 uint64_t end, uint64_t *payload, uint64_t *payload_end,
			 const uint8_t **type)
{
	uint64_t size;
	uint64_t header = 8;

	if (off + 8 > end || off + 8 > file_size)
		return false;

	size = rd32(file + off);
	*type = file + off + 4;
	if (size == 1) {
		if (off + 16 > end || off + 16 > file_size)
			return false;
		size = rd64(file + off + 8);
		header = 16;
	} else if (size == 0) {
		size = end - off;
	}

	if (size < header || off + size > end || off + size > file_size)
		return false;

	*payload = off + header;
	*payload_end = off + size;
	return true;
}

struct bit_reader {
	const uint8_t *data;
	size_t bits;
	size_t pos;
};

static bool bits_get(struct bit_reader *br, unsigned int n, uint32_t *out)
{
	uint32_t v = 0;
	unsigned int i;

	if (n > 32 || br->pos + n > br->bits)
		return false;

	for (i = 0; i < n; i++) {
		size_t bit = br->pos++;

		v <<= 1;
		v |= (br->data[bit / 8] >> (7 - (bit % 8))) & 1;
	}

	*out = v;
	return true;
}

static bool bits_get_aac_object_type(struct bit_reader *br, uint8_t *out)
{
	uint32_t v;

	if (!bits_get(br, 5, &v))
		return false;
	if (v == 31) {
		uint32_t ext;

		if (!bits_get(br, 6, &ext))
			return false;
		v = 32 + ext;
	}

	*out = v > UINT8_MAX ? UINT8_MAX : (uint8_t)v;
	return true;
}

static const uint32_t aac_sample_rates[] = {
	96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
	16000, 12000, 11025, 8000, 7350,
};

static const char *aac_object_name(uint8_t object_type)
{
	switch (object_type) {
	case 1:
		return "Main";
	case 2:
		return "LC";
	case 3:
		return "SSR";
	case 4:
		return "LTP";
	case 5:
		return "SBR";
	case 29:
		return "PS";
	default:
		return "unknown";
	}
}

static bool bits_get_aac_sample_rate(struct bit_reader *br, uint8_t *index,
				     uint32_t *rate)
{
	uint32_t v;

	if (!bits_get(br, 4, &v))
		return false;

	*index = v;
	if (v == 15)
		return bits_get(br, 24, rate);

	*rate = v < ARRAY_SIZE(aac_sample_rates) ? aac_sample_rates[v] : 0;
	return true;
}

static int parse_aac_audio_specific_config(struct video_track *t,
					   const uint8_t *p, size_t size)
{
	struct bit_reader br = {
		.data = p,
		.bits = size * 8,
	};
	uint32_t channels;

	if (!size)
		return -1;

	t->audio_specific_config = (struct blob) { p, size };

	if (!bits_get_aac_object_type(&br, &t->aac_object_type))
		return -1;
	if (!bits_get_aac_sample_rate(&br, &t->aac_sample_rate_index,
				      &t->aac_sample_rate))
		return -1;
	if (!bits_get(&br, 4, &channels))
		return -1;
	t->aac_channel_config = channels > UINT8_MAX ? UINT8_MAX : channels;

	if (t->aac_object_type == 5 || t->aac_object_type == 29) {
		uint8_t ext_rate_index;

		if (!bits_get_aac_sample_rate(&br, &ext_rate_index,
					      &t->aac_ext_sample_rate))
			return -1;
		if (!bits_get_aac_object_type(&br, &t->aac_ext_object_type))
			return -1;
	}

	return 0;
}

static int desc_header(const uint8_t *p, size_t size, size_t *off,
		       uint8_t *tag, size_t *len)
{
	size_t l = 0;
	unsigned int i;

	if (*off >= size)
		return -1;

	*tag = p[(*off)++];
	for (i = 0; i < 4; i++) {
		uint8_t b;

		if (*off >= size)
			return -1;
		b = p[(*off)++];
		l = (l << 7) | (b & 0x7f);
		if (!(b & 0x80)) {
			if (*off + l > size)
				return -1;
			*len = l;
			return 0;
		}
	}

	return -1;
}

static int parse_decoder_config_desc(struct video_track *t,
				     const uint8_t *p, size_t size)
{
	size_t off = 13;

	if (size < off)
		return -1;

	t->esds_object_type = p[0];
	t->esds_stream_type = p[1] >> 2;
	t->esds_buffer_size = ((uint32_t)p[2] << 16) |
			      ((uint32_t)p[3] << 8) | p[4];
	t->esds_max_bitrate = rd32(p + 5);
	t->esds_avg_bitrate = rd32(p + 9);

	while (off < size) {
		uint8_t tag;
		size_t len;
		size_t payload;

		if (desc_header(p, size, &off, &tag, &len))
			return -1;
		payload = off;
		if (tag == 0x05) {
			if (parse_aac_audio_specific_config(t, p + payload, len))
				return -1;
		}
		off = payload + len;
	}

	return 0;
}

static int parse_esds(struct video_track *t, const uint8_t *p, size_t size)
{
	size_t off = 4;

	if (size < off)
		return -1;

	t->has_esds = true;

	while (off < size) {
		uint8_t tag;
		size_t len;
		size_t payload;
		size_t end;

		if (desc_header(p, size, &off, &tag, &len))
			return -1;
		payload = off;
		end = payload + len;

		if (tag == 0x03) {
			uint8_t flags;
			size_t child = payload + 3;

			if (payload + 3 > end)
				return -1;
			flags = p[payload + 2];
			if (flags & 0x80)
				child += 2;
			if (flags & 0x40) {
				if (child >= end || child + 1 + p[child] > end)
					return -1;
				child += 1 + p[child];
			}
			if (flags & 0x20)
				child += 2;
			if (child > end)
				return -1;

			while (child < end) {
				uint8_t tag2;
				size_t len2;
				size_t payload2;

				if (desc_header(p, end, &child, &tag2, &len2))
					return -1;
				payload2 = child;
				if (tag2 == 0x04 &&
				    parse_decoder_config_desc(t, p + payload2,
							      len2))
					return -1;
				child = payload2 + len2;
			}
		} else if (tag == 0x04) {
			if (parse_decoder_config_desc(t, p + payload, len))
				return -1;
		} else if (tag == 0x05) {
			if (parse_aac_audio_specific_config(t, p + payload, len))
				return -1;
		}

		off = end;
	}

	return 0;
}

static int parse_avcc(struct video_track *t, const uint8_t *p, size_t size)
{
	unsigned int i;
	uint8_t sps_count;
	uint8_t pps_count;
	size_t off;

	if (size < 7)
		return -1;

	t->h264_format = H264_BITSTREAM_AVCC;
	t->nal_length_size = (p[4] & 0x03) + 1;
	sps_count = p[5] & 0x1f;
	off = 6;

	for (i = 0; i < sps_count; i++) {
		uint16_t len;

		if (off + 2 > size || t->num_sps >= ARRAY_SIZE(t->sps))
			return -1;
		len = rd16(p + off);
		off += 2;
		if (off + len > size)
			return -1;
		t->sps[t->num_sps++] = (struct blob) { p + off, len };
		off += len;
	}

	if (off + 1 > size)
		return -1;

	pps_count = p[off++];
	for (i = 0; i < pps_count; i++) {
		uint16_t len;

		if (off + 2 > size || t->num_pps >= ARRAY_SIZE(t->pps))
			return -1;
		len = rd16(p + off);
		off += 2;
		if (off + len > size)
			return -1;
		t->pps[t->num_pps++] = (struct blob) { p + off, len };
		off += len;
	}

	return t->nal_length_size >= 1 && t->nal_length_size <= 4 ? 0 : -1;
}

static int h264_add_parameter_set(struct video_track *t, const uint8_t *data,
				  size_t size)
{
	uint8_t type;

	if (!size)
		return -1;

	type = data[0] & 0x1f;
	if (type == GST_H264_NAL_SPS) {
		if (t->num_sps >= ARRAY_SIZE(t->sps))
			return -1;
		t->sps[t->num_sps++] = (struct blob) { data, size };
		return 0;
	}
	if (type == GST_H264_NAL_PPS) {
		if (t->num_pps >= ARRAY_SIZE(t->pps))
			return -1;
		t->pps[t->num_pps++] = (struct blob) { data, size };
		return 0;
	}

	return 0;
}

static int parse_h264_annexb_extradata(struct video_track *t,
				       const uint8_t *data, size_t size)
{
	GstH264NalParser *parser;
	uint32_t off = 0;
	int ret = -1;

	parser = gst_h264_nal_parser_new();
	if (!parser)
		return -1;

	while (off < size) {
		GstH264NalUnit nalu;
		GstH264ParserResult pres;

		pres = gst_h264_parser_identify_nalu(parser, data, off, size,
						     &nalu);
		if (pres != GST_H264_PARSER_OK)
			break;
		if (h264_add_parameter_set(t, nalu.data + nalu.offset,
					   nalu.size))
			goto out;
		off = nalu.offset + nalu.size;
	}

	ret = t->num_sps && t->num_pps ? 0 : -1;
out:
	gst_h264_nal_parser_free(parser);
	return ret;
}

static int parse_h264_extradata(struct video_track *t,
				const uint8_t *data, size_t size)
{
	if (!data || !size)
		return -1;

	if (size >= 7 && data[0] == 1)
		return parse_avcc(t, data, size);

	t->h264_format = H264_BITSTREAM_ANNEXB;
	t->nal_length_size = 0;
	return parse_h264_annexb_extradata(t, data, size);
}

static const uint8_t *track_sample_data(const uint8_t *file,
					const struct video_track *t,
					uint32_t sample)
{
	if (t->sample_data)
		return t->sample_data[sample];
	return file + t->samples[sample].offset;
}

static GstH264ParserResult identify_h264_nalu(GstH264NalParser *parser,
					      const struct video_track *t,
					      const uint8_t *data,
					      uint32_t offset, uint32_t size,
					      GstH264NalUnit *nalu)
{
	if (t->h264_format == H264_BITSTREAM_ANNEXB)
		return gst_h264_parser_identify_nalu(parser, data, offset,
						     size, nalu);

	return gst_h264_parser_identify_nalu_avc(parser, data, offset, size,
						 t->nal_length_size, nalu);
}

static int parse_stsd(struct video_track *t, const uint8_t *file,
		      uint64_t payload, uint64_t end)
{
	uint32_t entries;
	uint64_t off;

	if (payload + 8 > end)
		return -1;

	entries = rd32(file + payload + 4);
	off = payload + 8;

	while (entries-- && off + 8 <= end) {
		uint32_t entry_size = rd32(file + off);
		const uint8_t *entry_type = file + off + 4;
		uint64_t entry_end = off + entry_size;
		uint64_t child;

		if (entry_size < 8 || entry_end > end)
			return -1;

		if (fourcc_is(entry_type, "mp4a")) {
			uint16_t version;
			uint32_t sample_rate_fixed;

			if (off + 8 + 28 > entry_end)
				return -1;

			t->is_audio = true;
			(void)fourcc_str(entry_type, t->audio_format);
			version = rd16(file + off + 16);
			t->audio_channels = rd16(file + off + 24);
			t->audio_sample_size = rd16(file + off + 26);
			sample_rate_fixed = rd32(file + off + 32);
			t->audio_sample_rate = sample_rate_fixed >> 16;

			child = off + 8 + 28;
			if (version == 1)
				child += 16;

			while (child + 8 <= entry_end) {
				uint64_t payload2, end2;
				const uint8_t *type2;

				if (!atom_payload(file, entry_end, child, entry_end,
						  &payload2, &end2, &type2))
					return -1;
				if (fourcc_is(type2, "esds") &&
				    parse_esds(t, file + payload2, end2 - payload2))
					return -1;
				child = end2;
			}

			off = entry_end;
			continue;
		}

		if (!fourcc_is(entry_type, "avc1") && !fourcc_is(entry_type, "avc3")) {
			off = entry_end;
			continue;
		}

		if (off + 8 + 78 > entry_end)
			return -1;

		t->width = rd16(file + off + 8 + 24);
		t->height = rd16(file + off + 8 + 26);

		child = off + 8 + 78;
		while (child + 8 <= entry_end) {
			uint64_t payload2, end2;
			const uint8_t *type2;

			if (!atom_payload(file, entry_end, child, entry_end,
					  &payload2, &end2, &type2))
				return -1;
			if (fourcc_is(type2, "avcC"))
				return parse_avcc(t, file + payload2, end2 - payload2);
			child = end2;
		}
		off = entry_end;
	}

	return 0;
}

static int parse_stts(struct video_track *t, const uint8_t *p, size_t size)
{
	uint32_t count;
	uint32_t i;

	if (size < 8)
		return -1;

	count = rd32(p + 4);
	if (size < 8 + (size_t)count * 8)
		return -1;

	t->stts = calloc(count, sizeof(*t->stts));
	if (!t->stts)
		return -1;
	t->stts_count = count;

	for (i = 0; i < count; i++) {
		t->stts[i].count = rd32(p + 8 + i * 8);
		t->stts[i].delta = rd32(p + 12 + i * 8);
	}

	return 0;
}

static int parse_ctts(struct video_track *t, const uint8_t *p, size_t size)
{
	uint32_t count;
	uint32_t i;
	uint8_t version;

	if (size < 8)
		return -1;

	version = p[0];
	if (version > 1)
		return -1;

	count = rd32(p + 4);
	if (size < 8 + (size_t)count * 8)
		return -1;

	t->ctts = calloc(count, sizeof(*t->ctts));
	if (!t->ctts)
		return -1;
	t->ctts_count = count;

	for (i = 0; i < count; i++) {
		t->ctts[i].count = rd32(p + 8 + i * 8);
		t->ctts[i].offset = version ?
				     (int32_t)rd32(p + 12 + i * 8) :
				     (int64_t)rd32(p + 12 + i * 8);
	}

	return 0;
}

static int parse_stsc(struct video_track *t, const uint8_t *p, size_t size)
{
	uint32_t count;
	uint32_t i;

	if (size < 8)
		return -1;

	count = rd32(p + 4);
	if (size < 8 + (size_t)count * 12)
		return -1;

	t->stsc = calloc(count, sizeof(*t->stsc));
	if (!t->stsc)
		return -1;
	t->stsc_count = count;

	for (i = 0; i < count; i++) {
		t->stsc[i].first_chunk = rd32(p + 8 + i * 12);
		t->stsc[i].samples_per_chunk = rd32(p + 12 + i * 12);
		t->stsc[i].sample_description_index = rd32(p + 16 + i * 12);
	}

	return 0;
}

static int parse_stsz(struct video_track *t, const uint8_t *p, size_t size)
{
	uint32_t i;

	if (size < 12)
		return -1;

	t->default_sample_size = rd32(p + 4);
	t->sample_count = rd32(p + 8);

	if (t->default_sample_size)
		return 0;

	if (size < 12 + (size_t)t->sample_count * 4)
		return -1;

	t->sample_sizes = calloc(t->sample_count, sizeof(*t->sample_sizes));
	if (!t->sample_sizes)
		return -1;

	for (i = 0; i < t->sample_count; i++)
		t->sample_sizes[i] = rd32(p + 12 + i * 4);

	return 0;
}

static int parse_stco(struct video_track *t, const uint8_t *p, size_t size,
		      bool co64)
{
	uint32_t count;
	uint32_t i;
	size_t step = co64 ? 8 : 4;

	if (size < 8)
		return -1;

	count = rd32(p + 4);
	if (size < 8 + (size_t)count * step)
		return -1;

	t->chunk_offsets = calloc(count, sizeof(*t->chunk_offsets));
	if (!t->chunk_offsets)
		return -1;
	t->chunk_count = count;

	for (i = 0; i < count; i++)
		t->chunk_offsets[i] = co64 ? rd64(p + 8 + i * 8) :
					     rd32(p + 8 + i * 4);

	return 0;
}

static int parse_stss(struct video_track *t, const uint8_t *p, size_t size)
{
	uint32_t count;
	uint32_t i;

	if (size < 8)
		return -1;

	count = rd32(p + 4);
	if (size < 8 + (size_t)count * 4)
		return -1;

	t->sync_samples = calloc(count, sizeof(*t->sync_samples));
	if (!t->sync_samples)
		return -1;
	t->sync_count = count;

	for (i = 0; i < count; i++)
		t->sync_samples[i] = rd32(p + 8 + i * 4);

	return 0;
}

static void free_track(struct video_track *t);

static int parse_atoms(const uint8_t *file, size_t file_size, uint64_t start,
		       uint64_t end, struct video_track *cur,
		       struct video_track *out_video,
		       struct video_track *out_audio, unsigned int depth)
{
	uint64_t off = start;

	while (off + 8 <= end) {
		uint64_t payload, payload_end;
		const uint8_t *type;
		char type_buf[5];

		if (!atom_payload(file, file_size, off, end, &payload,
				  &payload_end, &type))
			return -1;

		if (fourcc_is(type, "trak")) {
			struct video_track track = { 0 };
			bool keep = false;

			if (parse_atoms(file, file_size, payload, payload_end,
					&track, out_video, out_audio, depth + 1))
				return -1;
			if (track.is_video && track.nal_length_size &&
			    !out_video->is_video) {
				*out_video = track;
				out_video->is_video = true;
				keep = true;
			} else if (track.is_audio && !out_audio->is_audio) {
				*out_audio = track;
				out_audio->is_audio = true;
				keep = true;
			}
			if (!keep)
				free_track(&track);
		} else if (cur && fourcc_is(type, "hdlr")) {
			if (payload + 12 <= payload_end) {
				if (fourcc_is(file + payload + 8, "vide"))
					cur->is_video = true;
				else if (fourcc_is(file + payload + 8, "soun"))
					cur->is_audio = true;
			}
		} else if (cur && fourcc_is(type, "mdhd")) {
			uint8_t version;

			if (payload + 4 > payload_end)
				return -1;
			version = file[payload];
			if (!version && payload + 20 <= payload_end)
				cur->timescale = rd32(file + payload + 12);
			else if (version == 1 && payload + 32 <= payload_end)
				cur->timescale = rd32(file + payload + 20);
		} else if (cur && fourcc_is(type, "stsd")) {
			if (parse_stsd(cur, file, payload, payload_end))
				return -1;
		} else if (cur && fourcc_is(type, "stts")) {
			if (parse_stts(cur, file + payload, payload_end - payload))
				return -1;
		} else if (cur && fourcc_is(type, "ctts")) {
			if (parse_ctts(cur, file + payload, payload_end - payload))
				return -1;
		} else if (cur && fourcc_is(type, "stsc")) {
			if (parse_stsc(cur, file + payload, payload_end - payload))
				return -1;
		} else if (cur && fourcc_is(type, "stsz")) {
			if (parse_stsz(cur, file + payload, payload_end - payload))
				return -1;
		} else if (cur && fourcc_is(type, "stco")) {
			if (parse_stco(cur, file + payload, payload_end - payload,
				       false))
				return -1;
		} else if (cur && fourcc_is(type, "co64")) {
			if (parse_stco(cur, file + payload, payload_end - payload,
				       true))
				return -1;
		} else if (cur && fourcc_is(type, "stss")) {
			if (parse_stss(cur, file + payload, payload_end - payload))
				return -1;
		} else if (fourcc_is(type, "moov") || fourcc_is(type, "mdia") ||
			   fourcc_is(type, "minf") || fourcc_is(type, "stbl") ||
			   fourcc_is(type, "edts") || fourcc_is(type, "dinf")) {
			if (parse_atoms(file, file_size, payload, payload_end,
					cur, out_video, out_audio, depth + 1))
				return -1;
		} else {
			(void)fourcc_str(type, type_buf);
			(void)depth;
		}

		off = payload_end;
	}

	return 0;
}

static uint32_t sample_size(const struct video_track *t, uint32_t sample)
{
	return t->default_sample_size ? t->default_sample_size :
					t->sample_sizes[sample];
}

static bool sample_is_sync(const struct video_track *t, uint32_t sample)
{
	uint32_t one_based = sample + 1;
	uint32_t i;

	if (!t->sync_count)
		return true;

	for (i = 0; i < t->sync_count; i++)
		if (t->sync_samples[i] == one_based)
			return true;

	return false;
}

static uint64_t ticks_to_us(uint64_t ticks, uint32_t timescale)
{
	uint64_t q;
	uint64_t r;

	if (!timescale)
		return 0;

	q = ticks / timescale;
	r = ticks % timescale;
	if (q > UINT64_MAX / 1000000)
		return UINT64_MAX;

	return q * 1000000 + (r * 1000000 + timescale / 2) / timescale;
}

static int build_sample_table(struct video_track *t, size_t file_size)
{
	uint32_t sample = 0;
	uint32_t stsc_idx = 0;
	uint32_t chunk;
	uint32_t stts_idx = 0;
	uint32_t stts_left = 0;
	uint32_t stts_delta = 0;
	uint32_t ctts_idx = 0;
	uint32_t ctts_left = 0;
	int64_t ctts_offset = 0;
	uint64_t dts = 0;
	int64_t min_pts = INT64_MAX;

	if (!t->sample_count || !t->stsc_count || !t->chunk_count ||
	    (!t->default_sample_size && !t->sample_sizes))
		return -1;

	t->samples = calloc(t->sample_count, sizeof(*t->samples));
	if (!t->samples)
		return -1;

	for (chunk = 1; chunk <= t->chunk_count && sample < t->sample_count; chunk++) {
		uint32_t per_chunk;
		uint32_t i;
		uint64_t off = t->chunk_offsets[chunk - 1];

		while (stsc_idx + 1 < t->stsc_count &&
		       t->stsc[stsc_idx + 1].first_chunk <= chunk)
			stsc_idx++;

		per_chunk = t->stsc[stsc_idx].samples_per_chunk;
		for (i = 0; i < per_chunk && sample < t->sample_count; i++, sample++) {
			uint32_t size = sample_size(t, sample);
			bool has_time;
			int64_t pts;

			if (off + size > file_size)
				return -1;

			if (!stts_left && stts_idx < t->stts_count) {
				stts_left = t->stts[stts_idx].count;
				stts_delta = t->stts[stts_idx].delta;
				stts_idx++;
			}

			if (!ctts_left && ctts_idx < t->ctts_count) {
				ctts_left = t->ctts[ctts_idx].count;
				ctts_offset = t->ctts[ctts_idx].offset;
				ctts_idx++;
			}

			has_time = t->timescale && stts_left;
			pts = (int64_t)dts + (ctts_left ? ctts_offset : 0);

			t->samples[sample].offset = off;
			t->samples[sample].dts_ticks = dts;
			t->samples[sample].pts_ticks = pts;
			t->samples[sample].size = size;
			t->samples[sample].duration = stts_left ? stts_delta : 0;
			t->samples[sample].has_time = has_time;
			t->samples[sample].sync = sample_is_sync(t, sample);

			if (has_time && pts < min_pts)
				min_pts = pts;

			if (stts_left) {
				dts += stts_delta;
				stts_left--;
			}
			if (ctts_left)
				ctts_left--;

			off += size;
		}
	}

	if (sample != t->sample_count)
		return -1;

	if (min_pts == INT64_MAX)
		min_pts = 0;
	t->min_pts_ticks = min_pts;

	for (sample = 0; sample < t->sample_count; sample++) {
		struct sample_info *s = &t->samples[sample];
		int64_t rel;

		if (!s->has_time)
			continue;

		rel = s->pts_ticks - min_pts;
		s->pts_us = ticks_to_us(rel > 0 ? (uint64_t)rel : 0,
					t->timescale);
	}

	return 0;
}

#ifdef HAVE_LIBAVCODEC
static const char *ffmpeg_errstr(int err, char *buf, size_t len);

struct ffmpeg_demux_track {
	int stream_index;
	enum AVMediaType type;
	enum AVCodecID codec_id;
	AVRational time_base;
	struct video_track *out;
	uint32_t count;
	uint32_t capacity;
};

static bool is_h264_codec(enum AVCodecID codec_id)
{
	return codec_id == AV_CODEC_ID_H264;
}

static bool is_playable_audio_codec(enum AVCodecID codec_id)
{
	switch (codec_id) {
	case AV_CODEC_ID_AAC:
	case AV_CODEC_ID_MP1:
	case AV_CODEC_ID_MP2:
	case AV_CODEC_ID_MP3:
	case AV_CODEC_ID_VORBIS:
	case AV_CODEC_ID_FLAC:
	case AV_CODEC_ID_APE:
	case AV_CODEC_ID_PCM_S16LE:
	case AV_CODEC_ID_PCM_S16BE:
	case AV_CODEC_ID_PCM_U8:
	case AV_CODEC_ID_ALAC:
	case AV_CODEC_ID_AMR_NB:
	case AV_CODEC_ID_AMR_WB:
	case AV_CODEC_ID_OPUS:
		return true;
	default:
		return false;
	}
}

static bool codec_is_aac(enum AVCodecID codec_id)
{
	return codec_id == AV_CODEC_ID_AAC;
}

static uint64_t av_ts_to_us(int64_t ts, AVRational time_base)
{
	if (ts == AV_NOPTS_VALUE)
		return 0;
	if (ts <= 0)
		return 0;

	return (uint64_t)av_rescale_q(ts, time_base, AV_TIME_BASE_Q);
}

static uint32_t av_duration_to_ticks(int64_t duration, AVRational time_base,
				     uint32_t timescale)
{
	int64_t ticks;

	if (duration <= 0 || !time_base.den || !time_base.num || !timescale)
		return 0;

	ticks = av_rescale_q(duration, time_base, (AVRational) { 1, timescale });
	if (ticks <= 0)
		return 0;
	return ticks > UINT32_MAX ? UINT32_MAX : (uint32_t)ticks;
}

static int ffmpeg_track_reserve(struct ffmpeg_demux_track *dt)
{
	struct sample_info *samples;
	uint8_t **sample_data;
	uint32_t capacity;

	if (dt->count < dt->capacity)
		return 0;

	capacity = dt->capacity ? dt->capacity * 2 : 256;
	samples = realloc(dt->out->samples, (size_t)capacity * sizeof(*samples));
	if (!samples)
		return -1;
	dt->out->samples = samples;

	sample_data = realloc(dt->out->sample_data,
			      (size_t)capacity * sizeof(*sample_data));
	if (!sample_data)
		return -1;
	dt->out->sample_data = sample_data;

	memset(dt->out->samples + dt->capacity, 0,
	       (capacity - dt->capacity) * sizeof(*dt->out->samples));
	memset(dt->out->sample_data + dt->capacity, 0,
	       (capacity - dt->capacity) * sizeof(*dt->out->sample_data));
	dt->capacity = capacity;
	dt->out->owns_sample_data = true;
	return 0;
}

static int ffmpeg_track_add_packet(struct ffmpeg_demux_track *dt,
				   const AVPacket *pkt)
{
	struct video_track *t = dt->out;
	struct sample_info *s;
	uint8_t *data;
	bool need_h264_params;
	int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
	int64_t dts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pts;

	if (pkt->size <= 0)
		return 0;
	if (ffmpeg_track_reserve(dt))
		return -1;

	need_h264_params = is_h264_codec(t->ffmpeg_codec_id) &&
			   (!t->num_sps || !t->num_pps);
	data = malloc((size_t)pkt->size + AV_INPUT_BUFFER_PADDING_SIZE);
	if (!data)
		return -1;
	memcpy(data, pkt->data, pkt->size);
	memset(data + pkt->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

	s = &t->samples[dt->count];
	s->offset = 0;
	s->size = pkt->size;
	s->dts_ticks = dts > 0 ? (uint64_t)dts : 0;
	s->pts_ticks = pts != AV_NOPTS_VALUE ? pts : 0;
	s->pts_us = av_ts_to_us(pts, dt->time_base);
	s->duration = av_duration_to_ticks(pkt->duration, dt->time_base,
					   t->timescale);
	s->has_time = pts != AV_NOPTS_VALUE;
	s->sync = !!(pkt->flags & AV_PKT_FLAG_KEY);
	t->sample_data[dt->count] = data;
	if (need_h264_params) {
		t->h264_format = H264_BITSTREAM_ANNEXB;
		t->nal_length_size = 0;
		(void)parse_h264_annexb_extradata(t, data, pkt->size);
	}
	dt->count++;
	t->sample_count = dt->count;
	return 0;
}

static const char *ffmpeg_codec_name(enum AVCodecID codec_id)
{
	const char *name = avcodec_get_name(codec_id);

	return name ? name : "unknown";
}

static int ffmpeg_copy_extradata(struct video_track *t,
				 const AVCodecParameters *par)
{
	if (!par->extradata || par->extradata_size <= 0)
		return 0;

	t->extra_data = malloc((size_t)par->extradata_size +
			       AV_INPUT_BUFFER_PADDING_SIZE);
	if (!t->extra_data)
		return -1;
	memcpy(t->extra_data, par->extradata, par->extradata_size);
	memset(t->extra_data + par->extradata_size, 0,
	       AV_INPUT_BUFFER_PADDING_SIZE);
	t->extra_size = par->extradata_size;
	return 0;
}

static void ffmpeg_init_video_track(struct video_track *t,
				    const AVCodecParameters *par,
				    AVRational time_base)
{
	t->is_video = true;
	t->ffmpeg_codec_id = par->codec_id;
	t->timescale = time_base.den > 0 ?
		       (uint32_t)time_base.den : 1000000;
	t->width = par->width > UINT16_MAX ? UINT16_MAX : par->width;
	t->height = par->height > UINT16_MAX ? UINT16_MAX : par->height;
}

static void ffmpeg_init_audio_track(struct video_track *t,
				    const AVCodecParameters *par,
				    AVRational time_base)
{
	const char *codec = ffmpeg_codec_name(par->codec_id);

	t->is_audio = true;
	t->ffmpeg_codec_id = par->codec_id;
	t->timescale = time_base.den > 0 ?
		       (uint32_t)time_base.den :
		       (par->sample_rate > 0 ? (uint32_t)par->sample_rate :
					       1000000);
	snprintf(t->audio_format, sizeof(t->audio_format), "%.4s", codec);
	t->audio_channels = par->ch_layout.nb_channels > UINT16_MAX ?
			    UINT16_MAX : par->ch_layout.nb_channels;
	t->audio_sample_rate = par->sample_rate > 0 ? par->sample_rate : 0;
	t->audio_sample_size = par->bits_per_coded_sample > UINT16_MAX ?
			       UINT16_MAX : par->bits_per_coded_sample;
	t->esds_avg_bitrate = par->bit_rate > 0 && par->bit_rate <= UINT32_MAX ?
			      (uint32_t)par->bit_rate : 0;
}

static int demux_with_ffmpeg(const char *path, struct video_track *video,
			     struct video_track *audio)
{
	AVFormatContext *fmt = NULL;
	AVPacket *pkt = NULL;
	struct ffmpeg_demux_track vdt = {
		.stream_index = -1,
		.type = AVMEDIA_TYPE_VIDEO,
		.out = video,
	};
	struct ffmpeg_demux_track adt = {
		.stream_index = -1,
		.type = AVMEDIA_TYPE_AUDIO,
		.out = audio,
	};
	unsigned int i;
	int ret;

	ret = avformat_open_input(&fmt, path, NULL, NULL);
	if (ret < 0) {
		char errbuf[AV_ERROR_MAX_STRING_SIZE];

		fprintf(stderr, "ffmpeg open input failed: %s\n",
			ffmpeg_errstr(ret, errbuf, sizeof(errbuf)));
		return -1;
	}

	fmt->probesize = 10000000;
	fmt->max_analyze_duration = 5 * AV_TIME_BASE;

	ret = avformat_find_stream_info(fmt, NULL);
	if (ret < 0) {
		char errbuf[AV_ERROR_MAX_STRING_SIZE];

		fprintf(stderr, "ffmpeg stream info failed: %s\n",
			ffmpeg_errstr(ret, errbuf, sizeof(errbuf)));
		goto out;
	}

	for (i = 0; i < fmt->nb_streams; i++) {
		AVStream *st = fmt->streams[i];
		AVCodecParameters *par = st->codecpar;

		if (vdt.stream_index < 0 &&
		    par->codec_type == AVMEDIA_TYPE_VIDEO &&
		    is_h264_codec(par->codec_id)) {
			vdt.stream_index = i;
			vdt.codec_id = par->codec_id;
			vdt.time_base = st->time_base;
			ffmpeg_init_video_track(video, par, st->time_base);
			video->codec = par->codec_id;
			if (ffmpeg_copy_extradata(video, par)) {
				ret = -1;
				goto out;
			}
			if (is_h264_codec(par->codec_id) && video->extra_size &&
			    parse_h264_extradata(video, video->extra_data,
						 video->extra_size)) {
				fprintf(stderr, "ffmpeg H.264 extradata unsupported\n");
				ret = -1;
				goto out;
			}
		}

		if (adt.stream_index < 0 &&
		    par->codec_type == AVMEDIA_TYPE_AUDIO &&
		    is_playable_audio_codec(par->codec_id)) {
			adt.stream_index = i;
			adt.codec_id = par->codec_id;
			adt.time_base = st->time_base;
			ffmpeg_init_audio_track(audio, par, st->time_base);
			if (ffmpeg_copy_extradata(audio, par)) {
				ret = -1;
				goto out;
			}
			if (codec_is_aac(par->codec_id) && audio->extra_size) {
				audio->has_esds = true;
				audio->audio_specific_config =
					(struct blob) { audio->extra_data,
							audio->extra_size };
				parse_aac_audio_specific_config(audio,
								audio->extra_data,
								audio->extra_size);
			}
		}
	}

	if (vdt.stream_index < 0 && adt.stream_index < 0) {
		fprintf(stderr,
			"ffmpeg found no supported video (H.264) or audio stream\n");
		ret = -1;
		goto out;
	}

	pkt = av_packet_alloc();
	if (!pkt) {
		ret = -1;
		goto out;
	}

	while ((ret = av_read_frame(fmt, pkt)) >= 0) {
		if (pkt->stream_index == vdt.stream_index) {
			if (ffmpeg_track_add_packet(&vdt, pkt)) {
				ret = -1;
				goto out;
			}
		} else if (pkt->stream_index == adt.stream_index) {
			if (ffmpeg_track_add_packet(&adt, pkt)) {
				ret = -1;
				goto out;
			}
		}
		av_packet_unref(pkt);
	}
	if (ret == AVERROR_EOF)
		ret = 0;
	if (ret < 0) {
		char errbuf[AV_ERROR_MAX_STRING_SIZE];

		fprintf(stderr, "ffmpeg demux failed: %s\n",
			ffmpeg_errstr(ret, errbuf, sizeof(errbuf)));
		goto out;
	}

	if (video->is_video && !video->sample_count) {
		fprintf(stderr, "ffmpeg video stream has no packets\n");
		ret = -1;
		goto out;
	}
	if (video->is_video && is_h264_codec(video->codec) &&
	    (!video->num_sps || !video->num_pps)) {
		fprintf(stderr, "ffmpeg H.264 SPS/PPS not found\n");
		ret = -1;
		goto out;
	}
	if (audio->is_audio && !audio->sample_count) {
		free_track(audio);
		memset(audio, 0, sizeof(*audio));
	}

	fprintf(stderr,
		"ffmpeg demux format=%s video=%s samples=%u audio=%s samples=%u\n",
		fmt->iformat && fmt->iformat->name ? fmt->iformat->name : "unknown",
		video->is_video ? ffmpeg_codec_name(vdt.codec_id) : "none",
		video->sample_count,
		audio->is_audio ? ffmpeg_codec_name(adt.codec_id) : "none",
		audio->sample_count);

out:
	if (pkt)
		av_packet_free(&pkt);
	avformat_close_input(&fmt);
	return ret < 0 ? -1 : 0;
}
#else
static int demux_with_ffmpeg(const char *path, struct video_track *video,
			     struct video_track *audio)
{
	(void)path;
	(void)video;
	(void)audio;
	return -ENOSYS;
}
#endif

static void copy_sps_info(struct h264_stream_info *info, const GstH264SPS *sps)
{
	info->have_sps = true;
	info->profile_idc = sps->profile_idc;
	info->level_idc = sps->level_idc;
	info->max_frame_num = sps->max_frame_num;
	info->width_mbs_minus1 = sps->pic_width_in_mbs_minus1;
	info->height_map_units_minus1 = sps->pic_height_in_map_units_minus1;
	info->crop_x = sps->crop_rect_x > 0 ? (uint32_t)sps->crop_rect_x : 0;
	info->crop_y = sps->crop_rect_y > 0 ? (uint32_t)sps->crop_rect_y : 0;
	info->crop_width = sps->crop_rect_width > 0 ?
			   (uint32_t)sps->crop_rect_width : 0;
	info->crop_height = sps->crop_rect_height > 0 ?
			    (uint32_t)sps->crop_rect_height : 0;
	info->frame_cropping_flag = sps->frame_cropping_flag;
	info->num_ref_frames = sps->num_ref_frames;
	info->pic_order_cnt_type = sps->pic_order_cnt_type;
	info->log2_max_pic_order_cnt_lsb_minus4 =
		sps->log2_max_pic_order_cnt_lsb_minus4;
	info->frame_mbs_only_flag = sps->frame_mbs_only_flag;
	info->vui_timing = sps->vui_parameters.timing_info_present_flag;
	info->vui_num_units_in_tick = sps->vui_parameters.num_units_in_tick;
	info->vui_time_scale = sps->vui_parameters.time_scale;
	info->vui_fixed_frame_rate = sps->vui_parameters.fixed_frame_rate_flag;
	info->vui_bitstream_restriction =
		sps->vui_parameters.bitstream_restriction_flag;
	info->vui_num_reorder_frames = sps->vui_parameters.num_reorder_frames;
	info->vui_max_dec_frame_buffering =
		sps->vui_parameters.max_dec_frame_buffering;
}

static void copy_pps_info(struct h264_stream_info *info, const GstH264PPS *pps)
{
	info->have_pps = true;
	info->pps_id = pps->id;
	info->sps_id = pps->sequence->id;
	info->entropy_coding_mode = pps->entropy_coding_mode_flag;
	info->num_ref_idx_l0_default_active_minus1 =
		pps->num_ref_idx_l0_active_minus1;
	info->num_ref_idx_l1_default_active_minus1 =
		pps->num_ref_idx_l1_active_minus1;
	info->weighted_pred_flag = pps->weighted_pred_flag;
	info->weighted_bipred_idc = pps->weighted_bipred_idc;
}

static int prime_h264_parser(GstH264NalParser *parser,
			     const struct video_track *t,
			     struct h264_stream_info *info)
{
	unsigned int i;

	for (i = 0; i < t->num_sps; i++) {
		GstH264NalUnit nalu;
		GstH264SPS sps;
		GstH264ParserResult res;
		uint8_t *tmp;

		tmp = malloc(t->sps[i].size + 4);
		if (!tmp)
			return -1;
		tmp[0] = (t->sps[i].size >> 24) & 0xff;
		tmp[1] = (t->sps[i].size >> 16) & 0xff;
		tmp[2] = (t->sps[i].size >> 8) & 0xff;
		tmp[3] = t->sps[i].size & 0xff;
		memcpy(tmp + 4, t->sps[i].data, t->sps[i].size);

		res = gst_h264_parser_identify_nalu_avc(parser, tmp, 0,
							t->sps[i].size + 4, 4,
							&nalu);
		if (res == GST_H264_PARSER_OK)
			res = gst_h264_parser_parse_sps(parser, &nalu, &sps);
		free(tmp);
		if (res != GST_H264_PARSER_OK)
			return -1;
		copy_sps_info(info, &sps);
		if (gst_h264_parser_update_sps(parser, &sps) != GST_H264_PARSER_OK)
			return -1;
	}

	for (i = 0; i < t->num_pps; i++) {
		GstH264NalUnit nalu;
		GstH264PPS pps;
		GstH264ParserResult res;
		uint8_t *tmp;

		tmp = malloc(t->pps[i].size + 4);
		if (!tmp)
			return -1;
		tmp[0] = (t->pps[i].size >> 24) & 0xff;
		tmp[1] = (t->pps[i].size >> 16) & 0xff;
		tmp[2] = (t->pps[i].size >> 8) & 0xff;
		tmp[3] = t->pps[i].size & 0xff;
		memcpy(tmp + 4, t->pps[i].data, t->pps[i].size);

		res = gst_h264_parser_identify_nalu_avc(parser, tmp, 0,
							t->pps[i].size + 4, 4,
							&nalu);
		if (res == GST_H264_PARSER_OK)
			res = gst_h264_parser_parse_pps(parser, &nalu, &pps);
		free(tmp);
		if (res != GST_H264_PARSER_OK)
			return -1;
		copy_pps_info(info, &pps);
		if (gst_h264_parser_update_pps(parser, &pps) != GST_H264_PARSER_OK)
			return -1;
	}

	return 0;
}

static int walk_h264_samples(const uint8_t *file, const struct video_track *t,
			     const struct options *opt, struct stats *stats)
{
	GstH264NalParser *parser;
	struct h264_stream_info info = { 0 };
	uint32_t limit = t->sample_count;
	uint32_t i;
	int ret = -1;

	parser = gst_h264_nal_parser_new();
	if (!parser)
		return -1;

	if (prime_h264_parser(parser, t, &info)) {
		fprintf(stderr, "failed to parse avcC SPS/PPS\n");
		goto out;
	}

	for (i = 0; i < limit; i++) {
		const struct sample_info *s = &t->samples[i];
		const uint8_t *data = track_sample_data(file, t, i);
		uint32_t off = 0;
		uint64_t sample_start = now_us();

		stats->samples++;
		stats->bytes += s->size;

		while (off < s->size) {
			GstH264NalUnit nalu;
			GstH264ParserResult pres;
			uint64_t stage = now_us();

			pres = identify_h264_nalu(parser, t, data, off, s->size,
						  &nalu);
			stats->identify_us += now_us() - stage;
			if (pres != GST_H264_PARSER_OK) {
				stats->parse_errors++;
				break;
			}

			stats->nalus++;
			switch (nalu.type) {
			case GST_H264_NAL_SPS:
			case GST_H264_NAL_SUBSET_SPS:
				stats->sps++;
				stage = now_us();
				pres = gst_h264_parser_parse_nal(parser, &nalu);
				stats->parse_us += now_us() - stage;
				break;
			case GST_H264_NAL_PPS:
				stats->pps++;
				stage = now_us();
				pres = gst_h264_parser_parse_nal(parser, &nalu);
				stats->parse_us += now_us() - stage;
				break;
			case GST_H264_NAL_SEI:
				stats->sei++;
				break;
			case GST_H264_NAL_AU_DELIMITER:
				stats->aud++;
				break;
			case GST_H264_NAL_SLICE:
			case GST_H264_NAL_SLICE_DPA:
			case GST_H264_NAL_SLICE_DPB:
			case GST_H264_NAL_SLICE_DPC:
			case GST_H264_NAL_SLICE_IDR: {
				GstH264SliceHdr slice;
				uint32_t stype;

				stats->slices++;
				if (nalu.type == GST_H264_NAL_SLICE_IDR)
					stats->idr++;

				stage = now_us();
				pres = gst_h264_parser_parse_slice_hdr(parser,
								       &nalu,
								       &slice,
								       true,
								       true);
				stats->parse_us += now_us() - stage;
				if (pres == GST_H264_PARSER_OK) {
					stype = slice.type % 5;
					if (stype == GST_H264_P_SLICE)
						stats->p_slices++;
					else if (stype == GST_H264_B_SLICE)
						stats->b_slices++;
					else if (stype == GST_H264_I_SLICE)
						stats->i_slices++;
					if (nalu.ref_idc)
						stats->ref_slices++;
					else
						stats->nonref_slices++;
					if (slice.field_pic_flag)
						stats->field_slices++;
					if (slice.ref_pic_list_modification_flag_l0)
						stats->ref_list_mod_l0++;
					if (slice.ref_pic_list_modification_flag_l1)
						stats->ref_list_mod_l1++;
					if (slice.dec_ref_pic_marking.
					    adaptive_ref_pic_marking_mode_flag)
						stats->adaptive_marking++;
					if ((stype == GST_H264_P_SLICE &&
					     slice.pps->weighted_pred_flag) ||
					    (stype == GST_H264_B_SLICE &&
					     slice.pps->weighted_bipred_idc))
						stats->pred_weight++;
					if ((uint32_t)slice.num_ref_idx_l0_active_minus1 + 1 >
					    stats->max_ref_idx_l0)
						stats->max_ref_idx_l0 =
							slice.num_ref_idx_l0_active_minus1 + 1;
					if ((uint32_t)slice.num_ref_idx_l1_active_minus1 + 1 >
					    stats->max_ref_idx_l1)
						stats->max_ref_idx_l1 =
							slice.num_ref_idx_l1_active_minus1 + 1;
					if (slice.frame_num > stats->max_frame_num_seen)
						stats->max_frame_num_seen = slice.frame_num;
					if (slice.pic_order_cnt_lsb >
					    stats->max_poc_lsb_seen)
						stats->max_poc_lsb_seen =
							slice.pic_order_cnt_lsb;
				}
				break;
			}
			default:
				break;
			}

			if (pres != GST_H264_PARSER_OK)
				stats->parse_errors++;

			off = nalu.offset + nalu.size;
		}


		{
			uint64_t elapsed = now_us() - sample_start;
			if (elapsed > stats->max_sample_us)
				stats->max_sample_us = elapsed;
		}
	}

	ret = 0;
out:
	if (ret == 0) {
		fprintf(stderr,
			"h264 stream profile=%u level=%u coded=%ux%u display=%ux%u crop=%u,%u flag=%u max_frame_num=%u num_ref_frames=%u poc_type=%u log2_poc_lsb_minus4=%u frame_mbs_only=%u pps=%u sps=%u cabac=%u default_refs=%u/%u weighted=%u/%u vui_timing=%u vui_ticks=%u/%u fixed=%u reorder=%u max_dpb=%u\n",
			info.profile_idc, info.level_idc,
			h264_info_coded_width(&info),
			h264_info_coded_height(&info),
			h264_info_display_width(&info),
			h264_info_display_height(&info),
			info.crop_x, info.crop_y, info.frame_cropping_flag,
			info.max_frame_num, info.num_ref_frames,
			info.pic_order_cnt_type,
			info.log2_max_pic_order_cnt_lsb_minus4,
			info.frame_mbs_only_flag, info.pps_id, info.sps_id,
			info.entropy_coding_mode,
			info.num_ref_idx_l0_default_active_minus1 + 1,
			info.num_ref_idx_l1_default_active_minus1 + 1,
			info.weighted_pred_flag, info.weighted_bipred_idc,
			info.vui_timing, info.vui_num_units_in_tick,
			info.vui_time_scale, info.vui_fixed_frame_rate,
			info.vui_num_reorder_frames,
			info.vui_max_dec_frame_buffering);
	}
	gst_h264_nal_parser_free(parser);
	return ret;
}

static uint64_t track_duration_ticks(const struct video_track *t)
{
	uint64_t ticks = 0;
	uint32_t i;

	for (i = 0; i < t->stts_count; i++)
		ticks += (uint64_t)t->stts[i].count * t->stts[i].delta;

	return ticks;
}

static void print_blob_hex_prefix(const struct blob *b, size_t max)
{
	size_t i;

	for (i = 0; i < b->size && i < max; i++)
		fprintf(stderr, "%02x", b->data[i]);
	if (b->size > max)
		fprintf(stderr, "...");
}

static void print_audio_summary(const struct video_track *audio,
				const char *label)
{
	uint64_t duration_ticks = track_duration_ticks(audio);
	double duration_sec = audio->timescale && duration_ticks ?
			      (double)duration_ticks / audio->timescale : 0.0;
	uint32_t sample_delta = audio->sample_count && audio->samples ?
				audio->samples[0].duration : 0;

	fprintf(stderr,
		"%s audio=%s samples=%u chunks=%u timescale=%u duration=%.3fs sample_rate=%u channels=%u sample_size=%u sample_delta=%u",
		label,
		audio->audio_format[0] ? audio->audio_format : "unknown",
		audio->sample_count, audio->chunk_count, audio->timescale,
		duration_sec, audio->audio_sample_rate, audio->audio_channels,
		audio->audio_sample_size, sample_delta);

	if (audio->has_esds)
		fprintf(stderr,
			" esds_object=0x%02x stream_type=0x%02x buffer=%u avg_bitrate=%u max_bitrate=%u",
			audio->esds_object_type, audio->esds_stream_type,
			audio->esds_buffer_size, audio->esds_avg_bitrate,
			audio->esds_max_bitrate);

	if (audio->audio_specific_config.size) {
		fprintf(stderr, " asc=");
		print_blob_hex_prefix(&audio->audio_specific_config, 16);
		fprintf(stderr,
			" aac_object=%u(%s) aac_rate=%u aac_channels=%u",
			audio->aac_object_type,
			aac_object_name(audio->aac_object_type),
			audio->aac_sample_rate, audio->aac_channel_config);
		if (audio->aac_ext_object_type || audio->aac_ext_sample_rate)
			fprintf(stderr, " aac_ext=%u(%s)/%u",
				audio->aac_ext_object_type,
				aac_object_name(audio->aac_ext_object_type),
				audio->aac_ext_sample_rate);
	}

	fprintf(stderr, "\n");
}

static void free_track(struct video_track *t)
{
	uint32_t i;

	free(t->stts);
	free(t->ctts);
	free(t->stsc);
	free(t->sample_sizes);
	free(t->chunk_offsets);
	free(t->sync_samples);
	if (t->owns_sample_data && t->sample_data) {
		for (i = 0; i < t->sample_count; i++)
			free(t->sample_data[i]);
	}
	free(t->sample_data);
	free(t->extra_data);
	free(t->samples);
}

struct audio_play_stats {
	uint32_t samples;
	uint32_t decoded;
	uint32_t errors;
	uint64_t input_bytes;
	uint64_t pcm_frames;
	uint64_t decode_us;
	uint64_t convert_us;
	uint64_t write_us;
	uint64_t max_decode_us;
	uint64_t max_convert_us;
	uint64_t max_write_us;
};

static int audio_recover_pcm(snd_pcm_t *pcm, int err)
{
	if (err == -EPIPE || err == -ESTRPIPE)
		return snd_pcm_recover(pcm, err, 1);
	return err;
}

static int open_audio_pcm(snd_pcm_t **out, const struct options *opt,
			  unsigned int rate, unsigned int channels)
{
	const snd_pcm_format_t fmt = SND_PCM_FORMAT_S16_LE;
	snd_pcm_t *pcm = NULL;
	int ret;

	if (channels < 1 || channels > 2) {
		fprintf(stderr, "unsupported ALSA channel count: %u\n", channels);
		return -EINVAL;
	}

	ret = snd_pcm_open(&pcm, opt->pcm_dev, SND_PCM_STREAM_PLAYBACK, 0);
	if (ret < 0) {
		fprintf(stderr, "open pcm %s failed: %s\n", opt->pcm_dev,
			snd_strerror(ret));
		return ret;
	}

	ret = snd_pcm_set_params(pcm, fmt, SND_PCM_ACCESS_RW_INTERLEAVED,
				 channels, rate, 1, 100000);
	if (ret < 0) {
		fprintf(stderr, "set pcm params failed: %s\n",
			snd_strerror(ret));
		snd_pcm_close(pcm);
		return ret;
	}


	*out = pcm;
	return 0;
}

static int write_pcm_all(snd_pcm_t *pcm, const int16_t *data,
			 snd_pcm_uframes_t frames, unsigned int channels,
			 struct audio_play_stats *stats,
			 const struct options *opt)
{
	snd_pcm_uframes_t written_total = 0;

	while (written_total < frames) {
		snd_pcm_sframes_t written;
		uint64_t start = now_us();
		uint64_t elapsed;

		if (playback_aborted(opt))
			return -ECANCELED;

		written = snd_pcm_writei(pcm, data + written_total * channels,
					 frames - written_total);
		elapsed = now_us() - start;
		profile_add_us(&stats->write_us, &stats->max_write_us, elapsed);

		if (written < 0) {
			int ret = audio_recover_pcm(pcm, (int)written);

			if (ret < 0) {
				fprintf(stderr, "pcm write failed: %s\n",
					snd_strerror(ret));
				return ret;
			}
			continue;
		}
		if (!written) {
			if (playback_aborted(opt))
				return -ECANCELED;
			usleep(1000);
			continue;
		}
		written_total += (snd_pcm_uframes_t)written;
	}

	return 0;
}

#ifdef HAVE_LIBAVCODEC
static const char *ffmpeg_errstr(int err, char *buf, size_t len)
{
	if (av_strerror(err, buf, len) < 0)
		snprintf(buf, len, "ffmpeg error %d", err);
	return buf;
}

static int16_t float_to_s16(double val)
{
	if (!(val == val))
		return 0;
	if (val >= 1.0)
		return INT16_MAX;
	if (val <= -1.0)
		return INT16_MIN;
	return (int16_t)(val < 0.0 ? val * 32768.0 : val * 32767.0);
}

static unsigned int ffmpeg_frame_channels(const AVFrame *frame,
					  const AVCodecContext *ctx)
{
	if (frame->ch_layout.nb_channels > 0)
		return frame->ch_layout.nb_channels;
	if (ctx->ch_layout.nb_channels > 0)
		return ctx->ch_layout.nb_channels;
	return 0;
}

static int ffmpeg_frame_to_s16(const AVFrame *frame, unsigned int channels,
			       int16_t **out, size_t *out_capacity,
			       size_t *out_frames)
{
	enum AVSampleFormat fmt = frame->format;
	int planar = av_sample_fmt_is_planar(fmt);
	int samples = frame->nb_samples;
	size_t frames;
	size_t total;
	int16_t *dst;
	int i;
	unsigned int ch;

	if (samples <= 0) {
		*out_frames = 0;
		return 0;
	}
	if (!channels || channels > 8)
		return -EINVAL;
	frames = (size_t)samples;
	if (frames > SIZE_MAX / channels ||
	    frames * channels > SIZE_MAX / sizeof(*dst))
		return -ENOMEM;

	total = frames * channels;
	if (total > *out_capacity) {
		dst = realloc(*out, total * sizeof(*dst));
		if (!dst)
			return -ENOMEM;
		*out = dst;
		*out_capacity = total;
	} else {
		dst = *out;
	}

	switch (fmt) {
	case AV_SAMPLE_FMT_S16:
		memcpy(dst, frame->data[0], total * sizeof(*dst));
		break;
	case AV_SAMPLE_FMT_S16P:
		for (i = 0; i < samples; i++) {
			for (ch = 0; ch < channels; ch++) {
				const int16_t *src =
					(const int16_t *)frame->extended_data[ch];

				dst[(size_t)i * channels + ch] = src[i];
			}
		}
		break;
	case AV_SAMPLE_FMT_S32:
		for (i = 0; i < samples; i++) {
			for (ch = 0; ch < channels; ch++) {
				const int32_t *src = (const int32_t *)frame->data[0];
				int32_t val = src[(size_t)i * channels + ch];

				dst[(size_t)i * channels + ch] =
					(int16_t)(val >> 16);
			}
		}
		break;
	case AV_SAMPLE_FMT_S32P:
		if (channels == 2) {
			const int32_t *src0 =
				(const int32_t *)frame->extended_data[0];
			const int32_t *src1 =
				(const int32_t *)frame->extended_data[1];

			for (i = 0; i < samples; i++) {
				dst[(size_t)i * 2] = (int16_t)(src0[i] >> 16);
				dst[(size_t)i * 2 + 1] = (int16_t)(src1[i] >> 16);
			}
		} else {
			for (i = 0; i < samples; i++) {
				for (ch = 0; ch < channels; ch++) {
					const int32_t *src =
						(const int32_t *)frame->extended_data[ch];

					dst[(size_t)i * channels + ch] =
						(int16_t)(src[i] >> 16);
				}
			}
		}
		break;
	case AV_SAMPLE_FMT_FLT:
		for (i = 0; i < samples; i++) {
			for (ch = 0; ch < channels; ch++) {
				const float *src = (const float *)frame->data[0];

				dst[(size_t)i * channels + ch] =
					float_to_s16(src[(size_t)i * channels + ch]);
			}
		}
		break;
	case AV_SAMPLE_FMT_FLTP:
		for (i = 0; i < samples; i++) {
			for (ch = 0; ch < channels; ch++) {
				const float *src =
					(const float *)frame->extended_data[ch];

				dst[(size_t)i * channels + ch] =
					float_to_s16(src[i]);
			}
		}
		break;
	case AV_SAMPLE_FMT_DBL:
		for (i = 0; i < samples; i++) {
			for (ch = 0; ch < channels; ch++) {
				const double *src = (const double *)frame->data[0];

				dst[(size_t)i * channels + ch] =
					float_to_s16(src[(size_t)i * channels + ch]);
			}
		}
		break;
	case AV_SAMPLE_FMT_DBLP:
		for (i = 0; i < samples; i++) {
			for (ch = 0; ch < channels; ch++) {
				const double *src =
					(const double *)frame->extended_data[ch];

				dst[(size_t)i * channels + ch] =
					float_to_s16(src[i]);
			}
		}
		break;
	default:
		fprintf(stderr, "unsupported ffmpeg sample format for ALSA: %s%s\n",
			av_get_sample_fmt_name(fmt) ?
			av_get_sample_fmt_name(fmt) : "unknown",
			planar ? " planar" : "");
		return -EINVAL;
	}

	*out_frames = frames;
	return 0;
}

static int ffmpeg_handle_audio_frame(AVCodecContext *ctx, AVFrame *frame,
				     snd_pcm_t *pcm,
				     struct audio_play_stats *stats,
				     unsigned int *rate,
				     unsigned int *channels,
				     int16_t **s16_buf,
				     size_t *s16_capacity,
				     const struct options *opt)
{
	unsigned int frame_rate = frame->sample_rate ?
				  frame->sample_rate : ctx->sample_rate;
	unsigned int frame_channels = ffmpeg_frame_channels(frame, ctx);
	size_t frames = frame->nb_samples > 0 ? (size_t)frame->nb_samples : 0;
	int ret;

	if (!frames)
		return 0;
	if (!frame_rate || !frame_channels) {
		fprintf(stderr, "invalid ffmpeg decoded params: %uHz %uch\n",
			frame_rate, frame_channels);
		return -EINVAL;
	}
	if (frame_rate != *rate || frame_channels != *channels) {
		fprintf(stderr,
			"ffmpeg audio params changed: %uHz/%uch expected %uHz/%uch\n",
			frame_rate, frame_channels, *rate, *channels);
		return -EINVAL;
	}

	if (pcm) {
		uint64_t stage = now_us();

		ret = ffmpeg_frame_to_s16(frame, frame_channels, s16_buf,
					  s16_capacity, &frames);
		profile_add_us(&stats->convert_us, &stats->max_convert_us,
			       now_us() - stage);
		if (ret)
			return ret;
		ret = write_pcm_all(pcm, *s16_buf, frames, frame_channels,
				    stats, opt);
		if (ret < 0)
			return ret;
	}

	stats->decoded++;
	stats->pcm_frames += frames;

	return 0;
}

static int ffmpeg_receive_audio(AVCodecContext *ctx, AVFrame *frame,
				snd_pcm_t *pcm, struct audio_play_stats *stats,
				unsigned int *rate, unsigned int *channels,
				int16_t **s16_buf,
				size_t *s16_capacity,
				const struct options *opt,
				uint64_t *decode_elapsed_us)
{
	for (;;) {
		uint64_t stage = now_us();
		int ret;

		if (playback_aborted(opt))
			return -ECANCELED;

		ret = avcodec_receive_frame(ctx, frame);

		*decode_elapsed_us += now_us() - stage;
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return 0;
		if (ret < 0) {
			char errbuf[AV_ERROR_MAX_STRING_SIZE];

			fprintf(stderr, "ffmpeg receive frame failed: %s\n",
				ffmpeg_errstr(ret, errbuf, sizeof(errbuf)));
			return ret;
		}

		ret = ffmpeg_handle_audio_frame(ctx, frame, pcm, stats,
						rate, channels, s16_buf,
						s16_capacity, opt);
		av_frame_unref(frame);
		if (ret)
			return ret;
	}
}

static int play_aac_audio_ffmpeg(const uint8_t *file,
				 const struct video_track *audio,
				 const struct options *opt,
				 bool fixed)
{
	const bool aac = audio->ffmpeg_codec_id == AV_CODEC_ID_NONE ||
			 audio->ffmpeg_codec_id == AV_CODEC_ID_AAC;
	const char *decoder_name = fixed && aac ? "aac_fixed" : NULL;
	const AVCodec *codec;
	AVCodecContext *ctx = NULL;
	AVPacket *pkt = NULL;
	AVFrame *frame = NULL;
	snd_pcm_t *pcm = NULL;
	struct audio_play_stats stats = { 0 };
	uint8_t *pkt_buf = NULL;
	int16_t *s16_buf = NULL;
	size_t s16_capacity = 0;
	uint32_t limit;
	uint32_t i;
	uint32_t max_sample_size = 0;
	unsigned int rate;
	unsigned int channels;
	uint64_t start_us;
	uint64_t elapsed_us;
	bool benchmark = opt->no_pace;
	int ret = -1;

	if (!audio->is_audio) {
		fprintf(stderr, "audio track not found\n");
		return -1;
	}
	if (aac && !audio->audio_specific_config.size) {
		fprintf(stderr, "AAC AudioSpecificConfig missing\n");
		return -1;
	}

	codec = decoder_name ? avcodec_find_decoder_by_name(decoder_name) : NULL;
	if (!codec) {
		enum AVCodecID codec_id = audio->ffmpeg_codec_id ?
					  audio->ffmpeg_codec_id :
					  AV_CODEC_ID_AAC;

		codec = avcodec_find_decoder(codec_id);
	}
	if (!codec) {
		fprintf(stderr, "ffmpeg decoder not found: %s\n",
			audio->audio_format[0] ? audio->audio_format : "unknown");
		return -ENOENT;
	}

	ctx = avcodec_alloc_context3(codec);
	pkt = av_packet_alloc();
	frame = av_frame_alloc();
	if (!ctx || !pkt || !frame) {
		fprintf(stderr, "failed to allocate ffmpeg decoder state\n");
		goto out;
	}

	rate = audio->aac_sample_rate ? audio->aac_sample_rate :
				       audio->audio_sample_rate;
	channels = audio->aac_channel_config ? audio->aac_channel_config :
					      audio->audio_channels;
	if (!rate || !channels) {
		fprintf(stderr, "invalid ffmpeg audio params: %uHz %uch\n",
			rate, channels);
		goto out;
	}

	ctx->sample_rate = rate;
	av_channel_layout_default(&ctx->ch_layout, channels);
	ctx->bit_rate = audio->esds_avg_bitrate;
	ctx->bits_per_coded_sample = audio->audio_sample_size;
	ctx->thread_count = 1;
	ctx->request_sample_fmt = fixed && aac ? AV_SAMPLE_FMT_S16 :
					  AV_SAMPLE_FMT_NONE;
	if (audio->audio_specific_config.size) {
		ctx->extradata = av_mallocz(audio->audio_specific_config.size +
					    AV_INPUT_BUFFER_PADDING_SIZE);
		if (!ctx->extradata)
			goto out;
		memcpy(ctx->extradata, audio->audio_specific_config.data,
		       audio->audio_specific_config.size);
		ctx->extradata_size = audio->audio_specific_config.size;
	} else if (audio->extra_size) {
		ctx->extradata = av_mallocz(audio->extra_size +
					    AV_INPUT_BUFFER_PADDING_SIZE);
		if (!ctx->extradata)
			goto out;
		memcpy(ctx->extradata, audio->extra_data, audio->extra_size);
		ctx->extradata_size = audio->extra_size;
	}

	ret = avcodec_open2(ctx, codec, NULL);
	if (ret < 0) {
		char errbuf[AV_ERROR_MAX_STRING_SIZE];

		fprintf(stderr, "ffmpeg open decoder %s failed: %s\n",
			codec->name, ffmpeg_errstr(ret, errbuf, sizeof(errbuf)));
		goto out;
	}

	if (!benchmark) {
		ret = open_audio_pcm(&pcm, opt, rate, channels);
		if (ret < 0)
			goto out;
	}

	limit = audio->sample_count;
	for (i = 0; i < limit; i++) {
		if (audio->samples[i].size > max_sample_size)
			max_sample_size = audio->samples[i].size;
	}
	pkt_buf = av_malloc((size_t)max_sample_size +
			    AV_INPUT_BUFFER_PADDING_SIZE);
	if (!pkt_buf)
		goto out;

	ret = audio_wait_for_playback_base(opt);
	if (ret)
		goto out;
	start_us = now_us();

	for (i = 0; i < limit; i++) {
		const struct sample_info *s = &audio->samples[i];
		uint64_t decode_elapsed = 0;

		if (playback_aborted(opt)) {
			ret = -ECANCELED;
			goto out;
		}

		stats.samples++;
		stats.input_bytes += s->size;

		memcpy(pkt_buf, track_sample_data(file, audio, i), s->size);
		memset(pkt_buf + s->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
		av_packet_unref(pkt);
		pkt->data = pkt_buf;
		pkt->size = s->size;
		pkt->pts = s->pts_us;
		pkt->duration = s->duration;

		for (;;) {
			uint64_t stage = now_us();

			ret = avcodec_send_packet(ctx, pkt);
			decode_elapsed += now_us() - stage;
			if (ret != AVERROR(EAGAIN))
				break;
			ret = ffmpeg_receive_audio(ctx, frame, pcm, &stats,
						   &rate, &channels, &s16_buf,
						   &s16_capacity, opt,
						   &decode_elapsed);
			if (ret)
				goto out;
		}

		if (ret == AVERROR_INVALIDDATA) {
			char errbuf[AV_ERROR_MAX_STRING_SIZE];

			stats.errors++;
			fprintf(stderr, "ffmpeg decode sample=%u failed: %s\n",
				i + 1, ffmpeg_errstr(ret, errbuf,
						     sizeof(errbuf)));
			profile_add_us(&stats.decode_us, &stats.max_decode_us,
				       decode_elapsed);
			continue;
		}
		if (ret < 0) {
			char errbuf[AV_ERROR_MAX_STRING_SIZE];

			fprintf(stderr, "ffmpeg send packet sample=%u failed: %s\n",
				i + 1, ffmpeg_errstr(ret, errbuf,
						     sizeof(errbuf)));
			goto out;
		}

		ret = ffmpeg_receive_audio(ctx, frame, pcm, &stats,
					   &rate, &channels, &s16_buf,
					   &s16_capacity, opt, &decode_elapsed);
		profile_add_us(&stats.decode_us, &stats.max_decode_us,
			       decode_elapsed);
		if (ret)
			goto out;
	}

	{
		uint64_t decode_elapsed = 0;
		uint64_t stage = now_us();

		if (playback_aborted(opt)) {
			ret = -ECANCELED;
			goto out;
		}

		ret = avcodec_send_packet(ctx, NULL);
		decode_elapsed += now_us() - stage;
		if (ret >= 0) {
			ret = ffmpeg_receive_audio(ctx, frame, pcm, &stats,
						   &rate, &channels, &s16_buf,
						   &s16_capacity, opt,
						   &decode_elapsed);
			profile_add_us(&stats.decode_us, &stats.max_decode_us,
				       decode_elapsed);
			if (ret)
				goto out;
		}
	}

	if (pcm) {
		if (playback_aborted(opt)) {
			ret = -ECANCELED;
			goto out;
		}
		ret = snd_pcm_drain(pcm);
		if (ret < 0) {
			fprintf(stderr, "pcm drain failed: %s\n",
				snd_strerror(ret));
			goto out;
		}
	}
	ret = 0;

	elapsed_us = now_us() - start_us;
	fprintf(stderr,
		"audio %s ok backend=ffmpeg decoder=%s decoded=%u pcm_frames=%" PRIu64 " errors=%u bytes=%" PRIu64 " elapsed_us=%" PRIu64 "\n",
		benchmark ? "benchmark" : "play", codec->name,
		stats.decoded, stats.pcm_frames, stats.errors,
		stats.input_bytes, elapsed_us);

	if (opt->profile) {
		double decode_avg = stats.samples ?
				    (double)stats.decode_us / stats.samples : 0.0;
		double write_avg = stats.decoded ?
				   (double)stats.write_us / stats.decoded : 0.0;
		double pcm_rate = elapsed_us ?
				  (double)stats.pcm_frames * 1000000.0 /
				  elapsed_us : 0.0;
		double sample_rate = elapsed_us ?
				     (double)stats.decoded * 1000000.0 /
				     elapsed_us : 0.0;

		fprintf(stderr,
			"audio profile samples=%u decoded=%u avg_us: decode=%.1f write=%.1f max_us: decode=%" PRIu64 " write=%" PRIu64 " sample_rate=%.1f fps pcm_rate=%.1f fps\n",
			stats.samples, stats.decoded, decode_avg, write_avg,
			stats.max_decode_us, stats.max_write_us, sample_rate,
			pcm_rate);
	}

	if (stats.errors && !ret)
		ret = 2;

out:
	free(s16_buf);
	if (pkt_buf)
		av_free(pkt_buf);
	if (pcm)
		snd_pcm_close(pcm);
	if (frame)
		av_frame_free(&frame);
	if (pkt)
		av_packet_free(&pkt);
	if (ctx)
		avcodec_free_context(&ctx);
	return ret;
}
#else
static int play_aac_audio_ffmpeg(const uint8_t *file,
				 const struct video_track *audio,
				 const struct options *opt,
				 bool fixed)
{
	(void)file;
	(void)audio;
	(void)opt;
	fprintf(stderr,
		"ffmpeg backend not built%s: enable BR2_PACKAGE_FFMPEG and rebuild fplayerdemo\n",
		fixed ? " with aac_fixed" : "");
	return -ENOSYS;
}
#endif

static int play_aac_audio(const uint8_t *file, const struct video_track *audio,
			  const struct options *opt)
{
	if (opt->audio_backend == AUDIO_BACKEND_AUTO)
		return play_aac_audio_ffmpeg(file, audio, opt, true);
	if (opt->audio_backend == AUDIO_BACKEND_FFMPEG)
		return play_aac_audio_ffmpeg(file, audio, opt, false);
	if (opt->audio_backend == AUDIO_BACKEND_FFMPEG_FIXED)
		return play_aac_audio_ffmpeg(file, audio, opt, true);

	return play_aac_audio_ffmpeg(file, audio, opt, true);
}

struct audio_thread_args {
	const uint8_t *file;
	const struct video_track *audio;
	struct options opt;
	int ret;
};

static void *audio_thread_main(void *arg)
{
	struct audio_thread_args *audio = arg;

	set_thread_name("f1c-audio");

	audio->ret = play_aac_audio(audio->file, audio->audio, &audio->opt);
	return NULL;
}

struct mapped_buffer {
	void *addr;
	uint32_t length;
};

struct lite_v4l2 {
	int video_fd;
	int media_fd;
	struct mapped_buffer output;
	struct mapped_buffer capture[LITE_CAPTURE_BUFFERS];
	struct v4l2_pix_format capture_fmt;
	uint8_t capture_refs[LITE_CAPTURE_BUFFERS];
	uint32_t capture_count;
	uint32_t output_size;
	uint32_t capture_size;
	bool stream_output;
	bool stream_capture;
};

struct lite_pic {
	bool valid;
	bool reference;
	bool long_term;
	uint32_t frame_num;
	int32_t pic_num;
	int32_t long_term_pic_num;
	int32_t long_term_frame_idx;
	int32_t top_poc;
	int32_t bottom_poc;
	int32_t poc;
	uint32_t frame_id;
	uint32_t capture_index;
};

struct lite_dpb {
	struct lite_pic pics[LITE_MAX_DPB];
	uint32_t max_num_ref_frames;
	uint32_t max_frame_num;
	int32_t max_long_term_frame_idx;
};

struct poc_state {
	int32_t prev_ref_poc_msb;
	uint32_t prev_ref_poc_lsb;
	uint32_t prev_frame_num;
	uint32_t prev_frame_num_offset;
};

struct ref_entry {
	struct lite_pic *pic;
	int dpb_index;
	int32_t pic_num;
	int32_t long_term_pic_num;
};

static int32_t wrap_short_pic_num(uint32_t frame_num, uint32_t curr_frame_num,
				  uint32_t max_frame_num)
{
	return frame_num > curr_frame_num ? (int32_t)(frame_num - max_frame_num) :
					    (int32_t)frame_num;
}

static void dpb_update_pic_nums(struct lite_dpb *dpb, uint32_t curr_frame_num)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dpb->pics); i++) {
		struct lite_pic *pic = &dpb->pics[i];

		if (!pic->valid || !pic->reference)
			continue;

		if (pic->long_term)
			pic->pic_num = pic->long_term_pic_num;
		else
			pic->pic_num = wrap_short_pic_num(pic->frame_num,
							  curr_frame_num,
							  dpb->max_frame_num);
	}
}

static void capture_get(struct lite_v4l2 *dec, uint32_t index)
{
	if (index < dec->capture_count && dec->capture_refs[index] < UINT8_MAX)
		dec->capture_refs[index]++;
}

static void capture_put(struct lite_v4l2 *dec, uint32_t index)
{
	if (index < dec->capture_count && dec->capture_refs[index])
		dec->capture_refs[index]--;
}

static void scanout_hold_put_oldest(struct lite_kms *kms, struct lite_v4l2 *dec)
{
	uint32_t pos = kms->scanout_pos % ARRAY_SIZE(kms->scanout_hold);

	if (kms->scanout_valid[pos]) {
		capture_put(dec, kms->scanout_hold[pos]);
		kms->scanout_valid[pos] = false;
	}
}

static void scanout_hold_push(struct lite_kms *kms, struct lite_v4l2 *dec,
			      uint32_t capture_index)
{
	uint32_t pos = kms->scanout_pos % ARRAY_SIZE(kms->scanout_hold);

	scanout_hold_put_oldest(kms, dec);
	kms->scanout_hold[pos] = capture_index;
	kms->scanout_valid[pos] = true;
	kms->scanout_pos++;
}

static void scanout_hold_clear(struct lite_kms *kms, struct lite_v4l2 *dec)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(kms->scanout_hold); i++) {
		if (!kms->scanout_valid[i])
			continue;
		capture_put(dec, kms->scanout_hold[i]);
		kms->scanout_valid[i] = false;
	}
	kms->scanout_pos = 0;
}

static void dpb_release_pic(struct lite_dpb *dpb, struct lite_v4l2 *dec,
			    unsigned int idx)
{
	if (idx >= ARRAY_SIZE(dpb->pics) || !dpb->pics[idx].valid)
		return;

	capture_put(dec, dpb->pics[idx].capture_index);
	memset(&dpb->pics[idx], 0, sizeof(dpb->pics[idx]));
}

static void dpb_clear(struct lite_dpb *dpb, struct lite_v4l2 *dec)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dpb->pics); i++)
		dpb_release_pic(dpb, dec, i);
}

static unsigned int dpb_ref_count(const struct lite_dpb *dpb)
{
	unsigned int i, refs = 0;

	for (i = 0; i < ARRAY_SIZE(dpb->pics); i++)
		if (dpb->pics[i].valid && dpb->pics[i].reference)
			refs++;

	return refs;
}

static int dpb_find_unused(const struct lite_dpb *dpb)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dpb->pics); i++)
		if (!dpb->pics[i].valid)
			return i;

	return -1;
}

static struct lite_pic *dpb_find_short(struct lite_dpb *dpb, int32_t pic_num)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dpb->pics); i++) {
		struct lite_pic *pic = &dpb->pics[i];

		if (pic->valid && pic->reference && !pic->long_term &&
		    pic->pic_num == pic_num)
			return pic;
	}

	return NULL;
}

static struct lite_pic *dpb_find_long(struct lite_dpb *dpb,
				      int32_t long_term_pic_num)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dpb->pics); i++) {
		struct lite_pic *pic = &dpb->pics[i];

		if (pic->valid && pic->reference && pic->long_term &&
		    pic->long_term_pic_num == long_term_pic_num)
			return pic;
	}

	return NULL;
}

static int dpb_index_of(const struct lite_dpb *dpb, const struct lite_pic *pic)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dpb->pics); i++)
		if (&dpb->pics[i] == pic)
			return i;

	return -1;
}

static int cmp_short_desc(const void *a, const void *b)
{
	const struct ref_entry *ra = a;
	const struct ref_entry *rb = b;

	return rb->pic_num - ra->pic_num;
}

static int cmp_long_asc(const void *a, const void *b)
{
	const struct ref_entry *ra = a;
	const struct ref_entry *rb = b;

	return ra->long_term_pic_num - rb->long_term_pic_num;
}

static int cmp_poc_desc(const void *a, const void *b)
{
	const struct ref_entry *ra = a;
	const struct ref_entry *rb = b;

	return rb->pic->poc - ra->pic->poc;
}

static int cmp_poc_asc(const void *a, const void *b)
{
	const struct ref_entry *ra = a;
	const struct ref_entry *rb = b;

	return ra->pic->poc - rb->pic->poc;
}

static uint32_t collect_short_refs(struct lite_dpb *dpb,
				   struct ref_entry *out)
{
	uint32_t n = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dpb->pics); i++) {
		struct lite_pic *pic = &dpb->pics[i];

		if (pic->valid && pic->reference && !pic->long_term) {
			out[n++] = (struct ref_entry) {
				.pic = pic,
				.dpb_index = i,
				.pic_num = pic->pic_num,
			};
		}
	}

	return n;
}

static uint32_t collect_long_refs(struct lite_dpb *dpb,
				  struct ref_entry *out)
{
	uint32_t n = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dpb->pics); i++) {
		struct lite_pic *pic = &dpb->pics[i];

		if (pic->valid && pic->reference && pic->long_term) {
			out[n++] = (struct ref_entry) {
				.pic = pic,
				.dpb_index = i,
				.long_term_pic_num = pic->long_term_pic_num,
			};
		}
	}

	return n;
}

static void fill_v4l2_sps(struct v4l2_ctrl_h264_sps *out,
			  const GstH264SPS *sps)
{
	unsigned int i;

	*out = (struct v4l2_ctrl_h264_sps) {
		.profile_idc = sps->profile_idc,
		.constraint_set_flags = sps->constraint_set0_flag |
			(sps->constraint_set1_flag << 1) |
			(sps->constraint_set2_flag << 2) |
			(sps->constraint_set3_flag << 3) |
			(sps->constraint_set4_flag << 4) |
			(sps->constraint_set5_flag << 5),
		.level_idc = sps->level_idc,
		.seq_parameter_set_id = sps->id,
		.chroma_format_idc = sps->chroma_format_idc,
		.bit_depth_luma_minus8 = sps->bit_depth_luma_minus8,
		.bit_depth_chroma_minus8 = sps->bit_depth_chroma_minus8,
		.log2_max_frame_num_minus4 = sps->log2_max_frame_num_minus4,
		.pic_order_cnt_type = sps->pic_order_cnt_type,
		.log2_max_pic_order_cnt_lsb_minus4 =
			sps->log2_max_pic_order_cnt_lsb_minus4,
		.max_num_ref_frames = sps->num_ref_frames,
		.num_ref_frames_in_pic_order_cnt_cycle =
			sps->num_ref_frames_in_pic_order_cnt_cycle,
		.offset_for_non_ref_pic = sps->offset_for_non_ref_pic,
		.offset_for_top_to_bottom_field =
			sps->offset_for_top_to_bottom_field,
		.pic_width_in_mbs_minus1 = sps->pic_width_in_mbs_minus1,
		.pic_height_in_map_units_minus1 =
			sps->pic_height_in_map_units_minus1,
		.flags = (sps->separate_colour_plane_flag ?
			  V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE : 0) |
			 (sps->qpprime_y_zero_transform_bypass_flag ?
			  V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS : 0) |
			 (sps->delta_pic_order_always_zero_flag ?
			  V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO : 0) |
			 (sps->gaps_in_frame_num_value_allowed_flag ?
			  V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED : 0) |
			 (sps->frame_mbs_only_flag ?
			  V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY : 0) |
			 (sps->mb_adaptive_frame_field_flag ?
			  V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD : 0) |
			 (sps->direct_8x8_inference_flag ?
			  V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE : 0),
	};

	for (i = 0; i < sps->num_ref_frames_in_pic_order_cnt_cycle &&
		    i < ARRAY_SIZE(out->offset_for_ref_frame); i++)
		out->offset_for_ref_frame[i] = sps->offset_for_ref_frame[i];
}

static bool h264_scaling_matrix_present(const GstH264PPS *pps)
{
	return pps->sequence->scaling_matrix_present_flag ||
	       pps->pic_scaling_matrix_present_flag;
}

static void fill_v4l2_pps(struct v4l2_ctrl_h264_pps *out,
			  const GstH264PPS *pps, bool scaling_matrix_present)
{
	*out = (struct v4l2_ctrl_h264_pps) {
		.pic_parameter_set_id = pps->id,
		.seq_parameter_set_id = pps->sequence->id,
		.num_slice_groups_minus1 = pps->num_slice_groups_minus1,
		.num_ref_idx_l0_default_active_minus1 =
			pps->num_ref_idx_l0_active_minus1,
		.num_ref_idx_l1_default_active_minus1 =
			pps->num_ref_idx_l1_active_minus1,
		.weighted_bipred_idc = pps->weighted_bipred_idc,
		.pic_init_qp_minus26 = pps->pic_init_qp_minus26,
		.pic_init_qs_minus26 = pps->pic_init_qs_minus26,
		.chroma_qp_index_offset = pps->chroma_qp_index_offset,
		.second_chroma_qp_index_offset =
			pps->second_chroma_qp_index_offset,
		.flags = (pps->entropy_coding_mode_flag ?
			  V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE : 0) |
			 (pps->pic_order_present_flag ?
			  V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT : 0) |
			 (pps->weighted_pred_flag ?
			  V4L2_H264_PPS_FLAG_WEIGHTED_PRED : 0) |
			 (pps->deblocking_filter_control_present_flag ?
			  V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT : 0) |
			 (pps->constrained_intra_pred_flag ?
			  V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED : 0) |
			 (pps->redundant_pic_cnt_present_flag ?
			  V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT : 0) |
			 (pps->transform_8x8_mode_flag ?
			  V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE : 0) |
			 (scaling_matrix_present ?
			  V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT : 0),
	};
}

static void fill_v4l2_scaling(struct v4l2_ctrl_h264_scaling_matrix *out,
			      const GstH264PPS *pps)
{
	unsigned int i;
	unsigned int n;

	memset(out, 0, sizeof(*out));

	for (i = 0; i < ARRAY_SIZE(pps->scaling_lists_4x4); i++)
		gst_h264_quant_matrix_4x4_get_raster_from_zigzag(
			out->scaling_list_4x4[i], pps->scaling_lists_4x4[i]);

	n = pps->sequence->chroma_format_idc == 3 ? 6 : 2;
	for (i = 0; i < n; i++)
		gst_h264_quant_matrix_8x8_get_raster_from_zigzag(
			out->scaling_list_8x8[i], pps->scaling_lists_8x8[i]);
}

static void fill_v4l2_pred_weight(struct v4l2_ctrl_h264_pred_weights *out,
				  const GstH264SliceHdr *slice)
{
	unsigned int i, j;

	memset(out, 0, sizeof(*out));
	out->luma_log2_weight_denom =
		slice->pred_weight_table.luma_log2_weight_denom;
	out->chroma_log2_weight_denom =
		slice->pred_weight_table.chroma_log2_weight_denom;

	for (i = 0; i <= slice->num_ref_idx_l0_active_minus1; i++) {
		out->weight_factors[0].luma_weight[i] =
			slice->pred_weight_table.luma_weight_l0[i];
		out->weight_factors[0].luma_offset[i] =
			slice->pred_weight_table.luma_offset_l0[i];
	}

	if (slice->pps->sequence->chroma_array_type != 0) {
		for (i = 0; i <= slice->num_ref_idx_l0_active_minus1; i++) {
			for (j = 0; j < 2; j++) {
				out->weight_factors[0].chroma_weight[i][j] =
					slice->pred_weight_table.chroma_weight_l0[i][j];
				out->weight_factors[0].chroma_offset[i][j] =
					slice->pred_weight_table.chroma_offset_l0[i][j];
			}
		}
	}

	if (slice->type % 5 != GST_H264_B_SLICE)
		return;

	for (i = 0; i <= slice->num_ref_idx_l1_active_minus1; i++) {
		out->weight_factors[1].luma_weight[i] =
			slice->pred_weight_table.luma_weight_l1[i];
		out->weight_factors[1].luma_offset[i] =
			slice->pred_weight_table.luma_offset_l1[i];
	}

	if (slice->pps->sequence->chroma_array_type != 0) {
		for (i = 0; i <= slice->num_ref_idx_l1_active_minus1; i++) {
			for (j = 0; j < 2; j++) {
				out->weight_factors[1].chroma_weight[i][j] =
					slice->pred_weight_table.chroma_weight_l1[i][j];
				out->weight_factors[1].chroma_offset[i][j] =
					slice->pred_weight_table.chroma_offset_l1[i][j];
			}
		}
	}
}

static uint32_t h264_slice_header_bit_size(const GstH264NalUnit *nalu,
					   const GstH264SliceHdr *slice)
{
	return 8 * nalu->header_bytes + slice->header_size -
	       8 * slice->n_emulation_prevention_bytes;
}

static void fill_v4l2_slice_params(struct v4l2_ctrl_h264_slice_params *out,
				   const GstH264NalUnit *nalu,
				   const GstH264SliceHdr *slice)
{
	memset(out, 0, sizeof(*out));
	out->header_bit_size = h264_slice_header_bit_size(nalu, slice);
	out->first_mb_in_slice = slice->first_mb_in_slice;
	out->slice_type = slice->type % 5;
	out->colour_plane_id = slice->colour_plane_id;
	out->redundant_pic_cnt = slice->redundant_pic_cnt;
	out->cabac_init_idc = slice->cabac_init_idc;
	out->slice_qp_delta = slice->slice_qp_delta;
	out->slice_qs_delta = slice->slice_qs_delta;
	out->disable_deblocking_filter_idc =
		slice->disable_deblocking_filter_idc;
	out->slice_alpha_c0_offset_div2 = slice->slice_alpha_c0_offset_div2;
	out->slice_beta_offset_div2 = slice->slice_beta_offset_div2;
	out->num_ref_idx_l0_active_minus1 =
		slice->num_ref_idx_l0_active_minus1;
	out->num_ref_idx_l1_active_minus1 =
		slice->num_ref_idx_l1_active_minus1;
	out->flags = (slice->direct_spatial_mv_pred_flag ?
		      V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED : 0) |
		     (slice->sp_for_switch_flag ?
		      V4L2_H264_SLICE_FLAG_SP_FOR_SWITCH : 0);

	memset(out->ref_pic_list0, 0xff, sizeof(out->ref_pic_list0));
	memset(out->ref_pic_list1, 0xff, sizeof(out->ref_pic_list1));
}

static void poc_state_reset(struct poc_state *poc)
{
	memset(poc, 0, sizeof(*poc));
}

static void poc_type0_calc(const GstH264NalUnit *nalu,
			   const GstH264SliceHdr *slice,
			   const struct poc_state *state,
			   int32_t *top_poc, int32_t *bottom_poc,
			   int32_t *poc_msb)
{
	uint32_t max_poc_lsb = 1U << (slice->pps->sequence->log2_max_pic_order_cnt_lsb_minus4 + 4);
	uint32_t half_max_poc_lsb = max_poc_lsb / 2;
	uint32_t poc_lsb = slice->pic_order_cnt_lsb;
	int32_t msb;

	if (nalu->type == GST_H264_NAL_SLICE_IDR) {
		msb = 0;
	} else if (poc_lsb < state->prev_ref_poc_lsb &&
		   state->prev_ref_poc_lsb - poc_lsb >= half_max_poc_lsb) {
		msb = state->prev_ref_poc_msb + max_poc_lsb;
	} else if (poc_lsb > state->prev_ref_poc_lsb &&
		   poc_lsb - state->prev_ref_poc_lsb > half_max_poc_lsb) {
		msb = state->prev_ref_poc_msb - max_poc_lsb;
	} else {
		msb = state->prev_ref_poc_msb;
	}

	*top_poc = msb + (int32_t)poc_lsb;
	if (slice->field_pic_flag && slice->bottom_field_flag)
		*bottom_poc = msb + (int32_t)poc_lsb;
	else
		*bottom_poc = msb + (int32_t)poc_lsb +
			      slice->delta_pic_order_cnt_bottom;
	*poc_msb = msb;
}

static void poc_type2_calc(const GstH264NalUnit *nalu,
			   const GstH264SliceHdr *slice,
			   const struct poc_state *state,
			   int32_t *top_poc, int32_t *bottom_poc,
			   uint32_t *frame_num_offset)
{
	int32_t poc;

	if (nalu->type == GST_H264_NAL_SLICE_IDR) {
		*frame_num_offset = 0;
		poc = 0;
	} else {
		if (state->prev_frame_num > slice->frame_num)
			*frame_num_offset = state->prev_frame_num_offset +
					    slice->pps->sequence->max_frame_num;
		else
			*frame_num_offset = state->prev_frame_num_offset;

		poc = 2 * (int32_t)(*frame_num_offset + slice->frame_num);
		if (!nalu->ref_idc)
			poc--;
	}

	*top_poc = poc;
	*bottom_poc = poc;
}

static void poc_state_update_ref(struct poc_state *state,
				 const GstH264NalUnit *nalu,
				 const GstH264SliceHdr *slice,
				 int32_t poc_msb, uint32_t frame_num_offset)
{
	if (!nalu->ref_idc)
		goto update_frame_num;

	if (nalu->type == GST_H264_NAL_SLICE_IDR) {
		state->prev_ref_poc_msb = 0;
		state->prev_ref_poc_lsb = 0;
		goto update_frame_num;
	}

	if (slice->dec_ref_pic_marking.adaptive_ref_pic_marking_mode_flag) {
		unsigned int i;

		for (i = 0; i < slice->dec_ref_pic_marking.n_ref_pic_marking; i++) {
			const GstH264RefPicMarking *mmco =
				&slice->dec_ref_pic_marking.ref_pic_marking[i];

				if (mmco->memory_management_control_operation == 5) {
					state->prev_ref_poc_msb = 0;
					state->prev_ref_poc_lsb =
						slice->pic_order_cnt_lsb;
					goto update_frame_num;
				}
			}
		}

	state->prev_ref_poc_msb = poc_msb;
	state->prev_ref_poc_lsb = slice->pic_order_cnt_lsb;

update_frame_num:
	state->prev_frame_num = slice->frame_num;
	state->prev_frame_num_offset = frame_num_offset;
}

static void fill_v4l2_decode_params(struct v4l2_ctrl_h264_decode_params *out,
				    const GstH264NalUnit *nalu,
				    const GstH264SliceHdr *slice,
				    int32_t top_poc, int32_t bottom_poc)
{
	uint32_t stype = slice->type % 5;

	memset(out, 0, sizeof(*out));
	out->nal_ref_idc = nalu->ref_idc;
	out->frame_num = slice->frame_num;
	out->idr_pic_id = slice->idr_pic_id;
	out->pic_order_cnt_lsb = slice->pic_order_cnt_lsb;
	out->delta_pic_order_cnt_bottom = slice->delta_pic_order_cnt_bottom;
	out->delta_pic_order_cnt0 = slice->delta_pic_order_cnt[0];
	out->delta_pic_order_cnt1 = slice->delta_pic_order_cnt[1];
	out->dec_ref_pic_marking_bit_size = slice->dec_ref_pic_marking.bit_size;
	out->pic_order_cnt_bit_size = slice->pic_order_cnt_bit_size;
	out->slice_group_change_cycle = slice->slice_group_change_cycle;
	out->top_field_order_cnt = top_poc;
	out->bottom_field_order_cnt = bottom_poc;
	out->flags = (nalu->type == GST_H264_NAL_SLICE_IDR ?
		      V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC : 0) |
		     (slice->field_pic_flag ?
		      V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC : 0) |
		     (slice->bottom_field_flag ?
		      V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD : 0) |
		     (stype == GST_H264_P_SLICE ?
		      V4L2_H264_DECODE_PARAM_FLAG_PFRAME : 0) |
		     (stype == GST_H264_B_SLICE ?
		      V4L2_H264_DECODE_PARAM_FLAG_BFRAME : 0);
}

static uint32_t build_default_ref_list_p(struct lite_dpb *dpb,
					 struct ref_entry *list)
{
	struct ref_entry shorts[LITE_MAX_DPB];
	struct ref_entry longs[LITE_MAX_DPB];
	uint32_t n_short, n_long;

	n_short = collect_short_refs(dpb, shorts);
	n_long = collect_long_refs(dpb, longs);
	qsort(shorts, n_short, sizeof(shorts[0]), cmp_short_desc);
	qsort(longs, n_long, sizeof(longs[0]), cmp_long_asc);

	memcpy(list, shorts, n_short * sizeof(list[0]));
	memcpy(list + n_short, longs, n_long * sizeof(list[0]));
	return n_short + n_long;
}

static uint32_t build_default_ref_list_b(struct lite_dpb *dpb,
					 int32_t curr_poc,
					 struct ref_entry *list0,
					 struct ref_entry *list1)
{
	struct ref_entry before[LITE_MAX_DPB];
	struct ref_entry after[LITE_MAX_DPB];
	struct ref_entry longs[LITE_MAX_DPB];
	uint32_t n_before = 0, n_after = 0, n_long;
	uint32_t n0 = 0, n1 = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dpb->pics); i++) {
		struct lite_pic *pic = &dpb->pics[i];
		struct ref_entry entry;

		if (!pic->valid || !pic->reference || pic->long_term)
			continue;

		entry = (struct ref_entry) {
			.pic = pic,
			.dpb_index = i,
			.pic_num = pic->pic_num,
		};

		if (pic->poc < curr_poc)
			before[n_before++] = entry;
		else
			after[n_after++] = entry;
	}

	qsort(before, n_before, sizeof(before[0]), cmp_poc_desc);
	qsort(after, n_after, sizeof(after[0]), cmp_poc_asc);

	n_long = collect_long_refs(dpb, longs);
	qsort(longs, n_long, sizeof(longs[0]), cmp_long_asc);

	memcpy(list0 + n0, before, n_before * sizeof(list0[0]));
	n0 += n_before;
	memcpy(list0 + n0, after, n_after * sizeof(list0[0]));
	n0 += n_after;
	memcpy(list0 + n0, longs, n_long * sizeof(list0[0]));
	n0 += n_long;

	memcpy(list1 + n1, after, n_after * sizeof(list1[0]));
	n1 += n_after;
	memcpy(list1 + n1, before, n_before * sizeof(list1[0]));
	n1 += n_before;
	memcpy(list1 + n1, longs, n_long * sizeof(list1[0]));
	n1 += n_long;

	if (n1 > 1 && n1 == n0) {
		bool same = true;

		for (i = 0; i < n0; i++) {
			if (list0[i].pic != list1[i].pic) {
				same = false;
				break;
			}
		}

		if (same) {
			struct ref_entry tmp = list1[0];
			list1[0] = list1[1];
			list1[1] = tmp;
		}
	}

	return n0 > n1 ? n0 : n1;
}

static void ref_list_remove_duplicate(struct ref_entry *list, uint32_t *len,
				      uint32_t start, struct lite_pic *pic)
{
	uint32_t i;

	for (i = start; i < *len; i++) {
		if (list[i].pic == pic) {
			memmove(&list[i], &list[i + 1],
				(*len - i - 1) * sizeof(list[0]));
			(*len)--;
			return;
		}
	}
}

static int ref_list_insert(struct ref_entry *list, uint32_t *len,
			   uint32_t pos, struct ref_entry entry)
{
	if (!entry.pic)
		return -1;

	if (*len >= LITE_MAX_DPB)
		*len = LITE_MAX_DPB - 1;
	if (pos > *len)
		pos = *len;

	memmove(&list[pos + 1], &list[pos], (*len - pos) * sizeof(list[0]));
	list[pos] = entry;
	(*len)++;
	ref_list_remove_duplicate(list, len, pos + 1, entry.pic);
	return 0;
}

static int apply_ref_list_mods(struct lite_dpb *dpb,
			       const GstH264SliceHdr *slice,
			       bool list1,
			       struct ref_entry *list,
			       uint32_t *len)
{
	const GstH264RefPicListModification *mods;
	uint32_t n_mods;
	uint32_t ref_idx = 0;
	int32_t pic_num_pred = slice->frame_num;
	uint32_t i;

	if (list1) {
		mods = slice->ref_pic_list_modification_l1;
		n_mods = slice->n_ref_pic_list_modification_l1;
		if (!slice->ref_pic_list_modification_flag_l1)
			return 0;
	} else {
		mods = slice->ref_pic_list_modification_l0;
		n_mods = slice->n_ref_pic_list_modification_l0;
		if (!slice->ref_pic_list_modification_flag_l0)
			return 0;
	}

	for (i = 0; i < n_mods; i++) {
		const GstH264RefPicListModification *mod = &mods[i];
		struct lite_pic *pic = NULL;
		struct ref_entry entry = { 0 };
		int32_t pic_num;

		if (mod->modification_of_pic_nums_idc == 3)
			break;

		switch (mod->modification_of_pic_nums_idc) {
		case 0:
		case 1: {
			int32_t abs_diff = mod->value.abs_diff_pic_num_minus1 + 1;
			int32_t pic_num_no_wrap;

			if (mod->modification_of_pic_nums_idc == 0) {
				pic_num_no_wrap = pic_num_pred - abs_diff;
				if (pic_num_no_wrap < 0)
					pic_num_no_wrap += dpb->max_frame_num;
			} else {
				pic_num_no_wrap = pic_num_pred + abs_diff;
				if (pic_num_no_wrap >= (int32_t)dpb->max_frame_num)
					pic_num_no_wrap -= dpb->max_frame_num;
			}

			pic_num_pred = pic_num_no_wrap;
			pic_num = pic_num_no_wrap > (int32_t)slice->frame_num ?
				  pic_num_no_wrap - (int32_t)dpb->max_frame_num :
				  pic_num_no_wrap;
			pic = dpb_find_short(dpb, pic_num);
			entry.pic = pic;
			entry.dpb_index = pic ? dpb_index_of(dpb, pic) : -1;
			entry.pic_num = pic_num;
			break;
		}
		case 2:
			pic = dpb_find_long(dpb, mod->value.long_term_pic_num);
			entry.pic = pic;
			entry.dpb_index = pic ? dpb_index_of(dpb, pic) : -1;
			entry.long_term_pic_num = mod->value.long_term_pic_num;
			break;
		default:
			fprintf(stderr, "unsupported ref list modification idc=%u\n",
				mod->modification_of_pic_nums_idc);
			return -1;
		}

		if (ref_list_insert(list, len, ref_idx, entry))
			return -1;
		ref_idx++;
	}

	return 0;
}

static void trim_ref_list(uint32_t *len, uint32_t active)
{
	if (*len > active)
		*len = active;
}

static int build_ref_lists(struct lite_dpb *dpb,
			   const GstH264SliceHdr *slice,
			   int32_t curr_poc,
			   struct ref_entry *list0, uint32_t *len0,
			   struct ref_entry *list1, uint32_t *len1)
{
	uint32_t stype = slice->type % 5;

	*len0 = 0;
	*len1 = 0;

	if (stype == GST_H264_I_SLICE)
		return 0;

	if (stype == GST_H264_P_SLICE || stype == GST_H264_SP_SLICE) {
		*len0 = build_default_ref_list_p(dpb, list0);
		if (apply_ref_list_mods(dpb, slice, false, list0, len0))
			return -1;
		trim_ref_list(len0, slice->num_ref_idx_l0_active_minus1 + 1);
		return 0;
	}

	if (stype == GST_H264_B_SLICE) {
		build_default_ref_list_b(dpb, curr_poc, list0, list1);
		*len0 = dpb_ref_count(dpb);
		*len1 = dpb_ref_count(dpb);
		if (apply_ref_list_mods(dpb, slice, false, list0, len0) ||
		    apply_ref_list_mods(dpb, slice, true, list1, len1))
			return -1;
		trim_ref_list(len0, slice->num_ref_idx_l0_active_minus1 + 1);
		trim_ref_list(len1, slice->num_ref_idx_l1_active_minus1 + 1);
		return 0;
	}

	fprintf(stderr, "unsupported slice type=%u\n", stype);
	return -1;
}

static void fill_decode_dpb(struct lite_dpb *dpb,
			    struct v4l2_ctrl_h264_decode_params *decode,
			    struct ref_entry *map, uint32_t *map_len)
{
	unsigned int i;

	memset(decode->dpb, 0, sizeof(decode->dpb));
	*map_len = 0;

	for (i = 0; i < ARRAY_SIZE(dpb->pics) &&
		    *map_len < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		struct lite_pic *pic = &dpb->pics[i];
		struct v4l2_h264_dpb_entry *entry;

		if (!pic->valid || !pic->reference)
			continue;

		entry = &decode->dpb[*map_len];
		entry->reference_ts = (uint64_t)pic->frame_id * 1000;
		entry->pic_num = pic->long_term ? pic->long_term_pic_num :
						  pic->pic_num;
		entry->frame_num = pic->long_term ?
				    (uint16_t)pic->long_term_frame_idx :
				    (uint16_t)pic->frame_num;
		entry->fields = V4L2_H264_FRAME_REF;
		entry->top_field_order_cnt = pic->top_poc;
		entry->bottom_field_order_cnt = pic->bottom_poc;
		entry->flags = V4L2_H264_DPB_ENTRY_FLAG_VALID |
			       V4L2_H264_DPB_ENTRY_FLAG_ACTIVE |
			       (pic->long_term ?
				V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM : 0);

		map[*map_len] = (struct ref_entry) {
			.pic = pic,
			.dpb_index = *map_len,
		};
		(*map_len)++;
	}
}

static uint8_t v4l2_dpb_index_for_pic(const struct ref_entry *map,
				      uint32_t map_len,
				      const struct lite_pic *pic)
{
	uint32_t i;

	for (i = 0; i < map_len; i++)
		if (map[i].pic == pic)
			return map[i].dpb_index;

	return 0xff;
}

static void fill_v4l2_ref_list(struct v4l2_h264_reference *out,
			       const struct ref_entry *list, uint32_t len,
			       const struct ref_entry *map, uint32_t map_len)
{
	uint32_t i;

	memset(out, 0xff, sizeof(struct v4l2_h264_reference) *
			    V4L2_H264_REF_LIST_LEN);
	for (i = 0; i < len && i < V4L2_H264_REF_LIST_LEN; i++) {
		out[i].index = v4l2_dpb_index_for_pic(map, map_len, list[i].pic);
		out[i].fields = V4L2_H264_FRAME_REF;
	}
}

static void dpb_mark_short_unused(struct lite_dpb *dpb, struct lite_v4l2 *dec,
				  int32_t pic_num)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dpb->pics); i++) {
		struct lite_pic *pic = &dpb->pics[i];

		if (pic->valid && pic->reference && !pic->long_term &&
		    pic->pic_num == pic_num) {
			dpb_release_pic(dpb, dec, i);
			return;
		}
	}
}

static void dpb_mark_long_unused(struct lite_dpb *dpb, struct lite_v4l2 *dec,
				 int32_t long_term_pic_num)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dpb->pics); i++) {
		struct lite_pic *pic = &dpb->pics[i];

		if (pic->valid && pic->reference && pic->long_term &&
		    pic->long_term_pic_num == long_term_pic_num) {
			dpb_release_pic(dpb, dec, i);
			return;
		}
	}
}

static void dpb_mark_long_over_max(struct lite_dpb *dpb, struct lite_v4l2 *dec)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dpb->pics); i++) {
		struct lite_pic *pic = &dpb->pics[i];

		if (pic->valid && pic->reference && pic->long_term &&
		    pic->long_term_frame_idx > dpb->max_long_term_frame_idx)
			dpb_release_pic(dpb, dec, i);
	}
}

static void dpb_sliding_window(struct lite_dpb *dpb, struct lite_v4l2 *dec)
{
	int remove = -1;
	int32_t lowest_pic_num = INT32_MAX;
	unsigned int i;

	if (!dpb->max_num_ref_frames ||
	    dpb_ref_count(dpb) < dpb->max_num_ref_frames)
		return;

	for (i = 0; i < ARRAY_SIZE(dpb->pics); i++) {
		struct lite_pic *pic = &dpb->pics[i];

		if (pic->valid && pic->reference && !pic->long_term &&
		    pic->pic_num < lowest_pic_num) {
			lowest_pic_num = pic->pic_num;
			remove = i;
		}
	}

	if (remove >= 0)
		dpb_release_pic(dpb, dec, remove);
}

static int32_t mmco_short_pic_num(uint32_t curr_frame_num,
				  uint32_t max_frame_num,
				  uint32_t difference_of_pic_nums_minus1)
{
	int32_t pic_num_no_wrap;

	pic_num_no_wrap = (int32_t)curr_frame_num -
			  (int32_t)(difference_of_pic_nums_minus1 + 1);
	if (pic_num_no_wrap < 0)
		pic_num_no_wrap += max_frame_num;

	return pic_num_no_wrap > (int32_t)curr_frame_num ?
	       pic_num_no_wrap - (int32_t)max_frame_num : pic_num_no_wrap;
}

static int dpb_store_current(struct lite_dpb *dpb, struct lite_v4l2 *dec,
			     const GstH264NalUnit *nalu,
			     const GstH264SliceHdr *slice,
			     uint32_t frame_id, uint32_t capture_index,
			     int32_t top_poc, int32_t bottom_poc)
{
	bool make_long = false;
	int32_t long_idx = -1;
	int slot;
	unsigned int i;

	if (!nalu->ref_idc) {
		capture_put(dec, capture_index);
		return 0;
	}

	if (nalu->type == GST_H264_NAL_SLICE_IDR) {
		dpb_clear(dpb, dec);
		dpb->max_long_term_frame_idx =
			slice->dec_ref_pic_marking.long_term_reference_flag ? 0 : -1;
		make_long = slice->dec_ref_pic_marking.long_term_reference_flag;
		long_idx = 0;
	} else {
		if (slice->dec_ref_pic_marking.adaptive_ref_pic_marking_mode_flag) {
			for (i = 0; i < slice->dec_ref_pic_marking.n_ref_pic_marking; i++) {
				const GstH264RefPicMarking *mmco =
					&slice->dec_ref_pic_marking.ref_pic_marking[i];
				int32_t pic_num;

				switch (mmco->memory_management_control_operation) {
				case 0:
					break;
				case 1:
					pic_num = mmco_short_pic_num(slice->frame_num,
								     dpb->max_frame_num,
								     mmco->difference_of_pic_nums_minus1);
					dpb_mark_short_unused(dpb, dec, pic_num);
					break;
				case 2:
					dpb_mark_long_unused(dpb, dec,
							     mmco->long_term_pic_num);
					break;
				case 3: {
					struct lite_pic *pic;

					pic_num = mmco_short_pic_num(slice->frame_num,
								     dpb->max_frame_num,
								     mmco->difference_of_pic_nums_minus1);
					pic = dpb_find_short(dpb, pic_num);
					if (pic) {
						pic->long_term = true;
						pic->long_term_frame_idx =
							mmco->long_term_frame_idx;
						pic->long_term_pic_num =
							mmco->long_term_frame_idx;
					}
					break;
				}
				case 4:
					dpb->max_long_term_frame_idx =
						(int32_t)mmco->max_long_term_frame_idx_plus1 - 1;
					dpb_mark_long_over_max(dpb, dec);
					break;
				case 5:
					dpb_clear(dpb, dec);
					dpb->max_long_term_frame_idx = -1;
					break;
				case 6:
					make_long = true;
					long_idx = mmco->long_term_frame_idx;
					break;
				default:
					fprintf(stderr, "unsupported MMCO op=%u\n",
						mmco->memory_management_control_operation);
					return -1;
				}
			}
		} else {
			dpb_sliding_window(dpb, dec);
		}
	}

	slot = dpb_find_unused(dpb);
	if (slot < 0) {
		dpb_sliding_window(dpb, dec);
		slot = dpb_find_unused(dpb);
	}
	if (slot < 0) {
		fprintf(stderr, "DPB full, cannot store reference frame\n");
		return -1;
	}

	dpb->pics[slot] = (struct lite_pic) {
		.valid = true,
		.reference = true,
		.long_term = make_long,
		.frame_num = make_long ? (uint32_t)long_idx : slice->frame_num,
		.pic_num = make_long ? long_idx : (int32_t)slice->frame_num,
		.long_term_pic_num = make_long ? long_idx : -1,
		.long_term_frame_idx = make_long ? long_idx : -1,
		.top_poc = top_poc,
		.bottom_poc = bottom_poc,
		.poc = top_poc < bottom_poc ? top_poc : bottom_poc,
		.frame_id = frame_id,
		.capture_index = capture_index,
	};

	capture_get(dec, capture_index);
	capture_put(dec, capture_index);
	return 0;
}

static int lite_capture_alloc(struct lite_v4l2 *dec)
{
	unsigned int i;

	for (i = 0; i < dec->capture_count; i++) {
		if (!dec->capture_refs[i]) {
			dec->capture_refs[i] = 1;
			return i;
		}
	}

	return -1;
}

static uint32_t max_sample_size(const struct video_track *t)
{
	uint32_t max = 0;
	uint32_t i;

	for (i = 0; i < t->sample_count; i++) {
		if (t->samples[i].size > max)
			max = t->samples[i].size;
	}

	return max;
}

static void print_v4l2_capabilities(int fd)
{
	struct v4l2_capability cap = { 0 };
	uint32_t caps;

	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		fprintf(stderr, "v4l2 QUERYCAP failed: %s\n", strerror(errno));
		return;
	}

	caps = cap.device_caps ? cap.device_caps : cap.capabilities;
	fprintf(stderr,
		"v4l2 driver=%s card=%s bus=%s caps=0x%08x device_caps=0x%08x active=0x%08x\n",
		cap.driver, cap.card, cap.bus_info, cap.capabilities,
		cap.device_caps, caps);
}

static bool enum_v4l2_formats(int fd, enum v4l2_buf_type type,
			      uint32_t want, const char *label)
{
	bool found = false;
	uint32_t i;
	char want_buf[5];

	for (i = 0; ; i++) {
		struct v4l2_fmtdesc desc = {
			.index = i,
			.type = type,
		};
		char fmt_buf[5];

		if (xioctl(fd, VIDIOC_ENUM_FMT, &desc) < 0) {
			if (errno != EINVAL)
				fprintf(stderr, "v4l2 ENUM_FMT %s failed: %s\n",
					label, strerror(errno));
			break;
		}

		fprintf(stderr, "v4l2 %s fmt[%u]=%s (%s) flags=0x%x\n",
			label, i, pixfmt_str(desc.pixelformat, fmt_buf),
			desc.description, desc.flags);
		if (desc.pixelformat == want)
			found = true;
	}

	if (!found)
		fprintf(stderr, "v4l2 %s missing required format %s\n",
			label, pixfmt_str(want, want_buf));

	return found;
}

static int set_v4l2_format(int fd, enum v4l2_buf_type type, uint32_t pixfmt,
			   uint32_t width, uint32_t height, uint32_t sizeimage,
			   struct v4l2_pix_format *applied,
			   const char *label, bool verbose)
{
	struct v4l2_format fmt = { 0 };
	char want_buf[5], got_buf[5];

	fmt.type = type;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	fmt.fmt.pix.pixelformat = pixfmt;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.sizeimage = sizeimage;

	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		fprintf(stderr, "v4l2 S_FMT %s %s %ux%u failed: %s\n",
			label, pixfmt_str(pixfmt, want_buf), width, height,
			strerror(errno));
		return -1;
	}

	if (verbose)
		fprintf(stderr,
			"v4l2 %s S_FMT want=%s got=%s %ux%u bytesperline=%u sizeimage=%u\n",
			label, pixfmt_str(pixfmt, want_buf),
			pixfmt_str(fmt.fmt.pix.pixelformat, got_buf),
			fmt.fmt.pix.width, fmt.fmt.pix.height,
			fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);

	if (fmt.fmt.pix.pixelformat != pixfmt ||
	    fmt.fmt.pix.width < width || fmt.fmt.pix.height < height) {
		fprintf(stderr, "v4l2 S_FMT %s did not keep requested format\n",
			label);
		return -1;
	}

	if (applied)
		*applied = fmt.fmt.pix;

	return 0;
}

static int request_v4l2_buffers_probe(int fd, enum v4l2_buf_type type,
				      uint32_t count, const char *label)
{
	struct v4l2_requestbuffers req = {
		.count = count,
		.type = type,
		.memory = V4L2_MEMORY_MMAP,
	};

	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		fprintf(stderr, "v4l2 REQBUFS %s count=%u failed: %s\n",
			label, count, strerror(errno));
		return -1;
	}

	fprintf(stderr, "v4l2 %s REQBUFS requested=%u got=%u caps=0x%x\n",
		label, count, req.count, req.capabilities);

	return req.count ? 0 : -1;
}

static void release_v4l2_buffers(int fd, enum v4l2_buf_type type,
				 const char *label)
{
	struct v4l2_requestbuffers release = {
		.count = 0,
		.type = type,
		.memory = V4L2_MEMORY_MMAP,
	};

	if (xioctl(fd, VIDIOC_REQBUFS, &release) < 0)
		fprintf(stderr, "v4l2 REQBUFS %s release failed: %s\n",
			label, strerror(errno));
}

static void probe_h264_controls(int fd)
{
	static const struct {
		uint32_t id;
		const char *name;
		size_t expect;
		bool optional;
	} ctrls[] = {
		{ V4L2_CID_STATELESS_H264_DECODE_MODE, "DECODE_MODE", 0, false },
		{ V4L2_CID_STATELESS_H264_START_CODE, "START_CODE", 0, false },
		{ V4L2_CID_STATELESS_H264_SPS, "SPS",
		  sizeof(struct v4l2_ctrl_h264_sps), false },
		{ V4L2_CID_STATELESS_H264_PPS, "PPS",
		  sizeof(struct v4l2_ctrl_h264_pps), false },
		{ V4L2_CID_STATELESS_H264_SCALING_MATRIX, "SCALING_MATRIX",
		  sizeof(struct v4l2_ctrl_h264_scaling_matrix), true },
		{ V4L2_CID_STATELESS_H264_DECODE_PARAMS, "DECODE_PARAMS",
		  sizeof(struct v4l2_ctrl_h264_decode_params), false },
		{ V4L2_CID_STATELESS_H264_SLICE_PARAMS, "SLICE_PARAMS",
		  sizeof(struct v4l2_ctrl_h264_slice_params), false },
		{ V4L2_CID_STATELESS_H264_PRED_WEIGHTS, "PRED_WEIGHTS",
		  sizeof(struct v4l2_ctrl_h264_pred_weights), true },
	};
	struct v4l2_ext_control values[2] = {
		{ .id = V4L2_CID_STATELESS_H264_DECODE_MODE },
		{ .id = V4L2_CID_STATELESS_H264_START_CODE },
	};
	struct v4l2_ext_controls ext = {
		.count = ARRAY_SIZE(values),
		.controls = values,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ctrls); i++) {
		struct v4l2_query_ext_ctrl q = {
			.id = ctrls[i].id,
		};
		uint64_t bytes;

		if (xioctl(fd, VIDIOC_QUERY_EXT_CTRL, &q) < 0) {
			fprintf(stderr, "v4l2 ctrl %-14s missing%s: %s\n",
				ctrls[i].name, ctrls[i].optional ? " optional" : "",
				strerror(errno));
			continue;
		}

		bytes = (uint64_t)q.elem_size * q.elems;
		fprintf(stderr,
			"v4l2 ctrl %-14s type=0x%x elem=%u elems=%u bytes=%" PRIu64 " expect=%zu flags=0x%x\n",
			ctrls[i].name, q.type, q.elem_size, q.elems, bytes,
			ctrls[i].expect, q.flags);
	}

	if (xioctl(fd, VIDIOC_G_EXT_CTRLS, &ext) == 0)
		fprintf(stderr, "v4l2 h264 decode_mode=%d start_code=%d\n",
			values[0].value, values[1].value);
	else
		fprintf(stderr, "v4l2 G_EXT_CTRLS decode mode/start code failed: %s\n",
			strerror(errno));
}

static int probe_media_path(const char *path)
{
	struct media_device_info info = { 0 };
	int media_fd, request_fd = -1;
	int ret = -1;

	media_fd = open(path, O_RDWR | O_CLOEXEC);
	if (media_fd < 0)
		return -1;

	if (xioctl(media_fd, MEDIA_IOC_DEVICE_INFO, &info) == 0)
		fprintf(stderr,
			"media node=%s driver=%s model=%s bus=%s version=0x%x\n",
			path, info.driver, info.model, info.bus_info,
			info.media_version);
	else
		fprintf(stderr, "media node=%s DEVICE_INFO failed: %s\n",
			path, strerror(errno));

	if (xioctl(media_fd, MEDIA_IOC_REQUEST_ALLOC, &request_fd) < 0) {
		fprintf(stderr, "media node=%s REQUEST_ALLOC failed: %s\n",
			path, strerror(errno));
		goto out;
	}

	fprintf(stderr, "media node=%s request_alloc fd=%d ok\n", path, request_fd);
	close(request_fd);
	ret = 0;
out:
	close(media_fd);
	return ret;
}

static int probe_media_request(const char *forced)
{
	char path[32];
	unsigned int i;

	if (forced)
		return probe_media_path(forced);

	for (i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "/dev/media%u", i);
		if (probe_media_path(path) == 0)
			return 0;
	}

	fprintf(stderr, "media request probe failed on /dev/media0..7\n");
	return -1;
}

static int open_media_path(const char *path, bool verbose)
{
	struct media_device_info info = { 0 };
	int fd;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;

	if (xioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) < 0) {
		fprintf(stderr, "media node=%s DEVICE_INFO failed: %s\n",
			path, strerror(errno));
		close(fd);
		return -1;
	}

	if (strcmp((const char *)info.driver, "cedrus")) {
		close(fd);
		return -1;
	}

	if (verbose)
		fprintf(stderr, "media node=%s driver=%s model=%s\n",
			path, info.driver, info.model);
	return fd;
}

static int open_media_auto(const char *forced, bool verbose)
{
	char path[32];
	unsigned int i;
	int fd;

	if (forced)
		return open_media_path(forced, verbose);

	for (i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "/dev/media%u", i);
		fd = open_media_path(path, verbose);
		if (fd >= 0)
			return fd;
	}

	fprintf(stderr, "failed to open cedrus media node\n");
	return -1;
}

static int set_current_sps_control(int fd,
				   struct v4l2_ctrl_h264_sps *sps)
{
	struct v4l2_ext_control ctrl = {
		.id = V4L2_CID_STATELESS_H264_SPS,
		.size = sizeof(*sps),
		.ptr = sps,
	};
	struct v4l2_ext_controls ctrls = {
		.count = 1,
		.controls = &ctrl,
	};

	if (xioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) < 0) {
		fprintf(stderr, "v4l2 current SPS control failed idx=%u: %s\n",
			ctrls.error_idx, strerror(errno));
		return -1;
	}

	return 0;
}

static int reqbufs_exact(int fd, enum v4l2_buf_type type, uint32_t count,
			 const char *label)
{
	struct v4l2_requestbuffers req = {
		.count = count,
		.type = type,
		.memory = V4L2_MEMORY_MMAP,
	};

	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		fprintf(stderr, "v4l2 REQBUFS %s failed: %s\n",
			label, strerror(errno));
		return -1;
	}

	if (req.count < count) {
		fprintf(stderr, "v4l2 REQBUFS %s got %u < %u\n",
			label, req.count, count);
		return -1;
	}

	return 0;
}

static int reqbufs_at_least(int fd, enum v4l2_buf_type type,
			    uint32_t requested, uint32_t minimum,
			    uint32_t *actual, const char *label, bool verbose)
{
	struct v4l2_requestbuffers req = {
		.count = requested,
		.type = type,
		.memory = V4L2_MEMORY_MMAP,
	};

	if (minimum > requested)
		minimum = requested;

	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		fprintf(stderr, "v4l2 REQBUFS %s requested=%u failed: %s\n",
			label, requested, strerror(errno));
		return -1;
	}

	if (req.count < minimum) {
		fprintf(stderr, "v4l2 REQBUFS %s got %u < minimum %u\n",
			label, req.count, minimum);
		return -1;
	}

	*actual = req.count;
	if (verbose || req.count < requested)
		fprintf(stderr, "v4l2 %s buffers requested=%u got=%u minimum=%u\n",
			label, requested, req.count, minimum);

	return 0;
}

static int map_one_buffer(int fd, enum v4l2_buf_type type, uint32_t index,
			  struct mapped_buffer *map, const char *label,
			  bool verbose)
{
	struct v4l2_buffer buf = {
		.type = type,
		.memory = V4L2_MEMORY_MMAP,
		.index = index,
	};

	if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
		fprintf(stderr, "v4l2 QUERYBUF %s failed: %s\n",
			label, strerror(errno));
		return -1;
	}

	map->length = buf.length;
	map->addr = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
			 fd, buf.m.offset);
	if (map->addr == MAP_FAILED) {
		fprintf(stderr, "v4l2 mmap %s length=%u failed: %s\n",
			label, buf.length, strerror(errno));
		map->addr = NULL;
		map->length = 0;
		return -1;
	}

	if (verbose)
		fprintf(stderr, "v4l2 %s mmap length=%u offset=%u\n",
			label, buf.length, buf.m.offset);
	return 0;
}

static int stream_set(int fd, enum v4l2_buf_type type, bool on,
		      const char *label)
{
	if (xioctl(fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type) < 0) {
		fprintf(stderr, "v4l2 STREAM%s %s failed: %s\n",
			on ? "ON" : "OFF", label, strerror(errno));
		return -1;
	}
	return 0;
}

static void lite_v4l2_close(struct lite_v4l2 *dec)
{
	struct v4l2_requestbuffers req;
	unsigned int i;

	if (dec->video_fd >= 0) {
		if (dec->stream_capture)
			stream_set(dec->video_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
				   false, "capture");
		if (dec->stream_output)
			stream_set(dec->video_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
				   false, "output");
	}

	if (dec->output.addr)
		munmap(dec->output.addr, dec->output.length);
	for (i = 0; i < dec->capture_count; i++) {
		if (dec->capture[i].addr)
			munmap(dec->capture[i].addr, dec->capture[i].length);
	}

	if (dec->video_fd >= 0) {
		memset(&req, 0, sizeof(req));
		req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		req.memory = V4L2_MEMORY_MMAP;
		xioctl(dec->video_fd, VIDIOC_REQBUFS, &req);

		memset(&req, 0, sizeof(req));
		req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		req.memory = V4L2_MEMORY_MMAP;
		xioctl(dec->video_fd, VIDIOC_REQBUFS, &req);

		close(dec->video_fd);
	}

	if (dec->media_fd >= 0)
		close(dec->media_fd);
}

static int lite_v4l2_setup(struct lite_v4l2 *dec, const struct options *opt,
			   const struct video_track *track,
			   const struct h264_stream_info *info,
	struct v4l2_ctrl_h264_sps *sps)
{
	uint32_t coded_width = h264_sps_coded_width(sps);
	uint32_t coded_height = h264_sps_coded_height(sps);
	uint32_t min_capture;
	uint32_t request_capture;
	memset(dec, 0, sizeof(*dec));
	dec->video_fd = -1;
	dec->media_fd = -1;
	dec->output_size = max_sample_size(track);
	if (dec->output_size < H264_OUTPUT_SIZE_MIN)
		dec->output_size = H264_OUTPUT_SIZE_MIN;

	dec->video_fd = open(opt->video_dev, O_RDWR | O_CLOEXEC);
	if (dec->video_fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", opt->video_dev,
			strerror(errno));
		goto fail;
	}

	dec->media_fd = open_media_auto(opt->media_dev, false);
	if (dec->media_fd < 0)
		goto fail;

	if (set_v4l2_format(dec->video_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
			    V4L2_PIX_FMT_H264_SLICE, coded_width,
			    coded_height, dec->output_size, NULL, "output",
			    false))
		goto fail;

	if (set_current_sps_control(dec->video_fd, sps))
		goto fail;

	if (set_v4l2_format(dec->video_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
			    V4L2_PIX_FMT_SUNXI_TILED_NV12, coded_width,
			    coded_height, 0, &dec->capture_fmt, "capture",
			    false))
		goto fail;

	min_capture = min_capture_buffers(info, sps->max_num_ref_frames,
					  opt->display);
	request_capture = min_capture + read_capture_extra_buffers();
	if (request_capture > LITE_CAPTURE_BUFFERS)
		request_capture = LITE_CAPTURE_BUFFERS;
	request_capture = clamp_capture_request_to_cma(coded_width,
						       coded_height,
						       min_capture,
						       request_capture,
						       dec->output_size,
						       false);

	if (check_capture_memory_budget(coded_width, coded_height, min_capture,
					dec->output_size))
		goto fail;

	if (reqbufs_exact(dec->video_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
			  1, "output"))
		goto fail;
	if (reqbufs_at_least(dec->video_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
			     request_capture, min_capture,
			     &dec->capture_count, "capture", false))
		goto fail;

	if (map_one_buffer(dec->video_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 0,
			   &dec->output, "output", false))
		goto fail;

	for (uint32_t i = 0; i < dec->capture_count; i++) {
		char label[32];

		snprintf(label, sizeof(label), "capture%u", i);
		if (map_one_buffer(dec->video_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
				   i, &dec->capture[i], label,
				   false))
			goto fail;
	}

	if (stream_set(dec->video_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, true,
		       "output"))
		goto fail;
	dec->stream_output = true;

	if (stream_set(dec->video_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, true,
		       "capture"))
		goto fail;
	dec->stream_capture = true;

	return 0;

fail:
	lite_v4l2_close(dec);
	memset(dec, 0, sizeof(*dec));
	dec->video_fd = -1;
	dec->media_fd = -1;
	return -1;
}

static uint32_t track_frame_delay_ms(const struct video_track *track)
{
	uint32_t ticks = track->sample_count ? track->samples[0].duration : 0;
	uint64_t delay;

	if (!track->timescale || !ticks)
		return 33;

	delay = ((uint64_t)ticks * 1000 + track->timescale / 2) /
		track->timescale;
	return delay ? delay : 1;
}

static int lite_kms_export_fb(struct lite_kms *kms, struct lite_v4l2 *dec,
			      uint32_t index)
{
	struct v4l2_exportbuffer exp = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.index = index,
		.flags = O_CLOEXEC,
	};
	struct drm_mode_fb_cmd2 fb = {
		.width = kms->fb_width,
		.height = kms->fb_height,
		.pixel_format = DRM_FORMAT_NV12,
		.flags = DRM_MODE_FB_MODIFIERS,
	};
	uint32_t y_size = kms->stride * kms->coded_height;
	struct kms_fb *out = &kms->fb[index];
	int ret;

	if (xioctl(dec->video_fd, VIDIOC_EXPBUF, &exp) < 0) {
		fprintf(stderr, "v4l2 EXPBUF capture%u failed: %s\n",
			index, strerror(errno));
		return -1;
	}

	out->prime_fd = exp.fd;
	ret = prime_fd_to_handle(kms->fd, out->prime_fd, &out->handle);
	if (ret) {
		fprintf(stderr, "PRIME_FD_TO_HANDLE capture%u fd=%d failed: %s\n",
			index, out->prime_fd, strerror(-ret));
		goto fail;
	}

	fb.handles[0] = out->handle;
	fb.handles[1] = out->handle;
	fb.pitches[0] = kms->stride;
	fb.pitches[1] = kms->stride;
	fb.offsets[0] = 0;
	fb.offsets[1] = y_size;
	fb.modifier[0] = DRM_FORMAT_MOD_ALLWINNER_TILED;
	fb.modifier[1] = DRM_FORMAT_MOD_ALLWINNER_TILED;

	if (drm_ioctl(kms->fd, DRM_IOCTL_MODE_ADDFB2, &fb,
		      "DRM_IOCTL_MODE_ADDFB2"))
		goto fail;

	out->fb_id = fb.fb_id;
	return 0;

fail:
	if (out->handle) {
		close_gem_handle(kms->fd, out->handle);
		out->handle = 0;
	}
	if (out->prime_fd >= 0) {
		close(out->prime_fd);
		out->prime_fd = -1;
	}
	return -1;
}

static void lite_kms_close(struct lite_kms *kms)
{
	unsigned int i;

	if (kms->fd >= 0 && kms->atomic_pending)
		wait_atomic_event(kms, 1000);

	if (kms->fd >= 0) {
		for (i = 0; i < ARRAY_SIZE(kms->fb); i++) {
			if (kms->fb[i].fb_id)
				ioctl(kms->fd, DRM_IOCTL_MODE_RMFB,
				      &kms->fb[i].fb_id);
			close_gem_handle(kms->fd, kms->fb[i].handle);
			if (kms->fb[i].prime_fd >= 0)
				close(kms->fb[i].prime_fd);
		}
		close(kms->fd);
	}

	memset(kms, 0, sizeof(*kms));
	kms->fd = -1;
}

static int lite_kms_setup(struct lite_kms *kms, const struct options *opt,
			  const struct video_track *track,
			  const struct h264_stream_info *info,
			  struct lite_v4l2 *dec)
{
	const char *pace;
	const char *rotation;
	uint32_t plane_src_x, plane_src_y, plane_src_w, plane_src_h;
	uint32_t plane_dst_x, plane_dst_y, plane_dst_w, plane_dst_h;
	uint64_t src_right, src_bottom;
	unsigned int i;

	memset(kms, 0, sizeof(*kms));
	kms->fd = -1;
	for (i = 0; i < ARRAY_SIZE(kms->fb); i++)
		kms->fb[i].prime_fd = -1;

	kms->src_x = info->crop_x;
	kms->src_y = info->crop_y;
	kms->src_width = h264_info_display_width(info);
	kms->src_height = h264_info_display_height(info);
	if (!kms->src_width || !kms->src_height) {
		kms->src_width = track->width;
		kms->src_height = track->height;
	}
	kms->src_stride = dec->capture_fmt.bytesperline;
	kms->src_coded_height = dec->capture_fmt.height;
	kms->src_frame_size = dec->capture_fmt.sizeimage;
	kms->fb_width = dec->capture_fmt.width;
	kms->fb_height = dec->capture_fmt.height;
	kms->crop_no_scale = opt->crop_no_scale;
	kms->width = kms->src_width;
	kms->height = kms->src_height;
	kms->stride = kms->src_stride;
	kms->coded_height = kms->src_coded_height;
	kms->frame_size = kms->src_frame_size;
	kms->delay_ms = track_frame_delay_ms(track);
	pace = opt->no_pace ? "off" :
	       (track->sample_count && track->samples[0].has_time ?
		"mp4-pts" : "fixed");

	if ((kms->src_x | kms->src_y | kms->src_width | kms->src_height) & 1) {
		fprintf(stderr,
			"NV12 display source crop must be even, got %u,%u %ux%u\n",
			kms->src_x, kms->src_y,
			kms->src_width, kms->src_height);
		return -1;
	}
	if (!kms->src_stride || !kms->src_coded_height ||
	    !kms->fb_width || !kms->fb_height) {
		fprintf(stderr,
			"invalid capture layout: fb=%ux%u stride=%u coded_height=%u source=%u,%u %ux%u\n",
			kms->fb_width, kms->fb_height,
			kms->src_stride, kms->src_coded_height,
			kms->src_x, kms->src_y, kms->src_width, kms->src_height);
		return -1;
	}
	src_right = (uint64_t)kms->src_x + kms->src_width;
	src_bottom = (uint64_t)kms->src_y + kms->src_height;
	if (src_right > kms->fb_width || src_bottom > kms->fb_height ||
	    src_right > kms->src_stride ||
	    src_bottom > kms->src_coded_height) {
		fprintf(stderr,
			"source crop outside capture layout: fb=%ux%u stride=%u coded_height=%u crop=%u,%u %ux%u\n",
			kms->fb_width, kms->fb_height,
			kms->src_stride, kms->src_coded_height,
			kms->src_x, kms->src_y, kms->src_width, kms->src_height);
		return -1;
	}
	if (kms->src_frame_size <
	    kms->src_stride * kms->src_coded_height) {
		fprintf(stderr,
			"invalid capture sizeimage: %u < luma plane %u\n",
			kms->src_frame_size,
			kms->src_stride * kms->src_coded_height);
		return -1;
	}

	if (dec->capture_count > ARRAY_SIZE(kms->fb)) {
		fprintf(stderr, "too many capture buffers: %u\n",
			dec->capture_count);
		return -1;
	}
	for (i = 0; i < dec->capture_count; i++) {
		if (dec->capture[i].length < kms->src_frame_size) {
			fprintf(stderr,
				"capture%u too small for display: %u < %u\n",
				i, dec->capture[i].length,
				kms->src_frame_size);
			return -1;
		}
	}

	kms->fd = open(opt->drm_card, O_RDWR | O_CLOEXEC);
	if (kms->fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", opt->drm_card,
			strerror(errno));
		return -1;
	}

	if (set_client_cap(kms->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1))
		return -1;

	if (opt->atomic &&
	    set_client_cap(kms->fd, DRM_CLIENT_CAP_ATOMIC, 1))
		fprintf(stderr, "atomic client cap unavailable, using legacy SETPLANE\n");

	if (find_display(kms->fd, &kms->ids))
		return -1;
	if (find_plane(kms->fd, &kms->ids, opt->plane_id))
		return -1;

	if (opt->atomic) {
		if (get_plane_atomic_props(kms->fd, kms->ids.plane_id,
					   &kms->plane_props) == 0)
			kms->atomic_ready = true;
		else
			fprintf(stderr, "atomic unavailable, using legacy SETPLANE\n");
	}
	kms->rotation = auto_plane_rotation(&kms->ids.mode, kms->width,
					    kms->height,
					    kms->atomic_ready,
					    kms->plane_props.rotation);
	rotation = rotation_name(kms->rotation);

	for (i = 0; i < dec->capture_count; i++) {
		if (lite_kms_export_fb(kms, dec, i))
			return -1;
	}

	calc_kms_src_dst_rect(kms, &plane_src_x, &plane_src_y,
			      &plane_src_w, &plane_src_h,
			      &plane_dst_x, &plane_dst_y,
			      &plane_dst_w, &plane_dst_h);
	fprintf(stderr,
		"card=%s connector=%u crtc=%u plane=%u mode=%ux%u\n"
		"video=coded %ux%u source=%u,%u %ux%u display=%ux%u stride=%u coded_height=%u frame_size=%u delay=%ums timebase=%u modifier=%s dmabuf=%s commit=%s pace=%s orientation=%s%s reorder=%u scanout_hold=%u\n"
		"plane src=%u,%u %ux%u dst=%u,%u %ux%u scale=%.3fx%.3f\n",
		opt->drm_card, kms->ids.connector_id, kms->ids.crtc_id,
		kms->ids.plane_id, kms->ids.mode.hdisplay,
		kms->ids.mode.vdisplay, kms->fb_width, kms->fb_height,
		kms->src_x, kms->src_y, kms->src_width, kms->src_height,
		kms->width, kms->height, kms->stride, kms->coded_height,
		kms->frame_size,
		kms->delay_ms, track->timescale,
		"ALLWINNER_TILED",
		"required",
		kms->atomic_ready ? "atomic-nonblock" : "legacy", pace, rotation,
		kms->crop_no_scale ? " crop-noscale" : "",
		info->vui_bitstream_restriction ?
		info->vui_num_reorder_frames : 2,
		(uint32_t)ARRAY_SIZE(kms->scanout_hold),
		plane_src_x, plane_src_y, plane_src_w, plane_src_h,
		plane_dst_x, plane_dst_y,
		plane_dst_w, plane_dst_h,
		plane_dst_w ? (double)plane_src_w / plane_dst_w : 0.0,
		plane_dst_h ? (double)plane_src_h / plane_dst_h : 0.0);

	if (!kms->crop_no_scale && kms->width > 1024 &&
	    (plane_dst_w != kms->width || plane_dst_h != kms->height))
		fprintf(stderr,
			"warn: DE1 frontend is scaling a %u-wide tiled source; if the image is stretched, test <=1024-wide portrait to confirm a scaler/line-buffer limit.\n",
			kms->width);

	return 0;
}

static void display_queue_init(struct display_queue *q,
			       const struct h264_stream_info *info)
{
	memset(q, 0, sizeof(*q));
	q->reorder_delay = info->vui_bitstream_restriction ?
			   info->vui_num_reorder_frames : 2;
	if (q->reorder_delay >= LITE_REORDER_QUEUE)
		q->reorder_delay = LITE_REORDER_QUEUE - 1;
}

static int display_queue_push(struct display_queue *q, struct lite_v4l2 *dec,
			      uint32_t capture_index, uint32_t frame_id,
			      int32_t poc, uint64_t pts_us, bool has_time)
{
	if (q->count >= ARRAY_SIZE(q->frames)) {
		fprintf(stderr, "display reorder queue full\n");
		return -1;
	}

	capture_get(dec, capture_index);
	q->frames[q->count++] = (struct display_frame) {
		.valid = true,
		.capture_index = capture_index,
		.frame_id = frame_id,
		.pts_us = pts_us,
		.has_time = has_time,
		.poc = poc,
	};

	return 0;
}

static int display_queue_lowest(const struct display_queue *q)
{
	uint32_t i;
	int best = -1;

	for (i = 0; i < q->count; i++) {
		const struct display_frame *cur = &q->frames[i];
		const struct display_frame *old;

		if (!cur->valid)
			continue;
		if (best < 0) {
			best = i;
			continue;
		}

		old = &q->frames[best];
		if (cur->poc < old->poc ||
		    (cur->poc == old->poc && cur->frame_id < old->frame_id))
			best = i;
	}

	return best;
}

static int display_queue_show_one(struct display_queue *q,
				  struct lite_v4l2 *dec,
				  struct lite_kms *kms,
				  const struct options *opt)
{
	struct display_frame frame;
	uint32_t queued;
	uint64_t stage;
	uint64_t t;
	int idx;

	if (!q->count)
		return 0;

	queued = q->count;
	idx = display_queue_lowest(q);
	if (idx < 0)
		return -1;

	frame = q->frames[idx];
	memmove(&q->frames[idx], &q->frames[idx + 1],
		(q->count - idx - 1) * sizeof(q->frames[0]));
	q->count--;

	if (!kms->base_us)
		kms->base_us = opt->playback_base_us ?
				opt->playback_base_us : now_us();
	if (!opt->no_pace && (frame.has_time || kms->delay_ms)) {
		uint64_t target_us;

		if (frame.has_time)
			target_us = kms->base_us + frame.pts_us;
		else
			target_us = kms->base_us +
				    (uint64_t)kms->frames * kms->delay_ms * 1000;
		stage = opt->profile ? now_us() : 0;
		sleep_until_us(target_us);
		if (opt->profile)
			profile_add_us(&kms->sleep_us, &kms->max_sleep_us,
				       now_us() - stage);
	}

	if (frame.capture_index >= ARRAY_SIZE(kms->fb) ||
	    !kms->fb[frame.capture_index].fb_id) {
		fprintf(stderr, "display frame has no fb capture=%u\n",
			frame.capture_index);
		capture_put(dec, frame.capture_index);
		return -1;
	}

	stage = opt->profile ? now_us() : 0;
	if (commit_plane(kms, kms->fb[frame.capture_index].fb_id)) {
		capture_put(dec, frame.capture_index);
		return -1;
	}
	if (opt->profile)
		profile_add_us(&kms->commit_us, &kms->max_commit_us,
			       now_us() - stage);

	scanout_hold_push(kms, dec, frame.capture_index);
	kms->frames++;
	q->displayed++;

	t = now_ms();
	if (!kms->first_ms)
		kms->first_ms = t;
	kms->last_ms = t;

	return 0;
}

static int display_queue_maybe_show(struct display_queue *q,
				    struct lite_v4l2 *dec,
				    struct lite_kms *kms,
				    const struct options *opt)
{
	if (q->count <= q->reorder_delay)
		return 0;

	return display_queue_show_one(q, dec, kms, opt);
}

static int display_queue_drain(struct display_queue *q, struct lite_v4l2 *dec,
			       struct lite_kms *kms, const struct options *opt)
{
	while (q->count) {
		if (display_queue_show_one(q, dec, kms, opt))
			return -1;
	}

	return 0;
}

static void display_queue_clear(struct display_queue *q, struct lite_v4l2 *dec)
{
	uint32_t i;

	for (i = 0; i < q->count; i++)
		capture_put(dec, q->frames[i].capture_index);
	memset(q, 0, sizeof(*q));
}

static int lite_v4l2_set_request_controls(int fd, int request_fd,
					  bool need_sps,
					  bool need_scaling,
					  bool need_pred,
					  struct v4l2_ctrl_h264_sps *sps,
					  struct v4l2_ctrl_h264_pps *pps,
					  struct v4l2_ctrl_h264_scaling_matrix *scaling,
					  struct v4l2_ctrl_h264_decode_params *decode,
					  struct v4l2_ctrl_h264_slice_params *slice,
					  struct v4l2_ctrl_h264_pred_weights *pred)
{
	struct v4l2_ext_control ctrl[6];
	struct v4l2_ext_controls ctrls = {
		.which = V4L2_CTRL_WHICH_REQUEST_VAL,
		.request_fd = request_fd,
		.controls = ctrl,
	};

	memset(ctrl, 0, sizeof(ctrl));

	if (need_sps) {
		ctrl[ctrls.count].id = V4L2_CID_STATELESS_H264_SPS;
		ctrl[ctrls.count].size = sizeof(*sps);
		ctrl[ctrls.count].ptr = sps;
		ctrls.count++;
	}

	ctrl[ctrls.count].id = V4L2_CID_STATELESS_H264_PPS;
	ctrl[ctrls.count].size = sizeof(*pps);
	ctrl[ctrls.count].ptr = pps;
	ctrls.count++;

	if (need_scaling) {
		ctrl[ctrls.count].id = V4L2_CID_STATELESS_H264_SCALING_MATRIX;
		ctrl[ctrls.count].size = sizeof(*scaling);
		ctrl[ctrls.count].ptr = scaling;
		ctrls.count++;
	}

	ctrl[ctrls.count].id = V4L2_CID_STATELESS_H264_DECODE_PARAMS;
	ctrl[ctrls.count].size = sizeof(*decode);
	ctrl[ctrls.count].ptr = decode;
	ctrls.count++;

	ctrl[ctrls.count].id = V4L2_CID_STATELESS_H264_SLICE_PARAMS;
	ctrl[ctrls.count].size = sizeof(*slice);
	ctrl[ctrls.count].ptr = slice;
	ctrls.count++;

	if (need_pred) {
		ctrl[ctrls.count].id = V4L2_CID_STATELESS_H264_PRED_WEIGHTS;
		ctrl[ctrls.count].size = sizeof(*pred);
		ctrl[ctrls.count].ptr = pred;
		ctrls.count++;
	}

	if (xioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) < 0) {
		fprintf(stderr,
			"v4l2 request controls failed request=%d count=%u error_idx=%u: %s\n",
			request_fd, ctrls.count, ctrls.error_idx,
			strerror(errno));
		return -1;
	}

	return 0;
}

static int lite_v4l2_submit_slice(struct lite_v4l2 *dec,
				  uint32_t frame_num,
				  uint32_t capture_index,
				  const uint8_t *slice_data,
				  uint32_t slice_size,
				  bool need_sps,
				  bool need_scaling,
				  bool need_pred,
				  struct v4l2_ctrl_h264_sps *sps,
				  struct v4l2_ctrl_h264_pps *pps,
				  struct v4l2_ctrl_h264_scaling_matrix *scaling,
				  struct v4l2_ctrl_h264_decode_params *decode,
				  struct v4l2_ctrl_h264_slice_params *slice,
				  struct v4l2_ctrl_h264_pred_weights *pred,
				  struct submit_profile *profile)
{
	struct v4l2_buffer out = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.memory = V4L2_MEMORY_MMAP,
		.index = 0,
		.bytesused = slice_size,
		.timestamp.tv_sec = frame_num / 1000000,
		.timestamp.tv_usec = frame_num % 1000000,
		.flags = V4L2_BUF_FLAG_REQUEST_FD,
	};
	struct v4l2_buffer cap = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
		.index = capture_index,
		.bytesused = dec->capture[capture_index].length,
	};
	struct pollfd pfd;
	int request_fd = -1;
	int ret = -1;
	uint64_t stage = 0;

	if (slice_size > dec->output.length) {
		fprintf(stderr, "slice too large: %u > %u\n",
			slice_size, dec->output.length);
		return -1;
	}
	if (capture_index >= dec->capture_count) {
		fprintf(stderr, "invalid capture index %u\n", capture_index);
		return -1;
	}

	if (profile)
		stage = now_us();
	if (xioctl(dec->media_fd, MEDIA_IOC_REQUEST_ALLOC, &request_fd) < 0) {
		fprintf(stderr, "MEDIA_IOC_REQUEST_ALLOC failed: %s\n",
			strerror(errno));
		return -1;
	}
	if (profile)
		profile_add_us(&profile->request_us, &profile->max_request_us,
			       now_us() - stage);

	if (profile)
		stage = now_us();
	if (lite_v4l2_set_request_controls(dec->video_fd, request_fd,
					   need_sps, need_scaling, need_pred,
					   sps, pps, scaling, decode, slice,
					   pred))
		goto out;
	if (profile)
		profile_add_us(&profile->controls_us, &profile->max_controls_us,
			       now_us() - stage);

	if (profile)
		stage = now_us();
	memcpy(dec->output.addr, slice_data, slice_size);
	if (profile)
		profile_add_us(&profile->copy_us, &profile->max_copy_us,
			       now_us() - stage);

	out.request_fd = request_fd;
	if (profile)
		stage = now_us();
	if (xioctl(dec->video_fd, VIDIOC_QBUF, &out) < 0) {
		fprintf(stderr, "v4l2 QBUF output failed: %s\n", strerror(errno));
		goto out;
	}

	if (xioctl(dec->video_fd, VIDIOC_QBUF, &cap) < 0) {
		fprintf(stderr, "v4l2 QBUF capture failed: %s\n", strerror(errno));
		goto out;
	}

	if (xioctl(request_fd, MEDIA_REQUEST_IOC_QUEUE, NULL) < 0) {
		fprintf(stderr, "MEDIA_REQUEST_IOC_QUEUE failed: %s\n",
			strerror(errno));
		goto out;
	}
	if (profile)
		profile_add_us(&profile->queue_us, &profile->max_queue_us,
			       now_us() - stage);

	pfd.fd = request_fd;
	pfd.events = POLLPRI;
	pfd.revents = 0;
	if (profile)
		stage = now_us();
	if (poll(&pfd, 1, 1000) <= 0) {
		fprintf(stderr, "request poll failed revents=0x%x: %s\n",
			pfd.revents, errno ? strerror(errno) : "timeout");
		goto out;
	}
	if (profile)
		profile_add_us(&profile->wait_us, &profile->max_wait_us,
			       now_us() - stage);

	memset(&out, 0, sizeof(out));
	out.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	out.memory = V4L2_MEMORY_MMAP;
	if (profile)
		stage = now_us();
	if (xioctl(dec->video_fd, VIDIOC_DQBUF, &out) < 0) {
		fprintf(stderr, "v4l2 DQBUF output failed: %s\n", strerror(errno));
		goto out;
	}

	memset(&cap, 0, sizeof(cap));
	cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	cap.memory = V4L2_MEMORY_MMAP;
	if (xioctl(dec->video_fd, VIDIOC_DQBUF, &cap) < 0) {
		fprintf(stderr, "v4l2 DQBUF capture failed: %s\n", strerror(errno));
		goto out;
	}
	if (profile)
		profile_add_us(&profile->dq_us, &profile->max_dq_us,
			       now_us() - stage);

	if ((uint32_t)(cap.timestamp.tv_sec * 1000000 + cap.timestamp.tv_usec) !=
	    frame_num)
		fprintf(stderr, "capture timestamp mismatch got=%lu.%06lu want=%u\n",
			(unsigned long)cap.timestamp.tv_sec,
			(unsigned long)cap.timestamp.tv_usec, frame_num);

	ret = 0;
out:
	if (request_fd >= 0)
		close(request_fd);
	return ret;
}

static int decode_count_continuous(const uint8_t *file, size_t file_size,
				   const struct video_track *track,
				   const struct options *opt,
				   bool seamless_loop)
{
	GstH264NalParser *parser;
	struct h264_stream_info info = { 0 };
	struct lite_v4l2 dec;
	struct lite_kms kms;
	struct display_queue display;
	struct usage_report usage = { 0 };
	struct submit_profile submit = { 0 };
	struct lite_dpb dpb = {
		.max_long_term_frame_idx = -1,
	};
	struct poc_state poc;
	bool have_dec = false;
	bool have_kms = false;
	uint32_t target = track->sample_count;
	uint32_t decoded = 0;
	uint32_t scanned = 0;
	uint64_t start_us = now_us();
	uint64_t submit_us = 0;
	uint64_t max_submit_us = 0;
	int ret = -1;

	memset(&dec, 0, sizeof(dec));
	dec.video_fd = -1;
	dec.media_fd = -1;
	memset(&kms, 0, sizeof(kms));
	kms.fd = -1;
	display_queue_init(&display, &info);
	poc_state_reset(&poc);

	parser = gst_h264_nal_parser_new();
	if (!parser)
		return -1;

	if (prime_h264_parser(parser, track, &info)) {
		fprintf(stderr, "failed to parse avcC SPS/PPS\n");
		goto out;
	}
	display_queue_init(&display, &info);
	usage_report_maybe(&usage, opt, decoded, decoded, NULL, submit_us,
			   &submit, false);

loop_start:
	scanned = 0;
	for (scanned = 0; scanned < track->sample_count && decoded < target;
	     scanned++) {
		const struct sample_info *s = &track->samples[scanned];
		const uint8_t *data = track_sample_data(file, track, scanned);
		uint32_t off = 0;
		bool submitted_sample = false;

		while (off < s->size && decoded < target && !submitted_sample) {
			GstH264NalUnit nalu;
			GstH264SliceHdr slice;
			GstH264ParserResult pres;
			struct v4l2_ctrl_h264_sps v4l2_sps;
			struct v4l2_ctrl_h264_pps v4l2_pps;
			struct v4l2_ctrl_h264_scaling_matrix v4l2_scaling;
			struct v4l2_ctrl_h264_decode_params v4l2_decode;
			struct v4l2_ctrl_h264_slice_params v4l2_slice;
			struct v4l2_ctrl_h264_pred_weights v4l2_pred;
			struct ref_entry list0[LITE_MAX_DPB];
			struct ref_entry list1[LITE_MAX_DPB];
			struct ref_entry v4l2_map[LITE_MAX_DPB];
			uint32_t len0 = 0, len1 = 0, v4l2_map_len = 0;
			bool scaling_present;
			bool pred_required;
			uint32_t frame_id;
			uint32_t stype;
			uint64_t stage;
			int32_t top_poc, bottom_poc, curr_poc, poc_msb = 0;
			uint32_t frame_num_offset = 0;
			int capture_index;

			pres = identify_h264_nalu(parser, track, data, off,
						  s->size, &nalu);
			if (pres != GST_H264_PARSER_OK)
				break;

			off = nalu.offset + nalu.size;

			if (nalu.type != GST_H264_NAL_SLICE &&
			    nalu.type != GST_H264_NAL_SLICE_IDR)
				continue;

			memset(&slice, 0, sizeof(slice));
			pres = gst_h264_parser_parse_slice_hdr(parser, &nalu,
							       &slice, true,
							       true);
			if (pres != GST_H264_PARSER_OK) {
				fprintf(stderr, "sample %u slice parse failed: %d\n",
					scanned + 1, pres);
				goto out;
			}

			if (!slice.pps || !slice.pps->sequence) {
				fprintf(stderr, "sample %u missing PPS/SPS\n",
					scanned + 1);
				goto out;
			}

			if (slice.field_pic_flag ||
			    slice.pps->sequence->pic_order_cnt_type > 2 ||
			    slice.pps->sequence->pic_order_cnt_type == 1) {
				fprintf(stderr,
					"sample %u unsupported count slice: field=%u poc_type=%u\n",
					scanned + 1, slice.field_pic_flag,
					slice.pps->sequence->pic_order_cnt_type);
				goto out;
			}

			if (!dpb.max_frame_num) {
				dpb.max_frame_num = slice.pps->sequence->max_frame_num;
				dpb.max_num_ref_frames =
					slice.pps->sequence->num_ref_frames;
			}

			if (slice.pps->sequence->pic_order_cnt_type == 0)
				poc_type0_calc(&nalu, &slice, &poc, &top_poc,
					       &bottom_poc, &poc_msb);
			else
				poc_type2_calc(&nalu, &slice, &poc, &top_poc,
					       &bottom_poc, &frame_num_offset);
			curr_poc = top_poc < bottom_poc ? top_poc : bottom_poc;
			stype = slice.type % 5;
			dpb_update_pic_nums(&dpb, slice.frame_num);

			if (opt->display && have_kms && decoded &&
			    nalu.type == GST_H264_NAL_SLICE_IDR) {
				if (display_queue_drain(&display, &dec, &kms, opt))
					goto out;
			}

			scaling_present = h264_scaling_matrix_present(slice.pps);
			fill_v4l2_sps(&v4l2_sps, slice.pps->sequence);
			fill_v4l2_pps(&v4l2_pps, slice.pps, scaling_present);
			if (scaling_present)
				fill_v4l2_scaling(&v4l2_scaling, slice.pps);
			else
				memset(&v4l2_scaling, 0, sizeof(v4l2_scaling));
			fill_v4l2_decode_params(&v4l2_decode, &nalu, &slice,
						 top_poc, bottom_poc);
			fill_v4l2_slice_params(&v4l2_slice, &nalu, &slice);
			fill_decode_dpb(&dpb, &v4l2_decode, v4l2_map,
					&v4l2_map_len);
			if (build_ref_lists(&dpb, &slice, curr_poc, list0, &len0,
					    list1, &len1)) {
				fprintf(stderr, "sample %u failed to build ref lists\n",
					scanned + 1);
				goto out;
			}
			fill_v4l2_ref_list(v4l2_slice.ref_pic_list0, list0, len0,
					   v4l2_map, v4l2_map_len);
			fill_v4l2_ref_list(v4l2_slice.ref_pic_list1, list1, len1,
					   v4l2_map, v4l2_map_len);

			pred_required =
				V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED(&v4l2_pps,
								     &v4l2_slice);
			if (pred_required)
				fill_v4l2_pred_weight(&v4l2_pred, &slice);
			else
				memset(&v4l2_pred, 0, sizeof(v4l2_pred));

			if (!have_dec) {
				if (lite_v4l2_setup(&dec, opt, track,
						    &info, &v4l2_sps))
					goto out;
				have_dec = true;
				if (opt->display) {
					if (lite_kms_setup(&kms, opt, track,
							   &info, &dec)) {
						lite_kms_close(&kms);
						goto out;
					}
					have_kms = true;
				}
			}

			capture_index = lite_capture_alloc(&dec);
			if (capture_index < 0) {
				fprintf(stderr, "sample %u no free capture buffer\n",
					scanned + 1);
				goto out;
			}

			frame_id = scanned + 1;
			stage = opt->profile ? now_us() : 0;
			if (lite_v4l2_submit_slice(&dec, frame_id,
						   capture_index,
						   nalu.data + nalu.offset,
						   nalu.size,
						   decoded == 0,
						   scaling_present,
						   pred_required,
						   &v4l2_sps, &v4l2_pps,
						   &v4l2_scaling, &v4l2_decode,
						   &v4l2_slice, &v4l2_pred,
						   usage_timing_enabled(opt) ?
						   &submit : NULL))
				goto out;
			if (opt->profile)
				profile_add_us(&submit_us, &max_submit_us,
					       now_us() - stage);

			if (opt->display &&
			    display_queue_push(&display, &dec, capture_index,
					       frame_id, curr_poc,
					       s->pts_us, s->has_time)) {
				capture_put(&dec, capture_index);
				goto out;
			}

			if (dpb_store_current(&dpb, &dec, &nalu, &slice,
					      frame_id, capture_index, top_poc,
					      bottom_poc))
				goto out;
			poc_state_update_ref(&poc, &nalu, &slice, poc_msb,
					     frame_num_offset);

			if (opt->display &&
			    display_queue_maybe_show(&display, &dec, &kms, opt))
				goto out;

			decoded++;
			usage_report_maybe(&usage, opt, decoded,
					   opt->display ? kms.frames : decoded,
					   opt->display && have_kms ? &kms : NULL,
					   submit_us, &submit, false);
			submitted_sample = true;
		}
	}

	if (!decoded) {
		fprintf(stderr, "decode count submitted no frames\n");
		goto out;
	}

	if (opt->display && display_queue_drain(&display, &dec, &kms, opt))
		goto out;

	if (seamless_loop) {
		poc_state_reset(&poc);
		dpb_clear(&dpb, &dec);
		display_queue_init(&display, &info);
		if (have_kms) {
			kms.first_ms = 0;
			kms.last_ms = 0;
			kms.frames = 0;
		}
		decoded = 0;
		scanned = 0;
		start_us = now_us();
		goto loop_start;
	}

	usage_report_maybe(&usage, opt, decoded,
			   opt->display ? kms.frames : decoded,
			   opt->display && have_kms ? &kms : NULL,
			   submit_us, &submit, true);

	if (opt->display) {
		uint64_t elapsed_ms = kms.first_ms && kms.last_ms >= kms.first_ms ?
				      kms.last_ms - kms.first_ms : 0;

		if (elapsed_ms)
			fprintf(stderr, "done: %u frames, %.1f fps avg\n",
				kms.frames,
				(double)kms.frames * 1000.0 / elapsed_ms);
		fprintf(stderr,
			"decode display ok decoded=%u displayed=%u scanned=%u elapsed_us=%" PRIu64 "\n",
			decoded, kms.frames, scanned, now_us() - start_us);
	} else {
		fprintf(stderr,
			"decode count ok decoded=%u scanned=%u elapsed_us=%" PRIu64 "\n",
			decoded, scanned, now_us() - start_us);
	}

	ret = 0;
	if (opt->profile) {
		double submit_avg = decoded ? (double)submit_us / decoded : 0.0;
		double commit_avg = kms.frames ?
				    (double)kms.commit_us / kms.frames : 0.0;
		double sleep_avg = kms.frames ?
				   (double)kms.sleep_us / kms.frames : 0.0;

		fprintf(stderr,
			"profile frames=%u avg_us: submit=%.1f commit=%.1f sleep=%.1f max_us: submit=%" PRIu64 " commit=%" PRIu64 " sleep=%" PRIu64 "\n",
			decoded, submit_avg, commit_avg, sleep_avg,
			max_submit_us, kms.max_commit_us, kms.max_sleep_us);
	}
out:
	if (have_dec) {
		if (opt->display) {
			display_queue_clear(&display, &dec);
			if (have_kms)
				scanout_hold_clear(&kms, &dec);
		}
		dpb_clear(&dpb, &dec);
		if (have_kms)
			lite_kms_close(&kms);
		lite_v4l2_close(&dec);
	}
	gst_h264_nal_parser_free(parser);
	return ret;
}

static int probe_v4l2_decode(const struct options *opt,
			     const struct video_track *track)
{
	GstH264NalParser *parser = NULL;
	struct h264_stream_info info = { 0 };
	uint32_t output_size = max_sample_size(track);
	uint32_t coded_width;
	uint32_t coded_height;
	uint32_t min_capture = 6;
	uint32_t request_capture = 6;
	int fd;
	bool ok = true;

	if (output_size < H264_OUTPUT_SIZE_MIN)
		output_size = H264_OUTPUT_SIZE_MIN;

	parser = gst_h264_nal_parser_new();
	if (!parser)
		return -1;
	if (prime_h264_parser(parser, track, &info)) {
		fprintf(stderr, "failed to parse avcC SPS/PPS\n");
		gst_h264_nal_parser_free(parser);
		return -1;
	}
	coded_width = h264_info_coded_width(&info);
	coded_height = h264_info_coded_height(&info);
	min_capture = min_capture_buffers(&info, info.num_ref_frames, false);
	request_capture = min_capture + read_capture_extra_buffers();
	if (request_capture > LITE_CAPTURE_BUFFERS)
		request_capture = LITE_CAPTURE_BUFFERS;
	request_capture = clamp_capture_request_to_cma(coded_width,
						       coded_height,
						       min_capture,
						       request_capture,
						       output_size, true);

	fd = open(opt->video_dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", opt->video_dev,
			strerror(errno));
		gst_h264_nal_parser_free(parser);
		return -1;
	}

	fprintf(stderr, "v4l2 decode probe video=%s media=%s\n",
		opt->video_dev, opt->media_dev ? opt->media_dev : "auto");

	print_v4l2_capabilities(fd);
	ok &= enum_v4l2_formats(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
				V4L2_PIX_FMT_H264_SLICE, "output");
	ok &= enum_v4l2_formats(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
				V4L2_PIX_FMT_SUNXI_TILED_NV12, "capture");
	probe_h264_controls(fd);

	if (set_v4l2_format(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
			    V4L2_PIX_FMT_H264_SLICE, coded_width,
			    coded_height, output_size, NULL, "output", true))
		ok = false;
	if (set_v4l2_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
			    V4L2_PIX_FMT_SUNXI_TILED_NV12, coded_width,
			    coded_height, 0, NULL, "capture", true))
		ok = false;

	if (ok && check_capture_memory_budget(coded_width, coded_height,
					      min_capture, output_size) == 0) {
		if (request_v4l2_buffers_probe(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
					       1, "output"))
			ok = false;
		if (request_v4l2_buffers_probe(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
					       request_capture, "capture"))
			ok = false;
		release_v4l2_buffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, "capture");
		release_v4l2_buffers(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, "output");
	} else if (ok) {
		ok = false;
	}

	close(fd);
	gst_h264_nal_parser_free(parser);

	if (probe_media_request(opt->media_dev))
		ok = false;

	fprintf(stderr, "v4l2 decode probe %s\n", ok ? "ok" : "failed");
	return ok ? 0 : -1;
}

static int play_one_file(const struct options *base_opt, const char *path,
			 size_t index, size_t total, bool seamless_loop)
{
	struct options opt = *base_opt;
	struct video_track track = { 0 };
	struct video_track audio = { 0 };
	struct stats stats = { 0 };
	uint8_t *file = NULL;
	size_t file_size = 0;
	uint64_t start, demux_us, parse_us;
	uint64_t duration_ticks;
	double duration_sec = 0.0;
	double fps = 0.0;
	struct audio_thread_args av_audio = { 0 };
	pthread_t audio_thread;
	int abort_playback = 0;
	bool audio_thread_started = false;
	bool ffmpeg_demux = false;
	int ret = 1;

	opt.input = path;
	opt.playback_base_us = 0;
	opt.abort_playback = &abort_playback;

	fprintf(stderr, "==> %s [%zu/%zu]\n", path, index + 1, total);

	if (map_file_ro(path, &file, &file_size))
		return 1;

	start = now_us();
	if (parse_atoms(file, file_size, 0, file_size, NULL, &track, &audio, 0)) {
		free_track(&track);
		free_track(&audio);
		memset(&track, 0, sizeof(track));
		memset(&audio, 0, sizeof(audio));
		if (demux_with_ffmpeg(path, &track, &audio)) {
			fprintf(stderr, "failed to demux media file\n");
			goto out;
		}
		ffmpeg_demux = true;
	} else {
		if (track.is_video && build_sample_table(&track, file_size)) {
			fprintf(stderr, "failed to parse MP4 AVC sample table\n");
			free_track(&track);
			free_track(&audio);
			memset(&track, 0, sizeof(track));
			memset(&audio, 0, sizeof(audio));
			if (demux_with_ffmpeg(path, &track, &audio)) {
				fprintf(stderr, "failed to demux media file\n");
				goto out;
			}
			ffmpeg_demux = true;
		}

		if (!ffmpeg_demux && audio.is_audio &&
		    build_sample_table(&audio, file_size)) {
			fprintf(stderr, "warning: failed to parse MP4 audio sample table\n");
			free_track(&audio);
			memset(&audio, 0, sizeof(audio));
		}
	}
	demux_us = now_us() - start;

	if (track.is_video) {
		duration_ticks = track_duration_ticks(&track);
		if (track.timescale && duration_ticks) {
			duration_sec = (double)duration_ticks / track.timescale;
			fps = duration_sec ? track.sample_count / duration_sec : 0.0;
		}

		fprintf(stderr,
			"%s video=%ux%u samples=%u chunks=%u timescale=%u duration=%.3fs fps=%.3f ctts=%u nal_length=%u h264=%s sps=%u pps=%u\n",
			ffmpeg_demux ? "ffmpeg" : "mp4",
			track.width, track.height, track.sample_count,
			track.chunk_count, track.timescale, duration_sec, fps,
			track.ctts_count, track.nal_length_size,
			track.h264_format == H264_BITSTREAM_ANNEXB ?
			"annexb" : "avcc", track.num_sps, track.num_pps);
	} else {
		fprintf(stderr, "%s video=none\n", ffmpeg_demux ? "ffmpeg" : "mp4");
	}

	if (audio.is_audio)
		print_audio_summary(&audio, ffmpeg_demux ? "ffmpeg" : "mp4");
	else
		fprintf(stderr, "%s audio=none\n", ffmpeg_demux ? "ffmpeg" : "mp4");

	if (opt.auto_play) {
		opt.display = track.is_video;
		opt.audio_play = audio.is_audio;
	}

	if (opt.audio_play && !opt.display) {
		ret = play_aac_audio(file, &audio, &opt) ? 1 : 0;
		goto out;
	}

	if (!track.is_video) {
		fprintf(stderr, "supported video track not found\n");
		goto out;
	}

	if (opt.display) {
		if (opt.audio_play && opt.display) {
			int thread_ret;

			if (!audio.is_audio) {
				fprintf(stderr, "audio track not found\n");
				goto out;
			}

			if (!opt.no_pace)
				opt.playback_base_us = now_us() +
						       AV_PLAYBACK_START_DELAY_US;

			av_audio.file = file;
			av_audio.audio = &audio;
			av_audio.opt = opt;
			av_audio.ret = -1;
			thread_ret = pthread_create(&audio_thread, NULL,
						    audio_thread_main,
						    &av_audio);
			if (thread_ret) {
				fprintf(stderr, "start audio thread failed: %s\n",
					strerror(thread_ret));
				goto out;
			}
			audio_thread_started = true;
		}

		set_thread_name("f1c-video");

		if (is_h264_codec(track.codec)) {
			ret = decode_count_continuous(file, file_size, &track, &opt,
						      seamless_loop) ?
			      1 : 0;
		} else {
			fprintf(stderr, "unsupported video codec: %d\n", track.codec);
			ret = 1;
		}

		if (audio_thread_started) {
			int thread_ret;

			if (ret)
				playback_abort(&abort_playback);

			thread_ret = pthread_join(audio_thread, NULL);
			audio_thread_started = false;
			if (thread_ret) {
				fprintf(stderr, "join audio thread failed: %s\n",
					strerror(thread_ret));
				ret = 1;
			} else if (av_audio.ret) {
				fprintf(stderr, "audio playback failed: %d\n",
					av_audio.ret);
				if (!ret)
					ret = 1;
			}
		}
		goto out;
	}

	start = now_us();
	if (walk_h264_samples(file, &track, &opt, &stats))
		goto out;
	parse_us = now_us() - start;

	fprintf(stderr,
		"h264 samples=%u nalus=%u slices=%u I/P/B=%u/%u/%u idr=%u ref/nonref=%u/%u fields=%u refmod=%u/%u adaptive_mark=%u pred_weight=%u max_refs=%u/%u max_frame_num=%u max_poc_lsb=%u sps=%u pps=%u sei=%u aud=%u bytes=%" PRIu64 " errors=%u\n",
		stats.samples, stats.nalus, stats.slices,
		stats.i_slices, stats.p_slices, stats.b_slices, stats.idr,
		stats.ref_slices, stats.nonref_slices, stats.field_slices,
		stats.ref_list_mod_l0, stats.ref_list_mod_l1,
		stats.adaptive_marking, stats.pred_weight,
		stats.max_ref_idx_l0, stats.max_ref_idx_l1,
		stats.max_frame_num_seen, stats.max_poc_lsb_seen,
		stats.sps, stats.pps, stats.sei, stats.aud, stats.bytes,
		stats.parse_errors);

	if (opt.profile) {
		double sample_us = stats.samples ?
				   (double)parse_us / stats.samples : 0.0;
		double identify_us = stats.nalus ?
				      (double)stats.identify_us / stats.nalus : 0.0;
		double nal_parse_us = stats.nalus ?
				       (double)stats.parse_us / stats.nalus : 0.0;
		double rate = parse_us ? (double)stats.samples * 1000000.0 /
					 parse_us : 0.0;

		fprintf(stderr,
			"profile demux_us=%" PRIu64 " parse_us=%" PRIu64 " avg_sample_us=%.1f identify_us=%.2f nal_parse_us=%.2f max_sample_us=%" PRIu64 " rate=%.1f fps\n",
			demux_us, parse_us, sample_us, identify_us, nal_parse_us,
			stats.max_sample_us, rate);
	}

	ret = stats.parse_errors ? 2 : 0;
out:
	if (audio_thread_started) {
		playback_abort(&abort_playback);
		pthread_join(audio_thread, NULL);
	}
	free_track(&track);
	free_track(&audio);
	if (file)
		munmap(file, file_size);
	return ret;
}

int main(int argc, char **argv)
{
	struct options opt = { 0 };
	struct playlist list = { 0 };
	size_t i;
	int ret = 0;

	set_thread_name("fplayerdemo");

	if (parse_args(argc, argv, &opt)) {
		usage(argv[0]);
		return 1;
	}

	if (playlist_build(&list, opt.input)) {
		fprintf(stderr, "failed to build playlist from %s\n", opt.input);
		return 1;
	}

	fprintf(stderr, "playlist entries=%zu loop=%s\n",
		list.count, opt.loop ? "on" : "off");

	do {
		for (i = 0; i < list.count; i++) {
			bool seamless = opt.loop && list.count == 1;
			ret = play_one_file(&opt, list.items[i], i, list.count,
					    seamless);
			if (ret && !opt.loop)
				goto out;
		}
	} while (opt.loop && list.count > 1);

out:
	playlist_free(&list);
	return ret;
}
