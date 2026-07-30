// SPDX-License-Identifier: GPL-2.0
/*
 * F1C200S Cedrus/KMS player and diagnostics tool.
 *
 * This tool bypasses GStreamer pipelines and appsink. It parses MP4/AVC files
 * directly, walks H.264 NAL units with its built-in parser, and can probe the
 * direct V4L2 stateless decoder setup used by the custom request/KMS path.
 */

#include "fplayerdemo.h"

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
		".mp4", ".m4v", ".mov", ".m4a",
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
	struct av_start_clock av_clock = { 0 };
	pthread_t audio_thread;
	int abort_playback = 0;
	bool audio_thread_started = false;
	bool av_clock_initialized = false;
	int ret = 1;

	opt.input = path;
	opt.playback_base_us = 0;
	opt.abort_playback = &abort_playback;

	fprintf(stderr, "==> %s [%zu/%zu]\n", path, index + 1, total);

	start = now_us();
	if (map_file_ro(path, &file, &file_size))
		return 1;

	if (parse_atoms(file, file_size, 0, file_size, NULL, &track,
			&audio, 0)) {
		fprintf(stderr, "failed to parse MP4 container\n");
		goto out;
	}
	if (track.is_video && build_sample_table(&track, file_size)) {
		fprintf(stderr, "failed to parse MP4 AVC sample table\n");
		goto out;
	}
	if (audio.is_audio && build_sample_table(&audio, file_size)) {
		fprintf(stderr,
			"warning: failed to parse MP4 audio sample table\n");
		free_track(&audio);
		memset(&audio, 0, sizeof(audio));
	}
	demux_us = now_us() - start;
	if (opt.profile)
		fprintf(stderr, "profile input demux_us=%" PRIu64 "\n", demux_us);

	if (track.is_video) {
		char sample_count[24];

		duration_ticks = track_duration_ticks(&track);
		if (track.timescale && duration_ticks) {
			duration_sec = (double)duration_ticks / track.timescale;
			fps = duration_sec ? track.sample_count / duration_sec : 0.0;
		}
		snprintf(sample_count, sizeof(sample_count), "%u",
			 track.sample_count);

		fprintf(stderr,
			"%s video=%ux%u samples=%s chunks=%u timescale=%u duration=%.3fs fps=%.3f ctts=%u nal_length=%u h264=avcc sps=%u pps=%u\n",
			"mp4",
			track.width, track.height, sample_count,
			track.chunk_count, track.timescale, duration_sec, fps,
			track.ctts_count, track.nal_length_size,
			track.num_sps, track.num_pps);
	} else {
		fprintf(stderr, "mp4 video=none\n");
	}

	if (audio.is_audio)
		print_audio_summary(&audio, "mp4");
	else
		fprintf(stderr, "mp4 audio=none\n");

	if (opt.auto_play) {
		opt.display = track.is_video;
		opt.audio_play = audio.is_audio;
	}
	if (opt.display && opt.audio_play && !opt.no_pace) {
		if (av_start_clock_init(&av_clock)) {
			fprintf(stderr, "A/V start clock init failed\n");
			goto out;
		}
		av_clock_initialized = true;
		opt.av_clock = &av_clock;
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

		ret = video_decode(file, file_size, &track, &opt,
				   seamless_loop) ? 1 : 0;

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
	if (av_clock_initialized)
		av_start_clock_destroy(&av_clock);
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

	if (install_signal_handlers()) {
		fprintf(stderr, "failed to install signal handlers: %s\n",
			strerror(errno));
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
			if (playback_aborted(&opt))
				goto out;
			if (ret && !opt.loop)
				goto out;
		}
	} while (opt.loop && !playback_aborted(&opt));

out:
	playlist_free(&list);
	if (playback_aborted(&opt))
		ret = 130;
	return ret;
}
