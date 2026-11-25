/*
FlyingLayers for OBS Studio
Copyright (C) 2026 YearNorth

addAlignment(), croppedSize(), cropToBounds(), calculateBounds() and the
transform part of computeLayout() follow add_alignment(), calc_cx(),
crop_to_bounds(), calculate_bounds_data() and update_item_transform() of
libobs/obs-scene.c (OBS Studio 33.0.1), Copyright (C) 2023 by Lain Bailey
and Philippe Groarke, GPL-2.0-or-later. They are reproduced on purpose so
that the transition places layers exactly where OBS draws them.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "layout.h"

#include <graphics/math-defs.h>
#include <graphics/vec3.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fl {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float lerp(float from, float to, float k)
{
	return from + (to - from) * k;
}

// Same as add_alignment() in libobs/obs-scene.c, integer division included.
void addAlignment(vec2 &v, uint32_t align, int cx, int cy)
{
	if (align & OBS_ALIGN_RIGHT)
		v.x += (float)cx;
	else if ((align & OBS_ALIGN_LEFT) == 0)
		v.x += (float)(cx / 2);

	if (align & OBS_ALIGN_BOTTOM)
		v.y += (float)cy;
	else if ((align & OBS_ALIGN_TOP) == 0)
		v.y += (float)(cy / 2);
}

// Same as calc_cx() / calc_cy() in libobs/obs-scene.c.
uint32_t croppedSize(uint32_t size, uint32_t crop)
{
	return (crop > size) ? 2 : (size - crop);
}

struct BoundsCrop {
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;
};

bool cropToBounds(const obs_transform_info &info)
{
	return info.crop_to_bounds &&
	       (info.bounds_type == OBS_BOUNDS_SCALE_OUTER || info.bounds_type == OBS_BOUNDS_SCALE_TO_HEIGHT ||
		info.bounds_type == OBS_BOUNDS_SCALE_TO_WIDTH);
}

// Same as calculate_bounds_data() in libobs/obs-scene.c, reading the bounds
// in absolute coordinates as returned by obs_sceneitem_get_info2().
void calculateBounds(const obs_transform_info &info, vec2 &origin, vec2 &scale, uint32_t &cx, uint32_t &cy,
		     BoundsCrop &boundsCrop)
{
	const vec2 bounds = info.bounds;

	float width = (float)cx * fabsf(scale.x);
	float height = (float)cy * fabsf(scale.y);
	const float itemAspect = width / height;
	const float boundsAspect = bounds.x / bounds.y;
	uint32_t boundsType = info.bounds_type;

	if (info.bounds_type == OBS_BOUNDS_MAX_ONLY)
		if (width > bounds.x || height > bounds.y)
			boundsType = OBS_BOUNDS_SCALE_INNER;

	if (boundsType == OBS_BOUNDS_SCALE_INNER || boundsType == OBS_BOUNDS_SCALE_OUTER) {
		bool useWidth = (boundsAspect < itemAspect);
		if (info.bounds_type == OBS_BOUNDS_SCALE_OUTER)
			useWidth = !useWidth;

		const float mul = useWidth ? bounds.x / width : bounds.y / height;
		vec2_mulf(&scale, &scale, mul);

	} else if (boundsType == OBS_BOUNDS_SCALE_TO_WIDTH) {
		vec2_mulf(&scale, &scale, bounds.x / width);

	} else if (boundsType == OBS_BOUNDS_SCALE_TO_HEIGHT) {
		vec2_mulf(&scale, &scale, bounds.y / height);

	} else if (boundsType == OBS_BOUNDS_STRETCH) {
		scale.x = copysignf(bounds.x / (float)cx, scale.x);
		scale.y = copysignf(bounds.y / (float)cy, scale.y);
	}

	width = (float)cx * scale.x;
	height = (float)cy * scale.y;

	const float widthDiff = bounds.x - fabsf(width);
	const float heightDiff = bounds.y - fabsf(height);
	cx = (uint32_t)bounds.x;
	cy = (uint32_t)bounds.y;

	addAlignment(origin, info.bounds_alignment, (int)-widthDiff, (int)-heightDiff);

	if (cropToBounds(info) && (widthDiff < -0.1 || heightDiff < -0.1)) {
		const bool cropWidth = widthDiff < -0.1;
		const bool cropFlipped = cropWidth ? width < 0.0f : height < 0.0f;

		const float cropDiff = cropWidth ? widthDiff : heightDiff;
		const float cropScale = cropWidth ? scale.x : scale.y;
		float cropOrigin = cropWidth ? origin.x : origin.y;

		const uint32_t alignMask = cropWidth ? OBS_ALIGN_LEFT | OBS_ALIGN_RIGHT
						     : OBS_ALIGN_TOP | OBS_ALIGN_BOTTOM;
		const uint32_t cropAlign = info.bounds_alignment & alignMask;

		const float overdraw = fabsf(cropDiff / cropScale);

		float overdrawTl;
		if (cropAlign & (OBS_ALIGN_TOP | OBS_ALIGN_LEFT))
			overdrawTl = 0;
		else if (cropAlign & (OBS_ALIGN_BOTTOM | OBS_ALIGN_RIGHT))
			overdrawTl = overdraw;
		else
			overdrawTl = overdraw / 2;

		const float overdrawBr = overdraw - overdrawTl;

		int cropBr, cropTl;
		if (cropFlipped) {
			if (cropAlign == OBS_ALIGN_CENTER)
				cropOrigin *= 2;
			else if (cropAlign & (OBS_ALIGN_TOP | OBS_ALIGN_LEFT))
				cropOrigin -= cropDiff;

			cropBr = (int)roundf(overdrawTl);
			cropTl = (int)roundf(overdrawBr);
		} else {
			cropOrigin = 0;
			cropBr = (int)roundf(overdrawBr);
			cropTl = (int)roundf(overdrawTl);
		}

		if (cropWidth) {
			boundsCrop.right = cropBr;
			boundsCrop.left = cropTl;
			origin.x = cropOrigin;
		} else {
			boundsCrop.bottom = cropBr;
			boundsCrop.top = cropTl;
			origin.y = cropOrigin;
		}
	}

	origin.x += (width < 0.0f) ? width : 0.0f;
	origin.y += (height < 0.0f) ? height : 0.0f;
}

} // namespace

namespace {

// Cubic Bezier from (0,0) to (1,1) with control points (x1,y1), (x2,y2):
// finds the curve parameter whose x is t, then returns its y.
float cubicBezier(const Bezier &bezier, float t)
{
	const float x1 = std::clamp(bezier.x1, 0.0f, 1.0f);
	const float x2 = std::clamp(bezier.x2, 0.0f, 1.0f);
	auto coord = [](float p1, float p2, float u) {
		const float v = 1.0f - u;
		return 3.0f * v * v * u * p1 + 3.0f * v * u * u * p2 + u * u * u;
	};

	// x(u) is monotonic because x1, x2 are within 0..1: bisection is
	// robust and 24 steps are well below float precision.
	float lo = 0.0f, hi = 1.0f, u = t;
	for (int i = 0; i < 24; ++i) {
		u = 0.5f * (lo + hi);
		if (coord(x1, x2, u) < t)
			lo = u;
		else
			hi = u;
	}
	return coord(bezier.y1, bezier.y2, u);
}

} // namespace

float applyEasing(Easing easing, float t, const Bezier &bezier)
{
	t = std::clamp(t, 0.0f, 1.0f);
	if (t <= 0.0f)
		return 0.0f;
	if (t >= 1.0f)
		return 1.0f;

	constexpr float back = 1.70158f;

	switch (easing) {
	case Easing::EaseIn:
		return t * t * t;
	case Easing::EaseOut: {
		const float u = 1.0f - t;
		return 1.0f - u * u * u;
	}
	case Easing::EaseInOut:
		if (t < 0.5f)
			return 4.0f * t * t * t;
		else {
			const float u = -2.0f * t + 2.0f;
			return 1.0f - u * u * u / 2.0f;
		}
	case Easing::Smooth:
		return 0.5f - 0.5f * cosf(kPi * t);
	case Easing::BackOut: {
		// Classic "back out" starts at almost five times the average
		// speed, which reads as a jolt; t^2 makes it start from rest.
		const float u = t * t - 1.0f;
		return 1.0f + (back + 1.0f) * u * u * u + back * u * u;
	}
	case Easing::BackInOut: {
		const float c = back * 1.525f;
		if (t < 0.5f) {
			const float u = 2.0f * t;
			return u * u * ((c + 1.0f) * u - c) / 2.0f;
		}
		const float u = 2.0f * t - 2.0f;
		return (u * u * ((c + 1.0f) * u + c) + 2.0f) / 2.0f;
	}
	case Easing::ElasticOut: {
		const float u = t * t; // start from rest, as for BackOut
		return powf(2.0f, -10.0f * u) * sinf((u * 10.0f - 0.75f) * (2.0f * kPi / 3.0f)) + 1.0f;
	}
	case Easing::Custom:
		return cubicBezier(bezier, t);
	case Easing::Linear:
	default:
		return t;
	}
}

// ---- Timing
// -------------------------------------------------------------------

void groupWindows(const Timing &timing, Window &exit, Window &fly, Window &enter)
{
	float exitPart = std::clamp(timing.exitPortion, 0.05f, 1.0f);
	float enterPart = std::clamp(timing.enterPortion, 0.05f, 1.0f);

	if (!timing.sequenced) {
		// All start together; exits end early, entries start late.
		exit = Window{0.0f, exitPart};
		fly = Window{0.0f, 1.0f};
		enter = Window{1.0f - enterPart, enterPart};
		return;
	}

	// One after the other, leaving at least 10 % for the flight.
	const float sum = exitPart + enterPart;
	if (sum > 0.9f) {
		exitPart *= 0.9f / sum;
		enterPart *= 0.9f / sum;
	}
	exit = Window{0.0f, exitPart};
	fly = Window{exitPart, 1.0f - exitPart - enterPart};
	enter = Window{1.0f - enterPart, enterPart};
}

Window cascadeWindow(const Window &group, float cascade, int index, int count)
{
	cascade = std::clamp(cascade, 0.0f, 0.9f);
	if (count <= 1 || cascade <= 0.0f)
		return group;

	// Each member runs for (1 - cascade) of the group window; starts are
	// spread evenly over the rest.
	const float length = group.length * (1.0f - cascade);
	const float step = group.length * cascade / (float)(count - 1);
	return Window{group.start + step * (float)index, length};
}

float localProgress(const Window &window, float t)
{
	if (window.length <= 0.0f)
		return t >= window.start ? 1.0f : 0.0f;
	return std::clamp((t - window.start) / window.length, 0.0f, 1.0f);
}

bool Layout::hasCrop() const
{
	return cropL > 0.001f || cropT > 0.001f || fabsf(contentW - srcW) > 0.001f || fabsf(contentH - srcH) > 0.001f;
}

bool computeLayout(const obs_transform_info &info, const obs_sceneitem_crop &crop, uint32_t srcW, uint32_t srcH,
		   Layout &out)
{
	if (!srcW || !srcH)
		return false;

	// Mirrors update_item_transform() in libobs/obs-scene.c.
	uint32_t cx = croppedSize(srcW, (uint32_t)crop.left + (uint32_t)crop.right);
	uint32_t cy = croppedSize(srcH, (uint32_t)crop.top + (uint32_t)crop.bottom);

	vec2 origin;
	vec2_zero(&origin);
	vec2 scale = info.scale;
	const vec2 position = info.pos;
	BoundsCrop boundsCrop;

	if (info.bounds_type != OBS_BOUNDS_NONE) {
		if (info.bounds.x <= 0.0f || info.bounds.y <= 0.0f)
			return false;
		calculateBounds(info, origin, scale, cx, cy, boundsCrop);
	} else {
		cx = (uint32_t)((float)cx * scale.x);
		cy = (uint32_t)((float)cy * scale.y);
	}

	addAlignment(origin, info.alignment, (int)cx, (int)cy);

	matrix4 drawTransform;
	matrix4_identity(&drawTransform);
	matrix4_scale3f(&drawTransform, &drawTransform, scale.x, scale.y, 1.0f);
	matrix4_translate3f(&drawTransform, &drawTransform, -origin.x, -origin.y, 0.0f);
	matrix4_rotate_aa4f(&drawTransform, &drawTransform, 0.0f, 0.0f, 1.0f, RAD(info.rot));
	matrix4_translate3f(&drawTransform, &drawTransform, position.x, position.y, 0.0f);

	// What the scene actually draws: the source minus user and bounds crop.
	const uint32_t texW =
		croppedSize(srcW, (uint32_t)(crop.left + crop.right + boundsCrop.left + boundsCrop.right));
	const uint32_t texH =
		croppedSize(srcH, (uint32_t)(crop.top + crop.bottom + boundsCrop.top + boundsCrop.bottom));

	out.srcW = (float)srcW;
	out.srcH = (float)srcH;
	out.cropL = (float)(crop.left + boundsCrop.left);
	out.cropT = (float)(crop.top + boundsCrop.top);
	out.contentW = (float)texW;
	out.contentH = (float)texH;
	out.sx = scale.x;
	out.sy = scale.y;
	out.rot = info.rot;

	vec3 local;
	vec3_set(&local, (float)texW * 0.5f, (float)texH * 0.5f, 0.0f);
	vec3 centre;
	vec3_transform(&centre, &local, &drawTransform);
	out.cx = centre.x;
	out.cy = centre.y;

	return std::isfinite(out.cx) && std::isfinite(out.cy) && std::isfinite(out.sx) && std::isfinite(out.sy);
}

bool fitLayout(uint32_t srcW, uint32_t srcH, float trW, float trH, obs_transition_scale_type scaleType,
	       uint32_t alignment, Layout &out)
{
	if (!srcW || !srcH || trW <= 0.0f || trH <= 0.0f)
		return false;

	const float sourceW = (float)srcW;
	const float sourceH = (float)srcH;
	float sx = 1.0f, sy = 1.0f;

	if (scaleType == OBS_TRANSITION_SCALE_MAX_ONLY && (sourceW > trW || sourceH > trH))
		scaleType = OBS_TRANSITION_SCALE_ASPECT;

	if (scaleType == OBS_TRANSITION_SCALE_ASPECT) {
		const bool useWidth = trW / trH < sourceW / sourceH;
		sx = sy = useWidth ? trW / sourceW : trH / sourceH;
	} else if (scaleType == OBS_TRANSITION_SCALE_STRETCH) {
		sx = trW / sourceW;
		sy = trH / sourceH;
	}

	const float scaledW = sourceW * sx;
	const float scaledH = sourceH * sy;
	vec2 pos;
	vec2_zero(&pos);
	addAlignment(pos, alignment, (int)(trW - scaledW), (int)(trH - scaledH));

	out.srcW = out.contentW = sourceW;
	out.srcH = out.contentH = sourceH;
	out.cropL = out.cropT = 0.0f;
	out.sx = sx;
	out.sy = sy;
	out.rot = 0.0f;
	out.cx = pos.x + scaledW * 0.5f;
	out.cy = pos.y + scaledH * 0.5f;
	return true;
}

void placeInFrame(Layout &layer, const Layout &fit)
{
	const float originX = fit.cx - fit.srcW * fit.sx * 0.5f;
	const float originY = fit.cy - fit.srcH * fit.sy * 0.5f;
	layer.cx = originX + layer.cx * fit.sx;
	layer.cy = originY + layer.cy * fit.sy;
	layer.sx *= fit.sx;
	layer.sy *= fit.sy;
}

Layout interpolate(const Layout &a, const Layout &b, float k)
{
	if (k == 0.0f)
		return a;
	if (k == 1.0f)
		return b;

	float turn = fmodf(b.rot - a.rot, 360.0f);
	if (turn > 180.0f)
		turn -= 360.0f;
	else if (turn < -180.0f)
		turn += 360.0f;

	// Overshooting curves must not turn a layer inside out.
	auto scale = [k](float from, float to) {
		const float v = lerp(from, to, k);
		if (from > 0.0f && to > 0.0f)
			return std::max(v, 1e-4f);
		if (from < 0.0f && to < 0.0f)
			return std::min(v, -1e-4f);
		return v;
	};

	Layout out;
	out.srcW = b.srcW;
	out.srcH = b.srcH;
	out.cropL = std::clamp(lerp(a.cropL, b.cropL, k), 0.0f, out.srcW - 1.0f);
	out.cropT = std::clamp(lerp(a.cropT, b.cropT, k), 0.0f, out.srcH - 1.0f);
	out.contentW = std::clamp(lerp(a.contentW, b.contentW, k), 1.0f, out.srcW - out.cropL);
	out.contentH = std::clamp(lerp(a.contentH, b.contentH, k), 1.0f, out.srcH - out.cropT);
	out.sx = scale(a.sx, b.sx);
	out.sy = scale(a.sy, b.sy);
	out.rot = a.rot + turn * k;
	out.cx = lerp(a.cx, b.cx, k);
	out.cy = lerp(a.cy, b.cy, k);
	return out;
}

void layoutMatrix(const Layout &layout, matrix4 &out)
{
	matrix4_identity(&out);
	matrix4_translate3f(&out, &out, -layout.contentW * 0.5f, -layout.contentH * 0.5f, 0.0f);
	matrix4_scale3f(&out, &out, layout.sx, layout.sy, 1.0f);
	matrix4_rotate_aa4f(&out, &out, 0.0f, 0.0f, 1.0f, RAD(layout.rot));
	matrix4_translate3f(&out, &out, layout.cx, layout.cy, 0.0f);
}

void layoutCorners(const Layout &layout, vec2 corners[4])
{
	matrix4 m;
	layoutMatrix(layout, m);

	const float xs[4] = {0.0f, layout.contentW, layout.contentW, 0.0f};
	const float ys[4] = {0.0f, 0.0f, layout.contentH, layout.contentH};
	for (int i = 0; i < 4; ++i) {
		vec3 corner;
		vec3_set(&corner, xs[i], ys[i], 0.0f);
		vec3 p;
		vec3_transform(&p, &corner, &m);
		vec2_set(&corners[i], p.x, p.y);
	}
}

void sourceCorners(const Layout &layout, vec2 corners[4])
{
	matrix4 m;
	layoutMatrix(layout, m);

	const float x0 = -layout.cropL, x1 = layout.srcW - layout.cropL;
	const float y0 = -layout.cropT, y1 = layout.srcH - layout.cropT;
	const float xs[4] = {x0, x1, x1, x0};
	const float ys[4] = {y0, y0, y1, y1};
	for (int i = 0; i < 4; ++i) {
		vec3 corner;
		vec3_set(&corner, xs[i], ys[i], 0.0f);
		vec3 p;
		vec3_transform(&p, &corner, &m);
		vec2_set(&corners[i], p.x, p.y);
	}
}

Rect layoutBounds(const Layout &layout)
{
	vec2 corners[4];
	layoutCorners(layout, corners);

	Rect r{INFINITY, INFINITY, -INFINITY, -INFINITY};
	for (const vec2 &p : corners) {
		r.minX = std::min(r.minX, p.x);
		r.minY = std::min(r.minY, p.y);
		r.maxX = std::max(r.maxX, p.x);
		r.maxY = std::max(r.maxY, p.y);
	}
	return r;
}

Edge resolveEdge(const Layout &layout, Edge edge, float canvasW, float canvasH)
{
	if (edge != Edge::Auto)
		return edge;

	// Nearest edge to the layer's centre, so it moves the short way.
	const float left = layout.cx;
	const float right = canvasW - layout.cx;
	const float top = layout.cy;
	const float bottom = canvasH - layout.cy;
	const float nearest = std::min(std::min(left, right), std::min(top, bottom));
	if (nearest == left)
		return Edge::Left;
	if (nearest == right)
		return Edge::Right;
	if (nearest == top)
		return Edge::Top;
	return Edge::Bottom;
}

void offscreenOffset(const Layout &layout, Edge edge, float canvasW, float canvasH, float &dx, float &dy)
{
	const Rect r = layoutBounds(layout);

	dx = 0.0f;
	dy = 0.0f;
	switch (resolveEdge(layout, edge, canvasW, canvasH)) {
	case Edge::Left:
		dx = -std::max(r.maxX, 0.0f);
		break;
	case Edge::Right:
		dx = std::max(canvasW - r.minX, 0.0f);
		break;
	case Edge::Top:
		dy = -std::max(r.maxY, 0.0f);
		break;
	case Edge::Bottom:
	default:
		dy = std::max(canvasH - r.minY, 0.0f);
		break;
	}
}

void wipe(Layout &layout, Edge edge, float hidden)
{
	hidden = std::clamp(hidden, 0.0f, 1.0f);
	const bool horizontal = edge == Edge::Left || edge == Edge::Right;

	// The hidden side is opposite to the edge the wipe starts from, as
	// seen on screen; with a flipped layer that is the other source side.
	const bool hideScreenRightOrBottom = edge == Edge::Left || edge == Edge::Top;
	const float sign = horizontal ? layout.sx : layout.sy;
	const bool hideSourceEnd = hideScreenRightOrBottom == (sign >= 0.0f);

	const float size = horizontal ? layout.contentW : layout.contentH;
	const float cut = std::min(size * hidden, size - 0.01f);

	// Centre of the visible part moves by half the cut, in source pixels,
	// along the layer's own axis.
	const float shift = hideSourceEnd ? -cut * 0.5f : cut * 0.5f;
	if (horizontal) {
		if (!hideSourceEnd)
			layout.cropL += cut;
		layout.contentW -= cut;
	} else {
		if (!hideSourceEnd)
			layout.cropT += cut;
		layout.contentH -= cut;
	}

	matrix4 rotation;
	matrix4_identity(&rotation);
	matrix4_rotate_aa4f(&rotation, &rotation, 0.0f, 0.0f, 1.0f, RAD(layout.rot));
	vec3 local;
	vec3_set(&local, horizontal ? shift * layout.sx : 0.0f, horizontal ? 0.0f : shift * layout.sy, 0.0f);
	vec3 moved;
	vec3_transform(&moved, &local, &rotation);
	layout.cx += moved.x;
	layout.cy += moved.y;
}

std::vector<Pair> planLayers(const std::vector<const char *> &a, const std::vector<const char *> &b)
{
	auto same = [](const char *x, const char *y) {
		return x && y && std::strcmp(x, y) == 0;
	};

	// n-th occurrence of a key in B pairs with the n-th occurrence in A.
	std::vector<int> matchOfB(b.size(), -1);
	std::vector<int> matchOfA(a.size(), -1);
	for (size_t j = 0; j < b.size(); ++j) {
		int occurrence = 0;
		for (size_t p = 0; p < j; ++p)
			if (same(b[p], b[j]))
				++occurrence;

		for (size_t i = 0; i < a.size(); ++i) {
			if (!same(a[i], b[j]))
				continue;
			if (occurrence-- == 0) {
				matchOfB[j] = (int)i;
				matchOfA[i] = (int)j;
				break;
			}
		}
	}

	std::vector<Pair> order;
	order.reserve(a.size() + b.size());
	for (size_t j = 0; j < b.size(); ++j)
		order.push_back(Pair{matchOfB[j], (int)j});

	// Layers leaving the scene go right above the layer that was below
	// them in A (or at the bottom when there is none).
	int anchor = -1;
	for (size_t i = 0; i < a.size(); ++i) {
		if (matchOfA[i] >= 0) {
			for (size_t pos = 0; pos < order.size(); ++pos) {
				if (order[pos].a == (int)i) {
					anchor = (int)pos;
					break;
				}
			}
			continue;
		}

		order.insert(order.begin() + (anchor + 1), Pair{(int)i, -1});
		++anchor;
	}

	return order;
}

} // namespace fl
