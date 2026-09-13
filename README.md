# Sand Deformation

An Unreal Engine 5 plugin that simulates a persistent, world-anchored sand
surface on the GPU. Sand is not snow: it **slumps** when you pile it too
steeply, it **ripples** when something hits it, and it **throws grains** when
you land in it. All three are in here.

Two compute shader passes run every frame; a game-thread subsystem drives
them, an actor component turns footfalls and landings into GPU input, and a
material samples the result.

Developed against **UE 5.8**. Companion to the
[Snow Deformation](https://github.com/MatinEsmaeili00/SnowDeformation) plugin,
which shares the world-anchored-region technique but none of the physics.

## Documentation

This README covers using the plugin. To understand how it works — or rebuild
it yourself — [**`Docs/`**](Docs/README.md) explains every part and the
reasoning behind it.

| # | Document | |
|---|---|---|
| 1 | [Architecture](Docs/01-architecture.md) | The scrolling region, the four-channel state texture, and why sand can't go idle the way snow can |
| 2 | [**Sand physics**](Docs/02-sand-physics.md) | Angle of repose and the wave equation, derived — the heart of this plugin |
| 3 | [Shader walkthrough](Docs/03-shader-walkthrough.md) | `SandDeformation.usf` line by line |
| 4 | [C++ reference](Docs/04-cpp-reference.md) | Every class, and why it's that kind of object |
| 5 | [Impacts & particles](Docs/05-impacts-and-particles.md) | Landing/jump detection, the ripple kick, Niagara bursts |
| 6 | [Material wiring](Docs/06-material-wiring.md) | Getting GPU output into a material |
| 7 | [Tuning & troubleshooting](Docs/07-tuning-and-troubleshooting.md) | Every knob, and symptom → cause |

## What makes sand different

| | Snow | Sand |
|---|---|---|
| **Slopes** | Holds any shape you press into it | Collapses past its *angle of repose* (~34°) |
| **Waves** | None | Ripples propagate outward from impacts |
| **Rim** | Only written on undisturbed ground, or it erases prints | Written freely — slumping is what keeps it physical |
| **Height** | Normalised 0..1, positive means *pressed down* | Signed **world units**, positive means *piled up* |
| **Idle** | Free the moment nothing touches it | Must keep simulating while ripples ring out |

The height convention flip matters: "material flows downhill" is only simple
to write when up is positive, and the slump solver compares real slopes
against a real angle, so the numbers have to be real world units.

## How it works

```
USandDeformerComponent (per character)
        │  footfalls: traces each foot socket to ground
        │  impacts:   landing (LandedDelegate) and jump take-off
        ▼
USandDeformationSubsystem::Tick()   (game thread, once per world per frame)
        │  1. moves the simulated region to follow the focus actor
        │  2. collects contacts + impacts from every registered component
        │  3. converts them to FSandDeformerGPU (region-local space)
        │  4. ENQUEUE_RENDER_COMMAND → SandDeformation::Dispatch_RenderThread
        │  5. pushes SandRegion to the material layer
        ▼
SandDeformation.usf (render thread, GPU)
        │  Pass 1  SandSimulateCS:
        │            reproject last frame into this frame's region
        │            slump  - collapse slopes past the angle of repose
        │            ripple - integrate a 2D wave equation
        │            stamp  - press craters, pile rims, kick waves
        │  Pass 2  SandNormalsCS:
        │            static + ripple height → normals → output texture
        ▼
SandData render target → your sand material
```

### The state texture

Simulation state is one RGBA32F texture, ping-ponged between frames:

| Channel | Meaning |
|---|---|
| `R` | `Height` — persistent surface displacement, world units, signed |
| `G` | `RippleHeight` — transient wave displacement, world units |
| `B` | `RippleVelocity` — wave velocity, world units/second |
| `A` | `Disturbance` — 0..1, decays; drives material FX |

### The output texture

| Channel | Meaning |
|---|---|
| `R` | Total surface height in **world units** (static + ripple) |
| `G`, `B` | World-space normal X and Y (Z reconstructed in the material) |
| `A` | Disturbance mask, 0..1 |

Because height is already in world units, the material drives World Position
Offset with it directly — no depth multiplier needed.

## Setting it up

1. Drop this folder into `<Project>/Plugins/` and enable it.
2. Add a **Sand Deformer** component to your character. The defaults match the
   UE5 Mannequin (`foot_l` / `foot_r`).
3. Tune in **Project Settings → Plugins → Sand Deformation**.
4. Wire the output into a material — see
   [doc 6](Docs/06-material-wiring.md). The quickest route is a **Sand
   Surface** component on your sand mesh, which needs no configuration at all.
5. Optional: assign a Niagara system to the deformer's **Impact Effect** to
   throw grains on landing — see [doc 5](Docs/05-impacts-and-particles.md).

## Impacts and particles

Landing and jumping are discrete events, not continuous contact, so they're
handled separately from footfalls:

- **Landing** binds `ACharacter::LandedDelegate`. Strength scales between
  `MinLandingSpeed` and `MaxLandingSpeed`, so dropping off a kerb does
  nothing and a long fall digs a real crater.
- **Jump take-off** is detected as the frame the character starts falling with
  upward velocity, scaled by `JumpImpactScale`.

Both dig a crater, pile a rim, and **kick the wave field** — that ripple
impulse is what sends a ring spreading outward, and it's the effect that reads
most clearly in motion.

Both also fire `OnSandImpact(Location, Strength, bWasLanding)` and optionally
spawn a Niagara system, which receives:

| Parameter | |
|---|---|
| `User.ImpactStrength` | 0..1 |
| `User.IsLanding` | 1 landing, 0 jump take-off |

The delegate means you're never forced to use Niagara — bind it and do
whatever you like.

## Tuning

Full table in [doc 7](Docs/07-tuning-and-troubleshooting.md). The ones that
matter most:

| Setting | Default | Effect |
|---|---|---|
| `AngleOfReposeDegrees` | 34 | Steepest slope sand holds. Lower = runs like a fluid; higher = holds walls like soil. |
| `SlumpRate` | 6 | How fast over-steep slopes collapse. |
| `RippleSpeed` | 120 | Wave speed. Auto-clamped to whatever the grid can carry stably. |
| `RippleDamping` | 1.5 | Low = rings travel far; high = they die at the impact. |
| `RegionSizeWorld` | 4096 | World units covered. **Shrink this before raising resolution** — it's the cheapest way to sharpen detail. |
| `HeightRestoreRate` | 0 | Wind flattening the surface. 0 = marks last forever. |

## Known limitations

- Single region, single focus actor per world. Splitscreen would need several.
- No replication — this is a client-side visual effect.
- The slump solver is one Jacobi iteration per frame. It converges over a few
  frames rather than instantly, which looks like sand settling, so it's a
  feature here rather than a compromise.
- Deformation displaces existing geometry; it does not create it. A flat
  two-triangle plane shades correctly but will not physically dent — see
  [doc 6](Docs/06-material-wiring.md#world-position-offset-needs-geometry).
- The surface is not saved across level load.

## License

Copyright Matin. All rights reserved.
