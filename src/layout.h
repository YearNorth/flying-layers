/*
FlyingLayers for OBS Studio
Copyright (C) 2026 YearNorth

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Pure geometry used by the transition: where a layer is on screen, how to
// interpolate between two placements and how to pair the layers of two
// scenes. Nothing here touches the GPU, so it can be tested on its own.

#pragma once

#include <graphics/matrix4.h>
#include <obs.h>

#include <cstdint>
#include <vector>

namespace fl {

enum class Easing : int {
	Linear = 0,
	EaseIn,
	EaseOut,
	EaseInOut,
	Smooth,
	BackOut,    // soft start, overshoots the destination a little, then settles
	BackInOut,  // pulls back a little, overshoots, then settles
	ElasticOut, // soft start, then springs around the destination
	Custom,     // cubic Bezier, as CSS cubic-bezier(x1, y1, x2, y2)
};

// Control points of a custom curve. x1 and x2 are kept within 0..1.
struct Bezier {
	float x1 = 0.25f;
	float y1 = 0.1f;
	float x2 = 0.25f;
	float y2 = 1.0f;
};

// Maps linear progress t (0..1) to eased progress. The result is 0 at t = 0
// and 1 at t = 1, and can go outside 0..1 in between (back, elastic, some
// custom curves).
float applyEasing(Easing easing, float t, const Bezier &bezier = Bezier());

// ---- Timing ----------------------------------------------------------------

// Part of the transition (0..1) during which something happens.
struct Window {
	float start = 0.0f;
	float length = 1.0f;
};

struct Timing {
	bool sequenced = false;    // exits, then flights, then entries
	float exitPortion = 1.0f;  // share of the transition used by layers going out
	float enterPortion = 1.0f; // share used by layers coming in
	float cascade = 0.0f;      // 0 = all together, up to 0.9 = one after the other
};

// Windows of the layers going out, flying and coming in.
void groupWindows(const Timing &timing, Window &exit, Window &fly, Window &enter);

// Window of member `index` of `count` inside a group window, with the
// cascade delay between members.
Window cascadeWindow(const Window &group, float cascade, int index, int count);

// Progress (0..1) of transition time t inside a window.
float localProgress(const Window &window, float t);

// Placement of one layer in scene pixels, in a form that interpolates well:
// the centre of the visible content, its scale, rotation and crop.
struct Layout {
	float srcW = 0.0f; // source size before crop
	float srcH = 0.0f;
	float cropL = 0.0f; // offset of the visible part, in source pixels
	float cropT = 0.0f;
	float contentW = 0.0f; // size of the visible part, in source pixels
	float contentH = 0.0f;
	float sx = 1.0f; // source pixel -> scene pixel, signed (negative = flipped)
	float sy = 1.0f;
	float rot = 0.0f; // degrees, clockwise as in OBS
	float cx = 0.0f;  // centre of the visible part, in scene pixels
	float cy = 0.0f;

	bool hasCrop() const;
};

// Placement of a scene item, computed exactly like libobs does when it
// renders the scene (alignment, bounding box, crop to bounds, flips).
// Returns false when the item has nothing to draw.
bool computeLayout(const obs_transform_info &info, const obs_sceneitem_crop &crop, uint32_t srcW, uint32_t srcH,
		   Layout &out);

// Placement OBS gives to a single source (not a scene) shown by a
// transition of size trW x trH: the same scaling and alignment as
// recalculate_transition_matrix() in libobs/obs-source-transition.c.
bool fitLayout(uint32_t srcW, uint32_t srcH, float trW, float trH, obs_transition_scale_type scaleType,
	       uint32_t alignment, Layout &out);

// Moves a layer of a scene into the frame of a transition, where `fit` is
// the placement fitLayout() gives to the whole scene. A scene the size of the
// frame is left as it is. With a stretched scene, a rotated layer keeps its
// shape (OBS would skew it).
void placeInFrame(Layout &layer, const Layout &fit);

// Interpolates two placements. Rotation takes the shortest way round. k can
// go a little outside 0..1 (overshooting curves): crop stays within the
// source and scale does not change sign unless the two placements differ in
// sign.
Layout interpolate(const Layout &a, const Layout &b, float k);

// Matrix mapping visible content pixels (0..contentW, 0..contentH) to scene
// pixels.
void layoutMatrix(const Layout &layout, matrix4 &out);

struct Rect {
	float minX, minY, maxX, maxY;
};

// Axis-aligned box of the layer on screen, rotation included.
Rect layoutBounds(const Layout &layout);

// Corners of the layer on screen.
void layoutCorners(const Layout &layout, vec2 corners[4]);

// Corners of the whole source (crop ignored) on screen. They move when the
// layer moves, scales or turns, but not when only its crop changes (wipe).
void sourceCorners(const Layout &layout, vec2 corners[4]);

enum class Edge : int {
	Auto = 0, // nearest edge of the canvas
	Left,
	Right,
	Top,
	Bottom,
};

// `edge`, with Auto replaced by the canvas edge nearest to the layer.
Edge resolveEdge(const Layout &layout, Edge edge, float canvasW, float canvasH);

// Offset that moves the layer just outside the canvas through `edge`.
void offscreenOffset(const Layout &layout, Edge edge, float canvasW, float canvasH, float &dx, float &dy);

// Hides the share `hidden` (0..1) of the layer on the side opposite to
// `edge`, as a wipe that starts from `edge` does. The visible part stays in
// place. `edge` is in screen terms (Left is the left of the layer as seen,
// flips included) and must not be Auto.
void wipe(Layout &layout, Edge edge, float hidden);

// One entry of the draw list: index of the layer in scene A and in scene B,
// or -1 when the layer is only in one of them.
struct Pair {
	int a = -1;
	int b = -1;
};

// Pairs the layers of two scenes and returns them in draw order, bottom to
// top. Lists are bottom to top; layers match when their keys are equal
// (the n-th occurrence of a key in A matches the n-th one in B). The order
// is the one of scene B; layers only in A keep their place relative to the
// layers below them in A.
std::vector<Pair> planLayers(const std::vector<const char *> &a, const std::vector<const char *> &b);

} // namespace fl
