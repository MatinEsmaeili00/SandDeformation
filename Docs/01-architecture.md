# 1. Architecture

[← Docs index](README.md) · Next: [Sand physics →](02-sand-physics.md)

## The scrolling region

Storing sand depth for a whole level means a texture covering the whole level,
which is enormous and almost entirely empty. Instead: keep one modest texture
(1024² by default) covering a square of world space `RegionSizeWorld` units
across, centred on the player. Sand outside it isn't simulated.

The trick is what happens when the player moves. If the texture moved with
them, every mark in it would slide along, glued to the screen. So each frame,
before anything else, the shader **re-samples last frame's texture at an
offset equal to how far the player moved**:

```hlsl
const float2 PrevUV = (LocalXY + RegionOffsetFromPrev) / RegionSize + 0.5f;
```

Derived step by step in
[doc 3](03-shader-walkthrough.md#reprojection-and-why-every-neighbour-goes-through-it).
The cost is fixed regardless of level size; the only thing given up is sand
memory beyond `RegionSizeWorld / 2` from the player.

Everything inside the shader works in **region-local** coordinates rather than
absolute world ones. A level built 500,000 units from the origin would push
`float` into a range where consecutive representable values are over a
centimetre apart, and detail would visibly quantise.

## The state texture

One RGBA32F texture, ping-ponged between frames:

| Channel | Meaning | Why it's here |
|---|---|---|
| `R` | `Height` — persistent displacement, world units, signed | The surface itself. Survives indefinitely. |
| `G` | `RippleHeight` — wave displacement, world units | Kept separate from `Height` so waves can ring out and vanish without eroding the marks underneath. |
| `B` | `RippleVelocity` — world units/second | The wave equation is second-order in time; storing velocity turns it into two first-order steps and avoids needing *two* previous frames. |
| `A` | `Disturbance` — 0..1, decays | A cheap "this was recently touched" signal for the material (darker damp sand, dust) and for gameplay. |

Separating static height from ripple height is the important call. Adding
ripples straight into `Height` would work for one frame and then be wrong:
waves would leave permanent scars, and the slump solver would fight the wave
solver over the same number. Two layers summed at the end
([`TotalHeight`](03-shader-walkthrough.md#pass-2-sandnormalscs)) keeps each
solver's assumptions intact.

### Why full float

`RTF_RGBA32f`, not 16f. Ripple velocity is integrated across frames, so half
precision accumulates error into visible drift within seconds. The *output*
texture is 16f — it's consumed once per frame and never fed back.

## Why sand can't go idle

The snow version of this plugin stops dispatching the moment nothing is
touching the field and the region is stationary. Sand can't: after you stop
touching it, ripples are still ringing out and over-steep slopes are still
collapsing. Stopping would freeze a wave mid-travel.

So the subsystem keeps a settle window instead, sized from the physics:

```cpp
const float RippleSettle = 4.6f / FMath::Max(RippleDamping, 0.1f);
SettleTimeRemaining = FMath::Max(RippleSettle, 2.0f);
```

Wave amplitude decays as `exp(-damping · t)`, so reaching ~1% takes
`ln(100)/damping ≈ 4.6/damping` seconds. The 2-second floor covers the slump
solver finishing. Derivation in
[doc 2](02-sand-physics.md#how-long-do-ripples-take-to-die).

A **moving** region still always dispatches, idle or not — the texture is
world-anchored, so a move must be reprojected or every mark slides with the
player. That's the condition easiest to get wrong.

## The pieces

```
USandDeformerComponent (per character)              ── game thread
        │  footfalls: which feet are planted right now (pulled)
        │  impacts:   landing / jump take-off (event-driven)
        ▼
USandDeformationSubsystem::Tick()                   ── game thread
        │  region follows focus actor, gathers contacts,
        │  converts to region-local, hands to the render thread
        ▼
SandDeformation::Dispatch_RenderThread()            ── render thread
        │  RDG: register textures, upload deformers, two passes
        ▼
SandDeformation.usf                                 ── GPU
        │  Pass 1 SandSimulateCS : reproject → slump → ripple → stamp
        │  Pass 2 SandNormalsCS  : static+ripple → normals → output
        ▼
SandData render target → your material
```

## Why each piece is the kind of object it is

**`USandDeformationSubsystem` is a `UTickableWorldSubsystem`.** The simulation
is per-world (PIE must not share state with the editor world), needs to tick,
and should exist without anyone placing an actor. An actor would have to be
placed in every level; a `GameInstanceSubsystem` would wrongly share one
surface across level loads.

**`USandDeformerComponent` does not tick.** The subsystem *pulls* from every
registered component during its own tick. That gives one deterministic point
in the frame where all contacts are gathered, so they all land in the same
dispatch against the same region centre. If components pushed on their own
schedule, some contacts would be rebased against a stale centre and land
offset — which is exactly the bug
[`StampSandAt` is written to avoid](04-cpp-reference.md#banking-world-space).

**Except impacts, which are events.** A landing can begin and end between two
polls, so it can't be sampled reliably; `ACharacter` already detects it
exactly. Those arrive on a delegate and are banked until the next pull. See
[doc 5](05-impacts-and-particles.md).

**`USandDeformationSettings` is a `UDeveloperSettings`.** A world subsystem
has **no details panel anywhere in the editor**, so `EditAnywhere` on it is
unreachable — you literally cannot tune it. This registers a Project Settings
page instead.

**Registrations are weak pointers.** A character can be destroyed without
`EndPlay` running cleanly in every path; pruning stale entries during
iteration means a destroyed actor can never leave a dangling pointer.

## What this design does not do

- One region, one focus actor per world.
- No replication — sand is cosmetic, so each client simulates its own.
- No persistence across level load.
- It produces a height field; it does not create geometry. See
  [doc 6](06-material-wiring.md#world-position-offset-needs-geometry).

---

Next: [Sand physics →](02-sand-physics.md)
