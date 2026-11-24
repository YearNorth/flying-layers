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
#include <vector>

namespace {

// Settings keys
constexpr const char *kEasing = "easing";
constexpr const char *kEnterEffect = "enter_effect";
constexpr const char *kExitEffect = "exit_effect";
constexpr const char *kSlideEdge = "slide_edge";
constexpr const char *kMotionFade = "motion_fade";

// How a layer that is only in one of the two scenes comes in or goes out.
enum class Appear : int {
	Fade = 0,
	Zoom,
	Slide,
	Cut,
};

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
		if (effect_)
			gs_effect_destroy(effect_);
		obs_leave_graphics();
	}

	bool valid() const { return effect_ != nullptr; }
	obs_source_t *source() const { return self_; }

	void update(obs_data_t *settings)
	{
		easing_ = (int)obs_data_get_int(settings, kEasing);
		enterEffect_ = (int)obs_data_get_int(settings, kEnterEffect);
		exitEffect_ = (int)obs_data_get_int(settings, kExitEffect);
		slideEdge_ = (int)obs_data_get_int(settings, kSlideEdge);
		motionFade_ = obs_data_get_bool(settings, kMotionFade);
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
			// Not two scenes (should not happen with the main output):
			// plain crossfade.
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

	static obs_properties_t *properties()
	{
		obs_properties_t *props = obs_properties_create();

		obs_property_t *easing = obs_properties_add_list(props, kEasing, obs_module_text("FlyingLayers.Easing"),
								 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(easing, obs_module_text("FlyingLayers.Easing.Linear"),
					  (int)fl::Easing::Linear);
		obs_property_list_add_int(easing, obs_module_text("FlyingLayers.Easing.EaseIn"),
					  (int)fl::Easing::EaseIn);
		obs_property_list_add_int(easing, obs_module_text("FlyingLayers.Easing.EaseOut"),
					  (int)fl::Easing::EaseOut);
		obs_property_list_add_int(easing, obs_module_text("FlyingLayers.Easing.EaseInOut"),
					  (int)fl::Easing::EaseInOut);
		obs_property_list_add_int(easing, obs_module_text("FlyingLayers.Easing.Smooth"),
					  (int)fl::Easing::Smooth);

		addAppearList(props, kEnterEffect, "FlyingLayers.Enter");
		addAppearList(props, kExitEffect, "FlyingLayers.Exit");

		obs_property_t *edge = obs_properties_add_list(props, kSlideEdge,
							       obs_module_text("FlyingLayers.SlideEdge"),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(edge, obs_module_text("FlyingLayers.Edge.Auto"), (int)fl::Edge::Auto);
		obs_property_list_add_int(edge, obs_module_text("FlyingLayers.Edge.Left"), (int)fl::Edge::Left);
		obs_property_list_add_int(edge, obs_module_text("FlyingLayers.Edge.Right"), (int)fl::Edge::Right);
		obs_property_list_add_int(edge, obs_module_text("FlyingLayers.Edge.Top"), (int)fl::Edge::Top);
		obs_property_list_add_int(edge, obs_module_text("FlyingLayers.Edge.Bottom"), (int)fl::Edge::Bottom);

		obs_properties_add_bool(props, kMotionFade, obs_module_text("FlyingLayers.MotionFade"));

		return props;
	}

	static void defaults(obs_data_t *settings)
	{
		obs_data_set_default_int(settings, kEasing, (int)fl::Easing::EaseInOut);
		obs_data_set_default_int(settings, kEnterEffect, (int)Appear::Fade);
		obs_data_set_default_int(settings, kExitEffect, (int)Appear::Fade);
		obs_data_set_default_int(settings, kSlideEdge, (int)fl::Edge::Auto);
		obs_data_set_default_bool(settings, kMotionFade, true);
	}

private:
	static void addAppearList(obs_properties_t *props, const char *key, const char *label)
	{
		obs_property_t *list = obs_properties_add_list(props, key, obs_module_text(label), OBS_COMBO_TYPE_LIST,
							       OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(list, obs_module_text("FlyingLayers.Appear.Fade"), (int)Appear::Fade);
		obs_property_list_add_int(list, obs_module_text("FlyingLayers.Appear.Zoom"), (int)Appear::Zoom);
		obs_property_list_add_int(list, obs_module_text("FlyingLayers.Appear.Slide"), (int)Appear::Slide);
		obs_property_list_add_int(list, obs_module_text("FlyingLayers.Appear.Cut"), (int)Appear::Cut);
	}

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

	// ---- Drawing ------------------------------------------------------

	void renderFlying(obs_source_t *sourceA, obs_source_t *sourceB, obs_scene_t *sceneA, obs_scene_t *sceneB,
			  float t)
	{
		if (replan_.exchange(false) || planA_ != sourceA || planB_ != sourceB)
			buildPlan(sourceA, sourceB, sceneA, sceneB);

		const float k = fl::applyEasing((fl::Easing)easing_.load(), t);
		const float canvasW = (float)obs_source_get_width(sourceB);
		const float canvasH = (float)obs_source_get_height(sourceB);
		const Appear enter = (Appear)enterEffect_.load();
		const Appear exit = (Appear)exitEffect_.load();
		const fl::Edge edge = (fl::Edge)slideEdge_.load();
		const bool motionFade = motionFade_.load();

		gs_blend_state_push();
		gs_reset_blend_state();

		size_t textureIndex = 0;
		for (const PlannedLayer &layer : plan_) {
			LayerState a, b;
			const bool hasA = readLayer(layer.a, a);
			const bool hasB = readLayer(layer.b, b);

			if (hasA && hasB) {
				// Same layer in both scenes: fly.
				const fl::Layout current = fl::interpolate(a.layout, b.layout, k);
				drawLayer(b.source, current, 1.0f, b.nestedScene, textureIndex);
			} else if (hasB) {
				// Coming in: the effect runs backwards towards B.
				fl::Layout current = b.layout;
				const float opacity =
					appear(current, enter, 1.0f - k, t >= 0.5f, edge, motionFade, canvasW, canvasH);
				drawLayer(b.source, current, opacity, b.nestedScene, textureIndex);
			} else if (hasA) {
				// Going out.
				fl::Layout current = a.layout;
				const float opacity =
					appear(current, exit, k, t < 0.5f, edge, motionFade, canvasW, canvasH);
				drawLayer(a.source, current, opacity, a.nestedScene, textureIndex);
			}
		}

		gs_blend_state_pop();
	}

	// Applies an entry/exit effect. `away` goes from 0 (layer in place) to
	// 1 (layer gone). Changes `layout` and returns the opacity.
	static float appear(fl::Layout &layout, Appear effect, float away, bool cutVisible, fl::Edge edge,
			    bool motionFade, float canvasW, float canvasH)
	{
		away = std::clamp(away, 0.0f, 1.0f);

		switch (effect) {
		case Appear::Zoom: {
			const float s = 1.0f - away;
			layout.sx *= s;
			layout.sy *= s;
			return motionFade ? 1.0f - away : 1.0f;
		}
		case Appear::Slide: {
			float dx, dy;
			fl::offscreenOffset(layout, edge, canvasW, canvasH, dx, dy);
			layout.cx += dx * away;
			layout.cy += dy * away;
			return motionFade ? 1.0f - away : 1.0f;
		}
		case Appear::Cut:
			return cutVisible ? 1.0f : 0.0f;
		case Appear::Fade:
		default:
			return 1.0f - away;
		}
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

	void drawLayer(obs_source_t *source, const fl::Layout &layout, float opacity, bool nestedScene,
		       size_t &textureIndex)
	{
		if (opacity <= 0.001f || fabsf(layout.sx) < 1e-6f || fabsf(layout.sy) < 1e-6f)
			return;

		matrix4 transform;
		fl::layoutMatrix(layout, transform);

		const bool useTexture = nestedScene || layout.hasCrop() || opacity < 0.999f;
		if (!useTexture || !effect_) {
			// Fast path: draw the source straight onto the output.
			gs_matrix_push();
			gs_matrix_mul(&transform);
			obs_source_video_render(source);
			gs_matrix_pop();
			return;
		}

		// Render the visible part of the source to a texture, then draw it
		// with the layer's opacity, as OBS does for cropped items.
		const uint32_t texW = (uint32_t)std::max(1.0f, ceilf(layout.contentW - 0.001f));
		const uint32_t texH = (uint32_t)std::max(1.0f, ceilf(layout.contentH - 0.001f));

		const gs_color_space space = gs_get_color_space();
		gs_texrender_t *texrender = texrenderAt(textureIndex++, gs_get_format_from_space(space));
		if (!texrender)
			return;

		gs_texrender_reset(texrender);
		if (gs_texrender_begin_with_color_space(texrender, texW, texH, space)) {
			vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)texW, 0.0f, (float)texH, -100.0f, 100.0f);
			gs_matrix_translate3f(-layout.cropL, -layout.cropT, 0.0f);

			gs_blend_state_push();
			gs_reset_blend_state();
			obs_source_video_render(source);
			gs_blend_state_pop();

			gs_texrender_end(texrender);
		}

		gs_texture_t *texture = gs_texrender_get_texture(texrender);
		if (!texture)
			return;

		gs_matrix_push();
		gs_matrix_mul(&transform);

		// The texture holds premultiplied alpha.
		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		drawTexture(texture, opacity, 0, 0);
		gs_blend_state_pop();

		gs_matrix_pop();
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

	// Fallback when one side is not a scene: crossfade, the two
	// premultiplied images summed with their weights.
	static void crossfadeCallback(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy)
	{
		auto *self = static_cast<FlyingTransition *>(data);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
		if (a)
			self->drawTexture(a, 1.0f - t, cx, cy);
		if (b)
			self->drawTexture(b, t, cx, cy);
		gs_blend_state_pop();
	}

	obs_source_t *self_;
	gs_effect_t *effect_ = nullptr;
	gs_eparam_t *imageParam_ = nullptr;
	gs_eparam_t *opacityParam_ = nullptr;

	// Written by the UI thread, read by the graphics thread.
	std::atomic<int> easing_{(int)fl::Easing::EaseInOut};
	std::atomic<int> enterEffect_{(int)Appear::Fade};
	std::atomic<int> exitEffect_{(int)Appear::Fade};
	std::atomic<int> slideEdge_{(int)fl::Edge::Auto};
	std::atomic<bool> motionFade_{true};
	std::atomic<bool> replan_{true};

	// Graphics thread only.
	std::vector<PlannedLayer> plan_;
	obs_source_t *planA_ = nullptr; // identity only, not referenced
	obs_source_t *planB_ = nullptr;
	std::vector<gs_texrender_t *> texrenders_;
};

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
