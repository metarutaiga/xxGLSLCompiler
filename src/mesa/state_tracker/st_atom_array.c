
/**************************************************************************
 *
 * Copyright 2007 VMware, Inc.
 * Copyright 2012 Marek Olšák <maraeo@gmail.com>
 * All Rights Reserved.
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
 * IN NO EVENT SHALL AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

/*
 * This converts the VBO's vertex attribute/array information into
 * Gallium vertex state and binds it.
 *
 * Authors:
 *   Keith Whitwell <keithw@vmware.com>
 *   Marek Olšák <maraeo@gmail.com>
 */

#include "st_context.h"
#include "st_atom.h"
#include "st_cb_bufferobjects.h"
#include "st_draw.h"
#include "st_program.h"

#include "cso_cache/cso_context.h"
#include "util/u_math.h"
#include "util/u_upload_mgr.h"
#include "main/bufferobj.h"
#include "main/glformats.h"
#include "main/varray.h"
#include "main/arrayobj.h"

/* vertex_formats[gltype - GL_BYTE][integer*2 + normalized][size - 1] */
static const uint16_t vertex_formats[][4][4] = {
   { /* GL_BYTE */
      {
         PIPE_FORMAT_R8_SSCALED,
         PIPE_FORMAT_R8G8_SSCALED,
         PIPE_FORMAT_R8G8B8_SSCALED,
         PIPE_FORMAT_R8G8B8A8_SSCALED
      },
      {
         PIPE_FORMAT_R8_SNORM,
         PIPE_FORMAT_R8G8_SNORM,
         PIPE_FORMAT_R8G8B8_SNORM,
         PIPE_FORMAT_R8G8B8A8_SNORM
      },
      {
         PIPE_FORMAT_R8_SINT,
         PIPE_FORMAT_R8G8_SINT,
         PIPE_FORMAT_R8G8B8_SINT,
         PIPE_FORMAT_R8G8B8A8_SINT
      },
   },
   { /* GL_UNSIGNED_BYTE */
      {
         PIPE_FORMAT_R8_USCALED,
         PIPE_FORMAT_R8G8_USCALED,
         PIPE_FORMAT_R8G8B8_USCALED,
         PIPE_FORMAT_R8G8B8A8_USCALED
      },
      {
         PIPE_FORMAT_R8_UNORM,
         PIPE_FORMAT_R8G8_UNORM,
         PIPE_FORMAT_R8G8B8_UNORM,
         PIPE_FORMAT_R8G8B8A8_UNORM
      },
      {
         PIPE_FORMAT_R8_UINT,
         PIPE_FORMAT_R8G8_UINT,
         PIPE_FORMAT_R8G8B8_UINT,
         PIPE_FORMAT_R8G8B8A8_UINT
      },
   },
   { /* GL_SHORT */
      {
         PIPE_FORMAT_R16_SSCALED,
         PIPE_FORMAT_R16G16_SSCALED,
         PIPE_FORMAT_R16G16B16_SSCALED,
         PIPE_FORMAT_R16G16B16A16_SSCALED
      },
      {
         PIPE_FORMAT_R16_SNORM,
         PIPE_FORMAT_R16G16_SNORM,
         PIPE_FORMAT_R16G16B16_SNORM,
         PIPE_FORMAT_R16G16B16A16_SNORM
      },
      {
         PIPE_FORMAT_R16_SINT,
         PIPE_FORMAT_R16G16_SINT,
         PIPE_FORMAT_R16G16B16_SINT,
         PIPE_FORMAT_R16G16B16A16_SINT
      },
   },
   { /* GL_UNSIGNED_SHORT */
      {
         PIPE_FORMAT_R16_USCALED,
         PIPE_FORMAT_R16G16_USCALED,
         PIPE_FORMAT_R16G16B16_USCALED,
         PIPE_FORMAT_R16G16B16A16_USCALED
      },
      {
         PIPE_FORMAT_R16_UNORM,
         PIPE_FORMAT_R16G16_UNORM,
         PIPE_FORMAT_R16G16B16_UNORM,
         PIPE_FORMAT_R16G16B16A16_UNORM
      },
      {
         PIPE_FORMAT_R16_UINT,
         PIPE_FORMAT_R16G16_UINT,
         PIPE_FORMAT_R16G16B16_UINT,
         PIPE_FORMAT_R16G16B16A16_UINT
      },
   },
   { /* GL_INT */
      {
         PIPE_FORMAT_R32_SSCALED,
         PIPE_FORMAT_R32G32_SSCALED,
         PIPE_FORMAT_R32G32B32_SSCALED,
         PIPE_FORMAT_R32G32B32A32_SSCALED
      },
      {
         PIPE_FORMAT_R32_SNORM,
         PIPE_FORMAT_R32G32_SNORM,
         PIPE_FORMAT_R32G32B32_SNORM,
         PIPE_FORMAT_R32G32B32A32_SNORM
      },
      {
         PIPE_FORMAT_R32_SINT,
         PIPE_FORMAT_R32G32_SINT,
         PIPE_FORMAT_R32G32B32_SINT,
         PIPE_FORMAT_R32G32B32A32_SINT
      },
   },
   { /* GL_UNSIGNED_INT */
      {
         PIPE_FORMAT_R32_USCALED,
         PIPE_FORMAT_R32G32_USCALED,
         PIPE_FORMAT_R32G32B32_USCALED,
         PIPE_FORMAT_R32G32B32A32_USCALED
      },
      {
         PIPE_FORMAT_R32_UNORM,
         PIPE_FORMAT_R32G32_UNORM,
         PIPE_FORMAT_R32G32B32_UNORM,
         PIPE_FORMAT_R32G32B32A32_UNORM
      },
      {
         PIPE_FORMAT_R32_UINT,
         PIPE_FORMAT_R32G32_UINT,
         PIPE_FORMAT_R32G32B32_UINT,
         PIPE_FORMAT_R32G32B32A32_UINT
      },
   },
   { /* GL_FLOAT */
      {
         PIPE_FORMAT_R32_FLOAT,
         PIPE_FORMAT_R32G32_FLOAT,
         PIPE_FORMAT_R32G32B32_FLOAT,
         PIPE_FORMAT_R32G32B32A32_FLOAT
      },
      {
         PIPE_FORMAT_R32_FLOAT,
         PIPE_FORMAT_R32G32_FLOAT,
         PIPE_FORMAT_R32G32B32_FLOAT,
         PIPE_FORMAT_R32G32B32A32_FLOAT
      },
   },
   {{0}}, /* GL_2_BYTES */
   {{0}}, /* GL_3_BYTES */
   {{0}}, /* GL_4_BYTES */
   { /* GL_DOUBLE */
      {
         PIPE_FORMAT_R64_FLOAT,
         PIPE_FORMAT_R64G64_FLOAT,
         PIPE_FORMAT_R64G64B64_FLOAT,
         PIPE_FORMAT_R64G64B64A64_FLOAT
      },
      {
         PIPE_FORMAT_R64_FLOAT,
         PIPE_FORMAT_R64G64_FLOAT,
         PIPE_FORMAT_R64G64B64_FLOAT,
         PIPE_FORMAT_R64G64B64A64_FLOAT
      },
   },
   { /* GL_HALF_FLOAT */
      {
         PIPE_FORMAT_R16_FLOAT,
         PIPE_FORMAT_R16G16_FLOAT,
         PIPE_FORMAT_R16G16B16_FLOAT,
         PIPE_FORMAT_R16G16B16A16_FLOAT
      },
      {
         PIPE_FORMAT_R16_FLOAT,
         PIPE_FORMAT_R16G16_FLOAT,
         PIPE_FORMAT_R16G16B16_FLOAT,
         PIPE_FORMAT_R16G16B16A16_FLOAT
      },
   },
   { /* GL_FIXED */
      {
         PIPE_FORMAT_R32_FIXED,
         PIPE_FORMAT_R32G32_FIXED,
         PIPE_FORMAT_R32G32B32_FIXED,
         PIPE_FORMAT_R32G32B32A32_FIXED
      },
      {
         PIPE_FORMAT_R32_FIXED,
         PIPE_FORMAT_R32G32_FIXED,
         PIPE_FORMAT_R32G32B32_FIXED,
         PIPE_FORMAT_R32G32B32A32_FIXED
      },
   },
};


/**
 * Return a PIPE_FORMAT_x for the given GL datatype and size.
 */
static enum pipe_format
st_pipe_vertex_format(const struct gl_vertex_format *vformat)
{
   const GLubyte size = vformat->Size;
   const GLenum16 format = vformat->Format;
   const bool normalized = vformat->Normalized;
   const bool integer = vformat->Integer;
   GLenum16 type = vformat->Type;
   unsigned index;

   assert(size >= 1 && size <= 4);
   assert(format == GL_RGBA || format == GL_BGRA);
   assert(vformat->_ElementSize == _mesa_bytes_per_vertex_attrib(size, type));

   switch (type) {
   case GL_HALF_FLOAT_OES:
      type = GL_HALF_FLOAT;
      break;

   case GL_INT_2_10_10_10_REV:
      assert(size == 4 && !integer);

      if (format == GL_BGRA) {
         if (normalized)
            return PIPE_FORMAT_B10G10R10A2_SNORM;
         else
            return PIPE_FORMAT_B10G10R10A2_SSCALED;
      } else {
         if (normalized)
            return PIPE_FORMAT_R10G10B10A2_SNORM;
         else
            return PIPE_FORMAT_R10G10B10A2_SSCALED;
      }
      break;

   case GL_UNSIGNED_INT_2_10_10_10_REV:
      assert(size == 4 && !integer);

      if (format == GL_BGRA) {
         if (normalized)
            return PIPE_FORMAT_B10G10R10A2_UNORM;
         else
            return PIPE_FORMAT_B10G10R10A2_USCALED;
      } else {
         if (normalized)
            return PIPE_FORMAT_R10G10B10A2_UNORM;
         else
            return PIPE_FORMAT_R10G10B10A2_USCALED;
      }
      break;

   case GL_UNSIGNED_INT_10F_11F_11F_REV:
      assert(size == 3 && !integer && format == GL_RGBA);
      return PIPE_FORMAT_R11G11B10_FLOAT;

   case GL_UNSIGNED_BYTE:
      if (format == GL_BGRA) {
         /* this is an odd-ball case */
         assert(normalized);
         return PIPE_FORMAT_B8G8R8A8_UNORM;
      }
      break;
   }

   index = integer*2 + normalized;
   assert(index <= 2);
   assert(type >= GL_BYTE && type <= GL_FIXED);
   return vertex_formats[type - GL_BYTE][index][size-1];
}

static void init_velement(struct pipe_vertex_element *velement,
                          int src_offset, int format,
                          int instance_divisor, int vbo_index)
{
   velement->src_offset = src_offset;
   velement->src_format = format;
   velement->instance_divisor = instance_divisor;
   velement->vertex_buffer_index = vbo_index;
   assert(velement->src_format);
}

static void init_velement_lowered(const struct st_vertex_program *vp,
                                  struct pipe_vertex_element *velements,
                                  const struct gl_vertex_format *vformat,
                                  int src_offset, int instance_divisor,
                                  int vbo_index, int idx)
{
   const GLubyte nr_components = vformat->Size;

   if (vformat->Doubles) {
      int lower_format;

      if (nr_components < 2)
         lower_format = PIPE_FORMAT_R32G32_UINT;
      else
         lower_format = PIPE_FORMAT_R32G32B32A32_UINT;

      init_velement(&velements[idx], src_offset,
                    lower_format, instance_divisor, vbo_index);
      idx++;

      if (idx < vp->num_inputs &&
          vp->index_to_input[idx] == ST_DOUBLE_ATTRIB_PLACEHOLDER) {
         if (nr_components >= 3) {
            if (nr_components == 3)
               lower_format = PIPE_FORMAT_R32G32_UINT;
            else
               lower_format = PIPE_FORMAT_R32G32B32A32_UINT;

            init_velement(&velements[idx], src_offset + 4 * sizeof(float),
                        lower_format, instance_divisor, vbo_index);
         } else {
            /* The values here are undefined. Fill in some conservative
             * dummy values.
             */
            init_velement(&velements[idx], src_offset, PIPE_FORMAT_R32G32_UINT,
                          instance_divisor, vbo_index);
         }
      }
   } else {
      const unsigned format = st_pipe_vertex_format(vformat);

      init_velement(&velements[idx], src_offset,
                    format, instance_divisor, vbo_index);
   }
}

void
st_setup_arrays(struct st_context *st,
                const struct st_vertex_program *vp,
                const struct st_common_variant *vp_variant,
                struct pipe_vertex_element *velements,
                struct pipe_vertex_buffer *vbuffer, unsigned *num_vbuffers,
                bool *has_user_vertex_buffers)
{
   struct gl_context *ctx = st->ctx;
   const struct gl_vertex_array_object *vao = ctx->Array._DrawVAO;
   const GLbitfield inputs_read = vp_variant->vert_attrib_mask;
   const ubyte *input_to_index = vp->input_to_index;
   bool uses_user_vertex_buffers = false;

   /* Process attribute array data. */
   GLbitfield mask = inputs_read & _mesa_draw_array_bits(ctx);
   while (mask) {
      /* The attribute index to start pulling a binding */
      const gl_vert_attrib i = ffs(mask) - 1;
      const struct gl_vertex_buffer_binding *const binding
         = _mesa_draw_buffer_binding(vao, i);
      const unsigned bufidx = (*num_vbuffers)++;

      if (_mesa_is_bufferobj(binding->BufferObj)) {
         /* Set the binding */
         struct st_buffer_object *stobj = st_buffer_object(binding->BufferObj);
         vbuffer[bufidx].buffer.resource = stobj ? stobj->buffer : NULL;
         vbuffer[bufidx].is_user_buffer = false;
         vbuffer[bufidx].buffer_offset = _mesa_draw_binding_offset(binding);
         if (st->has_signed_vertex_buffer_offset) {
            /* 'buffer_offset' will be interpreted as an signed int, so make sure
             * the user supplied offset is not negative (application bug).
             */
            if ((int) vbuffer[bufidx].buffer_offset < 0) {
               assert ((int) vbuffer[bufidx].buffer_offset >= 0);
               /* Fallback if assert are disabled: we can't disable this attribute
                * since other parts expects it (e.g: velements, vp_variant), so
                * use a non-buggy offset value instead */
               vbuffer[bufidx].buffer_offset = 0;
            }
         }
      } else {
         /* Set the binding */
         const void *ptr = (const void *)_mesa_draw_binding_offset(binding);
         vbuffer[bufidx].buffer.user = ptr;
         vbuffer[bufidx].is_user_buffer = true;
         vbuffer[bufidx].buffer_offset = 0;

         uses_user_vertex_buffers = true;
         if (!binding->InstanceDivisor)
            st->draw_needs_minmax_index = true;
      }
      vbuffer[bufidx].stride = binding->Stride; /* in bytes */

      const GLbitfield boundmask = _mesa_draw_bound_attrib_bits(binding);
      GLbitfield attrmask = mask & boundmask;
      /* Mark the those attributes as processed */
      mask &= ~boundmask;
      /* We can assume that we have array for the binding */
      assert(attrmask);
      /* Walk attributes belonging to the binding */
      while (attrmask) {
         const gl_vert_attrib attr = u_bit_scan(&attrmask);
         const struct gl_array_attributes *const attrib
            = _mesa_draw_array_attrib(vao, attr);
         const GLuint off = _mesa_draw_attributes_relative_offset(attrib);
         init_velement_lowered(vp, velements, &attrib->Format, off,
                               binding->InstanceDivisor, bufidx,
                               input_to_index[attr]);
      }
   }
   *has_user_vertex_buffers = uses_user_vertex_buffers;
}

void
st_setup_current(struct st_context *st,
                 const struct st_vertex_program *vp,
                 const struct st_common_variant *vp_variant,
                 struct pipe_vertex_element *velements,
                 struct pipe_vertex_buffer *vbuffer, unsigned *num_vbuffers)
{
   struct gl_context *ctx = st->ctx;
   const GLbitfield inputs_read = vp_variant->vert_attrib_mask;

   /* Process values that should have better been uniforms in the application */
   GLbitfield curmask = inputs_read & _mesa_draw_current_bits(ctx);
   if (curmask) {
      const ubyte *input_to_index = vp->input_to_index;
      /* For each attribute, upload the maximum possible size. */
      GLubyte data[VERT_ATTRIB_MAX * sizeof(GLdouble) * 4];
      GLubyte *cursor = data;
      const unsigned bufidx = (*num_vbuffers)++;
      unsigned max_alignment = 1;

      while (curmask) {
         const gl_vert_attrib attr = u_bit_scan(&curmask);
         const struct gl_array_attributes *const attrib
            = _mesa_draw_current_attrib(ctx, attr);
         const unsigned size = attrib->Format._ElementSize;
         const unsigned alignment = util_next_power_of_two(size);
         max_alignment = MAX2(max_alignment, alignment);
         memcpy(cursor, attrib->Ptr, size);
         if (alignment != size)
            memset(cursor + size, 0, alignment - size);

         init_velement_lowered(vp, velements, &attrib->Format, cursor - data, 0,
                               bufidx, input_to_index[attr]);

         cursor += alignment;
      }

      vbuffer[bufidx].is_user_buffer = false;
      vbuffer[bufidx].buffer.resource = NULL;
      /* vbuffer[bufidx].buffer_offset is set below */
      vbuffer[bufidx].stride = 0;

      /* Use const_uploader for zero-stride vertex attributes, because
       * it may use a better memory placement than stream_uploader.
       * The reason is that zero-stride attributes can be fetched many
       * times (thousands of times), so a better placement is going to
       * perform better.
       */
      struct u_upload_mgr *uploader = st->can_bind_const_buffer_as_vertex ?
                                      st->pipe->const_uploader :
                                      st->pipe->stream_uploader;
      u_upload_data(uploader,
                    0, cursor - data, max_alignment, data,
                    &vbuffer[bufidx].buffer_offset,
                    &vbuffer[bufidx].buffer.resource);
      /* Always unmap. The uploader might use explicit flushes. */
      u_upload_unmap(uploader);
   }
}

void
st_setup_current_user(struct st_context *st,
                      const struct st_vertex_program *vp,
                      const struct st_common_variant *vp_variant,
                      struct pipe_vertex_element *velements,
                      struct pipe_vertex_buffer *vbuffer, unsigned *num_vbuffers)
{
   struct gl_context *ctx = st->ctx;
   const GLbitfield inputs_read = vp_variant->vert_attrib_mask;
   const ubyte *input_to_index = vp->input_to_index;

   /* Process values that should have better been uniforms in the application */
   GLbitfield curmask = inputs_read & _mesa_draw_current_bits(ctx);
   /* For each attribute, make an own user buffer binding. */
   while (curmask) {
      const gl_vert_attrib attr = u_bit_scan(&curmask);
      const struct gl_array_attributes *const attrib
         = _mesa_draw_current_attrib(ctx, attr);
      const unsigned bufidx = (*num_vbuffers)++;

      init_velement_lowered(vp, velements, &attrib->Format, 0, 0,
                            bufidx, input_to_index[attr]);

      vbuffer[bufidx].is_user_buffer = true;
      vbuffer[bufidx].buffer.user = attrib->Ptr;
      vbuffer[bufidx].buffer_offset = 0;
      vbuffer[bufidx].stride = 0;
   }
}

void
st_update_array(struct st_context *st)
{
   /* vertex program validation must be done before this */
   /* _NEW_PROGRAM, ST_NEW_VS_STATE */
   const struct st_vertex_program *vp = (struct st_vertex_program *)st->vp;
   const struct st_common_variant *vp_variant = st->vp_variant;

   struct pipe_vertex_buffer vbuffer[PIPE_MAX_ATTRIBS];
   unsigned num_vbuffers = 0, first_upload_vbuffer;
   struct pipe_vertex_element velements[PIPE_MAX_ATTRIBS];
   unsigned num_velements;
   bool uses_user_vertex_buffers;

   st->draw_needs_minmax_index = false;

   /* ST_NEW_VERTEX_ARRAYS alias ctx->DriverFlags.NewArray */
   /* Setup arrays */
   st_setup_arrays(st, vp, vp_variant, velements, vbuffer, &num_vbuffers,
                   &uses_user_vertex_buffers);

   /* _NEW_CURRENT_ATTRIB */
   /* Setup current uploads */
   first_upload_vbuffer = num_vbuffers;
   st_setup_current(st, vp, vp_variant, velements, vbuffer, &num_vbuffers);

   /* Set the array into cso */
   num_velements = vp->num_inputs + vp_variant->key.passthrough_edgeflags;

   /* Set vertex buffers and elements. */
   struct cso_context *cso = st->cso_context;
   unsigned unbind_trailing_vbuffers =
      st->last_num_vbuffers > num_vbuffers ?
         st->last_num_vbuffers - num_vbuffers : 0;
   cso_set_vertex_buffers_and_elements(cso, num_velements, velements,
                                       num_vbuffers,
                                       unbind_trailing_vbuffers,
                                       vbuffer, uses_user_vertex_buffers);
   st->last_num_vbuffers = num_vbuffers;

   /* Unreference uploaded buffer resources. */
   for (unsigned i = first_upload_vbuffer; i < num_vbuffers; ++i) {
      pipe_resource_reference(&vbuffer[i].buffer.resource, NULL);
   }
}
