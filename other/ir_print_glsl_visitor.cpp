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

#include <inttypes.h> /* for PRIx64 macro */
#include "ir_print_glsl_visitor.h"
#include "compiler/glsl_types.h"
#include "glsl_parser_extras.h"
#include "main/macros.h"
#include "util/hash_table.h"
#include "util/u_string.h"

static void print_type(FILE *f, const glsl_type *t);

static const char *const glsl_expression_operation_strings[] = {
   "~",
   "!",
   "-",
   "abs",
   "sign",
   "1.0/",
   "inversesqrt",
   "sqrt",
   "exp",
   "log",
   "exp2",
   "log2",
   "int",
   "uint",
   "float",
   "bool",
   "float",
   "bool",
   "int",
   "float",
   "uint",
   "int",
   "float",
   "double",
   "int",
   "double",
   "uint",
   "double",
   "bool",
   "intBitsToFloat",
   "floatBitsToInt",
   "uintBitsToFloat",
   "floatBitsToUint",
   "bitcast_u642d",
   "bitcast_i642d",
   "bitcast_d2u64",
   "bitcast_d2i64",
   "i642i",
   "u642i",
   "i642u",
   "u642u",
   "i642b",
   "i642f",
   "u642f",
   "i642d",
   "u642d",
   "i2i64",
   "u2i64",
   "b2i64",
   "f2i64",
   "d2i64",
   "i2u64",
   "u2u64",
   "f2u64",
   "d2u64",
   "u642i64",
   "i642u64",
   "trunc",
   "ceil",
   "floor",
   "fract",
   "roundEven",
   "sin",
   "cos",
   "atan",
   "dFdx",
   "dFdxCoarse",
   "dFdxFine",
   "dFdy",
   "dFdyCoarse",
   "dFdyFine",
   "packSnorm2x16",
   "packSnorm4x8",
   "packUnorm2x16",
   "packUnorm4x8",
   "packHalf2x16",
   "unpackSnorm2x16",
   "unpackSnorm4x8",
   "unpackUnorm2x16",
   "unpackUnorm4x8",
   "unpackHalf2x16",
   "bitfield_reverse",
   "bit_count",
   "find_msb",
   "find_lsb",
   "clz",
   "saturate",
   "packDouble2x32",
   "unpackDouble2x32",
   "packSampler2x32",
   "packImage2x32",
   "unpackSampler2x32",
   "unpackImage2x32",
   "frexp_sig",
   "frexp_exp",
   "noise",
   "subroutine_to_int",
   "interpolate_at_centroid",
   "get_buffer_size",
   "ssbo_unsized_array_length",
   "packInt2x32",
   "packUint2x32",
   "unpackInt2x32",
   "unpackUint2x32",
   "+",
   "-",
   "add_sat",
   "sub_sat",
   "abs_sub",
   "average",
   "average_rounded",
   "*",
   "*",
   "imul_high",
   "/",
   "carry",
   "borrow",
   "mod",
   "<",
   ">=",
   "==",
   "!=",
   "all_equal",
   "any_nequal",
   "<<",
   ">>",
   "&",
   "^",
   "|",
   "&&",
   "^^",
   "||",
   "dot",
   "min",
   "max",
   "pow",
   "ubo_load",
   "ldexp",
   "vector_extract",
   "interpolate_at_offset",
   "interpolate_at_sample",
   "atan2",
   "fma",
   "mix",
   "csel",
   "bitfield_extract",
   "vector_insert",
   "bitfield_insert",
   "vector",
};

static const char *const glsl_expression_vector_operation_strings[] = {
   "lessThan",
   "greaterThanEqual",
   "equal",
   "notEqual",
};

static bool is_binop_func_like(ir_expression_operation op, const glsl_type* type)
{
   if (op == ir_binop_equal || op == ir_binop_nequal)
      return false;
   if (op == ir_binop_mod || (op >= ir_binop_dot && op <= ir_binop_pow))
      return true;
   if (type->is_vector() && (op >= ir_binop_less && op <= ir_binop_nequal))
      return true;
   return false;
}

extern "C" {
void
_mesa_print_glsl(FILE *f, exec_list *instructions, struct _mesa_glsl_parse_state *state)
{
   if (state) {
      fprintf(f, "#version %i", state->language_version);
      if (state->es_shader && state->language_version >= 300)
         fprintf(f, " es");
      fprintf(f, "\n");
      if (state->es_shader) {
         fprintf(f, "precision %s float;\n", state->stage == MESA_SHADER_VERTEX ? "highp" : "mediump");
         fprintf(f, "precision mediump int;\n");
      }
   }

   foreach_in_list(ir_instruction, ir, instructions) {
      ir_print_glsl_visitor v(f);

      if (ir->ir_type == ir_type_variable) {
         ir_variable *var = ir->as_variable();
         if (is_gl_identifier(var->name))
            continue;
         ir->accept(&v);
         fprintf(f, ";");
         fprintf(f, "\n");
         continue;
      }

      ir->accept(&v);
      if (ir->ir_type != ir_type_function)
         fprintf(f, "\n");
   }
}

} /* extern "C" */

ir_print_glsl_visitor::ir_print_glsl_visitor(FILE *f)
   : f(f)
{
   indentation = 0;
   printable_names = _mesa_pointer_hash_table_create(NULL);
   symbols = _mesa_symbol_table_ctor();
   mem_ctx = ralloc_context(NULL);
}

ir_print_glsl_visitor::~ir_print_glsl_visitor()
{
   _mesa_hash_table_destroy(printable_names, NULL);
   _mesa_symbol_table_dtor(symbols);
   ralloc_free(mem_ctx);
}

void
ir_print_glsl_visitor::indent(void)
{
   for (int i = 0; i < indentation; i++)
      fprintf(f, "  ");
}

const char *
ir_print_glsl_visitor::unique_name(ir_variable *var)
{
   /* var->name can be NULL in function prototypes when a type is given for a
    * parameter but no name is given.  In that case, just return an empty
    * string.  Don't worry about tracking the generated name in the printable
    * names hash because this is the only scope where it can ever appear.
    */
   if (var->name == NULL) {
      static unsigned arg = 1;
      return ralloc_asprintf(this->mem_ctx, "parameter_%u", arg++);
   }

   /* Do we already have a name for this variable? */
   struct hash_entry * entry =
      _mesa_hash_table_search(this->printable_names, var);

   if (entry != NULL) {
      return (const char *) entry->data;
   }

   /* If there's no conflict, just use the original name */
   const char* name = NULL;
   if (_mesa_symbol_table_find_symbol(this->symbols, var->name) == NULL) {
      name = var->name;
   } else {
      static unsigned i = 1;
      name = ralloc_asprintf(this->mem_ctx, "%s_%u", var->name, ++i);
   }
   _mesa_hash_table_insert(this->printable_names, var, (void *) name);
   _mesa_symbol_table_add_symbol(this->symbols, name, var);
   return name;
}

static void
print_type(FILE *f, const glsl_type *t)
{
   if (t->is_array()) {

   } else if (t->is_struct() && !is_gl_identifier(t->name)) {
      fprintf(f, "%s_%p", t->name, (void *) t);
   } else {
      fprintf(f, "%s", t->name);
   }
}

void
ir_print_glsl_visitor::visit(ir_rvalue *)
{
   fprintf(f, "error");
}

void
ir_print_glsl_visitor::visit(ir_variable *ir)
{
   static const char *const mode[] = { "", "uniform ", "", "", "in ", "out ", "in ", "out ", "inout ", "", "", "" };
   fprintf(f, "%s", mode[ir->data.mode]);

   if (ir->type->base_type == GLSL_TYPE_ARRAY) {
       print_type(f, ir->type->fields.array);
       fprintf(f, " %s", unique_name(ir));
       fprintf(f, "[%u]", ir->type->length);
       return;
   }

   print_type(f, ir->type);
   fprintf(f, " %s", unique_name(ir));
}

void
ir_print_glsl_visitor::visit(ir_function_signature *ir)
{
   _mesa_symbol_table_push_scope(symbols);

   print_type(f, ir->return_type);
   fprintf(f, " %s(", ir->function_name());
   foreach_in_list(ir_variable, inst, &ir->parameters) {
      if (inst != ir->parameters.head_sentinel.next)
         fprintf(f, ", ");
      inst->accept(this);
   }
   fprintf(f, ")\n{\n");

   indentation++;
   foreach_in_list(ir_instruction, inst, &ir->body) {
      indent();
      inst->accept(this);
      if (inst->ir_type == ir_type_if)
         fprintf(f, "\n");
      else
         fprintf(f, ";\n");
   }
   indentation--;
   indent();
   fprintf(f, "}\n");

   _mesa_symbol_table_pop_scope(symbols);
}

void
ir_print_glsl_visitor::visit(ir_function *ir)
{
   foreach_in_list(ir_function_signature, sig, &ir->signatures) {
      indent();
      sig->accept(this);
   }
}

void
ir_print_glsl_visitor::visit(ir_expression *ir)
{
   if (ir->num_operands == 1) {
      if (ir->operation >= ir_unop_f2i && ir->operation <= ir_unop_d2b) {
         print_type(f, ir->type);
         fprintf(f, "(");
      } else if (ir->operation == ir_unop_rcp) {
         fprintf(f, "(1.0/(");
      } else {
         fprintf(f, "%s(", glsl_expression_operation_strings[ir->operation]);
      }
      if (ir->operands[0])
   	     ir->operands[0]->accept(this);
      fprintf(f, ")");
      if (ir->operation == ir_unop_rcp) {
         fprintf(f, ")");
      }
   } else if (ir->operation == ir_binop_vector_extract) {
      if (ir->operands[0])
         ir->operands[0]->accept(this);
      fprintf(f, "[");
      if (ir->operands[1])
         ir->operands[1]->accept(this);
      fprintf(f, "]");
   } else if (is_binop_func_like(ir->operation, ir->type)) {
      if (ir->operation == ir_binop_mod) {
         fprintf(f, "(");
         print_type(f, ir->type);
         fprintf(f, "(");
      }
      if (ir->type->is_vector() && (ir->operation >= ir_binop_less && ir->operation <= ir_binop_nequal))
         fprintf(f, "%s(", glsl_expression_vector_operation_strings[ir->operation-ir_binop_less]);
      else
         fprintf(f, "%s(", glsl_expression_operation_strings[ir->operation]);

      if (ir->operands[0])
         ir->operands[0]->accept(this);
      fprintf(f, ", ");
      if (ir->operands[1])
         ir->operands[1]->accept(this);
      fprintf(f, ")");
      if (ir->operation == ir_binop_mod)
         fprintf(f, "))");
   } else if (ir->num_operands == 2) {
      fprintf(f, "(");
      if (ir->operands[0])
         ir->operands[0]->accept(this);

      fprintf(f, " %s ", glsl_expression_operation_strings[ir->operation]);

      if (ir->operands[1])
          ir->operands[1]->accept(this);
      fprintf(f, ")");
   } else {
      fprintf(f, "%s(", glsl_expression_operation_strings[ir->operation]);
      if (ir->operands[0])
         ir->operands[0]->accept(this);
      fprintf(f, ", ");
      if (ir->operands[1])
         ir->operands[1]->accept(this);
      fprintf(f, ", ");
      if (ir->operands[2])
         ir->operands[2]->accept(this);
      fprintf(f, ")");
   }
}

void
ir_print_glsl_visitor::visit(ir_texture *ir)
{
   if (ir->op == ir_samples_identical) {
      fprintf(f, "%s(", ir->opcode_string());
      ir->sampler->accept(this);
      fprintf(f, ", ");
      ir->coordinate->accept(this);
      fprintf(f, ")");
      return;
   }

   if (ir->op == ir_txf) {
      fprintf(f, "texelFetch");
   } else {
      fprintf(f, "texture");
   }

   if (ir->projector)
      fprintf(f, "Proj");
   if (ir->op == ir_txl)
      fprintf(f, "Lod");
   if (ir->op == ir_txd)
      fprintf(f, "Grad");
   if (ir->offset != NULL)
      fprintf(f, "Offset");

   fprintf(f, "(");
   ir->sampler->accept(this);

   if (ir->op != ir_txs && ir->op != ir_query_levels && ir->op != ir_texture_samples) {

      fprintf(f, ", ");
      ir->coordinate->accept(this);

      if (ir->offset != NULL) {
         fprintf(f, ", ");
         ir->offset->accept(this);
      }
   }

   if (ir->op != ir_txf && ir->op != ir_txf_ms && ir->op != ir_txs && ir->op != ir_tg4 && ir->op != ir_query_levels && ir->op != ir_texture_samples) {

      if (ir->projector) {
         fprintf(f, ", ");
         ir->projector->accept(this);
      }
   }

   switch (ir->op)
   {
   case ir_tex:
   case ir_lod:
   case ir_query_levels:
   case ir_texture_samples:
      break;
   case ir_txb:
      fprintf(f, ", ");
      ir->lod_info.bias->accept(this);
      break;
   case ir_txl:
   case ir_txf:
   case ir_txs:
      fprintf(f, ", ");
      ir->lod_info.lod->accept(this);
      break;
   case ir_txf_ms:
      fprintf(f, ", ");
      ir->lod_info.sample_index->accept(this);
      break;
   case ir_txd:
      fprintf(f, ", ");
      ir->lod_info.grad.dPdx->accept(this);
      fprintf(f, ", ");
      ir->lod_info.grad.dPdy->accept(this);
      break;
   case ir_tg4:
      ir->lod_info.component->accept(this);
      break;
   case ir_samples_identical:
      unreachable("ir_samples_identical was already handled");
   };
   fprintf(f, ")");
}

void
ir_print_glsl_visitor::visit(ir_swizzle *ir)
{
   const unsigned swiz[4] = {
      ir->mask.x,
      ir->mask.y,
      ir->mask.z,
      ir->mask.w,
   };

   if (ir->val->type->is_float() && ir->val->type->components() == 1) {
      fprintf(f, "vec2(");
      ir->val->accept(this);
      fprintf(f, ", 0.0)");
   } else {
      ir->val->accept(this);
   }
   fprintf(f, ".");
   for (unsigned i = 0; i < ir->mask.num_components; i++) {
      fprintf(f, "%c", "xyzw"[swiz[i]]);
   }
}

void
ir_print_glsl_visitor::visit(ir_dereference_variable *ir)
{
   ir_variable *var = ir->variable_referenced();
   fprintf(f, "%s", unique_name(var));
}

void
ir_print_glsl_visitor::visit(ir_dereference_array *ir)
{
   ir->array->accept(this);
   fprintf(f, "[");
   ir->array_index->accept(this);
   fprintf(f, "]");
}

void
ir_print_glsl_visitor::visit(ir_dereference_record *ir)
{
   ir->record->accept(this);

   const char *field_name =
      ir->record->type->fields.structure[ir->field_idx].name;
   fprintf(f, ".%s", field_name);
}

void
ir_print_glsl_visitor::visit(ir_assignment *ir)
{
   if (ir->condition)
      ir->condition->accept(this);

   ir->lhs->accept(this);

   if (ir->write_mask != ((1 << ir->lhs->type->components()) - 1)) {
      char mask[5];
      unsigned j = 0;

      for (unsigned i = 0; i < 4; i++) {
         if ((ir->write_mask & (1 << i)) != 0) {
            mask[j] = "xyzw"[i];
            j++;
         }
      }
      mask[j] = '\0';
      fprintf(f, ".%s", mask);
   }

   fprintf(f, " = ");
   ir->rhs->accept(this);
}

void
ir_print_glsl_visitor::visit(ir_constant *ir)
{
   if (ir->type->components() > 1 || ir->type->is_float() == false) {
      print_type(f, ir->type);
      fprintf(f, "(");
   }

   if (ir->type->is_array()) {
      for (unsigned i = 0; i < ir->type->length; i++)
         ir->get_array_element(i)->accept(this);
   } else if (ir->type->is_struct()) {
      for (unsigned i = 0; i < ir->type->length; i++) {
         fprintf(f, "(%s ", ir->type->fields.structure[i].name);
         ir->get_record_field(i)->accept(this);
         fprintf(f, ")");
      }
   } else {
      for (unsigned i = 0; i < ir->type->components(); i++) {
         if (i != 0)
            fprintf(f, ", ");
         switch (ir->type->base_type) {
         case GLSL_TYPE_UINT:  fprintf(f, "%u", ir->value.u[i]); break;
         case GLSL_TYPE_INT:   fprintf(f, "%d", ir->value.i[i]); break;
         case GLSL_TYPE_FLOAT:
            if (ir->value.f[i] == 0.0f)
               /* 0.0 == -0.0, so print with %f to get the proper sign. */
               fprintf(f, "%.1f", ir->value.f[i]);
            else if (fabs(ir->value.f[i]) < 0.000001f)
               fprintf(f, "%a", ir->value.f[i]);
            else if (fabs(ir->value.f[i]) > 1000000.0f)
               fprintf(f, "%e", ir->value.f[i]);
            else if (fmod(ir->value.f[i] * 10.0f, 1.0f) == 0.0f)
               fprintf(f, "%.1f", ir->value.f[i]);
            else
               fprintf(f, "%f", ir->value.f[i]);
            break;
         case GLSL_TYPE_BOOL:  fprintf(f, "%d", ir->value.b[i]); break;
         case GLSL_TYPE_DOUBLE:
            if (ir->value.d[i] == 0.0)
               /* 0.0 == -0.0, so print with %f to get the proper sign. */
               fprintf(f, "%.1f", ir->value.d[i]);
            else if (fabs(ir->value.d[i]) < 0.000001)
               fprintf(f, "%a", ir->value.d[i]);
            else if (fabs(ir->value.d[i]) > 1000000.0)
               fprintf(f, "%e", ir->value.d[i]);
            else if (fmod(ir->value.d[i] * 10.0, 1.0) == 0.0)
               fprintf(f, "%.1f", ir->value.d[i]);
            else
               fprintf(f, "%f", ir->value.d[i]);
            break;
         default:
            unreachable("Invalid constant type");
         }
      }
   }

   if (ir->type->components() > 1 || ir->type->is_float() == false) {
      fprintf(f, ")");
   }
}

void
ir_print_glsl_visitor::visit(ir_call *ir)
{
   if (ir->return_deref) {
      ir->return_deref->accept(this);
      fprintf(f, " = ");
   }
   fprintf(f, "%s", ir->callee_name());
   fprintf(f, "(");
   foreach_in_list(ir_rvalue, param, &ir->actual_parameters) {
      if (param != ir->actual_parameters.head_sentinel.next)
         fprintf(f, ", ");
      param->accept(this);
   }
   fprintf(f, ")");
}

void
ir_print_glsl_visitor::visit(ir_return *ir)
{
   ir_rvalue *const value = ir->get_value();
   if (value) {
      fprintf(f, "return ");
      value->accept(this);
   }
}

void
ir_print_glsl_visitor::visit(ir_discard *ir)
{
   if (ir->condition) {
      fprintf(f, "if ");
      ir->condition->accept(this);
      fprintf(f, "\n");
      indentation++;
      indent();
      indentation--;
   }

   fprintf(f, "discard");
}

void
ir_print_glsl_visitor::visit(ir_demote *ir)
{
   fprintf(f, "(demote)");
}

void
ir_print_glsl_visitor::visit(ir_if *ir)
{
   fprintf(f, "if (");
   ir->condition->accept(this);

   fprintf(f, ") {\n");
   indentation++;

   foreach_in_list(ir_instruction, inst, &ir->then_instructions) {
      indent();
      inst->accept(this);
      fprintf(f, ";\n");
   }

   indentation--;
   indent();
   fprintf(f, "}\n");

   indent();
   if (!ir->else_instructions.is_empty()) {
      fprintf(f, "else {\n");
      indentation++;

      foreach_in_list(ir_instruction, inst, &ir->else_instructions) {
         indent();
         inst->accept(this);
         fprintf(f, ";\n");
      }
      indentation--;
      indent();
      fprintf(f, "}\n");
   }
}

void
ir_print_glsl_visitor::visit(ir_loop *ir)
{
   fprintf(f, "while (true) {\n");
   indentation++;

   foreach_in_list(ir_instruction, inst, &ir->body_instructions) {
      indent();
      inst->accept(this);
      fprintf(f, "\n");
   }
   indentation--;
   indent();
   fprintf(f, "}\n");
}

void
ir_print_glsl_visitor::visit(ir_loop_jump *ir)
{
   fprintf(f, "%s", ir->is_break() ? "break" : "continue");
}

void
ir_print_glsl_visitor::visit(ir_emit_vertex *ir)
{
   fprintf(f, "(emit-vertex ");
   ir->stream->accept(this);
   fprintf(f, ")\n");
}

void
ir_print_glsl_visitor::visit(ir_end_primitive *ir)
{
   fprintf(f, "(end-primitive ");
   ir->stream->accept(this);
   fprintf(f, ")\n");
}

void
ir_print_glsl_visitor::visit(ir_barrier *)
{
   fprintf(f, "(barrier)\n");
}
