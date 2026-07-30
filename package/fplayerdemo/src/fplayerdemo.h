// SPDX-License-Identifier: GPL-2.0
#ifndef FPLAYERDEMO_INTERNAL_H
#define FPLAYERDEMO_INTERNAL_H

/*
 * F1C200S Cedrus/KMS player and diagnostics tool.
 *
 * This tool bypasses GStreamer pipelines and appsink. It parses MP4/AVC files
 * directly, walks H.264 NAL units with its built-in parser, and can probe the
 * direct V4L2 stateless decoder setup used by the custom request/KMS path.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
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
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <linux/media.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>

#include "h264_parser.h"
#ifdef HAVE_LIBAVCODEC
#include <libavcodec/avcodec.h>
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

struct track_packet {
	struct sample_info sample;
	const uint8_t *data;
};

struct video_track {
	bool is_video;
	bool is_audio;
	uint32_t timescale;
	uint16_t width;
	uint16_t height;
	enum AVCodecID codec;
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
};

struct av_start_clock {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	uint64_t base_us;
	bool initialized;
	bool ready;
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
	struct av_start_clock *av_clock;
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
	uint64_t last_present_us;
	uint64_t interval_us;
	uint64_t min_interval_us;
	uint64_t max_interval_us;
	uint64_t late_us;
	uint64_t max_late_us;
	uint32_t interval_count;
	uint32_t late_frames;
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

struct file_window {
	const uint8_t *file;
	size_t file_size;
	uint64_t discard_end;
	bool enabled;
	bool allow_discard;
};

struct audio_thread_args {
	const uint8_t *file;
	const struct video_track *audio;
	struct options opt;
	int ret;
};

/* common.c */
uint64_t now_us(void);
uint64_t now_ms(void);
void profile_add_us(uint64_t *total, uint64_t *max, uint64_t val);
bool usage_timing_enabled(const struct options *opt);
void usage_report_maybe(struct usage_report *report,
			const struct options *opt,
			uint32_t decoded, uint32_t displayed,
			const struct lite_kms *kms,
			uint64_t submit_us,
			const struct submit_profile *submit,
			bool force);
bool playback_aborted(const struct options *opt);
int sleep_until_us(uint64_t target_us, const struct options *opt);
void playback_abort(int *abort_playback);
int install_signal_handlers(void);
int av_start_clock_init(struct av_start_clock *clock);
void av_start_clock_destroy(struct av_start_clock *clock);
void av_start_clock_publish(const struct options *opt);
uint64_t playback_base_get(const struct options *opt);
int audio_wait_for_playback_base(const struct options *opt);
void set_thread_name(const char *name);
uint32_t align_up_u32(uint32_t val, uint32_t align);
uint32_t align_down_u32(uint32_t val, uint32_t align);
uint32_t h264_sps_coded_width(const struct v4l2_ctrl_h264_sps *sps);
uint32_t h264_sps_coded_height(const struct v4l2_ctrl_h264_sps *sps);
uint32_t h264_info_coded_width(const struct h264_stream_info *info);
uint32_t h264_info_coded_height(const struct h264_stream_info *info);
uint32_t h264_info_display_width(const struct h264_stream_info *info);
uint32_t h264_info_display_height(const struct h264_stream_info *info);
uint32_t min_capture_buffers(const struct h264_stream_info *info,
			     uint32_t max_num_ref_frames, bool display);
uint32_t read_capture_extra_buffers(void);
int check_capture_memory_budget(uint32_t width, uint32_t height,
				uint32_t min_capture, uint32_t output_size);
uint32_t clamp_capture_request_to_cma(uint32_t width, uint32_t height,
				      uint32_t minimum, uint32_t requested,
				      uint32_t output_size, bool verbose);
uint16_t rd16(const uint8_t *p);
uint32_t rd32(const uint8_t *p);
uint64_t rd64(const uint8_t *p);
bool fourcc_is(const uint8_t *p, const char *s);
char *pixfmt_str(uint32_t pixfmt, char out[5]);
char *fourcc_str(const uint8_t *p, char out[5]);
int xioctl(int fd, unsigned long req, void *arg);
#ifdef HAVE_LIBAVCODEC
const char *ffmpeg_errstr(int err, char *buf, size_t len);
#endif

/* display.c */
int drm_ioctl(int fd, unsigned long request, void *arg, const char *name);
int set_client_cap(int fd, uint64_t capability, uint64_t value);
int find_display(int fd, struct kms_ids *ids);
int find_plane(int fd, struct kms_ids *ids, uint32_t force_plane_id);
void close_gem_handle(int fd, uint32_t handle);
int prime_fd_to_handle(int drm_fd, int prime_fd, uint32_t *handle);
int get_plane_atomic_props(int fd, uint32_t plane_id,
			   struct plane_props *props);
int wait_atomic_event(struct lite_kms *kms, int timeout_ms);
void calc_kms_src_dst_rect(const struct lite_kms *kms,
			   uint32_t *src_x, uint32_t *src_y,
			   uint32_t *src_w, uint32_t *src_h,
			   uint32_t *dst_x, uint32_t *dst_y,
			   uint32_t *dst_w, uint32_t *dst_h);
uint32_t auto_plane_rotation(const struct drm_mode_modeinfo *mode,
			     uint32_t width, uint32_t height,
			     bool atomic_ready, uint32_t rotation_prop);
const char *rotation_name(uint32_t rotation);
int commit_plane(struct lite_kms *kms, uint32_t fb_id);

/* file_io.c */
int map_file_ro(const char *path, uint8_t **data, size_t *size);
void file_window_init(struct file_window *win, const uint8_t *file,
			      size_t file_size, bool enabled, bool allow_discard);
void file_window_reset(struct file_window *win);
void file_window_advance(struct file_window *win, uint64_t offset);

/* media.c */
int track_packet_get(const uint8_t *file, const struct video_track *t,
		     uint32_t sample, struct track_packet *packet,
		     const struct options *opt);
void track_packet_put(struct track_packet *packet);
H264ParserResult identify_h264_nalu(H264NalParser *parser,
				       const struct video_track *t,
				       const uint8_t *data,
				       uint32_t offset, uint32_t size,
				       H264NalUnit *nalu);
int parse_atoms(const uint8_t *file, size_t file_size, uint64_t start,
		uint64_t end, struct video_track *cur,
		struct video_track *out_video,
		struct video_track *out_audio, unsigned int depth);
int build_sample_table(struct video_track *t, size_t file_size);
int video_decode(const uint8_t *file, size_t file_size,
		 const struct video_track *track, const struct options *opt,
		 bool seamless_loop);
uint64_t track_duration_ticks(const struct video_track *t);
void print_audio_summary(const struct video_track *audio, const char *label);
void free_track(struct video_track *t);

/* audio.c */
int play_aac_audio(const uint8_t *file, const struct video_track *audio,
		   const struct options *opt);
void *audio_thread_main(void *arg);

/* h264.c */
int walk_h264_samples(const uint8_t *file, const struct video_track *t,
		      const struct options *opt, struct stats *stats);
int decode_count_continuous(const uint8_t *file, size_t file_size,
			    const struct video_track *track,
			    const struct options *opt, bool seamless_loop);

#endif
