# JRef

A ZBrush 2023 plugin that keeps your reference images with the model. You load
one image per view (Front, Back, Right, Left, Top, Bottom), and JRef shows the
right one whenever you snap to that view. As you zoom, pan and turn, the image
stays lined up with the model.

You can show the references in two ways:

- **Classic** draws the image behind the model, like Image Plane.
- **PureRef** shows the images in a panel at the side of the canvas. With
  **Multiview** on, the panel shows two views at once, and **3D Auto Track**
  keeps the other view on the part of the model you're working on, at any
  angle and in perspective.

JRef draws in its own click-through window over ZBrush's canvas. You keep
sculpting as normal, and ZBrush's menus, gizmo and cursor stay on top.

## Install

JRef needs ZBrush 2023 on Windows 10 or 11. Nothing needs building.

1. Download `JRef-buildN.zip` from the
   [latest release](https://github.com/Frissj/ZBrush-Reference/releases/latest)
   and unzip it anywhere.
2. In ZBrush 2023, go to **ZScript > Load** and pick `JRef\JRef.txt`.
3. That's it. JRef copies itself into `ZStartup\ZPlugs64` (as `ZZ_JRef.zsc`,
   with its `JRef_Data` folder). From then on it loads every time ZBrush
   starts, under **ZPlugin > JRef**. You can delete the unzipped folder.

To update, load `JRef.txt` from a newer release the same way. If `ZPlugs64`
can't be written without admin rights, JRef keeps its log and settings in
`C:\Users\Public\Documents\JRef` instead.

## Getting started

1. Put the model in Edit mode and snap to a view, for example Front.
2. Open **ZPlugin > JRef > Views** and press **Load Front** to pick the image.
3. Press **Overlay**. JRef finds the canvas, and the image appears behind the
   model.
4. If the image needs lining up, press **Adjust Image**. Drag inside it to move
   it, and drag a corner or edge (or right-drag) to scale it. Click off the
   image or press Esc when you're done.
5. Load the other views the same way. Each one shows whenever you snap to its
   view.

## Controls

All controls are under **ZPlugin > JRef**.

| Control | What it does |
| --- | --- |
| Overlay | Turns the overlay on. Press it again if JRef loses the canvas. |
| Overlay Off | Hides the overlay. |
| Classic / PureRef | The display mode: the image behind the model, or in a panel at the side. |
| Auto Persp | Each time you change mode, puts ZBrush's **Persp** back as you last had it in that mode. At first it's on in PureRef and off in Classic. |
| Reset | Snaps to the Front view lined up with its image. Also resets Gamma, Saturation and every image's Adjust Image size and position. Asks first, since it can't be undone. |
| Swap Side | Moves the PureRef panel to the other side of the canvas. Only shown in PureRef. |
| Adjust Image | Moves and scales the current image to line it up with the model. Only shown in Classic. |
| Anchor | Records the current zoom and position as the 1:1 match with the image. |
| Views | **Load** and **Remove** each view's image. The trash can (Remove) shows only once a view has an image. |
| Image | **Gamma** and **Saturation** of the images; **Model Opacity** lets the reference show through the model in Classic. **Placement** holds the current image's **Height**, **Width**, **X**, **Y** and **Turn**. |
| Multiview | **Multiview** shows two views in the PureRef panel. **Zoom Out Scale PureRef** shows the panel's images that many times smaller than the model (default 2). **3D Auto Track** (on by default) keeps the other view on the part you're looking at, with or without **Persp**. |
| Upper Image / Lower Image | Which views Multiview shows, top and bottom. |
| Project | **Open Project** opens a saved JRef project. **Snapshot** saves what the overlay captured and drew, for reporting a problem. **About** shows the running build. |

## Projects

The first image you load starts a project: a `JRef` folder next to that image.
It holds copies of the images, each view's outline and your settings. You can
move the folder or copy it to another computer, then open it with
**Project > Open Project**.

JRef also watches ZBrush open and save `.zpr` files. When you save a ZBrush
project, JRef writes a `.jref` link file next to it. Opening that ZBrush
project again, on any computer, reopens its references. A ZBrush project with
no link opens with no references.

## How 3D Auto Track works

JRef reads ZBrush's camera (zoom, pan and rotation) straight from its memory.
Each time ZBrush finishes drawing a frame and the camera has been still for a
moment, JRef keeps the frame's depth as 3D points. It builds those up from
every angle you look from. To place the other view, it takes the surface under
the middle of your work area, from those points, and goes half the part's
thickness into the model. So the side view shows the same part of the model as
the main view, even when you've turned to an angle with no reference image.

## Other ZBrush versions

JRef has no addresses from one ZBrush build written into it, so it isn't tied
to one build. It finds everything in memory each time:

- the camera, by the values ZBrush's own TransformGet reports
- the rotation, as a matrix turned by the angle ZBrush reports
- ZBrush's canvas images, by working out their layout from the objects in
  memory

It has only been tested on ZBrush 2023. Another version could still store
rotations or depth differently, which only testing there would show.

## Building the DLL

Only needed if you change the source: the releases and the repo come with the
DLL built. The overlay, memory reading and project watching live in
`JRef_Data\JRef64.dll`, built from `dll\ReferenceScaler.cpp`. To build it you
need Visual Studio 2026 (x64 C++ tools):

```bash
cmd //c "dll\\build.bat"
```

That writes `JRef_Data\JRef64.dll` and `JRef_Data\build.txt`. The build number
is `kDllBuild` in the source, and `zrsBuild`/`zrsDllBuild` in `JRef.txt`. Bump
all three together. The plugin refuses a DLL whose build doesn't match. A
copy installed from this folder updates itself when ZBrush starts and finds a
newer build here.

## Troubleshooting

- JRef writes a log to `JRef_Data\nav_log.txt` next to the installed DLL. It
  records what it found in ZBrush's memory, each project it opened and the
  overlay's timings.
- **Project > About** shows which build is running and whether the DLL loaded.
- If the image doesn't follow the camera after a ZBrush restart, press
  **Overlay** again. JRef searches ZBrush's memory for the camera when the
  overlay starts. That takes about a second.

## License

See [LICENSE](LICENSE).
