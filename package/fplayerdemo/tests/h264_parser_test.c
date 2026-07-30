// SPDX-License-Identifier: GPL-2.0
#include "h264_parser.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* 16x16 main-profile stream covering I/P/B slices and weighted prediction. */
static const uint8_t selftest_h264[] = {
	0x00, 0x00, 0x00, 0x01, 0x09, 0x10, 0x00, 0x00,
	0x00, 0x01, 0x67, 0x4d, 0x40, 0x0a, 0xec, 0xaf,
	0x60, 0x22, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00,
	0x00, 0x03, 0x00, 0x10, 0x1e, 0x24, 0x4b, 0x2c,
	0x00, 0x00, 0x00, 0x01, 0x68, 0xeb, 0xe3, 0xcb,
	0x20, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00,
	0x12, 0xff, 0xfe, 0xf7, 0xad, 0xdf, 0x81, 0x4d,
	0xc3, 0x2b, 0x3d, 0x00, 0x00, 0x00, 0x01, 0x09,
	0x30, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x23, 0x6c,
	0x41, 0x0f, 0xfe, 0xe0, 0x00, 0x00, 0x00, 0x01,
	0x09, 0x50, 0x00, 0x00, 0x01, 0x41, 0x9e, 0x41,
	0x78, 0x82, 0x3f, 0xa6, 0x81, 0x00, 0x00, 0x00,
	0x01, 0x09, 0x50, 0x00, 0x00, 0x01, 0x01, 0x9e,
	0x62, 0x6a, 0x41, 0x0f, 0xab, 0x80,
};

struct parse_summary {
	unsigned int sps_count;
	unsigned int pps_count;
	unsigned int slice_count;
	unsigned int i_slices;
	unsigned int p_slices;
	unsigned int b_slices;
	unsigned int weighted_slices;
	unsigned int nonref_slices;
	uint8_t profile_idc;
	uint8_t level_idc;
	uint32_t coded_width;
	uint32_t coded_height;
	int display_width;
	int display_height;
	uint32_t max_header_bits;
	uint32_t max_poc_bits;
};

static int read_file(const char *path, uint8_t **data, size_t *size)
{
	struct stat st;
	FILE *file;

	file = fopen(path, "rb");
	if (!file)
		return -1;
	if (fstat(fileno(file), &st) || st.st_size <= 0 ||
	    (uintmax_t)st.st_size > UINT32_MAX) {
		fclose(file);
		return -1;
	}
	*size = st.st_size;
	*data = malloc(*size);
	if (!*data || fread(*data, 1, *size, file) != *size) {
		free(*data);
		*data = NULL;
		fclose(file);
		return -1;
	}
	fclose(file);
	return 0;
}

static int check_avc_nalu(H264NalParser *parser, const H264NalUnit *annexb)
{
	H264NalUnit avc_nalu;
	uint8_t *avc;
	uint32_t size = annexb->size;
	H264ParserResult result;

	avc = malloc((size_t)size + 4);
	if (!avc)
		return -1;
	avc[0] = size >> 24;
	avc[1] = size >> 16;
	avc[2] = size >> 8;
	avc[3] = size;
	memcpy(avc + 4, annexb->data + annexb->offset, size);
	result = h264_parser_identify_nalu_avc(parser, avc, 0, size + 4, 4,
						 &avc_nalu);
	free(avc);
	if (result != H264_PARSER_OK || avc_nalu.size != annexb->size ||
	    avc_nalu.type != annexb->type || avc_nalu.ref_idc != annexb->ref_idc)
		return -1;
	return 0;
}

static bool all_zero(const uint8_t *data, size_t size)
{
	size_t i;

	for (i = 0; i < size; i++)
		if (data[i])
			return false;
	return true;
}

static int parse_stream(const uint8_t *data, size_t size, bool dump,
			struct parse_summary *summary, bool report_errors)
{
	H264NalParser *parser;
	H264NalUnit nalu = { 0 };
	H264ParserResult result = H264_PARSER_OK;
	H264SPS last_sps = { 0 };
	uint32_t offset = 0;
	uint32_t error_offset = 0;
	uint16_t error_type = 0;
	unsigned int sps_count = 0;
	unsigned int pps_count = 0;
	unsigned int slice_count = 0;
	int ret = 1;

	if (summary)
		memset(summary, 0, sizeof(*summary));
	parser = h264_nal_parser_new();
	if (!parser)
		return 1;

	while (offset < size) {
		result = h264_parser_identify_nalu(parser, data, offset, size, &nalu);
		if (result != H264_PARSER_OK) {
			if (result == H264_PARSER_NO_NAL &&
			    all_zero(data + offset, size - offset)) {
				offset = size;
				break;
			}
			error_offset = offset;
			error_type = 0;
			goto parse_error;
		}
		if (check_avc_nalu(parser, &nalu)) {
			result = H264_PARSER_ERROR;
			error_offset = nalu.offset;
			error_type = nalu.type;
			goto parse_error;
		}
		offset = nalu.offset + nalu.size;
		if (nalu.type == H264_NAL_SPS || nalu.type == H264_NAL_SUBSET_SPS) {
			result = h264_parser_parse_sps(parser, &nalu, &last_sps);
			if (result == H264_PARSER_OK)
				result = h264_parser_update_sps(parser, &last_sps);
			if (result != H264_PARSER_OK) {
				error_offset = nalu.offset;
				error_type = nalu.type;
				goto parse_error;
			}
			sps_count++;
		} else if (nalu.type == H264_NAL_PPS) {
			H264PPS pps;

			result = h264_parser_parse_pps(parser, &nalu, &pps);
			if (result == H264_PARSER_OK)
				result = h264_parser_update_pps(parser, &pps);
			if (result != H264_PARSER_OK) {
				error_offset = nalu.offset;
				error_type = nalu.type;
				goto parse_error;
			}
			pps_count++;
		} else if (nalu.type == H264_NAL_SLICE ||
			   nalu.type == H264_NAL_SLICE_IDR) {
			H264SliceHdr slice;

			result = h264_parser_parse_slice_hdr(parser, &nalu, &slice,
							true, true);
			if (result != H264_PARSER_OK) {
				error_offset = nalu.offset;
				error_type = nalu.type;
				goto parse_error;
			}
			slice_count++;
			if (summary) {
				uint32_t stype = slice.type % 5;
				uint32_t header_bits =
					8 * nalu.header_bytes + slice.header_size;

				if (stype == H264_I_SLICE)
					summary->i_slices++;
				else if (stype == H264_P_SLICE)
					summary->p_slices++;
				else if (stype == H264_B_SLICE)
					summary->b_slices++;
				if ((stype == H264_P_SLICE &&
				     slice.pps->weighted_pred_flag) ||
				    (stype == H264_B_SLICE &&
				     slice.pps->weighted_bipred_idc == 1))
					summary->weighted_slices++;
				if (!nalu.ref_idc)
					summary->nonref_slices++;
				if (header_bits > summary->max_header_bits)
					summary->max_header_bits = header_bits;
				if (slice.pic_order_cnt_bit_size >
				    summary->max_poc_bits)
					summary->max_poc_bits =
						slice.pic_order_cnt_bit_size;
			}
			if (dump)
				fprintf(stdout,
					"slice=%u nal=%u ref=%u header=%u type=%u frame=%u "
					"poc=%u refs=%u/%u override=%u direct=%u "
					"cabac=%u qp=%d deblock=%u alpha=%d beta=%d "
					"mods=%u/%u mmco=%u marking_bits=%u poc_bits=%u "
					"weight=%u/%u/%d/%d/%d/%d\n",
					slice_count, nalu.type, nalu.ref_idc,
					8 * nalu.header_bytes + slice.header_size,
					slice.type, slice.frame_num,
					slice.pic_order_cnt_lsb,
					slice.num_ref_idx_l0_active_minus1,
					slice.num_ref_idx_l1_active_minus1,
					slice.num_ref_idx_active_override_flag,
					slice.direct_spatial_mv_pred_flag,
					slice.cabac_init_idc, slice.slice_qp_delta,
					slice.disable_deblocking_filter_idc,
					slice.slice_alpha_c0_offset_div2,
					slice.slice_beta_offset_div2,
					slice.n_ref_pic_list_modification_l0,
					slice.n_ref_pic_list_modification_l1,
					slice.dec_ref_pic_marking.n_ref_pic_marking,
					slice.dec_ref_pic_marking.bit_size,
					slice.pic_order_cnt_bit_size,
					slice.pred_weight_table.luma_log2_weight_denom,
					slice.pred_weight_table.chroma_log2_weight_denom,
					slice.pred_weight_table.luma_weight_l0[0],
					slice.pred_weight_table.luma_offset_l0[0],
					slice.pred_weight_table.chroma_weight_l0[0][0],
					slice.pred_weight_table.chroma_offset_l0[0][0]);
		}
	}

	if (!sps_count || !pps_count || !slice_count) {
		if (report_errors)
			fprintf(stderr,
				"missing required NAL units: sps=%u pps=%u slices=%u\n",
				sps_count, pps_count, slice_count);
		goto out_parser;
	}
	if (summary) {
		summary->sps_count = sps_count;
		summary->pps_count = pps_count;
		summary->slice_count = slice_count;
		summary->profile_idc = last_sps.profile_idc;
		summary->level_idc = last_sps.level_idc;
		summary->coded_width =
			(last_sps.pic_width_in_mbs_minus1 + 1) * 16;
		summary->coded_height =
			(last_sps.pic_height_in_map_units_minus1 + 1) * 16 *
			(2 - last_sps.frame_mbs_only_flag);
		summary->display_width = last_sps.crop_rect_width ?
			last_sps.crop_rect_width : (int)summary->coded_width;
		summary->display_height = last_sps.crop_rect_height ?
			last_sps.crop_rect_height : (int)summary->coded_height;
	}
	if (report_errors || dump)
		fprintf(stdout,
			"sps=%u pps=%u slices=%u coded=%ux%u display=%dx%d\n",
			sps_count, pps_count, slice_count,
			(last_sps.pic_width_in_mbs_minus1 + 1) * 16,
			(last_sps.pic_height_in_map_units_minus1 + 1) * 16 *
			(2 - last_sps.frame_mbs_only_flag),
			last_sps.crop_rect_width ? last_sps.crop_rect_width :
			(int)(last_sps.pic_width_in_mbs_minus1 + 1) * 16,
			last_sps.crop_rect_height ? last_sps.crop_rect_height :
			(int)(last_sps.pic_height_in_map_units_minus1 + 1) * 16 *
			(2 - last_sps.frame_mbs_only_flag));
	ret = 0;
	goto out_parser;

parse_error:
	if (report_errors)
		fprintf(stderr, "parse failed at offset=%u type=%u result=%d\n",
			error_offset, error_type, result);
out_parser:
	h264_nal_parser_free(parser);
	return ret;
}

static int run_selftest(void)
{
	uint8_t trailing_zero_stream[sizeof(selftest_h264) + 4];
	uint8_t invalid_stream[sizeof(selftest_h264) + 4];
	struct parse_summary summary;

	if (parse_stream(selftest_h264, sizeof(selftest_h264), false,
			 &summary, false)) {
		fprintf(stderr, "selftest rejected the valid stream\n");
		return 1;
	}
	if (summary.sps_count != 1 || summary.pps_count != 1 ||
	    summary.slice_count != 4 || summary.i_slices != 1 ||
	    summary.p_slices != 1 || summary.b_slices != 2 ||
	    summary.weighted_slices != 1 || summary.nonref_slices != 1 ||
	    summary.profile_idc != 77 || summary.level_idc != 10 ||
	    summary.coded_width != 16 || summary.coded_height != 16 ||
	    summary.display_width != 16 || summary.display_height != 16 ||
	    summary.max_header_bits != 48 || summary.max_poc_bits != 6) {
		fprintf(stderr, "selftest parser summary mismatch\n");
		return 1;
	}

	memcpy(trailing_zero_stream, selftest_h264, sizeof(selftest_h264));
	memset(trailing_zero_stream + sizeof(selftest_h264), 0, 4);
	if (parse_stream(trailing_zero_stream, sizeof(trailing_zero_stream),
			 false, NULL, false)) {
		fprintf(stderr, "selftest rejected trailing_zero_8bits\n");
		return 1;
	}

	memcpy(invalid_stream, selftest_h264, sizeof(selftest_h264));
	invalid_stream[sizeof(selftest_h264) + 0] = 0x00;
	invalid_stream[sizeof(selftest_h264) + 1] = 0x00;
	invalid_stream[sizeof(selftest_h264) + 2] = 0x01;
	invalid_stream[sizeof(selftest_h264) + 3] = 0x80;
	if (!parse_stream(invalid_stream, sizeof(invalid_stream), false,
			  NULL, false)) {
		fprintf(stderr, "selftest accepted a forbidden NAL header\n");
		return 1;
	}

	fprintf(stdout, "h264 parser selftest ok\n");
	return 0;
}

int main(int argc, char **argv)
{
	uint8_t *data = NULL;
	size_t size = 0;
	bool dump = false;
	int ret;

	if (argc == 2 && !strcmp(argv[1], "--selftest"))
		return run_selftest();
	if (argc == 3 && !strcmp(argv[1], "--dump")) {
		dump = true;
		argv++;
		argc--;
	}
	if (argc != 2 || read_file(argv[1], &data, &size)) {
		fprintf(stderr, "read input failed: %s\n",
			argc == 2 ? argv[1] : "missing path");
		return 2;
	}

	ret = parse_stream(data, size, dump, NULL, true);
	free(data);
	return ret;
}
