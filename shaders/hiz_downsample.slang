// Khoros Engine - Hierarchical-Z Depth Pyramid Downsampler
// Downsamples depth tile (2x2) using conservative MIN reduction for Reversed-Z.

[[vk::binding(0, 0)]]
Texture2D<float> input_mip;

[[vk::binding(1, 0)]]
RWTexture2D<float> output_mip;

struct HiZDownsamplePush {
    uint2 output_size;
};

[[vk::push_constant]]
HiZDownsamplePush push;

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= push.output_size.x || dtid.y >= push.output_size.y) {
        return;
    }

    uint2 in_coord = dtid.xy * 2u;
    float d00 = input_mip.Load(int3(in_coord, 0));
    float d10 = input_mip.Load(int3(in_coord + uint2(1u, 0u), 0));
    float d01 = input_mip.Load(int3(in_coord + uint2(0u, 1u), 0));
    float d11 = input_mip.Load(int3(in_coord + uint2(1u, 1u), 0));

    // Reversed-Z: conservative depth is the minimum depth (farthest surface in tile)
    float min_d = min(min(d00, d10), min(d01, d11));
    output_mip[dtid.xy] = min_d;
}
