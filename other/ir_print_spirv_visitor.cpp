/*
 * Copyright © 2010 Intel Corporation
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

#include "ir_print_spirv_visitor.h"
#include "compiler/glsl_types.h"
#include "glsl_parser_extras.h"
#include "main/macros.h"
#include "util/hash_table.h"
#include "util/u_string.h"
#include "compiler/spirv/spirv.h"
#include "compiler/spirv/GLSL.std.450.h"

static const unsigned int reflection_float_type[4][4] = {
   GL_FLOAT,      GL_FLOAT_VEC2,   GL_FLOAT_VEC3,   GL_FLOAT_VEC4,
   GL_FLOAT_VEC2, GL_FLOAT_MAT2,   GL_FLOAT_MAT2x3, GL_FLOAT_MAT2x4,
   GL_FLOAT_VEC3, GL_FLOAT_MAT3x2, GL_FLOAT_MAT3,   GL_FLOAT_MAT3x4,
   GL_FLOAT_VEC4, GL_FLOAT_MAT4x2, GL_FLOAT_MAT4x3, GL_FLOAT_MAT4,
};

static const unsigned int reflection_int_type[4] = {
   GL_INT, GL_INT_VEC2, GL_INT_VEC3, GL_INT_VEC4,
};

static const unsigned int storage_mode[] = {
   SpvStorageClassFunction,       // ir_var_auto
   SpvStorageClassUniform,        // ir_var_uniform
   SpvStorageClassWorkgroup,      // ir_var_shader_storage
   SpvStorageClassCrossWorkgroup, // ir_var_shader_shared
   SpvStorageClassInput,          // ir_var_shader_in
   SpvStorageClassOutput,         // ir_var_shader_out
   SpvStorageClassInput,          // ir_var_function_in
   SpvStorageClassOutput,         // ir_var_function_out
   SpvStorageClassWorkgroup,      // ir_var_function_inout
   SpvStorageClassPushConstant,   // ir_var_const_in
   SpvStorageClassGeneric,        // ir_var_system_value
   SpvStorageClassFunction,       // ir_var_temporary
};

static const unsigned int stage_type[] = {
   SpvExecutionModelVertex,
   SpvExecutionModelTessellationControl,
   SpvExecutionModelTessellationEvaluation,
   SpvExecutionModelGeometry,
   SpvExecutionModelFragment,
   SpvExecutionModelGLCompute,
};

binary_buffer::binary_buffer()
{
   u_vector_init(&vector_buffer, sizeof(int), 1024);
}

binary_buffer::~binary_buffer()
{
   u_vector_finish(&vector_buffer);
}

void binary_buffer::opcode(unsigned short length, unsigned short opcode, ...)
{
   push(opcode, length);

   va_list ap;
   va_start(ap, opcode);
   for (unsigned int i = 1; i < length; ++i) {
      push(va_arg(ap, unsigned int));
   }
   va_end(ap);
}

void binary_buffer::text(unsigned short opcode, unsigned int id, const char *text)
{
   unsigned int length = (int)strlen(text);
   unsigned int count = (length + sizeof(int)) / sizeof(int);
   push(opcode, count + 2);
   push(id);
   push(text);
}

void binary_buffer::text(unsigned short opcode, unsigned int id, unsigned int index, const char *text)
{
   unsigned int length = (int)strlen(text);
   unsigned int count = (length + sizeof(int)) / sizeof(int);
   push(opcode, count + 3);
   push(id);
   push(index);
   push(text);
}

void binary_buffer::push(unsigned short low, unsigned short high)
{
   int* buf = (int*)u_vector_add(&vector_buffer);
   (*buf) = (high << 16) | low;
}

void binary_buffer::push(unsigned int value)
{
   int* buf = (int*)u_vector_add(&vector_buffer);
   (*buf) = value;
}

void binary_buffer::push(const char *text)
{
   size_t len = strlen(text);
   while (len >= sizeof(int)) {
      unsigned int value = 0;
      memcpy(&value, text, sizeof(int));
      push(value);
      text += sizeof(int);
      len -= sizeof(int);
   }
   unsigned int value = 0;
   memcpy(&value, text, len);
   push(value);
}

void binary_buffer::push(binary_buffer& buffer)
{
   unsigned int count = buffer.count();
   for (unsigned int i = 0; i < count; ++i) {
      push(buffer[i]);
   }
}

unsigned int binary_buffer::count()
{
   return u_vector_length(&vector_buffer);
}

unsigned int* binary_buffer::data()
{
   return (unsigned int*)u_vector_tail(&vector_buffer);
}

unsigned int& binary_buffer::operator[] (size_t i)
{
   return data()[i];
}

spirv_buffer::spirv_buffer()
{
   memset((char*)this + offsetof(spirv_buffer, memory_begin), 0, offsetof(spirv_buffer, memory_end) - offsetof(spirv_buffer, memory_begin));
}

extern "C" {
void
_mesa_print_spirv(spirv_buffer *f, exec_list *instructions, gl_shader_stage stage, unsigned version, bool es, unsigned binding)
{
   f->shader_stage = stage;
   f->id = 1;
   f->binding_id = binding;

   if (es) {
      if (stage == MESA_SHADER_FRAGMENT) {
         f->precision_float = GLSL_PRECISION_MEDIUM;
         f->precision_int = GLSL_PRECISION_MEDIUM;
      } else {
         f->precision_float = GLSL_PRECISION_HIGH;
         f->precision_int = GLSL_PRECISION_MEDIUM;
      }
   } else {
      f->precision_float = GLSL_PRECISION_NONE;
      f->precision_int = GLSL_PRECISION_NONE;
   }

   // Capability
   f->capability.opcode(2, SpvOpCapability, SpvCapabilityShader);

   // ExtInstImport
   f->ext_inst_import_id = f->id++;
   f->extensions.text(SpvOpExtInstImport, f->ext_inst_import_id, "GLSL.std.450");

   // MemoryModel Logical GLSL450
   f->extensions.opcode(3, SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);

   // spirv visitor
   ir_print_spirv_visitor v(f);

   foreach_in_list(ir_instruction, ir, instructions) {
      ir->accept(&v);
   }

   // Uniform
   unsigned int uniforms_count = f->uniforms.count();
   if (uniforms_count != 0) {
      f->types.push(SpvOpTypeStruct, uniforms_count + 2);
      f->types.push(f->uniform_struct_id);
      for (unsigned int i = 0; i < uniforms_count; ++i) {
         f->types.push(f->uniforms[i]);
      }

      f->types.opcode(4, SpvOpTypePointer, f->uniform_pointer_id, SpvStorageClassUniform, f->uniform_struct_id);
      f->types.opcode(4, SpvOpVariable, f->uniform_pointer_id, f->uniform_id, SpvStorageClassUniform);
   }

   // gl_PerVertex
   unsigned int per_vertices_count = f->per_vertices.count();
   if (per_vertices_count != 0) {
      f->types.push(SpvOpTypeStruct, per_vertices_count + 2);
      f->types.push(f->gl_per_vertex_id);
      for (unsigned int i = 0; i < per_vertices_count; ++i) {
         f->types.push(f->per_vertices[i]);
      }
   }

   // Header - Mesa-IR/SPIR-V Translator
   unsigned int bound_id = f->id++;
   f->push(SpvMagicNumber);
   f->push(0x00010000);
   f->push(0x00100000);
   f->push(bound_id);
   f->push(0u);

   // Capability / Extension
   f->push(f->capability);
   f->push(f->extensions);

   // EntryPoint Fragment 4 "main" 20 22 37 43 46 49
   unsigned int inouts_count = f->inouts.count();
   f->push(SpvOpEntryPoint, inouts_count + 5);
   f->push(stage_type[stage]);
   f->push(f->main_id);
   f->push("main");
   for (unsigned int i = 0; i < inouts_count; ++i) {
      f->push(f->inouts[i]);
   }

   // ExecutionMode 4 OriginUpperLeft
   if (stage == MESA_SHADER_FRAGMENT) {
      f->opcode(3, SpvOpExecutionMode, f->main_id, SpvExecutionModeOriginUpperLeft);
   }

   // Source ESSL 300
   f->opcode(3, SpvOpSource, es ? SpvSourceLanguageESSL : SpvSourceLanguageGLSL, version);

   // Other
   f->push(f->names);
   f->push(f->decorates);
   f->push(f->types);
   f->push(f->builtins);
   f->push(f->functions);
}

} /* extern "C" */

ir_print_spirv_visitor::ir_print_spirv_visitor(spirv_buffer *f)
   : f(f)
{
   indentation = 0;
   parameter_number = 0;
   name_number = 0;
   printable_names =
      _mesa_hash_table_create(NULL, _mesa_hash_pointer, _mesa_key_pointer_equal);
   symbols = _mesa_symbol_table_ctor();
   mem_ctx = ralloc_context(NULL);
}

ir_print_spirv_visitor::~ir_print_spirv_visitor()
{
   _mesa_hash_table_destroy(printable_names, NULL);
   _mesa_symbol_table_dtor(symbols);
   ralloc_free(mem_ctx);
}

unsigned int
ir_print_spirv_visitor::unique_name(ir_variable *var)
{
   /* var->name can be NULL in function prototypes when a type is given for a
    * parameter but no name is given.  In that case, just return an empty
    * string.  Don't worry about tracking the generated name in the printable
    * names hash because this is the only scope where it can ever appear.
    */
   if (var->name == NULL) {
      return parameter_number++;
   }

   /* Do we already have a name for this variable? */
   struct hash_entry *entry =
      _mesa_hash_table_search(this->printable_names, var);

   if (entry != NULL) {
      return (unsigned int)(intptr_t)entry->data;
   }

   /* If there's no conflict, just use the original name */
   const char *name = NULL;
   if (_mesa_symbol_table_find_symbol(this->symbols, var->name) == NULL) {
      name = var->name;
   } else {
      name = ralloc_asprintf(this->mem_ctx, "%s_%u", var->name, ++name_number);
   }

   unsigned int name_id = f->id++;
   f->names.text(SpvOpName, name_id, name);

   _mesa_hash_table_insert(this->printable_names, var, (void *)(intptr_t)name_id);
   _mesa_symbol_table_add_symbol(this->symbols, name, var);

   var->ir_pointer = name_id;
   return name_id;
}

void
ir_print_spirv_visitor::visit(ir_rvalue *)
{
   //fprintf(f, "error");
}

unsigned int
ir_print_spirv_visitor::visit_type(const struct glsl_type *type)
{
   if (type->is_array()) {
      unsigned int vector_id = f->id++;
      unsigned int base_type_id = visit_type(type->fields.array);
      unsigned int array_size_id = visit_constant_value(type->array_size());

      f->types.opcode(4, SpvOpTypeArray, vector_id, base_type_id, array_size_id);
      f->decorates.opcode(4, SpvOpDecorate, vector_id, SpvDecorationArrayStride, type->fields.array->std430_array_stride(false));

      return vector_id;
   } else if (type->is_sampler()) {
      unsigned int sampled_image_id = f->sampler_id[type->sampler_dimensionality];
      if (sampled_image_id == 0) {
         unsigned int image_id = f->id++;
         unsigned int type_id = visit_type(glsl_type::float_type);
         unsigned int dim_id = SpvDim1D;
         switch (type->sampler_dimensionality) {
            case GLSL_SAMPLER_DIM_1D:       dim_id = SpvDim1D;          break;
            case GLSL_SAMPLER_DIM_2D:       dim_id = SpvDim2D;          break;
            case GLSL_SAMPLER_DIM_3D:       dim_id = SpvDim3D;          break;
            case GLSL_SAMPLER_DIM_CUBE:     dim_id = SpvDimCube;        break;
            case GLSL_SAMPLER_DIM_RECT:     dim_id = SpvDimRect;        break;
            case GLSL_SAMPLER_DIM_BUF:      dim_id = SpvDimBuffer;      break;
            case GLSL_SAMPLER_DIM_EXTERNAL: dim_id = SpvDim1D;          break;// TODO
            case GLSL_SAMPLER_DIM_MS:       dim_id = SpvDim1D;          break;// TODO
            case GLSL_SAMPLER_DIM_SUBPASS:  dim_id = SpvDimSubpassData; break;
         }
         sampled_image_id = f->id++;

         f->types.opcode(9, SpvOpTypeImage, image_id, type_id, dim_id, 0, 0, 0, 1, SpvImageFormatUnknown);
         f->types.opcode(3, SpvOpTypeSampledImage, sampled_image_id, image_id);

         f->sampler_id[type->sampler_dimensionality] = sampled_image_id;
      }
      return sampled_image_id;
   } else if (type->is_boolean()) {
      unsigned int bool_id = f->bool_id;
      if (bool_id == 0) {
         bool_id = f->id++;

         f->types.opcode(2, SpvOpTypeBool, bool_id);

         f->bool_id = bool_id;
      }
      return bool_id;
   } else if (type->is_void()) {
      unsigned int void_id = f->void_id;
      if (void_id == 0) {
         void_id = f->id++;

         f->types.opcode(2, SpvOpTypeVoid, void_id);

         f->void_id = void_id;
      }
      return void_id;
   }

   // Scalar
   unsigned int* vector_ids;
   unsigned int scalar_id;
   if (type->is_float()) {
      vector_ids = f->float_id[type->vector_elements];
      scalar_id = f->float_id[1][1];
      if (scalar_id == 0) {
         scalar_id = f->id++;

         f->types.opcode(3, SpvOpTypeFloat, scalar_id, 32);

         f->float_id[1][1] = scalar_id;
      }
   } else if (type->is_integer()) {
      vector_ids = f->int_id[type->vector_elements];
      scalar_id = f->int_id[1][1];
      if (scalar_id == 0) {
         scalar_id = f->id++;

         f->types.opcode(4, SpvOpTypeInt, scalar_id, 32, true);

         f->int_id[1][1] = scalar_id;
      }
   } else {
      return 0;
   }

   // Vector
   unsigned int vector_id = vector_ids[1];
   if (vector_id == 0) {
      vector_id = f->id++;

      f->types.opcode(4, SpvOpTypeVector, vector_id, scalar_id, type->vector_elements);

      vector_ids[1] = vector_id;
   }

   // Matrix
   unsigned int matrix_id = vector_ids[type->matrix_columns];
   if (matrix_id == 0) {
      matrix_id = f->id++;

      f->types.opcode(4, SpvOpTypeMatrix, matrix_id, vector_id, type->matrix_columns);

      vector_ids[type->matrix_columns] = matrix_id;
   }

   return matrix_id;
}

unsigned int
ir_print_spirv_visitor::visit_type_pointer(const struct glsl_type *type, unsigned int mode, unsigned int pointer_to)
{
   unsigned int storage_class = storage_mode[mode];

   if (type->is_array()) {
      return visit_type_pointer(type->fields.array, mode, pointer_to);
   } else if (type->is_sampler()) {
      unsigned int pointer_id = f->pointer_sampler_id[type->sampler_dimensionality];
      if (pointer_id == 0) {
         pointer_id = f->id++;

         f->types.opcode(4, SpvOpTypePointer, pointer_id, SpvStorageClassUniformConstant, pointer_to);

         f->pointer_sampler_id[type->sampler_dimensionality] = pointer_id;
      }
      return pointer_id;
   } else if (type->is_boolean()) {
      unsigned int pointer_id = f->pointer_bool_id[storage_class];
      if (pointer_id == 0) {
         pointer_id = f->id++;

         f->types.opcode(4, SpvOpTypePointer, pointer_id, storage_class, pointer_to);

         f->pointer_bool_id[storage_class] = pointer_id;
      }
      return pointer_id;
   }

   unsigned int* pointer_ids;
   if (type->is_float()) {
      pointer_ids = f->pointer_float_id[storage_class][type->vector_elements];
   } else if (type->is_integer()) {
      pointer_ids = f->pointer_int_id[storage_class][type->vector_elements];
   } else {
      return 0;
   }

   // Matrix
   unsigned int pointer_id = pointer_ids[type->matrix_columns];
   if (pointer_id == 0) {
      pointer_id = f->id++;

      f->types.opcode(4, SpvOpTypePointer, pointer_id, storage_class, pointer_to);

      pointer_ids[type->matrix_columns] = pointer_id;
   }

   return pointer_id;
}

unsigned int
ir_print_spirv_visitor::visit_constant_value(float value)
{
   ir_constant ir_constant_value(value);
   ir_constant_value.ir_value = 0;
   visit(&ir_constant_value);

   return ir_constant_value.ir_value;
}

unsigned int
ir_print_spirv_visitor::visit_constant_value(int value)
{
   ir_constant ir_constant_value(value);
   ir_constant_value.ir_value = 0;
   visit(&ir_constant_value);

   return ir_constant_value.ir_value;
}

unsigned int
ir_print_spirv_visitor::visit_constant_value(unsigned int value)
{
   ir_constant ir_constant_value(value);
   ir_constant_value.ir_value = 0;
   visit(&ir_constant_value);

   return ir_constant_value.ir_value;
}

void
ir_print_spirv_visitor::visit_value(ir_rvalue *ir)
{
   if (ir->ir_value == 0) {
      if (ir->ir_pointer == 0 && ir->ir_uniform) {
         unsigned int uniform_type = visit_type(ir->type);
         unsigned int type_pointer_id = visit_type_pointer(ir->type, ir_var_uniform, uniform_type);
         unsigned int pointer_id = f->id++;
         unsigned int index_id = visit_constant_value(ir->ir_uniform - 1);

         f->functions.opcode(5, SpvOpAccessChain, type_pointer_id, pointer_id, f->uniform_id, index_id);

         ir->ir_pointer = pointer_id;
      }
      if (ir->ir_pointer != 0) {
         unsigned int type_id = visit_type(ir->type);
         unsigned int value_id = f->id++;

         f->functions.opcode(4, SpvOpLoad, type_id, value_id, ir->ir_pointer);
         visit_precision(ir->ir_value, ir->type->base_type, GLSL_PRECISION_NONE);

         ir->ir_value = value_id;
      }
   }
}

void
ir_print_spirv_visitor::visit_precision(unsigned int id, unsigned int type, unsigned int precision)
{
   switch (type) {
   default:
      break;
   case GLSL_TYPE_UINT:
   case GLSL_TYPE_INT:
      if (precision == GLSL_PRECISION_MEDIUM || f->precision_int == GLSL_PRECISION_MEDIUM) {
         f->decorates.opcode(3, SpvOpDecorate, id, SpvDecorationRelaxedPrecision);
      }
      break;
   case GLSL_TYPE_FLOAT:
      if (precision == GLSL_PRECISION_MEDIUM || f->precision_float == GLSL_PRECISION_MEDIUM) {
         f->decorates.opcode(3, SpvOpDecorate, id, SpvDecorationRelaxedPrecision);
      }
      break;
   }
}

void
ir_print_spirv_visitor::visit(ir_variable *ir)
{
   if (is_gl_identifier(ir->name))
      return;

   unsigned int type_id = visit_type(ir->type);

   if (ir->data.mode == ir_var_uniform) {
      if (ir->type->is_sampler()) {
         if (ir->data.explicit_binding) {
           ir->ir_initialized = ir->data.binding;
           if (f->binding_id <= ir->data.binding)
              f->binding_id = ir->data.binding + 1;
         } else {
           ir->ir_initialized = f->binding_id++;
         }
      } else if (ir->type->is_interface()) {
         const glsl_type* interface_type = ir->get_interface_type();
         unsigned int interface_name_id = f->id++;

         f->names.text(SpvOpName, interface_name_id, interface_type->name);
         f->decorates.opcode(3, SpvOpDecorate, interface_name_id, SpvDecorationBlock);

         unsigned int offset = 0;
         binary_buffer ids;
         for (unsigned int i = 0; i < interface_type->length; ++i) {
            glsl_struct_field& field = interface_type->fields.structure[i];

            f->names.text(SpvOpMemberName, interface_name_id, i, field.name);
            f->decorates.opcode(5, SpvOpMemberDecorate, interface_name_id, i, SpvDecorationOffset, offset);
            ids.push(visit_type(field.type));

            offset += field.type->std430_array_stride(false);
         }

         unsigned int ids_count = ids.count();
         f->types.push(SpvOpTypeStruct, ids_count + 2);
         f->types.push(interface_name_id);
         for (unsigned int i = 0; i < ids_count; ++i)
            f->types.push(ids[i]);

         unsigned int pointer_id = f->id++;
         unsigned int name_id = unique_name(ir);

         f->types.opcode(4, SpvOpTypePointer, pointer_id, SpvStorageClassPushConstant, interface_name_id);
         f->types.opcode(4, SpvOpVariable, pointer_id, name_id, SpvStorageClassPushConstant);

         ir->ir_pointer = pointer_id;
         ir->ir_value = name_id;
      } else {
         if (f->uniform_struct_id == 0) {
            unsigned int uniform_struct_id = f->id++;
            unsigned int uniform_pointer_id = f->id++;
            unsigned int uniform_id = f->id++;
            unsigned int binding_id = f->binding_id++;
            char block_name[64] = {};
            snprintf(block_name, sizeof(block_name), "Global%d", binding_id);

            f->names.text(SpvOpName, uniform_struct_id, block_name);
            f->names.text(SpvOpName, uniform_id, "");
            f->decorates.opcode(3, SpvOpDecorate, uniform_struct_id, SpvDecorationBlock);
            f->decorates.opcode(4, SpvOpDecorate, uniform_id, SpvDecorationDescriptorSet, 0u);
            f->decorates.opcode(4, SpvOpDecorate, uniform_id, SpvDecorationBinding, binding_id);

            f->uniform_struct_id = uniform_struct_id;
            f->uniform_pointer_id = uniform_pointer_id;
            f->uniform_id = uniform_id;
         }

         f->names.text(SpvOpMemberName, f->uniform_struct_id, f->uniforms.count(), ir->name);
         f->decorates.opcode(5, SpvOpMemberDecorate, f->uniform_struct_id, f->uniforms.count(), SpvDecorationOffset, f->uniform_offset);

         if (ir->type->is_matrix()) {
            f->decorates.opcode(4, SpvOpMemberDecorate, f->uniform_struct_id, f->uniforms.count(), SpvDecorationColMajor);
            f->decorates.opcode(5, SpvOpMemberDecorate, f->uniform_struct_id, f->uniforms.count(), SpvDecorationMatrixStride, ir->type->std430_array_stride(false));
         }

         ir->ir_uniform = f->uniforms.count();
         f->uniforms.push(type_id);
         f->uniform_offset += ir->type->std430_array_stride(false);
      }
   } else {
      unsigned int pointer_id = visit_type_pointer(ir->type, ir->data.mode, type_id);
      unsigned int name_id = unique_name(ir);
      unsigned int storage_class = storage_mode[ir->data.mode];

      if (ir->data.mode == ir_var_auto || ir->data.mode == ir_var_temporary) {
         f->functions.opcode(4, SpvOpVariable, pointer_id, name_id, storage_class);
         visit_precision(name_id, ir->type->base_type, ir->data.precision);
      } else {
         f->types.opcode(4, SpvOpVariable, pointer_id, name_id, storage_class);
      }

      if (ir->data.mode == ir_var_shader_in || ir->data.mode == ir_var_shader_out) {
         f->inouts.push(name_id);

         unsigned int loc_id;
         if (ir->data.explicit_location) {
            switch (f->shader_stage) {
            case MESA_SHADER_VERTEX:
               loc_id = (ir->data.mode == ir_var_shader_in)
                  ? (ir->data.location - VERT_ATTRIB_GENERIC0)
                  : (ir->data.location - VARYING_SLOT_VAR0);
               break;

            case MESA_SHADER_FRAGMENT:
               loc_id = (ir->data.mode == ir_var_shader_out)
                  ? (ir->data.location - FRAG_RESULT_DATA0)
                  : (ir->data.location - VARYING_SLOT_VAR0);
               break;

            default:
               unreachable("Unexpected shader type");
               break;
            }
         }
         else {
            loc_id = ir->data.mode == ir_var_shader_in ? f->input_loc++ : f->output_loc++;
         }

         f->decorates.opcode(4, SpvOpDecorate, name_id, SpvDecorationLocation, loc_id);
      }
   }
}

void
ir_print_spirv_visitor::visit(ir_function_signature *ir)
{
   // TypeVoid
   unsigned int type_id;
   if (ir->return_type->base_type == GLSL_TYPE_VOID) {
      if (f->void_id == 0) {
         f->void_id = f->id++;
         f->types.opcode(2, SpvOpTypeVoid, f->void_id);
      }
      type_id = f->void_id;
   } else {
      return;
   }

   // TypeFunction
   if (f->void_function_id == 0) {
      f->void_function_id = f->id++;
      f->types.opcode(3, SpvOpTypeFunction, f->void_function_id, type_id);
   }

   // TypeName
   unsigned int function_name_id = 0;
   if (strcasecmp(ir->function_name(), "main") == 0) {
      function_name_id = f->main_id = f->id++;
   } else {
      function_name_id = f->id++;
   }
   f->names.text(SpvOpName, function_name_id, ir->function_name());
   f->functions.opcode(5, SpvOpFunction, type_id, function_name_id, SpvFunctionControlMaskNone, f->void_function_id);

   // Label
   unsigned int label_id = f->id++;
   f->functions.opcode(2, SpvOpLabel, label_id);

   foreach_in_list(ir_variable, inst, &ir->parameters) {
      inst->accept(this);
   }

   foreach_in_list(ir_instruction, inst, &ir->body) {
      inst->accept(this);
   }

   // Return
   f->functions.opcode(1, SpvOpReturn);
   f->functions.opcode(1, SpvOpFunctionEnd);
}

void
ir_print_spirv_visitor::visit(ir_function *ir)
{
   foreach_in_list(ir_function_signature, sig, &ir->signatures) {
      sig->accept(this);
   }
}

void
ir_print_spirv_visitor::visit(ir_expression *ir)
{
   unsigned int operands[4] = {};

   for (unsigned int i = 0; i < ir->num_operands; ++i) {
      if (ir->operands[i] == NULL)
         return;
      ir->operands[i]->accept(this);
      visit_value(ir->operands[i]);
      operands[i] = ir->operands[i]->ir_value;
   }

   unsigned int type_id = visit_type(ir->type);
   bool float_type;
   bool signed_type;
   switch (ir->type->base_type)
   {
   default:
   case GLSL_TYPE_FLOAT:
   case GLSL_TYPE_DOUBLE:
      float_type = true;
      signed_type = true;
      break;
   case GLSL_TYPE_INT:
   case GLSL_TYPE_INT64:
      float_type = false;
      signed_type = true;
      break;
   case GLSL_TYPE_UINT:
   case GLSL_TYPE_UINT64:
      float_type = false;
      signed_type = false;
      break;
   }

   if (ir->operation == ir_unop_saturate) {
      if (ir->num_operands != 1) {
         unreachable("unknown number of operands");
         return;
      }

      unsigned int value_id = f->id++;
      unsigned int opcode = float_type ? GLSLstd450FClamp : signed_type ? GLSLstd450SClamp : GLSLstd450UClamp;
      unsigned int zero_id = visit_constant_value(0.0f);
      unsigned int one_id = visit_constant_value(1.0f);

      f->functions.opcode(8, SpvOpExtInst, type_id, value_id, f->ext_inst_import_id, opcode, operands[0], zero_id, one_id);

      ir->ir_value = value_id;
   } else if (ir->operation == ir_binop_mul) {
      if (ir->num_operands != 2) {
         unreachable("unknown number of operands");
         return;
      }

      unsigned int value_id = f->id++;
      unsigned short opcode;
      if (ir->operands[0]->type->is_scalar()) {
         if (ir->operands[1]->type->is_scalar()) {
            opcode = float_type ? SpvOpFMul : SpvOpIMul;
         } else if (ir->operands[1]->type->is_vector()) {
            opcode = SpvOpVectorTimesScalar;
            operands[0] = ir->operands[1]->ir_value;
            operands[1] = ir->operands[0]->ir_value;
         } else if (ir->operands[1]->type->is_matrix()) {
            opcode = SpvOpMatrixTimesScalar;
            operands[0] = ir->operands[1]->ir_value;
            operands[1] = ir->operands[0]->ir_value;
         } else {
            unreachable("unknown multiply operation");
         }
      } else if (ir->operands[0]->type->is_vector()) {
         if (ir->operands[1]->type->is_scalar()) {
            opcode = SpvOpVectorTimesScalar;
         } else if (ir->operands[1]->type->is_vector()) {
            opcode = float_type ? SpvOpFMul : SpvOpIMul;
         } else if (ir->operands[1]->type->is_matrix()) {
            opcode = SpvOpVectorTimesMatrix;
         } else {
            unreachable("unknown multiply operation");
         }
      } else if (ir->operands[0]->type->is_matrix()) {
         if (ir->operands[1]->type->is_scalar()) {
            opcode = SpvOpMatrixTimesScalar;
         } else if (ir->operands[1]->type->is_vector()) {
            opcode = SpvOpMatrixTimesVector;
         } else if (ir->operands[1]->type->is_matrix()) {
            opcode = SpvOpMatrixTimesMatrix;
         } else {
            unreachable("unknown multiply operation");
         }
      } else {
         unreachable("unknown multiply operation");
      }
      f->functions.opcode(5, opcode, type_id, value_id, operands[0], operands[1]);

      ir->ir_value = value_id;
   } else if (ir->operation >= ir_unop_bit_not && ir->operation <= ir_last_unop) {
      if (ir->num_operands != 1) {
         unreachable("unknown number of operands");
         return;
      }

      unsigned int value_id = f->id++;
      unsigned short opcode;
      switch (ir->operation) {
      default:
         unreachable("unknown operation");
      case ir_unop_neg:
         opcode = float_type ? SpvOpFNegate : SpvOpSNegate;

         f->functions.opcode(4, opcode, type_id, value_id, operands[0]);
         break;
      case ir_unop_rcp: {
         opcode = float_type ? SpvOpFDiv : signed_type ? SpvOpSDiv : SpvOpUDiv;
         unsigned int one_id = visit_constant_value(1.0f);

         f->functions.opcode(5, opcode, type_id, value_id, one_id, operands[0]);
         break;
      }
      case ir_unop_abs:
      case ir_unop_sign:
      case ir_unop_rsq:
      case ir_unop_sqrt:
      case ir_unop_exp:
      case ir_unop_log:
      case ir_unop_exp2:
      case ir_unop_log2:
      case ir_unop_trunc:
      case ir_unop_ceil:
      case ir_unop_floor:
      case ir_unop_fract:
      case ir_unop_round_even:
      case ir_unop_sin:
      case ir_unop_cos:
         switch (ir->operation) {
         default:
         case ir_unop_abs:        opcode = float_type ? GLSLstd450FAbs  : GLSLstd450SAbs;  break;
         case ir_unop_sign:       opcode = float_type ? GLSLstd450FSign : GLSLstd450SSign; break;
         case ir_unop_rsq:        opcode = GLSLstd450InverseSqrt;                          break;
         case ir_unop_sqrt:       opcode = GLSLstd450Sqrt;                                 break;
         case ir_unop_exp:        opcode = GLSLstd450Exp;                                  break;
         case ir_unop_log:        opcode = GLSLstd450Log;                                  break;
         case ir_unop_exp2:       opcode = GLSLstd450Exp2;                                 break;
         case ir_unop_log2:       opcode = GLSLstd450Log2;                                 break;
         case ir_unop_trunc:      opcode = GLSLstd450Trunc;                                break;
         case ir_unop_ceil:       opcode = GLSLstd450Ceil;                                 break;
         case ir_unop_floor:      opcode = GLSLstd450Floor;                                break;
         case ir_unop_fract:      opcode = GLSLstd450Fract;                                break;
         case ir_unop_round_even: opcode = GLSLstd450RoundEven;                            break;
         case ir_unop_sin:        opcode = GLSLstd450Sin;                                  break;
         case ir_unop_cos:        opcode = GLSLstd450Cos;                                  break;
         }
         f->functions.opcode(6, SpvOpExtInst, type_id, value_id, f->ext_inst_import_id, opcode, operands[0]);
         break;
      case ir_unop_f2i:
      case ir_unop_f2u:
      case ir_unop_i2f:
      case ir_unop_u2f:
      case ir_unop_i2u:
      case ir_unop_u2i:
         switch (ir->operation) {
         default:
         case ir_unop_f2i: opcode = SpvOpConvertFToS; break;
         case ir_unop_f2u: opcode = SpvOpConvertFToU; break;
         case ir_unop_i2f: opcode = SpvOpConvertSToF; break;
         case ir_unop_u2f: opcode = SpvOpConvertUToF; break;
         case ir_unop_i2u: opcode = SpvOpUConvert;    break;
         case ir_unop_u2i: opcode = SpvOpSConvert;    break;
         }
         f->functions.opcode(4, opcode, type_id, value_id, operands[0]);
         break;
      }

      ir->ir_value = value_id;
   } else if (ir->operation >= ir_binop_add && ir->operation <= ir_last_binop) {
      if (ir->num_operands != 2) {
         unreachable("unknown number of operands");
         return;
      }

      switch (ir->operation) {
      default:
         break;
      case ir_binop_add:
      case ir_binop_sub:
      case ir_binop_div:
      case ir_binop_mod:
         for (unsigned int i = 0; i < 2; ++i) {
            if (ir->operands[i]->type == ir->type) {
               operands[i] = ir->operands[i]->ir_value;
            } else if (ir->operands[i]->type->components() == 1) {
               operands[i] = f->id++;
               f->functions.push(SpvOpCompositeConstruct, ir->type->components() + 3);
               f->functions.push(type_id);
               f->functions.push(operands[i]);
               for (unsigned int j = 0; j < ir->type->components(); ++j) {
                  f->functions.push(ir->operands[i]->ir_value);
               }
            } else {
               unreachable("operands must match result or be scalar");
            }
         }
      }

      unsigned int value_id = f->id++;
      unsigned short opcode;
      switch (ir->operation) {
      default:
         unreachable("unknown operation");
      case ir_binop_add:
      case ir_binop_sub:
      case ir_binop_div:
      case ir_binop_mod:
      case ir_binop_less:
      case ir_binop_gequal:
      case ir_binop_equal:
      case ir_binop_nequal:
      case ir_binop_dot:
         switch (ir->operation) {
         default:
         case ir_binop_add:     opcode = float_type ? SpvOpFAdd                 : SpvOpIAdd;                                                     break;
         case ir_binop_sub:     opcode = float_type ? SpvOpFSub                 : SpvOpISub;                                                     break;
         case ir_binop_div:     opcode = float_type ? SpvOpFDiv                 : signed_type ? SpvOpSDiv              : SpvOpUDiv;              break;
         case ir_binop_mod:     opcode = float_type ? SpvOpFMod                 : signed_type ? SpvOpSMod              : SpvOpUMod;              break;
         case ir_binop_less:    opcode = float_type ? SpvOpFOrdLessThan         : signed_type ? SpvOpSLessThan         : SpvOpULessThan;         break;
         case ir_binop_gequal:  opcode = float_type ? SpvOpFOrdGreaterThanEqual : signed_type ? SpvOpSGreaterThanEqual : SpvOpUGreaterThanEqual; break;
         case ir_binop_equal:   opcode = float_type ? SpvOpFOrdEqual            : SpvOpIEqual;                                                   break;
         case ir_binop_nequal:  opcode = float_type ? SpvOpFOrdNotEqual         : SpvOpINotEqual;                                                break;
         case ir_binop_dot:     opcode = SpvOpDot;                                                                                               break;
         }
         f->functions.opcode(5, opcode, type_id, value_id, operands[0], operands[1]);
         break;
      case ir_binop_min:
      case ir_binop_max:
      case ir_binop_pow:
      case ir_binop_ldexp:
         switch (ir->operation) {
         default:
         case ir_binop_min:   opcode = float_type ? GLSLstd450FMin : signed_type ? GLSLstd450SMin : GLSLstd450UMin; break;
         case ir_binop_max:   opcode = float_type ? GLSLstd450FMax : signed_type ? GLSLstd450SMax : GLSLstd450UMax; break;
         case ir_binop_pow:   opcode = GLSLstd450Pow;                                                               break;
         case ir_binop_ldexp: opcode = GLSLstd450Ldexp;                                                             break;
         }
         f->functions.opcode(7, SpvOpExtInst, type_id, value_id, f->ext_inst_import_id, opcode, operands[0], operands[1]);
         break;
      }

      ir->ir_value = value_id;
   } else if (ir->operation >= ir_triop_fma && ir->operation <= ir_last_triop) {
      if (ir->num_operands != 3) {
         unreachable("unknown number of operands");
         return;
      }

      switch (ir->operation) {
      default:
         break;
      case ir_triop_fma:
      case ir_triop_lrp:
         for (unsigned int i = 0; i < 3; ++i) {
            if (ir->operands[i]->type == ir->type) {
               operands[i] = ir->operands[i]->ir_value;
            } else if (ir->operands[i]->type->components() == 1) {
               operands[i] = f->id++;
               f->functions.push(SpvOpCompositeConstruct, ir->type->components() + 3);
               f->functions.push(type_id);
               f->functions.push(operands[i]);
               for (unsigned int j = 0; j < ir->type->components(); ++j) {
                  f->functions.push(ir->operands[i]->ir_value);
               }
            } else {
               unreachable("operands must match result or be scalar");
            }
         }
      }

      unsigned int value_id = f->id++;
      unsigned short opcode;
      switch (ir->operation) {
      default:
         unreachable("unknown operation");
      case ir_triop_fma:
      case ir_triop_lrp:
         switch (ir->operation) {
         default:
         case ir_triop_fma: opcode = GLSLstd450Fma;                                break;
         case ir_triop_lrp: opcode = float_type ? GLSLstd450FMix : GLSLstd450IMix; break;
         }
         f->functions.opcode(8, SpvOpExtInst, type_id, value_id, f->ext_inst_import_id, opcode, operands[0], operands[1], operands[2]);
         break;
      }

      ir->ir_value = value_id;
   }

   visit_precision(ir->ir_value, ir->type->base_type, GLSL_PRECISION_NONE);
}

void
ir_print_spirv_visitor::visit(ir_texture *ir)
{
   if (ir->op == ir_samples_identical) {
      ir->sampler->accept(this);
      ir->coordinate->accept(this);
      return;
   }

   ir->sampler->accept(this);
   visit_value(ir->sampler);

   unsigned int coordinate_id = 0;
   unsigned int image_operand_ids[16] = {};

   if (ir->op != ir_txs && ir->op != ir_query_levels && ir->op != ir_texture_samples) {
      ir->coordinate->accept(this);
      visit_value(ir->coordinate);
      coordinate_id = ir->coordinate->ir_value;

      if (ir->offset) {
         ir->offset->accept(this);
         visit_value(ir->offset);

         image_operand_ids[SpvImageOperandsBiasShift] = ir->offset->ir_value;
      }
   }

   if (ir->op != ir_txf && ir->op != ir_txf_ms && ir->op != ir_txs && ir->op != ir_tg4 && ir->op != ir_query_levels && ir->op != ir_texture_samples) {
      if (ir->projector) {
         ir->projector->accept(this);
         visit_value(ir->projector);

         unsigned int components[4];
         unsigned int coordinate_component = ir->coordinate->type->components();
         if (coordinate_component == 1) {
            components[0] = coordinate_id;
         }
         else {
            for (unsigned int i = 0; i < coordinate_component; ++i) {
               unsigned int type_id = visit_type(glsl_type::float_type);
               unsigned int id = f->id++;
               f->functions.opcode(5, SpvOpCompositeExtract, type_id, id, coordinate_id, i);

               components[i] = id;
            }
         }

         const glsl_type* combined_type;
         switch (coordinate_component)
         {
         case 1:
            combined_type = glsl_type::vec2_type;
            break;
         case 2:
            combined_type = glsl_type::vec3_type;
            break;
         case 3:
            combined_type = glsl_type::vec4_type;
            break;
         default:
            unreachable("unknown component");
         }

         coordinate_id = f->id++;
         f->functions.push(SpvOpCompositeConstruct, coordinate_component + 4);
         f->functions.push(visit_type(combined_type));
         f->functions.push(coordinate_id);
         for (unsigned int i = 0; i < coordinate_component; ++i) {
            f->functions.push(components[i]);
         }
         f->functions.push(ir->projector->ir_value);
      }
   }

   unsigned short opcode = ir->projector ? SpvOpImageSampleProjImplicitLod : SpvOpImageSampleImplicitLod;
   unsigned int component_id = 0;
   switch (ir->op)
   {
   case ir_tex:
   case ir_lod:
   case ir_query_levels:
   case ir_texture_samples:
      break;
   case ir_txb:
      ir->lod_info.bias->accept(this);
      visit_value(ir->lod_info.bias);

      // Only valid with implicit-lod instructions
      opcode = ir->projector ? SpvOpImageSampleProjImplicitLod : SpvOpImageSampleImplicitLod;
      image_operand_ids[SpvImageOperandsBiasShift] = ir->lod_info.bias->ir_value;
      break;
   case ir_txl:
   case ir_txf:
   case ir_txs:
      ir->lod_info.lod->accept(this);
      visit_value(ir->lod_info.lod);

      // Only valid with explicit-lod instructions
      opcode = ir->projector ? SpvOpImageSampleProjExplicitLod : SpvOpImageSampleExplicitLod;
      image_operand_ids[SpvImageOperandsLodShift] = ir->lod_info.lod->ir_value;
      break;
   case ir_txf_ms:
      ir->lod_info.sample_index->accept(this);
      visit_value(ir->lod_info.sample_index);

      opcode = SpvOpImageFetch;
      image_operand_ids[SpvImageOperandsSampleShift] = ir->lod_info.sample_index->ir_value;
      break;
   case ir_txd:
      ir->lod_info.grad.dPdx->accept(this);
      ir->lod_info.grad.dPdy->accept(this);
      visit_value(ir->lod_info.grad.dPdx);
      visit_value(ir->lod_info.grad.dPdy);

      // Only valid with explicit-lod instructions
      opcode = ir->projector ? SpvOpImageSampleProjExplicitLod : SpvOpImageSampleExplicitLod;
      image_operand_ids[SpvImageOperandsGradShift] = ir->lod_info.grad.dPdx->ir_value;
      image_operand_ids[SpvImageOperandsConstOffsetShift] = ir->lod_info.grad.dPdy->ir_value;
      break;
   case ir_tg4:
      ir->lod_info.component->accept(this);
      visit_value(ir->lod_info.component);

      opcode = SpvOpImageGather;
      component_id = ir->lod_info.component->ir_value;
      break;
   case ir_samples_identical:
      unreachable("ir_samples_identical was already handled");
   };

   unsigned int image_operand_type = 0;
   unsigned int image_operand_count = 0;
   for (unsigned int i = 0; i < 16; ++i) {
      unsigned int id = image_operand_ids[i];
      if (id == 0)
         continue;
      if (i != SpvImageOperandsConstOffsetShift)
         image_operand_type |= (1 << i);
      image_operand_count++;
   }

   unsigned int type_id = visit_type(ir->type);
   unsigned int result_id = f->id++;
   switch (ir->op)
   {
   default:
      if (image_operand_type) {
         f->functions.push(opcode, image_operand_count + 6);
         f->functions.push(type_id);
         f->functions.push(result_id);
         f->functions.push(ir->sampler->ir_value);
         f->functions.push(coordinate_id);
         f->functions.push(image_operand_type);
         for (unsigned int i = 0; i < 16; ++i) {
            unsigned int id = image_operand_ids[i];
            if (id == 0)
               continue;
            f->functions.push(id);
         }
      } else {
         f->functions.opcode(5, opcode, type_id, result_id, ir->sampler->ir_value, coordinate_id);
      }
      ir->ir_value = result_id;
      break;
   case ir_txf_ms:
      f->functions.push(SpvOpImageFetch, image_operand_count + 6);
      f->functions.push(type_id);
      f->functions.push(result_id);
      f->functions.push(ir->sampler->ir_value);
      f->functions.push(coordinate_id);
      f->functions.push(image_operand_type);
      for (unsigned int i = 0; i < 16; ++i) {
         unsigned int id = image_operand_ids[i];
         if (id == 0)
            continue;
         f->functions.push(id);
      }
      break;
   case ir_txs:
      f->functions.opcode(5, SpvOpImageQuerySizeLod, type_id, result_id, ir->sampler->ir_value, ir->lod_info.lod->ir_value);
      break;
   case ir_lod:
      f->functions.opcode(5, SpvOpImageQueryLod, type_id, result_id, ir->sampler->ir_value, coordinate_id);
      break;
   case ir_tg4:
      f->functions.push(SpvOpImageGather, image_operand_count + 7);
      f->functions.push(type_id);
      f->functions.push(result_id);
      f->functions.push(ir->sampler->ir_value);
      f->functions.push(coordinate_id);
      f->functions.push(component_id);
      f->functions.push(image_operand_type);
      for (unsigned int i = 0; i < 16; ++i) {
         unsigned int id = image_operand_ids[i];
         if (id == 0)
            continue;
         f->functions.push(id);
      }
      break;
   case ir_query_levels:
      f->functions.opcode(4, SpvOpImageQueryLevels, type_id, result_id, ir->sampler->ir_value);
      break;
   case ir_texture_samples:
      f->functions.opcode(4, SpvOpImageQuerySamples, type_id, result_id, ir->sampler->ir_value);
      break;
   }
   ir->ir_value = result_id;
#if 0
   const ir_dereference_variable* var = ir->sampler->as_dereference_variable();
   if (var && var->var->data.precision == GLSL_PRECISION_MEDIUM) {
      f->decorates.push(SpvOpDecorate, 3);
      f->decorates.push(result_id);
      f->decorates.push(SpvDecorationRelaxedPrecision);
   }
#endif
}

void
ir_print_spirv_visitor::visit(ir_swizzle *ir)
{
   ir->val->accept(this);
   visit_value(ir->val);

   unsigned int type_id = visit_type(ir->type);
   unsigned int value_id = f->id++;
   unsigned int source_id = ir->val->ir_value;

   if (ir->mask.num_components == 1) {
      f->functions.opcode(5, SpvOpCompositeExtract, type_id, value_id, source_id, ir->mask.x);

      ir->ir_value = value_id;
      return;
   }

   if (ir->val->type->is_vector() == false) {
      f->functions.push(SpvOpCompositeConstruct, ir->mask.num_components + 3);
      f->functions.push(type_id);
      f->functions.push(value_id);
      for (unsigned int i = 0; i < ir->mask.num_components; ++i) {
         f->functions.push(source_id);
      }

      ir->ir_value = value_id;
      return;
   }

   f->functions.push(SpvOpVectorShuffle, ir->mask.num_components + 5);
   f->functions.push(type_id);
   f->functions.push(value_id);
   f->functions.push(source_id);
   f->functions.push(source_id);
   if (ir->mask.num_components >= 1)
      f->functions.push(ir->mask.x);
   if (ir->mask.num_components >= 2)
      f->functions.push(ir->mask.y);
   if (ir->mask.num_components >= 3)
      f->functions.push(ir->mask.z);
   if (ir->mask.num_components >= 4)
      f->functions.push(ir->mask.w);
   ir->ir_value = value_id;
}

void
ir_print_spirv_visitor::visit(ir_dereference_variable *ir)
{
   ir_variable *var = ir->variable_referenced();
   if (is_gl_identifier(var->name) == false && var->data.mode != ir_var_uniform)
      unique_name(var);

   switch (var->data.mode) {
   case ir_var_uniform:
      if (var->type->is_sampler()) {
         unsigned int sampled_image_id = visit_type(var->type);
         unsigned int value_id = f->id++;
         if (var->ir_pointer == 0) {
            unsigned int name_id = unique_name(var);
            unsigned int binding_id = var->ir_initialized;
            unsigned int type_pointer_id = visit_type_pointer(ir->type, var->data.mode, sampled_image_id);

            f->decorates.opcode(4, SpvOpDecorate, name_id, SpvDecorationDescriptorSet, 0);
            f->decorates.opcode(4, SpvOpDecorate, name_id, SpvDecorationBinding, binding_id);
            f->types.opcode(4, SpvOpVariable, type_pointer_id, var->ir_pointer, SpvStorageClassUniformConstant);
         }
         f->functions.opcode(4, SpvOpLoad, sampled_image_id, value_id, var->ir_pointer);
 
         ir->ir_value = value_id;
      } else if (var->type->is_interface()) {
         ir->ir_value = var->ir_value;
         break;
      } else {
         ir->ir_uniform = var->ir_uniform + 1;
         break;
      }
      ir->ir_pointer = var->ir_pointer;
      break;
   case ir_var_shader_out:
      if (f->shader_stage != MESA_SHADER_FRAGMENT && is_gl_identifier(var->name)) {
         if (f->gl_per_vertex_id == 0) {
            unsigned int name_id = f->id++;

            f->names.text(SpvOpName, name_id, "gl_PerVertex");
            f->decorates.opcode(3, SpvOpDecorate, name_id, SpvDecorationBlock);

            f->gl_per_vertex_id = name_id;
         }

         if (var->ir_initialized == 0) {
            const glsl_type* type;
            SpvBuiltIn built_in;
            if (strcmp(var->name, "gl_Position") == 0) {
               type = glsl_type::vec4_type;
               built_in = SpvBuiltInPosition;
            } else if (strcmp(var->name, "gl_PointSize") == 0) {
               type = glsl_type::float_type;
               built_in = SpvBuiltInPointSize;
            } else {
               break;
            }

            unsigned int struct_pointer_id = f->id++;
            unsigned int type_id = visit_type(type);
            unsigned int type_pointer_id = visit_type_pointer(type, var->data.mode, type_id);
            unsigned int variable_id = f->id++;
            unsigned int int_type_id = visit_type(glsl_type::int_type);
            unsigned int constant_id = f->id++;
            unsigned int pointer_id = f->id++;

            f->names.text(SpvOpMemberName, f->gl_per_vertex_id, f->per_vertices.count(), var->name);
            f->decorates.opcode(5, SpvOpMemberDecorate, f->gl_per_vertex_id, f->per_vertices.count(), SpvDecorationBuiltIn, built_in);
            f->builtins.opcode(4, SpvOpTypePointer, struct_pointer_id, SpvStorageClassOutput, f->gl_per_vertex_id);
            f->builtins.opcode(4, SpvOpVariable, struct_pointer_id, variable_id, SpvStorageClassOutput);
            f->builtins.opcode(4, SpvOpConstant, int_type_id, constant_id, f->per_vertices.count());
            f->functions.opcode(5, SpvOpAccessChain, type_pointer_id, pointer_id, variable_id, constant_id);

            f->inouts.push(variable_id);
            f->per_vertices.push(type_id);
            var->ir_initialized = pointer_id;
         }
         ir->ir_pointer = var->ir_initialized;
         break;
      } else if (f->shader_stage == MESA_SHADER_FRAGMENT && is_gl_identifier(var->name)) {
         if (var->ir_initialized == 0) {
            const glsl_type* type;
            if (strcmp(var->name, "gl_FragColor") == 0) {
               type = glsl_type::vec4_type;
            } else {
               break;
            }

            unsigned int type_id = visit_type(type);
            unsigned int type_pointer_id = visit_type_pointer(type, var->data.mode, type_id);
            unsigned int name_id = unique_name(var);

            f->decorates.opcode(4, SpvOpDecorate, name_id, SpvDecorationBinding, 0);
            f->builtins.opcode(4, SpvOpVariable, type_pointer_id, name_id, SpvStorageClassOutput);

            var->ir_initialized = name_id;
         }
         ir->ir_pointer = var->ir_initialized;
         break;
      }
      ir->ir_pointer = var->ir_pointer;
      break;
   default:
      ir->ir_pointer = var->ir_pointer;
      break;
   }
}

void
ir_print_spirv_visitor::visit(ir_dereference_array *ir)
{
   ir_dereference_variable* var = ir->array->as_dereference_variable();

   ir->array->accept(this);
   ir->array_index->accept(this);
   visit_value(ir->array_index);

   unsigned int type_id = visit_type(ir->type);
   unsigned int pointer_id;
   if (ir->array->ir_uniform) {
      unsigned int type_pointer_id = visit_type_pointer(ir->type, ir_var_uniform, type_id);
      unsigned int index_id = visit_constant_value(ir->array->ir_uniform - 1);
      pointer_id = f->id++;

      f->functions.opcode(6, SpvOpAccessChain, type_pointer_id, pointer_id, f->uniform_id, index_id, ir->array_index->ir_value);
   } else if (var && var->var) {
      unsigned int type_pointer_id = visit_type_pointer(ir->type, var->var->data.mode, type_id);
      pointer_id = f->id++;

      f->functions.opcode(5, SpvOpAccessChain, type_pointer_id, pointer_id, ir->array->ir_pointer, ir->array_index->ir_value);
   } else {
      unsigned int type_pointer_id = visit_type_pointer(ir->type, ir_var_auto, type_id);
      pointer_id = f->id++;

      f->functions.opcode(5, SpvOpAccessChain, type_pointer_id, pointer_id, ir->array->ir_pointer, ir->array_index->ir_value);
   }

   ir->ir_pointer = pointer_id;
}

void
ir_print_spirv_visitor::visit(ir_dereference_record *ir)
{
   ir->record->accept(this);
   visit_value(ir->record);

   glsl_struct_field& field = ir->record->type->fields.structure[ir->field_idx];
   unsigned int type_id = visit_type(field.type);
   unsigned int pointer_id = visit_type_pointer(field.type, ir_var_const_in, type_id);
   unsigned int value_id = f->id++;
   unsigned int index_id = visit_constant_value(ir->field_idx);

   f->functions.opcode(5, SpvOpAccessChain, pointer_id, value_id, ir->record->ir_value, index_id);

   ir->ir_pointer = value_id;
}

void
ir_print_spirv_visitor::visit(ir_assignment *ir)
{
   if (ir->condition)
      ir->condition->accept(this);

   ir->rhs->accept(this);
   ir->lhs->accept(this);
   visit_value(ir->rhs);

   unsigned int value_id;
   bool full_write = (util_bitcount(ir->write_mask) == ir->lhs->type->components()) || (ir->write_mask == 0 && ir->lhs->ir_value == 0);
   if (full_write && (ir->lhs->type->components() == ir->rhs->type->components())) {
      value_id = ir->rhs->ir_value;
   } else if (ir->rhs->type->components() == 1) {
      if (full_write) {
         unsigned int type_id = visit_type(ir->lhs->type);
         value_id = f->id++;

         f->functions.push(SpvOpCompositeConstruct, ir->lhs->type->components() + 3);
         f->functions.push(type_id);
         f->functions.push(value_id);
         for (unsigned int i = 0; i < ir->lhs->type->components(); ++i) {
            f->functions.push(ir->rhs->ir_value);
         }
      } else {
         ir_variable *var = ir->lhs->variable_referenced();
         unsigned int type_id = visit_type(ir->lhs->type->get_base_type());
         unsigned int type_pointer_id = visit_type_pointer(ir->lhs->type->get_base_type(), var->data.mode, type_id);
         for (unsigned int i = 0; i < ir->lhs->type->components(); ++i) {
            if (ir->write_mask & (1 << i)) {
               unsigned int access_id = f->id++;
               unsigned int index_id = visit_constant_value(i);

               f->functions.opcode(5, SpvOpAccessChain, type_pointer_id, access_id, ir->lhs->ir_pointer, index_id);
               f->functions.opcode(3, SpvOpStore, access_id, ir->rhs->ir_value);
            }
         }
         return;
      }
   } else {
      visit_value(ir->lhs);

      unsigned int type_id = visit_type(ir->lhs->type);
      value_id = f->id++;

      f->functions.push(SpvOpVectorShuffle, ir->lhs->type->components() + 5);
      f->functions.push(type_id);
      f->functions.push(value_id);
      f->functions.push(ir->lhs->ir_value);
      f->functions.push(ir->rhs->ir_value);

      for (unsigned int i = 0, j = 0; i < ir->lhs->type->components(); ++i) {
         if (ir->write_mask & (1 << i)) {
            f->functions.push(ir->lhs->type->components() + j++);
         } else {
            f->functions.push(i);
         }
      }
   }

   if (ir->lhs->ir_pointer != 0) {
      f->functions.opcode(3, SpvOpStore, ir->lhs->ir_pointer, value_id);
   }

   ir->lhs->ir_value = value_id;
}

void
ir_print_spirv_visitor::visit(ir_constant *ir)
{
   if (ir->type->is_array()) {
      for (unsigned i = 0; i < ir->type->length; i++)
         ir->get_array_element(i)->accept(this);
   } else if (ir->type->is_struct()) {
      for (unsigned i = 0; i < ir->type->length; i++) {
         ir->get_record_field(i)->accept(this);
      }
   } else {
      if (ir->type->components() == 1) {
         switch (ir->type->base_type) {
         case GLSL_TYPE_UINT:
            if (ir->value.u[0] <= 15)
               ir->ir_value = f->constant_int_id[ir->value.u[0]];
            break;
         case GLSL_TYPE_INT:
            if (ir->value.i[0] >= 0 && ir->value.i[0] <= 15)
               ir->ir_value = f->constant_int_id[ir->value.i[0]];
            break;
         case GLSL_TYPE_FLOAT:
            if (ir->value.f[0] >= 0.0f && ir->value.f[0] <= 15.0f && fmodf(ir->value.f[0], 1.0f) == 0.0f)
               ir->ir_value = f->constant_float_id[(int)ir->value.f[0]];
            break;
         default:
            break;
         }
         if (ir->ir_value)
            return;

         unsigned int type_id = visit_type(ir->type);
         unsigned int constant_id = f->id++;
         unsigned int value;
         switch (ir->type->base_type) {
         case GLSL_TYPE_UINT:  value = ir->value.u[0];         break;
         case GLSL_TYPE_INT:   value = ir->value.i[0];         break;
         case GLSL_TYPE_FLOAT: value = *(int*)&ir->value.f[0]; break;
         default:
            unreachable("Invalid constant type");
         }
         f->types.opcode(4, SpvOpConstant, type_id, constant_id, value);

         ir->ir_value = constant_id;
#if 0
         visit_precision(ir->ir_value, ir->type->base_type, GLSL_PRECISION_NONE);
#endif

         switch (ir->type->base_type) {
         case GLSL_TYPE_UINT:
            if (ir->value.u[0] <= 15)
               f->constant_int_id[ir->value.u[0]] = ir->ir_value;
            break;
         case GLSL_TYPE_INT:
            if (ir->value.i[0] >= 0 && ir->value.i[0] <= 15)
               f->constant_int_id[ir->value.i[0]] = ir->ir_value;
            break;
         case GLSL_TYPE_FLOAT:
            if (ir->value.f[0] >= 0.0f && ir->value.f[0] <= 15.0f && fmodf(ir->value.f[0], 1.0f) == 0.0f)
               f->constant_float_id[(int)ir->value.f[0]] = ir->ir_value;
            break;
         default:
            break;
         }
      } else {
         binary_buffer ids;
         for (unsigned int i = 0; i < ir->type->components(); i++) {
            switch (ir->type->base_type) {
            case GLSL_TYPE_UINT:
               ids.push(visit_constant_value(ir->value.u[i]));
               break;
            case GLSL_TYPE_INT:
               ids.push(visit_constant_value(ir->value.i[i]));
               break;
            case GLSL_TYPE_FLOAT:
               ids.push(visit_constant_value(ir->value.f[i]));
               break;
            default:
               unreachable("Invalid constant type");
            }
         }
         unsigned int value_id = f->id++;
         unsigned int type_id = visit_type(ir->type);
         unsigned int ids_count = ids.count();

         f->types.push(SpvOpConstantComposite, ids_count + 3);
         f->types.push(type_id);
         f->types.push(value_id);
         for (unsigned i = 0; i < ids_count; i++) {
            f->types.push(ids[i]);
         }
         ir->ir_value = value_id;
      }
#if 0
      visit_precision(ir->ir_value, ir->type->base_type, GLSL_PRECISION_NONE);
#endif
   }
}

void
ir_print_spirv_visitor::visit(ir_call *ir)
{
   //if (ir->return_deref) {
   //   ir->return_deref->accept(this);
   //   fprintf(f, " = ");
   //}
   //fprintf(f, "%s", ir->callee_name());
   //fprintf(f, "(");
   //foreach_in_list(ir_rvalue, param, &ir->actual_parameters) {
   //   if (param != ir->actual_parameters.head_sentinel.next)
   //      fprintf(f, ", ");
   //   param->accept(this);
   //}
   //fprintf(f, ")");
}

void
ir_print_spirv_visitor::visit(ir_return *ir)
{
   //ir_rvalue *const value = ir->get_value();
   //if (value) {
   //   fprintf(f, "return ");
   //   value->accept(this);
   //}
}

void
ir_print_spirv_visitor::visit(ir_discard *ir)
{
   if (ir->condition) {
      ir->condition->accept(this);

      unsigned int label_begin_id = f->id++;
      unsigned int label_end_id = f->id++;

      f->functions.opcode(3, SpvOpSelectionMerge, label_end_id, SpvSelectionControlMaskNone);
      f->functions.opcode(4, SpvOpBranchConditional, ir->condition->ir_value, label_begin_id, label_end_id);
      f->functions.opcode(2, SpvOpLabel, label_begin_id);
      f->functions.opcode(1, SpvOpKill, 1);
      f->functions.opcode(2, SpvOpLabel, label_end_id);
   } else {
      f->functions.opcode(1, SpvOpKill, 1);
   }
}

void
ir_print_spirv_visitor::visit(ir_demote *ir)
{
   //fprintf(f, "(demote)");
}

void
ir_print_spirv_visitor::visit(ir_if *ir)
{
   ir->condition->accept(this);

   unsigned int label_then_id = f->id++;
   unsigned int label_else_id = f->id++;
   unsigned int label_end_id = label_else_id;

   if (ir->else_instructions.is_empty() == false) {
      label_end_id = f->id++;
   } else {
      f->functions.opcode(3, SpvOpSelectionMerge, label_else_id, SpvSelectionControlMaskNone);
   }

   f->functions.opcode(4, SpvOpBranchConditional, ir->condition->ir_value, label_then_id, label_else_id);
   f->functions.opcode(2, SpvOpLabel, label_then_id);

   foreach_in_list(ir_instruction, inst, &ir->then_instructions) {
      inst->parent = ir;
      inst->accept(this);
   }

   if (ir->else_instructions.is_empty() == false) {
      f->functions.opcode(2, SpvOpBranch, label_end_id);
      f->functions.opcode(2, SpvOpLabel, label_else_id);

      foreach_in_list(ir_instruction, inst, &ir->else_instructions) {
         inst->parent = ir;
         inst->accept(this);
      }
   }

   f->functions.opcode(2, SpvOpBranch, label_end_id);
   f->functions.opcode(2, SpvOpLabel, label_end_id);
}

void
ir_print_spirv_visitor::visit(ir_loop *ir)
{
   unsigned int label_id = f->id++;
   unsigned int label_inner_id = f->id++;
   unsigned int label_outer_id = f->id++;

   f->functions.opcode(2, SpvOpBranch, label_id);
   f->functions.opcode(2, SpvOpLabel, label_id);
   f->functions.opcode(4, SpvOpLoopMerge, label_outer_id, label_inner_id, SpvLoopControlMaskNone);
   f->functions.opcode(2, SpvOpBranch, label_inner_id);
   f->functions.opcode(2, SpvOpLabel, label_inner_id);

   ir->ir_label = label_id;
   ir->ir_label_break = label_outer_id;

   foreach_in_list(ir_instruction, inst, &ir->body_instructions) {
      inst->parent = ir;
      inst->accept(this);
   }

   f->functions.opcode(2, SpvOpBranch, label_id);
   f->functions.opcode(2, SpvOpLabel, label_outer_id);
}

void
ir_print_spirv_visitor::visit(ir_loop_jump *ir)
{
   const ir_loop *loop = NULL;
   ir_instruction *parent = (ir_instruction *)ir->parent;
   while (parent) {
      loop = parent->as_loop();
      if (loop)
         break;
      parent = (ir_instruction*)parent->parent;
   }
   if (loop == NULL)
      return;

   unsigned int label_id = f->id++;
   unsigned int branch_id = ir->is_break() ? loop->ir_label_break : loop->ir_label;

   f->functions.opcode(2, SpvOpBranch, branch_id);
   f->functions.opcode(2, SpvOpLabel, label_id);
}

void
ir_print_spirv_visitor::visit(ir_emit_vertex *ir)
{
   //fprintf(f, "(emit-vertex ");
   //ir->stream->accept(this);
   //fprintf(f, ")\n");
}

void
ir_print_spirv_visitor::visit(ir_end_primitive *ir)
{
   //fprintf(f, "(end-primitive ");
   //ir->stream->accept(this);
   //fprintf(f, ")\n");
}

void
ir_print_spirv_visitor::visit(ir_barrier *)
{
   //fprintf(f, "(barrier)\n");
}
