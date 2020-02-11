#version 140
#extension GL_ARB_shader_image_load_store : require

uniform sampler2D tex;
layout(rgba32f) uniform image2D image;
writeonly uniform image2D image_write;

layout(r32i) uniform iimage2D counter;

void main()
{
    vec4 color = imageLoad(image, ivec2(gl_FragCoord.xy));
    color += texture(tex, vec2(0.0));
    color += texture(tex, vec2(1.0));
    memoryBarrier();
    imageStore(image, ivec2(gl_FragCoord.xy), vec4(1.0) + color);
    imageStore(image_write, ivec2(gl_FragCoord.xy), vec4(1.0) + color);
    gl_FragColor = vec4(1.0);

    int number = imageAtomicAdd(counter, ivec2(gl_FragCoord.xy), 1);
    number = imageAtomicMin(counter, ivec2(gl_FragCoord.xy), number);
    number = imageAtomicMax(counter, ivec2(gl_FragCoord.xy), number);
    number = imageAtomicAnd(counter, ivec2(gl_FragCoord.xy), number);
    number = imageAtomicOr(counter, ivec2(gl_FragCoord.xy), number);
    number = imageAtomicXor(counter, ivec2(gl_FragCoord.xy), number);
    number = imageAtomicExchange(counter, ivec2(gl_FragCoord.xy), number);
    number = imageAtomicCompSwap(counter, ivec2(gl_FragCoord.xy), 1, number);
}
