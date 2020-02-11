#version 110
#extension GL_EXT_gpu_shader4 : require

void main()
{
    gl_FragColor = vec4(gl_PrimitiveID);
}
