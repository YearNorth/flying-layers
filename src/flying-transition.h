/*
Flying Layers for OBS Studio
Copyright (C) 2026 YearNorth

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

// Registers the "Flying Layers" scene transition: layers shown in both
// scenes move and resize from their place in the first scene to their place
// in the second; the others come in or go out (fade, zoom, slide or cut).
void flying_layers_register_transition();
