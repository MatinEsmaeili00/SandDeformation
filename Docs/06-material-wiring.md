# 6. Material wiring

[← Impacts & particles](05-impacts-and-particles.md) · [Docs index](README.md) · Next: [Tuning & troubleshooting →](07-tuning-and-troubleshooting.md)

## Why this is the fiddly part

**A material cannot reference an object that only exists at runtime.**

Material asset references are baked at save time. The simulation's render
target is created with `NewObject` while the game runs — it has no asset path,
so there's nothing for a Texture Sample node to point at.

And the texture has **no inherent world position**. It's a square that moves.
A material sampling it must be told where that square is, every frame, or it
can't turn a world position into a UV.

## What gets sent

### The `SandData` texture

| Channel | Meaning |
|---|---|
| `R` | Total surface height in **world units** (static + ripple), signed |
| `G`, `B` | World-space normal X and Y |
| `A` | Disturbance, 0..1 — recently touched |

Height being in world units is the useful difference from the snow plugin:
World Position Offset uses it **directly**, with no depth multiplier.

### The `SandRegion` vector

| Channel | Meaning |
|---|---|
| `R`, `G` | Region centre, world XY |
| `B` | Region size, world units |
| `A` | Height scale — a look multiplier, 1 by default |

## The three routes

### 1. A Sand Surface component (easiest — no configuration at all)

Put a **Sand Surface** component on the actor whose mesh is the sand. At
`BeginPlay` it converts that mesh's materials to dynamic instances and
registers them, and the subsystem keeps `SandData` and `SandRegion` current
every frame.

Your material needs a **texture parameter** named `SandData` and a **vector
parameter** named `SandRegion`. Nothing to assign in Project Settings, no
render target asset to create. Works against the transient target the
simulation makes for itself.

### 2. Assets in Project Settings

Sidesteps the runtime-texture problem by making the texture not be runtime:

1. Create a **Texture Render Target 2D** — format **RGBA16f**, tick **Can
   Create UAV**. Its size sets the simulation resolution.
2. Create a **Material Parameter Collection** with one **vector** parameter
   named `SandRegion`.
3. Assign both under *Project Settings → Plugins → Sand Deformation →
   Material Wiring*.

The material then uses a plain Texture Sample and a Collection Parameter node.
Best when several materials need the data, since the collection is global.

RGBA16f matters: heights are **signed** — sand piled above rest is positive,
dug out is negative — so a fixed-point format clips half the range away. The
plugin warns if the format is wrong and force-enables the UAV flag if you
forget it.

### 3. Read it yourself

`GetSandDataRenderTarget`, `GetSandRegionParameter`, `GetRegionCenter`,
`GetRegionSize` are Blueprint-pure.

## The material graph

```
UV       = (AbsoluteWorldPosition.xy - SandRegion.rg) / SandRegion.b + 0.5
Sand     = TextureSample(SandData, UV)        // Clamp sampler
Height   = Sand.r * SandRegion.a              // already world units
NormalXY = Sand.gb
Disturb  = Sand.a
```

That UV line is the inverse of the shader's texel → world mapping.

### Fade at the region edge

```
Fade = saturate(min(UV, 1 - UV) * 16)
Mask = Fade.x * Fade.y
```

Multiply `Height` and `NormalXY` by `Mask`. Without it the clamped sampler
smears the boundary texels outward forever and you get streaks to the horizon.

### The normal gotcha

```
Normal = float3(NormalXY, sqrt(saturate(1 - dot(NormalXY, NormalXY))))
```

Z is reconstructed because only X and Y are stored — for a height field Z is
always positive, so nothing is lost.

**These are world-space normals.** X and Y are world axes, Z is world up. You
must **untick *Tangent Space Normal*** in the material's details panel, or
they're interpreted in the mesh's tangent basis and the lighting comes out
wrong. This catches everyone exactly once.

### World Position Offset needs geometry

```
WPO = float3(0, 0, Height)
```

No sign flip and no multiplier — height is already the surface Z in world
units, positive up.

**WPO moves vertices that already exist. It does not create them.** A default
`/Engine/BasicShapes/Plane` is four vertices; there's nothing between the
corners to displace, so it will not dent regardless of settings. Options:

- a subdivided grid mesh (a 64×64 plane is plenty),
- a Nanite mesh with displacement,
- or accept shading-only, which reads well because the normals do most of the
  visual work.

Shading-only is a legitimate choice, not a fallback. Start there and add
geometry when you want silhouettes.

### Using the disturbance channel

The `A` channel is what makes sand look *disturbed* rather than merely dented:

- **Darken base colour** by it — freshly turned sand is damper and darker.
- **Drop roughness** slightly where disturbed.
- Blend in a second, coarser sand texture so churned areas look grainier.

It decays at `DisturbanceDecay` per second, so the effect fades back to
undisturbed sand on its own.

### All of it in one Custom node

Inputs `SandData` (Texture Object), `SandRegion` (float4), `WorldPos`
(float3), with *Additional Outputs* `Normal` (float3) and `Disturbance`
(float):

```hlsl
float2 UV    = (WorldPos.xy - SandRegion.xy) / SandRegion.z + 0.5f;
float2 Fade  = saturate(min(UV, 1.0f - UV) * 16.0f);
float  Mask  = Fade.x * Fade.y;
float4 Sand  = Texture2DSample(SandData, SandDataSampler, saturate(UV));
Normal       = float3(Sand.gb * Mask, 0.0f);
Normal.z     = sqrt(saturate(1.0f - dot(Normal.xy, Normal.xy)));
Disturbance  = Sand.a * Mask;
return Sand.r * Mask * SandRegion.w;   // height, world units
```

---

Next: [Tuning & troubleshooting →](07-tuning-and-troubleshooting.md)
