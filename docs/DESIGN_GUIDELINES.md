# LCL Visual Design Guidelines

## Direction

Blend clear hierarchy with glassmorphism depth.

- Typography and spacing should stay calm, neutral, and practical.
- Surfaces should feel layered, not flat.
- Contrast should prioritize readability over colorful accents.

## Core Palette

Use neutral system tones as defaults with one restrained blue accent family.

- `bg/base`: `#000000`
- `bg/surface`: `#1C1C1E`
- `bg/elevated`: `rgba(44, 44, 46, 0.95)`
- `bg/glass-tint`: `rgba(28, 28, 30, 0.72)`
- `text/primary`: `#FFFFFF`
- `text/secondary`: `rgba(235, 235, 245, 0.60)`
- `separator`: `rgba(84, 84, 88, 0.50)`
- `accent`: `#0A84FF`
- `focus`: `rgba(100, 210, 255, 0.86)`

## Window Chrome

- Corner radius baseline: `20` logical units
- Header controls: circular (`roundness = 2.0`), neutral fill, subtle border
- No hard separator line between titlebar and content
- Use inset dual-border (dark outer + light inner) for depth

## Mobile System UI

### Application icon

- The canonical launcher icon is a `60 x 60` logical-unit square.
- System UI owns its continuous rounded-rectangle silhouette: corner radius is
  `16` logical units (`16 / 60`) and corner roundness is `3.2`.
- Application icon assets must be square and must not bake in a rounded mask.
  Launcher, placeholders, and transition proxies apply the same border-radius
  contract.
- During an icon-to-window morph in portrait, icon content is anchored to
  `top + left + right`; the final source pixel row extends through the
  unanchored bottom region.
- During the same morph in landscape, icon content is anchored to
  `top + left + bottom`; the final source pixel column extends through the
  unanchored right region.

### Application thumbnail

- A portrait thumbnail uses `top + left + right` anchoring. A landscape
  thumbnail uses `top + left + bottom` anchoring.
- Thumbnail content preserves its aspect ratio and does not extend its final
  pixel row or column.

### Gesture pill

- The gesture pill is a MobileWM-owned attached surface. The compositor knows
  only that it is an adornment child of a WindowGroup; it does not know or draw
  the pill visual.
- Its reference geometry is a `393`-unit-wide parent with a `134 x 5` pill and
  an `8`-unit bottom inset.
- Treat the pill as content inside a conceptual parent anchored to
  `left: 0`, `right: 0`, and `bottom: 0`.
- Responsive scaling is driven by the parent width. The resulting geometry
  preserves left, right, and bottom anchoring; the pill must not scale from its
  center or drift vertically.

## Glass Rules

- Glass is depth, not color.
- Keep tint dark-neutral for terminal and tooling surfaces.
- Prefer subtle blur/refraction with high text contrast.

## Control Rules

- Built-in controls resolve colors, borders, radii, and interaction values
  through `lcl-theme`; drawing code must not embed a second palette.
- Text fields use a restrained focus ring and an accent-colored caret.
- Toggle selection uses the accent family; inactive tracks and menu selection
  remain neutral.
- Application-specific colors remain explicit overrides and must not mutate the
  shared theme.

## Terminal Rules

- Terminal background must be dark neutral, not transparent black-only and not saturated.
- Terminal text defaults to neutral white/gray tones.
- Accent colors should be optional and sparse.

## Implementation Notes

- CSD and DesktopWM frames use the same renderer-independent
  `WindowChromeWidget` state and produce the same logical chrome `DisplayList`.
  Both are ordinary client-side recordings; the compositor does not link or
  execute window-chrome policy.
- Keep all radius/border tokens centralized where possible.
- Layout, hit testing, actions, interaction state, motion, and visual color
  resolution come from `lcl-window-chrome`; neither client host may reimplement
  them.
- Window motion and hit testing must share the same forward/inverse
  `WindowGroupTransform`. Device scale is applied only during raster replay.
