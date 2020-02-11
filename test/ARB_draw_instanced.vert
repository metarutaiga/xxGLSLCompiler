#version 130
#extension GL_ARB_draw_instanced : require

void main()
{
    gl_Position = vec4(gl_VertexID + gl_InstanceID);
}
