#version 450

layout(binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 outUv;
layout(location = 1) in vec3 outColor;

layout(location = 0) out vec4 fragColor;

layout(push_constant) uniform Push {
    int mode;
} push;

void main() {
    if (push.mode == 0) {
        float a = texture(tex, outUv).r;
        float scan = 0.88 + 0.12 * step(0.5, fract(gl_FragCoord.y / 2.0));
        fragColor = vec4(outColor * scan, a);
    } else {
        fragColor = texture(tex, outUv);
    }
}
