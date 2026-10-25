# Ramen Motion Design Concept

## Overview

A frontend menu designed around the RG Rotate's swivel screen. The UI treats the screen's pivot point as the hub of a physical system: a transparent glass tube in the shape of a quarter-arc, centered on the pivot, with each emulated system represented by a colored sphere riding inside it. Every major transition (boot, system select, game select, settings, launch, in-game menu) is expressed as motion of the spheres, the tube, a camera, or an industrial CRT monitor. The goal is one continuous physical story rather than a set of unrelated animations.

## Hardware context and conventions

- **Screen.** Square, side length `S`.
- **Pivot.** In the closed orientation, the pivot is the screen's top-right corner. Opening the screen is a 90° clockwise swing about the pivot, which places it at the bottom-right corner of the open, upright screen. All layouts in this document use the open orientation.
- **Closed position.** The screen is rotated 270° clockwise from its open orientation.
- **Backlight.** The display is assumed to turn on just as the screen reaches its open stop. The opening animation therefore begins at the end of the physical motion.
- **Coordinates.** Screen space with the origin at top-left and y pointing down. The pivot sits at `(S, S)`.
- **Arc angle `θ`.** Measured around the pivot. `θ = 0°` points left along the bottom edge (9 o'clock) and `θ = 90°` points up along the right edge (12 o'clock). Increasing `θ` is clockwise on screen.

## Core motif: the glass tube

- **Shape.** A transparent tube follows a circle of radius `r` centered on the pivot. In the system view, only the circle's upper-left quadrant is visible. It runs from `θ = 0°`, where it meets the bottom edge, to `θ = 90°`, where it meets the right edge. The other three-quarters of the circle are implied off-screen.
- **Radius.** `r ≈ 0.6–0.7 · S`. This keeps the spheres clear of the cramped pivot corner and leaves space on the far side of the screen for a title or system info.
- **Glass treatment.**
  - A thin specular highlight runs along the tube's curvature.
  - Spheres show slight refraction or blur where they pass under the tube's edges.
  - Without these cues the tube reads as a flat outline rather than glass.

## System carousel layout

Three systems spaced evenly around a full circle would sit 120° apart, which is wider than the 90° visible arc, so the neighbors could never be seen. The wheel is therefore a repeating carousel.

- **Slots.** 9 slots at 40° spacing, holding the three systems three times over: `A B C A B C A B C`.
- **Selected position.** `θ = 45°`, on the screen's diagonal through the pivot.
- **Neighbors.** At `θ = 5°` (at the bottom edge) and `θ = 85°` (at the right edge). Each sits right at a screen edge and is partially clipped, which gives the "peeking" look.
- **Selected styling.** The centered sphere is scaled to about 1.1–1.2× and the neighbors are slightly dimmed.
- **Sphere material.** Each sphere is glossy and liquid-like: a tinted, semi-translucent body with a bright specular highlight near the top and soft internal depth, so text inside it can read as submerged.

### Label surfacing

Labels do not ride the wheel while it turns. They appear only once the wheel has settled, and they surface like an object rising through the sphere's liquid to its top.

- **Reveal from the center.** The label's visibility opens outward from the horizontal center of the text, via an alpha mask that widens to both sides. The middle letters appear first and the ends last.
- **Rise.** At the same time, the label drifts upward a few pixels with an ease-out and comes to rest just below the specular highlight.
- **Sharpening.** The label starts slightly blurred and refracted, then sharpens to full clarity as it reaches its resting position, so it reads as breaking through to the clearest part of the liquid.
- **Leaving.** When the wheel starts moving again (D-pad press or selection), the label reverses quickly: it sinks, blurs, and closes toward its center. It resurfaces on the next settle.
- **Timing.** About 200–300 ms to surface and about 100–150 ms to sink, so navigation never waits on the labels.

## Sequence 1: Opening (boot spin)

The screen lights up as it lands. The wheel should look as if it has been spinning the whole time and the screen simply caught it mid-motion.

1. **First frame.** The wheel is already spinning clockwise, the same direction as the screen's swing, so it reads as the hardware's momentum carried into the UI. The animation never starts from rest.
2. **Fast phase.** Each sphere is motion-blurred into a smear along the arc. The three colors blend into a continuous rainbow ring.
3. **Spin-down.** The wheel decelerates under exponential friction, and the ring resolves back into distinct spheres as it slows.
4. **Ticks.** A soft tick plays each time a sphere passes a slot boundary, like a prize-wheel peg. The ticks space out naturally as the wheel slows.
5. **Detent settle.** Once the wheel is within about one slot of its target, control passes to an underdamped spring with a damping ratio of about 0.6–0.8, which settles the wheel into the detent. Ticks stop when the spring takes over.
6. **Target.** The wheel lands on the last-played system, so the spin-down feels purposeful rather than random.

**Constraints:**

- **Duration.** The whole sequence runs about 0.8–1.2 s.
- **Skipping.** Any input snaps the wheel straight to its settled state.
- **Backlight lag.** If the backlight takes time to come up, keep the first ~100 ms of motion low-information (blur, low contrast) so nothing important plays on a dark panel.

## Sequence 2: System navigation

- **Movement.** Each D-pad press rotates the wheel one slot (40°), using the same detent spring as the boot settle. The boot animation and the menu feel like one mechanism.
- **Ticks.** A single tick plays per step, matching the boot spin.

## Sequence 3: System selection and camera chase

1. **Confirm jiggle.** The selected sphere does a quick squash-and-stretch along the tube's tangent, about 150–250 ms. It should read as "pressed," not "rattled," so there is no sideways shake.
2. **Clear-out.** At the same moment as the jiggle, the two neighbors retract along the tube and exit through the screen edges they were peeking from: the 5° sphere past the bottom edge, the 85° sphere past the right edge. The confirmation and the clear-out read as a single beat.
3. **Anticipation.** The selected sphere pulls back a few degrees (counterclockwise) before launching.
4. **Launch.** The sphere accelerates clockwise along the tube toward the right-edge end and exits past `θ = 90°`, into the circle's upper-right quadrant.
   - This continues the direction of both the physical swing and the boot spin, so the whole forward path flows one way around the pivot. Only back navigation reverses it.
5. **Camera chase.** The camera pans right by one screen width (`+S` in x). It is driven by a spring that trails the sphere, so the sphere briefly leads past the screen edge before the view catches up. That lag is what reads as "chasing."
6. **Zoom-out.** A modest zoom-out, to a scale of about 0.8–0.9, begins partway through the pan. It finishes on the same frame the sphere settles, so the pan and zoom land together.

## Game view

### Geometry

- **The new view.** After the pan, the pivot sits at the new view's bottom-left corner. The visible stretch of tube is the circle's upper-right quadrant, running from 12 o'clock (where it meets the left edge) to 3 o'clock (where it meets the bottom edge).
- **Mirror layout.** This view is a horizontal mirror of the system view.
- **Parking spot.** The system sphere parks on this quadrant's diagonal (about 1:30), mirroring the selected position in the system view.
- **Monitor placement.** The tube occupies the view's lower-left region, and the zoom-out frees the upper and right portions of the view for the metadata monitor.

### Game cycling

- **Capsules.** Games are shown as smaller capsules or pellets sliding through the tube, tinted with the selected system's color.
- **Navigation.** Games use the same detent spring as the system carousel, with tighter slot spacing (on the order of 15°) to fit more entries.

### Industrial monitor

- **Hardware look.**
  - A chunky bezel with rivets or hex screws.
  - A color CRT tube with a dark green background behind the UI. Color is needed because the monitor shows box art and gameplay video.
  - Faint scanlines and slight barrel distortion.
- **Power-on.** As the camera arrives, the monitor flickers on with a CRT-style power-on: a bright horizontal line expands vertically into the full picture. This echoes the device's own screen waking at the start.
- **Contents.** Metadata for the highlighted game: title, year, play time, and similar details, plus box art and/or a gameplay video preview when either is provided. Without them, the dark green background and the text carry the screen on their own.

## Sequence 4: Game launch

1. **Zoom in.** The camera zooms into the monitor's glass until the bezel leaves the frame. Scanlines scale up with the zoom, creating the sense of "falling into" the screen.
2. **Fade.** The picture fades to black, and audio fades out with it.
3. **Hold.** Black is held until the emulator signals that it has produced its first frame. A fixed timer is not used: core startup times vary between systems, and a timer risks flashing the frontend or a garbage frame.
4. **Hand-off.** The game appears.

**Exiting a game** reverses this sequence: fade in already zoomed into the glass, then pull back out to the full monitor in the game view.

## Sequence 5: In-game menu

- **Background.** The paused game frame is shown behind a green-tinted panel that matches the monitor's dark green background, with a faint scanline overlay, as if the game were being viewed on the monitor.
- **Open.** The menu opens with the CRT power-on effect: a horizontal line expanding vertically.
- **Close.** The menu closes with the classic power-off collapse: the picture squeezes to a horizontal line, then to a dot.
- **Readability.** The paused frame is dimmed and desaturated *before* the green tint is applied. Otherwise bright or busy frames can wash out green text, and contrast would vary heavily from game to game.

## Sequence 6: Emulator settings (degauss)

Opening the selected system's emulator settings triggers a degauss on the monitor.

- **Reference.** A real degauss is an audible "thunk" followed by a decaying hum, while the picture wobbles and warps and the wobble dies away over one to two seconds.
- **Compressed timing.** The effect is compressed to about 400–600 ms with an exponential decay, so most of the wobble happens in the first third.
- **Settings content.** The settings render only after the wobble settles, so the degauss reads as the monitor switching modes rather than as decoration.
- **Color purity swirl.** Because the monitor is a color CRT, the rainbow color blotches associated with degaussing are physically accurate: they come from the electron beams landing slightly off the shadow mask while it is magnetized. The swirl should be most visible over the dark green background and any box art or video on screen, and should decay along with the geometry wobble.

## Back navigation

Back replays the forward path in reverse:

1. The system sphere rides back counterclockwise through the tube, from the upper-right quadrant into the upper-left.
2. The camera pans left and zooms back in.
3. The neighbor spheres slide back in from the edges they exited through.

The pivot remains the fixed reference point throughout.

## Timing and tuning summary

| Element | Value |
|---|---|
| Boot spin + settle | 0.8–1.2 s, skippable on any input |
| Low-information lead-in (backlight) | ~100 ms |
| Detent spring damping ratio | ~0.6–0.8 (underdamped) |
| System slot spacing | 40° (9 slots), neighbors partially clipped |
| Selected sphere scale | 1.1–1.2× |
| Label surface / sink | ~200–300 ms / ~100–150 ms |
| Confirm jiggle | 150–250 ms |
| Camera pan | +S in x, spring trailing the sphere |
| Game view zoom | ~0.8–0.9× scale |
| Game slot spacing | ~15° |
| Degauss | 400–600 ms, exponential decay |
| Launch fade | Held until the core's first frame is ready |

## Implementation notes

- **Hinge sensing.** If the hinge reports only open or closed rather than a continuous angle, the boot spin plays as a timed animation triggered when the state changes. It is designed to start at the end of the swing either way, so it does not depend on live angle tracking.
- **One spring.** A single detent spring model drives the system carousel, the game carousel, and the boot settle. This keeps the feel consistent and the code small.
- **Motion blur.** The blur during the boot spin can be approximated by stretching each sphere's sprite along the arc tangent in proportion to angular velocity. True accumulation blur is not required.

## Resolved decisions

- **Monitor type.** A color CRT with a dark green background, so it can display box art and gameplay video. The degauss uses a physically accurate rainbow color purity swirl.
- **Neighbor clipping.** Partially clipped neighbors, using 40° slots (neighbors at 5° and 85°).
- **Label handling.** Labels appear only after the wheel settles, revealing outward from the text's center while rising through the sphere's liquid to rest just below its highlight.
