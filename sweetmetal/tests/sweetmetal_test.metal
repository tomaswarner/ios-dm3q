// Owned fixture: the functions the metal2vulkan-runtime crate's examples run
// (runtime/examples/). Written from scratch for them; `scripts/regen-air-fixtures/
// regen-air-fixtures.sh --dir examples` compiles it. No translator sweep reads this directory.
#include <metal_stdlib>
using namespace metal;

// compute_dispatch, indirect_command_buffer and adopted_buffer:
// output[i] = input[i] * scale + i.
kernel void scale_and_offset(device const float *input [[buffer(0)]],
                             device float *output [[buffer(1)]],
                             constant float &scale [[buffer(2)]],
                             uint index [[thread_position_in_grid]]) {
    output[index] = input[index] * scale + float(index);
}

// render_to_texture: a triangle list whose vertices read their positions and colors from two
// buffers by vertex ID; each triangle takes one color.
struct Varyings {
    float4 position [[position]];
    float4 color [[flat]];
};

vertex Varyings colored_vertex(device const float2 *positions [[buffer(0)]],
                               device const float4 *colors [[buffer(1)]],
                               uint id [[vertex_id]]) {
    Varyings out;
    out.position = float4(positions[id], 0.0, 1.0);
    out.color = colors[id];
    return out;
}

fragment float4 colored_fragment(Varyings in [[stage_in]]) {
    return in.color;
}

// The runtime's made_at_creation lane: each pixel of the triangles sampled from a half texture
// through a linear filter, at its position over an 8-texel side.
fragment half4 sampled_fragment(Varyings in [[stage_in]],
                                texture2d<half> source [[texture(0)]]) {
    constexpr sampler linear(filter::linear);
    return source.sample(linear, in.position.xy / 8.0);
}

// heap_placement: each texel of `source` with its color channels inverted, into `destination`.
kernel void invert_texels(texture2d<float, access::read> source [[texture(0)]],
                          texture2d<float, access::write> destination [[texture(1)]],
                          uint2 position [[thread_position_in_grid]]) {
    float4 texel = source.read(position);
    destination.write(float4(1.0 - texel.rgb, texel.a), position);
}

// sweetmetal slice 3: vertex descriptor input
struct AttrIn {
    float3 position [[attribute(0)]];
    float4 color [[attribute(1)]];
};
struct AttrOut {
    float4 position [[position]];
    float4 color;
};
vertex AttrOut attr_vertex(AttrIn in [[stage_in]]) { return { float4(in.position, 1), in.color }; }
fragment float4 attr_fragment(AttrOut in [[stage_in]]) { return in.color; }

// function constants: out[i] = in[i] * factor (+ 1 when add_one)
constant float factor [[function_constant(0)]];
constant bool add_one [[function_constant(1)]];
kernel void scaled(device const float *in [[buffer(0)]], device float *out [[buffer(1)]],
                   uint i [[thread_position_in_grid]]) {
    out[i] = in[i] * factor + (add_one ? 1.0f : 0.0f);
}
