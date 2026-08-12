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

`Reflow` samples Yoga-facing values every frame, so painting and hit testing
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

Protocol v4 pairs `ConfigureBounds` and `AttachBuffer` with a monotonic
`configureSerial`. Maximize/restore retains the old mapped client buffer while
the shared spring animates one window-group rect for client and chrome. A
matching resized buffer crossfades in; stale serials are closed and rejected.
If no matching buffer arrives within 750 ms, geometry and state roll back and
a fresh rollback configure is issued.
