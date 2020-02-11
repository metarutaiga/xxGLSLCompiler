/* -*- c++ -*- */
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

#pragma once
#ifndef IR_PRINT_SPIRV_VISITOR_H
#define IR_PRINT_SPIRV_VISITOR_H

#include "ir.h"
#include "ir_visitor.h"

extern "C" {
#include "program/symbol_table.h"
}

class binary_buffer {
public:
   binary_buffer();
   virtual ~binary_buffer();
   void opcode(unsigned short length, unsigned short opcode, ...);
   void opcode(unsigned short length, unsigned short opcode, binary_buffer& buffer);
   void opcode(unsigned short length, unsigned short opcode, unsigned int v1, binary_buffer& buffer);
   void opcode(unsigned short length, unsigned short opcode, unsigned int v1, unsigned int v2, binary_buffer& buffer);
   void opcode(unsigned short length, unsigned short opcode, unsigned int v1, unsigned int v2, unsigned int v3, binary_buffer& buffer);
   void opcode(unsigned short length, unsigned short opcode, unsigned int v1, unsigned int v2, unsigned int v3, unsigned int v4, binary_buffer& buffer);
   void text(unsigned short opcode, const char* text);
   void text(unsigned short opcode, unsigned int id, const char* text);
   void text(unsigned short opcode, unsigned int id, unsigned int index, const char* text);
   void push(unsigned short low, unsigned short high);
   void push(unsigned int value);
   void push(const char *text);
   void push(binary_buffer& buffer);
   void clear();
   unsigned int count();
   unsigned int *data();
   unsigned int& operator[] (size_t i);
protected:
   unsigned int *buffer;
   unsigned int index;
   unsigned int capacity;
};

class spirv_buffer : public binary_buffer {
public:
   spirv_buffer();

   binary_buffer capability;
   binary_buffer extensions;
   binary_buffer names;
   binary_buffer decorates;
   binary_buffer types;
   binary_buffer builtins;
   binary_buffer functions;
   binary_buffer variables;
   binary_buffer codes;

   binary_buffer inouts;
   binary_buffer uniforms;
   binary_buffer per_vertices;

   gl_shader_stage shader_stage;

   unsigned int id;
   unsigned int binding_id;

   unsigned int precision_float;
   unsigned int precision_int;

   void *memory_begin;

   unsigned int ext_inst_import_id;
   unsigned int uniform_struct_id;
   unsigned int uniform_id;
   unsigned int uniform_pointer_id;
   unsigned int uniform_offset;
   unsigned int function_id;
   unsigned int main_id;

   unsigned int gl_per_vertex_name_id;
   unsigned int gl_per_vertex_struct_id;

   unsigned int void_id;
   unsigned int void_function_id;
   unsigned int bool_id;
   unsigned int float_id[5][5][5];
   unsigned int int_id[5][5][5];
   unsigned int unsigned_int_id[5][5][5];
   unsigned int image_id[16][16][6];
   unsigned int sampler_id[16];

   unsigned int constant_float_id[16];
   unsigned int constant_int_id[16];
   unsigned int constant_unsigned_int_id[16];

   unsigned int pointer_bool_id[16];
   unsigned int pointer_float_id[16][5][5][5];
   unsigned int pointer_int_id[16][5][5][5];
   unsigned int pointer_unsigned_int_id[16][5][5][5];
   unsigned int pointer_image_id[16][16][6];
   unsigned int pointer_sampler_id[16];

   unsigned int input_loc;
   unsigned int output_loc;

   void *memory_end;
};

/**
 * Abstract base class of visitors of IR instruction trees
 */
class ir_print_spirv_visitor : public ir_visitor {
public:
   ir_print_spirv_visitor(spirv_buffer *f);
   virtual ~ir_print_spirv_visitor();

   /**
    * \name Visit methods
    *
    * As typical for the visitor pattern, there must be one \c visit method for
    * each concrete subclass of \c ir_instruction.  Virtual base classes within
    * the hierarchy should not have \c visit methods.
    */
   /*@{*/
   virtual void visit(ir_rvalue *);
   virtual void visit(ir_variable *);
   virtual void visit(ir_function_signature *);
   virtual void visit(ir_function *);
   virtual void visit(ir_expression *);
   virtual void visit(ir_texture *);
   virtual void visit(ir_swizzle *);
   virtual void visit(ir_dereference_variable *);
   virtual void visit(ir_dereference_array *);
   virtual void visit(ir_dereference_record *);
   virtual void visit(ir_assignment *);
   virtual void visit(ir_constant *);
   virtual void visit(ir_call *);
   virtual void visit(ir_return *);
   virtual void visit(ir_discard *);
   virtual void visit(ir_demote *);
   virtual void visit(ir_if *);
   virtual void visit(ir_loop *);
   virtual void visit(ir_loop_jump *);
   virtual void visit(ir_emit_vertex *);
   virtual void visit(ir_end_primitive *);
   virtual void visit(ir_barrier *);
   /*@}*/

public:
   unsigned int visit_type(const struct glsl_type *type, GLenum format = GL_FLOAT);
   unsigned int visit_type_pointer(const struct glsl_type *type, unsigned int mode, unsigned int type_id, GLenum format = GL_FLOAT);
   unsigned int visit_constant_value(float value);
   unsigned int visit_constant_value(int value);
   unsigned int visit_constant_value(unsigned int value);
   void visit_value(ir_rvalue *ir);
   void visit_precision(unsigned int id, unsigned int type, unsigned int precision);

private:
   /**
    * Fetch/generate a unique name for ir_variable.
    *
    * GLSL IR permits multiple ir_variables to share the same name.  This works
    * fine until we try to print it, when we really need a unique one.
    */
   unsigned int unique_name(ir_variable *var);

   /** A mapping from ir_variable * -> unique printable names. */
   int parameter_number;
   int name_number;
   hash_table *printable_names;
   _mesa_symbol_table *symbols;

   void *mem_ctx;
   spirv_buffer *f;

   int indentation;
};

extern "C" {
void
_mesa_print_spirv(spirv_buffer *f, exec_list *instructions, struct _mesa_glsl_parse_state* state, unsigned binding);
}

#endif /* IR_PRINT_SPIRV_VISITOR_H */
