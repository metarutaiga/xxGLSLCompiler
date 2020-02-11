#version 140
#extension GL_ARB_texture_query_levels : require
#extension GL_ARB_texture_query_lod : require

uniform sampler2D sampler;

void main()
{
    gl_FragColor = vec4(textureSize(sampler, 0), textureQueryLevels(sampler), textureQueryLOD(sampler, vec2(0.0)).x);
}
