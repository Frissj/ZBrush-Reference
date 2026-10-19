# ZBrush-Reference

**Reference Scaler**: a ZBrush 2022 ZScript plugin. It makes the reference image
loaded with **Texture > Image Plane** scale with the camera, so the image grows
and shrinks with the model as you zoom in and out.

## Install

1. In ZBrush 2022, go to **ZScript > Load** and pick `ReferenceScaler.txt`.
   ZBrush compiles it to `ReferenceScaler.zsc` next to the source.
2. Copy `ReferenceScaler.zsc` into `ZBrush 2022/ZStartup/ZPlugs64/` so it loads
   on every start.
3. The controls appear under **ZPlugin > Reference Scaler**.

## Use

1. Load your image with **Texture > Image Plane** and put the model in Edit mode.
2. Line the image up with the model.
3. Press **Anchor**. This sets the current zoom as 1:1.
4. Turn on **Live**. Each time you zoom, the Image Plane size slider is set to
   `anchored size × (current model scale ÷ anchored model scale)`, so the image
   keeps the same scale relative to the model.

| Control | What it does |
| --- | --- |
| Anchor | Records the model scale, its screen position and the Image Plane slider values. |
| Live | Keeps updating on a timer and each time the mouse is released (when a zoom drag ends). |
| Sync | Updates once. Give it a hotkey (Ctrl+Alt+click) if you'd rather sync by hand. |
| Reset | Puts the Image Plane sliders back to their anchored values. |
| Interval | Seconds between Live updates. |
| Follow Position + Offset Gain | Also moves the image with Image Plane's Shift H/V so it stays under the model when zooming moves the model on screen. Offset Gain is how many canvas pixels the image moves per slider unit. Adjust it by eye, and use a negative value if the image moves the wrong way. |
| Detect / Set Paths | Finds the Image Plane sliders, or lets you type their paths. |

## Debugging

ZBrush has no script console. This plugin gives you three ways to see what it's doing:

- **Status**: a popup listing everything the plugin sees. That's edit mode, the
  model scale/position from `TransformGet`, the anchor values, the Image Plane
  slider value and the detected slider paths.
- **Debug** switch: while it's on, each update writes a one-line summary to the
  bar at the top of the canvas. **Sync** always writes that line.
- **Dump UI**: writes every ZBrush interface item (ID, full path, value) to
  `ReferenceScaler_UI.txt` and remembers the values. The popup shows where the
  file was saved.
- **Dump Changes**: writes only the items whose value changed since the last
  dump to `ReferenceScaler_Changes.txt`, and lists the first 25 in the popup.
  For example: Dump UI, zoom, then Dump Changes shows what zooming changes.
- **Syntax errors**: ZBrush reports these in a popup with the line number
  when you load the `.txt` via **ZScript > Load**.

After you edit `ReferenceScaler.txt`, load it again with **ZScript > Load**. That
rebuilds the `.zsc`, and you then copy the new `.zsc` into `ZPlugs64`.

## Notes and limitations

- **Not yet tested in ZBrush.** It was written against the ZScript 2022 command
  reference.
- **Slider names:** the plugin looks for the Image Plane size slider under names
  such as `Texture:Image Plane:ImgSize` and `…:Size`, and for the shift sliders
  under `…:Shift H` and `…:Shift V`. If **Detect** reports "not found", use
  **Set Paths** and type the path shown in the slider's popup info (hold Ctrl
  while hovering over it).
- **Only one ZScript runs at a time.** Pressing a button in another plugin,
  including Image Plane's own buttons, ends the Live loop, so turn Live back
  on afterwards. If setting Image Plane's slider ends the loop on its own (Live
  stops after one update), use **Sync** on a hotkey instead.
- After you switch Image Plane views (Front/Side…) or load a new image, line it
  up again and press **Anchor**.
- In ZBrush 2022, zooming changes the model's scale on the canvas. The plugin
  reads that scale with `TransformGet`, so it doesn't track Document zoom (the
  2D canvas zoom).
