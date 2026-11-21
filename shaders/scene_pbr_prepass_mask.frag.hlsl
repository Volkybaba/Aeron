/* Alpha-mask depth/normal/velocity prepass fragment shader. */

#include "scene_pbr_atlas_sample.hlsli"
#include "scene_pbr_prepass_common.hlsli"
#include "scene_pbr_vsout.hlsli"

/* Unified with the mesh pipeline (t8/t9): the C side binds the
 * material/variant lookup textures at sampler slots 8/9 for every PBR
 * fragment shader, so the registers must match across pipelines. */
#define AERON_PBR_MATERIAL_REGISTER t8
#define AERON_PBR_VARIANT_REGISTER t9
#include "scene_pbr_material_alpha.hlsli"

PbrPrepassFSOut main(PbrMaskPrepassVSOut i)
{
    uint4 lookup = i.material_lookup;
    GltfMaterial material = pbr_resolve_material(
        lookup.x, lookup.y, lookup.z, lookup.w);
    pbr_apply_alpha_mask(pbr_base_alpha(material, i.uv),
                         material.metal_rough.z);

    return pbr_build_prepass_output(i.position, i.world_pos, i.world_normal,
                                    i.clip_curr, i.clip_prev);
}
