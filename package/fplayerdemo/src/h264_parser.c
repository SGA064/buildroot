// SPDX-License-Identifier: GPL-2.0
#include "h264_parser.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct bit_reader {
	const uint8_t *data;
	size_t size;
	size_t byte;
	uint32_t bit;
	unsigned int zero_bytes;
	uint8_t current;
	uint8_t bits_left;
};

struct H264NalParser {
	H264SPS sps[H264_MAX_SPS_COUNT];
	H264PPS pps[H264_MAX_PPS_COUNT];
};

static const uint8_t default_4x4_intra[16] = {
	6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42,
};

static const uint8_t default_4x4_inter[16] = {
	10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34,
};

static const uint8_t default_8x8_intra[64] = {
	6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23,
	23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
	27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31,
	31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42,
};

static const uint8_t default_8x8_inter[64] = {
	9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21,
	21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
	24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27,
	27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35,
};

static const uint8_t zigzag_4x4[16] = {
	0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15,
};

static const uint8_t zigzag_8x8[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

#define READ_BITS(br, dst, count) \
	do { \
		uint32_t value_; \
		if (!br_read_bits((br), (count), &value_)) \
			goto fail; \
		(dst) = value_; \
	} while (0)

#define READ_UE_MAX(br, dst, max_value) \
	do { \
		uint32_t value_; \
		if (!br_read_ue((br), &value_) || value_ > (uint32_t)(max_value)) \
			goto fail; \
		(dst) = value_; \
	} while (0)

#define READ_SE_RANGE(br, dst, min_value, max_value) \
	do { \
		int32_t value_; \
		if (!br_read_se((br), &value_) || value_ < (min_value) || \
		    value_ > (max_value)) \
			goto fail; \
		(dst) = value_; \
	} while (0)

static bool br_init(struct bit_reader *br, const H264NalUnit *nalu)
{
	memset(br, 0, sizeof(*br));
	if (!nalu || nalu->size <= nalu->header_bytes)
		return false;

	br->data = nalu->data + nalu->offset + nalu->header_bytes;
	br->size = nalu->size - nalu->header_bytes;
	return true;
}

static void br_clear(struct bit_reader *br)
{
	memset(br, 0, sizeof(*br));
}

static bool br_next_byte(struct bit_reader *br)
{
	while (br->byte < br->size) {
		uint8_t byte = br->data[br->byte++];

		if (br->zero_bytes >= 2 && byte == 0x03) {
			br->zero_bytes = 0;
			continue;
		}

		br->current = byte;
		br->bits_left = 8;
		if (byte == 0) {
			if (br->zero_bytes < 2)
				br->zero_bytes++;
		} else {
			br->zero_bytes = 0;
		}
		return true;
	}

	return false;
}

static bool br_read_bits(struct bit_reader *br, unsigned int count,
			 uint32_t *out)
{
	struct bit_reader next = *br;
	uint32_t value = 0;
	unsigned int i;

	if (count > 32)
		return false;

	for (i = 0; i < count; i++) {
		if (!next.bits_left && !br_next_byte(&next))
			return false;
		if (next.bit == UINT32_MAX)
			return false;

		value = (value << 1) |
			((next.current >> (--next.bits_left)) & 1);
		next.bit++;
	}
	*br = next;
	*out = value;
	return true;
}

static bool br_read_ue(struct bit_reader *br, uint32_t *out)
{
	unsigned int leading = 0;
	uint32_t bit;
	uint32_t suffix = 0;
	uint64_t value;

	for (;;) {
		if (!br_read_bits(br, 1, &bit))
			return false;
		if (bit)
			break;
		if (++leading > 31)
			return false;
	}

	if (leading && !br_read_bits(br, leading, &suffix))
		return false;
	value = ((uint64_t)1 << leading) - 1 + suffix;
	if (value > UINT32_MAX)
		return false;
	*out = value;
	return true;
}

static bool br_read_se(struct bit_reader *br, int32_t *out)
{
	uint32_t code;
	int64_t value;

	if (!br_read_ue(br, &code))
		return false;
	value = (code & 1) ? (int64_t)(code + 1) / 2 : -(int64_t)code / 2;
	if (value < INT32_MIN || value > INT32_MAX)
		return false;
	*out = value;
	return true;
}

static bool br_more_data(const struct bit_reader *br)
{
	struct bit_reader next = *br;
	uint32_t bit;

	if (!br_read_bits(&next, 1, &bit))
		return false;
	if (!bit)
		return true;

	while (br_read_bits(&next, 1, &bit))
		if (bit)
			return true;
	return false;
}

static unsigned int ceil_log2_u32(uint32_t value)
{
	unsigned int bits = 0;

	if (value <= 1)
		return 0;
	value--;
	while (value) {
		bits++;
		value >>= 1;
	}
	return bits;
}

static bool profile_has_chroma(uint8_t profile)
{
	switch (profile) {
	case 44:
	case 83:
	case 86:
	case 100:
	case 110:
	case 118:
	case 122:
	case 128:
	case 134:
	case 135:
	case 138:
	case 139:
	case 244:
		return true;
	default:
		return false;
	}
}

static bool parse_hrd(struct bit_reader *br)
{
	uint32_t count;
	uint32_t ignored;
	uint32_t i;

	if (!br_read_ue(br, &count) || count > 31 ||
	    !br_read_bits(br, 4, &ignored) || !br_read_bits(br, 4, &ignored))
		return false;
	for (i = 0; i <= count; i++) {
		if (!br_read_ue(br, &ignored) || !br_read_ue(br, &ignored) ||
		    !br_read_bits(br, 1, &ignored))
			return false;
	}
	return br_read_bits(br, 5, &ignored) &&
	       br_read_bits(br, 5, &ignored) &&
	       br_read_bits(br, 5, &ignored) &&
	       br_read_bits(br, 5, &ignored);
}

static bool parse_vui(struct bit_reader *br, struct H264VUIParams *vui)
{
	uint32_t flag;
	uint32_t value;
	uint32_t nal_hrd;
	uint32_t vcl_hrd;

	READ_BITS(br, flag, 1);
	if (flag) {
		READ_BITS(br, value, 8);
		if (value == 255) {
			READ_BITS(br, value, 16);
			READ_BITS(br, value, 16);
		}
	}
	READ_BITS(br, flag, 1);
	if (flag)
		READ_BITS(br, value, 1);
	READ_BITS(br, flag, 1);
	if (flag) {
		READ_BITS(br, value, 3);
		READ_BITS(br, value, 1);
		READ_BITS(br, flag, 1);
		if (flag) {
			READ_BITS(br, value, 8);
			READ_BITS(br, value, 8);
			READ_BITS(br, value, 8);
		}
	}
	READ_BITS(br, flag, 1);
	if (flag) {
		READ_UE_MAX(br, value, UINT32_MAX);
		READ_UE_MAX(br, value, UINT32_MAX);
	}
	READ_BITS(br, vui->timing_info_present_flag, 1);
	if (vui->timing_info_present_flag) {
		READ_BITS(br, vui->num_units_in_tick, 32);
		READ_BITS(br, vui->time_scale, 32);
		READ_BITS(br, vui->fixed_frame_rate_flag, 1);
	}
	READ_BITS(br, nal_hrd, 1);
	if (nal_hrd && !parse_hrd(br))
		goto fail;
	READ_BITS(br, vcl_hrd, 1);
	if (vcl_hrd && !parse_hrd(br))
		goto fail;
	if (nal_hrd || vcl_hrd)
		READ_BITS(br, value, 1);
	READ_BITS(br, value, 1);
	READ_BITS(br, vui->bitstream_restriction_flag, 1);
	if (vui->bitstream_restriction_flag) {
		READ_BITS(br, value, 1);
		READ_UE_MAX(br, value, UINT32_MAX);
		READ_UE_MAX(br, value, 16);
		READ_UE_MAX(br, value, 16);
		READ_UE_MAX(br, value, 16);
		READ_UE_MAX(br, vui->num_reorder_frames, UINT32_MAX);
		READ_UE_MAX(br, vui->max_dec_frame_buffering, UINT32_MAX);
	}
	return true;

fail:
	return false;
}

static bool parse_scaling_lists(struct bit_reader *br,
				uint8_t lists4[6][16], uint8_t lists8[6][64],
				const uint8_t fallback4_inter[16],
				const uint8_t fallback4_intra[16],
				const uint8_t fallback8_inter[64],
				const uint8_t fallback8_intra[64],
				unsigned int count)
{
	static const uint8_t * const defaults[12] = {
		default_4x4_intra, default_4x4_intra, default_4x4_intra,
		default_4x4_inter, default_4x4_inter, default_4x4_inter,
		default_8x8_intra, default_8x8_inter, default_8x8_intra,
		default_8x8_inter, default_8x8_intra, default_8x8_inter,
	};
	unsigned int i;

	for (i = 0; i < 12; i++) {
		uint8_t *list;
		const uint8_t *fallback;
		unsigned int size;
		uint32_t present = 0;

		if (i < 6) {
			list = lists4[i];
			size = 16;
			fallback = i < 3 ? fallback4_intra : fallback4_inter;
		} else {
			list = lists8[i - 6];
			size = 64;
			fallback = (i & 1) ? fallback8_inter : fallback8_intra;
		}

		if (i < count && !br_read_bits(br, 1, &present))
			return false;
		if (i < count && present) {
			int32_t last = 8;
			int32_t next = 8;
			unsigned int j;

			for (j = 0; j < size; j++) {
				if (next != 0) {
					int32_t delta;

					if (!br_read_se(br, &delta))
						return false;
					next = (last + delta) & 0xff;
				}
				if (j == 0 && next == 0) {
					memcpy(list, defaults[i], size);
					break;
				}
				list[j] = next == 0 ? last : next;
				last = list[j];
			}
			continue;
		}

		switch (i) {
		case 0:
		case 3:
		case 6:
		case 7:
			memcpy(list, fallback, size);
			break;
		case 1:
		case 2:
		case 4:
		case 5:
			memcpy(list, lists4[i - 1], size);
			break;
		default:
			memcpy(list, lists8[i - 8], size);
			break;
		}
	}
	return true;
}

static bool parse_sps_data(struct bit_reader *br, H264SPS *sps)
{
	static const uint8_t sub_width[] = { 1, 2, 2, 1 };
	static const uint8_t sub_height[] = { 1, 2, 1, 1 };
	uint32_t constraints;
	uint32_t value;
	uint32_t i;
	uint64_t width;
	uint64_t height;

	memset(sps, 0, sizeof(*sps));
	sps->chroma_format_idc = 1;
	memset(sps->scaling_lists_4x4, 16, sizeof(sps->scaling_lists_4x4));
	memset(sps->scaling_lists_8x8, 16, sizeof(sps->scaling_lists_8x8));

	READ_BITS(br, sps->profile_idc, 8);
	READ_BITS(br, constraints, 8);
	sps->constraint_set0_flag = !!(constraints & 0x80);
	sps->constraint_set1_flag = !!(constraints & 0x40);
	sps->constraint_set2_flag = !!(constraints & 0x20);
	sps->constraint_set3_flag = !!(constraints & 0x10);
	sps->constraint_set4_flag = !!(constraints & 0x08);
	sps->constraint_set5_flag = !!(constraints & 0x04);
	READ_BITS(br, sps->level_idc, 8);
	READ_UE_MAX(br, sps->id, H264_MAX_SPS_COUNT - 1);

	if (profile_has_chroma(sps->profile_idc)) {
		READ_UE_MAX(br, sps->chroma_format_idc, 3);
		if (sps->chroma_format_idc == 3)
			READ_BITS(br, sps->separate_colour_plane_flag, 1);
		READ_UE_MAX(br, sps->bit_depth_luma_minus8, 6);
		READ_UE_MAX(br, sps->bit_depth_chroma_minus8, 6);
		READ_BITS(br, sps->qpprime_y_zero_transform_bypass_flag, 1);
		READ_BITS(br, sps->scaling_matrix_present_flag, 1);
		if (sps->scaling_matrix_present_flag &&
		    !parse_scaling_lists(br, sps->scaling_lists_4x4,
					 sps->scaling_lists_8x8,
					 default_4x4_inter, default_4x4_intra,
					 default_8x8_inter, default_8x8_intra,
					 sps->chroma_format_idc == 3 ? 12 : 8))
			goto fail;
	}

	READ_UE_MAX(br, sps->log2_max_frame_num_minus4, 12);
	sps->max_frame_num = 1u << (sps->log2_max_frame_num_minus4 + 4);
	READ_UE_MAX(br, sps->pic_order_cnt_type, 2);
	if (sps->pic_order_cnt_type == 0) {
		READ_UE_MAX(br, sps->log2_max_pic_order_cnt_lsb_minus4, 12);
	} else if (sps->pic_order_cnt_type == 1) {
		READ_BITS(br, sps->delta_pic_order_always_zero_flag, 1);
		READ_SE_RANGE(br, sps->offset_for_non_ref_pic, INT32_MIN, INT32_MAX);
		READ_SE_RANGE(br, sps->offset_for_top_to_bottom_field,
			      INT32_MIN, INT32_MAX);
		READ_UE_MAX(br, sps->num_ref_frames_in_pic_order_cnt_cycle, 255);
		for (i = 0; i < sps->num_ref_frames_in_pic_order_cnt_cycle; i++)
			READ_SE_RANGE(br, sps->offset_for_ref_frame[i],
				      INT32_MIN, INT32_MAX);
	}

	READ_UE_MAX(br, sps->num_ref_frames, UINT32_MAX);
	READ_BITS(br, sps->gaps_in_frame_num_value_allowed_flag, 1);
	READ_UE_MAX(br, sps->pic_width_in_mbs_minus1, UINT32_MAX);
	READ_UE_MAX(br, sps->pic_height_in_map_units_minus1, UINT32_MAX);
	READ_BITS(br, sps->frame_mbs_only_flag, 1);
	if (!sps->frame_mbs_only_flag)
		READ_BITS(br, sps->mb_adaptive_frame_field_flag, 1);
	READ_BITS(br, sps->direct_8x8_inference_flag, 1);
	READ_BITS(br, sps->frame_cropping_flag, 1);
	if (sps->frame_cropping_flag) {
		READ_UE_MAX(br, sps->frame_crop_left_offset, UINT32_MAX);
		READ_UE_MAX(br, sps->frame_crop_right_offset, UINT32_MAX);
		READ_UE_MAX(br, sps->frame_crop_top_offset, UINT32_MAX);
		READ_UE_MAX(br, sps->frame_crop_bottom_offset, UINT32_MAX);
	}
	READ_BITS(br, sps->vui_parameters_present_flag, 1);
	if (sps->vui_parameters_present_flag &&
	    !parse_vui(br, &sps->vui_parameters))
		goto fail;

	sps->chroma_array_type = sps->separate_colour_plane_flag ?
		0 : sps->chroma_format_idc;
	width = (uint64_t)(sps->pic_width_in_mbs_minus1 + 1) * 16;
	height = (uint64_t)(sps->pic_height_in_map_units_minus1 + 1) * 16 *
		 (2 - sps->frame_mbs_only_flag);
	if (!width || !height || width > INT_MAX || height > INT_MAX)
		goto fail;
	if (sps->frame_cropping_flag) {
		uint64_t crop_x = sub_width[sps->chroma_format_idc];
		uint64_t crop_y = sub_height[sps->chroma_format_idc] *
				  (2 - sps->frame_mbs_only_flag);
		uint64_t crop_w = (uint64_t)(sps->frame_crop_left_offset +
					    sps->frame_crop_right_offset) * crop_x;
		uint64_t crop_h = (uint64_t)(sps->frame_crop_top_offset +
					    sps->frame_crop_bottom_offset) * crop_y;

		if (crop_w >= width || crop_h >= height)
			goto fail;
		sps->crop_rect_x = sps->frame_crop_left_offset * crop_x;
		sps->crop_rect_y = sps->frame_crop_top_offset * crop_y;
		sps->crop_rect_width = width - crop_w;
		sps->crop_rect_height = height - crop_h;
	}
	sps->valid = true;
	return true;

fail:
	(void)value;
	return false;
}

static bool parse_pps_data(H264NalParser *parser, struct bit_reader *br,
			   H264PPS *pps)
{
	H264SPS *sps;
	uint32_t sps_id;
	uint32_t value;
	uint32_t i;
	int32_t qp_offset;

	memset(pps, 0, sizeof(*pps));
	READ_UE_MAX(br, pps->id, H264_MAX_PPS_COUNT - 1);
	READ_UE_MAX(br, sps_id, H264_MAX_SPS_COUNT - 1);
	sps = &parser->sps[sps_id];
	if (!sps->valid)
		goto fail;
	pps->sequence = sps;
	memcpy(pps->scaling_lists_4x4, sps->scaling_lists_4x4,
	       sizeof(pps->scaling_lists_4x4));
	memcpy(pps->scaling_lists_8x8, sps->scaling_lists_8x8,
	       sizeof(pps->scaling_lists_8x8));

	READ_BITS(br, pps->entropy_coding_mode_flag, 1);
	READ_BITS(br, pps->pic_order_present_flag, 1);
	READ_UE_MAX(br, pps->num_slice_groups_minus1, 7);
	if (pps->num_slice_groups_minus1) {
		READ_UE_MAX(br, pps->slice_group_map_type, 6);
		if (pps->slice_group_map_type == 0) {
			for (i = 0; i <= pps->num_slice_groups_minus1; i++)
				READ_UE_MAX(br, value, UINT32_MAX);
		} else if (pps->slice_group_map_type == 2) {
			for (i = 0; i < pps->num_slice_groups_minus1; i++) {
				READ_UE_MAX(br, value, UINT32_MAX);
				READ_UE_MAX(br, value, UINT32_MAX);
			}
		} else if (pps->slice_group_map_type >= 3 &&
			   pps->slice_group_map_type <= 5) {
			READ_BITS(br, pps->slice_group_change_direction_flag, 1);
			READ_UE_MAX(br, pps->slice_group_change_rate_minus1,
				    UINT32_MAX);
		} else if (pps->slice_group_map_type == 6) {
			uint32_t map_size;
			unsigned int bits;

			READ_UE_MAX(br, map_size, 0x00ffffff);
			bits = ceil_log2_u32(pps->num_slice_groups_minus1 + 1);
			for (i = 0; i <= map_size; i++)
				READ_BITS(br, value, bits);
		}
	}

	READ_UE_MAX(br, pps->num_ref_idx_l0_active_minus1, 31);
	READ_UE_MAX(br, pps->num_ref_idx_l1_active_minus1, 31);
	READ_BITS(br, pps->weighted_pred_flag, 1);
	READ_BITS(br, pps->weighted_bipred_idc, 2);
	qp_offset = 6 * (sps->bit_depth_luma_minus8 +
			 sps->separate_colour_plane_flag);
	READ_SE_RANGE(br, pps->pic_init_qp_minus26, -(26 + qp_offset), 25);
	READ_SE_RANGE(br, pps->pic_init_qs_minus26, -26, 25);
	READ_SE_RANGE(br, pps->chroma_qp_index_offset, -12, 12);
	pps->second_chroma_qp_index_offset = pps->chroma_qp_index_offset;
	READ_BITS(br, pps->deblocking_filter_control_present_flag, 1);
	READ_BITS(br, pps->constrained_intra_pred_flag, 1);
	READ_BITS(br, pps->redundant_pic_cnt_present_flag, 1);

	if (br_more_data(br)) {
		unsigned int lists;

		READ_BITS(br, pps->transform_8x8_mode_flag, 1);
		READ_BITS(br, pps->pic_scaling_matrix_present_flag, 1);
		if (pps->pic_scaling_matrix_present_flag) {
			lists = 6 + (sps->chroma_format_idc == 3 ? 6 : 2) *
				pps->transform_8x8_mode_flag;
			if (!parse_scaling_lists(br, pps->scaling_lists_4x4,
						 pps->scaling_lists_8x8,
						 sps->scaling_matrix_present_flag ?
						 sps->scaling_lists_4x4[3] :
						 default_4x4_inter,
						 sps->scaling_matrix_present_flag ?
						 sps->scaling_lists_4x4[0] :
						 default_4x4_intra,
						 sps->scaling_matrix_present_flag ?
						 sps->scaling_lists_8x8[1] :
						 default_8x8_inter,
						 sps->scaling_matrix_present_flag ?
						 sps->scaling_lists_8x8[0] :
						 default_8x8_intra,
						 lists))
				goto fail;
		}
		READ_SE_RANGE(br, pps->second_chroma_qp_index_offset, -12, 12);
	}
	(void)value;
	pps->valid = true;
	return true;

fail:
	return false;
}

static bool parse_ref_list(struct bit_reader *br, H264SliceHdr *slice,
			   bool list1)
{
	struct H264RefPicListModification *mods = list1 ?
		slice->ref_pic_list_modification_l1 :
		slice->ref_pic_list_modification_l0;
	uint8_t *flag = list1 ? &slice->ref_pic_list_modification_flag_l1 :
		&slice->ref_pic_list_modification_flag_l0;
	uint8_t *count = list1 ? &slice->n_ref_pic_list_modification_l1 :
		&slice->n_ref_pic_list_modification_l0;
	uint32_t value;
	unsigned int i = 0;

	if (!br_read_bits(br, 1, &value))
		return false;
	*flag = value;
	if (!*flag)
		return true;

	for (;;) {
		if (i >= H264_MAX_REFS || !br_read_ue(br, &value) || value > 3)
			return false;
		mods[i].modification_of_pic_nums_idc = value;
		if (value == 0 || value == 1) {
			if (!br_read_ue(br, &mods[i].value.abs_diff_pic_num_minus1) ||
			    mods[i].value.abs_diff_pic_num_minus1 >= slice->max_pic_num)
				return false;
		} else if (value == 2) {
			if (!br_read_ue(br, &mods[i].value.long_term_pic_num))
				return false;
		}
		i++;
		if (value == 3)
			break;
	}
	*count = i;
	return true;
}

static bool parse_pred_weights(struct bit_reader *br, H264SliceHdr *slice)
{
	struct H264PredWeightTable *weights = &slice->pred_weight_table;
	bool is_b = slice->type % 5 == H264_B_SLICE;
	bool chroma = slice->pps->sequence->chroma_array_type != 0;
	uint32_t flag;
	unsigned int i;
	unsigned int j;

	READ_UE_MAX(br, weights->luma_log2_weight_denom, 7);
	for (i = 0; i <= slice->num_ref_idx_l0_active_minus1; i++)
		weights->luma_weight_l0[i] = 1 << weights->luma_log2_weight_denom;
	if (is_b)
		for (i = 0; i <= slice->num_ref_idx_l1_active_minus1; i++)
			weights->luma_weight_l1[i] =
				1 << weights->luma_log2_weight_denom;

	if (chroma) {
		READ_UE_MAX(br, weights->chroma_log2_weight_denom, 7);
		for (i = 0; i <= slice->num_ref_idx_l0_active_minus1; i++)
			for (j = 0; j < 2; j++)
				weights->chroma_weight_l0[i][j] =
					1 << weights->chroma_log2_weight_denom;
		if (is_b)
			for (i = 0; i <= slice->num_ref_idx_l1_active_minus1; i++)
				for (j = 0; j < 2; j++)
					weights->chroma_weight_l1[i][j] =
						1 << weights->chroma_log2_weight_denom;
	}

	for (i = 0; i <= slice->num_ref_idx_l0_active_minus1; i++) {
		READ_BITS(br, flag, 1);
		if (flag) {
			READ_SE_RANGE(br, weights->luma_weight_l0[i], -128, 127);
			READ_SE_RANGE(br, weights->luma_offset_l0[i], -128, 127);
		}
		if (chroma) {
			READ_BITS(br, flag, 1);
			if (flag)
				for (j = 0; j < 2; j++) {
					READ_SE_RANGE(br, weights->chroma_weight_l0[i][j],
						      -128, 127);
					READ_SE_RANGE(br, weights->chroma_offset_l0[i][j],
						      -128, 127);
				}
		}
	}
	if (is_b) {
		for (i = 0; i <= slice->num_ref_idx_l1_active_minus1; i++) {
			READ_BITS(br, flag, 1);
			if (flag) {
				READ_SE_RANGE(br, weights->luma_weight_l1[i], -128, 127);
				READ_SE_RANGE(br, weights->luma_offset_l1[i], -128, 127);
			}
			if (chroma) {
				READ_BITS(br, flag, 1);
				if (flag)
					for (j = 0; j < 2; j++) {
						READ_SE_RANGE(br,
							      weights->chroma_weight_l1[i][j],
							      -128, 127);
						READ_SE_RANGE(br,
							      weights->chroma_offset_l1[i][j],
							      -128, 127);
					}
			}
		}
	}
	return true;

fail:
	return false;
}

static bool parse_dec_ref_marking(struct bit_reader *br,
				  const H264NalUnit *nalu,
				  H264SliceHdr *slice)
{
	struct H264DecRefPicMarking *marking = &slice->dec_ref_pic_marking;
	uint32_t start = br->bit;
	uint32_t value;

	if (nalu->idr_pic_flag) {
		READ_BITS(br, marking->no_output_of_prior_pics_flag, 1);
		READ_BITS(br, marking->long_term_reference_flag, 1);
	} else {
		READ_BITS(br, marking->adaptive_ref_pic_marking_mode_flag, 1);
		if (marking->adaptive_ref_pic_marking_mode_flag) {
			for (;;) {
				struct H264RefPicMarking *op;

				READ_UE_MAX(br, value, 6);
				if (!value)
					break;
				if (marking->n_ref_pic_marking >= H264_MAX_MMCO)
					goto fail;
				op = &marking->ref_pic_marking[
					marking->n_ref_pic_marking++];
				op->memory_management_control_operation = value;
				if (value == 1 || value == 3)
					READ_UE_MAX(br, op->difference_of_pic_nums_minus1,
						    UINT32_MAX);
				if (value == 2)
					READ_UE_MAX(br, op->long_term_pic_num, UINT32_MAX);
				if (value == 3 || value == 6)
					READ_UE_MAX(br, op->long_term_frame_idx, UINT32_MAX);
				if (value == 4)
					READ_UE_MAX(br, op->max_long_term_frame_idx_plus1,
						    UINT32_MAX);
			}
		}
	}
	marking->bit_size = br->bit - start;
	return true;

fail:
	return false;
}

static bool start_code_at(const uint8_t *data, uint32_t pos, uint32_t size,
			  uint32_t *length)
{
	if (pos + 3 <= size && !data[pos] && !data[pos + 1] && data[pos + 2] == 1) {
		*length = 3;
		return true;
	}
	if (pos + 4 <= size && !data[pos] && !data[pos + 1] &&
	    !data[pos + 2] && data[pos + 3] == 1) {
		*length = 4;
		return true;
	}
	return false;
}

static H264ParserResult init_nalu(const uint8_t *data, uint32_t offset,
				  uint32_t size, uint32_t sc_offset,
				  H264NalUnit *nalu)
{
	uint8_t header;

	if (offset >= size)
		return H264_PARSER_NO_NAL;
	header = data[offset];
	if (header & 0x80)
		return H264_PARSER_BROKEN_DATA;
	memset(nalu, 0, sizeof(*nalu));
	nalu->data = data;
	nalu->offset = offset;
	nalu->sc_offset = sc_offset;
	nalu->size = size - offset;
	nalu->ref_idc = (header >> 5) & 3;
	nalu->type = header & 0x1f;
	nalu->idr_pic_flag = nalu->type == H264_NAL_SLICE_IDR;
	nalu->header_bytes = 1;
	return H264_PARSER_OK;
}

H264NalParser *h264_nal_parser_new(void)
{
	return calloc(1, sizeof(H264NalParser));
}

void h264_nal_parser_free(H264NalParser *parser)
{
	free(parser);
}

H264ParserResult h264_parser_identify_nalu(H264NalParser *parser,
					   const uint8_t *data,
					   uint32_t offset, uint32_t size,
					   H264NalUnit *nalu)
{
	uint32_t prefix = 0;
	uint32_t start;
	uint32_t end;
	H264ParserResult result;

	(void)parser;
	for (start = offset; start < size; start++)
		if (start_code_at(data, start, size, &prefix))
			break;
	if (start == size)
		return H264_PARSER_NO_NAL;

	result = init_nalu(data, start + prefix, size, start, nalu);
	if (result != H264_PARSER_OK)
		return result;
	for (end = nalu->offset + 1; end < size; end++) {
		uint32_t next_prefix;

		if (start_code_at(data, end, size, &next_prefix)) {
			nalu->size = end - nalu->offset;
			break;
		}
	}
	return H264_PARSER_OK;
}

H264ParserResult h264_parser_identify_nalu_avc(H264NalParser *parser,
					       const uint8_t *data,
					       uint32_t offset, uint32_t size,
					       uint8_t nal_length_size,
					       H264NalUnit *nalu)
{
	uint32_t length = 0;
	uint32_t i;
	H264ParserResult result;

	(void)parser;
	if (nal_length_size < 1 || nal_length_size > 4 || offset > size ||
	    nal_length_size > size - offset)
		return H264_PARSER_BROKEN_DATA;
	for (i = 0; i < nal_length_size; i++)
		length = (length << 8) | data[offset + i];
	offset += nal_length_size;
	if (!length)
		return H264_PARSER_NO_NAL;
	if (offset > size || length > size - offset)
		return H264_PARSER_BROKEN_DATA;
	result = init_nalu(data, offset, offset + length, offset, nalu);
	if (result == H264_PARSER_OK)
		nalu->size = length;
	return result;
}

H264ParserResult h264_parser_parse_sps(H264NalParser *parser,
				       H264NalUnit *nalu, H264SPS *sps)
{
	struct bit_reader br;
	bool ok;

	(void)parser;
	if (!br_init(&br, nalu))
		return H264_PARSER_ERROR;
	ok = parse_sps_data(&br, sps);
	br_clear(&br);
	return ok ? H264_PARSER_OK : H264_PARSER_ERROR;
}

H264ParserResult h264_parser_parse_pps(H264NalParser *parser,
				       H264NalUnit *nalu, H264PPS *pps)
{
	struct bit_reader br;
	bool ok;

	if (!br_init(&br, nalu))
		return H264_PARSER_ERROR;
	ok = parse_pps_data(parser, &br, pps);
	br_clear(&br);
	return ok ? H264_PARSER_OK : H264_PARSER_BROKEN_LINK;
}

H264ParserResult h264_parser_update_sps(H264NalParser *parser, H264SPS *sps)
{
	if (!parser || !sps || !sps->valid || sps->id < 0 ||
	    sps->id >= H264_MAX_SPS_COUNT)
		return H264_PARSER_ERROR;
	parser->sps[sps->id] = *sps;
	return H264_PARSER_OK;
}

H264ParserResult h264_parser_update_pps(H264NalParser *parser, H264PPS *pps)
{
	int sps_id;

	if (!parser || !pps || !pps->valid || pps->id < 0 ||
	    pps->id >= H264_MAX_PPS_COUNT || !pps->sequence)
		return H264_PARSER_ERROR;
	sps_id = pps->sequence->id;
	if (sps_id < 0 || sps_id >= H264_MAX_SPS_COUNT ||
	    !parser->sps[sps_id].valid)
		return H264_PARSER_BROKEN_LINK;
	parser->pps[pps->id] = *pps;
	parser->pps[pps->id].sequence = &parser->sps[sps_id];
	return H264_PARSER_OK;
}

H264ParserResult h264_parser_parse_nal(H264NalParser *parser,
				       H264NalUnit *nalu)
{
	if (nalu->type == H264_NAL_SPS || nalu->type == H264_NAL_SUBSET_SPS) {
		H264SPS sps;
		H264ParserResult result = h264_parser_parse_sps(parser, nalu, &sps);

		return result == H264_PARSER_OK ?
			h264_parser_update_sps(parser, &sps) : result;
	}
	if (nalu->type == H264_NAL_PPS) {
		H264PPS pps;
		H264ParserResult result = h264_parser_parse_pps(parser, nalu, &pps);

		return result == H264_PARSER_OK ?
			h264_parser_update_pps(parser, &pps) : result;
	}
	return H264_PARSER_OK;
}

H264ParserResult h264_parser_parse_slice_hdr(H264NalParser *parser,
					     H264NalUnit *nalu,
					     H264SliceHdr *slice,
					     bool parse_pred_weight,
					     bool parse_ref_marking)
{
	struct bit_reader br;
	H264PPS *pps;
	H264SPS *sps;
	uint32_t pps_id;
	uint32_t value;
	uint32_t poc_start;
	uint32_t slice_type;
	bool is_i;
	bool is_si;
	bool is_p;
	bool is_sp;
	bool is_b;

	(void)parse_pred_weight;
	(void)parse_ref_marking;
	memset(slice, 0, sizeof(*slice));
	if (!br_init(&br, nalu))
		return H264_PARSER_ERROR;

	READ_UE_MAX(&br, slice->first_mb_in_slice, UINT32_MAX);
	READ_UE_MAX(&br, slice->type, 9);
	READ_UE_MAX(&br, pps_id, H264_MAX_PPS_COUNT - 1);
	pps = &parser->pps[pps_id];
	if (!pps->valid || !pps->sequence)
		goto broken_link;
	sps = pps->sequence;
	slice->pps = pps;
	slice_type = slice->type % 5;
	is_i = slice_type == H264_I_SLICE;
	is_si = slice_type == H264_SI_SLICE;
	is_p = slice_type == H264_P_SLICE;
	is_sp = slice_type == H264_SP_SLICE;
	is_b = slice_type == H264_B_SLICE;
	if (!is_i) {
		slice->num_ref_idx_l0_active_minus1 =
			pps->num_ref_idx_l0_active_minus1;
		if (is_b)
			slice->num_ref_idx_l1_active_minus1 =
				pps->num_ref_idx_l1_active_minus1;
	}

	if (sps->separate_colour_plane_flag)
		READ_BITS(&br, slice->colour_plane_id, 2);
	READ_BITS(&br, slice->frame_num, sps->log2_max_frame_num_minus4 + 4);
	if (!sps->frame_mbs_only_flag) {
		READ_BITS(&br, slice->field_pic_flag, 1);
		if (slice->field_pic_flag)
			READ_BITS(&br, slice->bottom_field_flag, 1);
	}
	slice->max_pic_num = slice->field_pic_flag ?
		2 * sps->max_frame_num : sps->max_frame_num;
	if (nalu->idr_pic_flag)
		READ_UE_MAX(&br, slice->idr_pic_id, UINT16_MAX);

	poc_start = br.bit;
	if (sps->pic_order_cnt_type == 0) {
		READ_BITS(&br, slice->pic_order_cnt_lsb,
			  sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
		if (pps->pic_order_present_flag && !slice->field_pic_flag)
			READ_SE_RANGE(&br, slice->delta_pic_order_cnt_bottom,
				      INT32_MIN, INT32_MAX);
	} else if (sps->pic_order_cnt_type == 1 &&
		   !sps->delta_pic_order_always_zero_flag) {
		READ_SE_RANGE(&br, slice->delta_pic_order_cnt[0],
			      INT32_MIN, INT32_MAX);
		if (pps->pic_order_present_flag && !slice->field_pic_flag)
			READ_SE_RANGE(&br, slice->delta_pic_order_cnt[1],
				      INT32_MIN, INT32_MAX);
	}
	slice->pic_order_cnt_bit_size = br.bit - poc_start;

	if (pps->redundant_pic_cnt_present_flag)
		READ_UE_MAX(&br, slice->redundant_pic_cnt, INT8_MAX);
	if (is_b)
		READ_BITS(&br, slice->direct_spatial_mv_pred_flag, 1);
	if (is_p || is_sp || is_b) {
		READ_BITS(&br, slice->num_ref_idx_active_override_flag, 1);
		if (slice->num_ref_idx_active_override_flag) {
			READ_UE_MAX(&br, slice->num_ref_idx_l0_active_minus1, 31);
			if (is_b)
				READ_UE_MAX(&br, slice->num_ref_idx_l1_active_minus1,
					    31);
		}
	}
	if (!is_i && !is_si && !parse_ref_list(&br, slice, false))
		goto fail;
	if (is_b && !parse_ref_list(&br, slice, true))
		goto fail;
	if ((pps->weighted_pred_flag && (is_p || is_sp)) ||
	    (pps->weighted_bipred_idc == 1 && is_b))
		if (!parse_pred_weights(&br, slice))
			goto fail;
	if (nalu->ref_idc && !parse_dec_ref_marking(&br, nalu, slice))
		goto fail;
	if (pps->entropy_coding_mode_flag && !is_i && !is_si)
		READ_UE_MAX(&br, slice->cabac_init_idc, 2);
	READ_SE_RANGE(&br, slice->slice_qp_delta, -87, 77);
	if (is_sp || is_si) {
		if (is_sp)
			READ_BITS(&br, slice->sp_for_switch_flag, 1);
		READ_SE_RANGE(&br, slice->slice_qs_delta, -51, 51);
	}
	if (pps->deblocking_filter_control_present_flag) {
		READ_UE_MAX(&br, slice->disable_deblocking_filter_idc, 2);
		if (slice->disable_deblocking_filter_idc != 1) {
			READ_SE_RANGE(&br, slice->slice_alpha_c0_offset_div2, -6, 6);
			READ_SE_RANGE(&br, slice->slice_beta_offset_div2, -6, 6);
		}
	}
	if (pps->num_slice_groups_minus1 && pps->slice_group_map_type >= 3 &&
	    pps->slice_group_map_type <= 5) {
		uint64_t pic_size = (uint64_t)(sps->pic_width_in_mbs_minus1 + 1) *
			(sps->pic_height_in_map_units_minus1 + 1);
		uint64_t rate = (uint64_t)pps->slice_group_change_rate_minus1 + 1;
		unsigned int bits;

		if (!rate || pic_size / rate + 1 > UINT32_MAX)
			goto fail;
		bits = ceil_log2_u32(pic_size / rate + 1);
		READ_BITS(&br, slice->slice_group_change_cycle, bits);
	}
	slice->header_size = br.bit;
	slice->n_emulation_prevention_bytes = 0;
	br_clear(&br);
	return H264_PARSER_OK;

broken_link:
	br_clear(&br);
	return H264_PARSER_BROKEN_LINK;
fail:
	(void)value;
	br_clear(&br);
	return H264_PARSER_ERROR;
}

void h264_quant_matrix_4x4_get_raster_from_zigzag(uint8_t out[16],
						  const uint8_t quant[16])
{
	unsigned int i;

	for (i = 0; i < 16; i++)
		out[zigzag_4x4[i]] = quant[i];
}

void h264_quant_matrix_8x8_get_raster_from_zigzag(uint8_t out[64],
						  const uint8_t quant[64])
{
	unsigned int i;

	for (i = 0; i < 64; i++)
		out[zigzag_8x8[i]] = quant[i];
}
