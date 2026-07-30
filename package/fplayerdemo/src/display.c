// SPDX-License-Identifier: GPL-2.0
#include "fplayerdemo.h"

static uint64_t ptr_to_u64(const void *ptr)
{
	return (uint64_t)(uintptr_t)ptr;
}

int drm_ioctl(int fd, unsigned long request, void *arg,
		     const char *name)
{
	if (xioctl(fd, request, arg) < 0) {
		fprintf(stderr, "%s: %s\n", name, strerror(errno));
		return -1;
	}

	return 0;
}

int set_client_cap(int fd, uint64_t capability, uint64_t value)
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

int find_display(int fd, struct kms_ids *ids)
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

int find_plane(int fd, struct kms_ids *ids, uint32_t force_plane_id)
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

void close_gem_handle(int fd, uint32_t handle)
{
	struct drm_gem_close close = { .handle = handle };

	if (handle)
		ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close);
}

int prime_fd_to_handle(int drm_fd, int prime_fd, uint32_t *handle)
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

int get_plane_atomic_props(int fd, uint32_t plane_id,
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

int wait_atomic_event(struct lite_kms *kms, int timeout_ms)
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

void calc_kms_src_dst_rect(const struct lite_kms *kms,
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

uint32_t auto_plane_rotation(const struct drm_mode_modeinfo *mode,
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

const char *rotation_name(uint32_t rotation)
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

int commit_plane(struct lite_kms *kms, uint32_t fb_id)
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
