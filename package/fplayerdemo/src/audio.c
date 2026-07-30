// SPDX-License-Identifier: GPL-2.0
#include "fplayerdemo.h"

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

		if (frame->format == AV_SAMPLE_FMT_S16 && frame->data[0]) {
			ret = write_pcm_all(pcm, (const int16_t *)frame->data[0],
					    frames, frame_channels, stats, opt);
		} else {
			ret = ffmpeg_frame_to_s16(frame, frame_channels, s16_buf,
						  s16_capacity, &frames);
			profile_add_us(&stats->convert_us, &stats->max_convert_us,
				       now_us() - stage);
			if (ret)
				return ret;
			ret = write_pcm_all(pcm, *s16_buf, frames,
					    frame_channels, stats, opt);
		}
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
	const char *decoder_name = fixed ? "aac_fixed" : NULL;
	const AVCodec *codec;
	AVCodecContext *ctx = NULL;
	AVPacket *pkt = NULL;
	AVFrame *frame = NULL;
	struct track_packet input = { 0 };
	snd_pcm_t *pcm = NULL;
	struct audio_play_stats stats = { 0 };
	uint8_t *pkt_buf = NULL;
	int16_t *s16_buf = NULL;
	size_t s16_capacity = 0;
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
	/* MP4 AAC samples need AudioSpecificConfig from the container. */
	if (!audio->audio_specific_config.size) {
		fprintf(stderr, "AAC AudioSpecificConfig missing\n");
		return -1;
	}

	codec = decoder_name ? avcodec_find_decoder_by_name(decoder_name) : NULL;
	if (!codec)
		codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
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
	ctx->request_sample_fmt = fixed ? AV_SAMPLE_FMT_S16 :
					  AV_SAMPLE_FMT_NONE;
	if (audio->audio_specific_config.size) {
		ctx->extradata = av_mallocz(audio->audio_specific_config.size +
					    AV_INPUT_BUFFER_PADDING_SIZE);
		if (!ctx->extradata)
			goto out;
		memcpy(ctx->extradata, audio->audio_specific_config.data,
		       audio->audio_specific_config.size);
		ctx->extradata_size = audio->audio_specific_config.size;
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

	for (i = 0; i < audio->sample_count; i++) {
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

	for (i = 0;; i++) {
		const struct sample_info *s;
		const uint8_t *src;
		uint64_t decode_elapsed = 0;
		int packet_ret = track_packet_get(file, audio, i, &input, opt);

		if (packet_ret < 0) {
			ret = packet_ret;
			goto out;
		}
		if (!packet_ret)
			break;
		s = &input.sample;
		src = input.data;

		if (playback_aborted(opt)) {
			ret = -ECANCELED;
			goto out;
		}

		stats.samples++;
		stats.input_bytes += s->size;

		av_packet_unref(pkt);
		memcpy(pkt_buf, src, s->size);
		memset(pkt_buf + s->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
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
			track_packet_put(&input);
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
		track_packet_put(&input);
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
	track_packet_put(&input);
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

int play_aac_audio(const uint8_t *file, const struct video_track *audio,
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


void *audio_thread_main(void *arg)
{
	struct audio_thread_args *audio = arg;

	set_thread_name("f1c-audio");
	(void)setpriority(PRIO_PROCESS, 0, 2);

	audio->ret = play_aac_audio(audio->file, audio->audio, &audio->opt);
	return NULL;
}
