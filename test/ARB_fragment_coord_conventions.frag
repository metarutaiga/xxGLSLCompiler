#version 120
#extension GL_ARB_fragment_coord_conventions : require

void main()
{
    gl_FragColor = gl_FragCoord + gl_PointCoord.xyyy + vec4(gl_FrontFacing ? 1.0 : 0.0);
}
