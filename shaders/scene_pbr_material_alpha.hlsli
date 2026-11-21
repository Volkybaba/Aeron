/* Shared PBR material layout, variant lookup, and base-alpha evaluation. */

#ifndef SCENE_PBR_MATERIAL_ALPHA_HLSLI
#define SCENE_PBR_MATERIAL_ALPHA_HLSLI

#ifndef AERON_PBR_MATERIAL_REGISTER
#define AERON_PBR_MATERIAL_REGISTER t7
#endif
#ifndef AERON_PBR_VARIANT_REGISTER
#define AERON_PBR_VARIANT_REGISTER t8
#endif
#ifndef AERON_PBR_BASE_TEXTURE_REGISTER
#define AERON_PBR_BASE_TEXTURE_REGISTER t0
#endif
#ifndef AERON_PBR_BASE_SAMPLER_REGISTER
#define AERON_PBR_BASE_SAMPLER_REGISTER s0
#endif

static const uint GLTF_NO_MATERIAL = 0xFFFFFFFFu;
static const uint GLTF_MATERIAL_HAS_NORMAL = 0x1u;
static const uint GLTF_MATERIAL_HAS_METALLIC_ROUGHNESS = 0x2u;
static const uint GLTF_MATERIAL_HAS_EMISSIVE = 0x4u;
static const uint GLTF_MATERIAL_ALPHA_BLEND = 0x8u;
static const uint GLTF_MATERIAL_LEGACY_EMISSIVE = 0x10u;
static const uint GLTF_MATERIAL_ALPHA_MASK = 0x20u;

struct GltfMaterial
{
    float4 base_rect;
    float4 normal_rect;
    float4 mr_rect;
    float4 emissive_rect;
    float4 base_color_factor;
    float4 emissive_packed;
    float4 metal_rough; /* x=metallic, y=roughness, z=alpha cutoff */
    uint   flags;
    uint3  _pad;
};

/* Fragment SSBOs as RGBA32F lookup textures (GLES: Mali exposes zero
 * fragment storage blocks). Materials: 8 texels per record (7 float4
 * fields + texel 7 carrying the uint flags as raw bits in .x, read back
 * with asuint). Variants: 1 texel per uint4 group (raw bits, asuint). */
Texture2D<float4> g_materials_tex
    : register(AERON_PBR_MATERIAL_REGISTER, space2);
Texture2D<float4> g_prim_to_material_tex
    : register(AERON_PBR_VARIANT_REGISTER, space2);
Texture2D g_base_color
    : register(AERON_PBR_BASE_TEXTURE_REGISTER, space2);
SamplerState g_base_sampler
    : register(AERON_PBR_BASE_SAMPLER_REGISTER, space2);

uint pbr_resolve_material_index(uint prim_id, uint variant_row_base,
                                uint variant_group_count)
{
    uint slot = prim_id >> 2u;
    uint comp = prim_id & 3u;
    uint4 packed = slot < variant_group_count
        ? asuint(g_prim_to_material_tex.Load(int3(0, variant_row_base + slot, 0)))
        : uint4(GLTF_NO_MATERIAL, GLTF_NO_MATERIAL,
                GLTF_NO_MATERIAL, GLTF_NO_MATERIAL);
    return comp == 0u ? packed.x
         : comp == 1u ? packed.y
         : comp == 2u ? packed.z
                      : packed.w;
}

GltfMaterial pbr_default_material()
{
    GltfMaterial material;
    material.base_rect = float4(0, 0, 0, 0);
    material.normal_rect = float4(0, 0, 0, 0);
    material.mr_rect = float4(0, 0, 0, 0);
    material.emissive_rect = float4(0, 0, 0, 0);
    material.base_color_factor = float4(1, 1, 1, 1);
    material.emissive_packed = float4(0, 0, 0, 1);
    material.metal_rough = float4(0, 1, 0.5f, 0);
    material.flags = 0u;
    material._pad = uint3(0, 0, 0);
    return material;
}

GltfMaterial pbr_load_material(uint material_index)
{
    GltfMaterial material;
    material.base_rect = g_materials_tex.Load(int3(0, material_index, 0));
    material.normal_rect = g_materials_tex.Load(int3(1, material_index, 0));
    material.mr_rect = g_materials_tex.Load(int3(2, material_index, 0));
    material.emissive_rect = g_materials_tex.Load(int3(3, material_index, 0));
    material.base_color_factor = g_materials_tex.Load(int3(4, material_index, 0));
    material.emissive_packed = g_materials_tex.Load(int3(5, material_index, 0));
    material.metal_rough = g_materials_tex.Load(int3(6, material_index, 0));
    material.flags = asuint(g_materials_tex.Load(int3(7, material_index, 0)).x);
    material._pad = uint3(0, 0, 0);
    return material;
}

GltfMaterial pbr_resolve_material(uint prim_id, uint variant_row_base,
                                  uint variant_group_count,
                                  uint material_count)
{
    uint material_index = pbr_resolve_material_index(
        prim_id, variant_row_base, variant_group_count);
    if (material_index != GLTF_NO_MATERIAL &&
        material_index < material_count) {
        return pbr_load_material(material_index);
    }
    return pbr_default_material();
}

float4 pbr_sample_base_color(GltfMaterial material, float2 uv)
{
    return material.base_rect.z > 0.0f && material.base_rect.w > 0.0f
        ? atlas_sample(g_base_color, g_base_sampler, uv, material.base_rect)
        : float4(1, 1, 1, 1);
}

float pbr_base_alpha(GltfMaterial material, float2 uv)
{
    return pbr_sample_base_color(material, uv).a *
           material.base_color_factor.a;
}

void pbr_apply_alpha_mask(float alpha, float cutoff)
{
    clip(alpha - cutoff);
}

#endif /* SCENE_PBR_MATERIAL_ALPHA_HLSLI */
