#version 450

layout(binding = 0) uniform sampler2D tex;

layout(push_constant) uniform PushConsts {
    int subpixelMode;
} pc;

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 t = texture(tex, inUV);
    outColor = vec4(t.rgb * inColor, t.a);
}
