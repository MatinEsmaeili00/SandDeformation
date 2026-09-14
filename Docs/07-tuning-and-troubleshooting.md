# 7. Tuning & troubleshooting

[← Material wiring](06-material-wiring.md) · [Docs index](README.md)

## Where settings live

- **Simulation-wide** → *Project Settings → Plugins → Sand Deformation*, also
  writable at runtime on the subsystem.
- **Per-character** → the **Sand Deformer** component's details panel.

## Field

| Setting | Default | What it really does |
|---|---|---|
| `TextureResolution` | 1024² | Texel density, with `RegionSizeWorld`. **Ignored when a render target asset is assigned** — that asset's size wins. |
| `RegionSizeWorld` | 4096 | World units covered. See below. |

At the defaults a texel is 4 cm, so a footprint spans ~11 texels — visible,
not sharp.

| Region | Texels per footprint | Marks persist within |
|---|---|---|
| 4096 | ~11 | 20 m of the player |
| 2048 | ~22 | 10 m |
| 1024 | ~44 | 5 m |

**Shrink the region before you raise the resolution** for detail: halving it
doubles sharpness for free, where doubling resolution costs 4× memory and
bandwidth for the same gain. Smaller regions also reduce reprojection blur.

**But smaller texels lower the ripple speed cap.** The CFL limit is
`0.7 × texel / dt`, so halving the region halves how fast waves can travel.
If rings matter more than footprint sharpness, keep the region large, or raise
`RippleSubSteps` to buy the ceiling back
([why](02-sand-physics.md#the-cfl-condition-is-also-why-ripples-need-sub-stepping)).

## Sand behaviour

| Setting | Default | What it really does |
|---|---|---|
| `AngleOfReposeDegrees` | 34 | Steepest slope sand holds. **The character knob.** ~25 = loose, almost fluid; ~34 = dry sand; 45+ = damp sand or soil that holds walls. |
| `SlumpRate` | 6 | How fast excess slope collapses. Low = you watch walls sag for a second; high = collapses almost instantly. |
| `HeightRestoreRate` | 0 | World units/second drifting back to flat — wind. 0 = marks last forever. Try 0.5–2 for a windy desert. |

## Ripples

| Setting | Default | What it really does |
|---|---|---|
| `RippleSpeed` | 450 | Wave speed, world units/s. Clamped to the CFL limit, so a huge value just means "as fast as this grid allows" ([why](02-sand-physics.md#stability-the-cfl-condition)). |
| `RippleSubSteps` | 4 | Simulation iterations per frame. **The setting that decides whether ripples travel at all** - at 1 a ring dies within a metre no matter what RippleSpeed says ([why](02-sand-physics.md#the-cfl-condition-is-also-why-ripples-need-sub-stepping)). Costs N dispatches. |
| `RippleDamping` | 0.6 | Energy lost per second. Low = rings cross the whole region; high = they die at the impact. Also sets [how long the sim keeps running](01-architecture.md#why-sand-cant-go-idle). |
| `RippleImpulseScale` | 2.5 | Multiplies every impulse, from every source. Changes the energy actually injected, so it changes how far rings travel as well as how tall they are. |
| `RippleVisualScale` | 3.0 | Exaggerates the ripple layer **in the output texture only**. Never fed back into state, so it cannot destabilise the solver at any value. |
| `DisturbanceDecay` | 0.35 | How fast the 0..1 disturbed mask fades. |

### Making ripples read stronger or weaker

Reach for **`RippleVisualScale` first**. It is applied in the normals pass on
the way out and never re-enters the simulation, so it is safe at any value —
and because the normals are derived from the scaled height, it deepens the
shading as well as the displacement. Raising the impulse to get the same
result risks the solver; raising this cannot.

**The two scales multiply.** `RippleImpulseScale` sets how much goes in,
`RippleVisualScale` how hard what comes out is pushed, and the visible
amplitude is roughly their product. Halving both quarters the effect — worth
remembering when a change lands far harder than expected. If ripples are
overwhelming, halving both is usually a better first move than zeroing one,
since it preserves the shape of the effect while scaling it down.

Leave `RippleSpeed` and `RippleSubSteps` alone when tuning *strength*. They
govern how far and fast rings travel, not how tall they are; turning them down
to calm things makes ripples die at your feet instead, which reads as a broken
effect rather than a subtle one.

## Shading

| Setting | Default | |
|---|---|---|
| `NormalStrength` | 3.5 | Scales XY of the normal. Pure look control; doesn't touch the simulation. |
| `HeightScale` | 1.0 | Multiplier the material applies to height. Exaggerate displacement without changing physics. |

## Per-character

| Setting | Default | |
|---|---|---|
| `ContactHeight` | 20 | How close a foot socket must be to the ground to count as planted. **See below.** |
| `TraceDownDistance` | 60 | How far below the socket to look for ground. |
| `FootprintRadius` / `Depth` | 22 / 8 | Footfall size, world units. |
| `FootprintRimHeight` / `RimWidth` | 2.5 / 10 | Sand pushed up around a footfall. |
| `FootprintRippleImpulse` | 90 | Ripples from walking. Fires on the frame a foot first lands, not every frame it stays down. |
| `MinSpeedForFullStrength` | 80 | Speed for a full-strength footfall; never fades below 0.35. |
| `MinLandingSpeed` / `MaxLandingSpeed` | 150 / 1200 | Fall speed ramp for impact strength. |
| `ImpactRadius` / `Depth` | 70 / 30 | Landing crater. |
| `ImpactRippleImpulse` | 1200 | **The ring that spreads out.** Raise this first if impacts feel weak. |
| `JumpImpactScale` | 0.45 | Take-off puff size relative to a full landing. |

---

# Troubleshooting

## Nothing prints on flat ground

**Only get marks stepping off a slope, or landing from a jump.**

`ContactHeight` is too low. The test is:

```cpp
SocketLoc.Z - Hit.Location.Z <= ContactHeight
```

Foot **bones** sit at the ankle, typically 10–15 units above the sole, so a
threshold below that means the foot is never considered planted on flat
ground. The default here is 20 for exactly this reason — but if you're using a
different skeleton, raise it until footfalls register.

## Nothing happens at all

1. Is there a **Sand Deformer** component on the character?
2. Does the log show `Sand surface allocated at 1024x1024`? If not the
   subsystem never ticked — check you're in Game/PIE, not an editor preview
   world.
3. Do `FootSocketNames` match your skeleton's bone names?
4. Does the ground block `TraceChannel` (`WorldStatic` by default)?

## The field explodes / everything turns white or black

A feedback simulation that has diverged — almost always NaN spreading through
the state texture. The shader clamps both the timestep and the wave speed
specifically to prevent this, so if it still happens, check:

- `RippleDamping` set negative (would add energy every frame).
- `SlumpRate` negative.
- `AngleOfReposeDegrees` at or near 0, making `MaxDiff` zero so *every* slope
  is over the limit.

Once the state contains NaN it never recovers — NaN propagates through every
subsequent frame. Restart PIE after fixing the setting.

## Ripples look square, or spread faster diagonally

Inherent to a 5-point Laplacian on a square grid: it's slightly anisotropic,
so waves travel marginally faster along the axes. Barely visible at normal
damping. A 9-point stencil including diagonals reduces it, at extra cost.

Raising `RippleDamping` hides it by killing waves before they travel far
enough for the anisotropy to accumulate.

## Craters fill themselves in immediately

`SlumpRate` too high, or `AngleOfReposeDegrees` too low. The crater walls are
steeper than the angle allows, so the solver collapses them — working
correctly, just tuned aggressively. Raise the angle toward 40+, or lower
`SlumpRate` to watch them sag more slowly.

## Rims never appear

Same cause. The rim is piled sand, and if the angle of repose is low it can't
support the pile and flattens within a frame or two. Raise
`AngleOfReposeDegrees`, or lower `ImpactRimWidth` so the rim is steeper-sided
relative to its height... which the solver will then collapse. Rim height and
angle of repose fight each other by design; that's the physics.

## Impacts dig a hole but there's no visible ring

`ImpactRippleImpulse` too low, or `RippleDamping` too high. The ring is the
part that reads in motion.

If the ring is there but dies almost immediately, the cause is `RippleSubSteps`
rather than the impulse - at 1 the CFL cap confines a wave to about a metre no
matter how hard you hit it.

Also check the material is actually using the height channel; the ring is a
height change, so a material that only reads normals will show it weakly.

## Shading looks wrong / lit from the wrong side

***Tangent Space Normal* is still ticked.** The shader outputs **world-space**
normals — [detail](06-material-wiring.md#the-normal-gotcha).

## Marks slide along with the player

Reprojection offset wrong, or the dispatch skipped on a frame where the region
moved. A moving region must **always** dispatch —
[detail](04-cpp-reference.md#the-settle-window).

## Ripples look fine standing still, then wash out as soon as you walk

The region centre isn't snapped to the texel grid. Reprojection is then a
bilinear tap at a fractional offset — a blur applied to the whole field every
frame you move. Footprints survive it because they're broad and constantly
re-stamped; ripples don't, because they are precisely the high-frequency
content a tent kernel removes.

The "fine standing still" half of the symptom is the diagnostic: a stationary
region samples exactly at texel centres, so the filter is a no-op and the
solver looks perfect. If you only ever test from a standstill, this bug is
invisible.
[Derivation and fix](03-shader-walkthrough.md#why-the-region-centre-is-snapped-to-whole-texels).

Turning `RippleImpulseScale` or `RippleVisualScale` up will *not* rescue this
— the blur is proportional, so a stronger ripple simply blurs from a larger
starting amplitude at the same rate.

## The surface renders grey/default and shows nothing at all

Before suspecting the simulation, check whether the **material itself
compiled**. A material that fails to compile is silently replaced by the
Default Material, which samples none of your data — so the sim can be running
perfectly and you will see a flat surface regardless.

Look for this in the log at load:

```
LogMaterial: Warning: [AssetLog] ...M_YourSand.uasset: Failed to compile Material
for platform PCD3D_SM6, Default Material will be used in game.
```

The Stats panel in the Material Editor shows the same error with the offending
node. A common one when wiring the packed output texture is a `ComponentMask`
asking for `A` fed from a `TextureSample`'s **RGB** pin — three channels in, a
fourth requested. Wire it from the `RGBA` pin, or take the `A` pin directly.

This failure mode is worth ruling out first because no amount of tuning
touches it, and every simulation knob appears to do nothing.

## Sand is visibly disappearing or accumulating over time

The slump solver is conservative by construction
([proof](02-sand-physics.md#why-this-conserves-sand)), so this shouldn't
happen from slumping. Look at `HeightRestoreRate`, which deliberately removes
material, or at the region edge, where sand that scrolls out is simply gone —
that's the fundamental limit of a bounded region.

## A shader parameter reads as zero

Name mismatch between `BEGIN_SHADER_PARAMETER_STRUCT` and the HLSL global.
There is no error for this — check spelling first, always. Also check you used
`FVector2f`/`FIntPoint`, not `FVector2D`, which is double-precision in UE5.

## Editing the .usf changes nothing

Shaders are cached. `r.ShaderDevelopmentMode=1` plus `recompileshaders
changed`, or restart the editor.

**UnrealBuildTool does not validate HLSL** — a clean C++ build says nothing
about your shader. To check without opening the editor:

```
UnrealEditor-Cmd.exe <project>.uproject -nosplash -unattended -nopause -AbsLog=<path>
```

then grep for `FSandSimulateCS` / `FSandNormalsCS` and `Failed to compile`.

---

[← Back to the docs index](README.md)
