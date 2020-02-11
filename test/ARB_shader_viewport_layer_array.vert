#version 110
#extension GL_ARB_shader_viewport_layer_array : require

void main()
{
    gl_Position = vec4(gl_Layer + gl_ViewportIndex);
}
