# LCL Motion

`lcl-motion` is the renderer- and UI-independent animation target shared by
`lcl-ui` and the compositor. It owns named/custom easing, physical springs,
scalar property channels, keyframe timelines, and the system motion tokens.
The compositor links `lcl-motion` directly and does not link `lcl-ui`.

## Implicit UI transactions

```cpp
app.animate(
    lcl::motion::Motion::spring(0.24f, 0.08f),
    {.layout = lcl::ui::LayoutMode::Reflow},
    [&] {
        panel->setWidth(420.0f);
        panel->setOpacity(1.0f);
    });
```

`Reflow` samples LCL layout values every frame, so painting and hit testing
share the presentation geometry. `Morph` freezes the pre-transaction raster,
computes and renders the final UI immediately, then crossfades the old pixels
into the new presentation while its layout transforms settle. Input for that
window is frozen until both the crossfade and geometry morph finish.

JavaScript uses the same transaction and scheduler:

```js
app.animate(
  {type: "spring", duration: 240, bounce: 0.08, layout: "reflow"},
  () => {
    panel.setWidth(420);
    panel.setOpacity(1);
  });
```

## Keyframes

Mounted widgets expose presentation-only keyframes. They return an
`AnimationHandle` with `play`, `pause`, `reverse`, `cancel`, `finish`, `seek`,
`progress`, completion, and `commitFinalStyles` controls. The default fill is
`none`; `forwards` retains the final presentation until cancellation or a new
animation.

```cpp
lcl::motion::AnimationOptions options;
options.durationSec = 0.2f;
options.fill = lcl::motion::FillMode::Forwards;
auto handle = panel->animate(
    lcl::ui::AnimatableProperty::Opacity,
    {{0.0f, 0.0f}, {1.0f, 1.0f}}, options);
```

```js
const handle = panel.animate(
  [{opacity: 0, offset: 0}, {opacity: 1, offset: 1}],
  {duration: 200, fill: "forwards", iterations: 2, direction: "alternate"});
```

`lcl::ui::AnimationEngine` and `LCL.AnimationEngine` remain compatibility
adapters over the shared channel engine.

## Compositor resize ownership

Protocol v26 pairs `ConfigureBounds` with a monotonic `configureSerial` and
`geometryGeneration`. Resize has one `AtomicRetained` policy: parent and every
size-changing attached/popup surface raster in parallel through central
`lcl-rasterd`. The previous complete WindowGroup remains byte-for-byte and
geometry-for-geometry unchanged until all matching layers are ready, then the
new snapshot is promoted in one display transaction. There is no stretch,
snapshot, crossfade, background reveal, timeout partial publish, or
parent-first acknowledgement barrier. A newer target discards the obsolete
generation; a slow app does not stall other compositor-owned motion.
