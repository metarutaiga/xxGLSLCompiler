/*
 * © Copyright 2018 Alyssa Rosenzweig
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
 */

#ifndef __PAN_FORMAT_H__
#define __PAN_FORMAT_H__

#include "pan_context.h"
#include "util/format/u_format.h"

unsigned
panfrost_translate_swizzle_4(const unsigned char swizzle[4]);

unsigned
panfrost_get_default_swizzle(unsigned components);

enum mali_format
panfrost_find_format(const struct util_format_description *desc);

void
panfrost_invert_swizzle(const unsigned char *in, unsigned char *out);

bool
panfrost_is_z24s8_variant(enum pipe_format fmt);

#endif


