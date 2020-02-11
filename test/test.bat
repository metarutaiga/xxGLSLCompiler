@for %%s in (*.vert) do ..\bin\xxGLSLCompiler.Release.x64.exe --dump-spirv-validation --version 450 %%s
@for %%s in (*.frag) do ..\bin\xxGLSLCompiler.Release.x64.exe --dump-spirv-validation --version 450 %%s
@pause