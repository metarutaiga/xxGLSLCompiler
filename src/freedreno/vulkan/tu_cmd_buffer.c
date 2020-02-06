/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "tu_private.h"

#include "registers/adreno_pm4.xml.h"
#include "registers/adreno_common.xml.h"

#include "vk_format.h"

#include "tu_cs.h"
#include "tu_blit.h"

#define OVERFLOW_FLAG_REG REG_A6XX_CP_SCRATCH_REG(0)

void
tu_bo_list_init(struct tu_bo_list *list)
{
   list->count = list->capacity = 0;
   list->bo_infos = NULL;
}

void
tu_bo_list_destroy(struct tu_bo_list *list)
{
   free(list->bo_infos);
}

void
tu_bo_list_reset(struct tu_bo_list *list)
{
   list->count = 0;
}

/**
 * \a flags consists of MSM_SUBMIT_BO_FLAGS.
 */
static uint32_t
tu_bo_list_add_info(struct tu_bo_list *list,
                    const struct drm_msm_gem_submit_bo *bo_info)
{
   assert(bo_info->handle != 0);

   for (uint32_t i = 0; i < list->count; ++i) {
      if (list->bo_infos[i].handle == bo_info->handle) {
         assert(list->bo_infos[i].presumed == bo_info->presumed);
         list->bo_infos[i].flags |= bo_info->flags;
         return i;
      }
   }

   /* grow list->bo_infos if needed */
   if (list->count == list->capacity) {
      uint32_t new_capacity = MAX2(2 * list->count, 16);
      struct drm_msm_gem_submit_bo *new_bo_infos = realloc(
         list->bo_infos, new_capacity * sizeof(struct drm_msm_gem_submit_bo));
      if (!new_bo_infos)
         return TU_BO_LIST_FAILED;
      list->bo_infos = new_bo_infos;
      list->capacity = new_capacity;
   }

   list->bo_infos[list->count] = *bo_info;
   return list->count++;
}

uint32_t
tu_bo_list_add(struct tu_bo_list *list,
               const struct tu_bo *bo,
               uint32_t flags)
{
   return tu_bo_list_add_info(list, &(struct drm_msm_gem_submit_bo) {
                                       .flags = flags,
                                       .handle = bo->gem_handle,
                                       .presumed = bo->iova,
                                    });
}

VkResult
tu_bo_list_merge(struct tu_bo_list *list, const struct tu_bo_list *other)
{
   for (uint32_t i = 0; i < other->count; i++) {
      if (tu_bo_list_add_info(list, other->bo_infos + i) == TU_BO_LIST_FAILED)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   return VK_SUCCESS;
}

static void
tu_tiling_config_update_tile_layout(struct tu_tiling_config *tiling,
                                    const struct tu_device *dev,
                                    uint32_t pixels)
{
   const uint32_t tile_align_w = dev->physical_device->tile_align_w;
   const uint32_t tile_align_h = dev->physical_device->tile_align_h;
   const uint32_t max_tile_width = 1024; /* A6xx */

   tiling->tile0.offset = (VkOffset2D) {
      .x = tiling->render_area.offset.x & ~(tile_align_w - 1),
      .y = tiling->render_area.offset.y & ~(tile_align_h - 1),
   };

   const uint32_t ra_width =
      tiling->render_area.extent.width +
      (tiling->render_area.offset.x - tiling->tile0.offset.x);
   const uint32_t ra_height =
      tiling->render_area.extent.height +
      (tiling->render_area.offset.y - tiling->tile0.offset.y);

   /* start from 1 tile */
   tiling->tile_count = (VkExtent2D) {
      .width = 1,
      .height = 1,
   };
   tiling->tile0.extent = (VkExtent2D) {
      .width = align(ra_width, tile_align_w),
      .height = align(ra_height, tile_align_h),
   };

   /* do not exceed max tile width */
   while (tiling->tile0.extent.width > max_tile_width) {
      tiling->tile_count.width++;
      tiling->tile0.extent.width =
         align(ra_width / tiling->tile_count.width, tile_align_w);
   }

   /* do not exceed gmem size */
   while (tiling->tile0.extent.width * tiling->tile0.extent.height > pixels) {
      if (tiling->tile0.extent.width > MAX2(tile_align_w, tiling->tile0.extent.height)) {
         tiling->tile_count.width++;
         tiling->tile0.extent.width =
            align(DIV_ROUND_UP(ra_width, tiling->tile_count.width), tile_align_w);
      } else {
         /* if this assert fails then layout is impossible.. */
         assert(tiling->tile0.extent.height > tile_align_h);
         tiling->tile_count.height++;
         tiling->tile0.extent.height =
            align(DIV_ROUND_UP(ra_height, tiling->tile_count.height), tile_align_h);
      }
   }
}

static void
tu_tiling_config_update_pipe_layout(struct tu_tiling_config *tiling,
                                    const struct tu_device *dev)
{
   const uint32_t max_pipe_count = 32; /* A6xx */

   /* start from 1 tile per pipe */
   tiling->pipe0 = (VkExtent2D) {
      .width = 1,
      .height = 1,
   };
   tiling->pipe_count = tiling->tile_count;

   /* do not exceed max pipe count vertically */
   while (tiling->pipe_count.height > max_pipe_count) {
      tiling->pipe0.height += 2;
      tiling->pipe_count.height =
         (tiling->tile_count.height + tiling->pipe0.height - 1) /
         tiling->pipe0.height;
   }

   /* do not exceed max pipe count */
   while (tiling->pipe_count.width * tiling->pipe_count.height >
          max_pipe_count) {
      tiling->pipe0.width += 1;
      tiling->pipe_count.width =
         (tiling->tile_count.width + tiling->pipe0.width - 1) /
         tiling->pipe0.width;
   }
}

static void
tu_tiling_config_update_pipes(struct tu_tiling_config *tiling,
                              const struct tu_device *dev)
{
   const uint32_t max_pipe_count = 32; /* A6xx */
   const uint32_t used_pipe_count =
      tiling->pipe_count.width * tiling->pipe_count.height;
   const VkExtent2D last_pipe = {
      .width = (tiling->tile_count.width - 1) % tiling->pipe0.width + 1,
      .height = (tiling->tile_count.height - 1) % tiling->pipe0.height + 1,
   };

   assert(used_pipe_count <= max_pipe_count);
   assert(max_pipe_count <= ARRAY_SIZE(tiling->pipe_config));

   for (uint32_t y = 0; y < tiling->pipe_count.height; y++) {
      for (uint32_t x = 0; x < tiling->pipe_count.width; x++) {
         const uint32_t pipe_x = tiling->pipe0.width * x;
         const uint32_t pipe_y = tiling->pipe0.height * y;
         const uint32_t pipe_w = (x == tiling->pipe_count.width - 1)
                                    ? last_pipe.width
                                    : tiling->pipe0.width;
         const uint32_t pipe_h = (y == tiling->pipe_count.height - 1)
                                    ? last_pipe.height
                                    : tiling->pipe0.height;
         const uint32_t n = tiling->pipe_count.width * y + x;

         tiling->pipe_config[n] = A6XX_VSC_PIPE_CONFIG_REG_X(pipe_x) |
                                  A6XX_VSC_PIPE_CONFIG_REG_Y(pipe_y) |
                                  A6XX_VSC_PIPE_CONFIG_REG_W(pipe_w) |
                                  A6XX_VSC_PIPE_CONFIG_REG_H(pipe_h);
         tiling->pipe_sizes[n] = CP_SET_BIN_DATA5_0_VSC_SIZE(pipe_w * pipe_h);
      }
   }

   memset(tiling->pipe_config + used_pipe_count, 0,
          sizeof(uint32_t) * (max_pipe_count - used_pipe_count));
}

static void
tu_tiling_config_get_tile(const struct tu_tiling_config *tiling,
                          const struct tu_device *dev,
                          uint32_t tx,
                          uint32_t ty,
                          struct tu_tile *tile)
{
   /* find the pipe and the slot for tile (tx, ty) */
   const uint32_t px = tx / tiling->pipe0.width;
   const uint32_t py = ty / tiling->pipe0.height;
   const uint32_t sx = tx - tiling->pipe0.width * px;
   const uint32_t sy = ty - tiling->pipe0.height * py;

   assert(tx < tiling->tile_count.width && ty < tiling->tile_count.height);
   assert(px < tiling->pipe_count.width && py < tiling->pipe_count.height);
   assert(sx < tiling->pipe0.width && sy < tiling->pipe0.height);

   /* convert to 1D indices */
   tile->pipe = tiling->pipe_count.width * py + px;
   tile->slot = tiling->pipe0.width * sy + sx;

   /* get the blit area for the tile */
   tile->begin = (VkOffset2D) {
      .x = tiling->tile0.offset.x + tiling->tile0.extent.width * tx,
      .y = tiling->tile0.offset.y + tiling->tile0.extent.height * ty,
   };
   tile->end.x =
      (tx == tiling->tile_count.width - 1)
         ? tiling->render_area.offset.x + tiling->render_area.extent.width
         : tile->begin.x + tiling->tile0.extent.width;
   tile->end.y =
      (ty == tiling->tile_count.height - 1)
         ? tiling->render_area.offset.y + tiling->render_area.extent.height
         : tile->begin.y + tiling->tile0.extent.height;
}

enum a3xx_msaa_samples
tu_msaa_samples(uint32_t samples)
{
   switch (samples) {
   case 1:
      return MSAA_ONE;
   case 2:
      return MSAA_TWO;
   case 4:
      return MSAA_FOUR;
   case 8:
      return MSAA_EIGHT;
   default:
      assert(!"invalid sample count");
      return MSAA_ONE;
   }
}

static enum a4xx_index_size
tu6_index_size(VkIndexType type)
{
   switch (type) {
   case VK_INDEX_TYPE_UINT16:
      return INDEX4_SIZE_16_BIT;
   case VK_INDEX_TYPE_UINT32:
      return INDEX4_SIZE_32_BIT;
   default:
      unreachable("invalid VkIndexType");
      return INDEX4_SIZE_8_BIT;
   }
}

static void
tu6_emit_marker(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   tu_cs_emit_write_reg(cs, cmd->marker_reg, ++cmd->marker_seqno);
}

unsigned
tu6_emit_event_write(struct tu_cmd_buffer *cmd,
                     struct tu_cs *cs,
                     enum vgt_event_type event,
                     bool need_seqno)
{
   unsigned seqno = 0;

   tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, need_seqno ? 4 : 1);
   tu_cs_emit(cs, CP_EVENT_WRITE_0_EVENT(event));
   if (need_seqno) {
      tu_cs_emit_qw(cs, cmd->scratch_bo.iova);
      seqno = ++cmd->scratch_seqno;
      tu_cs_emit(cs, seqno);
   }

   return seqno;
}

static void
tu6_emit_cache_flush(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   tu6_emit_event_write(cmd, cs, 0x31, false);
}

static void
tu6_emit_lrz_flush(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   tu6_emit_event_write(cmd, cs, LRZ_FLUSH, false);
}

static void
tu6_emit_wfi(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   if (cmd->wait_for_idle) {
      tu_cs_emit_wfi(cs);
      cmd->wait_for_idle = false;
   }
}

#define tu_image_view_ubwc_pitches(iview)                                \
   .pitch = tu_image_ubwc_pitch(iview->image, iview->base_mip),          \
   .array_pitch = tu_image_ubwc_size(iview->image, iview->base_mip) >> 2

static void
tu6_emit_zs(struct tu_cmd_buffer *cmd,
            const struct tu_subpass *subpass,
            struct tu_cs *cs)
{
   const struct tu_framebuffer *fb = cmd->state.framebuffer;

   const uint32_t a = subpass->depth_stencil_attachment.attachment;
   if (a == VK_ATTACHMENT_UNUSED) {
      tu_cs_emit_regs(cs,
                      A6XX_RB_DEPTH_BUFFER_INFO(.depth_format = DEPTH6_NONE),
                      A6XX_RB_DEPTH_BUFFER_PITCH(0),
                      A6XX_RB_DEPTH_BUFFER_ARRAY_PITCH(0),
                      A6XX_RB_DEPTH_BUFFER_BASE(0),
                      A6XX_RB_DEPTH_BUFFER_BASE_GMEM(0));

      tu_cs_emit_regs(cs,
                      A6XX_GRAS_SU_DEPTH_BUFFER_INFO(.depth_format = DEPTH6_NONE));

      tu_cs_emit_regs(cs,
                      A6XX_GRAS_LRZ_BUFFER_BASE(0),
                      A6XX_GRAS_LRZ_BUFFER_PITCH(0),
                      A6XX_GRAS_LRZ_FAST_CLEAR_BUFFER_BASE(0));

      tu_cs_emit_regs(cs, A6XX_RB_STENCIL_INFO(0));

      return;
   }

   const struct tu_image_view *iview = fb->attachments[a].attachment;
   enum a6xx_depth_format fmt = tu6_pipe2depth(iview->vk_format);

   tu_cs_emit_regs(cs,
                   A6XX_RB_DEPTH_BUFFER_INFO(.depth_format = fmt),
                   A6XX_RB_DEPTH_BUFFER_PITCH(tu_image_stride(iview->image, iview->base_mip)),
                   A6XX_RB_DEPTH_BUFFER_ARRAY_PITCH(iview->image->layout.layer_size),
                   A6XX_RB_DEPTH_BUFFER_BASE(tu_image_view_base_ref(iview)),
                   A6XX_RB_DEPTH_BUFFER_BASE_GMEM(cmd->state.pass->attachments[a].gmem_offset));

   tu_cs_emit_regs(cs,
                   A6XX_GRAS_SU_DEPTH_BUFFER_INFO(.depth_format = fmt));

   tu_cs_emit_regs(cs,
                   A6XX_RB_DEPTH_FLAG_BUFFER_BASE(tu_image_view_ubwc_base_ref(iview)),
                   A6XX_RB_DEPTH_FLAG_BUFFER_PITCH(tu_image_view_ubwc_pitches(iview)));

   tu_cs_emit_regs(cs,
                   A6XX_GRAS_LRZ_BUFFER_BASE(0),
                   A6XX_GRAS_LRZ_BUFFER_PITCH(0),
                   A6XX_GRAS_LRZ_FAST_CLEAR_BUFFER_BASE(0));

   tu_cs_emit_regs(cs,
                   A6XX_RB_STENCIL_INFO(0));

   /* enable zs? */
}

static void
tu6_emit_mrt(struct tu_cmd_buffer *cmd,
             const struct tu_subpass *subpass,
             struct tu_cs *cs)
{
   const struct tu_framebuffer *fb = cmd->state.framebuffer;
   unsigned char mrt_comp[MAX_RTS] = { 0 };
   unsigned srgb_cntl = 0;

   for (uint32_t i = 0; i < subpass->color_count; ++i) {
      uint32_t a = subpass->color_attachments[i].attachment;
      if (a == VK_ATTACHMENT_UNUSED)
         continue;

      const struct tu_image_view *iview = fb->attachments[a].attachment;
      const enum a6xx_tile_mode tile_mode =
         tu6_get_image_tile_mode(iview->image, iview->base_mip);

      mrt_comp[i] = 0xf;

      if (vk_format_is_srgb(iview->vk_format))
         srgb_cntl |= (1 << i);

      const struct tu_native_format *format =
         tu6_get_native_format(iview->vk_format);
      assert(format && format->rb >= 0);

      tu_cs_emit_regs(cs,
                      A6XX_RB_MRT_BUF_INFO(i,
                                           .color_tile_mode = tile_mode,
                                           .color_format = format->rb,
                                           .color_swap = format->swap),
                      A6XX_RB_MRT_PITCH(i, tu_image_stride(iview->image, iview->base_mip)),
                      A6XX_RB_MRT_ARRAY_PITCH(i, iview->image->layout.layer_size),
                      A6XX_RB_MRT_BASE(i, tu_image_view_base_ref(iview)),
                      A6XX_RB_MRT_BASE_GMEM(i, cmd->state.pass->attachments[a].gmem_offset));

      tu_cs_emit_regs(cs,
                      A6XX_SP_FS_MRT_REG(i,
                                         .color_format = format->rb,
                                         .color_sint = vk_format_is_sint(iview->vk_format),
                                         .color_uint = vk_format_is_uint(iview->vk_format)));

      tu_cs_emit_regs(cs,
                      A6XX_RB_MRT_FLAG_BUFFER_ADDR(i, tu_image_view_ubwc_base_ref(iview)),
                      A6XX_RB_MRT_FLAG_BUFFER_PITCH(i, tu_image_view_ubwc_pitches(iview)));
   }

   tu_cs_emit_regs(cs,
                   A6XX_RB_SRGB_CNTL(srgb_cntl));

   tu_cs_emit_regs(cs,
                   A6XX_SP_SRGB_CNTL(srgb_cntl));

   tu_cs_emit_regs(cs,
                   A6XX_RB_RENDER_COMPONENTS(
                      .rt0 = mrt_comp[0],
                      .rt1 = mrt_comp[1],
                      .rt2 = mrt_comp[2],
                      .rt3 = mrt_comp[3],
                      .rt4 = mrt_comp[4],
                      .rt5 = mrt_comp[5],
                      .rt6 = mrt_comp[6],
                      .rt7 = mrt_comp[7]));

   tu_cs_emit_regs(cs,
                   A6XX_SP_FS_RENDER_COMPONENTS(
                      .rt0 = mrt_comp[0],
                      .rt1 = mrt_comp[1],
                      .rt2 = mrt_comp[2],
                      .rt3 = mrt_comp[3],
                      .rt4 = mrt_comp[4],
                      .rt5 = mrt_comp[5],
                      .rt6 = mrt_comp[6],
                      .rt7 = mrt_comp[7]));
}

static void
tu6_emit_msaa(struct tu_cmd_buffer *cmd,
              const struct tu_subpass *subpass,
              struct tu_cs *cs)
{
   const enum a3xx_msaa_samples samples = tu_msaa_samples(subpass->samples);
   bool msaa_disable = samples == MSAA_ONE;

   tu_cs_emit_regs(cs,
                   A6XX_SP_TP_RAS_MSAA_CNTL(samples),
                   A6XX_SP_TP_DEST_MSAA_CNTL(.samples = samples,
                                             .msaa_disable = msaa_disable));

   tu_cs_emit_regs(cs,
                   A6XX_GRAS_RAS_MSAA_CNTL(samples),
                   A6XX_GRAS_DEST_MSAA_CNTL(.samples = samples,
                                            .msaa_disable = msaa_disable));

   tu_cs_emit_regs(cs,
                   A6XX_RB_RAS_MSAA_CNTL(samples),
                   A6XX_RB_DEST_MSAA_CNTL(.samples = samples,
                                          .msaa_disable = msaa_disable));

   tu_cs_emit_regs(cs,
                   A6XX_RB_MSAA_CNTL(samples));
}

static void
tu6_emit_bin_size(struct tu_cmd_buffer *cmd, struct tu_cs *cs, uint32_t flags)
{
   const struct tu_tiling_config *tiling = &cmd->state.tiling_config;
   const uint32_t bin_w = tiling->tile0.extent.width;
   const uint32_t bin_h = tiling->tile0.extent.height;

   tu_cs_emit_regs(cs,
                   A6XX_GRAS_BIN_CONTROL(.binw = bin_w,
                                         .binh = bin_h,
                                         .dword = flags));

   tu_cs_emit_regs(cs,
                   A6XX_RB_BIN_CONTROL(.binw = bin_w,
                                       .binh = bin_h,
                                       .dword = flags));

   /* no flag for RB_BIN_CONTROL2... */
   tu_cs_emit_regs(cs,
                   A6XX_RB_BIN_CONTROL2(.binw = bin_w,
                                        .binh = bin_h));
}

static void
tu6_emit_render_cntl(struct tu_cmd_buffer *cmd,
                     struct tu_cs *cs,
                     bool binning)
{
   uint32_t cntl = 0;
   cntl |= A6XX_RB_RENDER_CNTL_UNK4;
   if (binning)
      cntl |= A6XX_RB_RENDER_CNTL_BINNING;

   tu_cs_emit_pkt7(cs, CP_REG_WRITE, 3);
   tu_cs_emit(cs, 0x2);
   tu_cs_emit(cs, REG_A6XX_RB_RENDER_CNTL);
   tu_cs_emit(cs, cntl);
}

static void
tu6_emit_blit_scissor(struct tu_cmd_buffer *cmd, struct tu_cs *cs, bool align)
{
   const VkRect2D *render_area = &cmd->state.tiling_config.render_area;
   uint32_t x1 = render_area->offset.x;
   uint32_t y1 = render_area->offset.y;
   uint32_t x2 = x1 + render_area->extent.width - 1;
   uint32_t y2 = y1 + render_area->extent.height - 1;

   /* TODO: alignment requirement seems to be less than tile_align_w/h */
   if (align) {
      x1 = x1 & ~cmd->device->physical_device->tile_align_w;
      y1 = y1 & ~cmd->device->physical_device->tile_align_h;
      x2 = ALIGN_POT(x2 + 1, cmd->device->physical_device->tile_align_w) - 1;
      y2 = ALIGN_POT(y2 + 1, cmd->device->physical_device->tile_align_h) - 1;
   }

   tu_cs_emit_regs(cs,
                   A6XX_RB_BLIT_SCISSOR_TL(.x = x1, .y = y1),
                   A6XX_RB_BLIT_SCISSOR_BR(.x = x2, .y = y2));
}

static void
tu6_emit_blit_info(struct tu_cmd_buffer *cmd,
                   struct tu_cs *cs,
                   const struct tu_image_view *iview,
                   uint32_t gmem_offset,
                   bool resolve)
{
   tu_cs_emit_regs(cs,
                   A6XX_RB_BLIT_INFO(.unk0 = !resolve, .gmem = !resolve));

   const struct tu_native_format *format =
      tu6_get_native_format(iview->vk_format);
   assert(format && format->rb >= 0);

   enum a6xx_tile_mode tile_mode =
      tu6_get_image_tile_mode(iview->image, iview->base_mip);
   tu_cs_emit_regs(cs,
                   A6XX_RB_BLIT_DST_INFO(
                      .tile_mode = tile_mode,
                      .samples = tu_msaa_samples(iview->image->samples),
                      .color_format = format->rb,
                      .color_swap = format->swap,
                      .flags = iview->image->layout.ubwc_size != 0),
                   A6XX_RB_BLIT_DST(tu_image_view_base_ref(iview)),
                   A6XX_RB_BLIT_DST_PITCH(tu_image_stride(iview->image, iview->base_mip)),
                   A6XX_RB_BLIT_DST_ARRAY_PITCH(iview->image->layout.layer_size));

   if (iview->image->layout.ubwc_size) {
      tu_cs_emit_regs(cs,
                      A6XX_RB_BLIT_FLAG_DST(tu_image_view_ubwc_base_ref(iview)),
                      A6XX_RB_BLIT_FLAG_DST_PITCH(tu_image_view_ubwc_pitches(iview)));
   }

   tu_cs_emit_regs(cs,
                   A6XX_RB_BLIT_BASE_GMEM(gmem_offset));
}

static void
tu6_emit_blit(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   tu6_emit_marker(cmd, cs);
   tu6_emit_event_write(cmd, cs, BLIT, false);
   tu6_emit_marker(cmd, cs);
}

static void
tu6_emit_window_scissor(struct tu_cmd_buffer *cmd,
                        struct tu_cs *cs,
                        uint32_t x1,
                        uint32_t y1,
                        uint32_t x2,
                        uint32_t y2)
{
   tu_cs_emit_regs(cs,
                   A6XX_GRAS_SC_WINDOW_SCISSOR_TL(.x = x1, .y = y1),
                   A6XX_GRAS_SC_WINDOW_SCISSOR_BR(.x = x2, .y = y2));

   tu_cs_emit_regs(cs,
                   A6XX_GRAS_RESOLVE_CNTL_1(.x = x1, .y = y1),
                   A6XX_GRAS_RESOLVE_CNTL_2(.x = x2, .y = y2));
}

static void
tu6_emit_window_offset(struct tu_cmd_buffer *cmd,
                       struct tu_cs *cs,
                       uint32_t x1,
                       uint32_t y1)
{
   tu_cs_emit_regs(cs,
                   A6XX_RB_WINDOW_OFFSET(.x = x1, .y = y1));

   tu_cs_emit_regs(cs,
                   A6XX_RB_WINDOW_OFFSET2(.x = x1, .y = y1));

   tu_cs_emit_regs(cs,
                   A6XX_SP_WINDOW_OFFSET(.x = x1, .y = y1));

   tu_cs_emit_regs(cs,
                   A6XX_SP_TP_WINDOW_OFFSET(.x = x1, .y = y1));
}

static bool
use_hw_binning(struct tu_cmd_buffer *cmd)
{
   const struct tu_tiling_config *tiling = &cmd->state.tiling_config;

   if (unlikely(cmd->device->physical_device->instance->debug_flags & TU_DEBUG_NOBIN))
      return false;

   return (tiling->tile_count.width * tiling->tile_count.height) > 2;
}

static void
tu6_emit_tile_select(struct tu_cmd_buffer *cmd,
                     struct tu_cs *cs,
                     const struct tu_tile *tile)
{
   tu_cs_emit_pkt7(cs, CP_SET_MARKER, 1);
   tu_cs_emit(cs, A6XX_CP_SET_MARKER_0_MODE(0x7));

   tu6_emit_marker(cmd, cs);
   tu_cs_emit_pkt7(cs, CP_SET_MARKER, 1);
   tu_cs_emit(cs, A6XX_CP_SET_MARKER_0_MODE(RM6_GMEM) | 0x10);
   tu6_emit_marker(cmd, cs);

   const uint32_t x1 = tile->begin.x;
   const uint32_t y1 = tile->begin.y;
   const uint32_t x2 = tile->end.x - 1;
   const uint32_t y2 = tile->end.y - 1;
   tu6_emit_window_scissor(cmd, cs, x1, y1, x2, y2);
   tu6_emit_window_offset(cmd, cs, x1, y1);

   tu_cs_emit_regs(cs,
                   A6XX_VPC_SO_OVERRIDE(.so_disable = true));

   if (use_hw_binning(cmd)) {
      tu_cs_emit_pkt7(cs, CP_WAIT_FOR_ME, 0);

      tu_cs_emit_pkt7(cs, CP_SET_MODE, 1);
      tu_cs_emit(cs, 0x0);

      tu_cs_emit_pkt7(cs, CP_REG_TEST, 1);
      tu_cs_emit(cs, A6XX_CP_REG_TEST_0_REG(OVERFLOW_FLAG_REG) |
                     A6XX_CP_REG_TEST_0_BIT(0) |
                     A6XX_CP_REG_TEST_0_WAIT_FOR_ME);

      tu_cs_emit_pkt7(cs, CP_COND_REG_EXEC, 2);
      tu_cs_emit(cs, CP_COND_REG_EXEC_0_MODE(PRED_TEST));
      tu_cs_emit(cs, CP_COND_REG_EXEC_1_DWORDS(11));

      /* if (no overflow) */ {
         tu_cs_emit_pkt7(cs, CP_SET_BIN_DATA5, 7);
         tu_cs_emit(cs, cmd->state.tiling_config.pipe_sizes[tile->pipe] |
                        CP_SET_BIN_DATA5_0_VSC_N(tile->slot));
         tu_cs_emit_qw(cs, cmd->vsc_data.iova + tile->pipe * cmd->vsc_data_pitch);
         tu_cs_emit_qw(cs, cmd->vsc_data.iova + (tile->pipe * 4) + (32 * cmd->vsc_data_pitch));
         tu_cs_emit_qw(cs, cmd->vsc_data2.iova + (tile->pipe * cmd->vsc_data2_pitch));

         tu_cs_emit_pkt7(cs, CP_SET_VISIBILITY_OVERRIDE, 1);
         tu_cs_emit(cs, 0x0);

         /* use a NOP packet to skip over the 'else' side: */
         tu_cs_emit_pkt7(cs, CP_NOP, 2);
      } /* else */ {
         tu_cs_emit_pkt7(cs, CP_SET_VISIBILITY_OVERRIDE, 1);
         tu_cs_emit(cs, 0x1);
      }

      tu_cs_emit_pkt7(cs, CP_SET_MODE, 1);
      tu_cs_emit(cs, 0x0);

      tu_cs_emit_regs(cs,
                      A6XX_RB_UNKNOWN_8804(0));

      tu_cs_emit_regs(cs,
                      A6XX_SP_TP_UNKNOWN_B304(0));

      tu_cs_emit_regs(cs,
                      A6XX_GRAS_UNKNOWN_80A4(0));
   } else {
      tu_cs_emit_pkt7(cs, CP_SET_VISIBILITY_OVERRIDE, 1);
      tu_cs_emit(cs, 0x1);

      tu_cs_emit_pkt7(cs, CP_SET_MODE, 1);
      tu_cs_emit(cs, 0x0);
   }
}

static void
tu6_emit_load_attachment(struct tu_cmd_buffer *cmd, struct tu_cs *cs, uint32_t a)
{
   const struct tu_tiling_config *tiling = &cmd->state.tiling_config;
   const struct tu_framebuffer *fb = cmd->state.framebuffer;
   const struct tu_image_view *iview = fb->attachments[a].attachment;
   const struct tu_render_pass_attachment *attachment =
      &cmd->state.pass->attachments[a];

   if (attachment->gmem_offset < 0)
      return;

   const uint32_t x1 = tiling->render_area.offset.x;
   const uint32_t y1 = tiling->render_area.offset.y;
   const uint32_t x2 = x1 + tiling->render_area.extent.width;
   const uint32_t y2 = y1 + tiling->render_area.extent.height;
   const uint32_t tile_x2 =
      tiling->tile0.offset.x + tiling->tile0.extent.width * tiling->tile_count.width;
   const uint32_t tile_y2 =
      tiling->tile0.offset.y + tiling->tile0.extent.height * tiling->tile_count.height;
   bool need_load =
      x1 != tiling->tile0.offset.x || x2 != MIN2(fb->width, tile_x2) ||
      y1 != tiling->tile0.offset.y || y2 != MIN2(fb->height, tile_y2);

   if (need_load)
      tu_finishme("improve handling of unaligned render area");

   if (attachment->load_op == VK_ATTACHMENT_LOAD_OP_LOAD)
      need_load = true;

   if (vk_format_has_stencil(iview->vk_format) &&
       attachment->stencil_load_op == VK_ATTACHMENT_LOAD_OP_LOAD)
      need_load = true;

   if (need_load) {
      tu6_emit_blit_info(cmd, cs, iview, attachment->gmem_offset, false);
      tu6_emit_blit(cmd, cs);
   }
}

static void
tu6_emit_clear_attachment(struct tu_cmd_buffer *cmd, struct tu_cs *cs,
                          uint32_t a,
                          const VkRenderPassBeginInfo *info)
{
   const struct tu_framebuffer *fb = cmd->state.framebuffer;
   const struct tu_image_view *iview = fb->attachments[a].attachment;
   const struct tu_render_pass_attachment *attachment =
      &cmd->state.pass->attachments[a];
   unsigned clear_mask = 0;

   /* note: this means it isn't used by any subpass and shouldn't be cleared anyway */
   if (attachment->gmem_offset < 0)
      return;

   if (attachment->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
      clear_mask = 0xf;

   if (vk_format_has_stencil(iview->vk_format)) {
      clear_mask &= 0x1;
      if (attachment->stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
         clear_mask |= 0x2;
   }
   if (!clear_mask)
      return;

   const struct tu_native_format *format =
      tu6_get_native_format(iview->vk_format);
   assert(format && format->rb >= 0);

   tu_cs_emit_regs(cs,
                   A6XX_RB_BLIT_DST_INFO(.color_format = format->rb));

   tu_cs_emit_regs(cs,
                   A6XX_RB_BLIT_INFO(.gmem = true,
                                     .clear_mask = clear_mask));

   tu_cs_emit_regs(cs,
                   A6XX_RB_BLIT_BASE_GMEM(attachment->gmem_offset));

   tu_cs_emit_regs(cs,
                   A6XX_RB_UNKNOWN_88D0(0));

   uint32_t clear_vals[4] = { 0 };
   tu_pack_clear_value(&info->pClearValues[a], iview->vk_format, clear_vals);

   tu_cs_emit_regs(cs,
                   A6XX_RB_BLIT_CLEAR_COLOR_DW0(clear_vals[0]),
                   A6XX_RB_BLIT_CLEAR_COLOR_DW1(clear_vals[1]),
                   A6XX_RB_BLIT_CLEAR_COLOR_DW2(clear_vals[2]),
                   A6XX_RB_BLIT_CLEAR_COLOR_DW3(clear_vals[3]));

   tu6_emit_blit(cmd, cs);
}

static void
tu6_emit_store_attachment(struct tu_cmd_buffer *cmd,
                          struct tu_cs *cs,
                          uint32_t a,
                          uint32_t gmem_a)
{
   if (cmd->state.pass->attachments[a].store_op == VK_ATTACHMENT_STORE_OP_DONT_CARE)
      return;

   tu6_emit_blit_info(cmd, cs,
                      cmd->state.framebuffer->attachments[a].attachment,
                      cmd->state.pass->attachments[gmem_a].gmem_offset, true);
   tu6_emit_blit(cmd, cs);
}

static void
tu6_emit_tile_store(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   const struct tu_render_pass *pass = cmd->state.pass;
   const struct tu_subpass *subpass = &pass->subpasses[pass->subpass_count-1];

   tu_cs_emit_pkt7(cs, CP_SET_DRAW_STATE, 3);
   tu_cs_emit(cs, CP_SET_DRAW_STATE__0_COUNT(0) |
                     CP_SET_DRAW_STATE__0_DISABLE_ALL_GROUPS |
                     CP_SET_DRAW_STATE__0_GROUP_ID(0));
   tu_cs_emit(cs, CP_SET_DRAW_STATE__1_ADDR_LO(0));
   tu_cs_emit(cs, CP_SET_DRAW_STATE__2_ADDR_HI(0));

   tu_cs_emit_pkt7(cs, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
   tu_cs_emit(cs, 0x0);

   tu6_emit_marker(cmd, cs);
   tu_cs_emit_pkt7(cs, CP_SET_MARKER, 1);
   tu_cs_emit(cs, A6XX_CP_SET_MARKER_0_MODE(RM6_RESOLVE) | 0x10);
   tu6_emit_marker(cmd, cs);

   tu6_emit_blit_scissor(cmd, cs, true);

   for (uint32_t a = 0; a < pass->attachment_count; ++a) {
      if (pass->attachments[a].gmem_offset >= 0)
         tu6_emit_store_attachment(cmd, cs, a, a);
   }

   if (subpass->resolve_attachments) {
      for (unsigned i = 0; i < subpass->color_count; i++) {
         uint32_t a = subpass->resolve_attachments[i].attachment;
         if (a != VK_ATTACHMENT_UNUSED)
            tu6_emit_store_attachment(cmd, cs, a,
                                      subpass->color_attachments[i].attachment);
      }
   }
}

static void
tu6_emit_restart_index(struct tu_cs *cs, uint32_t restart_index)
{
   tu_cs_emit_regs(cs,
                   A6XX_PC_RESTART_INDEX(restart_index));
}

static void
tu6_init_hw(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   VkResult result = tu_cs_reserve_space(cmd->device, cs, 256);
   if (result != VK_SUCCESS) {
      cmd->record_result = result;
      return;
   }

   tu6_emit_cache_flush(cmd, cs);

   tu_cs_emit_write_reg(cs, REG_A6XX_HLSQ_UPDATE_CNTL, 0xfffff);

   tu_cs_emit_write_reg(cs, REG_A6XX_RB_CCU_CNTL, 0x7c400004);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_UNKNOWN_8E04, 0x00100000);
   tu_cs_emit_write_reg(cs, REG_A6XX_SP_UNKNOWN_AE04, 0x8);
   tu_cs_emit_write_reg(cs, REG_A6XX_SP_UNKNOWN_AE00, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_SP_UNKNOWN_AE0F, 0x3f);
   tu_cs_emit_write_reg(cs, REG_A6XX_SP_UNKNOWN_B605, 0x44);
   tu_cs_emit_write_reg(cs, REG_A6XX_SP_UNKNOWN_B600, 0x100000);
   tu_cs_emit_write_reg(cs, REG_A6XX_HLSQ_UNKNOWN_BE00, 0x80);
   tu_cs_emit_write_reg(cs, REG_A6XX_HLSQ_UNKNOWN_BE01, 0);

   tu_cs_emit_write_reg(cs, REG_A6XX_VPC_UNKNOWN_9600, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_GRAS_UNKNOWN_8600, 0x880);
   tu_cs_emit_write_reg(cs, REG_A6XX_HLSQ_UNKNOWN_BE04, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_SP_UNKNOWN_AE03, 0x00000410);
   tu_cs_emit_write_reg(cs, REG_A6XX_SP_IBO_COUNT, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_SP_UNKNOWN_B182, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_HLSQ_UNKNOWN_BB11, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_UCHE_UNKNOWN_0E12, 0x3200000);
   tu_cs_emit_write_reg(cs, REG_A6XX_UCHE_CLIENT_PF, 4);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_UNKNOWN_8E01, 0x0);
   tu_cs_emit_write_reg(cs, REG_A6XX_SP_UNKNOWN_AB00, 0x5);
   tu_cs_emit_write_reg(cs, REG_A6XX_VFD_ADD_OFFSET, A6XX_VFD_ADD_OFFSET_VERTEX);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_UNKNOWN_8811, 0x00000010);
   tu_cs_emit_write_reg(cs, REG_A6XX_PC_MODE_CNTL, 0x1f);

   tu_cs_emit_write_reg(cs, REG_A6XX_RB_SRGB_CNTL, 0);

   tu_cs_emit_write_reg(cs, REG_A6XX_GRAS_UNKNOWN_8101, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_GRAS_SAMPLE_CNTL, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_GRAS_UNKNOWN_8110, 0);

   tu_cs_emit_write_reg(cs, REG_A6XX_RB_RENDER_CONTROL0, 0x401);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_RENDER_CONTROL1, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_FS_OUTPUT_CNTL0, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_SAMPLE_CNTL, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_UNKNOWN_8818, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_UNKNOWN_8819, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_UNKNOWN_881A, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_UNKNOWN_881B, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_UNKNOWN_881C, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_UNKNOWN_881D, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_UNKNOWN_881E, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_UNKNOWN_88F0, 0);

   tu_cs_emit_write_reg(cs, REG_A6XX_VPC_UNKNOWN_9101, 0xffff00);
   tu_cs_emit_write_reg(cs, REG_A6XX_VPC_UNKNOWN_9107, 0);

   tu_cs_emit_write_reg(cs, REG_A6XX_VPC_UNKNOWN_9236, 1);
   tu_cs_emit_write_reg(cs, REG_A6XX_VPC_UNKNOWN_9300, 0);

   tu_cs_emit_write_reg(cs, REG_A6XX_VPC_SO_OVERRIDE,
                        A6XX_VPC_SO_OVERRIDE_SO_DISABLE);

   tu_cs_emit_write_reg(cs, REG_A6XX_PC_UNKNOWN_9801, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_PC_UNKNOWN_9806, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_PC_UNKNOWN_9980, 0);

   tu_cs_emit_write_reg(cs, REG_A6XX_PC_PRIMITIVE_CNTL_6, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_PC_UNKNOWN_9B07, 0);

   tu_cs_emit_write_reg(cs, REG_A6XX_SP_UNKNOWN_A81B, 0);

   tu_cs_emit_write_reg(cs, REG_A6XX_SP_UNKNOWN_B183, 0);

   tu_cs_emit_write_reg(cs, REG_A6XX_GRAS_UNKNOWN_8099, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_GRAS_UNKNOWN_809B, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_GRAS_UNKNOWN_80A0, 2);
   tu_cs_emit_write_reg(cs, REG_A6XX_GRAS_UNKNOWN_80AF, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_VPC_UNKNOWN_9210, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_VPC_UNKNOWN_9211, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_VPC_UNKNOWN_9602, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_PC_UNKNOWN_9981, 0x3);
   tu_cs_emit_write_reg(cs, REG_A6XX_PC_UNKNOWN_9E72, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_VPC_UNKNOWN_9108, 0x3);
   tu_cs_emit_write_reg(cs, REG_A6XX_SP_TP_UNKNOWN_B304, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_SP_TP_UNKNOWN_B309, 0x000000a2);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_UNKNOWN_8804, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_GRAS_UNKNOWN_80A4, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_GRAS_UNKNOWN_80A5, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_GRAS_UNKNOWN_80A6, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_UNKNOWN_8805, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_UNKNOWN_8806, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_UNKNOWN_8878, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_RB_UNKNOWN_8879, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_HLSQ_CONTROL_5_REG, 0xfc);

   tu6_emit_marker(cmd, cs);

   tu_cs_emit_write_reg(cs, REG_A6XX_VFD_MODE_CNTL, 0x00000000);

   tu_cs_emit_write_reg(cs, REG_A6XX_VFD_UNKNOWN_A008, 0);

   tu_cs_emit_write_reg(cs, REG_A6XX_PC_MODE_CNTL, 0x0000001f);

   /* we don't use this yet.. probably best to disable.. */
   tu_cs_emit_pkt7(cs, CP_SET_DRAW_STATE, 3);
   tu_cs_emit(cs, CP_SET_DRAW_STATE__0_COUNT(0) |
                     CP_SET_DRAW_STATE__0_DISABLE_ALL_GROUPS |
                     CP_SET_DRAW_STATE__0_GROUP_ID(0));
   tu_cs_emit(cs, CP_SET_DRAW_STATE__1_ADDR_LO(0));
   tu_cs_emit(cs, CP_SET_DRAW_STATE__2_ADDR_HI(0));

   tu_cs_emit_regs(cs,
                   A6XX_VPC_SO_BUFFER_BASE(0),
                   A6XX_VPC_SO_BUFFER_SIZE(0));

   tu_cs_emit_regs(cs,
                   A6XX_VPC_SO_FLUSH_BASE(0));

   tu_cs_emit_regs(cs,
                   A6XX_VPC_SO_BUF_CNTL(0));

   tu_cs_emit_regs(cs,
                   A6XX_VPC_SO_BUFFER_OFFSET(0, 0));

   tu_cs_emit_regs(cs,
                   A6XX_VPC_SO_BUFFER_BASE(1, 0),
                   A6XX_VPC_SO_BUFFER_SIZE(1, 0));

   tu_cs_emit_regs(cs,
                   A6XX_VPC_SO_BUFFER_OFFSET(1, 0),
                   A6XX_VPC_SO_FLUSH_BASE(1, 0),
                   A6XX_VPC_SO_BUFFER_BASE(2, 0),
                   A6XX_VPC_SO_BUFFER_SIZE(2, 0));

   tu_cs_emit_regs(cs,
                   A6XX_VPC_SO_BUFFER_OFFSET(2, 0),
                   A6XX_VPC_SO_FLUSH_BASE(2, 0),
                   A6XX_VPC_SO_BUFFER_BASE(3, 0),
                   A6XX_VPC_SO_BUFFER_SIZE(3, 0));

   tu_cs_emit_regs(cs,
                   A6XX_VPC_SO_BUFFER_OFFSET(3, 0),
                   A6XX_VPC_SO_FLUSH_BASE(3, 0));

   tu_cs_emit_regs(cs,
                   A6XX_SP_HS_CTRL_REG0(0));

   tu_cs_emit_regs(cs,
                   A6XX_SP_GS_CTRL_REG0(0));

   tu_cs_emit_regs(cs,
                   A6XX_GRAS_LRZ_CNTL(0));

   tu_cs_emit_regs(cs,
                   A6XX_RB_LRZ_CNTL(0));

   tu_cs_sanity_check(cs);
}

static void
tu6_cache_flush(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   unsigned seqno;

   seqno = tu6_emit_event_write(cmd, cs, CACHE_FLUSH_AND_INV_EVENT, true);

   tu_cs_emit_pkt7(cs, CP_WAIT_REG_MEM, 6);
   tu_cs_emit(cs, CP_WAIT_REG_MEM_0_FUNCTION(WRITE_EQ) |
                  CP_WAIT_REG_MEM_0_POLL_MEMORY);
   tu_cs_emit_qw(cs, cmd->scratch_bo.iova);
   tu_cs_emit(cs, CP_WAIT_REG_MEM_3_REF(seqno));
   tu_cs_emit(cs, CP_WAIT_REG_MEM_4_MASK(~0));
   tu_cs_emit(cs, CP_WAIT_REG_MEM_5_DELAY_LOOP_CYCLES(16));

   seqno = tu6_emit_event_write(cmd, cs, CACHE_FLUSH_TS, true);

   tu_cs_emit_pkt7(cs, CP_WAIT_MEM_GTE, 4);
   tu_cs_emit(cs, CP_WAIT_MEM_GTE_0_RESERVED(0));
   tu_cs_emit_qw(cs, cmd->scratch_bo.iova);
   tu_cs_emit(cs, CP_WAIT_MEM_GTE_3_REF(seqno));
}

static void
update_vsc_pipe(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   const struct tu_tiling_config *tiling = &cmd->state.tiling_config;

   tu_cs_emit_regs(cs,
                   A6XX_VSC_BIN_SIZE(.width = tiling->tile0.extent.width,
                                     .height = tiling->tile0.extent.height),
                   A6XX_VSC_SIZE_ADDRESS(.bo = &cmd->vsc_data,
                                         .bo_offset = 32 * cmd->vsc_data_pitch));

   tu_cs_emit_regs(cs,
                   A6XX_VSC_BIN_COUNT(.nx = tiling->tile_count.width,
                                      .ny = tiling->tile_count.height));

   tu_cs_emit_pkt4(cs, REG_A6XX_VSC_PIPE_CONFIG_REG(0), 32);
   for (unsigned i = 0; i < 32; i++)
      tu_cs_emit(cs, tiling->pipe_config[i]);

   tu_cs_emit_regs(cs,
                   A6XX_VSC_PIPE_DATA2_ADDRESS(.bo = &cmd->vsc_data2),
                   A6XX_VSC_PIPE_DATA2_PITCH(cmd->vsc_data2_pitch),
                   A6XX_VSC_PIPE_DATA2_ARRAY_PITCH(cmd->vsc_data2.size));

   tu_cs_emit_regs(cs,
                   A6XX_VSC_PIPE_DATA_ADDRESS(.bo = &cmd->vsc_data),
                   A6XX_VSC_PIPE_DATA_PITCH(cmd->vsc_data_pitch),
                   A6XX_VSC_PIPE_DATA_ARRAY_PITCH(cmd->vsc_data.size));
}

static void
emit_vsc_overflow_test(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   const struct tu_tiling_config *tiling = &cmd->state.tiling_config;
   const uint32_t used_pipe_count =
      tiling->pipe_count.width * tiling->pipe_count.height;

   /* Clear vsc_scratch: */
   tu_cs_emit_pkt7(cs, CP_MEM_WRITE, 3);
   tu_cs_emit_qw(cs, cmd->scratch_bo.iova + VSC_SCRATCH);
   tu_cs_emit(cs, 0x0);

   /* Check for overflow, write vsc_scratch if detected: */
   for (int i = 0; i < used_pipe_count; i++) {
      tu_cs_emit_pkt7(cs, CP_COND_WRITE5, 8);
      tu_cs_emit(cs, CP_COND_WRITE5_0_FUNCTION(WRITE_GE) |
            CP_COND_WRITE5_0_WRITE_MEMORY);
      tu_cs_emit(cs, CP_COND_WRITE5_1_POLL_ADDR_LO(REG_A6XX_VSC_SIZE_REG(i)));
      tu_cs_emit(cs, CP_COND_WRITE5_2_POLL_ADDR_HI(0));
      tu_cs_emit(cs, CP_COND_WRITE5_3_REF(cmd->vsc_data_pitch));
      tu_cs_emit(cs, CP_COND_WRITE5_4_MASK(~0));
      tu_cs_emit_qw(cs, cmd->scratch_bo.iova + VSC_SCRATCH);
      tu_cs_emit(cs, CP_COND_WRITE5_7_WRITE_DATA(1 + cmd->vsc_data_pitch));

      tu_cs_emit_pkt7(cs, CP_COND_WRITE5, 8);
      tu_cs_emit(cs, CP_COND_WRITE5_0_FUNCTION(WRITE_GE) |
            CP_COND_WRITE5_0_WRITE_MEMORY);
      tu_cs_emit(cs, CP_COND_WRITE5_1_POLL_ADDR_LO(REG_A6XX_VSC_SIZE2_REG(i)));
      tu_cs_emit(cs, CP_COND_WRITE5_2_POLL_ADDR_HI(0));
      tu_cs_emit(cs, CP_COND_WRITE5_3_REF(cmd->vsc_data2_pitch));
      tu_cs_emit(cs, CP_COND_WRITE5_4_MASK(~0));
      tu_cs_emit_qw(cs, cmd->scratch_bo.iova + VSC_SCRATCH);
      tu_cs_emit(cs, CP_COND_WRITE5_7_WRITE_DATA(3 + cmd->vsc_data2_pitch));
   }

   tu_cs_emit_pkt7(cs, CP_WAIT_MEM_WRITES, 0);

   tu_cs_emit_pkt7(cs, CP_WAIT_FOR_ME, 0);

   tu_cs_emit_pkt7(cs, CP_MEM_TO_REG, 3);
   tu_cs_emit(cs, CP_MEM_TO_REG_0_REG(OVERFLOW_FLAG_REG) |
         CP_MEM_TO_REG_0_CNT(1 - 1));
   tu_cs_emit_qw(cs, cmd->scratch_bo.iova + VSC_SCRATCH);

   /*
    * This is a bit awkward, we really want a way to invert the
    * CP_REG_TEST/CP_COND_REG_EXEC logic, so that we can conditionally
    * execute cmds to use hwbinning when a bit is *not* set.  This
    * dance is to invert OVERFLOW_FLAG_REG
    *
    * A CP_NOP packet is used to skip executing the 'else' clause
    * if (b0 set)..
    */

   /* b0 will be set if VSC_DATA or VSC_DATA2 overflow: */
   tu_cs_emit_pkt7(cs, CP_REG_TEST, 1);
   tu_cs_emit(cs, A6XX_CP_REG_TEST_0_REG(OVERFLOW_FLAG_REG) |
         A6XX_CP_REG_TEST_0_BIT(0) |
         A6XX_CP_REG_TEST_0_WAIT_FOR_ME);

   tu_cs_emit_pkt7(cs, CP_COND_REG_EXEC, 2);
   tu_cs_emit(cs, CP_COND_REG_EXEC_0_MODE(PRED_TEST));
   tu_cs_emit(cs, CP_COND_REG_EXEC_1_DWORDS(7));

   /* if (b0 set) */ {
      /*
       * On overflow, mirror the value to control->vsc_overflow
       * which CPU is checking to detect overflow (see
       * check_vsc_overflow())
       */
      tu_cs_emit_pkt7(cs, CP_REG_TO_MEM, 3);
      tu_cs_emit(cs, CP_REG_TO_MEM_0_REG(OVERFLOW_FLAG_REG) |
            CP_REG_TO_MEM_0_CNT(0));
      tu_cs_emit_qw(cs, cmd->scratch_bo.iova + VSC_OVERFLOW);

      tu_cs_emit_pkt4(cs, OVERFLOW_FLAG_REG, 1);
      tu_cs_emit(cs, 0x0);

      tu_cs_emit_pkt7(cs, CP_NOP, 2);  /* skip 'else' when 'if' is taken */
   } /* else */ {
      tu_cs_emit_pkt4(cs, OVERFLOW_FLAG_REG, 1);
      tu_cs_emit(cs, 0x1);
   }
}

static void
tu6_emit_binning_pass(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   const struct tu_tiling_config *tiling = &cmd->state.tiling_config;

   uint32_t x1 = tiling->tile0.offset.x;
   uint32_t y1 = tiling->tile0.offset.y;
   uint32_t x2 = tiling->render_area.offset.x + tiling->render_area.extent.width - 1;
   uint32_t y2 = tiling->render_area.offset.y + tiling->render_area.extent.height - 1;

   tu6_emit_window_scissor(cmd, cs, x1, y1, x2, y2);

   tu6_emit_marker(cmd, cs);
   tu_cs_emit_pkt7(cs, CP_SET_MARKER, 1);
   tu_cs_emit(cs, A6XX_CP_SET_MARKER_0_MODE(RM6_BINNING));
   tu6_emit_marker(cmd, cs);

   tu_cs_emit_pkt7(cs, CP_SET_VISIBILITY_OVERRIDE, 1);
   tu_cs_emit(cs, 0x1);

   tu_cs_emit_pkt7(cs, CP_SET_MODE, 1);
   tu_cs_emit(cs, 0x1);

   tu_cs_emit_wfi(cs);

   tu_cs_emit_regs(cs,
                   A6XX_VFD_MODE_CNTL(.binning_pass = true));

   update_vsc_pipe(cmd, cs);

   tu_cs_emit_regs(cs,
                   A6XX_PC_UNKNOWN_9805(.unknown = 0x1));

   tu_cs_emit_regs(cs,
                   A6XX_SP_UNKNOWN_A0F8(.unknown = 0x1));

   tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 1);
   tu_cs_emit(cs, UNK_2C);

   tu_cs_emit_regs(cs,
                   A6XX_RB_WINDOW_OFFSET(.x = 0, .y = 0));

   tu_cs_emit_regs(cs,
                   A6XX_SP_TP_WINDOW_OFFSET(.x = 0, .y = 0));

   /* emit IB to binning drawcmds: */
   tu_cs_emit_call(cs, &cmd->draw_cs);

   tu_cs_emit_pkt7(cs, CP_SET_DRAW_STATE, 3);
   tu_cs_emit(cs, CP_SET_DRAW_STATE__0_COUNT(0) |
                  CP_SET_DRAW_STATE__0_DISABLE_ALL_GROUPS |
                  CP_SET_DRAW_STATE__0_GROUP_ID(0));
   tu_cs_emit(cs, CP_SET_DRAW_STATE__1_ADDR_LO(0));
   tu_cs_emit(cs, CP_SET_DRAW_STATE__2_ADDR_HI(0));

   tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 1);
   tu_cs_emit(cs, UNK_2D);

   tu6_emit_event_write(cmd, cs, CACHE_INVALIDATE, false);
   tu6_cache_flush(cmd, cs);

   tu_cs_emit_wfi(cs);

   tu_cs_emit_pkt7(cs, CP_WAIT_FOR_ME, 0);

   emit_vsc_overflow_test(cmd, cs);

   tu_cs_emit_pkt7(cs, CP_SET_VISIBILITY_OVERRIDE, 1);
   tu_cs_emit(cs, 0x0);

   tu_cs_emit_pkt7(cs, CP_SET_MODE, 1);
   tu_cs_emit(cs, 0x0);

   tu_cs_emit_wfi(cs);

   tu_cs_emit_regs(cs,
                   A6XX_RB_CCU_CNTL(.unknown = 0x7c400004));

   cmd->wait_for_idle = false;
}

static void
tu6_render_begin(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   VkResult result = tu_cs_reserve_space(cmd->device, cs, 1024);
   if (result != VK_SUCCESS) {
      cmd->record_result = result;
      return;
   }

   tu6_emit_lrz_flush(cmd, cs);

   /* lrz clear? */

   tu6_emit_cache_flush(cmd, cs);

   tu_cs_emit_pkt7(cs, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
   tu_cs_emit(cs, 0x0);

   /* 0x10000000 for BYPASS.. 0x7c13c080 for GMEM: */
   tu6_emit_wfi(cmd, cs);
   tu_cs_emit_regs(cs,
                   A6XX_RB_CCU_CNTL(0x7c400004));

   if (use_hw_binning(cmd)) {
      tu6_emit_bin_size(cmd, cs, A6XX_RB_BIN_CONTROL_BINNING_PASS | 0x6000000);

      tu6_emit_render_cntl(cmd, cs, true);

      tu6_emit_binning_pass(cmd, cs);

      tu6_emit_bin_size(cmd, cs, A6XX_RB_BIN_CONTROL_USE_VIZ | 0x6000000);

      tu_cs_emit_regs(cs,
                      A6XX_VFD_MODE_CNTL(0));

      tu_cs_emit_regs(cs, A6XX_PC_UNKNOWN_9805(.unknown = 0x1));

      tu_cs_emit_regs(cs, A6XX_SP_UNKNOWN_A0F8(.unknown = 0x1));

      tu_cs_emit_pkt7(cs, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
      tu_cs_emit(cs, 0x1);
   } else {
      tu6_emit_bin_size(cmd, cs, 0x6000000);
   }

   tu6_emit_render_cntl(cmd, cs, false);

   tu_cs_sanity_check(cs);
}

static void
tu6_render_tile(struct tu_cmd_buffer *cmd,
                struct tu_cs *cs,
                const struct tu_tile *tile)
{
   const uint32_t render_tile_space = 256 + tu_cs_get_call_size(&cmd->draw_cs);
   VkResult result = tu_cs_reserve_space(cmd->device, cs, render_tile_space);
   if (result != VK_SUCCESS) {
      cmd->record_result = result;
      return;
   }

   tu6_emit_tile_select(cmd, cs, tile);
   tu_cs_emit_ib(cs, &cmd->state.tile_load_ib);

   tu_cs_emit_call(cs, &cmd->draw_cs);
   cmd->wait_for_idle = true;

   if (use_hw_binning(cmd)) {
      tu_cs_emit_pkt7(cs, CP_REG_TEST, 1);
      tu_cs_emit(cs, A6XX_CP_REG_TEST_0_REG(OVERFLOW_FLAG_REG) |
                     A6XX_CP_REG_TEST_0_BIT(0) |
                     A6XX_CP_REG_TEST_0_WAIT_FOR_ME);

      tu_cs_emit_pkt7(cs, CP_COND_REG_EXEC, 2);
      tu_cs_emit(cs, 0x10000000);
      tu_cs_emit(cs, 2);  /* conditionally execute next 2 dwords */

      /* if (no overflow) */ {
         tu_cs_emit_pkt7(cs, CP_SET_MARKER, 1);
         tu_cs_emit(cs, A6XX_CP_SET_MARKER_0_MODE(0x5) | 0x10);
      }
   }

   tu_cs_emit_ib(cs, &cmd->state.tile_store_ib);

   tu_cs_sanity_check(cs);
}

static void
tu6_render_end(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   const uint32_t space = 16 + tu_cs_get_call_size(&cmd->draw_epilogue_cs);
   VkResult result = tu_cs_reserve_space(cmd->device, cs, space);
   if (result != VK_SUCCESS) {
      cmd->record_result = result;
      return;
   }

   tu_cs_emit_call(cs, &cmd->draw_epilogue_cs);

   tu_cs_emit_regs(cs,
                   A6XX_GRAS_LRZ_CNTL(0));

   tu6_emit_lrz_flush(cmd, cs);

   tu6_emit_event_write(cmd, cs, CACHE_FLUSH_TS, true);

   tu_cs_sanity_check(cs);
}

static void
tu_cmd_render_tiles(struct tu_cmd_buffer *cmd)
{
   const struct tu_tiling_config *tiling = &cmd->state.tiling_config;

   tu6_render_begin(cmd, &cmd->cs);

   for (uint32_t y = 0; y < tiling->tile_count.height; y++) {
      for (uint32_t x = 0; x < tiling->tile_count.width; x++) {
         struct tu_tile tile;
         tu_tiling_config_get_tile(tiling, cmd->device, x, y, &tile);
         tu6_render_tile(cmd, &cmd->cs, &tile);
      }
   }

   tu6_render_end(cmd, &cmd->cs);
}

static void
tu_cmd_prepare_tile_load_ib(struct tu_cmd_buffer *cmd,
                            const VkRenderPassBeginInfo *info)
{
   const uint32_t tile_load_space =
      8 + (23+19) * cmd->state.pass->attachment_count +
      21 + (13 * cmd->state.subpass->color_count + 8) + 11;

   struct tu_cs sub_cs;

   VkResult result = tu_cs_begin_sub_stream(cmd->device, &cmd->sub_cs,
                                            tile_load_space, &sub_cs);
   if (result != VK_SUCCESS) {
      cmd->record_result = result;
      return;
   }

   tu6_emit_blit_scissor(cmd, &sub_cs, true);

   for (uint32_t i = 0; i < cmd->state.pass->attachment_count; ++i)
      tu6_emit_load_attachment(cmd, &sub_cs, i);

   tu6_emit_blit_scissor(cmd, &sub_cs, false);

   for (uint32_t i = 0; i < cmd->state.pass->attachment_count; ++i)
      tu6_emit_clear_attachment(cmd, &sub_cs, i, info);

   /* invalidate because reading input attachments will cache GMEM and
    * the cache isn''t updated when GMEM is written
    * TODO: is there a no-cache bit for textures?
    */
   if (cmd->state.subpass->input_count)
      tu6_emit_event_write(cmd, &sub_cs, CACHE_INVALIDATE, false);

   tu6_emit_zs(cmd, cmd->state.subpass, &sub_cs);
   tu6_emit_mrt(cmd, cmd->state.subpass, &sub_cs);
   tu6_emit_msaa(cmd, cmd->state.subpass, &sub_cs);

   cmd->state.tile_load_ib = tu_cs_end_sub_stream(&cmd->sub_cs, &sub_cs);
}

static void
tu_cmd_prepare_tile_store_ib(struct tu_cmd_buffer *cmd)
{
   const uint32_t tile_store_space = 32 + 23 * cmd->state.pass->attachment_count;
   struct tu_cs sub_cs;

   VkResult result = tu_cs_begin_sub_stream(cmd->device, &cmd->sub_cs,
                                            tile_store_space, &sub_cs);
   if (result != VK_SUCCESS) {
      cmd->record_result = result;
      return;
   }

   /* emit to tile-store sub_cs */
   tu6_emit_tile_store(cmd, &sub_cs);

   cmd->state.tile_store_ib = tu_cs_end_sub_stream(&cmd->sub_cs, &sub_cs);
}

static void
tu_cmd_update_tiling_config(struct tu_cmd_buffer *cmd,
                            const VkRect2D *render_area)
{
   const struct tu_device *dev = cmd->device;
   struct tu_tiling_config *tiling = &cmd->state.tiling_config;

   tiling->render_area = *render_area;

   tu_tiling_config_update_tile_layout(tiling, dev, cmd->state.pass->gmem_pixels);
   tu_tiling_config_update_pipe_layout(tiling, dev);
   tu_tiling_config_update_pipes(tiling, dev);
}

const struct tu_dynamic_state default_dynamic_state = {
   .viewport =
     {
       .count = 0,
     },
   .scissor =
     {
       .count = 0,
     },
   .line_width = 1.0f,
   .depth_bias =
     {
       .bias = 0.0f,
       .clamp = 0.0f,
       .slope = 0.0f,
     },
   .blend_constants = { 0.0f, 0.0f, 0.0f, 0.0f },
   .depth_bounds =
     {
       .min = 0.0f,
       .max = 1.0f,
     },
   .stencil_compare_mask =
     {
       .front = ~0u,
       .back = ~0u,
     },
   .stencil_write_mask =
     {
       .front = ~0u,
       .back = ~0u,
     },
   .stencil_reference =
     {
       .front = 0u,
       .back = 0u,
     },
};

static void UNUSED /* FINISHME */
tu_bind_dynamic_state(struct tu_cmd_buffer *cmd_buffer,
                      const struct tu_dynamic_state *src)
{
   struct tu_dynamic_state *dest = &cmd_buffer->state.dynamic;
   uint32_t copy_mask = src->mask;
   uint32_t dest_mask = 0;

   tu_use_args(cmd_buffer); /* FINISHME */

   /* Make sure to copy the number of viewports/scissors because they can
    * only be specified at pipeline creation time.
    */
   dest->viewport.count = src->viewport.count;
   dest->scissor.count = src->scissor.count;
   dest->discard_rectangle.count = src->discard_rectangle.count;

   if (copy_mask & TU_DYNAMIC_VIEWPORT) {
      if (memcmp(&dest->viewport.viewports, &src->viewport.viewports,
                 src->viewport.count * sizeof(VkViewport))) {
         typed_memcpy(dest->viewport.viewports, src->viewport.viewports,
                      src->viewport.count);
         dest_mask |= TU_DYNAMIC_VIEWPORT;
      }
   }

   if (copy_mask & TU_DYNAMIC_SCISSOR) {
      if (memcmp(&dest->scissor.scissors, &src->scissor.scissors,
                 src->scissor.count * sizeof(VkRect2D))) {
         typed_memcpy(dest->scissor.scissors, src->scissor.scissors,
                      src->scissor.count);
         dest_mask |= TU_DYNAMIC_SCISSOR;
      }
   }

   if (copy_mask & TU_DYNAMIC_LINE_WIDTH) {
      if (dest->line_width != src->line_width) {
         dest->line_width = src->line_width;
         dest_mask |= TU_DYNAMIC_LINE_WIDTH;
      }
   }

   if (copy_mask & TU_DYNAMIC_DEPTH_BIAS) {
      if (memcmp(&dest->depth_bias, &src->depth_bias,
                 sizeof(src->depth_bias))) {
         dest->depth_bias = src->depth_bias;
         dest_mask |= TU_DYNAMIC_DEPTH_BIAS;
      }
   }

   if (copy_mask & TU_DYNAMIC_BLEND_CONSTANTS) {
      if (memcmp(&dest->blend_constants, &src->blend_constants,
                 sizeof(src->blend_constants))) {
         typed_memcpy(dest->blend_constants, src->blend_constants, 4);
         dest_mask |= TU_DYNAMIC_BLEND_CONSTANTS;
      }
   }

   if (copy_mask & TU_DYNAMIC_DEPTH_BOUNDS) {
      if (memcmp(&dest->depth_bounds, &src->depth_bounds,
                 sizeof(src->depth_bounds))) {
         dest->depth_bounds = src->depth_bounds;
         dest_mask |= TU_DYNAMIC_DEPTH_BOUNDS;
      }
   }

   if (copy_mask & TU_DYNAMIC_STENCIL_COMPARE_MASK) {
      if (memcmp(&dest->stencil_compare_mask, &src->stencil_compare_mask,
                 sizeof(src->stencil_compare_mask))) {
         dest->stencil_compare_mask = src->stencil_compare_mask;
         dest_mask |= TU_DYNAMIC_STENCIL_COMPARE_MASK;
      }
   }

   if (copy_mask & TU_DYNAMIC_STENCIL_WRITE_MASK) {
      if (memcmp(&dest->stencil_write_mask, &src->stencil_write_mask,
                 sizeof(src->stencil_write_mask))) {
         dest->stencil_write_mask = src->stencil_write_mask;
         dest_mask |= TU_DYNAMIC_STENCIL_WRITE_MASK;
      }
   }

   if (copy_mask & TU_DYNAMIC_STENCIL_REFERENCE) {
      if (memcmp(&dest->stencil_reference, &src->stencil_reference,
                 sizeof(src->stencil_reference))) {
         dest->stencil_reference = src->stencil_reference;
         dest_mask |= TU_DYNAMIC_STENCIL_REFERENCE;
      }
   }

   if (copy_mask & TU_DYNAMIC_DISCARD_RECTANGLE) {
      if (memcmp(&dest->discard_rectangle.rectangles,
                 &src->discard_rectangle.rectangles,
                 src->discard_rectangle.count * sizeof(VkRect2D))) {
         typed_memcpy(dest->discard_rectangle.rectangles,
                      src->discard_rectangle.rectangles,
                      src->discard_rectangle.count);
         dest_mask |= TU_DYNAMIC_DISCARD_RECTANGLE;
      }
   }
}

static VkResult
tu_create_cmd_buffer(struct tu_device *device,
                     struct tu_cmd_pool *pool,
                     VkCommandBufferLevel level,
                     VkCommandBuffer *pCommandBuffer)
{
   struct tu_cmd_buffer *cmd_buffer;
   cmd_buffer = vk_zalloc(&pool->alloc, sizeof(*cmd_buffer), 8,
                          VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (cmd_buffer == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   cmd_buffer->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
   cmd_buffer->device = device;
   cmd_buffer->pool = pool;
   cmd_buffer->level = level;

   if (pool) {
      list_addtail(&cmd_buffer->pool_link, &pool->cmd_buffers);
      cmd_buffer->queue_family_index = pool->queue_family_index;

   } else {
      /* Init the pool_link so we can safely call list_del when we destroy
       * the command buffer
       */
      list_inithead(&cmd_buffer->pool_link);
      cmd_buffer->queue_family_index = TU_QUEUE_GENERAL;
   }

   tu_bo_list_init(&cmd_buffer->bo_list);
   tu_cs_init(&cmd_buffer->cs, TU_CS_MODE_GROW, 4096);
   tu_cs_init(&cmd_buffer->draw_cs, TU_CS_MODE_GROW, 4096);
   tu_cs_init(&cmd_buffer->draw_epilogue_cs, TU_CS_MODE_GROW, 4096);
   tu_cs_init(&cmd_buffer->sub_cs, TU_CS_MODE_SUB_STREAM, 2048);

   *pCommandBuffer = tu_cmd_buffer_to_handle(cmd_buffer);

   list_inithead(&cmd_buffer->upload.list);

   cmd_buffer->marker_reg = REG_A6XX_CP_SCRATCH_REG(
      cmd_buffer->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY ? 7 : 6);

   VkResult result = tu_bo_init_new(device, &cmd_buffer->scratch_bo, 0x1000);
   if (result != VK_SUCCESS)
      goto fail_scratch_bo;

#define VSC_DATA_SIZE(pitch)  ((pitch) * 32 + 0x100)  /* extra size to store VSC_SIZE */
#define VSC_DATA2_SIZE(pitch) ((pitch) * 32)

   /* TODO: resize on overflow or compute a max size from # of vertices in renderpass?? */
   cmd_buffer->vsc_data_pitch = 0x440 * 4;
   cmd_buffer->vsc_data2_pitch = 0x1040 * 4;

   result = tu_bo_init_new(device, &cmd_buffer->vsc_data, VSC_DATA_SIZE(cmd_buffer->vsc_data_pitch));
   if (result != VK_SUCCESS)
      goto fail_vsc_data;

   result = tu_bo_init_new(device, &cmd_buffer->vsc_data2, VSC_DATA2_SIZE(cmd_buffer->vsc_data2_pitch));
   if (result != VK_SUCCESS)
      goto fail_vsc_data2;

   return VK_SUCCESS;

fail_vsc_data2:
   tu_bo_finish(cmd_buffer->device, &cmd_buffer->vsc_data);
fail_vsc_data:
   tu_bo_finish(cmd_buffer->device, &cmd_buffer->scratch_bo);
fail_scratch_bo:
   list_del(&cmd_buffer->pool_link);
   return result;
}

static void
tu_cmd_buffer_destroy(struct tu_cmd_buffer *cmd_buffer)
{
   tu_bo_finish(cmd_buffer->device, &cmd_buffer->scratch_bo);
   tu_bo_finish(cmd_buffer->device, &cmd_buffer->vsc_data);
   tu_bo_finish(cmd_buffer->device, &cmd_buffer->vsc_data2);

   list_del(&cmd_buffer->pool_link);

   for (unsigned i = 0; i < VK_PIPELINE_BIND_POINT_RANGE_SIZE; i++)
      free(cmd_buffer->descriptors[i].push_set.set.mapped_ptr);

   tu_cs_finish(cmd_buffer->device, &cmd_buffer->cs);
   tu_cs_finish(cmd_buffer->device, &cmd_buffer->draw_cs);
   tu_cs_finish(cmd_buffer->device, &cmd_buffer->draw_epilogue_cs);
   tu_cs_finish(cmd_buffer->device, &cmd_buffer->sub_cs);

   tu_bo_list_destroy(&cmd_buffer->bo_list);
   vk_free(&cmd_buffer->pool->alloc, cmd_buffer);
}

static VkResult
tu_reset_cmd_buffer(struct tu_cmd_buffer *cmd_buffer)
{
   cmd_buffer->wait_for_idle = true;

   cmd_buffer->record_result = VK_SUCCESS;

   tu_bo_list_reset(&cmd_buffer->bo_list);
   tu_cs_reset(cmd_buffer->device, &cmd_buffer->cs);
   tu_cs_reset(cmd_buffer->device, &cmd_buffer->draw_cs);
   tu_cs_reset(cmd_buffer->device, &cmd_buffer->draw_epilogue_cs);
   tu_cs_reset(cmd_buffer->device, &cmd_buffer->sub_cs);

   for (unsigned i = 0; i < VK_PIPELINE_BIND_POINT_RANGE_SIZE; i++) {
      cmd_buffer->descriptors[i].valid = 0;
      cmd_buffer->descriptors[i].push_dirty = false;
   }

   cmd_buffer->status = TU_CMD_BUFFER_STATUS_INITIAL;

   return cmd_buffer->record_result;
}

VkResult
tu_AllocateCommandBuffers(VkDevice _device,
                          const VkCommandBufferAllocateInfo *pAllocateInfo,
                          VkCommandBuffer *pCommandBuffers)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(tu_cmd_pool, pool, pAllocateInfo->commandPool);

   VkResult result = VK_SUCCESS;
   uint32_t i;

   for (i = 0; i < pAllocateInfo->commandBufferCount; i++) {

      if (!list_is_empty(&pool->free_cmd_buffers)) {
         struct tu_cmd_buffer *cmd_buffer = list_first_entry(
            &pool->free_cmd_buffers, struct tu_cmd_buffer, pool_link);

         list_del(&cmd_buffer->pool_link);
         list_addtail(&cmd_buffer->pool_link, &pool->cmd_buffers);

         result = tu_reset_cmd_buffer(cmd_buffer);
         cmd_buffer->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
         cmd_buffer->level = pAllocateInfo->level;

         pCommandBuffers[i] = tu_cmd_buffer_to_handle(cmd_buffer);
      } else {
         result = tu_create_cmd_buffer(device, pool, pAllocateInfo->level,
                                       &pCommandBuffers[i]);
      }
      if (result != VK_SUCCESS)
         break;
   }

   if (result != VK_SUCCESS) {
      tu_FreeCommandBuffers(_device, pAllocateInfo->commandPool, i,
                            pCommandBuffers);

      /* From the Vulkan 1.0.66 spec:
       *
       * "vkAllocateCommandBuffers can be used to create multiple
       *  command buffers. If the creation of any of those command
       *  buffers fails, the implementation must destroy all
       *  successfully created command buffer objects from this
       *  command, set all entries of the pCommandBuffers array to
       *  NULL and return the error."
       */
      memset(pCommandBuffers, 0,
             sizeof(*pCommandBuffers) * pAllocateInfo->commandBufferCount);
   }

   return result;
}

void
tu_FreeCommandBuffers(VkDevice device,
                      VkCommandPool commandPool,
                      uint32_t commandBufferCount,
                      const VkCommandBuffer *pCommandBuffers)
{
   for (uint32_t i = 0; i < commandBufferCount; i++) {
      TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, pCommandBuffers[i]);

      if (cmd_buffer) {
         if (cmd_buffer->pool) {
            list_del(&cmd_buffer->pool_link);
            list_addtail(&cmd_buffer->pool_link,
                         &cmd_buffer->pool->free_cmd_buffers);
         } else
            tu_cmd_buffer_destroy(cmd_buffer);
      }
   }
}

VkResult
tu_ResetCommandBuffer(VkCommandBuffer commandBuffer,
                      VkCommandBufferResetFlags flags)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   return tu_reset_cmd_buffer(cmd_buffer);
}

VkResult
tu_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                      const VkCommandBufferBeginInfo *pBeginInfo)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   VkResult result = VK_SUCCESS;

   if (cmd_buffer->status != TU_CMD_BUFFER_STATUS_INITIAL) {
      /* If the command buffer has already been resetted with
       * vkResetCommandBuffer, no need to do it again.
       */
      result = tu_reset_cmd_buffer(cmd_buffer);
      if (result != VK_SUCCESS)
         return result;
   }

   memset(&cmd_buffer->state, 0, sizeof(cmd_buffer->state));
   cmd_buffer->usage_flags = pBeginInfo->flags;

   tu_cs_begin(&cmd_buffer->cs);
   tu_cs_begin(&cmd_buffer->draw_cs);
   tu_cs_begin(&cmd_buffer->draw_epilogue_cs);

   cmd_buffer->marker_seqno = 0;
   cmd_buffer->scratch_seqno = 0;

   /* setup initial configuration into command buffer */
   if (cmd_buffer->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
      switch (cmd_buffer->queue_family_index) {
      case TU_QUEUE_GENERAL:
         tu6_init_hw(cmd_buffer, &cmd_buffer->cs);
         break;
      default:
         break;
      }
   } else if (cmd_buffer->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY &&
              (pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) {
      assert(pBeginInfo->pInheritanceInfo);
      cmd_buffer->state.pass = tu_render_pass_from_handle(pBeginInfo->pInheritanceInfo->renderPass);
      cmd_buffer->state.subpass = &cmd_buffer->state.pass->subpasses[pBeginInfo->pInheritanceInfo->subpass];
   }

   cmd_buffer->status = TU_CMD_BUFFER_STATUS_RECORDING;

   return VK_SUCCESS;
}

void
tu_CmdBindVertexBuffers(VkCommandBuffer commandBuffer,
                        uint32_t firstBinding,
                        uint32_t bindingCount,
                        const VkBuffer *pBuffers,
                        const VkDeviceSize *pOffsets)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);

   assert(firstBinding + bindingCount <= MAX_VBS);

   for (uint32_t i = 0; i < bindingCount; i++) {
      cmd->state.vb.buffers[firstBinding + i] =
         tu_buffer_from_handle(pBuffers[i]);
      cmd->state.vb.offsets[firstBinding + i] = pOffsets[i];
   }

   /* VB states depend on VkPipelineVertexInputStateCreateInfo */
   cmd->state.dirty |= TU_CMD_DIRTY_VERTEX_BUFFERS;
}

void
tu_CmdBindIndexBuffer(VkCommandBuffer commandBuffer,
                      VkBuffer buffer,
                      VkDeviceSize offset,
                      VkIndexType indexType)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);
   TU_FROM_HANDLE(tu_buffer, buf, buffer);

   /* initialize/update the restart index */
   if (!cmd->state.index_buffer || cmd->state.index_type != indexType) {
      struct tu_cs *draw_cs = &cmd->draw_cs;
      VkResult result = tu_cs_reserve_space(cmd->device, draw_cs, 2);
      if (result != VK_SUCCESS) {
         cmd->record_result = result;
         return;
      }

      tu6_emit_restart_index(
         draw_cs, indexType == VK_INDEX_TYPE_UINT32 ? 0xffffffff : 0xffff);

      tu_cs_sanity_check(draw_cs);
   }

   /* track the BO */
   if (cmd->state.index_buffer != buf)
      tu_bo_list_add(&cmd->bo_list, buf->bo, MSM_SUBMIT_BO_READ);

   cmd->state.index_buffer = buf;
   cmd->state.index_offset = offset;
   cmd->state.index_type = indexType;
}

void
tu_CmdBindDescriptorSets(VkCommandBuffer commandBuffer,
                         VkPipelineBindPoint pipelineBindPoint,
                         VkPipelineLayout _layout,
                         uint32_t firstSet,
                         uint32_t descriptorSetCount,
                         const VkDescriptorSet *pDescriptorSets,
                         uint32_t dynamicOffsetCount,
                         const uint32_t *pDynamicOffsets)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   TU_FROM_HANDLE(tu_pipeline_layout, layout, _layout);
   unsigned dyn_idx = 0;

   struct tu_descriptor_state *descriptors_state =
      tu_get_descriptors_state(cmd_buffer, pipelineBindPoint);

   for (unsigned i = 0; i < descriptorSetCount; ++i) {
      unsigned idx = i + firstSet;
      TU_FROM_HANDLE(tu_descriptor_set, set, pDescriptorSets[i]);

      descriptors_state->sets[idx] = set;
      descriptors_state->valid |= (1u << idx);

      for(unsigned j = 0; j < set->layout->dynamic_offset_count; ++j, ++dyn_idx) {
         unsigned idx = j + layout->set[i + firstSet].dynamic_offset_start;
         assert(dyn_idx < dynamicOffsetCount);

         descriptors_state->dynamic_buffers[idx] =
         set->dynamic_descriptors[j].va + pDynamicOffsets[dyn_idx];
      }
   }

   cmd_buffer->state.dirty |= TU_CMD_DIRTY_DESCRIPTOR_SETS;
}

void
tu_CmdPushConstants(VkCommandBuffer commandBuffer,
                    VkPipelineLayout layout,
                    VkShaderStageFlags stageFlags,
                    uint32_t offset,
                    uint32_t size,
                    const void *pValues)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);
   memcpy((void*) cmd->push_constants + offset, pValues, size);
   cmd->state.dirty |= TU_CMD_DIRTY_PUSH_CONSTANTS;
}

VkResult
tu_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);

   if (cmd_buffer->scratch_seqno) {
      tu_bo_list_add(&cmd_buffer->bo_list, &cmd_buffer->scratch_bo,
                     MSM_SUBMIT_BO_WRITE);
   }

   if (cmd_buffer->use_vsc_data) {
      tu_bo_list_add(&cmd_buffer->bo_list, &cmd_buffer->vsc_data,
                     MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE);
      tu_bo_list_add(&cmd_buffer->bo_list, &cmd_buffer->vsc_data2,
                     MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE);
   }

   for (uint32_t i = 0; i < cmd_buffer->draw_cs.bo_count; i++) {
      tu_bo_list_add(&cmd_buffer->bo_list, cmd_buffer->draw_cs.bos[i],
                     MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_DUMP);
   }

   for (uint32_t i = 0; i < cmd_buffer->draw_epilogue_cs.bo_count; i++) {
      tu_bo_list_add(&cmd_buffer->bo_list, cmd_buffer->draw_epilogue_cs.bos[i],
                     MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_DUMP);
   }

   for (uint32_t i = 0; i < cmd_buffer->sub_cs.bo_count; i++) {
      tu_bo_list_add(&cmd_buffer->bo_list, cmd_buffer->sub_cs.bos[i],
                     MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_DUMP);
   }

   tu_cs_end(&cmd_buffer->cs);
   tu_cs_end(&cmd_buffer->draw_cs);
   tu_cs_end(&cmd_buffer->draw_epilogue_cs);

   cmd_buffer->status = TU_CMD_BUFFER_STATUS_EXECUTABLE;

   return cmd_buffer->record_result;
}

void
tu_CmdBindPipeline(VkCommandBuffer commandBuffer,
                   VkPipelineBindPoint pipelineBindPoint,
                   VkPipeline _pipeline)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);
   TU_FROM_HANDLE(tu_pipeline, pipeline, _pipeline);

   switch (pipelineBindPoint) {
   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      cmd->state.pipeline = pipeline;
      cmd->state.dirty |= TU_CMD_DIRTY_PIPELINE;
      break;
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      cmd->state.compute_pipeline = pipeline;
      cmd->state.dirty |= TU_CMD_DIRTY_COMPUTE_PIPELINE;
      break;
   default:
      unreachable("unrecognized pipeline bind point");
      break;
   }

   tu_bo_list_add(&cmd->bo_list, &pipeline->program.binary_bo,
                  MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_DUMP);
   for (uint32_t i = 0; i < pipeline->cs.bo_count; i++) {
      tu_bo_list_add(&cmd->bo_list, pipeline->cs.bos[i],
                     MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_DUMP);
   }
}

void
tu_CmdSetViewport(VkCommandBuffer commandBuffer,
                  uint32_t firstViewport,
                  uint32_t viewportCount,
                  const VkViewport *pViewports)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);
   struct tu_cs *draw_cs = &cmd->draw_cs;

   VkResult result = tu_cs_reserve_space(cmd->device, draw_cs, 12);
   if (result != VK_SUCCESS) {
      cmd->record_result = result;
      return;
   }

   assert(firstViewport == 0 && viewportCount == 1);
   tu6_emit_viewport(draw_cs, pViewports);

   tu_cs_sanity_check(draw_cs);
}

void
tu_CmdSetScissor(VkCommandBuffer commandBuffer,
                 uint32_t firstScissor,
                 uint32_t scissorCount,
                 const VkRect2D *pScissors)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);
   struct tu_cs *draw_cs = &cmd->draw_cs;

   VkResult result = tu_cs_reserve_space(cmd->device, draw_cs, 3);
   if (result != VK_SUCCESS) {
      cmd->record_result = result;
      return;
   }

   assert(firstScissor == 0 && scissorCount == 1);
   tu6_emit_scissor(draw_cs, pScissors);

   tu_cs_sanity_check(draw_cs);
}

void
tu_CmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);

   cmd->state.dynamic.line_width = lineWidth;

   /* line width depends on VkPipelineRasterizationStateCreateInfo */
   cmd->state.dirty |= TU_CMD_DIRTY_DYNAMIC_LINE_WIDTH;
}

void
tu_CmdSetDepthBias(VkCommandBuffer commandBuffer,
                   float depthBiasConstantFactor,
                   float depthBiasClamp,
                   float depthBiasSlopeFactor)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);
   struct tu_cs *draw_cs = &cmd->draw_cs;

   VkResult result = tu_cs_reserve_space(cmd->device, draw_cs, 4);
   if (result != VK_SUCCESS) {
      cmd->record_result = result;
      return;
   }

   tu6_emit_depth_bias(draw_cs, depthBiasConstantFactor, depthBiasClamp,
                       depthBiasSlopeFactor);

   tu_cs_sanity_check(draw_cs);
}

void
tu_CmdSetBlendConstants(VkCommandBuffer commandBuffer,
                        const float blendConstants[4])
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);
   struct tu_cs *draw_cs = &cmd->draw_cs;

   VkResult result = tu_cs_reserve_space(cmd->device, draw_cs, 5);
   if (result != VK_SUCCESS) {
      cmd->record_result = result;
      return;
   }

   tu6_emit_blend_constants(draw_cs, blendConstants);

   tu_cs_sanity_check(draw_cs);
}

void
tu_CmdSetDepthBounds(VkCommandBuffer commandBuffer,
                     float minDepthBounds,
                     float maxDepthBounds)
{
}

void
tu_CmdSetStencilCompareMask(VkCommandBuffer commandBuffer,
                            VkStencilFaceFlags faceMask,
                            uint32_t compareMask)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
      cmd->state.dynamic.stencil_compare_mask.front = compareMask;
   if (faceMask & VK_STENCIL_FACE_BACK_BIT)
      cmd->state.dynamic.stencil_compare_mask.back = compareMask;

   /* the front/back compare masks must be updated together */
   cmd->state.dirty |= TU_CMD_DIRTY_DYNAMIC_STENCIL_COMPARE_MASK;
}

void
tu_CmdSetStencilWriteMask(VkCommandBuffer commandBuffer,
                          VkStencilFaceFlags faceMask,
                          uint32_t writeMask)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
      cmd->state.dynamic.stencil_write_mask.front = writeMask;
   if (faceMask & VK_STENCIL_FACE_BACK_BIT)
      cmd->state.dynamic.stencil_write_mask.back = writeMask;

   /* the front/back write masks must be updated together */
   cmd->state.dirty |= TU_CMD_DIRTY_DYNAMIC_STENCIL_WRITE_MASK;
}

void
tu_CmdSetStencilReference(VkCommandBuffer commandBuffer,
                          VkStencilFaceFlags faceMask,
                          uint32_t reference)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
      cmd->state.dynamic.stencil_reference.front = reference;
   if (faceMask & VK_STENCIL_FACE_BACK_BIT)
      cmd->state.dynamic.stencil_reference.back = reference;

   /* the front/back references must be updated together */
   cmd->state.dirty |= TU_CMD_DIRTY_DYNAMIC_STENCIL_REFERENCE;
}

void
tu_CmdExecuteCommands(VkCommandBuffer commandBuffer,
                      uint32_t commandBufferCount,
                      const VkCommandBuffer *pCmdBuffers)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);
   VkResult result;

   assert(commandBufferCount > 0);

   for (uint32_t i = 0; i < commandBufferCount; i++) {
      TU_FROM_HANDLE(tu_cmd_buffer, secondary, pCmdBuffers[i]);

      result = tu_bo_list_merge(&cmd->bo_list, &secondary->bo_list);
      if (result != VK_SUCCESS) {
         cmd->record_result = result;
         break;
      }

      result = tu_cs_add_entries(&cmd->draw_cs, &secondary->draw_cs);
      if (result != VK_SUCCESS) {
         cmd->record_result = result;
         break;
      }

      result = tu_cs_add_entries(&cmd->draw_epilogue_cs,
            &secondary->draw_epilogue_cs);
      if (result != VK_SUCCESS) {
         cmd->record_result = result;
         break;
      }
   }
   cmd->state.dirty = ~0u; /* TODO: set dirty only what needs to be */
}

VkResult
tu_CreateCommandPool(VkDevice _device,
                     const VkCommandPoolCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkCommandPool *pCmdPool)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   struct tu_cmd_pool *pool;

   pool = vk_alloc2(&device->alloc, pAllocator, sizeof(*pool), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pool == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   if (pAllocator)
      pool->alloc = *pAllocator;
   else
      pool->alloc = device->alloc;

   list_inithead(&pool->cmd_buffers);
   list_inithead(&pool->free_cmd_buffers);

   pool->queue_family_index = pCreateInfo->queueFamilyIndex;

   *pCmdPool = tu_cmd_pool_to_handle(pool);

   return VK_SUCCESS;
}

void
tu_DestroyCommandPool(VkDevice _device,
                      VkCommandPool commandPool,
                      const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(tu_cmd_pool, pool, commandPool);

   if (!pool)
      return;

   list_for_each_entry_safe(struct tu_cmd_buffer, cmd_buffer,
                            &pool->cmd_buffers, pool_link)
   {
      tu_cmd_buffer_destroy(cmd_buffer);
   }

   list_for_each_entry_safe(struct tu_cmd_buffer, cmd_buffer,
                            &pool->free_cmd_buffers, pool_link)
   {
      tu_cmd_buffer_destroy(cmd_buffer);
   }

   vk_free2(&device->alloc, pAllocator, pool);
}

VkResult
tu_ResetCommandPool(VkDevice device,
                    VkCommandPool commandPool,
                    VkCommandPoolResetFlags flags)
{
   TU_FROM_HANDLE(tu_cmd_pool, pool, commandPool);
   VkResult result;

   list_for_each_entry(struct tu_cmd_buffer, cmd_buffer, &pool->cmd_buffers,
                       pool_link)
   {
      result = tu_reset_cmd_buffer(cmd_buffer);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

void
tu_TrimCommandPool(VkDevice device,
                   VkCommandPool commandPool,
                   VkCommandPoolTrimFlags flags)
{
   TU_FROM_HANDLE(tu_cmd_pool, pool, commandPool);

   if (!pool)
      return;

   list_for_each_entry_safe(struct tu_cmd_buffer, cmd_buffer,
                            &pool->free_cmd_buffers, pool_link)
   {
      tu_cmd_buffer_destroy(cmd_buffer);
   }
}

void
tu_CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                      const VkRenderPassBeginInfo *pRenderPassBegin,
                      VkSubpassContents contents)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);
   TU_FROM_HANDLE(tu_render_pass, pass, pRenderPassBegin->renderPass);
   TU_FROM_HANDLE(tu_framebuffer, fb, pRenderPassBegin->framebuffer);

   cmd->state.pass = pass;
   cmd->state.subpass = pass->subpasses;
   cmd->state.framebuffer = fb;

   tu_cmd_update_tiling_config(cmd, &pRenderPassBegin->renderArea);
   tu_cmd_prepare_tile_load_ib(cmd, pRenderPassBegin);
   tu_cmd_prepare_tile_store_ib(cmd);

   /* note: use_hw_binning only checks tiling config */
   if (use_hw_binning(cmd))
      cmd->use_vsc_data = true;

   for (uint32_t i = 0; i < fb->attachment_count; ++i) {
      const struct tu_image_view *iview = fb->attachments[i].attachment;
      tu_bo_list_add(&cmd->bo_list, iview->image->bo,
                     MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE);
   }
}

void
tu_CmdBeginRenderPass2(VkCommandBuffer commandBuffer,
                       const VkRenderPassBeginInfo *pRenderPassBeginInfo,
                       const VkSubpassBeginInfoKHR *pSubpassBeginInfo)
{
   tu_CmdBeginRenderPass(commandBuffer, pRenderPassBeginInfo,
                         pSubpassBeginInfo->contents);
}

void
tu_CmdNextSubpass(VkCommandBuffer commandBuffer, VkSubpassContents contents)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);
   const struct tu_render_pass *pass = cmd->state.pass;
   struct tu_cs *cs = &cmd->draw_cs;

   VkResult result = tu_cs_reserve_space(cmd->device, cs, 1024);
   if (result != VK_SUCCESS) {
      cmd->record_result = result;
      return;
   }

   const struct tu_subpass *subpass = cmd->state.subpass++;
   /* TODO:
    * if msaa samples change between subpasses,
    * attachment store is broken for some attachments
    */
   if (subpass->resolve_attachments) {
      tu6_emit_blit_scissor(cmd, cs, true);
      for (unsigned i = 0; i < subpass->color_count; i++) {
         uint32_t a = subpass->resolve_attachments[i].attachment;
         if (a != VK_ATTACHMENT_UNUSED) {
               tu6_emit_store_attachment(cmd, cs, a,
                                         subpass->color_attachments[i].attachment);
         }
      }
   }

   /* invalidate because reading input attachments will cache GMEM and
    * the cache isn''t updated when GMEM is written
    * TODO: is there a no-cache bit for textures?
    */
   if (cmd->state.subpass->input_count)
      tu6_emit_event_write(cmd, cs, CACHE_INVALIDATE, false);

   /* emit mrt/zs/msaa state for the subpass that is starting */
   tu6_emit_zs(cmd, cmd->state.subpass, cs);
   tu6_emit_mrt(cmd, cmd->state.subpass, cs);
   tu6_emit_msaa(cmd, cmd->state.subpass, cs);

   /* TODO:
    * since we don't know how to do GMEM->GMEM resolve,
    * resolve attachments are resolved to memory then loaded to GMEM again if needed
    */
   if (subpass->resolve_attachments) {
      for (unsigned i = 0; i < subpass->color_count; i++) {
         uint32_t a = subpass->resolve_attachments[i].attachment;
         const struct tu_image_view *iview =
            cmd->state.framebuffer->attachments[a].attachment;
         if (a != VK_ATTACHMENT_UNUSED && pass->attachments[a].gmem_offset >= 0) {
               tu_finishme("missing GMEM->GMEM resolve, performance will suffer\n");
               tu6_emit_blit_info(cmd, cs, iview, pass->attachments[a].gmem_offset, false);
               tu6_emit_blit(cmd, cs);
         }
      }
   }
}

void
tu_CmdNextSubpass2(VkCommandBuffer commandBuffer,
                   const VkSubpassBeginInfoKHR *pSubpassBeginInfo,
                   const VkSubpassEndInfoKHR *pSubpassEndInfo)
{
   tu_CmdNextSubpass(commandBuffer, pSubpassBeginInfo->contents);
}

struct tu_draw_info
{
   /**
    * Number of vertices.
    */
   uint32_t count;

   /**
    * Index of the first vertex.
    */
   int32_t vertex_offset;

   /**
    * First instance id.
    */
   uint32_t first_instance;

   /**
    * Number of instances.
    */
   uint32_t instance_count;

   /**
    * First index (indexed draws only).
    */
   uint32_t first_index;

   /**
    * Whether it's an indexed draw.
    */
   bool indexed;

   /**
    * Indirect draw parameters resource.
    */
   struct tu_buffer *indirect;
   uint64_t indirect_offset;
   uint32_t stride;

   /**
    * Draw count parameters resource.
    */
   struct tu_buffer *count_buffer;
   uint64_t count_buffer_offset;
};

#define ENABLE_ALL (CP_SET_DRAW_STATE__0_BINNING | CP_SET_DRAW_STATE__0_GMEM | CP_SET_DRAW_STATE__0_SYSMEM)
#define ENABLE_DRAW (CP_SET_DRAW_STATE__0_GMEM | CP_SET_DRAW_STATE__0_SYSMEM)

enum tu_draw_state_group_id
{
   TU_DRAW_STATE_PROGRAM,
   TU_DRAW_STATE_PROGRAM_BINNING,
   TU_DRAW_STATE_VI,
   TU_DRAW_STATE_VI_BINNING,
   TU_DRAW_STATE_VP,
   TU_DRAW_STATE_RAST,
   TU_DRAW_STATE_DS,
   TU_DRAW_STATE_BLEND,
   TU_DRAW_STATE_VS_CONST,
   TU_DRAW_STATE_FS_CONST,
   TU_DRAW_STATE_VS_TEX,
   TU_DRAW_STATE_FS_TEX,
   TU_DRAW_STATE_FS_IBO,
   TU_DRAW_STATE_VS_PARAMS,

   TU_DRAW_STATE_COUNT,
};

struct tu_draw_state_group
{
   enum tu_draw_state_group_id id;
   uint32_t enable_mask;
   struct tu_cs_entry ib;
};

const static struct tu_sampler*
sampler_ptr(struct tu_descriptor_state *descriptors_state,
            const struct tu_descriptor_map *map, unsigned i,
            unsigned array_index)
{
   assert(descriptors_state->valid & (1 << map->set[i]));

   struct tu_descriptor_set *set = descriptors_state->sets[map->set[i]];
   assert(map->binding[i] < set->layout->binding_count);

   const struct tu_descriptor_set_binding_layout *layout =
      &set->layout->binding[map->binding[i]];

   if (layout->immutable_samplers_offset) {
      const struct tu_sampler *immutable_samplers =
         tu_immutable_samplers(set->layout, layout);

      return &immutable_samplers[array_index];
   }

   switch (layout->type) {
   case VK_DESCRIPTOR_TYPE_SAMPLER:
      return (struct tu_sampler*) &set->mapped_ptr[layout->offset / 4];
   case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      return (struct tu_sampler*) &set->mapped_ptr[layout->offset / 4 + A6XX_TEX_CONST_DWORDS +
                                                   array_index *
                                                   (A6XX_TEX_CONST_DWORDS +
                                                    sizeof(struct tu_sampler) / 4)];
   default:
      unreachable("unimplemented descriptor type");
      break;
   }
}

static void
write_tex_const(struct tu_cmd_buffer *cmd,
                uint32_t *dst,
                struct tu_descriptor_state *descriptors_state,
                const struct tu_descriptor_map *map,
                unsigned i, unsigned array_index)
{
   assert(descriptors_state->valid & (1 << map->set[i]));

   struct tu_descriptor_set *set = descriptors_state->sets[map->set[i]];
   assert(map->binding[i] < set->layout->binding_count);

   const struct tu_descriptor_set_binding_layout *layout =
      &set->layout->binding[map->binding[i]];

   switch (layout->type) {
   case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
   case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
   case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
      memcpy(dst, &set->mapped_ptr[layout->offset / 4 +
                                   array_index * A6XX_TEX_CONST_DWORDS],
             A6XX_TEX_CONST_DWORDS * 4);
      break;
   case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      memcpy(dst, &set->mapped_ptr[layout->offset / 4 +
                                   array_index *
                                   (A6XX_TEX_CONST_DWORDS +
                                    sizeof(struct tu_sampler) / 4)],
             A6XX_TEX_CONST_DWORDS * 4);
      break;
   default:
      unreachable("unimplemented descriptor type");
      break;
   }

   if (layout->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
      const struct tu_tiling_config *tiling = &cmd->state.tiling_config;
      uint32_t a = cmd->state.subpass->input_attachments[map->value[i] +
                                                         array_index].attachment;
      const struct tu_render_pass_attachment *att = &cmd->state.pass->attachments[a];

      assert(att->gmem_offset >= 0);

      dst[0] &= ~(A6XX_TEX_CONST_0_SWAP__MASK | A6XX_TEX_CONST_0_TILE_MODE__MASK);
      dst[0] |= A6XX_TEX_CONST_0_TILE_MODE(TILE6_2);
      dst[2] &= ~(A6XX_TEX_CONST_2_TYPE__MASK | A6XX_TEX_CONST_2_PITCH__MASK);
      dst[2] |=
         A6XX_TEX_CONST_2_TYPE(A6XX_TEX_2D) |
         A6XX_TEX_CONST_2_PITCH(tiling->tile0.extent.width * att->cpp);
      dst[3] = 0;
      dst[4] = 0x100000 + att->gmem_offset;
      dst[5] = A6XX_TEX_CONST_5_DEPTH(1);
      for (unsigned i = 6; i < A6XX_TEX_CONST_DWORDS; i++)
         dst[i] = 0;

      if (cmd->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY)
         tu_finishme("patch input attachment pitch for secondary cmd buffer");
   }
}

static void
write_image_ibo(struct tu_cmd_buffer *cmd,
                uint32_t *dst,
                struct tu_descriptor_state *descriptors_state,
                const struct tu_descriptor_map *map,
                unsigned i, unsigned array_index)
{
   assert(descriptors_state->valid & (1 << map->set[i]));

   struct tu_descriptor_set *set = descriptors_state->sets[map->set[i]];
   assert(map->binding[i] < set->layout->binding_count);

   const struct tu_descriptor_set_binding_layout *layout =
      &set->layout->binding[map->binding[i]];

   assert(layout->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);

   memcpy(dst, &set->mapped_ptr[layout->offset / 4 +
                                (array_index * 2 + 1) * A6XX_TEX_CONST_DWORDS],
          A6XX_TEX_CONST_DWORDS * 4);
}

static uint64_t
buffer_ptr(struct tu_descriptor_state *descriptors_state,
           const struct tu_descriptor_map *map,
           unsigned i, unsigned array_index)
{
   assert(descriptors_state->valid & (1 << map->set[i]));

   struct tu_descriptor_set *set = descriptors_state->sets[map->set[i]];
   assert(map->binding[i] < set->layout->binding_count);

   const struct tu_descriptor_set_binding_layout *layout =
      &set->layout->binding[map->binding[i]];

   switch (layout->type) {
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
      return descriptors_state->dynamic_buffers[layout->dynamic_offset_offset +
                                                array_index];
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      return (uint64_t) set->mapped_ptr[layout->offset / 4 + array_index * 2 + 1] << 32 |
                        set->mapped_ptr[layout->offset / 4 + array_index * 2];
   default:
      unreachable("unimplemented descriptor type");
      break;
   }
}

static inline uint32_t
tu6_stage2opcode(gl_shader_stage type)
{
   switch (type) {
   case MESA_SHADER_VERTEX:
   case MESA_SHADER_TESS_CTRL:
   case MESA_SHADER_TESS_EVAL:
   case MESA_SHADER_GEOMETRY:
      return CP_LOAD_STATE6_GEOM;
   case MESA_SHADER_FRAGMENT:
   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_KERNEL:
      return CP_LOAD_STATE6_FRAG;
   default:
      unreachable("bad shader type");
   }
}

static inline enum a6xx_state_block
tu6_stage2shadersb(gl_shader_stage type)
{
   switch (type) {
   case MESA_SHADER_VERTEX:
      return SB6_VS_SHADER;
   case MESA_SHADER_FRAGMENT:
      return SB6_FS_SHADER;
   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_KERNEL:
      return SB6_CS_SHADER;
   default:
      unreachable("bad shader type");
      return ~0;
   }
}

static void
tu6_emit_user_consts(struct tu_cs *cs, const struct tu_pipeline *pipeline,
                     struct tu_descriptor_state *descriptors_state,
                     gl_shader_stage type,
                     uint32_t *push_constants)
{
   const struct tu_program_descriptor_linkage *link =
      &pipeline->program.link[type];
   const struct ir3_ubo_analysis_state *state = &link->ubo_state;

   for (uint32_t i = 0; i < ARRAY_SIZE(state->range); i++) {
      if (state->range[i].start < state->range[i].end) {
         uint32_t size = state->range[i].end - state->range[i].start;
         uint32_t offset = state->range[i].start;

         /* and even if the start of the const buffer is before
          * first_immediate, the end may not be:
          */
         size = MIN2(size, (16 * link->constlen) - state->range[i].offset);

         if (size == 0)
            continue;

         /* things should be aligned to vec4: */
         debug_assert((state->range[i].offset % 16) == 0);
         debug_assert((size % 16) == 0);
         debug_assert((offset % 16) == 0);

         if (i == 0) {
            /* push constants */
            tu_cs_emit_pkt7(cs, tu6_stage2opcode(type), 3 + (size / 4));
            tu_cs_emit(cs, CP_LOAD_STATE6_0_DST_OFF(state->range[i].offset / 16) |
                  CP_LOAD_STATE6_0_STATE_TYPE(ST6_CONSTANTS) |
                  CP_LOAD_STATE6_0_STATE_SRC(SS6_DIRECT) |
                  CP_LOAD_STATE6_0_STATE_BLOCK(tu6_stage2shadersb(type)) |
                  CP_LOAD_STATE6_0_NUM_UNIT(size / 16));
            tu_cs_emit(cs, 0);
            tu_cs_emit(cs, 0);
            for (unsigned i = 0; i < size / 4; i++)
               tu_cs_emit(cs, push_constants[i + offset / 4]);
            continue;
         }

         /* Look through the UBO map to find our UBO index, and get the VA for
          * that UBO.
          */
         uint64_t va = 0;
         uint32_t ubo_idx = i - 1;
         uint32_t ubo_map_base = 0;
         for (int j = 0; j < link->ubo_map.num; j++) {
            if (ubo_idx >= ubo_map_base &&
                ubo_idx < ubo_map_base + link->ubo_map.array_size[j]) {
               va = buffer_ptr(descriptors_state, &link->ubo_map, j,
                               ubo_idx - ubo_map_base);
               break;
            }
            ubo_map_base += link->ubo_map.array_size[j];
         }
         assert(va);

         tu_cs_emit_pkt7(cs, tu6_stage2opcode(type), 3);
         tu_cs_emit(cs, CP_LOAD_STATE6_0_DST_OFF(state->range[i].offset / 16) |
               CP_LOAD_STATE6_0_STATE_TYPE(ST6_CONSTANTS) |
               CP_LOAD_STATE6_0_STATE_SRC(SS6_INDIRECT) |
               CP_LOAD_STATE6_0_STATE_BLOCK(tu6_stage2shadersb(type)) |
               CP_LOAD_STATE6_0_NUM_UNIT(size / 16));
         tu_cs_emit_qw(cs, va + offset);
      }
   }
}

static void
tu6_emit_ubos(struct tu_cs *cs, const struct tu_pipeline *pipeline,
              struct tu_descriptor_state *descriptors_state,
              gl_shader_stage type)
{
   const struct tu_program_descriptor_linkage *link =
      &pipeline->program.link[type];

   uint32_t num = MIN2(link->ubo_map.num_desc, link->const_state.num_ubos);
   uint32_t anum = align(num, 2);

   if (!num)
      return;

   tu_cs_emit_pkt7(cs, tu6_stage2opcode(type), 3 + (2 * anum));
   tu_cs_emit(cs, CP_LOAD_STATE6_0_DST_OFF(link->const_state.offsets.ubo) |
         CP_LOAD_STATE6_0_STATE_TYPE(ST6_CONSTANTS) |
         CP_LOAD_STATE6_0_STATE_SRC(SS6_DIRECT) |
         CP_LOAD_STATE6_0_STATE_BLOCK(tu6_stage2shadersb(type)) |
         CP_LOAD_STATE6_0_NUM_UNIT(anum/2));
   tu_cs_emit(cs, CP_LOAD_STATE6_1_EXT_SRC_ADDR(0));
   tu_cs_emit(cs, CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(0));

   unsigned emitted = 0;
   for (unsigned i = 0; emitted < num && i < link->ubo_map.num; i++) {
      for (unsigned j = 0; emitted < num && j < link->ubo_map.array_size[i]; j++) {
         tu_cs_emit_qw(cs, buffer_ptr(descriptors_state, &link->ubo_map, i, j));
         emitted++;
      }
   }

   for (; emitted < anum; emitted++) {
      tu_cs_emit(cs, 0xffffffff);
      tu_cs_emit(cs, 0xffffffff);
   }
}

static struct tu_cs_entry
tu6_emit_consts(struct tu_cmd_buffer *cmd,
                const struct tu_pipeline *pipeline,
                struct tu_descriptor_state *descriptors_state,
                gl_shader_stage type)
{
   struct tu_cs cs;
   tu_cs_begin_sub_stream(cmd->device, &cmd->sub_cs, 512, &cs); /* TODO: maximum size? */

   tu6_emit_user_consts(&cs, pipeline, descriptors_state, type, cmd->push_constants);
   tu6_emit_ubos(&cs, pipeline, descriptors_state, type);

   return tu_cs_end_sub_stream(&cmd->sub_cs, &cs);
}

static VkResult
tu6_emit_vs_params(struct tu_cmd_buffer *cmd,
                   const struct tu_draw_info *draw,
                   struct tu_cs_entry *entry)
{
   /* TODO: fill out more than just base instance */
   const struct tu_program_descriptor_linkage *link =
      &cmd->state.pipeline->program.link[MESA_SHADER_VERTEX];
   const struct ir3_const_state *const_state = &link->const_state;
   struct tu_cs cs;

   if (const_state->offsets.driver_param >= link->constlen) {
      *entry = (struct tu_cs_entry) {};
      return VK_SUCCESS;
   }

   VkResult result = tu_cs_begin_sub_stream(cmd->device, &cmd->sub_cs, 8, &cs);
   if (result != VK_SUCCESS)
      return result;

   tu_cs_emit_pkt7(&cs, CP_LOAD_STATE6_GEOM, 3 + 4);
   tu_cs_emit(&cs, CP_LOAD_STATE6_0_DST_OFF(const_state->offsets.driver_param) |
         CP_LOAD_STATE6_0_STATE_TYPE(ST6_CONSTANTS) |
         CP_LOAD_STATE6_0_STATE_SRC(SS6_DIRECT) |
         CP_LOAD_STATE6_0_STATE_BLOCK(SB6_VS_SHADER) |
         CP_LOAD_STATE6_0_NUM_UNIT(1));
   tu_cs_emit(&cs, 0);
   tu_cs_emit(&cs, 0);

   STATIC_ASSERT(IR3_DP_INSTID_BASE == 2);

   tu_cs_emit(&cs, 0);
   tu_cs_emit(&cs, 0);
   tu_cs_emit(&cs, draw->first_instance);
   tu_cs_emit(&cs, 0);

   *entry = tu_cs_end_sub_stream(&cmd->sub_cs, &cs);
   return VK_SUCCESS;
}

static VkResult
tu6_emit_textures(struct tu_cmd_buffer *cmd,
                  const struct tu_pipeline *pipeline,
                  struct tu_descriptor_state *descriptors_state,
                  gl_shader_stage type,
                  struct tu_cs_entry *entry,
                  bool *needs_border)
{
   struct tu_device *device = cmd->device;
   struct tu_cs *draw_state = &cmd->sub_cs;
   const struct tu_program_descriptor_linkage *link =
      &pipeline->program.link[type];
   VkResult result;

   if (link->texture_map.num_desc == 0 && link->sampler_map.num_desc == 0) {
      *entry = (struct tu_cs_entry) {};
      return VK_SUCCESS;
   }

   /* allocate and fill texture state */
   struct ts_cs_memory tex_const;
   result = tu_cs_alloc(device, draw_state, link->texture_map.num_desc,
                        A6XX_TEX_CONST_DWORDS, &tex_const);
   if (result != VK_SUCCESS)
      return result;

   int tex_index = 0;
   for (unsigned i = 0; i < link->texture_map.num; i++) {
      for (int j = 0; j < link->texture_map.array_size[i]; j++) {
         write_tex_const(cmd,
                         &tex_const.map[A6XX_TEX_CONST_DWORDS * tex_index++],
                         descriptors_state, &link->texture_map, i, j);
      }
   }

   /* allocate and fill sampler state */
   struct ts_cs_memory tex_samp = { 0 };
   if (link->sampler_map.num_desc) {
      result = tu_cs_alloc(device, draw_state, link->sampler_map.num_desc,
                           A6XX_TEX_SAMP_DWORDS, &tex_samp);
      if (result != VK_SUCCESS)
         return result;

      int sampler_index = 0;
      for (unsigned i = 0; i < link->sampler_map.num; i++) {
         for (int j = 0; j < link->sampler_map.array_size[i]; j++) {
            const struct tu_sampler *sampler = sampler_ptr(descriptors_state,
                                                           &link->sampler_map,
                                                           i, j);
            memcpy(&tex_samp.map[A6XX_TEX_SAMP_DWORDS * sampler_index++],
                   sampler->state, sizeof(sampler->state));
            *needs_border |= sampler->needs_border;
         }
      }
   }

   unsigned tex_samp_reg, tex_const_reg, tex_count_reg;
   enum a6xx_state_block sb;

   switch (type) {
   case MESA_SHADER_VERTEX:
      sb = SB6_VS_TEX;
      tex_samp_reg = REG_A6XX_SP_VS_TEX_SAMP_LO;
      tex_const_reg = REG_A6XX_SP_VS_TEX_CONST_LO;
      tex_count_reg = REG_A6XX_SP_VS_TEX_COUNT;
      break;
   case MESA_SHADER_FRAGMENT:
      sb = SB6_FS_TEX;
      tex_samp_reg = REG_A6XX_SP_FS_TEX_SAMP_LO;
      tex_const_reg = REG_A6XX_SP_FS_TEX_CONST_LO;
      tex_count_reg = REG_A6XX_SP_FS_TEX_COUNT;
      break;
   case MESA_SHADER_COMPUTE:
      sb = SB6_CS_TEX;
      tex_samp_reg = REG_A6XX_SP_CS_TEX_SAMP_LO;
      tex_const_reg = REG_A6XX_SP_CS_TEX_CONST_LO;
      tex_count_reg = REG_A6XX_SP_CS_TEX_COUNT;
      break;
   default:
      unreachable("bad state block");
   }

   struct tu_cs cs;
   result = tu_cs_begin_sub_stream(device, draw_state, 16, &cs);
   if (result != VK_SUCCESS)
      return result;

   if (link->sampler_map.num_desc) {
      /* output sampler state: */
      tu_cs_emit_pkt7(&cs, tu6_stage2opcode(type), 3);
      tu_cs_emit(&cs, CP_LOAD_STATE6_0_DST_OFF(0) |
                 CP_LOAD_STATE6_0_STATE_TYPE(ST6_SHADER) |
                 CP_LOAD_STATE6_0_STATE_SRC(SS6_INDIRECT) |
                 CP_LOAD_STATE6_0_STATE_BLOCK(sb) |
                 CP_LOAD_STATE6_0_NUM_UNIT(link->sampler_map.num_desc));
      tu_cs_emit_qw(&cs, tex_samp.iova); /* SRC_ADDR_LO/HI */

      tu_cs_emit_pkt4(&cs, tex_samp_reg, 2);
      tu_cs_emit_qw(&cs, tex_samp.iova); /* SRC_ADDR_LO/HI */
   }

   /* emit texture state: */
   tu_cs_emit_pkt7(&cs, tu6_stage2opcode(type), 3);
   tu_cs_emit(&cs, CP_LOAD_STATE6_0_DST_OFF(0) |
      CP_LOAD_STATE6_0_STATE_TYPE(ST6_CONSTANTS) |
      CP_LOAD_STATE6_0_STATE_SRC(SS6_INDIRECT) |
      CP_LOAD_STATE6_0_STATE_BLOCK(sb) |
      CP_LOAD_STATE6_0_NUM_UNIT(link->texture_map.num_desc));
   tu_cs_emit_qw(&cs, tex_const.iova); /* SRC_ADDR_LO/HI */

   tu_cs_emit_pkt4(&cs, tex_const_reg, 2);
   tu_cs_emit_qw(&cs, tex_const.iova); /* SRC_ADDR_LO/HI */

   tu_cs_emit_pkt4(&cs, tex_count_reg, 1);
   tu_cs_emit(&cs, link->texture_map.num_desc);

   *entry = tu_cs_end_sub_stream(draw_state, &cs);
   return VK_SUCCESS;
}

static VkResult
tu6_emit_ibo(struct tu_cmd_buffer *cmd,
             const struct tu_pipeline *pipeline,
             struct tu_descriptor_state *descriptors_state,
             gl_shader_stage type,
             struct tu_cs_entry *entry)
{
   struct tu_device *device = cmd->device;
   struct tu_cs *draw_state = &cmd->sub_cs;
   const struct tu_program_descriptor_linkage *link =
      &pipeline->program.link[type];
   VkResult result;

   unsigned num_desc = link->ssbo_map.num_desc + link->image_map.num_desc;

   if (num_desc == 0) {
      *entry = (struct tu_cs_entry) {};
      return VK_SUCCESS;
   }

   struct ts_cs_memory ibo_const;
   result = tu_cs_alloc(device, draw_state, num_desc,
                        A6XX_TEX_CONST_DWORDS, &ibo_const);
   if (result != VK_SUCCESS)
      return result;

   int ssbo_index = 0;
   for (unsigned i = 0; i < link->ssbo_map.num; i++) {
      for (int j = 0; j < link->ssbo_map.array_size[i]; j++) {
         uint32_t *dst = &ibo_const.map[A6XX_TEX_CONST_DWORDS * ssbo_index];

         uint64_t va = buffer_ptr(descriptors_state, &link->ssbo_map, i, j);
         /* We don't expose robustBufferAccess, so leave the size unlimited. */
         uint32_t sz = MAX_STORAGE_BUFFER_RANGE / 4;

         dst[0] = A6XX_IBO_0_FMT(TFMT6_32_UINT);
         dst[1] = A6XX_IBO_1_WIDTH(sz & MASK(15)) |
                  A6XX_IBO_1_HEIGHT(sz >> 15);
         dst[2] = A6XX_IBO_2_UNK4 |
                  A6XX_IBO_2_UNK31 |
                  A6XX_IBO_2_TYPE(A6XX_TEX_1D);
         dst[3] = 0;
         dst[4] = va;
         dst[5] = va >> 32;
         for (int i = 6; i < A6XX_TEX_CONST_DWORDS; i++)
            dst[i] = 0;

         ssbo_index++;
      }
   }

   for (unsigned i = 0; i < link->image_map.num; i++) {
      for (int j = 0; j < link->image_map.array_size[i]; j++) {
         uint32_t *dst = &ibo_const.map[A6XX_TEX_CONST_DWORDS * ssbo_index];

         write_image_ibo(cmd, dst,
                         descriptors_state, &link->image_map, i, j);

         ssbo_index++;
      }
   }

   assert(ssbo_index == num_desc);

   struct tu_cs cs;
   result = tu_cs_begin_sub_stream(device, draw_state, 7, &cs);
   if (result != VK_SUCCESS)
      return result;

   uint32_t opcode, ibo_addr_reg;
   enum a6xx_state_block sb;
   enum a6xx_state_type st;

   switch (type) {
   case MESA_SHADER_FRAGMENT:
      opcode = CP_LOAD_STATE6;
      st = ST6_SHADER;
      sb = SB6_IBO;
      ibo_addr_reg = REG_A6XX_SP_IBO_LO;
      break;
   case MESA_SHADER_COMPUTE:
      opcode = CP_LOAD_STATE6_FRAG;
      st = ST6_IBO;
      sb = SB6_CS_SHADER;
      ibo_addr_reg = REG_A6XX_SP_CS_IBO_LO;
      break;
   default:
      unreachable("unsupported stage for ibos");
   }

   /* emit texture state: */
   tu_cs_emit_pkt7(&cs, opcode, 3);
   tu_cs_emit(&cs, CP_LOAD_STATE6_0_DST_OFF(0) |
              CP_LOAD_STATE6_0_STATE_TYPE(st) |
              CP_LOAD_STATE6_0_STATE_SRC(SS6_INDIRECT) |
              CP_LOAD_STATE6_0_STATE_BLOCK(sb) |
              CP_LOAD_STATE6_0_NUM_UNIT(num_desc));
   tu_cs_emit_qw(&cs, ibo_const.iova); /* SRC_ADDR_LO/HI */

   tu_cs_emit_pkt4(&cs, ibo_addr_reg, 2);
   tu_cs_emit_qw(&cs, ibo_const.iova); /* SRC_ADDR_LO/HI */

   *entry = tu_cs_end_sub_stream(draw_state, &cs);
   return VK_SUCCESS;
}

struct PACKED bcolor_entry {
   uint32_t fp32[4];
   uint16_t ui16[4];
   int16_t  si16[4];
   uint16_t fp16[4];
   uint16_t rgb565;
   uint16_t rgb5a1;
   uint16_t rgba4;
   uint8_t __pad0[2];
   uint8_t  ui8[4];
   int8_t   si8[4];
   uint32_t rgb10a2;
   uint32_t z24; /* also s8? */
   uint16_t srgb[4];      /* appears to duplicate fp16[], but clamped, used for srgb */
   uint8_t  __pad1[56];
} border_color[] = {
   [VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK] = {},
   [VK_BORDER_COLOR_INT_TRANSPARENT_BLACK] = {},
   [VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK] = {
      .fp32[3] = 0x3f800000,
      .ui16[3] = 0xffff,
      .si16[3] = 0x7fff,
      .fp16[3] = 0x3c00,
      .rgb5a1 = 0x8000,
      .rgba4 = 0xf000,
      .ui8[3] = 0xff,
      .si8[3] = 0x7f,
      .rgb10a2 = 0xc0000000,
      .srgb[3] = 0x3c00,
   },
   [VK_BORDER_COLOR_INT_OPAQUE_BLACK] = {
      .fp32[3] = 1,
      .fp16[3] = 1,
   },
   [VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE] = {
      .fp32[0 ... 3] = 0x3f800000,
      .ui16[0 ... 3] = 0xffff,
      .si16[0 ... 3] = 0x7fff,
      .fp16[0 ... 3] = 0x3c00,
      .rgb565 = 0xffff,
      .rgb5a1 = 0xffff,
      .rgba4 = 0xffff,
      .ui8[0 ... 3] = 0xff,
      .si8[0 ... 3] = 0x7f,
      .rgb10a2 = 0xffffffff,
      .z24 = 0xffffff,
      .srgb[0 ... 3] = 0x3c00,
   },
   [VK_BORDER_COLOR_INT_OPAQUE_WHITE] = {
      .fp32[0 ... 3] = 1,
      .fp16[0 ... 3] = 1,
   },
};

static VkResult
tu6_emit_border_color(struct tu_cmd_buffer *cmd,
                      struct tu_cs *cs)
{
   STATIC_ASSERT(sizeof(struct bcolor_entry) == 128);

   const struct tu_pipeline *pipeline = cmd->state.pipeline;
   struct tu_descriptor_state *descriptors_state =
      &cmd->descriptors[VK_PIPELINE_BIND_POINT_GRAPHICS];
   const struct tu_descriptor_map *vs_sampler =
      &pipeline->program.link[MESA_SHADER_VERTEX].sampler_map;
   const struct tu_descriptor_map *fs_sampler =
      &pipeline->program.link[MESA_SHADER_FRAGMENT].sampler_map;
   struct ts_cs_memory ptr;

   VkResult result = tu_cs_alloc(cmd->device, &cmd->sub_cs,
                                 vs_sampler->num_desc + fs_sampler->num_desc,
                                 128 / 4,
                                 &ptr);
   if (result != VK_SUCCESS)
      return result;

   for (unsigned i = 0; i < vs_sampler->num; i++) {
      for (unsigned j = 0; j < vs_sampler->array_size[i]; j++) {
         const struct tu_sampler *sampler = sampler_ptr(descriptors_state,
                                                        vs_sampler, i, j);
         memcpy(ptr.map, &border_color[sampler->border], 128);
         ptr.map += 128 / 4;
      }
   }

   for (unsigned i = 0; i < fs_sampler->num; i++) {
      for (unsigned j = 0; j < fs_sampler->array_size[i]; j++) {
         const struct tu_sampler *sampler = sampler_ptr(descriptors_state,
                                                        fs_sampler, i, j);
         memcpy(ptr.map, &border_color[sampler->border], 128);
         ptr.map += 128 / 4;
      }
   }

   tu_cs_emit_pkt4(cs, REG_A6XX_SP_TP_BORDER_COLOR_BASE_ADDR_LO, 2);
   tu_cs_emit_qw(cs, ptr.iova);
   return VK_SUCCESS;
}

static VkResult
tu6_bind_draw_states(struct tu_cmd_buffer *cmd,
                     struct tu_cs *cs,
                     const struct tu_draw_info *draw)
{
   const struct tu_pipeline *pipeline = cmd->state.pipeline;
   const struct tu_dynamic_state *dynamic = &cmd->state.dynamic;
   struct tu_draw_state_group draw_state_groups[TU_DRAW_STATE_COUNT];
   uint32_t draw_state_group_count = 0;

   struct tu_descriptor_state *descriptors_state =
      &cmd->descriptors[VK_PIPELINE_BIND_POINT_GRAPHICS];

   VkResult result = tu_cs_reserve_space(cmd->device, cs, 256);
   if (result != VK_SUCCESS)
      return result;

   /* TODO lrz */

   tu_cs_emit_write_reg(cs, REG_A6XX_PC_UNKNOWN_9806, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_PC_UNKNOWN_9990, 0);
   tu_cs_emit_write_reg(cs, REG_A6XX_VFD_UNKNOWN_A008, 0);

   tu_cs_emit_regs(cs,
                   A6XX_PC_PRIMITIVE_CNTL_0(.primitive_restart =
                                            pipeline->ia.primitive_restart && draw->indexed));

   if (cmd->state.dirty &
          (TU_CMD_DIRTY_PIPELINE | TU_CMD_DIRTY_DYNAMIC_LINE_WIDTH) &&
       (pipeline->dynamic_state.mask & TU_DYNAMIC_LINE_WIDTH)) {
      tu6_emit_gras_su_cntl(cs, pipeline->rast.gras_su_cntl,
                            dynamic->line_width);
   }

   if ((cmd->state.dirty & TU_CMD_DIRTY_DYNAMIC_STENCIL_COMPARE_MASK) &&
       (pipeline->dynamic_state.mask & TU_DYNAMIC_STENCIL_COMPARE_MASK)) {
      tu6_emit_stencil_compare_mask(cs, dynamic->stencil_compare_mask.front,
                                    dynamic->stencil_compare_mask.back);
   }

   if ((cmd->state.dirty & TU_CMD_DIRTY_DYNAMIC_STENCIL_WRITE_MASK) &&
       (pipeline->dynamic_state.mask & TU_DYNAMIC_STENCIL_WRITE_MASK)) {
      tu6_emit_stencil_write_mask(cs, dynamic->stencil_write_mask.front,
                                  dynamic->stencil_write_mask.back);
   }

   if ((cmd->state.dirty & TU_CMD_DIRTY_DYNAMIC_STENCIL_REFERENCE) &&
       (pipeline->dynamic_state.mask & TU_DYNAMIC_STENCIL_REFERENCE)) {
      tu6_emit_stencil_reference(cs, dynamic->stencil_reference.front,
                                 dynamic->stencil_reference.back);
   }

   if (cmd->state.dirty &
       (TU_CMD_DIRTY_PIPELINE | TU_CMD_DIRTY_VERTEX_BUFFERS)) {
      for (uint32_t i = 0; i < pipeline->vi.count; i++) {
         const uint32_t binding = pipeline->vi.bindings[i];
         const uint32_t stride = pipeline->vi.strides[i];
         const struct tu_buffer *buf = cmd->state.vb.buffers[binding];
         const VkDeviceSize offset = buf->bo_offset +
                                     cmd->state.vb.offsets[binding] +
                                     pipeline->vi.offsets[i];
         const VkDeviceSize size =
            offset < buf->bo->size ? buf->bo->size - offset : 0;

         tu_cs_emit_regs(cs,
                         A6XX_VFD_FETCH_BASE(i, .bo = buf->bo, .bo_offset = offset),
                         A6XX_VFD_FETCH_SIZE(i, size),
                         A6XX_VFD_FETCH_STRIDE(i, stride));
      }
   }

   if (cmd->state.dirty & TU_CMD_DIRTY_PIPELINE) {
      draw_state_groups[draw_state_group_count++] =
         (struct tu_draw_state_group) {
            .id = TU_DRAW_STATE_PROGRAM,
            .enable_mask = ENABLE_DRAW,
            .ib = pipeline->program.state_ib,
         };
      draw_state_groups[draw_state_group_count++] =
         (struct tu_draw_state_group) {
            .id = TU_DRAW_STATE_PROGRAM_BINNING,
            .enable_mask = CP_SET_DRAW_STATE__0_BINNING,
            .ib = pipeline->program.binning_state_ib,
         };
      draw_state_groups[draw_state_group_count++] =
         (struct tu_draw_state_group) {
            .id = TU_DRAW_STATE_VI,
            .enable_mask = ENABLE_DRAW,
            .ib = pipeline->vi.state_ib,
         };
      draw_state_groups[draw_state_group_count++] =
         (struct tu_draw_state_group) {
            .id = TU_DRAW_STATE_VI_BINNING,
            .enable_mask = CP_SET_DRAW_STATE__0_BINNING,
            .ib = pipeline->vi.binning_state_ib,
         };
      draw_state_groups[draw_state_group_count++] =
         (struct tu_draw_state_group) {
            .id = TU_DRAW_STATE_VP,
            .enable_mask = ENABLE_ALL,
            .ib = pipeline->vp.state_ib,
         };
      draw_state_groups[draw_state_group_count++] =
         (struct tu_draw_state_group) {
            .id = TU_DRAW_STATE_RAST,
            .enable_mask = ENABLE_ALL,
            .ib = pipeline->rast.state_ib,
         };
      draw_state_groups[draw_state_group_count++] =
         (struct tu_draw_state_group) {
            .id = TU_DRAW_STATE_DS,
            .enable_mask = ENABLE_ALL,
            .ib = pipeline->ds.state_ib,
         };
      draw_state_groups[draw_state_group_count++] =
         (struct tu_draw_state_group) {
            .id = TU_DRAW_STATE_BLEND,
            .enable_mask = ENABLE_ALL,
            .ib = pipeline->blend.state_ib,
         };
   }

   if (cmd->state.dirty &
         (TU_CMD_DIRTY_PIPELINE | TU_CMD_DIRTY_DESCRIPTOR_SETS | TU_CMD_DIRTY_PUSH_CONSTANTS)) {
      draw_state_groups[draw_state_group_count++] =
         (struct tu_draw_state_group) {
            .id = TU_DRAW_STATE_VS_CONST,
            .enable_mask = ENABLE_ALL,
            .ib = tu6_emit_consts(cmd, pipeline, descriptors_state, MESA_SHADER_VERTEX)
         };
      draw_state_groups[draw_state_group_count++] =
         (struct tu_draw_state_group) {
            .id = TU_DRAW_STATE_FS_CONST,
            .enable_mask = ENABLE_DRAW,
            .ib = tu6_emit_consts(cmd, pipeline, descriptors_state, MESA_SHADER_FRAGMENT)
         };
   }

   if (cmd->state.dirty &
         (TU_CMD_DIRTY_PIPELINE | TU_CMD_DIRTY_DESCRIPTOR_SETS)) {
      bool needs_border = false;
      struct tu_cs_entry vs_tex, fs_tex, fs_ibo;

      result = tu6_emit_textures(cmd, pipeline, descriptors_state,
                                 MESA_SHADER_VERTEX, &vs_tex, &needs_border);
      if (result != VK_SUCCESS)
         return result;

      result = tu6_emit_textures(cmd, pipeline, descriptors_state,
                                 MESA_SHADER_FRAGMENT, &fs_tex, &needs_border);
      if (result != VK_SUCCESS)
         return result;

      result = tu6_emit_ibo(cmd, pipeline, descriptors_state,
                            MESA_SHADER_FRAGMENT, &fs_ibo);
      if (result != VK_SUCCESS)
         return result;

      draw_state_groups[draw_state_group_count++] =
         (struct tu_draw_state_group) {
            .id = TU_DRAW_STATE_VS_TEX,
            .enable_mask = ENABLE_ALL,
            .ib = vs_tex,
         };
      draw_state_groups[draw_state_group_count++] =
         (struct tu_draw_state_group) {
            .id = TU_DRAW_STATE_FS_TEX,
            .enable_mask = ENABLE_DRAW,
            .ib = fs_tex,
         };
      draw_state_groups[draw_state_group_count++] =
         (struct tu_draw_state_group) {
            .id = TU_DRAW_STATE_FS_IBO,
            .enable_mask = ENABLE_DRAW,
            .ib = fs_ibo,
         };

      if (needs_border) {
         result = tu6_emit_border_color(cmd, cs);
         if (result != VK_SUCCESS)
            return result;
      }
   }

   struct tu_cs_entry vs_params;
   result = tu6_emit_vs_params(cmd, draw, &vs_params);
   if (result != VK_SUCCESS)
      return result;

   draw_state_groups[draw_state_group_count++] =
      (struct tu_draw_state_group) {
         .id = TU_DRAW_STATE_VS_PARAMS,
         .enable_mask = ENABLE_ALL,
         .ib = vs_params,
      };

   tu_cs_emit_pkt7(cs, CP_SET_DRAW_STATE, 3 * draw_state_group_count);
   for (uint32_t i = 0; i < draw_state_group_count; i++) {
      const struct tu_draw_state_group *group = &draw_state_groups[i];
      debug_assert((group->enable_mask & ~ENABLE_ALL) == 0);
      uint32_t cp_set_draw_state =
         CP_SET_DRAW_STATE__0_COUNT(group->ib.size / 4) |
         group->enable_mask |
         CP_SET_DRAW_STATE__0_GROUP_ID(group->id);
      uint64_t iova;
      if (group->ib.size) {
         iova = group->ib.bo->iova + group->ib.offset;
      } else {
         cp_set_draw_state |= CP_SET_DRAW_STATE__0_DISABLE;
         iova = 0;
      }

      tu_cs_emit(cs, cp_set_draw_state);
      tu_cs_emit_qw(cs, iova);
   }

   tu_cs_sanity_check(cs);

   /* track BOs */
   if (cmd->state.dirty & TU_CMD_DIRTY_VERTEX_BUFFERS) {
      for (uint32_t i = 0; i < MAX_VBS; i++) {
         const struct tu_buffer *buf = cmd->state.vb.buffers[i];
         if (buf)
            tu_bo_list_add(&cmd->bo_list, buf->bo, MSM_SUBMIT_BO_READ);
      }
   }
   if (cmd->state.dirty & TU_CMD_DIRTY_DESCRIPTOR_SETS) {
      unsigned i;
      for_each_bit(i, descriptors_state->valid) {
         struct tu_descriptor_set *set = descriptors_state->sets[i];
         for (unsigned j = 0; j < set->layout->buffer_count; ++j)
            if (set->descriptors[j]) {
               tu_bo_list_add(&cmd->bo_list, set->descriptors[j],
                              MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE);
            }
      }
   }

   /* Fragment shader state overwrites compute shader state, so flag the
    * compute pipeline for re-emit.
    */
   cmd->state.dirty = TU_CMD_DIRTY_COMPUTE_PIPELINE;
   return VK_SUCCESS;
}

static void
tu6_emit_draw_direct(struct tu_cmd_buffer *cmd,
                     struct tu_cs *cs,
                     const struct tu_draw_info *draw)
{

   const enum pc_di_primtype primtype = cmd->state.pipeline->ia.primtype;

   tu_cs_emit_regs(cs,
                   A6XX_VFD_INDEX_OFFSET(draw->vertex_offset),
                   A6XX_VFD_INSTANCE_START_OFFSET(draw->first_instance));

   /* TODO hw binning */
   if (draw->indexed) {
      const enum a4xx_index_size index_size =
         tu6_index_size(cmd->state.index_type);
      const uint32_t index_bytes =
         (cmd->state.index_type == VK_INDEX_TYPE_UINT32) ? 4 : 2;
      const struct tu_buffer *buf = cmd->state.index_buffer;
      const VkDeviceSize offset = buf->bo_offset + cmd->state.index_offset +
                                  index_bytes * draw->first_index;
      const uint32_t size = index_bytes * draw->count;

      const uint32_t cp_draw_indx =
         CP_DRAW_INDX_OFFSET_0_PRIM_TYPE(primtype) |
         CP_DRAW_INDX_OFFSET_0_SOURCE_SELECT(DI_SRC_SEL_DMA) |
         CP_DRAW_INDX_OFFSET_0_INDEX_SIZE(index_size) |
         CP_DRAW_INDX_OFFSET_0_VIS_CULL(USE_VISIBILITY) | 0x2000;

      tu_cs_emit_pkt7(cs, CP_DRAW_INDX_OFFSET, 7);
      tu_cs_emit(cs, cp_draw_indx);
      tu_cs_emit(cs, draw->instance_count);
      tu_cs_emit(cs, draw->count);
      tu_cs_emit(cs, 0x0); /* XXX */
      tu_cs_emit_qw(cs, buf->bo->iova + offset);
      tu_cs_emit(cs, size);
   } else {
      const uint32_t cp_draw_indx =
         CP_DRAW_INDX_OFFSET_0_PRIM_TYPE(primtype) |
         CP_DRAW_INDX_OFFSET_0_SOURCE_SELECT(DI_SRC_SEL_AUTO_INDEX) |
         CP_DRAW_INDX_OFFSET_0_VIS_CULL(USE_VISIBILITY) | 0x2000;

      tu_cs_emit_pkt7(cs, CP_DRAW_INDX_OFFSET, 3);
      tu_cs_emit(cs, cp_draw_indx);
      tu_cs_emit(cs, draw->instance_count);
      tu_cs_emit(cs, draw->count);
   }
}

static void
tu_draw(struct tu_cmd_buffer *cmd, const struct tu_draw_info *draw)
{
   struct tu_cs *cs = &cmd->draw_cs;
   VkResult result;

   result = tu6_bind_draw_states(cmd, cs, draw);
   if (result != VK_SUCCESS) {
      cmd->record_result = result;
      return;
   }

   result = tu_cs_reserve_space(cmd->device, cs, 32);
   if (result != VK_SUCCESS) {
      cmd->record_result = result;
      return;
   }

   if (draw->indirect) {
      tu_finishme("indirect draw");
      return;
   }

   /* TODO tu6_emit_marker should pick different regs depending on cs */

   tu6_emit_marker(cmd, cs);
   tu6_emit_draw_direct(cmd, cs, draw);
   tu6_emit_marker(cmd, cs);

   cmd->wait_for_idle = true;

   tu_cs_sanity_check(cs);
}

void
tu_CmdDraw(VkCommandBuffer commandBuffer,
           uint32_t vertexCount,
           uint32_t instanceCount,
           uint32_t firstVertex,
           uint32_t firstInstance)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   struct tu_draw_info info = {};

   info.count = vertexCount;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.vertex_offset = firstVertex;

   tu_draw(cmd_buffer, &info);
}

void
tu_CmdDrawIndexed(VkCommandBuffer commandBuffer,
                  uint32_t indexCount,
                  uint32_t instanceCount,
                  uint32_t firstIndex,
                  int32_t vertexOffset,
                  uint32_t firstInstance)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   struct tu_draw_info info = {};

   info.indexed = true;
   info.count = indexCount;
   info.instance_count = instanceCount;
   info.first_index = firstIndex;
   info.vertex_offset = vertexOffset;
   info.first_instance = firstInstance;

   tu_draw(cmd_buffer, &info);
}

void
tu_CmdDrawIndirect(VkCommandBuffer commandBuffer,
                   VkBuffer _buffer,
                   VkDeviceSize offset,
                   uint32_t drawCount,
                   uint32_t stride)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   TU_FROM_HANDLE(tu_buffer, buffer, _buffer);
   struct tu_draw_info info = {};

   info.count = drawCount;
   info.indirect = buffer;
   info.indirect_offset = offset;
   info.stride = stride;

   tu_draw(cmd_buffer, &info);
}

void
tu_CmdDrawIndexedIndirect(VkCommandBuffer commandBuffer,
                          VkBuffer _buffer,
                          VkDeviceSize offset,
                          uint32_t drawCount,
                          uint32_t stride)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   TU_FROM_HANDLE(tu_buffer, buffer, _buffer);
   struct tu_draw_info info = {};

   info.indexed = true;
   info.count = drawCount;
   info.indirect = buffer;
   info.indirect_offset = offset;
   info.stride = stride;

   tu_draw(cmd_buffer, &info);
}

struct tu_dispatch_info
{
   /**
    * Determine the layout of the grid (in block units) to be used.
    */
   uint32_t blocks[3];

   /**
    * A starting offset for the grid. If unaligned is set, the offset
    * must still be aligned.
    */
   uint32_t offsets[3];
   /**
    * Whether it's an unaligned compute dispatch.
    */
   bool unaligned;

   /**
    * Indirect compute parameters resource.
    */
   struct tu_buffer *indirect;
   uint64_t indirect_offset;
};

static void
tu_emit_compute_driver_params(struct tu_cs *cs, struct tu_pipeline *pipeline,
                              const struct tu_dispatch_info *info)
{
   gl_shader_stage type = MESA_SHADER_COMPUTE;
   const struct tu_program_descriptor_linkage *link =
      &pipeline->program.link[type];
   const struct ir3_const_state *const_state = &link->const_state;
   uint32_t offset = const_state->offsets.driver_param;

   if (link->constlen <= offset)
      return;

   if (!info->indirect) {
      uint32_t driver_params[IR3_DP_CS_COUNT] = {
         [IR3_DP_NUM_WORK_GROUPS_X] = info->blocks[0],
         [IR3_DP_NUM_WORK_GROUPS_Y] = info->blocks[1],
         [IR3_DP_NUM_WORK_GROUPS_Z] = info->blocks[2],
         [IR3_DP_LOCAL_GROUP_SIZE_X] = pipeline->compute.local_size[0],
         [IR3_DP_LOCAL_GROUP_SIZE_Y] = pipeline->compute.local_size[1],
         [IR3_DP_LOCAL_GROUP_SIZE_Z] = pipeline->compute.local_size[2],
      };

      uint32_t num_consts = MIN2(const_state->num_driver_params,
                                 (link->constlen - offset) * 4);
      /* push constants */
      tu_cs_emit_pkt7(cs, tu6_stage2opcode(type), 3 + num_consts);
      tu_cs_emit(cs, CP_LOAD_STATE6_0_DST_OFF(offset) |
                 CP_LOAD_STATE6_0_STATE_TYPE(ST6_CONSTANTS) |
                 CP_LOAD_STATE6_0_STATE_SRC(SS6_DIRECT) |
                 CP_LOAD_STATE6_0_STATE_BLOCK(tu6_stage2shadersb(type)) |
                 CP_LOAD_STATE6_0_NUM_UNIT(num_consts / 4));
      tu_cs_emit(cs, 0);
      tu_cs_emit(cs, 0);
      uint32_t i;
      for (i = 0; i < num_consts; i++)
         tu_cs_emit(cs, driver_params[i]);
   } else {
      tu_finishme("Indirect driver params");
   }
}

static void
tu_dispatch(struct tu_cmd_buffer *cmd,
            const struct tu_dispatch_info *info)
{
   struct tu_cs *cs = &cmd->cs;
   struct tu_pipeline *pipeline = cmd->state.compute_pipeline;
   struct tu_descriptor_state *descriptors_state =
      &cmd->descriptors[VK_PIPELINE_BIND_POINT_COMPUTE];

   VkResult result = tu_cs_reserve_space(cmd->device, cs, 256);
   if (result != VK_SUCCESS) {
      cmd->record_result = result;
      return;
   }

   if (cmd->state.dirty & TU_CMD_DIRTY_COMPUTE_PIPELINE)
      tu_cs_emit_ib(cs, &pipeline->program.state_ib);

   struct tu_cs_entry ib;

   ib = tu6_emit_consts(cmd, pipeline, descriptors_state, MESA_SHADER_COMPUTE);
   if (ib.size)
      tu_cs_emit_ib(cs, &ib);

   tu_emit_compute_driver_params(cs, pipeline, info);

   bool needs_border;
   result = tu6_emit_textures(cmd, pipeline, descriptors_state,
                              MESA_SHADER_COMPUTE, &ib, &needs_border);
   if (result != VK_SUCCESS) {
      cmd->record_result = result;
      return;
   }

   if (ib.size)
      tu_cs_emit_ib(cs, &ib);

   if (needs_border)
      tu_finishme("compute border color");

   result = tu6_emit_ibo(cmd, pipeline, descriptors_state, MESA_SHADER_COMPUTE, &ib);
   if (result != VK_SUCCESS) {
      cmd->record_result = result;
      return;
   }

   if (ib.size)
      tu_cs_emit_ib(cs, &ib);

   /* track BOs */
   if (cmd->state.dirty & TU_CMD_DIRTY_DESCRIPTOR_SETS) {
      unsigned i;
      for_each_bit(i, descriptors_state->valid) {
         struct tu_descriptor_set *set = descriptors_state->sets[i];
         for (unsigned j = 0; j < set->layout->buffer_count; ++j)
            if (set->descriptors[j]) {
               tu_bo_list_add(&cmd->bo_list, set->descriptors[j],
                              MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE);
            }
      }
   }

   /* Compute shader state overwrites fragment shader state, so we flag the
    * graphics pipeline for re-emit.
    */
   cmd->state.dirty = TU_CMD_DIRTY_PIPELINE;

   tu_cs_emit_pkt7(cs, CP_SET_MARKER, 1);
   tu_cs_emit(cs, A6XX_CP_SET_MARKER_0_MODE(0x8));

   const uint32_t *local_size = pipeline->compute.local_size;
   const uint32_t *num_groups = info->blocks;
   tu_cs_emit_regs(cs,
                   A6XX_HLSQ_CS_NDRANGE_0(.kerneldim = 3,
                                          .localsizex = local_size[0] - 1,
                                          .localsizey = local_size[1] - 1,
                                          .localsizez = local_size[2] - 1),
                   A6XX_HLSQ_CS_NDRANGE_1(.globalsize_x = local_size[0] * num_groups[0]),
                   A6XX_HLSQ_CS_NDRANGE_2(.globaloff_x = 0),
                   A6XX_HLSQ_CS_NDRANGE_3(.globalsize_y = local_size[1] * num_groups[1]),
                   A6XX_HLSQ_CS_NDRANGE_4(.globaloff_y = 0),
                   A6XX_HLSQ_CS_NDRANGE_5(.globalsize_z = local_size[2] * num_groups[2]),
                   A6XX_HLSQ_CS_NDRANGE_6(.globaloff_z = 0));

   tu_cs_emit_regs(cs,
                   A6XX_HLSQ_CS_KERNEL_GROUP_X(1),
                   A6XX_HLSQ_CS_KERNEL_GROUP_Y(1),
                   A6XX_HLSQ_CS_KERNEL_GROUP_Z(1));

   if (info->indirect) {
      uint64_t iova = tu_buffer_iova(info->indirect) + info->indirect_offset;

      tu_bo_list_add(&cmd->bo_list, info->indirect->bo,
                     MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE);

      tu_cs_emit_pkt7(cs, CP_EXEC_CS_INDIRECT, 4);
      tu_cs_emit(cs, 0x00000000);
      tu_cs_emit_qw(cs, iova);
      tu_cs_emit(cs,
                 A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEX(local_size[0] - 1) |
                 A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEY(local_size[1] - 1) |
                 A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEZ(local_size[2] - 1));
   } else {
      tu_cs_emit_pkt7(cs, CP_EXEC_CS, 4);
      tu_cs_emit(cs, 0x00000000);
      tu_cs_emit(cs, CP_EXEC_CS_1_NGROUPS_X(info->blocks[0]));
      tu_cs_emit(cs, CP_EXEC_CS_2_NGROUPS_Y(info->blocks[1]));
      tu_cs_emit(cs, CP_EXEC_CS_3_NGROUPS_Z(info->blocks[2]));
   }

   tu_cs_emit_wfi(cs);

   tu6_emit_cache_flush(cmd, cs);
}

void
tu_CmdDispatchBase(VkCommandBuffer commandBuffer,
                   uint32_t base_x,
                   uint32_t base_y,
                   uint32_t base_z,
                   uint32_t x,
                   uint32_t y,
                   uint32_t z)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   struct tu_dispatch_info info = {};

   info.blocks[0] = x;
   info.blocks[1] = y;
   info.blocks[2] = z;

   info.offsets[0] = base_x;
   info.offsets[1] = base_y;
   info.offsets[2] = base_z;
   tu_dispatch(cmd_buffer, &info);
}

void
tu_CmdDispatch(VkCommandBuffer commandBuffer,
               uint32_t x,
               uint32_t y,
               uint32_t z)
{
   tu_CmdDispatchBase(commandBuffer, 0, 0, 0, x, y, z);
}

void
tu_CmdDispatchIndirect(VkCommandBuffer commandBuffer,
                       VkBuffer _buffer,
                       VkDeviceSize offset)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   TU_FROM_HANDLE(tu_buffer, buffer, _buffer);
   struct tu_dispatch_info info = {};

   info.indirect = buffer;
   info.indirect_offset = offset;

   tu_dispatch(cmd_buffer, &info);
}

void
tu_CmdEndRenderPass(VkCommandBuffer commandBuffer)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);

   tu_cs_end(&cmd_buffer->draw_cs);
   tu_cs_end(&cmd_buffer->draw_epilogue_cs);

   tu_cmd_render_tiles(cmd_buffer);

   /* discard draw_cs and draw_epilogue_cs entries now that the tiles are
      rendered */
   tu_cs_discard_entries(&cmd_buffer->draw_cs);
   tu_cs_begin(&cmd_buffer->draw_cs);
   tu_cs_discard_entries(&cmd_buffer->draw_epilogue_cs);
   tu_cs_begin(&cmd_buffer->draw_epilogue_cs);

   cmd_buffer->state.pass = NULL;
   cmd_buffer->state.subpass = NULL;
   cmd_buffer->state.framebuffer = NULL;
}

void
tu_CmdEndRenderPass2(VkCommandBuffer commandBuffer,
                     const VkSubpassEndInfoKHR *pSubpassEndInfo)
{
   tu_CmdEndRenderPass(commandBuffer);
}

struct tu_barrier_info
{
   uint32_t eventCount;
   const VkEvent *pEvents;
   VkPipelineStageFlags srcStageMask;
};

static void
tu_barrier(struct tu_cmd_buffer *cmd_buffer,
           uint32_t memoryBarrierCount,
           const VkMemoryBarrier *pMemoryBarriers,
           uint32_t bufferMemoryBarrierCount,
           const VkBufferMemoryBarrier *pBufferMemoryBarriers,
           uint32_t imageMemoryBarrierCount,
           const VkImageMemoryBarrier *pImageMemoryBarriers,
           const struct tu_barrier_info *info)
{
}

void
tu_CmdPipelineBarrier(VkCommandBuffer commandBuffer,
                      VkPipelineStageFlags srcStageMask,
                      VkPipelineStageFlags destStageMask,
                      VkBool32 byRegion,
                      uint32_t memoryBarrierCount,
                      const VkMemoryBarrier *pMemoryBarriers,
                      uint32_t bufferMemoryBarrierCount,
                      const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                      uint32_t imageMemoryBarrierCount,
                      const VkImageMemoryBarrier *pImageMemoryBarriers)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   struct tu_barrier_info info;

   info.eventCount = 0;
   info.pEvents = NULL;
   info.srcStageMask = srcStageMask;

   tu_barrier(cmd_buffer, memoryBarrierCount, pMemoryBarriers,
              bufferMemoryBarrierCount, pBufferMemoryBarriers,
              imageMemoryBarrierCount, pImageMemoryBarriers, &info);
}

static void
write_event(struct tu_cmd_buffer *cmd, struct tu_event *event, unsigned value)
{
   struct tu_cs *cs = &cmd->cs;

   VkResult result = tu_cs_reserve_space(cmd->device, cs, 4);
   if (result != VK_SUCCESS) {
      cmd->record_result = result;
      return;
   }

   tu_bo_list_add(&cmd->bo_list, &event->bo, MSM_SUBMIT_BO_WRITE);

   /* TODO: any flush required before/after ? */

   tu_cs_emit_pkt7(cs, CP_MEM_WRITE, 3);
   tu_cs_emit_qw(cs, event->bo.iova); /* ADDR_LO/HI */
   tu_cs_emit(cs, value);
}

void
tu_CmdSetEvent(VkCommandBuffer commandBuffer,
               VkEvent _event,
               VkPipelineStageFlags stageMask)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);
   TU_FROM_HANDLE(tu_event, event, _event);

   write_event(cmd, event, 1);
}

void
tu_CmdResetEvent(VkCommandBuffer commandBuffer,
                 VkEvent _event,
                 VkPipelineStageFlags stageMask)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);
   TU_FROM_HANDLE(tu_event, event, _event);

   write_event(cmd, event, 0);
}

void
tu_CmdWaitEvents(VkCommandBuffer commandBuffer,
                 uint32_t eventCount,
                 const VkEvent *pEvents,
                 VkPipelineStageFlags srcStageMask,
                 VkPipelineStageFlags dstStageMask,
                 uint32_t memoryBarrierCount,
                 const VkMemoryBarrier *pMemoryBarriers,
                 uint32_t bufferMemoryBarrierCount,
                 const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                 uint32_t imageMemoryBarrierCount,
                 const VkImageMemoryBarrier *pImageMemoryBarriers)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);
   struct tu_cs *cs = &cmd->cs;

   VkResult result = tu_cs_reserve_space(cmd->device, cs, eventCount * 7);
   if (result != VK_SUCCESS) {
      cmd->record_result = result;
      return;
   }

   /* TODO: any flush required before/after? (CP_WAIT_FOR_ME?) */

   for (uint32_t i = 0; i < eventCount; i++) {
      const struct tu_event *event = (const struct tu_event*) pEvents[i];

      tu_bo_list_add(&cmd->bo_list, &event->bo, MSM_SUBMIT_BO_READ);

      tu_cs_emit_pkt7(cs, CP_WAIT_REG_MEM, 6);
      tu_cs_emit(cs, CP_WAIT_REG_MEM_0_FUNCTION(WRITE_EQ) |
                     CP_WAIT_REG_MEM_0_POLL_MEMORY);
      tu_cs_emit_qw(cs, event->bo.iova); /* POLL_ADDR_LO/HI */
      tu_cs_emit(cs, CP_WAIT_REG_MEM_3_REF(1));
      tu_cs_emit(cs, CP_WAIT_REG_MEM_4_MASK(~0u));
      tu_cs_emit(cs, CP_WAIT_REG_MEM_5_DELAY_LOOP_CYCLES(20));
   }
}

void
tu_CmdSetDeviceMask(VkCommandBuffer commandBuffer, uint32_t deviceMask)
{
   /* No-op */
}
