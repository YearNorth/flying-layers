/*
Flying Layers for OBS Studio
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

#include <obs.h>
#include <graphics/matrix4.h>

#include <cstdint>
#include <vector>

namespace fl {

enum class Easing : int {
	Linear = 0,
	EaseIn,
	EaseOut,
	EaseInOut,
	Smooth,
};

// Maps linear progress t (0..1) to eased progress (0..1).
float applyEasing(Easing easing, float t);

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

// Interpolates two placements. Rotation takes the shortest way round.
Layout interpolate(const Layout &a, const Layout &b, float k);

// Matrix mapping visible content pixels (0..contentW, 0..contentH) to scene
// pixels.
void layoutMatrix(const Layout &layout, matrix4 &out);

struct Rect {
	float minX, minY, maxX, maxY;
};

// Axis-aligned box of the layer on screen, rotation included.
Rect layoutBounds(const Layout &layout);

enum class Edge : int {
	Auto = 0, // nearest edge of the canvas
	Left,
	Right,
	Top,
	Bottom,
};

// Offset that moves the layer just outside the canvas through `edge`.
void offscreenOffset(const Layout &layout, Edge edge, float canvasW, float canvasH, float &dx, float &dy);

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
