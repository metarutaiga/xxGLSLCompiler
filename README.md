# xxGLSLCompiler
GLSL Compiler from The Mesa 3D Graphics Library https://www.mesa3d.org/

### Usage
```
usage: xxGLSLCompiler.EXE [options] <file.vert | file.tesc | file.tese | file.geom | file.frag | file.comp>

Possible options are:
    --dump-ast
    --dump-hir
    --dump-lir
    --dump-builder
    --dump-glsl
    --dump-spirv
    --dump-spirv-glsl
    --link
    --just-log
    --version (mandatory)
```

### Example
```
#version 300 es

in vec3 Position;

void main()
{
  gl_Position = vec4(Position, 1.0);
}

C:\>xxGLSLCompiler.exe --version 300 --dump-spirv test.vert
; SPIR-V
; Version: 1.0
; Generator: X-LEGEND Mesa-IR/SPIR-V Translator; 0
; Bound: 28
; Schema: 0
               OpCapability Shader
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint Vertex %main "main" %Position %24
               OpSource ESSL 300
               OpName %Position "Position"
               OpName %main "main"
               OpName %vec_ctor "vec_ctor"
               OpName %gl_PerVertex "gl_PerVertex"
               OpMemberName %gl_PerVertex 0 "gl_Position"
               OpDecorate %Position Location 0
               OpDecorate %gl_PerVertex Block
               OpMemberDecorate %gl_PerVertex 0 BuiltIn Position
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
    %v3float = OpTypeVector %float 3
%_ptr_Input_v3float = OpTypePointer Input %v3float
   %Position = OpVariable %_ptr_Input_v3float Input
       %void = OpTypeVoid
          %8 = OpTypeFunction %void
%_ptr_Function_v4float = OpTypePointer Function %v4float
    %float_1 = OpConstant %float 1
%_ptr_Function_float = OpTypePointer Function %float
        %int = OpTypeInt 32 1
      %int_3 = OpConstant %int 3
%_ptr_Output_v4float = OpTypePointer Output %v4float
%gl_PerVertex = OpTypeStruct %v4float
%_ptr_Output_gl_PerVertex = OpTypePointer Output %gl_PerVertex
         %24 = OpVariable %_ptr_Output_gl_PerVertex Output
      %int_0 = OpConstant %int 0
       %main = OpFunction %void None %8
         %10 = OpLabel
   %vec_ctor = OpVariable %_ptr_Function_v4float Function
         %17 = OpAccessChain %_ptr_Function_float %vec_ctor %int_3
               OpStore %17 %float_1
         %18 = OpLoad %v3float %Position
         %19 = OpLoad %v4float %vec_ctor
         %20 = OpVectorShuffle %v4float %19 %18 4 5 6 3
               OpStore %vec_ctor %20
         %26 = OpAccessChain %_ptr_Output_v4float %24 %int_0
         %27 = OpLoad %v4float %vec_ctor
               OpStore %26 %27
               OpReturn
               OpFunctionEnd
```
