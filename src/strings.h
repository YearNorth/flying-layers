/*
FlyingLayers for OBS Studio
Copyright (C) 2026 YearNorth

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <cstddef>

namespace fl {

struct BuiltInString {
	const char *key;
	const char *text;
};

// English texts compiled into the plugin (same as data/locale/en-US.ini).
extern const BuiltInString kBuiltInStrings[];
extern const size_t kBuiltInStringCount;

// Text for `key` in the user's language, or the built-in English text when
// the locale files are missing.
const char *text(const char *key);

} // namespace fl
