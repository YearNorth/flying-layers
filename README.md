# Flying Layers for OBS Studio

[Italiano](README.it.md)

A scene transition for OBS Studio that makes layers fly from one scene to the next:

- **layers in both scenes** move and resize from where they are in the first scene to where they are in the second, with position, scale, bounding box, rotation and crop all animated;
- **layers that leave** (only in the first scene) go out with a fade, zoom, slide to an edge, wipe or cut;
- **layers that arrive** (only in the second scene) come in with the same effects;
- audio crossfades as with the standard Fade transition.

The animation follows the OBS video clock frame by frame, so it is smooth at any frame rate. Each source is drawn once per frame, and the scenes themselves are never modified.

Requires OBS Studio 31.1 or later on macOS (Apple Silicon and Intel) or Windows (64-bit).

## Download

Get the latest version from [Releases](https://github.com/YearNorth/flying-layers/releases/latest):

| System | File |
| --- | --- |
| Windows | `flying-layers-<version>-windows-x64.zip` |
| macOS | `flying-layers-<version>-macos-universal.pkg` (installer) or `.tar.xz` (plugin only) |

## Install

**Windows.** Close OBS, then extract the zip into `C:\ProgramData\obs-studio\plugins\`, so that `C:\ProgramData\obs-studio\plugins\flying-layers\bin\64bit\flying-layers.dll` exists.

**macOS.** Close OBS, then open the `.pkg` and follow the installer. Alternatively, extract the `.tar.xz` and copy `flying-layers.plugin` into `~/Library/Application Support/obs-studio/plugins/`.

The macOS package is not notarized. If macOS blocks it, open **System Settings → Privacy & Security** and choose **Open Anyway**. For the plugin-only version, run `xattr -cr` on `flying-layers.plugin`.

## Usage

In the **Scene Transitions** dock, click **+** and choose **Flying Layers**. Set the duration as for any other transition, and open its **Properties** (gear icon) for the options:

| Group | Options |
| --- | --- |
| Motion curve | Linear, ease in, ease out, ease in-out, smooth, back (starts from rest, overshoots and settles), back in-out, elastic, or a custom Bézier curve (as CSS `cubic-bezier`) |
| Timing | **All together** or **in sequence** (layers go out, then fly, then come in). Share of the duration for layers going out and coming in. **Cascade**: the layers start one after another, in stacking order or by position on screen |
| Coming in and going out | Fade, zoom, slide, wipe or cut, chosen separately for entries and exits. Edge for slides and wipes (nearest, left, right, top, bottom). Optional fade during zoom and slide |
| Motion blur | Blurs layers along their movement, like a camera shutter. The shutter angle sets the amount (180° is the film look, 360° the full frame). Only real movement is blurred: wipes and crops stay sharp, and a still T-bar gives a sharp picture |

It works wherever OBS transitions work: direct scene switching, Studio Mode with the T-bar, hotkeys, Stream Deck and obs-websocket.

How layers are matched:

- two layers are the same layer when they show the same source. If a source appears more than once in a scene, the first copy matches the first, the second matches the second, and so on;
- while flying, layers use the stacking order of the destination scene. Leaving layers stay just above the layer that was below them;
- a source that is not a scene (a camera, an NDI or capture source, a source sent by another plugin) counts as a scene with one layer, placed as OBS shows it. A full-frame camera can fly into a picture-in-picture and back; two different single sources go out and come in with the chosen effects;
- groups and nested scenes fly as a single block;
- hidden layers do not take part.

## Build from source

Every push builds the plugin for macOS and Windows with GitHub Actions. Pushing a version tag (for example `1.0.0`) creates a draft release with the packages attached.

To build locally you need the [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) toolchain: Xcode 16 or later on macOS, Visual Studio 2022 on Windows. Dependencies are downloaded automatically.

```bash
cmake --preset macos
cmake --build --preset macos
```

```bash
cmake --preset windows-x64
cmake --build --preset windows-x64
```

## License

GPL-2.0-or-later, see [LICENSE](LICENSE). The scene-item geometry in `src/layout.cpp` and the vertex shader in `data/flying-layers.effect` follow OBS Studio code, which is under the same license.
