#version 450
// The GPU process's quads (gpu_vk.c): each instance is one display-list
// record, drawn over the pixel rectangle the CPU computed for it (the same
// rectangle raster_cpu.c fills).

struct Item {
    vec4 rect;      // x, y, w, h: the record's box, px
    vec4 color;
    vec4 uv;        // u0, v0, u1, v1
    ivec4 quad;     // x0, y0, x1, y1: the pixels to cover (x1, y1 excluded)
    ivec4 info;     // type, mask index, border rings, level
    vec4 extra;     // a mask blend's box
};

layout(std430, set = 0, binding = 0) readonly buffer Items { Item items[]; };

layout(push_constant) uniform Push { vec2 size; } pc;

layout(location = 0) flat out int v_item;

void main() {
    Item it = items[gl_InstanceIndex];
    const vec2 corner[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
    vec2 c = corner[gl_VertexIndex];
    vec2 p = mix(vec2(it.quad.xy), vec2(it.quad.zw), c);
    gl_Position = vec4(p / pc.size * 2.0 - 1.0, 0.0, 1.0);
    v_item = gl_InstanceIndex;
}
