// SPDX-License-Identifier: GPL-2.0
#include "fplayerdemo.h"

static void copy_sps_info(struct h264_stream_info *info, const H264SPS *sps)
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

static void copy_pps_info(struct h264_stream_info *info, const H264PPS *pps)
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

static int prime_h264_parser(H264NalParser *parser,
			     const struct video_track *t,
			     struct h264_stream_info *info)
{
	unsigned int i;

	for (i = 0; i < t->num_sps; i++) {
		H264NalUnit nalu;
		H264SPS sps;
		H264ParserResult res;
		uint8_t *tmp;

		tmp = malloc(t->sps[i].size + 4);
		if (!tmp)
			return -1;
		tmp[0] = (t->sps[i].size >> 24) & 0xff;
		tmp[1] = (t->sps[i].size >> 16) & 0xff;
		tmp[2] = (t->sps[i].size >> 8) & 0xff;
		tmp[3] = t->sps[i].size & 0xff;
		memcpy(tmp + 4, t->sps[i].data, t->sps[i].size);

		res = h264_parser_identify_nalu_avc(parser, tmp, 0,
							t->sps[i].size + 4, 4,
							&nalu);
		if (res == H264_PARSER_OK)
			res = h264_parser_parse_sps(parser, &nalu, &sps);
		free(tmp);
		if (res != H264_PARSER_OK)
			return -1;
		copy_sps_info(info, &sps);
		if (h264_parser_update_sps(parser, &sps) != H264_PARSER_OK)
			return -1;
	}

	for (i = 0; i < t->num_pps; i++) {
		H264NalUnit nalu;
		H264PPS pps;
		H264ParserResult res;
		uint8_t *tmp;

		tmp = malloc(t->pps[i].size + 4);
		if (!tmp)
			return -1;
		tmp[0] = (t->pps[i].size >> 24) & 0xff;
		tmp[1] = (t->pps[i].size >> 16) & 0xff;
		tmp[2] = (t->pps[i].size >> 8) & 0xff;
		tmp[3] = t->pps[i].size & 0xff;
		memcpy(tmp + 4, t->pps[i].data, t->pps[i].size);

		res = h264_parser_identify_nalu_avc(parser, tmp, 0,
							t->pps[i].size + 4, 4,
							&nalu);
		if (res == H264_PARSER_OK)
			res = h264_parser_parse_pps(parser, &nalu, &pps);
		free(tmp);
		if (res != H264_PARSER_OK)
			return -1;
		copy_pps_info(info, &pps);
		if (h264_parser_update_pps(parser, &pps) != H264_PARSER_OK)
			return -1;
	}

	return 0;
}

int walk_h264_samples(const uint8_t *file, const struct video_track *t,
			     const struct options *opt, struct stats *stats)
{
	H264NalParser *parser;
	struct h264_stream_info info = { 0 };
	uint32_t i = 0;
	int ret = -1;

	parser = h264_nal_parser_new();
	if (!parser)
		return -1;

	if (prime_h264_parser(parser, t, &info)) {
		fprintf(stderr, "failed to parse avcC SPS/PPS\n");
		goto out;
	}

	for (;;) {
		struct track_packet packet;
		const struct sample_info *s;
		const uint8_t *data;
		uint32_t off = 0;
		uint64_t sample_start = now_us();
		int packet_ret = track_packet_get(file, t, i, &packet, opt);

		if (packet_ret < 0)
			goto out;
		if (!packet_ret)
			break;
		s = &packet.sample;
		data = packet.data;

		stats->samples++;
		stats->bytes += s->size;

		while (off < s->size) {
			H264NalUnit nalu;
			H264ParserResult pres;
			uint64_t stage = now_us();

			pres = identify_h264_nalu(parser, t, data, off, s->size,
						  &nalu);
			stats->identify_us += now_us() - stage;
			if (pres != H264_PARSER_OK) {
				stats->parse_errors++;
				break;
			}

			stats->nalus++;
			switch (nalu.type) {
			case H264_NAL_SPS:
			case H264_NAL_SUBSET_SPS:
				stats->sps++;
				stage = now_us();
				pres = h264_parser_parse_nal(parser, &nalu);
				stats->parse_us += now_us() - stage;
				break;
			case H264_NAL_PPS:
				stats->pps++;
				stage = now_us();
				pres = h264_parser_parse_nal(parser, &nalu);
				stats->parse_us += now_us() - stage;
				break;
			case H264_NAL_SEI:
				stats->sei++;
				break;
			case H264_NAL_AU_DELIMITER:
				stats->aud++;
				break;
			case H264_NAL_SLICE:
			case H264_NAL_SLICE_DPA:
			case H264_NAL_SLICE_DPB:
			case H264_NAL_SLICE_DPC:
			case H264_NAL_SLICE_IDR: {
				H264SliceHdr slice;
				uint32_t stype;

				stats->slices++;
				if (nalu.type == H264_NAL_SLICE_IDR)
					stats->idr++;

				stage = now_us();
				pres = h264_parser_parse_slice_hdr(parser,
								       &nalu,
								       &slice,
								       true,
								       true);
				stats->parse_us += now_us() - stage;
				if (pres == H264_PARSER_OK) {
					stype = slice.type % 5;
					if (stype == H264_P_SLICE)
						stats->p_slices++;
					else if (stype == H264_B_SLICE)
						stats->b_slices++;
					else if (stype == H264_I_SLICE)
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
					if ((stype == H264_P_SLICE &&
					     slice.pps->weighted_pred_flag) ||
					    (stype == H264_B_SLICE &&
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

			if (pres != H264_PARSER_OK)
				stats->parse_errors++;

			off = nalu.offset + nalu.size;
		}


		{
			uint64_t elapsed = now_us() - sample_start;
			if (elapsed > stats->max_sample_us)
				stats->max_sample_us = elapsed;
		}
		track_packet_put(&packet);
		i++;
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
	h264_nal_parser_free(parser);
	return ret;
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
	int request_fd;
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
			  const H264SPS *sps)
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

static bool h264_scaling_matrix_present(const H264PPS *pps)
{
	return pps->sequence->scaling_matrix_present_flag ||
	       pps->pic_scaling_matrix_present_flag;
}

static void fill_v4l2_pps(struct v4l2_ctrl_h264_pps *out,
			  const H264PPS *pps, bool scaling_matrix_present)
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
			      const H264PPS *pps)
{
	unsigned int i;
	unsigned int n;

	memset(out, 0, sizeof(*out));

	for (i = 0; i < ARRAY_SIZE(pps->scaling_lists_4x4); i++)
		h264_quant_matrix_4x4_get_raster_from_zigzag(
			out->scaling_list_4x4[i], pps->scaling_lists_4x4[i]);

	n = pps->sequence->chroma_format_idc == 3 ? 6 : 2;
	for (i = 0; i < n; i++)
		h264_quant_matrix_8x8_get_raster_from_zigzag(
			out->scaling_list_8x8[i], pps->scaling_lists_8x8[i]);
}

static void fill_v4l2_pred_weight(struct v4l2_ctrl_h264_pred_weights *out,
				  const H264SliceHdr *slice)
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

	if (slice->type % 5 != H264_B_SLICE)
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

static uint32_t h264_slice_header_bit_size(const H264NalUnit *nalu,
					   const H264SliceHdr *slice)
{
	return 8 * nalu->header_bytes + slice->header_size -
	       8 * slice->n_emulation_prevention_bytes;
}

static void fill_v4l2_slice_params(struct v4l2_ctrl_h264_slice_params *out,
				   const H264NalUnit *nalu,
				   const H264SliceHdr *slice)
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

static void poc_type0_calc(const H264NalUnit *nalu,
			   const H264SliceHdr *slice,
			   const struct poc_state *state,
			   int32_t *top_poc, int32_t *bottom_poc,
			   int32_t *poc_msb)
{
	uint32_t max_poc_lsb = 1U << (slice->pps->sequence->log2_max_pic_order_cnt_lsb_minus4 + 4);
	uint32_t half_max_poc_lsb = max_poc_lsb / 2;
	uint32_t poc_lsb = slice->pic_order_cnt_lsb;
	int32_t msb;

	if (nalu->type == H264_NAL_SLICE_IDR) {
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

static void poc_type1_calc(const H264NalUnit *nalu,
			   const H264SliceHdr *slice,
			   const struct poc_state *state,
			   int32_t *top_poc, int32_t *bottom_poc,
			   uint32_t *frame_num_offset)
{
	const H264SPS *sps = slice->pps->sequence;
	uint32_t num_cycle = sps->num_ref_frames_in_pic_order_cnt_cycle;
	int32_t expected_delta_cycle = 0;
	int32_t expected_poc;
	uint32_t abs_frame_num;
	unsigned int i;

	/* H.264 8.2.1.2: derive FrameNumOffset like poc_type2_calc(). */
	if (nalu->type == H264_NAL_SLICE_IDR)
		*frame_num_offset = 0;
	else if (state->prev_frame_num > slice->frame_num)
		*frame_num_offset = state->prev_frame_num_offset +
				    sps->max_frame_num;
	else
		*frame_num_offset = state->prev_frame_num_offset;

	if (num_cycle != 0)
		abs_frame_num = *frame_num_offset + slice->frame_num;
	else
		abs_frame_num = 0;

	if (!nalu->ref_idc && abs_frame_num > 0)
		abs_frame_num -= 1;

	for (i = 0; i < num_cycle; i++)
		expected_delta_cycle += sps->offset_for_ref_frame[i];

	if (abs_frame_num > 0) {
		uint32_t cycle_cnt = (abs_frame_num - 1) / num_cycle;
		uint32_t frame_in_cycle = (abs_frame_num - 1) % num_cycle;

		expected_poc = (int32_t)cycle_cnt * expected_delta_cycle;
		for (i = 0; i <= frame_in_cycle; i++)
			expected_poc += sps->offset_for_ref_frame[i];
	} else {
		expected_poc = 0;
	}

	if (!nalu->ref_idc)
		expected_poc += sps->offset_for_non_ref_pic;

	/* Frame pictures only; field_pic_flag is rejected before POC calc. */
	*top_poc = expected_poc + slice->delta_pic_order_cnt[0];
	*bottom_poc = *top_poc + sps->offset_for_top_to_bottom_field +
		      slice->delta_pic_order_cnt[1];
}

static void poc_type2_calc(const H264NalUnit *nalu,
			   const H264SliceHdr *slice,
			   const struct poc_state *state,
			   int32_t *top_poc, int32_t *bottom_poc,
			   uint32_t *frame_num_offset)
{
	int32_t poc;

	if (nalu->type == H264_NAL_SLICE_IDR) {
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
				 const H264NalUnit *nalu,
				 const H264SliceHdr *slice,
				 int32_t poc_msb, uint32_t frame_num_offset)
{
	if (!nalu->ref_idc)
		goto update_frame_num;

	if (nalu->type == H264_NAL_SLICE_IDR) {
		state->prev_ref_poc_msb = 0;
		state->prev_ref_poc_lsb = 0;
		goto update_frame_num;
	}

	if (slice->dec_ref_pic_marking.adaptive_ref_pic_marking_mode_flag) {
		unsigned int i;

		for (i = 0; i < slice->dec_ref_pic_marking.n_ref_pic_marking; i++) {
			const H264RefPicMarking *mmco =
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
				    const H264NalUnit *nalu,
				    const H264SliceHdr *slice,
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
	out->flags = (nalu->type == H264_NAL_SLICE_IDR ?
		      V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC : 0) |
		     (slice->field_pic_flag ?
		      V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC : 0) |
		     (slice->bottom_field_flag ?
		      V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD : 0) |
		     (stype == H264_P_SLICE ?
		      V4L2_H264_DECODE_PARAM_FLAG_PFRAME : 0) |
		     (stype == H264_B_SLICE ?
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
			       const H264SliceHdr *slice,
			       bool list1,
			       struct ref_entry *list,
			       uint32_t *len)
{
	const H264RefPicListModification *mods;
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
		const H264RefPicListModification *mod = &mods[i];
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
			   const H264SliceHdr *slice,
			   int32_t curr_poc,
			   struct ref_entry *list0, uint32_t *len0,
			   struct ref_entry *list1, uint32_t *len1)
{
	uint32_t stype = slice->type % 5;

	*len0 = 0;
	*len1 = 0;

	if (stype == H264_I_SLICE)
		return 0;

	if (stype == H264_P_SLICE || stype == H264_SP_SLICE) {
		*len0 = build_default_ref_list_p(dpb, list0);
		if (apply_ref_list_mods(dpb, slice, false, list0, len0))
			return -1;
		trim_ref_list(len0, slice->num_ref_idx_l0_active_minus1 + 1);
		return 0;
	}

	if (stype == H264_B_SLICE) {
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
			     const H264NalUnit *nalu,
			     const H264SliceHdr *slice,
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

	if (nalu->type == H264_NAL_SLICE_IDR) {
		dpb_clear(dpb, dec);
		dpb->max_long_term_frame_idx =
			slice->dec_ref_pic_marking.long_term_reference_flag ? 0 : -1;
		make_long = slice->dec_ref_pic_marking.long_term_reference_flag;
		long_idx = 0;
	} else {
		if (slice->dec_ref_pic_marking.adaptive_ref_pic_marking_mode_flag) {
			for (i = 0; i < slice->dec_ref_pic_marking.n_ref_pic_marking; i++) {
				const H264RefPicMarking *mmco =
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

	if (!t->samples || !t->sample_count)
		return 0;

	for (i = 0; i < t->sample_count; i++) {
		if (t->samples[i].size > max)
			max = t->samples[i].size;
	}

	return max;
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

static int set_h264_decode_mode(int fd)
{
	struct v4l2_control ctrl = {
		.id = V4L2_CID_STATELESS_H264_DECODE_MODE,
		.value = V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED,
	};

	if (xioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
		fprintf(stderr, "v4l2 set H.264 slice decode mode failed: %s\n",
			strerror(errno));
		return -1;
	}
	ctrl.id = V4L2_CID_STATELESS_H264_START_CODE;
	ctrl.value = V4L2_STATELESS_H264_START_CODE_NONE;
	if (xioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
		fprintf(stderr, "v4l2 set H.264 start-code mode failed: %s\n",
			strerror(errno));
		return -1;
	}
	return 0;
}

static int reqbufs_exact(int fd, enum v4l2_buf_type type, uint32_t count,
			 uint32_t required_caps, const char *label)
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
	if ((req.capabilities & required_caps) != required_caps) {
		fprintf(stderr,
			"v4l2 REQBUFS %s missing capabilities 0x%x (got 0x%x)\n",
			label, required_caps, req.capabilities);
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

	if (dec->request_fd >= 0)
		close(dec->request_fd);

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
	dec->request_fd = -1;
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
	if (set_h264_decode_mode(dec->video_fd))
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

	if (reqbufs_exact(dec->video_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 1,
			  V4L2_BUF_CAP_SUPPORTS_REQUESTS |
			  V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF,
			  "output"))
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
	dec->request_fd = -1;
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
	fb.flags = DRM_MODE_FB_MODIFIERS;
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
	       (track->sample_count && track->samples[0].has_time ? "mp4-pts" :
		"fixed");

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
	uint64_t stage;
	uint64_t t;
	uint64_t target_us = 0;
	int idx;

	if (!q->count)
		return 0;

	idx = display_queue_lowest(q);
	if (idx < 0)
		return -1;

	frame = q->frames[idx];
	memmove(&q->frames[idx], &q->frames[idx + 1],
		(q->count - idx - 1) * sizeof(q->frames[0]));
	q->count--;

	if (!kms->base_us) {
		uint64_t base_us = playback_base_get(opt);

		kms->base_us = base_us ? base_us : now_us();
	}
	if (!opt->no_pace && (frame.has_time || kms->delay_ms)) {
		if (frame.has_time)
			target_us = kms->base_us + frame.pts_us;
		else
			target_us = kms->base_us +
				    (uint64_t)kms->frames * kms->delay_ms * 1000;
		stage = opt->profile ? now_us() : 0;
		if (sleep_until_us(target_us, opt)) {
			capture_put(dec, frame.capture_index);
			return -ECANCELED;
		}
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
	if (opt->profile) {
		uint64_t present_us = now_us();

		if (kms->last_present_us) {
			uint64_t interval = present_us - kms->last_present_us;

			kms->interval_us += interval;
			kms->interval_count++;
			if (!kms->min_interval_us || interval < kms->min_interval_us)
				kms->min_interval_us = interval;
			if (interval > kms->max_interval_us)
				kms->max_interval_us = interval;
		}
		kms->last_present_us = present_us;
		if (target_us && present_us > target_us) {
			uint64_t late = present_us - target_us;

			kms->late_us += late;
			if (late > kms->max_late_us)
				kms->max_late_us = late;
			if (late > (uint64_t)kms->delay_ms * 1000)
				kms->late_frames++;
		}
	}

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
				  bool first_slice,
				  bool last_slice,
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
		.flags = V4L2_BUF_FLAG_REQUEST_FD |
			 (last_slice ? 0 : V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF),
	};
	struct v4l2_buffer cap = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
		.index = capture_index,
		.bytesused = dec->capture[capture_index].length,
	};
	struct pollfd pfd;
	int request_fd;
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

	/*
	 * Allocate the media request fd once and reuse it for every frame via
	 * MEDIA_REQUEST_IOC_REINIT, saving an alloc+close syscall pair per
	 * frame. The fd is closed in lite_v4l2_close(). A failed request is
	 * closed below so the next call re-allocates from a clean state.
	 */
	if (profile)
		stage = now_us();
	if (dec->request_fd < 0) {
		if (xioctl(dec->media_fd, MEDIA_IOC_REQUEST_ALLOC,
			   &dec->request_fd) < 0) {
			fprintf(stderr, "MEDIA_IOC_REQUEST_ALLOC failed: %s\n",
				strerror(errno));
			return -1;
		}
	} else if (xioctl(dec->request_fd, MEDIA_REQUEST_IOC_REINIT,
			  NULL) < 0) {
		fprintf(stderr, "MEDIA_REQUEST_IOC_REINIT failed: %s\n",
			strerror(errno));
		close(dec->request_fd);
		dec->request_fd = -1;
		return -1;
	}
	request_fd = dec->request_fd;
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

	if (first_slice && xioctl(dec->video_fd, VIDIOC_QBUF, &cap) < 0) {
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

	if (last_slice) {
		memset(&cap, 0, sizeof(cap));
		cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		cap.memory = V4L2_MEMORY_MMAP;
		if (xioctl(dec->video_fd, VIDIOC_DQBUF, &cap) < 0) {
			fprintf(stderr, "v4l2 DQBUF capture failed: %s\n",
				strerror(errno));
			goto out;
		}
	}
	if (profile)
		profile_add_us(&profile->dq_us, &profile->max_dq_us,
			       now_us() - stage);

	if (last_slice &&
	    (uint32_t)(cap.timestamp.tv_sec * 1000000 + cap.timestamp.tv_usec) !=
	    frame_num)
		fprintf(stderr, "capture timestamp mismatch got=%lu.%06lu want=%u\n",
			(unsigned long)cap.timestamp.tv_sec,
			(unsigned long)cap.timestamp.tv_usec, frame_num);

	ret = 0;
out:
	/*
	 * On failure the request fd may be left half-queued; drop it so the next
	 * frame re-allocates a clean one. On success it is kept for reuse and
	 * closed in lite_v4l2_close().
	 */
	if (ret && dec->request_fd >= 0) {
		close(dec->request_fd);
		dec->request_fd = -1;
	}
	return ret;
}

static bool h264_sample_has_later_slice(H264NalParser *parser,
					const struct video_track *track,
					const uint8_t *data, uint32_t off,
					uint32_t size)
{
	while (off < size) {
		H264NalUnit nalu;

		if (identify_h264_nalu(parser, track, data, off, size, &nalu) !=
		    H264_PARSER_OK)
			return false;
		off = nalu.offset + nalu.size;
		if (nalu.type == H264_NAL_SLICE ||
		    nalu.type == H264_NAL_SLICE_IDR)
			return true;
	}

	return false;
}

static bool h264_slices_same_picture(const H264NalUnit *first_nalu,
				     const H264SliceHdr *first,
				     const H264NalUnit *nalu,
				     const H264SliceHdr *slice)
{
	const H264SPS *sps = first->pps->sequence;
	bool first_idr = first_nalu->type == H264_NAL_SLICE_IDR;
	bool idr = nalu->type == H264_NAL_SLICE_IDR;

	if (first->frame_num != slice->frame_num || first->pps != slice->pps ||
	    first->field_pic_flag != slice->field_pic_flag ||
	    first->bottom_field_flag != slice->bottom_field_flag ||
	    (!!first_nalu->ref_idc != !!nalu->ref_idc) || first_idr != idr)
		return false;
	if (first_idr && first->idr_pic_id != slice->idr_pic_id)
		return false;
	if (sps->pic_order_cnt_type == 0 &&
	    (first->pic_order_cnt_lsb != slice->pic_order_cnt_lsb ||
	     first->delta_pic_order_cnt_bottom !=
	     slice->delta_pic_order_cnt_bottom))
		return false;
	if (sps->pic_order_cnt_type == 1 &&
	    (first->delta_pic_order_cnt[0] != slice->delta_pic_order_cnt[0] ||
	     first->delta_pic_order_cnt[1] != slice->delta_pic_order_cnt[1]))
		return false;

	return true;
}

int decode_count_continuous(const uint8_t *file, size_t file_size,
				   const struct video_track *track,
				   const struct options *opt,
				   bool seamless_loop)
{
	H264NalParser *parser;
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
	struct file_window win = { 0 };
	struct track_packet packet = { 0 };
	bool have_dec = false;
	bool have_kms = false;
	uint32_t decoded = 0;
	uint32_t scanned = 0;
	uint64_t start_us = now_us();
	uint64_t submit_us = 0;
	uint64_t max_submit_us = 0;
	int ret = -1;

	memset(&dec, 0, sizeof(dec));
	dec.video_fd = -1;
	dec.media_fd = -1;
	dec.request_fd = -1;
	memset(&kms, 0, sizeof(kms));
	kms.fd = -1;
	display_queue_init(&display, &info);
	poc_state_reset(&poc);

	parser = h264_nal_parser_new();
	if (!parser)
		return -1;

	if (prime_h264_parser(parser, track, &info)) {
		fprintf(stderr, "failed to parse avcC SPS/PPS\n");
		goto out;
	}
	display_queue_init(&display, &info);
	if (!opt->display)
		usage_report_maybe(&usage, opt, decoded, decoded, NULL, submit_us,
				   &submit, false);

	file_window_init(&win, file, file_size, true, !opt->audio_play);

loop_start:
	scanned = 0;
	for (scanned = 0;; scanned++) {
		const struct sample_info *s;
		const uint8_t *data;
		H264NalUnit picture_nalu = { 0 };
		H264SliceHdr picture_slice = { 0 };
		uint32_t off = 0;
		bool submitted_sample = false;
		uint32_t frame_id = scanned + 1;
		int32_t top_poc = 0, bottom_poc = 0, curr_poc = 0, poc_msb = 0;
		uint32_t frame_num_offset = 0;
		int capture_index = -1;
		int packet_ret = track_packet_get(file, track, scanned, &packet, opt);

		if (packet_ret < 0) {
			ret = packet_ret;
			goto out;
		}
		if (!packet_ret)
			break;
		s = &packet.sample;
		data = packet.data;

		if (playback_aborted(opt)) {
			ret = -ECANCELED;
			goto out;
		}

		file_window_advance(&win, s->offset);

		while (off < s->size) {
			H264NalUnit nalu;
			H264SliceHdr slice;
			H264ParserResult pres;
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
			uint64_t stage;
			bool first_slice;
			bool last_slice;

			if (playback_aborted(opt)) {
				ret = -ECANCELED;
				goto out;
			}

			pres = identify_h264_nalu(parser, track, data, off,
						  s->size, &nalu);
			if (pres != H264_PARSER_OK)
				break;

			off = nalu.offset + nalu.size;

			if (nalu.type != H264_NAL_SLICE &&
			    nalu.type != H264_NAL_SLICE_IDR)
				continue;

			memset(&slice, 0, sizeof(slice));
			pres = h264_parser_parse_slice_hdr(parser, &nalu,
							       &slice, true,
							       true);
			if (pres != H264_PARSER_OK) {
				fprintf(stderr, "sample %u slice parse failed: %d\n",
					scanned + 1, pres);
				goto out;
			}

			if (!slice.pps || !slice.pps->sequence) {
				fprintf(stderr, "sample %u missing PPS/SPS\n",
					scanned + 1);
				goto out;
			}

			first_slice = !submitted_sample;
			last_slice = !h264_sample_has_later_slice(parser, track, data,
								 off, s->size);
			if (!first_slice &&
			    !h264_slices_same_picture(&picture_nalu, &picture_slice,
						      &nalu, &slice)) {
				fprintf(stderr,
					"sample %u contains slices from multiple pictures\n",
					scanned + 1);
				goto out;
			}

			if (slice.field_pic_flag ||
			    slice.pps->sequence->pic_order_cnt_type > 2) {
				fprintf(stderr,
					"sample %u unsupported count slice: field=%u poc_type=%u\n",
					scanned + 1, slice.field_pic_flag,
					slice.pps->sequence->pic_order_cnt_type);
				goto out;
			}

			if (first_slice && !dpb.max_frame_num) {
				dpb.max_frame_num = slice.pps->sequence->max_frame_num;
				dpb.max_num_ref_frames =
					slice.pps->sequence->num_ref_frames;
			}

			if (first_slice) {
				if (slice.pps->sequence->pic_order_cnt_type == 0)
					poc_type0_calc(&nalu, &slice, &poc, &top_poc,
						       &bottom_poc, &poc_msb);
				else if (slice.pps->sequence->pic_order_cnt_type == 1)
					poc_type1_calc(&nalu, &slice, &poc, &top_poc,
						       &bottom_poc, &frame_num_offset);
				else
					poc_type2_calc(&nalu, &slice, &poc, &top_poc,
						       &bottom_poc, &frame_num_offset);
				curr_poc = top_poc < bottom_poc ? top_poc : bottom_poc;
				dpb_update_pic_nums(&dpb, slice.frame_num);

				if (opt->display && have_kms && decoded &&
				    nalu.type == H264_NAL_SLICE_IDR &&
				    display_queue_drain(&display, &dec, &kms, opt))
					goto out;

				picture_nalu = nalu;
				picture_slice = slice;
				submitted_sample = true;
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
					av_start_clock_publish(opt);
				}
			}

			if (first_slice) {
				capture_index = lite_capture_alloc(&dec);
				if (capture_index < 0) {
					fprintf(stderr,
						"sample %u no free capture buffer\n",
						scanned + 1);
					goto out;
				}
			}

			stage = opt->profile ? now_us() : 0;
			if (lite_v4l2_submit_slice(&dec, frame_id,
						   capture_index,
						   nalu.data + nalu.offset,
						   nalu.size,
						   first_slice, last_slice,
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

			if (!last_slice)
				continue;

			if (opt->display &&
			    display_queue_push(&display, &dec, capture_index,
					       frame_id, curr_poc,
					       s->pts_us, s->has_time)) {
				capture_put(&dec, capture_index);
				goto out;
			}

			if (dpb_store_current(&dpb, &dec, &picture_nalu,
					      &picture_slice,
					      frame_id, capture_index, top_poc,
					      bottom_poc))
				goto out;
			poc_state_update_ref(&poc, &picture_nalu, &picture_slice, poc_msb,
					     frame_num_offset);

			if (opt->display &&
			    display_queue_maybe_show(&display, &dec, &kms, opt))
				goto out;

			decoded++;
			if (!opt->display || kms.frames || usage.initialized)
				usage_report_maybe(&usage, opt, decoded,
						   opt->display ? kms.frames : decoded,
						   opt->display && have_kms ? &kms : NULL,
						   submit_us, &submit, false);
		}
		track_packet_put(&packet);
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
		file_window_reset(&win);
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
		double n = decoded ? (double)decoded : 1.0;
		/*
		 * Per-stage split of the submit path. "prep" (request + controls +
		 * copy + queue) is CPU work done before the hardware runs;
		 * "wait" is the poll on the request completing (hardware decode); "dq"
		 * is buffer dequeue. prep+dq is the time that a multi-buffer output
		 * pipeline could overlap with wait, so it bounds the win from
		 * pipelining. "copy" includes any source page fault plus the write into
		 * the V4L2 output buffer.
		 */
		double prep_avg = (double)(submit.request_us + submit.controls_us +
					   submit.copy_us + submit.queue_us) / n;
		double wait_avg = (double)submit.wait_us / n;
		double dq_avg = (double)submit.dq_us / n;

		fprintf(stderr,
			"profile frames=%u avg_us: submit=%.1f commit=%.1f sleep=%.1f max_us: submit=%" PRIu64 " commit=%" PRIu64 " sleep=%" PRIu64 "\n",
			decoded, submit_avg, commit_avg, sleep_avg,
			max_submit_us, kms.max_commit_us, kms.max_sleep_us);
		fprintf(stderr,
			"profile submit avg_us: request=%.1f controls=%.1f copy=%.1f queue=%.1f wait=%.1f dq=%.1f overlap_prep+dq=%.1f max_us: request=%" PRIu64 " controls=%" PRIu64 " copy=%" PRIu64 " queue=%" PRIu64 " wait=%" PRIu64 " dq=%" PRIu64 "\n",
			(double)submit.request_us / n,
			(double)submit.controls_us / n,
			(double)submit.copy_us / n,
			(double)submit.queue_us / n,
			wait_avg, dq_avg, prep_avg + dq_avg,
			submit.max_request_us, submit.max_controls_us,
			submit.max_copy_us, submit.max_queue_us,
			submit.max_wait_us, submit.max_dq_us);
		fprintf(stderr,
			"profile cadence intervals=%u avg_us=%.1f min_us=%" PRIu64
			" max_us=%" PRIu64 " late_avg_us=%.1f late_max_us=%" PRIu64
			" late_over_frame=%u\n",
			kms.interval_count,
			kms.interval_count ?
			(double)kms.interval_us / kms.interval_count : 0.0,
			kms.min_interval_us, kms.max_interval_us,
			kms.frames ? (double)kms.late_us / kms.frames : 0.0,
			kms.max_late_us, kms.late_frames);
	}
out:
	track_packet_put(&packet);
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
	h264_nal_parser_free(parser);
	return ret;
}
