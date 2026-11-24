/*
Flying Layers for OBS Studio
Copyright (C) 2026 YearNorth

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "strings.h"

#include <obs-module.h>

#include <cstring>

namespace fl {

const BuiltInString kBuiltInStrings[] = {
	{"FlyingLayers.Title", "Flying Layers"},
	{"FlyingLayers.Group.Curve", "Curve"},
	{"FlyingLayers.Group.Timing", "Timing"},
	{"FlyingLayers.Group.Appear", "In and out"},
	{"FlyingLayers.Group.Blur", "Motion blur"},
	{"FlyingLayers.Easing", "Curve"},
	{"FlyingLayers.Easing.Linear", "Linear"},
	{"FlyingLayers.Easing.EaseIn", "Ease in"},
	{"FlyingLayers.Easing.EaseOut", "Ease out"},
	{"FlyingLayers.Easing.EaseInOut", "Ease in-out"},
	{"FlyingLayers.Easing.Smooth", "Smooth"},
	{"FlyingLayers.Easing.BackOut", "Back"},
	{"FlyingLayers.Easing.BackInOut", "Back in-out"},
	{"FlyingLayers.Easing.ElasticOut", "Elastic"},
	{"FlyingLayers.Easing.Custom", "Bézier"},
	{"FlyingLayers.Bezier.X1", "X1"},
	{"FlyingLayers.Bezier.Y1", "Y1"},
	{"FlyingLayers.Bezier.X2", "X2"},
	{"FlyingLayers.Bezier.Y2", "Y2"},
	{"FlyingLayers.TimingMode", "Order"},
	{"FlyingLayers.TimingMode.Together", "Together"},
	{"FlyingLayers.TimingMode.Sequenced", "Out, fly, in"},
	{"FlyingLayers.ExitPortion", "Out time"},
	{"FlyingLayers.EnterPortion", "In time"},
	{"FlyingLayers.Cascade", "Cascade"},
	{"FlyingLayers.CascadeOrder", "Cascade order"},
	{"FlyingLayers.CascadeOrder.StackUp", "Layers, bottom first"},
	{"FlyingLayers.CascadeOrder.StackDown", "Layers, top first"},
	{"FlyingLayers.CascadeOrder.LeftToRight", "Left to right"},
	{"FlyingLayers.CascadeOrder.RightToLeft", "Right to left"},
	{"FlyingLayers.CascadeOrder.TopToBottom", "Top to bottom"},
	{"FlyingLayers.CascadeOrder.BottomToTop", "Bottom to top"},
	{"FlyingLayers.Enter", "In"},
	{"FlyingLayers.Exit", "Out"},
	{"FlyingLayers.Appear.Fade", "Fade"},
	{"FlyingLayers.Appear.Zoom", "Zoom"},
	{"FlyingLayers.Appear.Slide", "Slide"},
	{"FlyingLayers.Appear.Wipe", "Wipe"},
	{"FlyingLayers.Appear.Cut", "Cut"},
	{"FlyingLayers.SlideEdge", "Edge"},
	{"FlyingLayers.Edge.Auto", "Nearest"},
	{"FlyingLayers.Edge.Left", "Left"},
	{"FlyingLayers.Edge.Right", "Right"},
	{"FlyingLayers.Edge.Top", "Top"},
	{"FlyingLayers.Edge.Bottom", "Bottom"},
	{"FlyingLayers.MotionFade", "Fade while moving"},
	{"FlyingLayers.MotionBlur", "Enabled"},
	{"FlyingLayers.ShutterAngle", "Shutter"},
};

const size_t kBuiltInStringCount = sizeof(kBuiltInStrings) / sizeof(kBuiltInStrings[0]);

const char *text(const char *key)
{
	const char *translated = obs_module_text(key);
	if (translated && std::strcmp(translated, key) != 0)
		return translated;

	for (const BuiltInString &s : kBuiltInStrings)
		if (std::strcmp(s.key, key) == 0)
			return s.text;
	return key;
}

} // namespace fl
