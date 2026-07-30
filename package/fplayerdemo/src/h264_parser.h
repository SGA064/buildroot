// SPDX-License-Identifier: GPL-2.0
#ifndef FPLAYERDEMO_H264_PARSER_H
#define FPLAYERDEMO_H264_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define H264_MAX_SPS_COUNT 32
#define H264_MAX_PPS_COUNT 256
#define H264_MAX_REFS 32
#define H264_MAX_MMCO 10

enum H264ParserResult {
	H264_PARSER_OK,
	H264_PARSER_ERROR,
	H264_PARSER_NO_NAL,
	H264_PARSER_BROKEN_DATA,
	H264_PARSER_BROKEN_LINK,
};

enum H264NalUnitType {
	H264_NAL_UNKNOWN = 0,
	H264_NAL_SLICE = 1,
	H264_NAL_SLICE_DPA = 2,
	H264_NAL_SLICE_DPB = 3,
	H264_NAL_SLICE_DPC = 4,
	H264_NAL_SLICE_IDR = 5,
	H264_NAL_SEI = 6,
	H264_NAL_SPS = 7,
	H264_NAL_PPS = 8,
	H264_NAL_AU_DELIMITER = 9,
	H264_NAL_SUBSET_SPS = 15,
};

enum H264SliceType {
	H264_P_SLICE = 0,
	H264_B_SLICE = 1,
	H264_I_SLICE = 2,
	H264_SP_SLICE = 3,
	H264_SI_SLICE = 4,
};

struct H264NalUnit {
	uint16_t ref_idc;
	uint16_t type;
	uint8_t idr_pic_flag;
	uint32_t size;
	uint32_t offset;
	uint32_t sc_offset;
	const uint8_t *data;
	uint8_t header_bytes;
};

struct H264VUIParams {
	uint8_t timing_info_present_flag;
	uint32_t num_units_in_tick;
	uint32_t time_scale;
	uint8_t fixed_frame_rate_flag;
	uint8_t bitstream_restriction_flag;
	uint32_t num_reorder_frames;
	uint32_t max_dec_frame_buffering;
};

struct H264SPS {
	int id;
	uint8_t profile_idc;
	uint8_t constraint_set0_flag;
	uint8_t constraint_set1_flag;
	uint8_t constraint_set2_flag;
	uint8_t constraint_set3_flag;
	uint8_t constraint_set4_flag;
	uint8_t constraint_set5_flag;
	uint8_t level_idc;
	uint8_t chroma_format_idc;
	uint8_t separate_colour_plane_flag;
	uint8_t bit_depth_luma_minus8;
	uint8_t bit_depth_chroma_minus8;
	uint8_t qpprime_y_zero_transform_bypass_flag;
	uint8_t scaling_matrix_present_flag;
	uint8_t scaling_lists_4x4[6][16];
	uint8_t scaling_lists_8x8[6][64];
	uint8_t log2_max_frame_num_minus4;
	uint8_t pic_order_cnt_type;
	uint8_t log2_max_pic_order_cnt_lsb_minus4;
	uint8_t delta_pic_order_always_zero_flag;
	int32_t offset_for_non_ref_pic;
	int32_t offset_for_top_to_bottom_field;
	uint8_t num_ref_frames_in_pic_order_cnt_cycle;
	int32_t offset_for_ref_frame[255];
	uint32_t num_ref_frames;
	uint8_t gaps_in_frame_num_value_allowed_flag;
	uint32_t pic_width_in_mbs_minus1;
	uint32_t pic_height_in_map_units_minus1;
	uint8_t frame_mbs_only_flag;
	uint8_t mb_adaptive_frame_field_flag;
	uint8_t direct_8x8_inference_flag;
	uint8_t frame_cropping_flag;
	uint32_t frame_crop_left_offset;
	uint32_t frame_crop_right_offset;
	uint32_t frame_crop_top_offset;
	uint32_t frame_crop_bottom_offset;
	uint8_t vui_parameters_present_flag;
	struct H264VUIParams vui_parameters;
	uint8_t chroma_array_type;
	uint32_t max_frame_num;
	int crop_rect_width;
	int crop_rect_height;
	int crop_rect_x;
	int crop_rect_y;
	bool valid;
};

struct H264PPS {
	int id;
	struct H264SPS *sequence;
	uint8_t entropy_coding_mode_flag;
	uint8_t pic_order_present_flag;
	uint32_t num_slice_groups_minus1;
	uint8_t slice_group_map_type;
	uint8_t slice_group_change_direction_flag;
	uint32_t slice_group_change_rate_minus1;
	uint8_t num_ref_idx_l0_active_minus1;
	uint8_t num_ref_idx_l1_active_minus1;
	uint8_t weighted_pred_flag;
	uint8_t weighted_bipred_idc;
	int8_t pic_init_qp_minus26;
	int8_t pic_init_qs_minus26;
	int8_t chroma_qp_index_offset;
	uint8_t deblocking_filter_control_present_flag;
	uint8_t constrained_intra_pred_flag;
	uint8_t redundant_pic_cnt_present_flag;
	uint8_t transform_8x8_mode_flag;
	uint8_t scaling_lists_4x4[6][16];
	uint8_t scaling_lists_8x8[6][64];
	int8_t second_chroma_qp_index_offset;
	uint8_t pic_scaling_matrix_present_flag;
	bool valid;
};

struct H264RefPicListModification {
	uint8_t modification_of_pic_nums_idc;
	union {
		uint32_t abs_diff_pic_num_minus1;
		uint32_t long_term_pic_num;
	} value;
};

struct H264PredWeightTable {
	uint8_t luma_log2_weight_denom;
	uint8_t chroma_log2_weight_denom;
	int16_t luma_weight_l0[H264_MAX_REFS];
	int8_t luma_offset_l0[H264_MAX_REFS];
	int16_t chroma_weight_l0[H264_MAX_REFS][2];
	int8_t chroma_offset_l0[H264_MAX_REFS][2];
	int16_t luma_weight_l1[H264_MAX_REFS];
	int8_t luma_offset_l1[H264_MAX_REFS];
	int16_t chroma_weight_l1[H264_MAX_REFS][2];
	int8_t chroma_offset_l1[H264_MAX_REFS][2];
};

struct H264RefPicMarking {
	uint8_t memory_management_control_operation;
	uint32_t difference_of_pic_nums_minus1;
	uint32_t long_term_pic_num;
	uint32_t long_term_frame_idx;
	uint32_t max_long_term_frame_idx_plus1;
};

struct H264DecRefPicMarking {
	uint8_t no_output_of_prior_pics_flag;
	uint8_t long_term_reference_flag;
	uint8_t adaptive_ref_pic_marking_mode_flag;
	struct H264RefPicMarking ref_pic_marking[H264_MAX_MMCO];
	uint8_t n_ref_pic_marking;
	uint32_t bit_size;
};

struct H264SliceHdr {
	uint32_t first_mb_in_slice;
	uint32_t type;
	struct H264PPS *pps;
	uint8_t colour_plane_id;
	uint16_t frame_num;
	uint8_t field_pic_flag;
	uint8_t bottom_field_flag;
	uint16_t idr_pic_id;
	uint16_t pic_order_cnt_lsb;
	int32_t delta_pic_order_cnt_bottom;
	int32_t delta_pic_order_cnt[2];
	uint8_t redundant_pic_cnt;
	uint8_t direct_spatial_mv_pred_flag;
	uint8_t num_ref_idx_l0_active_minus1;
	uint8_t num_ref_idx_l1_active_minus1;
	uint8_t ref_pic_list_modification_flag_l0;
	uint8_t n_ref_pic_list_modification_l0;
	struct H264RefPicListModification
		ref_pic_list_modification_l0[H264_MAX_REFS];
	uint8_t ref_pic_list_modification_flag_l1;
	uint8_t n_ref_pic_list_modification_l1;
	struct H264RefPicListModification
		ref_pic_list_modification_l1[H264_MAX_REFS];
	struct H264PredWeightTable pred_weight_table;
	struct H264DecRefPicMarking dec_ref_pic_marking;
	uint8_t cabac_init_idc;
	int8_t slice_qp_delta;
	int8_t slice_qs_delta;
	uint8_t disable_deblocking_filter_idc;
	int8_t slice_alpha_c0_offset_div2;
	int8_t slice_beta_offset_div2;
	uint16_t slice_group_change_cycle;
	uint32_t max_pic_num;
	uint32_t header_size;
	uint32_t n_emulation_prevention_bytes;
	uint8_t num_ref_idx_active_override_flag;
	uint8_t sp_for_switch_flag;
	uint32_t pic_order_cnt_bit_size;
};

typedef struct H264NalParser H264NalParser;
typedef struct H264NalUnit H264NalUnit;
typedef struct H264SPS H264SPS;
typedef struct H264PPS H264PPS;
typedef struct H264SliceHdr H264SliceHdr;
typedef struct H264RefPicListModification H264RefPicListModification;
typedef struct H264RefPicMarking H264RefPicMarking;
typedef enum H264ParserResult H264ParserResult;

H264NalParser *h264_nal_parser_new(void);
void h264_nal_parser_free(H264NalParser *parser);
H264ParserResult h264_parser_identify_nalu(H264NalParser *parser,
					   const uint8_t *data,
					   uint32_t offset, uint32_t size,
					   H264NalUnit *nalu);
H264ParserResult h264_parser_identify_nalu_avc(H264NalParser *parser,
					       const uint8_t *data,
					       uint32_t offset, uint32_t size,
					       uint8_t nal_length_size,
					       H264NalUnit *nalu);
H264ParserResult h264_parser_parse_sps(H264NalParser *parser,
				       H264NalUnit *nalu, H264SPS *sps);
H264ParserResult h264_parser_parse_pps(H264NalParser *parser,
				       H264NalUnit *nalu, H264PPS *pps);
H264ParserResult h264_parser_parse_slice_hdr(H264NalParser *parser,
					     H264NalUnit *nalu,
					     H264SliceHdr *slice,
					     bool parse_pred_weight,
					     bool parse_ref_marking);
H264ParserResult h264_parser_parse_nal(H264NalParser *parser,
				       H264NalUnit *nalu);
H264ParserResult h264_parser_update_sps(H264NalParser *parser,
					H264SPS *sps);
H264ParserResult h264_parser_update_pps(H264NalParser *parser,
					H264PPS *pps);
void h264_quant_matrix_4x4_get_raster_from_zigzag(uint8_t out[16],
						  const uint8_t quant[16]);
void h264_quant_matrix_8x8_get_raster_from_zigzag(uint8_t out[64],
						  const uint8_t quant[64]);

#endif
