/*
 * Flight-only fullscreen-quad vertex shader (v67).
 *
 * Identical to scene_fullscreen_quad.vert.hlsl except UV is flipped
 * on Y (uv.y = 1 - cy). Used ONLY by the flight present call
 * (fl_draw_present -> AeronScenePresentChain_Draw with flip_y=1):
 * on the GLES shim the flight RT holds its top at V=0, so NDC top
 * must sample V=0 for the flight scene to appear right-side-up.
 * The shared scene_fullscreen_quad.vert.hlsl is untouched, so the
 * hangar/concourse/CMD/preview/bloom paths (flip_y=0) are unaffected.
 * No vertex buffer, no cbuffer — same as the shared quad.
 */

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

VSOut main(uint vid : SV_VertexID)
{
    float cx = (float)(vid & 1u);
    float cy = (float)((vid >> 1u) & 1u);

    VSOut o;
    o.position = float4(cx * 2.0f - 1.0f, cy * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv       = float2(cx, 1.0f - cy);
    return o;
}
