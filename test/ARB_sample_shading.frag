#version 400
#extension GL_ARB_sample_shading : require

void main()
{
    gl_FragColor = vec4(gl_SampleID) + gl_SamplePosition.xxyy;
}
