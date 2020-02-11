#version 130
#extension GL_ARB_draw_instanced : require
#extension GL_ARB_shader_draw_parameters : require

void main()
{
    gl_Position = vec4(gl_VertexID + gl_InstanceID + gl_BaseVertexARB + gl_BaseInstanceARB + gl_DrawIDARB);
}
