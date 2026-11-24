/*
Flying Layers for OBS Studio
Copyright (C) 2026 YearNorth

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "flying-transition.h"

#include "layout.h"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <initializer_list>
#include <mutex>
#include <utility>
#include <vector>

namespace {

// ---- Settings ---------------------------------------------------------------

constexpr const char *kEasing = "easing";
constexpr const char *kBezierX1 = "bezier_x1";
constexpr const char *kBezierY1 = "bezier_y1";
constexpr const char *kBezierX2 = "bezier_x2";
constexpr const char *kBezierY2 = "bezier_y2";
constexpr const char *kTimingMode = "timing_mode";
constexpr const char *kExitPortion = "exit_portion";
constexpr const char *kEnterPortion = "enter_portion";
constexpr const char *kCascade = "cascade";
constexpr const char *kCascadeOrder = "cascade_order";
constexpr const char *kEnterEffect = "enter_effect";
constexpr const char *kExitEffect = "exit_effect";
constexpr const char *kSlideEdge = "slide_edge";
constexpr const char *kMotionFade = "motion_fade";
constexpr const char *kMotionBlur = "motion_blur";
constexpr const char *kShutterAngle = "shutter_angle";

// How a layer that is only in one of the two scenes comes in or goes out.
// Values are stored in the settings: only append.
enum class Appear : int {
	Fade = 0,
	Zoom,
	Slide,
	Cut,
	Wipe,
};

// Order in which layers start when the cascade is on.
enum class CascadeOrder : int {
	StackUp = 0, // bottom layer first
	StackDown,   // top layer first
	LeftToRight,
	RightToLeft,
	TopToBottom,
	BottomToTop,
};

enum class Role {
	Fly,
	Enter,
	Exit,
};

struct Settings {
	fl::Easing easing = fl::Easing::EaseInOut;
	fl::Bezier bezier;
	fl::Timing timing;
	CascadeOrder cascadeOrder = CascadeOrder::StackUp;
	Appear enter = Appear::Fade;
	Appear exit = Appear::Fade;
	fl::Edge edge = fl::Edge::Auto;
	bool motionFade = true;
	bool motionBlur = false;
	float shutter = 0.5f; // share of a frame interval the shutter is open (180 degrees)
};

// ---- Scene items ----------------------------------------------------------

// A layer of the transition: the scene item in scene A and in scene B
// (either can be missing). Holds a reference to each item.
struct PlannedLayer {
	obs_sceneitem_t *a = nullptr;
	obs_sceneitem_t *b = nullptr;
};

// What one scene item looks like right now.
struct LayerState {
	obs_source_t *source = nullptr; // not owned: kept alive by the item
	fl::Layout layout;
	bool nestedScene = false; // drawn through a texture so it is clipped like OBS does
};

// One layer for the current frame.
struct FrameLayer {
	Role role = Role::Fly;
	LayerState a;
	LayerState b;
	fl::Window window;

	const LayerState &shown() const { return role == Role::Exit ? a : b; }
};

bool collectItem(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	obs_sceneitem_addref(item);
	static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
	return true;
}

bool readLayer(obs_sceneitem_t *item, LayerState &out)
{
	if (!item || !obs_sceneitem_visible(item) || !obs_sceneitem_get_scene(item))
		return false;

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return false;

	obs_transform_info info;
	obs_sceneitem_get_info2(item, &info);
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);

	if (!fl::computeLayout(info, crop, obs_source_get_width(source), obs_source_get_height(source), out.layout))
		return false;

	out.source = source;
	out.nestedScene = obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE && !obs_sceneitem_is_group(item);
	return true;
}

// Applies an entry/exit effect. `away` is 0 with the layer in place and 1
// with the layer gone; overshooting curves can take it a little outside.
// Changes `layout` and returns the opacity.
float appear(fl::Layout &layout, Appear effect, float away, float progress, bool exiting, const Settings &settings,
	     float canvasW, float canvasH)
{
	const float clamped = std::clamp(away, 0.0f, 1.0f);
	const float fade = settings.motionFade ? 1.0f - clamped : 1.0f;

	switch (effect) {
	case Appear::Zoom: {
		const float s = std::max(1.0f - away, 0.0f);
		layout.sx *= s;
		layout.sy *= s;
		return fade;
	}
	case Appear::Slide: {
		float dx, dy;
		fl::offscreenOffset(layout, settings.edge, canvasW, canvasH, dx, dy);
		layout.cx += dx * away;
		layout.cy += dy * away;
		return fade;
	}
	case Appear::Wipe:
		fl::wipe(layout, fl::resolveEdge(layout, settings.edge, canvasW, canvasH), clamped);
		return 1.0f;
	case Appear::Cut:
		return (exiting ? progress < 0.5f : progress >= 0.5f) ? 1.0f : 0.0f;
	case Appear::Fade:
	default:
		return 1.0f - clamped;
	}
}

// Placement and opacity of a layer at transition time t.
float evaluate(const FrameLayer &layer, float t, const Settings &settings, float canvasW, float canvasH,
	       fl::Layout &out)
{
	const float progress = fl::localProgress(layer.window, t);
	const float k = fl::applyEasing(settings.easing, progress, settings.bezier);

	switch (layer.role) {
	case Role::Fly:
		out = fl::interpolate(layer.a.layout, layer.b.layout, k);
		return 1.0f;
	case Role::Enter:
		out = layer.b.layout;
		return appear(out, settings.enter, 1.0f - k, progress, false, settings, canvasW, canvasH);
	case Role::Exit:
	default:
		out = layer.a.layout;
		return appear(out, settings.exit, k, progress, true, settings, canvasW, canvasH);
	}
}

// Largest distance travelled by a corner of the layer between two
// placements, in scene pixels.
float travel(const fl::Layout &from, const fl::Layout &to)
{
	vec2 a[4], b[4];
	fl::layoutCorners(from, a);
	fl::layoutCorners(to, b);
	float most = 0.0f;
	for (int i = 0; i < 4; ++i)
		most = std::max(most, vec2_dist(&a[i], &b[i]));
	return most;
}

// ---- The transition
// ----------------------------------------------------------

class FlyingTransition {
public:
	FlyingTransition(obs_data_t *settings, obs_source_t *self) : self_(self)
	{
		char *file = obs_module_file("flying-layers.effect");
		obs_enter_graphics();
		effect_ = gs_effect_create_from_file(file, nullptr);
		obs_leave_graphics();
		bfree(file);

		if (effect_) {
			imageParam_ = gs_effect_get_param_by_name(effect_, "image");
			opacityParam_ = gs_effect_get_param_by_name(effect_, "opacity");
			uvOffsetParam_ = gs_effect_get_param_by_name(effect_, "uv_offset");
			uvScaleParam_ = gs_effect_get_param_by_name(effect_, "uv_scale");
		} else {
			obs_log(LOG_ERROR, "could not load flying-layers.effect");
		}

		update(settings);
	}

	~FlyingTransition()
	{
		obs_enter_graphics();
		clearPlan();
		for (gs_texrender_t *texrender : texrenders_)
			gs_texrender_destroy(texrender);
		texrenders_.clear();
		if (accumulator_)
			gs_texrender_destroy(accumulator_);
		if (effect_)
			gs_effect_destroy(effect_);
		obs_leave_graphics();
	}

	bool valid() const { return effect_ && imageParam_ && opacityParam_ && uvOffsetParam_ && uvScaleParam_; }
	obs_source_t *source() const { return self_; }

	void update(obs_data_t *data)
	{
		Settings s;
		s.easing = (fl::Easing)obs_data_get_int(data, kEasing);
		s.bezier.x1 = (float)obs_data_get_double(data, kBezierX1);
		s.bezier.y1 = (float)obs_data_get_double(data, kBezierY1);
		s.bezier.x2 = (float)obs_data_get_double(data, kBezierX2);
		s.bezier.y2 = (float)obs_data_get_double(data, kBezierY2);
		s.timing.sequenced = obs_data_get_int(data, kTimingMode) == 1;
		s.timing.exitPortion = (float)obs_data_get_int(data, kExitPortion) / 100.0f;
		s.timing.enterPortion = (float)obs_data_get_int(data, kEnterPortion) / 100.0f;
		s.timing.cascade = (float)obs_data_get_int(data, kCascade) / 100.0f;
		s.cascadeOrder = (CascadeOrder)obs_data_get_int(data, kCascadeOrder);
		s.enter = (Appear)obs_data_get_int(data, kEnterEffect);
		s.exit = (Appear)obs_data_get_int(data, kExitEffect);
		s.edge = (fl::Edge)obs_data_get_int(data, kSlideEdge);
		s.motionFade = obs_data_get_bool(data, kMotionFade);
		s.motionBlur = obs_data_get_bool(data, kMotionBlur);
		s.shutter = std::clamp((float)obs_data_get_int(data, kShutterAngle), 1.0f, 360.0f) / 360.0f;

		std::lock_guard<std::mutex> lock(settingsMutex_);
		settings_ = s;
	}

	void transitionStarted() { replan_ = true; }

	void render()
	{
		obs_source_t *sourceA = obs_transition_get_source(self_, OBS_TRANSITION_SOURCE_A);
		obs_source_t *sourceB = obs_transition_get_source(self_, OBS_TRANSITION_SOURCE_B);
		const float t = obs_transition_get_time(self_);

		obs_scene_t *sceneA = obs_scene_from_source(sourceA);
		obs_scene_t *sceneB = obs_scene_from_source(sourceB);
		const bool transitioning = sourceB && t < 1.0f;

		const bool previousSrgb = gs_set_linear_srgb(true);

		if (transitioning && sceneB && (sceneA || !sourceA)) {
			renderFlying(sourceA, sourceB, sceneA, sceneB, t);
		} else if (transitioning) {
			// Not two scenes (the preview in the properties dialog,
			// for example): plain crossfade.
			obs_transition_video_render2(self_, crossfadeCallback, nullptr);
		} else {
			// Idle, or the transition has just reached the end: let
			// OBS draw the scene and finish the transition.
			clearPlan();
			obs_transition_video_render_direct(self_, OBS_TRANSITION_SOURCE_B);
		}

		gs_set_linear_srgb(previousSrgb);

		obs_source_release(sourceA);
		obs_source_release(sourceB);
	}

	static obs_properties_t *properties();
	static void defaults(obs_data_t *settings);

private:
	// ---- Pairing ------------------------------------------------------

	void clearPlan()
	{
		for (PlannedLayer &layer : plan_) {
			obs_sceneitem_release(layer.a);
			obs_sceneitem_release(layer.b);
		}
		plan_.clear();
		planA_ = nullptr;
		planB_ = nullptr;
		lastFrameTime_ = 0;
		haveFrameStep_ = false;
	}

	// Pairs the layers of the two scenes. Runs once per transition, on the
	// first frame; every frame then only reads the items' transforms.
	void buildPlan(obs_source_t *sourceA, obs_source_t *sourceB, obs_scene_t *sceneA, obs_scene_t *sceneB)
	{
		clearPlan();

		std::vector<obs_sceneitem_t *> itemsA;
		std::vector<obs_sceneitem_t *> itemsB;
		if (sceneA)
			obs_scene_enum_items(sceneA, collectItem, &itemsA);
		obs_scene_enum_items(sceneB, collectItem, &itemsB);

		// Layers are the same when they show the same source. Names are
		// unique in OBS and survive the scene duplication of Studio Mode.
		auto keys = [](const std::vector<obs_sceneitem_t *> &items) {
			std::vector<const char *> out;
			out.reserve(items.size());
			for (obs_sceneitem_t *item : items)
				out.push_back(obs_source_get_name(obs_sceneitem_get_source(item)));
			return out;
		};

		const std::vector<fl::Pair> order = fl::planLayers(keys(itemsA), keys(itemsB));

		plan_.reserve(order.size());
		for (const fl::Pair &pair : order) {
			PlannedLayer layer;
			if (pair.a >= 0) {
				layer.a = itemsA[(size_t)pair.a];
				obs_sceneitem_addref(layer.a);
			}
			if (pair.b >= 0) {
				layer.b = itemsB[(size_t)pair.b];
				obs_sceneitem_addref(layer.b);
			}
			plan_.push_back(layer);
		}

		for (obs_sceneitem_t *item : itemsA)
			obs_sceneitem_release(item);
		for (obs_sceneitem_t *item : itemsB)
			obs_sceneitem_release(item);

		planA_ = sourceA;
		planB_ = sourceB;
	}

	// ---- Timing -------------------------------------------------------

	// Reads every layer for this frame and gives it its time window.
	void prepareFrame(const Settings &settings)
	{
		frame_.clear();
		for (const PlannedLayer &planned : plan_) {
			FrameLayer layer;
			const bool hasA = readLayer(planned.a, layer.a);
			const bool hasB = readLayer(planned.b, layer.b);
			if (hasA && hasB)
				layer.role = Role::Fly;
			else if (hasB)
				layer.role = Role::Enter;
			else if (hasA)
				layer.role = Role::Exit;
			else
				continue;
			frame_.push_back(layer);
		}

		fl::Window exit, fly, enter;
		fl::groupWindows(settings.timing, exit, fly, enter);

		for (Role role : {Role::Exit, Role::Fly, Role::Enter}) {
			order_.clear();
			for (size_t i = 0; i < frame_.size(); ++i)
				if (frame_[i].role == role)
					order_.push_back(i);

			sortForCascade(settings.cascadeOrder);

			const fl::Window &group = role == Role::Exit ? exit : role == Role::Fly ? fly : enter;
			for (size_t rank = 0; rank < order_.size(); ++rank)
				frame_[order_[rank]].window = fl::cascadeWindow(group, settings.timing.cascade,
										(int)rank, (int)order_.size());
		}
	}

	void sortForCascade(CascadeOrder order)
	{
		auto centre = [this](size_t i, bool x) {
			const fl::Layout &l = frame_[i].shown().layout;
			return x ? l.cx : l.cy;
		};
		switch (order) {
		case CascadeOrder::StackDown:
			std::reverse(order_.begin(), order_.end());
			break;
		case CascadeOrder::LeftToRight:
			std::stable_sort(order_.begin(), order_.end(),
					 [&](size_t p, size_t q) { return centre(p, true) < centre(q, true); });
			break;
		case CascadeOrder::RightToLeft:
			std::stable_sort(order_.begin(), order_.end(),
					 [&](size_t p, size_t q) { return centre(p, true) > centre(q, true); });
			break;
		case CascadeOrder::TopToBottom:
			std::stable_sort(order_.begin(), order_.end(),
					 [&](size_t p, size_t q) { return centre(p, false) < centre(q, false); });
			break;
		case CascadeOrder::BottomToTop:
			std::stable_sort(order_.begin(), order_.end(),
					 [&](size_t p, size_t q) { return centre(p, false) > centre(q, false); });
			break;
		case CascadeOrder::StackUp:
		default:
			break; // the plan is already bottom to top
		}
	}

	// Progress made by the transition between the previous frame and this
	// one, used for motion blur. Works with timed transitions and with the
	// T-bar alike. Rendering the same frame twice does not change it.
	float frameStep(float t)
	{
		const uint64_t frameTime = obs_get_video_frame_time();
		if (frameTime != lastFrameTime_) {
			if (lastFrameTime_ != 0 && t >= lastT_) {
				frameStep_ = t - lastT_;
				haveFrameStep_ = true;
			}
			lastFrameTime_ = frameTime;
			lastT_ = t;
		}
		return haveFrameStep_ ? frameStep_ : 0.0f;
	}

	// ---- Drawing ------------------------------------------------------

	void renderFlying(obs_source_t *sourceA, obs_source_t *sourceB, obs_scene_t *sceneA, obs_scene_t *sceneB,
			  float t)
	{
		if (replan_.exchange(false) || planA_ != sourceA || planB_ != sourceB)
			buildPlan(sourceA, sourceB, sceneA, sceneB);

		Settings settings;
		{
			std::lock_guard<std::mutex> lock(settingsMutex_);
			settings = settings_;
		}

		const float canvasW = (float)obs_source_get_width(sourceB);
		const float canvasH = (float)obs_source_get_height(sourceB);
		const float shutterOpen = settings.motionBlur ? frameStep(t) * settings.shutter : 0.0f;

		prepareFrame(settings);

		gs_blend_state_push();
		gs_reset_blend_state();

		size_t textureIndex = 0;
		for (const FrameLayer &layer : frame_) {
			fl::Layout layout;
			const float opacity = evaluate(layer, t, settings, canvasW, canvasH, layout);

			if (shutterOpen > 0.0f &&
			    drawBlurred(layer, t - shutterOpen, t, settings, canvasW, canvasH, textureIndex))
				continue;

			drawLayer(layer.shown(), layout, opacity, textureIndex);
		}

		gs_blend_state_pop();
	}

	gs_texrender_t *texrenderAt(size_t index, gs_color_format format)
	{
		while (texrenders_.size() <= index)
			texrenders_.push_back(nullptr);

		gs_texrender_t *&texrender = texrenders_[index];
		if (texrender && gs_texrender_get_format(texrender) != format) {
			gs_texrender_destroy(texrender);
			texrender = nullptr;
		}
		if (!texrender)
			texrender = gs_texrender_create(format, GS_ZS_NONE);
		return texrender;
	}

	// Renders the whole source (crop is applied when drawing) into a
	// texture of its own size.
	gs_texture_t *renderContent(const LayerState &state, size_t &textureIndex)
	{
		const uint32_t w = (uint32_t)state.layout.srcW;
		const uint32_t h = (uint32_t)state.layout.srcH;
		const gs_color_space space = gs_get_color_space();
		gs_texrender_t *texrender = texrenderAt(textureIndex++, gs_get_format_from_space(space));
		if (!texrender || !w || !h)
			return nullptr;

		gs_texrender_reset(texrender);
		if (gs_texrender_begin_with_color_space(texrender, w, h, space)) {
			vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

			gs_blend_state_push();
			gs_reset_blend_state();
			obs_source_video_render(state.source);
			gs_blend_state_pop();

			gs_texrender_end(texrender);
		}
		return gs_texrender_get_texture(texrender);
	}

	void setUv(float offsetX, float offsetY, float scaleX, float scaleY)
	{
		vec2 offset, scale;
		vec2_set(&offset, offsetX, offsetY);
		vec2_set(&scale, scaleX, scaleY);
		gs_effect_set_vec2(uvOffsetParam_, &offset);
		gs_effect_set_vec2(uvScaleParam_, &scale);
	}

	// Draws a premultiplied texture with an opacity. In SDR the opacity is
	// applied to gamma-encoded values, like the Fade transition of OBS, so
	// a half fade looks half as bright; in HDR it is applied to linear values.
	void drawTexture(gs_texture_t *texture, float opacity, uint32_t cx, uint32_t cy)
	{
		const bool linear = gs_get_color_space() != GS_CS_SRGB;
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);

		if (linear)
			gs_effect_set_texture_srgb(imageParam_, texture);
		else
			gs_effect_set_texture(imageParam_, texture);
		gs_effect_set_float(opacityParam_, opacity);

		while (gs_effect_loop(effect_, "Draw"))
			gs_draw_sprite(texture, 0, cx, cy);

		gs_enable_framebuffer_srgb(previous);
	}

	// Matrix that maps the unit square to the visible part of a layer.
	static void contentMatrix(const fl::Layout &layout, matrix4 &out)
	{
		matrix4 place;
		fl::layoutMatrix(layout, place);
		matrix4 size;
		matrix4_identity(&size);
		matrix4_scale3f(&size, &size, layout.contentW, layout.contentH, 1.0f);
		matrix4_mul(&out, &size, &place);
	}

	void setCropUv(const fl::Layout &layout)
	{
		setUv(layout.cropL / layout.srcW, layout.cropT / layout.srcH, layout.contentW / layout.srcW,
		      layout.contentH / layout.srcH);
	}

	void drawLayer(const LayerState &state, const fl::Layout &layout, float opacity, size_t &textureIndex)
	{
		if (opacity <= 0.001f || fabsf(layout.sx) < 1e-6f || fabsf(layout.sy) < 1e-6f)
			return;

		const bool useTexture = state.nestedScene || layout.hasCrop() || opacity < 0.999f;
		if (!useTexture) {
			// Fast path: draw the source straight onto the output.
			matrix4 transform;
			fl::layoutMatrix(layout, transform);
			gs_matrix_push();
			gs_matrix_mul(&transform);
			obs_source_video_render(state.source);
			gs_matrix_pop();
			return;
		}

		gs_texture_t *content = renderContent(state, textureIndex);
		if (!content)
			return;

		matrix4 transform;
		contentMatrix(layout, transform);
		gs_matrix_push();
		gs_matrix_mul(&transform);

		// The texture holds premultiplied alpha.
		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		setCropUv(layout);
		drawTexture(content, opacity, 1, 1);
		gs_blend_state_pop();

		gs_matrix_pop();
	}

	// Motion blur: averages the layer over the time the shutter is open
	// (from t0 to t1), the way a camera does. The source is rendered once;
	// only its texture is drawn several times, into a floating point
	// texture that covers just the area swept by the layer. Returns false
	// when the layer barely moves, so it is drawn normally.
	bool drawBlurred(const FrameLayer &layer, float t0, float t1, const Settings &settings, float canvasW,
			 float canvasH, size_t &textureIndex)
	{
		fl::Layout start, end;
		const float opacityStart = evaluate(layer, t0, settings, canvasW, canvasH, start);
		const float opacityEnd = evaluate(layer, t1, settings, canvasW, canvasH, end);
		if (opacityStart <= 0.001f && opacityEnd <= 0.001f)
			return false;

		const float distance = travel(start, end);
		if (distance < 2.0f)
			return false;

		// About one sample per pixel of travel, which hides the steps.
		const int samples = std::clamp((int)ceilf(distance), 3, 32);
		samples_.clear();
		fl::Rect area{INFINITY, INFINITY, -INFINITY, -INFINITY};
		for (int i = 0; i < samples; ++i) {
			const float t = t0 + (t1 - t0) * (float)i / (float)(samples - 1);
			Sample sample;
			sample.opacity = evaluate(layer, t, settings, canvasW, canvasH, sample.layout);
			if (sample.opacity <= 0.001f || fabsf(sample.layout.sx) < 1e-6f ||
			    fabsf(sample.layout.sy) < 1e-6f)
				sample.opacity = 0.0f;
			else {
				const fl::Rect r = fl::layoutBounds(sample.layout);
				area.minX = std::min(area.minX, r.minX);
				area.minY = std::min(area.minY, r.minY);
				area.maxX = std::max(area.maxX, r.maxX);
				area.maxY = std::max(area.maxY, r.maxY);
			}
			samples_.push_back(sample);
		}

		// Only the part on the canvas matters.
		const float x0 = floorf(std::max(area.minX, 0.0f));
		const float y0 = floorf(std::max(area.minY, 0.0f));
		const float x1 = ceilf(std::min(area.maxX, canvasW));
		const float y1 = ceilf(std::min(area.maxY, canvasH));
		if (x1 <= x0 || y1 <= y0)
			return true; // nothing visible

		gs_texture_t *content = renderContent(layer.shown(), textureIndex);
		if (!content)
			return true;

		const uint32_t w = (uint32_t)(x1 - x0);
		const uint32_t h = (uint32_t)(y1 - y0);
		const gs_color_space space = gs_get_color_space();
		const gs_color_space linearSpace = space == GS_CS_SRGB ? GS_CS_SRGB_16F : space;

		if (!accumulator_)
			accumulator_ = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
		gs_texrender_reset(accumulator_);
		if (!gs_texrender_begin_with_color_space(accumulator_, w, h, linearSpace))
			return true;

		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(x0, x0 + (float)w, y0, y0 + (float)h, -100.0f, 100.0f);

		// Sum of the samples, each weighted 1/N, in linear light.
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(false);

		for (const Sample &sample : samples_) {
			if (sample.opacity <= 0.0f)
				continue;
			// Effect parameters are consumed by each pass: set them
			// again for every sample.
			if (space == GS_CS_SRGB)
				gs_effect_set_texture_srgb(imageParam_, content);
			else
				gs_effect_set_texture(imageParam_, content);
			matrix4 transform;
			contentMatrix(sample.layout, transform);
			gs_matrix_push();
			gs_matrix_mul(&transform);
			setCropUv(sample.layout);
			gs_effect_set_float(opacityParam_, sample.opacity / (float)samples);
			while (gs_effect_loop(effect_, "Draw"))
				gs_draw_sprite(content, 0, 1, 1);
			gs_matrix_pop();
		}

		gs_enable_framebuffer_srgb(previous);
		gs_blend_state_pop();
		gs_texrender_end(accumulator_);

		gs_texture_t *blurred = gs_texrender_get_texture(accumulator_);
		if (!blurred)
			return true;

		// Composite the linear result over the output.
		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		const bool previousOut = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);
		gs_effect_set_texture(imageParam_, blurred);
		gs_effect_set_float(opacityParam_, 1.0f);
		setUv(0.0f, 0.0f, 1.0f, 1.0f);
		gs_matrix_push();
		gs_matrix_translate3f(x0, y0, 0.0f);
		while (gs_effect_loop(effect_, "Draw"))
			gs_draw_sprite(blurred, 0, w, h);
		gs_matrix_pop();
		gs_enable_framebuffer_srgb(previousOut);
		gs_blend_state_pop();
		return true;
	}

	// Fallback when one side is not a scene: crossfade, the two
	// premultiplied images summed with their weights.
	static void crossfadeCallback(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy)
	{
		auto *self = static_cast<FlyingTransition *>(data);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
		self->setUv(0.0f, 0.0f, 1.0f, 1.0f);
		if (a)
			self->drawTexture(a, 1.0f - t, cx, cy);
		if (b)
			self->drawTexture(b, t, cx, cy);
		gs_blend_state_pop();
	}

	struct Sample {
		fl::Layout layout;
		float opacity = 0.0f;
	};

	obs_source_t *self_;
	gs_effect_t *effect_ = nullptr;
	gs_eparam_t *imageParam_ = nullptr;
	gs_eparam_t *opacityParam_ = nullptr;
	gs_eparam_t *uvOffsetParam_ = nullptr;
	gs_eparam_t *uvScaleParam_ = nullptr;

	// Written by the UI thread, read by the graphics thread.
	std::mutex settingsMutex_;
	Settings settings_;
	std::atomic<bool> replan_{true};

	// Graphics thread only.
	std::vector<PlannedLayer> plan_;
	obs_source_t *planA_ = nullptr; // identity only, not referenced
	obs_source_t *planB_ = nullptr;
	std::vector<FrameLayer> frame_;
	std::vector<size_t> order_;
	std::vector<Sample> samples_;
	std::vector<gs_texrender_t *> texrenders_;
	gs_texrender_t *accumulator_ = nullptr;
	uint64_t lastFrameTime_ = 0;
	float lastT_ = 0.0f;
	float frameStep_ = 0.0f;
	bool haveFrameStep_ = false;
};

// ---- Properties
// ----------------------------------------------------------------

void addList(obs_properties_t *props, const char *key, const char *label,
	     std::initializer_list<std::pair<const char *, int>> items)
{
	obs_property_t *list =
		obs_properties_add_list(props, key, obs_module_text(label), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (const auto &item : items)
		obs_property_list_add_int(list, obs_module_text(item.first), item.second);
}

void addAppearList(obs_properties_t *props, const char *key, const char *label)
{
	addList(props, key, label,
		{{"FlyingLayers.Appear.Fade", (int)Appear::Fade},
		 {"FlyingLayers.Appear.Zoom", (int)Appear::Zoom},
		 {"FlyingLayers.Appear.Slide", (int)Appear::Slide},
		 {"FlyingLayers.Appear.Wipe", (int)Appear::Wipe},
		 {"FlyingLayers.Appear.Cut", (int)Appear::Cut}});
}

void setVisible(obs_properties_t *props, const char *key, bool visible)
{
	if (obs_property_t *p = obs_properties_get(props, key))
		obs_property_set_visible(p, visible);
}

// Shows only the options that apply to the current choices.
bool refreshVisibility(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool custom = obs_data_get_int(settings, kEasing) == (int)fl::Easing::Custom;
	for (const char *key : {kBezierX1, kBezierY1, kBezierX2, kBezierY2})
		setVisible(props, key, custom);

	setVisible(props, kCascadeOrder, obs_data_get_int(settings, kCascade) > 0);

	auto uses = [settings](Appear effect) {
		return obs_data_get_int(settings, kEnterEffect) == (int)effect ||
		       obs_data_get_int(settings, kExitEffect) == (int)effect;
	};
	setVisible(props, kSlideEdge, uses(Appear::Slide) || uses(Appear::Wipe));
	setVisible(props, kMotionFade, uses(Appear::Zoom) || uses(Appear::Slide));
	setVisible(props, kShutterAngle, obs_data_get_bool(settings, kMotionBlur));
	return true;
}

obs_properties_t *FlyingTransition::properties()
{
	obs_properties_t *props = obs_properties_create();

	// Motion curve
	obs_properties_t *curve = obs_properties_create();
	addList(curve, kEasing, "FlyingLayers.Easing",
		{{"FlyingLayers.Easing.Linear", (int)fl::Easing::Linear},
		 {"FlyingLayers.Easing.EaseIn", (int)fl::Easing::EaseIn},
		 {"FlyingLayers.Easing.EaseOut", (int)fl::Easing::EaseOut},
		 {"FlyingLayers.Easing.EaseInOut", (int)fl::Easing::EaseInOut},
		 {"FlyingLayers.Easing.Smooth", (int)fl::Easing::Smooth},
		 {"FlyingLayers.Easing.BackOut", (int)fl::Easing::BackOut},
		 {"FlyingLayers.Easing.BackInOut", (int)fl::Easing::BackInOut},
		 {"FlyingLayers.Easing.ElasticOut", (int)fl::Easing::ElasticOut},
		 {"FlyingLayers.Easing.Custom", (int)fl::Easing::Custom}});
	obs_properties_add_float_slider(curve, kBezierX1, obs_module_text("FlyingLayers.Bezier.X1"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(curve, kBezierY1, obs_module_text("FlyingLayers.Bezier.Y1"), -1.0, 2.0, 0.01);
	obs_properties_add_float_slider(curve, kBezierX2, obs_module_text("FlyingLayers.Bezier.X2"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(curve, kBezierY2, obs_module_text("FlyingLayers.Bezier.Y2"), -1.0, 2.0, 0.01);
	obs_properties_add_group(props, "group_curve", obs_module_text("FlyingLayers.Group.Curve"), OBS_GROUP_NORMAL,
				 curve);

	// Timing
	obs_properties_t *timing = obs_properties_create();
	addList(timing, kTimingMode, "FlyingLayers.TimingMode",
		{{"FlyingLayers.TimingMode.Together", 0}, {"FlyingLayers.TimingMode.Sequenced", 1}});
	obs_property_t *exitPortion = obs_properties_add_int_slider(
		timing, kExitPortion, obs_module_text("FlyingLayers.ExitPortion"), 5, 100, 5);
	obs_property_int_set_suffix(exitPortion, " %");
	obs_property_t *enterPortion = obs_properties_add_int_slider(
		timing, kEnterPortion, obs_module_text("FlyingLayers.EnterPortion"), 5, 100, 5);
	obs_property_int_set_suffix(enterPortion, " %");
	obs_property_t *cascade =
		obs_properties_add_int_slider(timing, kCascade, obs_module_text("FlyingLayers.Cascade"), 0, 90, 5);
	obs_property_int_set_suffix(cascade, " %");
	addList(timing, kCascadeOrder, "FlyingLayers.CascadeOrder",
		{{"FlyingLayers.CascadeOrder.StackUp", (int)CascadeOrder::StackUp},
		 {"FlyingLayers.CascadeOrder.StackDown", (int)CascadeOrder::StackDown},
		 {"FlyingLayers.CascadeOrder.LeftToRight", (int)CascadeOrder::LeftToRight},
		 {"FlyingLayers.CascadeOrder.RightToLeft", (int)CascadeOrder::RightToLeft},
		 {"FlyingLayers.CascadeOrder.TopToBottom", (int)CascadeOrder::TopToBottom},
		 {"FlyingLayers.CascadeOrder.BottomToTop", (int)CascadeOrder::BottomToTop}});
	obs_properties_add_group(props, "group_timing", obs_module_text("FlyingLayers.Group.Timing"), OBS_GROUP_NORMAL,
				 timing);

	// Layers coming in and going out
	obs_properties_t *appearing = obs_properties_create();
	addAppearList(appearing, kEnterEffect, "FlyingLayers.Enter");
	addAppearList(appearing, kExitEffect, "FlyingLayers.Exit");
	addList(appearing, kSlideEdge, "FlyingLayers.SlideEdge",
		{{"FlyingLayers.Edge.Auto", (int)fl::Edge::Auto},
		 {"FlyingLayers.Edge.Left", (int)fl::Edge::Left},
		 {"FlyingLayers.Edge.Right", (int)fl::Edge::Right},
		 {"FlyingLayers.Edge.Top", (int)fl::Edge::Top},
		 {"FlyingLayers.Edge.Bottom", (int)fl::Edge::Bottom}});
	obs_properties_add_bool(appearing, kMotionFade, obs_module_text("FlyingLayers.MotionFade"));
	obs_properties_add_group(props, "group_appear", obs_module_text("FlyingLayers.Group.Appear"), OBS_GROUP_NORMAL,
				 appearing);

	// Motion blur
	obs_properties_t *blur = obs_properties_create();
	obs_properties_add_bool(blur, kMotionBlur, obs_module_text("FlyingLayers.MotionBlur"));
	obs_property_t *shutter = obs_properties_add_int_slider(
		blur, kShutterAngle, obs_module_text("FlyingLayers.ShutterAngle"), 30, 360, 15);
	obs_property_int_set_suffix(shutter, "°");
	obs_properties_add_group(props, "group_blur", obs_module_text("FlyingLayers.Group.Blur"), OBS_GROUP_NORMAL,
				 blur);

	for (const char *key : {kEasing, kCascade, kEnterEffect, kExitEffect, kMotionBlur})
		obs_property_set_modified_callback(obs_properties_get(props, key), refreshVisibility);

	return props;
}

void FlyingTransition::defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, kEasing, (int)fl::Easing::EaseInOut);
	obs_data_set_default_double(settings, kBezierX1, 0.25);
	obs_data_set_default_double(settings, kBezierY1, 0.1);
	obs_data_set_default_double(settings, kBezierX2, 0.25);
	obs_data_set_default_double(settings, kBezierY2, 1.0);
	obs_data_set_default_int(settings, kTimingMode, 0);
	obs_data_set_default_int(settings, kExitPortion, 100);
	obs_data_set_default_int(settings, kEnterPortion, 100);
	obs_data_set_default_int(settings, kCascade, 0);
	obs_data_set_default_int(settings, kCascadeOrder, (int)CascadeOrder::StackUp);
	obs_data_set_default_int(settings, kEnterEffect, (int)Appear::Fade);
	obs_data_set_default_int(settings, kExitEffect, (int)Appear::Fade);
	obs_data_set_default_int(settings, kSlideEdge, (int)fl::Edge::Auto);
	obs_data_set_default_bool(settings, kMotionFade, true);
	obs_data_set_default_bool(settings, kMotionBlur, false);
	obs_data_set_default_int(settings, kShutterAngle, 180);
}

// ---- obs_source_info callbacks ---------------------------------------------

const char *ftGetName(void *)
{
	return obs_module_text("FlyingLayers.Title");
}

void *ftCreate(obs_data_t *settings, obs_source_t *source)
{
	auto *transition = new FlyingTransition(settings, source);
	if (!transition->valid()) {
		delete transition;
		return nullptr;
	}
	return transition;
}

void ftDestroy(void *data)
{
	delete static_cast<FlyingTransition *>(data);
}

void ftUpdate(void *data, obs_data_t *settings)
{
	static_cast<FlyingTransition *>(data)->update(settings);
}

void ftVideoRender(void *data, gs_effect_t *)
{
	static_cast<FlyingTransition *>(data)->render();
}

float ftMixA(void *, float t)
{
	return 1.0f - t;
}

float ftMixB(void *, float t)
{
	return t;
}

bool ftAudioRender(void *data, uint64_t *tsOut, struct obs_source_audio_mix *audio, uint32_t mixers, size_t channels,
		   size_t sampleRate)
{
	// Normal crossfade, like the Fade transition.
	obs_source_t *self = static_cast<FlyingTransition *>(data)->source();
	return obs_transition_audio_render(self, tsOut, audio, mixers, channels, sampleRate, ftMixA, ftMixB);
}

enum gs_color_space ftColorSpace(void *data, size_t count, const enum gs_color_space *preferred)
{
	obs_source_t *self = static_cast<FlyingTransition *>(data)->source();
	const enum gs_color_space transitionSpace = obs_transition_video_get_color_space(self);

	enum gs_color_space space = transitionSpace;
	for (size_t i = 0; i < count; ++i) {
		space = preferred[i];
		if (space == transitionSpace)
			break;
	}
	return space;
}

obs_properties_t *ftProperties(void *)
{
	return FlyingTransition::properties();
}

void ftDefaults(obs_data_t *settings)
{
	FlyingTransition::defaults(settings);
}

void ftTransitionStart(void *data)
{
	static_cast<FlyingTransition *>(data)->transitionStarted();
}

} // namespace

void flying_layers_register_transition()
{
	obs_source_info info = {};
	info.id = "flying_layers_transition";
	info.type = OBS_SOURCE_TYPE_TRANSITION;
	info.get_name = ftGetName;
	info.create = ftCreate;
	info.destroy = ftDestroy;
	info.update = ftUpdate;
	info.video_render = ftVideoRender;
	info.audio_render = ftAudioRender;
	info.get_properties = ftProperties;
	info.get_defaults = ftDefaults;
	info.transition_start = ftTransitionStart;
	info.video_get_color_space = ftColorSpace;
	obs_register_source(&info);
}
