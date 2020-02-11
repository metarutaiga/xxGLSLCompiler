#version 130
#extension GL_ARB_cull_distance : require

void main()
{
    gl_Position = vec4(1.0);
    gl_PointSize = 1.0;
    gl_ClipDistance[0] = 1.0;
    gl_CullDistance[0] = 1.0;
}
