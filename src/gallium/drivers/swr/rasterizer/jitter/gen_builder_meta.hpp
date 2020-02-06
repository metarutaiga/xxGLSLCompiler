//============================================================================
// Copyright (C) 2014-2017 Intel Corporation.   All Rights Reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice (including the next
// paragraph) shall be included in all copies or substantial portions of the
// Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//
// @file gen_builder_meta.hpp
//
// @brief auto-generated file
//
// DO NOT EDIT
//
// Generation Command Line:
//  ./rasterizer/codegen/gen_llvm_ir_macros.py
//    --output
//    rasterizer/jitter
//    --gen_meta_h
//
//============================================================================
// clang-format off
#pragma once

//============================================================================
// Auto-generated meta intrinsics
//============================================================================
Value* VGATHERPD(Value* src, Value* pBase, Value* indices, Value* mask, Value* scale, const llvm::Twine& name = "")
{
    SmallVector<Type*, 5> argTypes;
    argTypes.push_back(src->getType());
    argTypes.push_back(pBase->getType());
    argTypes.push_back(indices->getType());
    argTypes.push_back(mask->getType());
    argTypes.push_back(scale->getType());
    FunctionType* pFuncTy = FunctionType::get(src->getType(), argTypes, false);
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.VGATHERPD", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{src, pBase, indices, mask, scale}, name);
}

Value* VGATHERPS(Value* src, Value* pBase, Value* indices, Value* mask, Value* scale, const llvm::Twine& name = "")
{
    SmallVector<Type*, 5> argTypes;
    argTypes.push_back(src->getType());
    argTypes.push_back(pBase->getType());
    argTypes.push_back(indices->getType());
    argTypes.push_back(mask->getType());
    argTypes.push_back(scale->getType());
    FunctionType* pFuncTy = FunctionType::get(src->getType(), argTypes, false);
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.VGATHERPS", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{src, pBase, indices, mask, scale}, name);
}

Value* VGATHERDD(Value* src, Value* pBase, Value* indices, Value* mask, Value* scale, const llvm::Twine& name = "")
{
    SmallVector<Type*, 5> argTypes;
    argTypes.push_back(src->getType());
    argTypes.push_back(pBase->getType());
    argTypes.push_back(indices->getType());
    argTypes.push_back(mask->getType());
    argTypes.push_back(scale->getType());
    FunctionType* pFuncTy = FunctionType::get(src->getType(), argTypes, false);
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.VGATHERDD", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{src, pBase, indices, mask, scale}, name);
}

Value* VRCPPS(Value* a, const llvm::Twine& name = "")
{
    SmallVector<Type*, 1> argTypes;
    argTypes.push_back(a->getType());
    FunctionType* pFuncTy = FunctionType::get(a->getType(), argTypes, false);
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.VRCPPS", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{a}, name);
}

Value* VROUND(Value* a, Value* rounding, const llvm::Twine& name = "")
{
    SmallVector<Type*, 2> argTypes;
    argTypes.push_back(a->getType());
    argTypes.push_back(rounding->getType());
    FunctionType* pFuncTy = FunctionType::get(a->getType(), argTypes, false);
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.VROUND", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{a, rounding}, name);
}

Value* BEXTR_32(Value* src, Value* control, const llvm::Twine& name = "")
{
    SmallVector<Type*, 2> argTypes;
    argTypes.push_back(src->getType());
    argTypes.push_back(control->getType());
    FunctionType* pFuncTy = FunctionType::get(src->getType(), argTypes, false);
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.BEXTR_32", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{src, control}, name);
}

Value* VPSHUFB(Value* a, Value* b, const llvm::Twine& name = "")
{
    SmallVector<Type*, 2> argTypes;
    argTypes.push_back(a->getType());
    argTypes.push_back(b->getType());
    FunctionType* pFuncTy = FunctionType::get(a->getType(), argTypes, false);
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.VPSHUFB", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{a, b}, name);
}

Value* VPERMD(Value* a, Value* idx, const llvm::Twine& name = "")
{
    SmallVector<Type*, 2> argTypes;
    argTypes.push_back(a->getType());
    argTypes.push_back(idx->getType());
    FunctionType* pFuncTy = FunctionType::get(a->getType(), argTypes, false);
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.VPERMD", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{a, idx}, name);
}

Value* VPERMPS(Value* idx, Value* a, const llvm::Twine& name = "")
{
    SmallVector<Type*, 2> argTypes;
    argTypes.push_back(idx->getType());
    argTypes.push_back(a->getType());
    FunctionType* pFuncTy = FunctionType::get(a->getType(), argTypes, false);
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.VPERMPS", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{idx, a}, name);
}

Value* VCVTPD2PS(Value* a, const llvm::Twine& name = "")
{
    SmallVector<Type*, 1> argTypes;
    argTypes.push_back(a->getType());
    FunctionType* pFuncTy = FunctionType::get(VectorType::get(mFP32Ty, a->getType()->getVectorNumElements()), argTypes, false);
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.VCVTPD2PS", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{a}, name);
}

Value* VCVTPH2PS(Value* a, const llvm::Twine& name = "")
{
    SmallVector<Type*, 1> argTypes;
    argTypes.push_back(a->getType());
    FunctionType* pFuncTy = FunctionType::get(VectorType::get(mFP32Ty, a->getType()->getVectorNumElements()), argTypes, false);
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.VCVTPH2PS", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{a}, name);
}

Value* VCVTPS2PH(Value* a, Value* round, const llvm::Twine& name = "")
{
    SmallVector<Type*, 2> argTypes;
    argTypes.push_back(a->getType());
    argTypes.push_back(round->getType());
    FunctionType* pFuncTy = FunctionType::get(mSimdInt16Ty, argTypes, false);
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.VCVTPS2PH", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{a, round}, name);
}

Value* VHSUBPS(Value* a, Value* b, const llvm::Twine& name = "")
{
    SmallVector<Type*, 2> argTypes;
    argTypes.push_back(a->getType());
    argTypes.push_back(b->getType());
    FunctionType* pFuncTy = FunctionType::get(a->getType(), argTypes, false);
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.VHSUBPS", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{a, b}, name);
}

Value* VPTESTC(Value* a, Value* b, const llvm::Twine& name = "")
{
    SmallVector<Type*, 2> argTypes;
    argTypes.push_back(a->getType());
    argTypes.push_back(b->getType());
    FunctionType* pFuncTy = FunctionType::get(mInt32Ty, argTypes, false);
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.VPTESTC", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{a, b}, name);
}

Value* VPTESTZ(Value* a, Value* b, const llvm::Twine& name = "")
{
    SmallVector<Type*, 2> argTypes;
    argTypes.push_back(a->getType());
    argTypes.push_back(b->getType());
    FunctionType* pFuncTy = FunctionType::get(mInt32Ty, argTypes, false);
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.VPTESTZ", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{a, b}, name);
}

Value* VPHADDD(Value* a, Value* b, const llvm::Twine& name = "")
{
    SmallVector<Type*, 2> argTypes;
    argTypes.push_back(a->getType());
    argTypes.push_back(b->getType());
    FunctionType* pFuncTy = FunctionType::get(a->getType(), argTypes, false);
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.VPHADDD", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{a, b}, name);
}

Value* PDEP32(Value* a, Value* b, const llvm::Twine& name = "")
{
    SmallVector<Type*, 2> argTypes;
    argTypes.push_back(a->getType());
    argTypes.push_back(b->getType());
    FunctionType* pFuncTy = FunctionType::get(a->getType(), argTypes, false);
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.PDEP32", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{a, b}, name);
}

Value* RDTSC(const llvm::Twine& name = "")
{
    FunctionType* pFuncTy = FunctionType::get(mInt64Ty, {}, false);
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.RDTSC", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{}, name);
}

    // clang-format on
