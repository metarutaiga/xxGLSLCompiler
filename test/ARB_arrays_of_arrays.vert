#version 110
#extension GL_ARB_arrays_of_arrays : require

uniform vec4 a0;
uniform vec4 a1[4];
uniform vec4 a2[4][4];
uniform vec4 a3[4][4][4];

void main()
{
    gl_Position = a0 + a1[0] + a2[0][0] + a3[0][0][0];
}
