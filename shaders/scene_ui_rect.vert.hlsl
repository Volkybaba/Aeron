/*
 * UI rounded-rect vertex shader — instanced TRIANGLESTRIP quads via
 * per-instance vertex attributes (no storage buffer: GLES vertex
 * stages allow zero storage blocks), one analytic rounded rect per
 * instance.
 *
 * Attribute layout MUST match Blit2DRRectInstance in draw_list2d.c:
 *   dst_rect    : (x, y, w, h)                  target pixels, top-left + size
 *   params      : (radius, border, bevel, soft) all in pixels
 *   fill_top    : (r, g, b, a)                  PMA
 *   fill_bottom : (r, g, b, a)                  PMA
 *   border_col  : (r, g, b, a)                  PMA
 *   bevel_hi    : (r, g, b, a)                  PMA
 *   bevel_lo    : (r, g, b, a)                  PMA
 *
 * The quad is inflated by the soft radius so shadow/AA falloff is never
 * clipped by the rasterized footprint. The fragment stage works in
 * framebuffer pixel space via SV_Position, so only per-instance data
 * rides the varyings (all nointerpolation). Color rgb is premultiplied
 * by output_rgb_scale here (HDR paper-white), matching scene_blit.
 */

struct RRectInstance
{
    float4 dst_rect;
    float4 params;
    float4 fill_top;
    float4 fill_bottom;
    float4 border_col;
    float4 bevel_hi;
    float4 bevel_lo;
};

/* Per-instance attributes (locations 0..6, one float4 each). */
struct VSIn
{
    float4 dst_rect    : TEXCOORD0;
    float4 params      : TEXCOORD1;
    float4 fill_top    : TEXCOORD2;
    float4 fill_bottom : TEXCOORD3;
    float4 border_col  : TEXCOORD4;
    float4 bevel_hi    : TEXCOORD5;
    float4 bevel_lo    : TEXCOORD6;
};

cbuffer BlitRun : register(b0, space1)
{
    float2 ndc_scale;
    float  output_rgb_scale;
    uint   _pad; /* reserved; instance offset is the bound vertex-buffer offset */
};

struct VSOut
{
    float4                 position    : SV_Position;
    nointerpolation float4 rect        : TEXCOORD0; /* x, y, w, h in target px */
    nointerpolation float4 params      : TEXCOORD1; /* radius, border, bevel, soft */
    nointerpolation float4 fill_top    : TEXCOORD2;
    nointerpolation float4 fill_bottom : TEXCOORD3;
    nointerpolation float4 border_col  : TEXCOORD4;
    nointerpolation float4 bevel_hi    : TEXCOORD5;
    nointerpolation float4 bevel_lo    : TEXCOORD6;
};

VSOut main(VSIn input, uint vid : SV_VertexID)
{
    RRectInstance u;
    u.dst_rect    = input.dst_rect;
    u.params      = input.params;
    u.fill_top    = input.fill_top;
    u.fill_bottom = input.fill_bottom;
    u.border_col  = input.border_col;
    u.bevel_hi    = input.bevel_hi;
    u.bevel_lo    = input.bevel_lo;

    float cx = (float)(vid & 1u);
    float cy = (float)((vid >> 1u) & 1u);

    /* Inflate so the AA / shadow falloff (half the soft width, +1 px
     * guard) survives rasterization. */
    float  inflate = u.params.w * 0.5f + 1.0f;
    float2 pos_px  = u.dst_rect.xy - inflate +
                     float2(cx, 1.0f - cy) * (u.dst_rect.zw + 2.0f * inflate);
    float2 pos = float2(pos_px.x * ndc_scale.x - 1.0f,
                        1.0f - pos_px.y * ndc_scale.y);

    float scale = output_rgb_scale;

    VSOut o;
    o.position    = float4(pos, 0.0f, 1.0f);
    o.rect        = u.dst_rect;
    o.params      = u.params;
    o.fill_top    = float4(u.fill_top.rgb * scale, u.fill_top.a);
    o.fill_bottom = float4(u.fill_bottom.rgb * scale, u.fill_bottom.a);
    o.border_col  = float4(u.border_col.rgb * scale, u.border_col.a);
    o.bevel_hi    = float4(u.bevel_hi.rgb * scale, u.bevel_hi.a);
    o.bevel_lo    = float4(u.bevel_lo.rgb * scale, u.bevel_lo.a);
    return o;
}
