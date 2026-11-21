#include "aeron/compat/host.h"
#include "internal.h"

#include "aeron/aeron.h"
#include "aeron/render.h"
#include "aeron/scene/present.h"
#include "aeron/surface.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* DirectDraw -> Aeron compatibility shim.
 *
 * The recovered frontend-display code calls IDirectDraw / IDirectDrawSurface /
 * IDirectDrawPalette through their vtables exactly as the original did; this
 * layer provides those objects backed by Aeron CPU surfaces. The Direct3D
 * device/texture side of a surface (render-target backing) lives in
 * d3d.c; the shared object layout is in internal.h. */

/* --- format helpers ------------------------------------------------------ */

static AeronPixelFormat DDShim_FormatForBpp(int bpp) {
	if (bpp == 8) {
		return AERON_PIXEL_FORMAT_INDEX8;
	}
	/* 16bpp frontend/flight surfaces default to RGB565 (the common hardware
	 * format); GetSurfaceDesc reports the matching green mask so the recovered
	 * code derives g_pixelFormat555 == 0. */
	return AERON_PIXEL_FORMAT_RGB565;
}

static int DDShim_BppForFormat(AeronPixelFormat fmt) { return fmt == AERON_PIXEL_FORMAT_INDEX8 ? 8 : 16; }

static void DDShim_FillPixelFormat(DDPIXELFORMAT* pf, AeronPixelFormat fmt) {
	memset(pf, 0, sizeof(*pf));
	pf->dwSize = sizeof(DDPIXELFORMAT);
	if (fmt == AERON_PIXEL_FORMAT_INDEX8) {
		pf->dwFlags       = DDPF_RGB | DDPF_PALETTEINDEXED8;
		pf->dwRGBBitCount = 8;
		return;
	}
	pf->dwFlags       = DDPF_RGB;
	pf->dwRGBBitCount = 16;
	if (fmt == AERON_PIXEL_FORMAT_RGB555) {
		pf->dwRBitMask = 0x7C00;
		pf->dwGBitMask = 0x03E0;
		pf->dwBBitMask = 0x001F;
	} else { /* RGB565 */
		pf->dwRBitMask = 0xF800;
		pf->dwGBitMask = 0x07E0;
		pf->dwBBitMask = 0x001F;
	}
}

/* Fills a DDSURFACEDESC (already sized by the caller) with a surface's geometry,
 * pitch, pixel format, and caps. */
static void DDShim_FillSurfaceDesc(DDrawSurfaceShim* s, DDSURFACEDESC* d) {
	int pitch = s->cpu ? AeronSurface_GetPitch(s->cpu) : (s->width * (s->bpp / 8));
	d->dwFlags |= DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_CAPS;
	d->dwWidth        = (uint32_t)s->width;
	d->dwHeight       = (uint32_t)s->height;
	d->lPitch         = pitch;
	d->ddsCaps.dwCaps = s->caps;
	if (s->has_pixel_format) {
		d->ddpfPixelFormat = s->pixel_format;
	} else {
		DDShim_FillPixelFormat(&d->ddpfPixelFormat, s->format);
	}
}

/* --- present ------------------------------------------------------------- */

/* The recovered present path reaches this through Flip / Blt-to-primary. A
 * render surface is the composition buffer for its background, 2D layers, and
 * 3D output; plain CPU surfaces carry their completed pixels directly. */
/* Recovered presents can occur many times inside one host frame, especially
 * while loading mission species. Retain the final completed image here and
 * submit it once after the game tick; opaque HD flight frames suppress the
 * render-target submission without disturbing the flip chain. */
typedef enum DDShimPresentKind {
	DDSHIM_PRESENT_BLT = 0,
	DDSHIM_PRESENT_FLIP,
} DDShimPresentKind;

static DDrawSurfaceShim* g_ddLastPresented;
static AeronSurface*     g_ddRetainedCpuPresentation;
static int               g_ddClassicFlightRenderingSuppressed;
static int               g_ddSuppressedPresentPending;
static DDShimPresentKind g_ddSuppressedPresentKind;
static uint64_t          g_ddClassicFlightFrameSerial;
static uint64_t          g_ddPaletteRevision;

static int DDShim_EnsureRenderTargetStaging(DDrawSurfaceShim* s);

/* SetEntries updates the shared object; SDL surfaces cache separate copies. */
static void DDShim_ApplyPalette(DDrawSurfaceShim* surface, DDrawPaletteShim* palette) {
	if (!surface || surface->format != AERON_PIXEL_FORMAT_INDEX8 || !surface->cpu || !palette) {
		return;
	}
	if (surface->applied_palette == palette && surface->applied_palette_revision == palette->revision) {
		return;
	}
	if (AeronSurface_SetPalette(surface->cpu, palette->entries, 256)) {
		surface->applied_palette          = palette;
		surface->applied_palette_revision = palette->revision;
	}
}

/* An indexed DirectDraw display uses the palette attached to the primary
 * surface when an attached or offscreen surface is presented. */
static void DDShim_ApplyPrimaryPalette(DDrawSurfaceShim* surface) {
	if (surface && surface->owner && surface->owner->primary) {
		DDShim_ApplyPalette(surface, surface->owner->primary->palette);
	}
}

void AeronDx5_SetClassicFlightRenderingSuppressed(int suppressed) {
	g_ddClassicFlightRenderingSuppressed = suppressed ? 1 : 0;
}

int AeronDx5_IsClassicFlightRenderingSuppressed(void) { return g_ddClassicFlightRenderingSuppressed; }

static int DDShim_ReadbackRenderTarget(DDrawSurfaceShim* s);

int AeronDx5_ReadbackPresentedFlightFrame(void* dst, int pitch, int* width, int* height, AeronPixelFormat* format) {
	DDrawSurfaceShim* s = g_ddLastPresented;
	int had_staging;
	int cpu_pitch = 0;
	void* pixels;
	int y;

	if (!dst || pitch <= 0 || !width || !height || !format)
		return 0;
	if (!s || !s->rt || s->kind != DDSHIM_RENDER_TARGET)
		return 0;
	if (g_ddClassicFlightRenderingSuppressed)
		return 0;
	if (s->format != AERON_PIXEL_FORMAT_RGB565 && s->format != AERON_PIXEL_FORMAT_RGB555)
		return 0;
	if (s->width <= 0 || s->height <= 0 || pitch < s->width * 2)
		return 0;
	/* ReadbackRenderTarget skips the copy when nothing is dirty; a staging
	 * surface that was never populated would then read back blank. */
	had_staging = s->cpu != NULL;
	if (!s->gpu_dirty && !had_staging)
		return 0;
	if (!DDShim_ReadbackRenderTarget(s))
		return 0;
	if (!s->cpu)
		return 0;
	pixels = AeronSurface_Lock(s->cpu, &cpu_pitch);
	if (!pixels || cpu_pitch < s->width * 2)
		return 0;
	for (y = 0; y < s->height; ++y)
		memcpy((uint8_t*)dst + (size_t)pitch * (size_t)y,
		       (const uint8_t*)pixels + (size_t)cpu_pitch * (size_t)y,
		       (size_t)s->width * 2);
	AeronSurface_Unlock(s->cpu);
	*width = s->width;
	*height = s->height;
	*format = s->format;
	return 1;
}

static AeronRectI DDShim_ClassicPresentationRect(const DDrawSurfaceShim* frame) {
	return AeronDx5_PresentationRect(frame ? frame->width : 0, frame ? frame->height : 0);
}

static int DDShim_RetainCpuPresentation(DDrawSurfaceShim* frame) {
	AeronPixelFrameView view;
	AeronSurface*       replacement;

	if (!frame || !frame->cpu)
		return 0;
	DDShim_ApplyPrimaryPalette(frame);
	if (!AeronSurface_GetFrameView(frame->cpu, &view))
		return 0;

	if (!g_ddRetainedCpuPresentation || AeronSurface_GetWidth(g_ddRetainedCpuPresentation) != view.width ||
		AeronSurface_GetHeight(g_ddRetainedCpuPresentation) != view.height ||
		AeronSurface_GetFormat(g_ddRetainedCpuPresentation) != view.format) {
		replacement = NULL;
		if (!AeronSurface_Create(view.width, view.height, view.format,
								 AERON_SURFACE_CPU_LOCKABLE | AERON_SURFACE_PRESENTABLE, &replacement))
			return 0;
		AeronSurface_Destroy(g_ddRetainedCpuPresentation);
		g_ddRetainedCpuPresentation = replacement;
	}

	if (view.format == AERON_PIXEL_FORMAT_INDEX8 &&
		(!view.palette || !AeronSurface_SetPalette(g_ddRetainedCpuPresentation, view.palette, 256)))
		return 0;
	return AeronSurface_Blit(g_ddRetainedCpuPresentation, 0, 0, frame->cpu, NULL, AERON_SURFACE_BLIT_NONE);
}

static int DDShim_CopyRenderTargetToFront(DDrawSurfaceShim* frame) {
	AeronCommandBuffer* command_buffer;
	AeronTexture*       source;
	AeronTexture*       destination;

	if (!frame->rt_back)
		return 1;
	source      = Aeron_RenderTargetGetTexture(frame->rt);
	destination = Aeron_RenderTargetGetTexture(frame->rt_back);
	if (!source || !destination)
		return 0;

	command_buffer = Aeron_AcquireCommandBuffer();
	if (!command_buffer) {
		Aeron_RequestFatalRendererError("DirectDraw primary blit command-buffer acquisition");
		return 0;
	}
	if (!Aeron_CopyTextureCmd(command_buffer, &(AeronTextureCopyDesc) {
												  .source      = source,
												  .destination = destination,
												  .width       = (uint32_t)frame->width,
												  .height      = (uint32_t)frame->height,
												  .cycle       = 1,
											  })) {
		Aeron_CancelCommandBuffer(command_buffer);
		Aeron_RequestFatalRendererError("DirectDraw primary blit copy");
		return 0;
	}
	if (!Aeron_SubmitCommandBuffer(command_buffer)) {
		Aeron_RequestFatalRendererError("DirectDraw primary blit submission");
		return 0;
	}
	return 1;
}

static int DDShim_CommitRenderTargetPresent(DDrawSurfaceShim* frame, int notify, DDShimPresentKind kind) {
	if (!D3DCompat_FlushRenderTargetPass(frame))
		return 0;
	/* If a software overlay was the last writer (cpu_dirty), fold its staging
	 * back into the color target first so the presented texture includes it. */
	DDShim_WritebackRenderTarget(frame);
	if (!Aeron_RenderTargetGetTexture(frame->rt))
		return 0;
	if (kind == DDSHIM_PRESENT_BLT && !DDShim_CopyRenderTargetToFront(frame))
		return 0;
	++g_ddClassicFlightFrameSerial;
	if (notify)
		AeronDx5_NotifyPresent(frame->width, frame->height);
	g_ddLastPresented = frame;
	/* Flip rotates the completed target into the visible half. Blt copied into
	 * that half without changing the render surface's current storage. */
	if (kind == DDSHIM_PRESENT_FLIP && frame->rt_back) {
		AeronRenderTarget* swap = frame->rt;
		frame->rt               = frame->rt_back;
		frame->rt_back          = swap;
		/* CPU staging still describes the target that was current before the swap. */
		frame->gpu_dirty = 1;
	}
	return 1;
}

static void DDShim_Present(DDrawSurfaceShim* frame, DDShimPresentKind kind) {
	if (!frame) {
		return;
	}

	if (frame->rt) {
		if (g_ddClassicFlightRenderingSuppressed) {
			/* Preserve the recovered present boundary and scheduler bookkeeping, but
			 * keep the last complete classic texture and flip-chain position intact.
			 * The CPU staging may continue receiving software draws. Retain this
			 * boundary so EndFrame can commit it if suppression is lifted after the
			 * recovered tick that produced it. */
			AeronDx5_NotifyPresent(frame->width, frame->height);
			g_ddLastPresented            = frame;
			g_ddSuppressedPresentPending = 1;
			g_ddSuppressedPresentKind    = kind;
			return;
		}
		g_ddSuppressedPresentPending = 0;
		(void)DDShim_CommitRenderTargetPresent(frame, 1, kind);
		return;
	}

	g_ddSuppressedPresentPending = 0;
	if (!frame->cpu) {
		return;
	}
	if (!DDShim_RetainCpuPresentation(frame)) {
		Aeron_RequestFatalRendererError("retaining classic software-surface presentation");
		return;
	}
	++g_ddClassicFlightFrameSerial;
	/* Remaster: frame-boundary marker (see above). */
	AeronDx5_NotifyPresent(frame->width, frame->height);
	g_ddLastPresented = frame;
}

/* VolkyTFColl v13: brightness diagnostic logging (tie.brightness).
 * Rate-limited: log on brightness change, otherwise at most once
 * every 600 calls (~10 s at 60 fps) — no per-frame spam. The first
 * call always logs (last_b starts at -1), so the log shows at once
 * whether this path runs at all and which branch it takes. */
static float s_ddshim_brightness_log_last_b = -1.0f;
static int s_ddshim_brightness_log_tick = 0;

static void DDShim_LogBrightness(const char* where, float b, int tint) {
	++s_ddshim_brightness_log_tick;
	if (b != s_ddshim_brightness_log_last_b || (s_ddshim_brightness_log_tick % 600) == 0) {
		s_ddshim_brightness_log_last_b = b;
		Aeron_LogInfo("tie.brightness", "%s b=%.2f tint=%d", where, (double)b, tint);
	}
}

/* VolkyTFColl v14: CPU-side brightness for the retained CPU presentation.
 * The GPU fragment-uniform tint (v11/v12) is ignored by this device's
 * fullscreen composition pipeline (Mali G52 / Vulkan / SDL GPU): the tint
 * is submitted, copied and pushed, the compiled shader multiplies by it,
 * yet the screen never changes (v13 log: DX5 submit branch=retained
 * b=2.00 tint=1 every frame, no effect). So apply the brightness on
 * the CPU while expanding the INDEX8+palette frame to RGBA8888 — the
 * exact math of the on-screen tint (sRGB decode -> linear -> gamma
 * lift -> sRGB encode, byte-identical to screenshot.c's
 * Screenshot_BrightenedPaletteByte). VolkyTFColl v15: the v14 linear
 * factor barely lifted the shadows (sRGB 16 -> x2.0 -> ~26), so the
 * factor is a gamma lift now: out_linear = powf(in_linear, 1.0f / b).
 * Palette entries are used directly
 * as 8-bit values (Aeron_ConvertIndexed8PixelFrameToRgba8 does the same;
 * no extra <<2). Single application: layer.tint_enabled stays 0.
 * b == 1.0 takes the old path with no copy (zero performance impact). */
static uint8_t* s_ddBrightenedScratch       = NULL;
static size_t   s_ddBrightenedScratchBytes  = 0;
static uint32_t s_ddBrightenedScratchSerial = 0;

static float DDShim_Srgb8ToLinear(uint8_t value) {
	float srgb = (float)value / 255.0f;
	return srgb <= 0.04045f ? srgb / 12.92f : powf((srgb + 0.055f) / 1.055f, 2.4f);
}

static uint8_t DDShim_LinearToSrgb8(float value) {
	float srgb;
	if (!(value > 0.0f))
		return 0;
	if (value > 1.0f)
		value = 1.0f;
	srgb = value <= 0.0031308f ? 12.92f * value : 1.055f * powf(value, 1.0f / 2.4f) - 0.055f;
	if (srgb < 0.0f)
		srgb = 0.0f;
	if (srgb > 1.0f)
		srgb = 1.0f;
	return (uint8_t)(srgb * 255.0f + 0.5f);
}

/* VolkyTFColl v15: gamma lift instead of the v14 linear factor. A linear
 * multiply barely lifts the shadows (sRGB 16 -> x2.0 -> ~26), while
 * out_linear = powf(in_linear, 1.0f / b) raises them visibly (b=2.0:
 * 16 -> ~76, 80 -> ~146) and keeps the highlights (200 -> ~226).
 * b=1.0 is the identity; b=0.5 darkens consistently (80 -> ~19).
 * Same curve as Screenshot_BrightenedPaletteByte and the HD flight
 * capture path in screenshot.c. */
static float DDShim_ApplyBrightnessGamma(float linear, float brightness) {
	if (brightness == 1.0f || !(linear > 0.0f))
		return linear;
	return powf(linear, 1.0f / brightness);
}

/* Expand an INDEX8+palette view into the scratch RGBA8888 buffer with the
 * brightness factor applied per channel in linear space. Returns 1 and
 * fills *out on success; 0 leaves *out untouched (caller keeps the old
 * path). The scratch is reused across frames; generation bumps every
 * submit so the upload cache never holds a stale frame (it keys on
 * generation + pixels address). */
static int DDShim_BrightenRetainedView(const AeronPixelFrameView* view, float brightness,
				       AeronPixelFrameView* out) {
	size_t bytes;
	int    y;

	if (!view || !view->pixels || !view->palette || view->width <= 0 || view->height <= 0)
		return 0;
	bytes = (size_t)view->width * (size_t)view->height * 4u;
	if (!s_ddBrightenedScratch || s_ddBrightenedScratchBytes < bytes) {
		uint8_t* replacement = (uint8_t*)realloc(s_ddBrightenedScratch, bytes);
		if (!replacement)
			return 0;
		s_ddBrightenedScratch      = replacement;
		s_ddBrightenedScratchBytes = bytes;
	}
	for (y = 0; y < view->height; ++y) {
		const uint8_t* src_row = (const uint8_t*)view->pixels + (size_t)y * (size_t)view->pitch;
		uint8_t*       dst_row = s_ddBrightenedScratch + (size_t)y * (size_t)view->width * 4u;
		int            x;
		for (x = 0; x < view->width; ++x) {
			const AeronPaletteEntry* entry = &view->palette[src_row[x]];
			uint8_t*               out_px = dst_row + (size_t)x * 4u;
			out_px[0] = DDShim_LinearToSrgb8(DDShim_ApplyBrightnessGamma(DDShim_Srgb8ToLinear(entry->r), brightness));
			out_px[1] = DDShim_LinearToSrgb8(DDShim_ApplyBrightnessGamma(DDShim_Srgb8ToLinear(entry->g), brightness));
			out_px[2] = DDShim_LinearToSrgb8(DDShim_ApplyBrightnessGamma(DDShim_Srgb8ToLinear(entry->b), brightness));
			out_px[3] = entry->a;
		}
	}
	*out            = *view;
	out->pixels     = s_ddBrightenedScratch;
	out->pitch      = view->width * 4;
	out->bpp        = 32;
	out->format     = AERON_PIXEL_FORMAT_RGBA8888;
	out->palette    = NULL;
	out->generation = ++s_ddBrightenedScratchSerial;
	return 1;
}

static void DDShim_SubmitLastPresented(DDrawSurfaceShim* frame) {
	if (!frame) {
		/* VolkyTFColl v13: diagnostic — nothing presented at all. */
		DDShim_LogBrightness("DX5 submit branch=none", AeronScenePresent_Brightness(), 0);
		return;
	}
	if (frame->rt) {
		/* Double-buffered render surface: after the present-time swap
		 * the last COMPLETE frame lives in rt_back (rt receives the
		 * next frame's writes); single-buffered falls back to rt. */
		AeronRenderTarget*    src = frame->rt_back ? frame->rt_back : frame->rt;
		AeronTextureLayerDesc tex;
		memset(&tex, 0, sizeof(tex));
		tex.texture      = Aeron_RenderTargetGetTexture(src);
		tex.logical_rect = DDShim_ClassicPresentationRect(frame);
		tex.blend_mode   = AERON_LAYER_BLEND_OPAQUE;
		tex.color_space  = AERON_COLOR_SPACE_SRGB;
		/* VolkyTFColl v12: user brightness. The classic pixel layer
		 * (v11) never presents TIE98 scenes — the briefing room, menus
		 * and classic flight all come through this DX5 texture layer
		 * (EndFrame) — so the tint must live here too. Same linear
		 * factor as the flight tonemap exposure (u[4]); 100% = identity.
		 * No double application: HD flight suppresses this path
		 * (EndFrame returns early) and takes brightness from u[4]
		 * instead; the classic pixel layer is never submitted for a
		 * DX5-presented frame. */
		{
			const float brightness = AeronScenePresent_Brightness();
			if (brightness != 1.0f) {
				tex.tint_enabled = 1;
				tex.tint_rgba[0] = brightness;
				tex.tint_rgba[1] = brightness;
				tex.tint_rgba[2] = brightness;
				tex.tint_rgba[3] = 1.0f;
			}
			/* VolkyTFColl v13: diagnostic — which branch, live b, tint. */
			DDShim_LogBrightness("DX5 submit branch=rt", brightness, tex.tint_enabled);
		}
		if (tex.texture) {
			if (!Aeron_SubmitTextureLayer(&tex)) {
				Aeron_RequestFatalRendererError("retained classic texture presentation");
			}
		}
		return;
	}
	if (g_ddRetainedCpuPresentation) {
		AeronPixelFrameView view;
		AeronPixelLayerDesc layer;
		const float         brightness = AeronScenePresent_Brightness();
		int                 cpu_brightened = 0;
		if (!AeronSurface_GetFrameView(g_ddRetainedCpuPresentation, &view)) {
			return;
		}
		memset(&layer, 0, sizeof(layer));
		layer.frame        = view;
		layer.logical_rect = DDShim_ClassicPresentationRect(frame);
		layer.blend_mode   = AERON_LAYER_BLEND_OPAQUE;
		layer.sampling     = AERON_PIXEL_SAMPLING_SHARP_BILINEAR;
		/* VolkyTFColl v14: CPU-side brightness (DDShim_BrightenRetainedView).
		 * The v12 GPU tint is kept only as a fallback for non-INDEX8
		 * retained frames (or a scratch allocation failure); for
		 * INDEX8+palette frames the brightness is baked into the scratch
		 * pixels and tint stays off — single application, no double
		 * multiply. */
		if (brightness != 1.0f && view.format == AERON_PIXEL_FORMAT_INDEX8 && view.palette) {
			AeronPixelFrameView bright;
			if (DDShim_BrightenRetainedView(&view, brightness, &bright)) {
				layer.frame    = bright;
				cpu_brightened = 1;
			}
		}
		if (!cpu_brightened && brightness != 1.0f) {
			layer.tint_enabled = 1;
			layer.tint_rgba[0] = brightness;
			layer.tint_rgba[1] = brightness;
			layer.tint_rgba[2] = brightness;
			layer.tint_rgba[3] = 1.0f;
		}
		/* VolkyTFColl v13: diagnostic — which branch, live b, tint. */
		DDShim_LogBrightness("DX5 submit branch=retained", brightness, layer.tint_enabled);
		if (!Aeron_SubmitPixelLayer(&layer)) {
			Aeron_RequestFatalRendererError("retained classic software-surface presentation");
		}
	}
}

void AeronDx5_ForceSubmitRetainedFrame(void) { DDShim_SubmitLastPresented(g_ddLastPresented); }

void AeronDx5_EndFrame(void) {
	/* VolkyTFColl v13: diagnostic heartbeat — which presentation path
	 * runs (suppression / retained frame present / pending present).
	 * VolkyTFColl v14: separate counter. v13 shared the submit log's
	 * tick: EndFrame and the submit log each bumped it once per frame,
	 * so EndFrame always saw an odd tick and tick % 600 == 0 (600 even)
	 * could never fire — the heartbeat was dead code. */
	static int s_ddshim_endframe_log_tick = 0;
	if (++s_ddshim_endframe_log_tick >= 600) {
		s_ddshim_endframe_log_tick = 0;
		Aeron_LogInfo("tie.brightness", "Dx5 EndFrame suppressed=%d has_frame=%d pending=%d",
			(int)g_ddClassicFlightRenderingSuppressed, (int)(g_ddLastPresented != NULL),
			(int)g_ddSuppressedPresentPending);
	}
	if (g_ddClassicFlightRenderingSuppressed)
		return;
	/* TIE can enter a classic in-flight room inside the engine tick. The new
	 * snapshot lifts suppression only after that tick, so complete the retained
	 * recovered present before submitting the host frame. */
	if (g_ddSuppressedPresentPending) {
		g_ddSuppressedPresentPending = 0;
		/* The recovered boundary already notified its consumer while suppressed. */
		if (g_ddLastPresented) {
			(void)DDShim_CommitRenderTargetPresent(g_ddLastPresented, 0, g_ddSuppressedPresentKind);
		}
	}
	DDShim_SubmitLastPresented(g_ddLastPresented);
}

uint64_t AeronDx5_GetClassicFlightFrameSerial(void) { return g_ddClassicFlightFrameSerial; }

void AeronDx5_ResetPresentationState(void) {
	g_ddLastPresented                    = NULL;
	g_ddClassicFlightRenderingSuppressed = 0;
	g_ddSuppressedPresentPending         = 0;
	g_ddSuppressedPresentKind            = DDSHIM_PRESENT_BLT;
	g_ddClassicFlightFrameSerial         = 0;
}

void DDShim_ReleasePresentationResources(void) {
	AeronSurface_Destroy(g_ddRetainedCpuPresentation);
	g_ddRetainedCpuPresentation = NULL;
	/* VolkyTFColl v14: free the CPU-brightness scratch. */
	free(s_ddBrightenedScratch);
	s_ddBrightenedScratch      = NULL;
	s_ddBrightenedScratchBytes = 0;
}

/* Composite a source CPU surface onto a render surface's render target. The render
 * surface is the persistent composition buffer, so every blit into it -- the
 * frontend background bitmap (offscreen->front) then the 2D UI (back->front) --
 * accumulates on the render target underneath / over the 3D. Honors the source
 * rectangle, destination offset, and color key so attached frontend surfaces keep
 * the original DirectDraw placement. Returns 1 if it handled the blit. */
static int DDShim_ComposeOntoRenderTarget(DDrawSurfaceShim* dst, DDrawSurfaceShim* src, int dstX, int dstY,
										  const AeronSurfaceRect* srcRect, int colorKey) {
	AeronPixelFrameView view;
	AeronPixelLayerDesc layer;
	int                 bytesPerPixel;

	if (!dst->rt || !src || !src->cpu) {
		return 0;
	}
	if (g_ddClassicFlightRenderingSuppressed) {
		const uint32_t flags = colorKey && src->has_colorkey ? AERON_SURFACE_BLIT_COLOR_KEY : 0;
		if (!DDShim_EnsureRenderTargetStaging(dst) ||
			!AeronSurface_Blit(dst->cpu, dstX, dstY, src->cpu, srcRect, flags)) {
			return 0;
		}
		dst->cpu_dirty = 1;
		return 1;
	}
	if (!AeronSurface_GetFrameView(src->cpu, &view)) {
		return 0;
	}
	if (srcRect) {
		bytesPerPixel = view.bpp / 8;
		if (bytesPerPixel <= 0 || srcRect->x < 0 || srcRect->y < 0 || srcRect->w <= 0 || srcRect->h <= 0 ||
			srcRect->x + srcRect->w > view.width || srcRect->y + srcRect->h > view.height) {
			return 0;
		}
		view.pixels = (const unsigned char*)view.pixels + (size_t)srcRect->y * (size_t)view.pitch +
					  (size_t)srcRect->x * (size_t)bytesPerPixel;
		view.width  = srcRect->w;
		view.height = srcRect->h;
	}
	memset(&layer, 0, sizeof(layer));
	layer.frame               = view;
	layer.logical_rect.x      = dstX;
	layer.logical_rect.y      = dstY;
	layer.logical_rect.width  = view.width;
	layer.logical_rect.height = view.height;
	layer.sampling            = AERON_PIXEL_SAMPLING_SHARP_BILINEAR;
	/* The render target holds display-space (already-encoded) values -- std3D draws
	 * encoded output into it, matching the original DirectDraw surface. Preserve the
	 * 2D pixels as-is instead of sRGB-decoding them, so 2D and 3D share one space and
	 * the present applies a single decode. */
	layer.preserve_encoded_values = 1;
	if (colorKey && src->has_colorkey) {
		/* Color-key blit: the upload marks keyed pixels alpha=0; alpha blending
		 * then leaves the render target untouched there (revealing the background
		 * and 3D underneath) and replaces it with the opaque foreground elsewhere. */
		layer.blend_mode        = AERON_LAYER_BLEND_ALPHA;
		layer.color_key_enabled = 1;
		layer.color_key         = src->colorkey;
	} else {
		/* Opaque blit (the background restore): full overwrite. */
		layer.blend_mode = AERON_LAYER_BLEND_OPAQUE;
	}
	if (!Aeron_ComposePixelLayerToRenderTarget(dst->rt, &layer, 0, NULL)) {
		Aeron_RequestFatalRendererError("DirectDraw render-target composition");
		return 0;
	}
	dst->gpu_dirty = 1;
	dst->cpu_dirty = 0;
	return 1;
}

static int DDShim_ClearGpuTargetColor(const DDrawSurfaceShim* s, AeronRenderTarget* target, uint32_t fill) {
	AeronCommandBuffer* command_buffer;
	AeronRenderPass*    pass;
	float               rgba[4];
	int                 r;
	int                 g;
	int                 b;

	if (!s || !target) {
		return 0;
	}
	if (s->format == AERON_PIXEL_FORMAT_RGB555) {
		r       = (int)((fill >> 10) & 0x1F);
		g       = (int)((fill >> 5) & 0x1F);
		b       = (int)(fill & 0x1F);
		rgba[0] = (float)r / 31.0f;
		rgba[1] = (float)g / 31.0f;
		rgba[2] = (float)b / 31.0f;
	} else { /* RGB565 */
		r       = (int)((fill >> 11) & 0x1F);
		g       = (int)((fill >> 5) & 0x3F);
		b       = (int)(fill & 0x1F);
		rgba[0] = (float)r / 31.0f;
		rgba[1] = (float)g / 63.0f;
		rgba[2] = (float)b / 31.0f;
	}
	rgba[3] = 1.0f;

	command_buffer = Aeron_AcquireCommandBuffer();
	if (!command_buffer) {
		Aeron_RequestFatalRendererError("DirectDraw clear command-buffer acquisition");
		return 0;
	}
	pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
		.color_target     = target,
		.depth_target     = NULL,
		.viewport         = { 0, 0, s->width, s->height },
		.scissor          = { 0, 0, s->width, s->height },
		.clear_color      = 1,
		.clear_color_rgba = { rgba[0], rgba[1], rgba[2], rgba[3] },
		.command_buffer   = command_buffer,
	});
	if (!pass) {
		Aeron_CancelCommandBuffer(command_buffer);
		Aeron_RequestFatalRendererError("DirectDraw render-target clear");
		return 0;
	}
	Aeron_EndRenderPass(pass);
	if (!Aeron_SubmitCommandBuffer(command_buffer)) {
		Aeron_RequestFatalRendererError("DirectDraw render-target clear submission");
		return 0;
	}
	return 1;
}

/* Clears a render target to a raw 16bpp fill value (the surface's RGB565/RGB555
 * format), matching a DirectDraw DDBLT_COLORFILL on the 3D back buffer. Keep the
 * CPU staging synchronized with the known GPU clear so the original CPU starfield
 * can lock it without downloading the same clear image. Depth is left untouched
 * (std3D clears it separately). */
static int DDShim_ClearRenderTargetColor(DDrawSurfaceShim* s, uint32_t fill) {
	int staging_cleared;

	if (!s->rt) {
		return 0;
	}
	staging_cleared = DDShim_EnsureRenderTargetStaging(s) && AeronSurface_Clear(s->cpu, fill);
	if (g_ddClassicFlightRenderingSuppressed) {
		if (staging_cleared) {
			s->cpu_dirty = 1;
			s->gpu_dirty = 0;
		}
		return staging_cleared;
	}
	/* End any open scene pass before clearing so the load-op clear is not sequenced
	 * against an in-flight render pass on the same target. */
	if (!D3DCompat_FlushRenderTargetPass(s) || !DDShim_ClearGpuTargetColor(s, s->rt, fill)) {
		return 0;
	}
	if (staging_cleared) {
		/* Both representations contain the exact same full-surface clear. */
		s->gpu_dirty = 0;
		s->cpu_dirty = 0;
	} else {
		s->gpu_dirty = 1;
		s->cpu_dirty = 0;
	}
	return 1;
}

/* A primary surface is a presentation sink in the shim, so its visible pixels
 * live in the attached render surface's completed half. The original clears the
 * primary and back buffer separately before an attached frontend sequence; honor
 * the primary clear without disturbing the current work target. */
static int DDShim_ClearPrimaryColor(DDrawSurfaceShim* primary, uint32_t fill) {
	DDrawSurfaceShim* frame;

	if (!primary || primary->kind != DDSHIM_PRIMARY) {
		return 0;
	}
	frame = primary->attached;
	if (!frame || frame->kind != DDSHIM_RENDER_TARGET || !frame->rt_back) {
		return 1;
	}
	return DDShim_ClearGpuTargetColor(frame, frame->rt_back, fill);
}

void DDShim_WritebackRenderTarget(DDrawSurfaceShim* s) {
	if (g_ddClassicFlightRenderingSuppressed || !s || !s->rt || !s->cpu || !s->cpu_dirty) {
		return;
	}
	/* The staging holds the newest pixels (a software overlay drew into it after a
	 * readback). Compose it opaquely into the color target -- the same display-space
	 * preserve-encoded path as a background restore -- so the GPU pass or present
	 * that follows sees the CPU content. The color target is now authoritative. */
	if (!DDShim_ComposeOntoRenderTarget(s, s, 0, 0, NULL, 0)) {
		return;
	}
	s->cpu_dirty = 0;
	s->gpu_dirty = 1;
}

int AeronDx5_ComposeSurfaceOverRenderTarget(IDirectDrawSurface* dst, int dst_x, int dst_y,
											IDirectDrawSurface* src, const uint8_t* coverage,
											int coverage_pitch) {
	DDrawSurfaceShim*   d = (DDrawSurfaceShim*)dst;
	DDrawSurfaceShim*   s = (DDrawSurfaceShim*)src;
	AeronPixelLayerDesc layer;
	AeronPixelFrameView view;

	if (!d || !s || !coverage || d->kind != DDSHIM_RENDER_TARGET || !d->rt || !s->cpu) {
		return 0;
	}
	/* Order the composition after every earlier target writer. CPU overlays can
	 * remain pending when no Direct3D scene follows their surface unlock. */
	if (!D3DCompat_FlushRenderTargetPass(d)) {
		return 0;
	}
	DDShim_WritebackRenderTarget(d);
	if (d->cpu_dirty) {
		return 0;
	}
	if (!AeronSurface_GetFrameView(s->cpu, &view)) {
		return 0;
	}

	memset(&layer, 0, sizeof(layer));
	layer.frame               = view;
	layer.logical_rect.x      = dst_x;
	layer.logical_rect.y      = dst_y;
	layer.logical_rect.width  = view.width;
	layer.logical_rect.height = view.height;
	layer.sampling            = AERON_PIXEL_SAMPLING_SHARP_BILINEAR;
	/* Same display-space (already-encoded) convention as DDShim_ComposeOntoRenderTarget. */
	layer.preserve_encoded_values = 1;
	layer.blend_mode              = AERON_LAYER_BLEND_ALPHA;
	layer.coverage                = coverage;
	layer.coverage_pitch          = coverage_pitch;
	if (!Aeron_ComposePixelLayerToRenderTarget(d->rt, &layer, 0, NULL)) {
		Aeron_RequestFatalRendererError("DirectDraw coverage composition");
		return 0;
	}
	d->gpu_dirty = 1;
	return 1;
}

/* Lazily creates the 16bpp CPU staging surface a render target uses for known
 * clears and readback/writeback when a software overlay locks it (Model B).
 * Render targets that are neither cleared nor CPU-locked pay nothing. */
static int DDShim_EnsureRenderTargetStaging(DDrawSurfaceShim* s) {
	if (s->cpu) {
		return 1;
	}
	if (!AeronSurface_Create(s->width, s->height, s->format,
							 AERON_SURFACE_CPU_LOCKABLE | AERON_SURFACE_PRESENTABLE, &s->cpu)) {
		return 0;
	}
	if (s->has_colorkey) {
		AeronSurface_SetColorKey(s->cpu, 1, s->colorkey);
	}
	return 1;
}

static int DDShim_ReadbackRenderTarget(DDrawSurfaceShim* s) {
	int   pitch;
	void* pixels;
	if (!s || s->kind != DDSHIM_RENDER_TARGET)
		return 1;
	if (!DDShim_EnsureRenderTargetStaging(s))
		return 0;
	if (!s->gpu_dirty || g_ddClassicFlightRenderingSuppressed)
		return 1;
	if (!D3DCompat_FlushRenderTargetPass(s))
		return 0;
	pixels = AeronSurface_Lock(s->cpu, &pitch);
	if (!pixels)
		return 0;
	if (!Aeron_ReadRenderTargetPixels(s->rt, pixels, pitch, s->format)) {
		AeronSurface_Unlock(s->cpu);
		Aeron_RequestFatalRendererError("DirectDraw render-target readback");
		return 0;
	}
	AeronSurface_Unlock(s->cpu);
	s->gpu_dirty = 0;
	s->cpu_dirty = 0;
	return 1;
}

/* --- IDirectDrawSurface -------------------------------------------------- */

/* Win32 tagRECT the recovered code passes as the RECT arg. */
typedef struct DDShimRect {
	int32_t left;
	int32_t top;
	int32_t right;
	int32_t bottom;
} DDShimRect;

static int DDShim_RectToSurfaceRect(const void* rc, AeronSurfaceRect* out) {
	const DDShimRect* r = (const DDShimRect*)rc;
	if (!r) {
		return 0;
	}
	out->x = r->left;
	out->y = r->top;
	out->w = r->right - r->left;
	out->h = r->bottom - r->top;
	return 1;
}

static HRESULT AERON_DXAPI DDSurface_QueryInterface(IDirectDrawSurface* self, DxRefIid iid, void** out) {
	DDrawSurfaceShim* s = (DDrawSurfaceShim*)self;
	IDirect3DDevice*  device;

	if (!out) {
		return DX_E_INVALIDARG;
	}
	*out = NULL;

	/* std3D QIs its render-target surface for the Direct3D device (HAL or RGB
	 * GUID). Lazily promote this surface to a render target and bind a device. */
	if (DxGuidEqual(iid, &IID_IDirect3DHALDevice_Compat) ||
		DxGuidEqual(iid, &IID_IDirect3DRGBDevice_Compat)) {
		device = D3DCompat_CreateDeviceForSurface(s);
		if (!device) {
			return DX_E_FAIL;
		}
		*out = device;
		return DX_DD_OK;
	}

	/* std3D QIs a texture surface for IDirect3DTexture to get a handle and Load it. */
	if (DxGuidEqual(iid, &IID_IDirect3DTexture_Compat)) {
		IDirect3DTexture* texture = D3DCompat_CreateTexture(s);
		if (!texture) {
			return DX_E_FAIL;
		}
		*out = texture;
		return DX_DD_OK;
	}
	return DX_E_NOTIMPL;
}

static uint32_t AERON_DXAPI DDSurface_AddRef(IDirectDrawSurface* self) {
	return (uint32_t)++((DDrawSurfaceShim*)self)->refcount;
}

static uint32_t AERON_DXAPI DDSurface_Release(IDirectDrawSurface* self) {
	DDrawSurfaceShim* s = (DDrawSurfaceShim*)self;
	if (--s->refcount > 0) {
		return (uint32_t)s->refcount;
	}
	if (s == g_ddLastPresented) {
		/* Idle re-present must not touch a freed surface, and a present retained
		 * while classic rendering was suppressed can no longer be committed. */
		g_ddLastPresented            = NULL;
		g_ddSuppressedPresentPending = 0;
	}
	if (s->attached) {
		(void)s->attached->lpVtbl->Release((IDirectDrawSurface*)s->attached);
	}
	if (s->zbuffer) {
		(void)s->zbuffer->lpVtbl->Release((IDirectDrawSurface*)s->zbuffer);
	}
	if (s->palette) {
		(void)((IDirectDrawPalette*)s->palette)->lpVtbl->Release((IDirectDrawPalette*)s->palette);
	}
	/* Destroy the render-target backing (created lazily in D3DCompat_CreateDeviceForSurface
	 * for a DDSHIM_RENDER_TARGET). On a partial-creation failure these are already NULL. */
	if (s->rt) {
		Aeron_DestroyRenderTarget(s->rt);
	}
	if (s->rt_back) {
		Aeron_DestroyRenderTarget(s->rt_back);
	}
	if (s->depth) {
		Aeron_DestroyDepthTarget(s->depth);
	}
	if (s->cpu) {
		AeronSurface_Destroy(s->cpu);
	}
	free(s->pending_depth_upload_data);
	free(s);
	return 0;
}

void AeronDx5_CommitDepthSurfaceRect(IDirectDrawSurface* surface, const AeronDx5Rect* rect) {
	DDrawSurfaceShim*   depth_surface = (DDrawSurfaceShim*)surface;
	DDrawSurfaceShim*   render_surface;
	AeronPixelFrameView view;
	uint8_t*            upload_data;
	uint32_t            upload_size;
	int                 y;

	if (!depth_surface || !rect || !(render_surface = depth_surface->attached_to) || !render_surface->depth ||
		!depth_surface->cpu || rect->x < 0 || rect->y < 0 || rect->width <= 0 || rect->height <= 0 ||
		rect->x > depth_surface->width - rect->width || rect->y > depth_surface->height - rect->height ||
		(uint32_t)rect->width > UINT32_MAX / 2u / (uint32_t)rect->height ||
		!AeronSurface_GetFrameView(depth_surface->cpu, &view) || view.bpp != 16) {
		Aeron_RequestFatalRendererError("DirectDraw depth-surface rectangle commit");
		return;
	}
	if (!D3DCompat_FlushRenderTargetPass(render_surface)) {
		return;
	}

	upload_size = (uint32_t)rect->width * (uint32_t)rect->height * 2u;
	if (render_surface->pending_depth_upload_capacity < upload_size) {
		upload_data = (uint8_t*)realloc(render_surface->pending_depth_upload_data, upload_size);
		if (!upload_data) {
			Aeron_RequestFatalRendererError("DirectDraw depth-surface upload allocation");
			return;
		}
		render_surface->pending_depth_upload_data     = upload_data;
		render_surface->pending_depth_upload_capacity = upload_size;
	}
	for (y = 0; y < rect->height; ++y) {
		const uint8_t* source =
			(const uint8_t*)view.pixels + (size_t)(rect->y + y) * (size_t)view.pitch + (size_t)rect->x * 2u;
		memcpy(render_surface->pending_depth_upload_data + (size_t)y * (size_t)rect->width * 2u, source,
			   (size_t)rect->width * 2u);
	}
	render_surface->pending_depth_upload_rect = (AeronRectI) { rect->x, rect->y, rect->width, rect->height };
	render_surface->pending_depth_upload      = 1;
}

/* Attach a z-buffer to a render surface. The render-target backing owns the real
 * depth target. The CPU attachment also stages explicit depth writes made by
 * recovered code; std3D releases it at teardown. */
static HRESULT AERON_DXAPI DDSurface_AddAttachedSurface(IDirectDrawSurface* self,
														IDirectDrawSurface* attach) {
	DDrawSurfaceShim* s = (DDrawSurfaceShim*)self;
	if (!attach) {
		return DX_E_INVALIDARG;
	}
	attach->lpVtbl->AddRef(attach);
	s->zbuffer                               = (DDrawSurfaceShim*)attach;
	((DDrawSurfaceShim*)attach)->attached_to = s;
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDSurface_DeleteAttachedSurface(IDirectDrawSurface* self, uint32_t flags,
														   IDirectDrawSurface* attach) {
	DDrawSurfaceShim* s = (DDrawSurfaceShim*)self;
	DDrawSurfaceShim* a = (DDrawSurfaceShim*)attach;
	(void)flags;
	if (!a) {
		return DX_E_INVALIDARG;
	}
	/* Detach the z-buffer (std3D_DestroyDevice detaches before releasing it). Clear
	 * both links so the later render-surface Release does not touch a freed surface,
	 * then drop the reference AddAttachedSurface took. */
	if (s->zbuffer == a) {
		s->zbuffer = NULL;
	}
	if (a->attached_to == s) {
		a->attached_to = NULL;
	}
	attach->lpVtbl->Release(attach);
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDSurface_Blt(IDirectDrawSurface* self, void* dstRect, IDirectDrawSurface* src,
										 void* srcRect, uint32_t flags, DDBLTFX* fx) {
	DDrawSurfaceShim* d = (DDrawSurfaceShim*)self;
	if ((flags & DDBLT_ROP) && (!fx || fx->dwROP != DDROP_SRCCOPY))
		return DX_E_FAIL;

	if (flags & DDBLT_COLORFILL) {
		uint32_t         color = fx ? fx->dwFillColor : 0;
		AeronSurfaceRect r;
		if (d->kind == DDSHIM_PRIMARY) {
			/* The presentation proxy can model a full visible-surface clear through
			 * the completed flip-chain target. Partial primary fills are unused by
			 * the recovered flight paths and have no proxy backing to modify. */
			if (dstRect) {
				return DX_DD_OK;
			}
			return DDShim_ClearPrimaryColor(d, color) ? DX_DD_OK : DX_E_FAIL;
		}
		/* On a render target the fill clears the GPU color target (the flight 3D back
		 * buffer clear, matching the original DDBLT_COLORFILL on g_flightBackBuffer). */
		if (d->kind == DDSHIM_RENDER_TARGET && d->rt) {
			return DDShim_ClearRenderTargetColor(d, color) ? DX_DD_OK : DX_E_FAIL;
		}
		if (!d->cpu) {
			return DX_DD_OK;
		}
		if (DDShim_RectToSurfaceRect(dstRect, &r)) {
			return AeronSurface_ClearRect(d->cpu, &r, color) ? DX_DD_OK : DX_E_FAIL;
		}
		return AeronSurface_Clear(d->cpu, color) ? DX_DD_OK : DX_E_FAIL;
	}
	if (flags & DDBLT_DEPTHFILL) {
		/* Depth clear. The z-buffer surface's render target owns the real depth, so
		 * record a pending clear consumed at the next BeginScene. The fill
		 * value is 0 (near) or the format max (far). */
		DDrawSurfaceShim* rt = d->attached_to ? d->attached_to : (d->kind == DDSHIM_RENDER_TARGET ? d : NULL);
		if (rt) {
			rt->pending_depth_upload      = 0;
			rt->pending_depth_clear       = 1;
			rt->pending_depth_clear_value = (fx && fx->dwFillDepth != 0) ? 1.0f : 0.0f;
		}
		return DX_DD_OK;
	}
	{
		DDrawSurfaceShim* s = (DDrawSurfaceShim*)src;
		AeronSurfaceRect  sr;
		int               have_sr = DDShim_RectToSurfaceRect(srcRect, &sr);
		DDShimRect*       dr      = (DDShimRect*)dstRect;
		if (d->kind == DDSHIM_PRIMARY) {
			DDShim_Present(s, DDSHIM_PRESENT_BLT);
			return DX_DD_OK;
		}
		DDShim_ApplyPalette(s, s ? s->palette : NULL);
		DDShim_ApplyPalette(d, d->palette);
		if (d->kind == DDSHIM_RENDER_TARGET && !D3DCompat_FlushRenderTargetPass(d)) {
			return DX_E_FAIL;
		}
		if (DDShim_ComposeOntoRenderTarget(d, s, dr ? dr->left : 0, dr ? dr->top : 0, have_sr ? &sr : NULL,
										   flags & DDBLT_KEYSRC)) {
			return DX_DD_OK;
		}
		if (Aeron_FatalErrorRequested()) {
			return DX_E_FAIL;
		}
		if (!DDShim_ReadbackRenderTarget(s)) {
			return DX_E_FAIL;
		}
		if (!d->cpu || !s || !s->cpu) {
			return DX_DD_OK;
		}
		return AeronSurface_Blit(d->cpu, dr ? dr->left : 0, dr ? dr->top : 0, s->cpu, have_sr ? &sr : NULL,
								 (flags & DDBLT_KEYSRC) ? AERON_SURFACE_BLIT_COLOR_KEY : 0)
				   ? DX_DD_OK
				   : DX_E_FAIL;
	}
}

static HRESULT AERON_DXAPI DDSurface_BltFast(IDirectDrawSurface* self, uint32_t x, uint32_t y,
											 IDirectDrawSurface* src, void* srcRect, uint32_t flags) {
	DDrawSurfaceShim* d = (DDrawSurfaceShim*)self;
	DDrawSurfaceShim* s = (DDrawSurfaceShim*)src;
	AeronSurfaceRect  sr;
	int               have_sr = DDShim_RectToSurfaceRect(srcRect, &sr);

	if (d->kind == DDSHIM_PRIMARY) {
		DDShim_Present(s, DDSHIM_PRESENT_BLT);
		return DX_DD_OK;
	}
	DDShim_ApplyPalette(s, s ? s->palette : NULL);
	DDShim_ApplyPalette(d, d->palette);
	if (d->kind == DDSHIM_RENDER_TARGET && !D3DCompat_FlushRenderTargetPass(d)) {
		return DX_E_FAIL;
	}
	if (DDShim_ComposeOntoRenderTarget(d, s, (int)x, (int)y, have_sr ? &sr : NULL,
									   flags & DDBLTFAST_SRCCOLORKEY)) {
		return DX_DD_OK;
	}
	if (Aeron_FatalErrorRequested()) {
		return DX_E_FAIL;
	}
	if (!DDShim_ReadbackRenderTarget(s)) {
		return DX_E_FAIL;
	}
	if (!d->cpu || !s || !s->cpu) {
		return DX_DD_OK;
	}
	return AeronSurface_Blit(d->cpu, (int)x, (int)y, s->cpu, have_sr ? &sr : NULL,
							 (flags & DDBLTFAST_SRCCOLORKEY) ? AERON_SURFACE_BLIT_COLOR_KEY : 0)
			   ? DX_DD_OK
			   : DX_E_FAIL;
}

static HRESULT AERON_DXAPI DDSurface_Flip(IDirectDrawSurface* self, IDirectDrawSurface* override,
										  uint32_t flags) {
	DDrawSurfaceShim* s = (DDrawSurfaceShim*)self;
	(void)flags;
	/* Flip makes `override` (or the attached back buffer) the visible frame. */
	DDShim_Present(override ? (DDrawSurfaceShim*) override : s->attached, DDSHIM_PRESENT_FLIP);
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDSurface_GetAttachedSurface(IDirectDrawSurface* self, DDSCAPS* caps,
														IDirectDrawSurface** out) {
	DDrawSurfaceShim* s          = (DDrawSurfaceShim*)self;
	DDrawSurfaceShim* attachment = NULL;
	if (!caps || !out) {
		return DX_E_INVALIDARG;
	}
	*out = NULL;
	if (s->attached && (s->attached->caps & caps->dwCaps) == caps->dwCaps) {
		attachment = s->attached;
	} else if (s->zbuffer && (s->zbuffer->caps & caps->dwCaps) == caps->dwCaps) {
		attachment = s->zbuffer;
	}
	if (!attachment) {
		return DX_E_FAIL;
	}
	attachment->lpVtbl->AddRef((IDirectDrawSurface*)attachment);
	*out = (IDirectDrawSurface*)attachment;
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDSurface_GetColorKey(IDirectDrawSurface* self, uint32_t flags, DDCOLORKEY* key) {
	DDrawSurfaceShim* s = (DDrawSurfaceShim*)self;
	(void)flags;
	if (!key) {
		return DX_E_INVALIDARG;
	}
	key->dwColorSpaceLowValue  = s->colorkey;
	key->dwColorSpaceHighValue = s->colorkey;
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDSurface_GetFlipStatus(IDirectDrawSurface* self, uint32_t flags) {
	(void)self;
	(void)flags;
	return DX_DD_OK; /* flip always complete under Aeron present */
}

static HRESULT AERON_DXAPI DDSurface_GetSurfaceDesc(IDirectDrawSurface* self, DDSURFACEDESC* desc) {
	DDrawSurfaceShim* s = (DDrawSurfaceShim*)self;
	if (!desc) {
		return DX_E_INVALIDARG;
	}
	DDShim_FillSurfaceDesc(s, desc);
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDSurface_IsLost(IDirectDrawSurface* self) {
	(void)self;
	return DX_DD_OK; /* Aeron surfaces are never lost */
}

static HRESULT AERON_DXAPI DDSurface_Lock(IDirectDrawSurface* self, void* rect, DDSURFACEDESC* desc,
										  uint32_t flags, void* event) {
	DDrawSurfaceShim* s = (DDrawSurfaceShim*)self;
	int               pitch;
	void*             pixels;
	(void)rect;
	(void)flags;
	(void)event;

	if (!desc) {
		return DX_E_FAIL;
	}
	/* A render target is CPU-lockable via readback (Model B software overlays lock
	 * the surface std3D rendered into). Allocate staging on demand, and flush any
	 * open GPU scene pass so the readback sees completed pixels. */
	if (s->kind == DDSHIM_RENDER_TARGET) {
		if (!DDShim_ReadbackRenderTarget(s))
			return DX_E_FAIL;
	}
	if (!s->cpu) {
		return DX_E_FAIL;
	}
	pixels = AeronSurface_Lock(s->cpu, &pitch);
	if (!pixels) {
		return DX_E_FAIL;
	}
	DDShim_FillSurfaceDesc(s, desc);
	desc->lpSurface = pixels;
	desc->lPitch    = pitch;
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDSurface_Restore(IDirectDrawSurface* self) {
	(void)self;
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDSurface_SetColorKey(IDirectDrawSurface* self, uint32_t flags, DDCOLORKEY* key) {
	DDrawSurfaceShim* s = (DDrawSurfaceShim*)self;
	(void)flags;
	if (!key) {
		return DX_E_INVALIDARG;
	}
	s->has_colorkey = 1;
	s->colorkey     = key->dwColorSpaceLowValue;
	if (s->cpu) {
		AeronSurface_SetColorKey(s->cpu, 1, s->colorkey);
	}
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDSurface_SetPalette(IDirectDrawSurface* self, IDirectDrawPalette* pal) {
	DDrawSurfaceShim* s = (DDrawSurfaceShim*)self;
	DDrawPaletteShim* p = (DDrawPaletteShim*)pal;
	/* SetPalette takes a reference on the palette (DirectDraw semantics); the surface
	 * holds it until a new palette is set or the surface is released. Without this
	 * the palette can be freed (std3D_FreePalettes releases the palette-list node's
	 * only reference) while a texture surface still uses paletteHandle at cache time. */
	if (pal) {
		pal->lpVtbl->AddRef(pal);
	}
	if (s->palette) {
		((IDirectDrawPalette*)s->palette)->lpVtbl->Release((IDirectDrawPalette*)s->palette);
	}
	s->palette = p;
	if (s->cpu && p && s->format == AERON_PIXEL_FORMAT_INDEX8) {
		if (AeronSurface_SetPalette(s->cpu, p->entries, 256)) {
			s->applied_palette          = p;
			s->applied_palette_revision = p->revision;
		}
	}
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDSurface_Unlock(IDirectDrawSurface* self, void* p) {
	DDrawSurfaceShim* s = (DDrawSurfaceShim*)self;
	(void)p;
	if (s->cpu) {
		AeronSurface_Unlock(s->cpu);
	}
	/* The CPU staging is now authoritative; a later GPU pass or present writes it
	 * back into the color target (DDShim_WritebackRenderTarget). */
	if (s->kind == DDSHIM_RENDER_TARGET) {
		s->cpu_dirty = 1;
	}
	return DX_DD_OK;
}

const IDirectDrawSurfaceVtbl g_ddSurfaceVtbl = {
	.QueryInterface        = DDSurface_QueryInterface,
	.AddRef                = DDSurface_AddRef,
	.Release               = DDSurface_Release,
	.Blt                   = DDSurface_Blt,
	.BltFast               = DDSurface_BltFast,
	.Flip                  = DDSurface_Flip,
	.AddAttachedSurface    = DDSurface_AddAttachedSurface,
	.DeleteAttachedSurface = DDSurface_DeleteAttachedSurface,
	.GetAttachedSurface    = DDSurface_GetAttachedSurface,
	.GetColorKey           = DDSurface_GetColorKey,
	.GetFlipStatus         = DDSurface_GetFlipStatus,
	.GetSurfaceDesc        = DDSurface_GetSurfaceDesc,
	.IsLost                = DDSurface_IsLost,
	.Lock                  = DDSurface_Lock,
	.Restore               = DDSurface_Restore,
	.SetColorKey           = DDSurface_SetColorKey,
	.SetPalette            = DDSurface_SetPalette,
	.Unlock                = DDSurface_Unlock,
};

static DDrawSurfaceShim* DDShim_AllocSurface(DDrawShim* owner, DDShimKind kind, int w, int h,
											 AeronPixelFormat fmt, uint32_t caps) {
	DDrawSurfaceShim* s = (DDrawSurfaceShim*)calloc(1, sizeof(*s));
	if (!s) {
		return NULL;
	}
	s->lpVtbl   = &g_ddSurfaceVtbl;
	s->refcount = 1;
	s->kind     = kind;
	s->owner    = owner;
	s->width    = w;
	s->height   = h;
	s->format   = fmt;
	s->bpp      = DDShim_BppForFormat(fmt);
	s->caps     = caps;
	if (kind == DDSHIM_CPU) {
		if (!AeronSurface_Create(w, h, fmt, AERON_SURFACE_CPU_LOCKABLE | AERON_SURFACE_PRESENTABLE,
								 &s->cpu)) {
			free(s);
			return NULL;
		}
	}
	return s;
}

/* --- IDirectDrawPalette -------------------------------------------------- */

/* DirectDraw PALETTEENTRY: {peRed, peGreen, peBlue, peFlags}. */
typedef struct DDShimPaletteEntry {
	uint8_t peRed;
	uint8_t peGreen;
	uint8_t peBlue;
	uint8_t peFlags;
} DDShimPaletteEntry;

static uint32_t AERON_DXAPI DDPalette_AddRef(IDirectDrawPalette* self) {
	return (uint32_t)++((DDrawPaletteShim*)self)->refcount;
}

static uint32_t AERON_DXAPI DDPalette_Release(IDirectDrawPalette* self) {
	DDrawPaletteShim* p = (DDrawPaletteShim*)self;
	if (--p->refcount > 0) {
		return (uint32_t)p->refcount;
	}
	free(p);
	return 0;
}

static HRESULT AERON_DXAPI DDPalette_QueryInterface(IDirectDrawPalette* self, DxRefIid iid, void** out) {
	(void)self;
	(void)iid;
	if (out) {
		*out = NULL;
	}
	return DX_E_NOTIMPL;
}

static void DDShim_StorePaletteEntries(DDrawPaletteShim* p, uint32_t start, uint32_t count,
									   const DDShimPaletteEntry* src) {
	uint32_t i;
	int      changed = 0;
	for (i = 0; i < count && start + i < 256; ++i) {
		AeronPaletteEntry* dst = &p->entries[start + i];
		if (dst->r != src[i].peRed || dst->g != src[i].peGreen || dst->b != src[i].peBlue || dst->a != 255) {
			dst->r  = src[i].peRed;
			dst->g  = src[i].peGreen;
			dst->b  = src[i].peBlue;
			dst->a  = 255;
			changed = 1;
		}
	}
	if (changed)
		p->revision = ++g_ddPaletteRevision;
}

static HRESULT AERON_DXAPI DDPalette_SetEntries(IDirectDrawPalette* self, uint32_t flags, uint32_t start,
												uint32_t count, void* entries) {
	(void)flags;
	if (!entries) {
		return DX_E_INVALIDARG;
	}
	DDShim_StorePaletteEntries((DDrawPaletteShim*)self, start, count, (const DDShimPaletteEntry*)entries);
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDPalette_GetEntries(IDirectDrawPalette* self, uint32_t flags, uint32_t start,
												uint32_t count, void* entries) {
	DDrawPaletteShim*   p   = (DDrawPaletteShim*)self;
	DDShimPaletteEntry* dst = (DDShimPaletteEntry*)entries;
	uint32_t            i;
	(void)flags;
	if (!entries) {
		return DX_E_INVALIDARG;
	}
	for (i = 0; i < count && start + i < 256; ++i) {
		dst[i].peRed   = p->entries[start + i].r;
		dst[i].peGreen = p->entries[start + i].g;
		dst[i].peBlue  = p->entries[start + i].b;
		dst[i].peFlags = 0;
	}
	return DX_DD_OK;
}

const IDirectDrawPaletteVtbl g_ddPaletteVtbl = {
	.QueryInterface = DDPalette_QueryInterface,
	.AddRef         = DDPalette_AddRef,
	.Release        = DDPalette_Release,
	.GetEntries     = DDPalette_GetEntries,
	.SetEntries     = DDPalette_SetEntries,
};

/* --- IDirectDraw --------------------------------------------------------- */

static HRESULT AERON_DXAPI DDDevice_QueryInterface(IDirectDraw* self, DxRefIid iid, void** out) {
	IDirect3D* d3d;

	if (!out) {
		return DX_E_INVALIDARG;
	}

	/* std3D QIs the DirectDraw object for IDirect3D. */
	if (DxGuidEqual(iid, &CLSID_IDirect3D)) {
		d3d = D3DCompat_CreateD3D((DDrawShim*)self);
		if (!d3d) {
			return DX_E_FAIL;
		}
		*out = d3d;
		return DX_DD_OK;
	}

	/* Otherwise the recovered code QIs for IDirectDraw/IDirectDraw2; both resolve
	 * to the same device object. */
	self->lpVtbl->AddRef(self);
	*out = self;
	return DX_DD_OK;
}

static uint32_t AERON_DXAPI DDDevice_AddRef(IDirectDraw* self) {
	return (uint32_t)++((DDrawShim*)self)->refcount;
}

static uint32_t AERON_DXAPI DDDevice_Release(IDirectDraw* self) {
	DDrawShim* d = (DDrawShim*)self;
	if (--d->refcount > 0) {
		return (uint32_t)d->refcount;
	}
	free(d);
	return 0;
}

static HRESULT AERON_DXAPI DDDevice_SetCooperativeLevel(IDirectDraw* self, void* hwnd, uint32_t level) {
	(void)self;
	(void)hwnd;
	(void)level;
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDDevice_SetDisplayMode(IDirectDraw* self, uint32_t w, uint32_t h, uint32_t bpp) {
	DDrawShim* d   = (DDrawShim*)self;
	d->mode_w      = (int)w;
	d->mode_h      = (int)h;
	d->mode_bpp    = (int)bpp;
	d->mode_format = DDShim_FormatForBpp((int)bpp);
	/* The DirectDraw mode describes classic surface allocation only. Aeron's
	 * application-logical presentation size is owned by the presentation frame
	 * (fixed height, width tracking the window aspect) and is mapped to/from
	 * these surfaces at the composition/input boundary. Changing it here would
	 * reinterpret the centered 4:3 safe-frame rectangle as 640x480 coordinates
	 * and zoom/crop every submitted layer. */
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDDevice_CreateSurface(IDirectDraw* self, DDSURFACEDESC* desc,
												  IDirectDrawSurface** out, void* outer) {
	DDrawShim* d = (DDrawShim*)self;
	uint32_t   caps;
	int        w;
	int        h;
	(void)outer;

	if (!out || !desc) {
		return DX_E_INVALIDARG;
	}
	*out = NULL;
	caps = desc->ddsCaps.dwCaps;
	w    = (desc->dwFlags & DDSD_WIDTH) ? (int)desc->dwWidth : d->mode_w;
	h    = (desc->dwFlags & DDSD_HEIGHT) ? (int)desc->dwHeight : d->mode_h;

	if (caps & DDSCAPS_PRIMARYSURFACE) {
		DDrawSurfaceShim* primary =
			DDShim_AllocSurface(d, DDSHIM_PRIMARY, d->mode_w, d->mode_h, d->mode_format, caps);
		if (!primary) {
			return DX_E_FAIL;
		}
		/* A complex flip chain carries an attached back buffer (the front surface
		 * the recovered code renders into). */
		if ((caps & DDSCAPS_FLIP) || (desc->dwFlags & DDSD_BACKBUFFERCOUNT)) {
			primary->attached = DDShim_AllocSurface(d, DDSHIM_CPU, d->mode_w, d->mode_h, d->mode_format,
													DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX);
			if (!primary->attached) {
				(void)primary->lpVtbl->Release((IDirectDrawSurface*)primary);
				return DX_E_FAIL;
			}
		}
		d->primary = primary;
		*out       = (IDirectDrawSurface*)primary;
		return DX_DD_OK;
	}

	{
		AeronPixelFormat  fmt    = d->mode_format;
		int               has_pf = (desc->dwFlags & DDSD_PIXELFORMAT) != 0;
		DDrawSurfaceShim* s;

		/* Texture surfaces carry an explicit pixel format (one of the formats the
		 * shim reports from EnumTextureFormats). 16bpp texel formats (RGB565 /
		 * ARGB1555 / ARGB4444) share a raw 16bpp CPU backing; Load unpacks them via
		 * the stored masks. */
		if (has_pf) {
			fmt = desc->ddpfPixelFormat.dwRGBBitCount == 8 ? AERON_PIXEL_FORMAT_INDEX8
														   : AERON_PIXEL_FORMAT_RGB565;
		}
		s = DDShim_AllocSurface(d, DDSHIM_CPU, w, h, fmt, caps);
		if (!s) {
			return DX_E_FAIL;
		}
		if (has_pf) {
			s->has_pixel_format = 1;
			s->pixel_format     = desc->ddpfPixelFormat;
		}

		/* A mip-chain surface carries its smaller levels as a chain of attached
		 * surfaces (each half size, min 1), reachable via GetAttachedSurface. */
		if ((desc->dwFlags & DDSD_MIPMAPCOUNT) && desc->dwMipMapCount > 1) {
			DDrawSurfaceShim* level = s;
			int               lw    = w;
			int               lh    = h;
			uint32_t          i;

			for (i = 1; i < desc->dwMipMapCount; ++i) {
				DDrawSurfaceShim* next;
				lw   = lw > 1 ? lw / 2 : 1;
				lh   = lh > 1 ? lh / 2 : 1;
				next = DDShim_AllocSurface(d, DDSHIM_CPU, lw, lh, fmt, caps);
				if (!next) {
					(void)s->lpVtbl->Release((IDirectDrawSurface*)s);
					return DX_E_FAIL;
				}
				if (has_pf) {
					next->has_pixel_format = 1;
					next->pixel_format     = desc->ddpfPixelFormat;
				}
				level->attached = next;
				level           = next;
			}
		}
		*out = (IDirectDrawSurface*)s;
		return DX_DD_OK;
	}
}

static HRESULT AERON_DXAPI DDDevice_CreatePalette(IDirectDraw* self, uint32_t flags, void* entries,
												  IDirectDrawPalette** out, void* outer) {
	DDrawPaletteShim* p;
	(void)self;
	(void)flags;
	(void)outer;

	if (!out) {
		return DX_E_INVALIDARG;
	}
	*out = NULL;
	p    = (DDrawPaletteShim*)calloc(1, sizeof(*p));
	if (!p) {
		return DX_E_FAIL;
	}
	p->lpVtbl   = &g_ddPaletteVtbl;
	p->refcount = 1;
	if (entries) {
		DDShim_StorePaletteEntries(p, 0, 256, (const DDShimPaletteEntry*)entries);
	}
	*out = (IDirectDrawPalette*)p;
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDDevice_GetVerticalBlankStatus(IDirectDraw* self, int32_t* inVBlank) {
	(void)self;
	/* Aeron_Present owns vsync, so there is no hardware vertical-blank to report.
	 * Return failure: the recovered flip/flicker-sync loops all poll this as
	 * `while (!GetVerticalBlankStatus(&v) && ...)` and guard their blocks with
	 * `if (!GetVerticalBlankStatus(&v))`, so a nonzero result makes them skip the
	 * busy-wait entirely instead of spinning forever waiting for a blank edge that a
	 * constant status can never produce. */
	if (inVBlank) {
		*inVBlank = 0;
	}
	return DX_E_FAIL;
}

static HRESULT AERON_DXAPI DDDevice_GetMonitorFrequency(IDirectDraw* self, uint32_t* freq) {
	(void)self;
	if (freq) {
		*freq = 60; /* nominal; Aeron owns the real refresh */
	}
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDDevice_GetScanLine(IDirectDraw* self, uint32_t* scanLine) {
	(void)self;
	if (scanLine) {
		*scanLine = 0;
	}
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDDevice_GetCaps(IDirectDraw* self, void* hw, void* hel) {
	(void)self;
	(void)hw;
	(void)hel;
	/* Caller pre-zeroes DDCAPS; leaving it lets the recovered code take its
	 * non-hardware-accelerated present path. */
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDDevice_GetAvailableVidMem(IDirectDraw* self, DDSCAPS* caps, uint32_t* total,
													   uint32_t* freeMem) {
	(void)self;
	(void)caps;
	if (total) {
		*total = 0;
	}
	if (freeMem) {
		*freeMem = 0;
	}
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDDevice_DuplicateSurface(IDirectDraw* self, IDirectDrawSurface* src,
													 IDirectDrawSurface** out) {
	(void)self;
	(void)src;
	if (out) {
		*out = NULL;
	}
	return DX_E_NOTIMPL;
}

static HRESULT AERON_DXAPI DDDevice_FlipToGDISurface(IDirectDraw* self) {
	(void)self;
	return DX_DD_OK;
}

static HRESULT AERON_DXAPI DDDevice_RestoreDisplayMode(IDirectDraw* self) {
	(void)self;
	/* The compatibility device never changes the host desktop display mode. */
	return DX_DD_OK;
}

const IDirectDrawVtbl g_ddDeviceVtbl = {
	.QueryInterface         = DDDevice_QueryInterface,
	.AddRef                 = DDDevice_AddRef,
	.Release                = DDDevice_Release,
	.CreatePalette          = DDDevice_CreatePalette,
	.CreateSurface          = DDDevice_CreateSurface,
	.DuplicateSurface       = DDDevice_DuplicateSurface,
	.FlipToGDISurface       = DDDevice_FlipToGDISurface,
	.GetCaps                = DDDevice_GetCaps,
	.GetMonitorFrequency    = DDDevice_GetMonitorFrequency,
	.GetScanLine            = DDDevice_GetScanLine,
	.GetVerticalBlankStatus = DDDevice_GetVerticalBlankStatus,
	.RestoreDisplayMode     = DDDevice_RestoreDisplayMode,
	.SetCooperativeLevel    = DDDevice_SetCooperativeLevel,
	.SetDisplayMode         = DDDevice_SetDisplayMode,
	.GetAvailableVidMem     = DDDevice_GetAvailableVidMem,
};

HRESULT AERON_DXAPI DirectDrawCreate(const DxGuid* driver, IDirectDraw** out, void* outer) {
	DDrawShim* device;
	(void)driver;
	(void)outer;

	if (!out) {
		return DX_E_INVALIDARG;
	}
	*out   = NULL;
	device = (DDrawShim*)calloc(1, sizeof(*device));
	if (!device) {
		return DX_E_FAIL;
	}
	device->lpVtbl   = &g_ddDeviceVtbl;
	device->refcount = 1;
	*out             = (IDirectDraw*)device;
	return DX_DD_OK;
}

HRESULT DirectDrawCreate_Compat(const DxGuid* driver, IDirectDraw** out, void* outer) {
	return DirectDrawCreate(driver, out, outer);
}
