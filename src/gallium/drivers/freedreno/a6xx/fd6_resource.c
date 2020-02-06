/*
 * Copyright (C) 2018 Rob Clark <robclark@freedesktop.org>
 * Copyright © 2018 Google, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "fd6_resource.h"

/* indexed by cpp, including msaa 2x and 4x: */
static const struct {
	unsigned pitchalign;
	unsigned heightalign;
} tile_alignment[] = {
	[1]  = { 128, 32 },
	[2]  = {  64, 32 },
	[3]  = {  64, 32 },
	[4]  = {  64, 16 },
	[6]  = {  64, 16 },
	[8]  = {  64, 16 },
	[12] = {  64, 16 },
	[16] = {  64, 16 },
	[24] = {  64, 16 },
	[32] = {  64, 16 },
	[48] = {  64, 16 },
	[64] = {  64, 16 },

	/* special cases for r16: */
	[0]  = { 128, 16 },
};

/* NOTE: good way to test this is:  (for example)
 *  piglit/bin/texelFetch fs sampler3D 100x100x8
 */
static uint32_t
setup_slices(struct fd_resource *rsc, uint32_t alignment, enum pipe_format format)
{
	struct pipe_resource *prsc = &rsc->base;
	struct fd_screen *screen = fd_screen(prsc->screen);
	enum util_format_layout layout = util_format_description(format)->layout;
	uint32_t pitchalign = screen->gmem_alignw;
	uint32_t level, size = 0;
	uint32_t depth = prsc->depth0;
	/* linear dimensions: */
	uint32_t lwidth = prsc->width0;
	uint32_t lheight = prsc->height0;
	/* tile_mode dimensions: */
	uint32_t twidth = util_next_power_of_two(lwidth);
	uint32_t theight = util_next_power_of_two(lheight);
	/* in layer_first layout, the level (slice) contains just one
	 * layer (since in fact the layer contains the slices)
	 */
	uint32_t layers_in_level = rsc->layer_first ? 1 : prsc->array_size;
	int ta = rsc->cpp;

	/* The z16/r16 formats seem to not play by the normal tiling rules: */
	if ((rsc->cpp == 2) && (util_format_get_nr_components(format) == 1))
		ta = 0;

	debug_assert(ta < ARRAY_SIZE(tile_alignment));
	debug_assert(tile_alignment[ta].pitchalign);

	for (level = 0; level <= prsc->last_level; level++) {
		struct fd_resource_slice *slice = fd_resource_slice(rsc, level);
		bool linear_level = fd_resource_level_linear(prsc, level);
		uint32_t width, height;

		/* tiled levels of 3D textures are rounded up to PoT dimensions: */
		if ((prsc->target == PIPE_TEXTURE_3D) && rsc->tile_mode && !linear_level) {
			width = twidth;
			height = theight;
		} else {
			width = lwidth;
			height = lheight;
		}
		uint32_t aligned_height = height;
		uint32_t blocks;

		if (rsc->tile_mode && !linear_level) {
			pitchalign = tile_alignment[ta].pitchalign;
			aligned_height = align(aligned_height,
					tile_alignment[ta].heightalign);
		} else {
			pitchalign = 64;
		}

		/* The blits used for mem<->gmem work at a granularity of
		 * 32x32, which can cause faults due to over-fetch on the
		 * last level.  The simple solution is to over-allocate a
		 * bit the last level to ensure any over-fetch is harmless.
		 * The pitch is already sufficiently aligned, but height
		 * may not be:
		 */
		if ((level == prsc->last_level) && (prsc->target != PIPE_BUFFER))
			aligned_height = align(aligned_height, 32);

		if (layout == UTIL_FORMAT_LAYOUT_ASTC)
			slice->pitch =
				util_align_npot(width, pitchalign * util_format_get_blockwidth(format));
		else
			slice->pitch = align(width, pitchalign);

		slice->offset = size;
		blocks = util_format_get_nblocks(format, slice->pitch, aligned_height);

		/* 1d array and 2d array textures must all have the same layer size
		 * for each miplevel on a6xx. 3d textures can have different layer
		 * sizes for high levels, but the hw auto-sizer is buggy (or at least
		 * different than what this code does), so as soon as the layer size
		 * range gets into range, we stop reducing it.
		 */
		if (prsc->target == PIPE_TEXTURE_3D) {
			if (level <= 1 || (rsc->slices[level - 1].size0 > 0xf000)) {
				slice->size0 = align(blocks * rsc->cpp, alignment);
			} else {
				slice->size0 = rsc->slices[level - 1].size0;
			}
		} else {
			slice->size0 = align(blocks * rsc->cpp, alignment);
		}

		size += slice->size0 * depth * layers_in_level;

#if 0
		debug_printf("%s: %ux%ux%u@%u:\t%2u: stride=%4u, size=%6u,%7u, aligned_height=%3u, blocks=%u\n",
				util_format_name(prsc->format),
				width, height, depth, rsc->cpp,
				level, slice->pitch * rsc->cpp,
				slice->size0, size, aligned_height, blocks);
#endif

		depth = u_minify(depth, 1);
		lwidth = u_minify(lwidth, 1);
		lheight = u_minify(lheight, 1);
		twidth = u_minify(twidth, 1);
		theight = u_minify(theight, 1);
	}

	return size;
}

uint32_t
fd6_setup_slices(struct fd_resource *rsc)
{
	uint32_t alignment;

	switch (rsc->base.target) {
	case PIPE_TEXTURE_3D:
		rsc->layer_first = false;
		alignment = 4096;
		break;
	default:
		rsc->layer_first = true;
		alignment = 1;
		break;
	}

	return setup_slices(rsc, alignment, rsc->base.format);
}
