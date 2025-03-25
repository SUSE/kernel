// SPDX-License-Identifier: GPL-2.0+

#include <linux/crc32.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_fixed.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_vblank.h>
#include <linux/minmax.h>

#include "vkms_drv.h"

static u16 pre_mul_blend_channel(u16 src, u16 dst, u16 alpha)
{
	u32 new_color;

	new_color = (src * 0xffff + dst * (0xffff - alpha));

	return DIV_ROUND_CLOSEST(new_color, 0xffff);
}

/**
 * pre_mul_alpha_blend - alpha blending equation
 * @stage_buffer: The line with the pixels from src_plane
 * @output_buffer: A line buffer that receives all the blends output
 * @x_start: The start offset
 * @pixel_count: The number of pixels to blend
 *
 * The pixels [@x_start;@x_start+@pixel_count) in stage_buffer are blended at
 * [@x_start;@x_start+@pixel_count) in output_buffer.
 *
 * The current DRM assumption is that pixel color values have been already
 * pre-multiplied with the alpha channel values. See more
 * drm_plane_create_blend_mode_property(). Also, this formula assumes a
 * completely opaque background.
 */
static void pre_mul_alpha_blend(const struct line_buffer *stage_buffer,
				struct line_buffer *output_buffer, int x_start, int pixel_count)
{
	struct pixel_argb_u16 *out = &output_buffer->pixels[x_start];
	const struct pixel_argb_u16 *in = &stage_buffer->pixels[x_start];

	for (int i = 0; i < pixel_count; i++) {
		out[i].a = (u16)0xffff;
		out[i].r = pre_mul_blend_channel(in[i].r, out[i].r, in[i].a);
		out[i].g = pre_mul_blend_channel(in[i].g, out[i].g, in[i].a);
		out[i].b = pre_mul_blend_channel(in[i].b, out[i].b, in[i].a);
	}
}


static void fill_background(const struct pixel_argb_u16 *background_color,
			    struct line_buffer *output_buffer)
{
	for (size_t i = 0; i < output_buffer->n_pixels; i++)
		output_buffer->pixels[i] = *background_color;
}

// lerp(a, b, t) = a + (b - a) * t
static u16 lerp_u16(u16 a, u16 b, s64 t)
{
	s64 a_fp = drm_int2fixp(a);
	s64 b_fp = drm_int2fixp(b);

	s64 delta = drm_fixp_mul(b_fp - a_fp, t);

	return drm_fixp2int_round(a_fp + delta);
}

static s64 get_lut_index(const struct vkms_color_lut *lut, u16 channel_value)
{
	s64 color_channel_fp = drm_int2fixp(channel_value);

	return drm_fixp_mul(color_channel_fp, lut->channel_value2index_ratio);
}

/*
 * This enum is related to the positions of the variables inside
 * `struct drm_color_lut`, so the order of both needs to be the same.
 */
enum lut_channel {
	LUT_RED = 0,
	LUT_GREEN,
	LUT_BLUE,
	LUT_RESERVED
};

static u16 apply_lut_to_channel_value(const struct vkms_color_lut *lut, u16 channel_value,
				      enum lut_channel channel)
{
	s64 lut_index = get_lut_index(lut, channel_value);
	u16 *floor_lut_value, *ceil_lut_value;
	u16 floor_channel_value, ceil_channel_value;

	/*
	 * This checks if `struct drm_color_lut` has any gap added by the compiler
	 * between the struct fields.
	 */
	static_assert(sizeof(struct drm_color_lut) == sizeof(__u16) * 4);

	floor_lut_value = (__u16 *)&lut->base[drm_fixp2int(lut_index)];
	if (drm_fixp2int(lut_index) == (lut->lut_length - 1))
		/* We're at the end of the LUT array, use same value for ceil and floor */
		ceil_lut_value = floor_lut_value;
	else
		ceil_lut_value = (__u16 *)&lut->base[drm_fixp2int_ceil(lut_index)];

	floor_channel_value = floor_lut_value[channel];
	ceil_channel_value = ceil_lut_value[channel];

	return lerp_u16(floor_channel_value, ceil_channel_value,
			lut_index & DRM_FIXED_DECIMAL_MASK);
}

static void apply_lut(const struct vkms_crtc_state *crtc_state, struct line_buffer *output_buffer)
{
	if (!crtc_state->gamma_lut.base)
		return;

	if (!crtc_state->gamma_lut.lut_length)
		return;

	for (size_t x = 0; x < output_buffer->n_pixels; x++) {
		struct pixel_argb_u16 *pixel = &output_buffer->pixels[x];

		pixel->r = apply_lut_to_channel_value(&crtc_state->gamma_lut, pixel->r, LUT_RED);
		pixel->g = apply_lut_to_channel_value(&crtc_state->gamma_lut, pixel->g, LUT_GREEN);
		pixel->b = apply_lut_to_channel_value(&crtc_state->gamma_lut, pixel->b, LUT_BLUE);
	}
}

/**
 * direction_for_rotation() - Get the correct reading direction for a given rotation
 *
 * @rotation: Rotation to analyze. It correspond the field @frame_info.rotation.
 *
 * This function will use the @rotation setting of a source plane to compute the reading
 * direction in this plane which correspond to a "left to right writing" in the CRTC.
 * For example, if the buffer is reflected on X axis, the pixel must be read from right to left
 * to be written from left to right on the CRTC.
 */
static enum pixel_read_direction direction_for_rotation(unsigned int rotation)
{
	struct drm_rect tmp_a, tmp_b;
	int x, y;

	/*
	 * Points A and B are depicted as zero-size rectangles on the CRTC.
	 * The CRTC writing direction is from A to B. The plane reading direction
	 * is discovered by inverse-transforming A and B.
	 * The reading direction is computed by rotating the vector AB (top-left to top-right) in a
	 * 1x1 square.
	 */

	tmp_a = DRM_RECT_INIT(0, 0, 0, 0);
	tmp_b = DRM_RECT_INIT(1, 0, 0, 0);
	drm_rect_rotate_inv(&tmp_a, 1, 1, rotation);
	drm_rect_rotate_inv(&tmp_b, 1, 1, rotation);

	x = tmp_b.x1 - tmp_a.x1;
	y = tmp_b.y1 - tmp_a.y1;

	if (x == 1 && y == 0)
		return READ_LEFT_TO_RIGHT;
	else if (x == -1 && y == 0)
		return READ_RIGHT_TO_LEFT;
	else if (y == 1 && x == 0)
		return READ_TOP_TO_BOTTOM;
	else if (y == -1 && x == 0)
		return READ_BOTTOM_TO_TOP;

	WARN_ONCE(true, "The inverse of the rotation gives an incorrect direction.");
	return READ_LEFT_TO_RIGHT;
}

/**
 * clamp_line_coordinates() - Compute and clamp the coordinate to read and write during the blend
 * process.
 *
 * @direction: direction of the reading
 * @current_plane: current plane blended
 * @src_line: source line of the reading. Only the top-left coordinate is used. This rectangle
 * must be rotated and have a shape of 1*pixel_count if @direction is vertical and a shape of
 * pixel_count*1 if @direction is horizontal.
 * @src_x_start: x start coordinate for the line reading
 * @src_y_start: y start coordinate for the line reading
 * @dst_x_start: x coordinate to blend the read line
 * @pixel_count: number of pixels to blend
 *
 * This function is mainly a safety net to avoid reading outside the source buffer. As the
 * userspace should never ask to read outside the source plane, all the cases covered here should
 * be dead code.
 */
static void clamp_line_coordinates(enum pixel_read_direction direction,
				   const struct vkms_plane_state *current_plane,
				   const struct drm_rect *src_line, int *src_x_start,
				   int *src_y_start, int *dst_x_start, int *pixel_count)
{
	/* By default the start points are correct */
	*src_x_start = src_line->x1;
	*src_y_start = src_line->y1;
	*dst_x_start = current_plane->frame_info->dst.x1;

	/* Get the correct number of pixel to blend, it depends of the direction */
	switch (direction) {
	case READ_LEFT_TO_RIGHT:
	case READ_RIGHT_TO_LEFT:
		*pixel_count = drm_rect_width(src_line);
		break;
	case READ_BOTTOM_TO_TOP:
	case READ_TOP_TO_BOTTOM:
		*pixel_count = drm_rect_height(src_line);
		break;
	}

	/*
	 * Clamp the coordinates to avoid reading outside the buffer
	 *
	 * This is mainly a security check to avoid reading outside the buffer, the userspace
	 * should never request to read outside the source buffer.
	 */
	switch (direction) {
	case READ_LEFT_TO_RIGHT:
	case READ_RIGHT_TO_LEFT:
		if (*src_x_start < 0) {
			*pixel_count += *src_x_start;
			*dst_x_start -= *src_x_start;
			*src_x_start = 0;
		}
		if (*src_x_start + *pixel_count > current_plane->frame_info->fb->width)
			*pixel_count = max(0, (int)current_plane->frame_info->fb->width -
				*src_x_start);
		break;
	case READ_BOTTOM_TO_TOP:
	case READ_TOP_TO_BOTTOM:
		if (*src_y_start < 0) {
			*pixel_count += *src_y_start;
			*dst_x_start -= *src_y_start;
			*src_y_start = 0;
		}
		if (*src_y_start + *pixel_count > current_plane->frame_info->fb->height)
			*pixel_count = max(0, (int)current_plane->frame_info->fb->height -
				*src_y_start);
		break;
	}
}

/**
 * blend_line() - Blend a line from a plane to the output buffer
 *
 * @current_plane: current plane to work on
 * @y: line to write in the output buffer
 * @crtc_x_limit: width of the output buffer
 * @stage_buffer: temporary buffer to convert the pixel line from the source buffer
 * @output_buffer: buffer to blend the read line into.
 */
static void blend_line(struct vkms_plane_state *current_plane, int y,
		       int crtc_x_limit, struct line_buffer *stage_buffer,
		       struct line_buffer *output_buffer)
{
	int src_x_start, src_y_start, dst_x_start, pixel_count;
	struct drm_rect dst_line, tmp_src, src_line;

	/* Avoid rendering useless lines */
	if (y < current_plane->frame_info->dst.y1 ||
	    y >= current_plane->frame_info->dst.y2)
		return;

	/*
	 * dst_line is the line to copy. The initial coordinates are inside the
	 * destination framebuffer, and then drm_rect_* helpers are used to
	 * compute the correct position into the source framebuffer.
	 */
	dst_line = DRM_RECT_INIT(current_plane->frame_info->dst.x1, y,
				 drm_rect_width(&current_plane->frame_info->dst),
				 1);

	drm_rect_fp_to_int(&tmp_src, &current_plane->frame_info->src);

	/*
	 * [1]: Clamping src_line to the crtc_x_limit to avoid writing outside of
	 * the destination buffer
	 */
	dst_line.x1 = max_t(int, dst_line.x1, 0);
	dst_line.x2 = min_t(int, dst_line.x2, crtc_x_limit);
	/* The destination is completely outside of the crtc. */
	if (dst_line.x2 <= dst_line.x1)
		return;

	src_line = dst_line;

	/*
	 * Transform the coordinate x/y from the crtc to coordinates into
	 * coordinates for the src buffer.
	 *
	 * - Cancel the offset of the dst buffer.
	 * - Invert the rotation. This assumes that
	 *   dst = drm_rect_rotate(src, rotation) (dst and src have the
	 *   same size, but can be rotated).
	 * - Apply the offset of the source rectangle to the coordinate.
	 */
	drm_rect_translate(&src_line, -current_plane->frame_info->dst.x1,
			   -current_plane->frame_info->dst.y1);
	drm_rect_rotate_inv(&src_line, drm_rect_width(&tmp_src),
			    drm_rect_height(&tmp_src),
			    current_plane->frame_info->rotation);
	drm_rect_translate(&src_line, tmp_src.x1, tmp_src.y1);

	/* Get the correct reading direction in the source buffer. */

	enum pixel_read_direction direction =
		direction_for_rotation(current_plane->frame_info->rotation);

	/* [2]: Compute and clamp the number of pixel to read */
	clamp_line_coordinates(direction, current_plane, &src_line, &src_x_start, &src_y_start,
			       &dst_x_start, &pixel_count);

	if (pixel_count <= 0) {
		/* Nothing to read, so avoid multiple function calls */
		return;
	}

	/*
	 * Modify the starting point to take in account the rotation
	 *
	 * src_line is the top-left corner, so when reading READ_RIGHT_TO_LEFT or
	 * READ_BOTTOM_TO_TOP, it must be changed to the top-right/bottom-left
	 * corner.
	 */
	if (direction == READ_RIGHT_TO_LEFT) {
		// src_x_start is now the right point
		src_x_start += pixel_count - 1;
	} else if (direction == READ_BOTTOM_TO_TOP) {
		// src_y_start is now the bottom point
		src_y_start += pixel_count - 1;
	}

	/*
	 * Perform the conversion and the blending
	 *
	 * Here we know that the read line (x_start, y_start, pixel_count) is
	 * inside the source buffer [2] and we don't write outside the stage
	 * buffer [1].
	 */
	current_plane->pixel_read_line(current_plane, src_x_start, src_y_start, direction,
				       pixel_count, &stage_buffer->pixels[dst_x_start]);

	pre_mul_alpha_blend(stage_buffer, output_buffer,
			    dst_x_start, pixel_count);
}

/**
 * blend - blend the pixels from all planes and compute crc
 * @wb: The writeback frame buffer metadata
 * @crtc_state: The crtc state
 * @crc32: The crc output of the final frame
 * @output_buffer: A buffer of a row that will receive the result of the blend(s)
 * @stage_buffer: The line with the pixels from plane being blend to the output
 * @row_size: The size, in bytes, of a single row
 *
 * This function blends the pixels (Using the `pre_mul_alpha_blend`)
 * from all planes, calculates the crc32 of the output from the former step,
 * and, if necessary, convert and store the output to the writeback buffer.
 */
static void blend(struct vkms_writeback_job *wb,
		  struct vkms_crtc_state *crtc_state,
		  u32 *crc32, struct line_buffer *stage_buffer,
		  struct line_buffer *output_buffer, size_t row_size)
{
	struct vkms_plane_state **plane = crtc_state->active_planes;
	u32 n_active_planes = crtc_state->num_active_planes;

	const struct pixel_argb_u16 background_color = { .a = 0xffff };

	int crtc_y_limit = crtc_state->base.mode.vdisplay;
	int crtc_x_limit = crtc_state->base.mode.hdisplay;

	/*
	 * The planes are composed line-by-line to avoid heavy memory usage. It is a necessary
	 * complexity to avoid poor blending performance.
	 *
	 * The function pixel_read_line callback is used to read a line, using an efficient
	 * algorithm for a specific format, into the staging buffer.
	 */
	for (int y = 0; y < crtc_y_limit; y++) {
		fill_background(&background_color, output_buffer);

		/* The active planes are composed associatively in z-order. */
		for (size_t i = 0; i < n_active_planes; i++) {
			blend_line(plane[i], y, crtc_x_limit, stage_buffer, output_buffer);
		}

		apply_lut(crtc_state, output_buffer);

		*crc32 = crc32_le(*crc32, (void *)output_buffer->pixels, row_size);

		if (wb)
			vkms_writeback_row(wb, output_buffer, y);
	}
}

static int check_format_funcs(struct vkms_crtc_state *crtc_state,
			      struct vkms_writeback_job *active_wb)
{
	struct vkms_plane_state **planes = crtc_state->active_planes;
	u32 n_active_planes = crtc_state->num_active_planes;

	for (size_t i = 0; i < n_active_planes; i++)
		if (!planes[i]->pixel_read_line)
			return -1;

	if (active_wb && !active_wb->pixel_write)
		return -1;

	return 0;
}

static int check_iosys_map(struct vkms_crtc_state *crtc_state)
{
	struct vkms_plane_state **plane_state = crtc_state->active_planes;
	u32 n_active_planes = crtc_state->num_active_planes;

	for (size_t i = 0; i < n_active_planes; i++)
		if (iosys_map_is_null(&plane_state[i]->frame_info->map[0]))
			return -1;

	return 0;
}

static int compose_active_planes(struct vkms_writeback_job *active_wb,
				 struct vkms_crtc_state *crtc_state,
				 u32 *crc32)
{
	size_t line_width, pixel_size = sizeof(struct pixel_argb_u16);
	struct line_buffer output_buffer, stage_buffer;
	int ret = 0;

	/*
	 * This check exists so we can call `crc32_le` for the entire line
	 * instead doing it for each channel of each pixel in case
	 * `struct `pixel_argb_u16` had any gap added by the compiler
	 * between the struct fields.
	 */
	static_assert(sizeof(struct pixel_argb_u16) == 8);

	if (WARN_ON(check_iosys_map(crtc_state)))
		return -EINVAL;

	if (WARN_ON(check_format_funcs(crtc_state, active_wb)))
		return -EINVAL;

	line_width = crtc_state->base.mode.hdisplay;
	stage_buffer.n_pixels = line_width;
	output_buffer.n_pixels = line_width;

	stage_buffer.pixels = kvmalloc(line_width * pixel_size, GFP_KERNEL);
	if (!stage_buffer.pixels) {
		DRM_ERROR("Cannot allocate memory for the output line buffer");
		return -ENOMEM;
	}

	output_buffer.pixels = kvmalloc(line_width * pixel_size, GFP_KERNEL);
	if (!output_buffer.pixels) {
		DRM_ERROR("Cannot allocate memory for intermediate line buffer");
		ret = -ENOMEM;
		goto free_stage_buffer;
	}

	blend(active_wb, crtc_state, crc32, &stage_buffer,
	      &output_buffer, line_width * pixel_size);

	kvfree(output_buffer.pixels);
free_stage_buffer:
	kvfree(stage_buffer.pixels);

	return ret;
}

/**
 * vkms_composer_worker - ordered work_struct to compute CRC
 *
 * @work: work_struct
 *
 * Work handler for composing and computing CRCs. work_struct scheduled in
 * an ordered workqueue that's periodically scheduled to run by
 * vkms_vblank_simulate() and flushed at vkms_atomic_commit_tail().
 */
void vkms_composer_worker(struct work_struct *work)
{
	struct vkms_crtc_state *crtc_state = container_of(work,
							  struct vkms_crtc_state,
							  composer_work);
	struct drm_crtc *crtc = crtc_state->base.crtc;
	struct vkms_writeback_job *active_wb = crtc_state->active_writeback;
	struct vkms_output *out = drm_crtc_to_vkms_output(crtc);
	bool crc_pending, wb_pending;
	u64 frame_start, frame_end;
	u32 crc32 = 0;
	int ret;

	spin_lock_irq(&out->composer_lock);
	frame_start = crtc_state->frame_start;
	frame_end = crtc_state->frame_end;
	crc_pending = crtc_state->crc_pending;
	wb_pending = crtc_state->wb_pending;
	crtc_state->frame_start = 0;
	crtc_state->frame_end = 0;
	crtc_state->crc_pending = false;

	if (crtc->state->gamma_lut) {
		s64 max_lut_index_fp;
		s64 u16_max_fp = drm_int2fixp(0xffff);

		crtc_state->gamma_lut.base = (struct drm_color_lut *)crtc->state->gamma_lut->data;
		crtc_state->gamma_lut.lut_length =
			crtc->state->gamma_lut->length / sizeof(struct drm_color_lut);
		max_lut_index_fp = drm_int2fixp(crtc_state->gamma_lut.lut_length - 1);
		crtc_state->gamma_lut.channel_value2index_ratio = drm_fixp_div(max_lut_index_fp,
									       u16_max_fp);

	} else {
		crtc_state->gamma_lut.base = NULL;
	}

	spin_unlock_irq(&out->composer_lock);

	/*
	 * We raced with the vblank hrtimer and previous work already computed
	 * the crc, nothing to do.
	 */
	if (!crc_pending)
		return;

	if (wb_pending)
		ret = compose_active_planes(active_wb, crtc_state, &crc32);
	else
		ret = compose_active_planes(NULL, crtc_state, &crc32);

	if (ret)
		return;

	if (wb_pending) {
		drm_writeback_signal_completion(&out->wb_connector, 0);
		spin_lock_irq(&out->composer_lock);
		crtc_state->wb_pending = false;
		spin_unlock_irq(&out->composer_lock);
	}

	/*
	 * The worker can fall behind the vblank hrtimer, make sure we catch up.
	 */
	while (frame_start <= frame_end)
		drm_crtc_add_crc_entry(crtc, true, frame_start++, &crc32);
}

static const char *const pipe_crc_sources[] = { "auto" };

const char *const *vkms_get_crc_sources(struct drm_crtc *crtc,
					size_t *count)
{
	*count = ARRAY_SIZE(pipe_crc_sources);
	return pipe_crc_sources;
}

static int vkms_crc_parse_source(const char *src_name, bool *enabled)
{
	int ret = 0;

	if (!src_name) {
		*enabled = false;
	} else if (strcmp(src_name, "auto") == 0) {
		*enabled = true;
	} else {
		*enabled = false;
		ret = -EINVAL;
	}

	return ret;
}

int vkms_verify_crc_source(struct drm_crtc *crtc, const char *src_name,
			   size_t *values_cnt)
{
	bool enabled;

	if (vkms_crc_parse_source(src_name, &enabled) < 0) {
		DRM_DEBUG_DRIVER("unknown source %s\n", src_name);
		return -EINVAL;
	}

	*values_cnt = 1;

	return 0;
}

void vkms_set_composer(struct vkms_output *out, bool enabled)
{
	bool old_enabled;

	if (enabled)
		drm_crtc_vblank_get(&out->crtc);

	spin_lock_irq(&out->lock);
	old_enabled = out->composer_enabled;
	out->composer_enabled = enabled;
	spin_unlock_irq(&out->lock);

	if (old_enabled)
		drm_crtc_vblank_put(&out->crtc);
}

int vkms_set_crc_source(struct drm_crtc *crtc, const char *src_name)
{
	struct vkms_output *out = drm_crtc_to_vkms_output(crtc);
	bool enabled = false;
	int ret = 0;

	ret = vkms_crc_parse_source(src_name, &enabled);

	vkms_set_composer(out, enabled);

	return ret;
}
