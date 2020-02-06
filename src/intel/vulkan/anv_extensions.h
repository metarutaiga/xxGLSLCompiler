/*
 * Copyright 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */


#ifndef ANV_EXTENSIONS_H
#define ANV_EXTENSIONS_H

#include "stdbool.h"

#define ANV_INSTANCE_EXTENSION_COUNT 16

extern const VkExtensionProperties anv_instance_extensions[];

struct anv_instance_extension_table {
   union {
      bool extensions[ANV_INSTANCE_EXTENSION_COUNT];
      struct {
         bool KHR_device_group_creation;
         bool KHR_display;
         bool KHR_external_fence_capabilities;
         bool KHR_external_memory_capabilities;
         bool KHR_external_semaphore_capabilities;
         bool KHR_get_display_properties2;
         bool KHR_get_physical_device_properties2;
         bool KHR_get_surface_capabilities2;
         bool KHR_surface;
         bool KHR_wayland_surface;
         bool KHR_xcb_surface;
         bool KHR_xlib_surface;
         bool EXT_acquire_xlib_display;
         bool EXT_debug_report;
         bool EXT_direct_mode_display;
         bool EXT_display_surface_counter;
      };
   };
};

extern const struct anv_instance_extension_table anv_instance_extensions_supported;


#define ANV_DEVICE_EXTENSION_COUNT 48

extern const VkExtensionProperties anv_device_extensions[];

struct anv_device_extension_table {
   union {
      bool extensions[ANV_DEVICE_EXTENSION_COUNT];
      struct {
        bool ANDROID_external_memory_android_hardware_buffer;
        bool ANDROID_native_buffer;
        bool KHR_8bit_storage;
        bool KHR_16bit_storage;
        bool KHR_bind_memory2;
        bool KHR_create_renderpass2;
        bool KHR_dedicated_allocation;
        bool KHR_depth_stencil_resolve;
        bool KHR_descriptor_update_template;
        bool KHR_device_group;
        bool KHR_draw_indirect_count;
        bool KHR_driver_properties;
        bool KHR_external_fence;
        bool KHR_external_fence_fd;
        bool KHR_external_memory;
        bool KHR_external_memory_fd;
        bool KHR_external_semaphore;
        bool KHR_external_semaphore_fd;
        bool KHR_get_memory_requirements2;
        bool KHR_image_format_list;
        bool KHR_incremental_present;
        bool KHR_maintenance1;
        bool KHR_maintenance2;
        bool KHR_maintenance3;
        bool KHR_multiview;
        bool KHR_push_descriptor;
        bool KHR_relaxed_block_layout;
        bool KHR_sampler_mirror_clamp_to_edge;
        bool KHR_sampler_ycbcr_conversion;
        bool KHR_shader_draw_parameters;
        bool KHR_storage_buffer_storage_class;
        bool KHR_swapchain;
        bool KHR_variable_pointers;
        bool EXT_calibrated_timestamps;
        bool EXT_conditional_rendering;
        bool EXT_display_control;
        bool EXT_external_memory_dma_buf;
        bool EXT_global_priority;
        bool EXT_pci_bus_info;
        bool EXT_post_depth_coverage;
        bool EXT_sampler_filter_minmax;
        bool EXT_scalar_block_layout;
        bool EXT_shader_viewport_index_layer;
        bool EXT_shader_stencil_export;
        bool EXT_transform_feedback;
        bool EXT_vertex_attribute_divisor;
        bool GOOGLE_decorate_string;
        bool GOOGLE_hlsl_functionality1;
      };
   };
};

struct anv_physical_device;

void
anv_physical_device_get_supported_extensions(const struct anv_physical_device *device,
                                             struct anv_device_extension_table *extensions);

#endif /* ANV_EXTENSIONS_H */
