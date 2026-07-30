// SPDX-License-Identifier: GPL-2.0
#include "fplayerdemo.h"

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

H264ParserResult identify_h264_nalu(H264NalParser *parser,
					      const struct video_track *t,
					      const uint8_t *data,
					      uint32_t offset, uint32_t size,
					      H264NalUnit *nalu)
{
	return h264_parser_identify_nalu_avc(parser, data, offset, size,
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
			t->codec = AV_CODEC_ID_AAC;
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
			if (fourcc_is(type2, "avcC")) {
				if (parse_avcc(t, file + payload2, end2 - payload2))
					return -1;
				t->codec = AV_CODEC_ID_H264;
				return 0;
			}
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

void free_track(struct video_track *t);

int parse_atoms(const uint8_t *file, size_t file_size, uint64_t start,
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

/*
 * Test whether a sample is a sync (key) frame while walking samples in
 * ascending order. *cursor tracks the position in the ascending sync_samples
 * table so the whole table is scanned once across all samples (O(n)) instead
 * of a linear search per sample. Callers must invoke this with monotonically
 * non-decreasing sample indices.
 */
static bool sample_is_sync(const struct video_track *t, uint32_t sample,
			   uint32_t *cursor)
{
	uint32_t one_based = sample + 1;

	if (!t->sync_count)
		return true;

	while (*cursor < t->sync_count && t->sync_samples[*cursor] < one_based)
		(*cursor)++;

	return *cursor < t->sync_count &&
	       t->sync_samples[*cursor] == one_based;
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

int build_sample_table(struct video_track *t, size_t file_size)
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
	uint32_t sync_cursor = 0;

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
			t->samples[sample].sync = sample_is_sync(t, sample,
								 &sync_cursor);

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

int track_packet_get(const uint8_t *file, const struct video_track *track,
                     uint32_t sample, struct track_packet *packet,
                     const struct options *opt)
{
	(void)opt;
	memset(packet, 0, sizeof(*packet));
	if (sample >= track->sample_count)
		return 0;
	packet->sample = track->samples[sample];
	packet->data = file + packet->sample.offset;
	return 1;
}

void track_packet_put(struct track_packet *packet)
{
	memset(packet, 0, sizeof(*packet));
}

uint64_t track_duration_ticks(const struct video_track *t)
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

void print_audio_summary(const struct video_track *audio,
				const char *label)
{
	uint64_t duration_ticks = track_duration_ticks(audio);
	double duration_sec = audio->timescale && duration_ticks ?
			      (double)duration_ticks / audio->timescale : 0.0;
	uint32_t sample_delta = audio->sample_count && audio->samples ?
				audio->samples[0].duration : 0;
	char sample_count[24];

	snprintf(sample_count, sizeof(sample_count), "%u", audio->sample_count);

	fprintf(stderr,
		"%s audio=%s samples=%s chunks=%u timescale=%u duration=%.3fs sample_rate=%u channels=%u sample_size=%u sample_delta=%u",
		label,
		audio->audio_format[0] ? audio->audio_format : "unknown",
		sample_count, audio->chunk_count, audio->timescale,
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

void free_track(struct video_track *t)
{
	free(t->stts);
	free(t->ctts);
	free(t->stsc);
	free(t->sample_sizes);
	free(t->chunk_offsets);
	free(t->sync_samples);
	free(t->samples);
}
