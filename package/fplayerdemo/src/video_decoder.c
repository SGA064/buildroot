// SPDX-License-Identifier: GPL-2.0
#include "fplayerdemo.h"

struct video_decoder_ops {
	const char *name;
	enum AVCodecID codec;
	int (*decode)(const uint8_t *file, size_t file_size,
		      const struct video_track *track,
		      const struct options *opt, bool seamless_loop);
};

static const struct video_decoder_ops video_decoders[] = {
	{
		.name = "h264-cedrus",
		.codec = AV_CODEC_ID_H264,
		.decode = decode_count_continuous,
	},
};

static const struct video_decoder_ops *video_decoder_find(enum AVCodecID codec)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(video_decoders); i++)
		if (video_decoders[i].codec == codec)
			return &video_decoders[i];
	return NULL;
}

int video_decode(const uint8_t *file, size_t file_size,
		 const struct video_track *track, const struct options *opt,
		 bool seamless_loop)
{
	const struct video_decoder_ops *decoder = video_decoder_find(track->codec);

	if (!decoder) {
		fprintf(stderr, "unsupported video codec: %d\n", track->codec);
		return -ENOTSUP;
	}

	return decoder->decode(file, file_size, track, opt, seamless_loop);
}
