/*
 * Rect-blit vertex shader — plain per-vertex TRIANGLELIST quads.
 *
 * Each sprite is expanded by the C side into six vertices carrying
 * the full sprite payload (dst/src/tint/bias/trap) plus a list
 * corner attribute (0,0),(1,0),(0,1) + (1,0),(1,1),(0,1). No
 * instancing and no SV_VertexID: both proved unreliable on the GLES
 * shim (Mali) — the instanced-attribute + VertexID variant drew zero
 * pixels even with a fixed-quad magenta fragment shader, so the
 * corner now rides an ordinary vertex attribute and the draw is a
 * plain DrawPrimitives. A strip would not restart between quads
 * (bridging triangles overdraw glyphs with the next sprite's UVs),
 * hence the list.
 *
 * One draw produces N quads via SDL_DrawGPUPrimitives(6*N, 0).
 * The bound vertex-buffer offset selects the run's first vertex.
 *
 * Attribute layout MUST match Blit2DVertex in draw_list2d.c:
 *   dst_rect : (x, y, w, h)        target pixels, top-left + size
 *   src_rect : (u0, v0, u1, v1)    texture UV
 *   tint     : (r, g, b, a)        multiplier
 *   bias     : (r, g, b, a)        additive (alpha-weighted)
 *   trap     : (top_dx_left, top_dx_right, top_w, _)
 *                                  pixel-space top-edge horizontal
 *                                  inset + per-corner projective w
 *                                  for hyperbolic UV interpolation
 *   corner   : (cx, cy)            strip corner, (0,0),(1,0),(0,1),(1,1)
 */

struct BlitUniforms
{
    float4 dst_rect;
    float4 src_rect;
    float4 tint;
    float4 bias;
    float4 trap;
};

/* Per-vertex attributes (locations 0..5). */
struct VSIn
{
    float4 dst_rect : TEXCOORD0;
    float4 src_rect : TEXCOORD1;
    float4 tint     : TEXCOORD2;
    float4 bias     : TEXCOORD3;
    float4 trap     : TEXCOORD4;
    float2 corner   : TEXCOORD5;
};

cbuffer BlitRun : register(b0, space1)
{
    float2 ndc_scale;
    float  output_rgb_scale;
    uint   _pad; /* reserved; instance offset is the bound vertex-buffer offset */
};

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float4 tint     : TEXCOORD1;
    float4 bias     : TEXCOORD2;
};

VSOut main(VSIn input)
{
    BlitUniforms u;
    u.dst_rect = input.dst_rect;
    u.src_rect = input.src_rect;
    u.tint     = input.tint;
    u.bias     = input.bias;
    u.trap     = input.trap;

    float cx = input.corner.x;
    float cy = input.corner.y;

    float2 pos_px = u.dst_rect.xy + float2(cx, 1.0f - cy) * u.dst_rect.zw;
    /* Trapezoid top-edge horizontal inset. (0, 0) = pass-through. */
    pos_px.x += cy * lerp(u.trap.x, u.trap.y, cx);
    /* v68: shim artik flip'i hedef bazinda yapiyor (offscreen FBO ->
     * program_flip, swapchain -> normal program); oyun tarafi
     * orijinal Vulkan formulune dondu, cift flip olmasin. */
    float2 pos = float2(pos_px.x * ndc_scale.x - 1.0f,
                        1.0f - pos_px.y * ndc_scale.y);

    /* uv.y mixes from src_rect.w (bottom of texture) at cy=0 to
     * src_rect.y (top of texture) at cy=1, matching the +Y up NDC vs
     * +Y down texcoord conventions. */
    float2 uv = float2(
        u.src_rect.x + cx * (u.src_rect.z - u.src_rect.x),
        u.src_rect.w + cy * (u.src_rect.y - u.src_rect.w));

    /* Per-corner clip-space `w` for projective UV interpolation.
     * trap.z = w at the TOP edge (cy=1), w=1 at the bottom. With the
     * rasterizer's perspective-correct varying interpolation, setting
     * position.w = w_corner makes UV interpolation hyperbolic — a
     * glyph spanning a slanted line samples as if projected from a
     * tilted plane. trap.z = 0 (the C zero-init default) is treated
     * as 1.0, preserving the existing rect-blit behavior. */
    float w_top    = (u.trap.z > 0.0f) ? u.trap.z : 1.0f;
    float w_corner = lerp(1.0f, w_top, cy);

    VSOut o;
    /* Pre-multiply xy by w so the pipeline's automatic divide lands
     * at the intended NDC. UV is plain — the rasterizer's
     * perspective-correct interpolation handles the projective
     * sampling on its own. */
    o.position = float4(pos * w_corner, 0.0f, w_corner);
    o.uv       = uv;
    o.tint     = float4(u.tint.rgb * output_rgb_scale, u.tint.a);
    o.bias     = float4(u.bias.rgb * output_rgb_scale, u.bias.a);
    return o;
}
