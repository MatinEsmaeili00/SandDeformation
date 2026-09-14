# 3. Shader walkthrough

[← Sand physics](02-sand-physics.md) · [Docs index](README.md) · Next: [C++ reference →](04-cpp-reference.md)

Source: [`Shaders/Private/SandDeformation.usf`](../Shaders/Private/SandDeformation.usf).
The *why* behind the two solvers is in [doc 2](02-sand-physics.md); this is
the code.

## Conventions

```
Height > 0    sand piled above rest level
Height = 0    undisturbed
Height < 0    dug out below rest level
```

All heights are **signed world units**, not normalised. Required by the slump
solver — [why](02-sand-physics.md#why-world-units).

## The struct contract

```hlsl
struct FSandDeformerGPU
{
	float2 LocalCenter;   // XY relative to region centre, world units
	float  Radius;
	float  Depth;         // world units this presses DOWN
	float  RimHeight;     // world units piled outside the core
	float  RimWidth;
	float  RippleImpulse; // world units/second into the wave field
	float  Strength;      // 0..1
};
```

32 bytes, matching `FSandDeformerGPU` in
[`SandDeformationComputePass.h`](../Source/SandDeformation/Public/SandDeformationComputePass.h),
guarded by a `static_assert`. Add a member on one side only and every field
after it reads as garbage on the GPU, with no error anywhere.

---

# Pass 1: `SandSimulateCS`

Four jobs: reproject, slump, ripple, stamp.

## The bounds check

```hlsl
const int2 Pixel = int2(DTid.xy);
if (any(Pixel >= TextureSize)) { return; }
```

Group count is a *ceiling* divide, so the last group can extend past the
texture. Without this, those threads write out of bounds on any resolution
that isn't a multiple of 8.

## Reprojection, and why every neighbour goes through it

```hlsl
float4 LoadReprojected(int2 Pixel)
{
	const float2 UV = (float2(Pixel) + 0.5f) / float2(TextureSize);
	const float2 LocalXY = (UV - 0.5f) * RegionSize;
	const float2 PrevUV = (LocalXY + RegionOffsetFromPrev) / RegionSize + 0.5f;

	if (any(PrevUV <= 0.0f) || any(PrevUV >= 1.0f))
	{
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}

	return PrevStateTexture.SampleLevel(PrevStateSampler, PrevUV, 0) * ClearMask;
}
```

Derivation. Let `C_now` be this frame's region centre, `C_prev` last frame's,
so `RegionOffsetFromPrev = C_now − C_prev`:

```
This texel's world position:  W      = C_now + LocalXY
In last frame's local space:  L_prev = W − C_prev
                                     = LocalXY + (C_now − C_prev)
                                     = LocalXY + RegionOffsetFromPrev
As a UV:                      PrevUV = L_prev / RegionSize + 0.5
```

**Both stencils read through this function**, not just the centre texel. That
is the part worth noticing. The slump and wave solvers need neighbour values,
and if the centre were reprojected while neighbours were read raw, the stencil
would mix this frame's grid with last frame's contents — the gradients would
be wrong by exactly the player's movement, and ripples would smear in the
direction of travel. Five samples instead of one buys correctness.

Details:

- **Outside 0..1** means this world position wasn't covered last frame — newly
  scrolled in, so undisturbed.
- **`SampleLevel`, bilinear, clamped.** `SampleLevel` rather than `Sample`
  because compute shaders have no implicit derivatives to choose a mip from.
- **`ClearMask`** is 0 on the first frame, 1 after — wipes the buffer once
  without a separate clear pass.

### Why the region centre is snapped to whole texels

A bilinear tap at a *fractional* texel offset is not a lookup — it is a blur.
It returns a weighted average of two adjacent texels, and at a half-texel
offset, the worst case, it returns exactly their mean. That is a low-pass
filter, and reprojection applies it to every texel of the field, every frame
the region moves.

Work out what that costs at a walking pace. The character moves ~600 cm/s
across 4 cm texels at 60 fps, so the region scrolls 2.5 texels per frame — a
fractional part of 0.5, the maximum-blur case, sixty times a second. The
height channel shrugs this off: it is broad, smooth, and re-stamped by the
deformers constantly. The ripple field does not, because the ripple field
*is* the high-frequency content a tent kernel is designed to remove. Rings
visibly wash out within a few frames of walking, while standing still looks
perfect — the tell that the problem is reprojection and not the solver.

The fix costs nothing and lives on the CPU, in `USandDeformationSubsystem::Tick`:

```cpp
const double TexelWorldX = static_cast<double>(RegionSizeWorld) / FMath::Max(TextureResolution.X, 1);
NewRegionCenter.X = FMath::RoundToDouble(NewRegionCenter.X / TexelWorldX) * TexelWorldX;
```

Quantise the region centre to the texel grid and every offset becomes a whole
number of texels. Then, with `k` the integer texel shift:

```
PrevUV = (Pixel + 0.5)/N + k/N = (Pixel + k + 0.5)/N
```

which is exactly the centre of texel `Pixel + k`. A bilinear tap landing dead
on a texel centre has weights 1 and 0 — it returns that texel unchanged. The
filter is still bilinear; it just no longer has anything to interpolate.

The price is that the region lags the focus actor by up to half a texel, 2 cm
at the defaults. Nothing drifts out of alignment, because the material reads
the same snapped centre through `GetSandRegionParameter`, and deformer
positions are rebased against it too.

This is worth internalising as a general rule: **any world-anchored scrolling
texture must snap to its own texel grid**, or it quietly destroys its own
high-frequency content. It is the single most common bug in this family of
techniques.

## The timestep clamp

```hlsl
const float Dt = min(DeltaTime, 1.0f / 30.0f);
```

Both solvers feed back into state. A hitch would hand them a huge `Δt` and the
field would diverge and never recover. Clamping makes a stall look like slow
motion. [Why this matters](02-sand-physics.md#stability-the-cfl-condition).

## Slump

```hlsl
const float MaxDiff = TanAngleOfRepose * Dx;
const float Rate = saturate(SlumpRate * Dt) * 0.25f;

float Flow = 0.0f;
float4 Neighbours = float4(StateL.r, StateR.r, StateU.r, StateD.r);

[unroll]
for (int n = 0; n < 4; ++n)
{
	const float Diff = Height - Neighbours[n];
	if (Diff > MaxDiff)       { Flow -= (Diff - MaxDiff) * Rate; }
	else if (Diff < -MaxDiff) { Flow += (-Diff - MaxDiff) * Rate; }
}

Height += Flow;
```

Packing the four neighbours into a `float4` and `[unroll]`ing lets the
compiler turn dynamic indexing into four straight-line reads — no register
indexing, no branching over the loop.

This is exactly conservative: the same pair computes equal and opposite
differences on its two threads, so what one sheds the other receives.
[Full argument](02-sand-physics.md#why-this-conserves-sand).

## Ripple

```hlsl
const float Laplacian =
	(StateL.g + StateR.g + StateU.g + StateD.g - 4.0f * RippleHeight) / (Dx * Dx);

const float MaxStableSpeed = 0.7f * Dx / max(Dt, 1e-5f);
const float Speed = min(RippleSpeed, MaxStableSpeed);

RippleVel += Speed * Speed * Laplacian * Dt;

const float2 EdgeTexels  = min(float2(Pixel), float2(MaxPixel - Pixel));
const float  EdgeDist    = min(EdgeTexels.x, EdgeTexels.y);
const float  SpongeWidth = max(float(min(TextureSize.x, TextureSize.y)) * 0.06f, 4.0f);
const float  Sponge      = 1.0f - saturate(EdgeDist / SpongeWidth);
const float  EdgeDamping = Sponge * Sponge * 12.0f;

RippleVel *= saturate(1.0f - (RippleDamping + EdgeDamping) * Dt);
RippleHeight += RippleVel * Dt;
RippleHeight *= saturate(1.0f - (RippleDamping * 0.2f + EdgeDamping) * Dt);

const float MaxAmplitude = Dx * 2.0f;
RippleHeight = clamp(RippleHeight, -MaxAmplitude, MaxAmplitude);
RippleVel    = clamp(RippleVel, -MaxAmplitude * 8.0f, MaxAmplitude * 8.0f);
```

Semi-implicit Euler on the wave equation, with the speed clamped to the CFL
limit so the solver cannot be configured into divergence. Derived in
[doc 2](02-sand-physics.md#part-2-ripples).

Note the Laplacian reads channel `.g` (ripple height) while slump reads `.r`
(static height). Two independent fields in one texture, each with its own
solver — see [why they're separate](01-architecture.md#the-state-texture).

Three additions to the bare wave equation, each fixing something the plain
form gets wrong:

**The sponge layer.** The stencil clamps its reads at the texture border, and
a clamped boundary is a *perfectly reflecting wall* — a ring that reaches the
edge turns around and comes back. Because the region is centred on the player,
it comes back at them from every side at once. Ramping the damping up steeply
over the outer 6% of the texture absorbs the wave before it arrives. It is the
cheap stand-in for a proper radiating (Sommerfeld) boundary condition, and the
squared falloff matters: a sudden jump in damping is itself an impedance
change, and reflects.

**The height leak.** Damping the velocity alone cannot remove a uniform
offset. The `k = 0` mode — a flat lift of the whole field — has zero Laplacian
by definition, so it feels no restoring force at all; damping velocity just
freezes it in place. Any net impulse that survives therefore leaves a mound in
`G` that tracks wherever the player walked and never fades. A slow
multiplicative leak on the height itself drains it. It is kept to a fifth of
the velocity damping so it clears the offset without noticeably shortening the
life of a real ring — the impulse profile below is designed so there is very
little to clear in the first place.

**The clamp** is insurance and should never engage in play. Past roughly a
texel of amplitude per texel of spacing, the surface is steeper than a 5-point
stencil can resolve; the Laplacian starts reporting nonsense and the solver
feeds on itself.

## Wind and disturbance decay

```hlsl
if (HeightRestoreRate > 0.0f)
{
	const float Restore = HeightRestoreRate * Dt;
	Height = (Height > 0.0f) ? max(Height - Restore, 0.0f) : min(Height + Restore, 0.0f);
}

Disturbance = max(Disturbance - DisturbanceDecay * Dt, 0.0f);
```

Pull toward zero from whichever side, clamped so it never overshoots past
flat. `HeightRestoreRate = 0` means marks last forever.

## Stamping

```hlsl
const float Dist = length(LocalXY - Def.LocalCenter);
const float Outer = Def.Radius + Def.RimWidth;
if (Dist > Outer) { continue; }
```

Early-out keeps the per-deformer cost to a subtract, a length and a compare
for texels that aren't affected.

### The core

```hlsl
const float T = 1.0f - saturate(Dist / max(Def.Radius, 0.001f));
const float Press = T * T * (3.0f - 2.0f * T);
Height = min(Height, -Press * Def.Depth * Def.Strength);
```

`T*T*(3−2T)` is smoothstep. Snow uses `pow(t, Falloff)` for a crisp-walled
cone; sand gets a bowl with soft shoulders, because a sharp-walled sand crater
would be instantly demolished by the slump solver anyway — starting near the
shape the physics allows avoids a frame of visible collapse.

`min` because pressing down is negative, and the deepest press wins. Like
snow's `max`, this makes the result order-independent across deformers, which
matters because loop order is arbitrary.

### The rim

```hlsl
const float Ring = sin(saturate((Dist - Def.Radius) / max(Def.RimWidth, 0.001f)) * PI);
Height = max(Height, Ring * Def.RimHeight * Def.Strength);
```

`sin(r·π)` rises 0 → 1 → 0 across the band: a smooth ridge with no seam at
either edge.

**Unlike snow, this is not gated on undisturbed ground.** Snow has to check,
or a new rim erases an existing print. Sand doesn't need the guard because the
slump solver is already the thing that stops rims building unphysically — pile
one too high and it collapses on its own. One solver removing the need for a
special case elsewhere is a good sign the physics is carrying its weight.

### The wave kick

```hlsl
const float RippleSigma = max(Def.Radius, 1.0f) * 0.707f;
const float RippleReach = RippleSigma * 3.0f;

const float U = (Dist * Dist) / (2.0f * RippleSigma * RippleSigma);
const float Profile = (1.0f - U) * exp(-U);
RippleVel += Def.RippleImpulse * Def.Strength * Profile;
```

Velocity, not height — [why that gives a travelling ring](02-sand-physics.md#coupling-impacts-into-the-wave-field).

The *shape* matters as much as the quantity. This profile is the normalised
2D Laplacian-of-Gaussian, and it is chosen because its integral over the plane
is exactly zero. With `u = r²/2σ²` and the substitution `r dr = σ² du`:

```
∫₀^∞ (1−u)·e^(−u) · 2πr dr = 2πσ² ∫₀^∞ (1−u)e^(−u) du = 2πσ²·(1 − 1) = 0
```

Note how tight that is: the two integrals `∫e^(−u)du` and `∫u·e^(−u)du` are
both exactly 1, so the positive core is paid for precisely by the negative
annulus around it. Drop the `(1−u)` factor for a plain Gaussian and the
integral is `2πσ²` — all of it net injection.

That is the whole point, and it connects back to the height leak above: the
`k = 0` mode has no restoring force, so whatever net volume an impulse injects
into `G` stays there forever. A single-signed blob — which is what the old
linear cone `1 − r/Outer` was — dumps net volume on every single footstep, and
walking accumulates a ridge along your path. A net-zero kick launches a
travelling ring and leaves nothing behind it.

It also simply looks right. A ripple is a dip ringed by a crest, not a dome.

`σ = 0.707·Radius` puts the zero crossing at `σ√2 = Radius` — exactly the
deformer's own radius, so the core pushes over the footprint and the rebound
begins right where the sand ends. Peak value at `r = 0` is 1, the same as the
cone it replaces, so existing impulse magnitudes carry over unchanged.

One consequence to watch: the negative annulus extends further than the
footprint does, so the per-deformer early-out has to cover `RippleReach`, not
just `Outer`. Clip the annulus off and the profile stops summing to zero and
you are back to injecting DC. Widening that early-out is also why the rim
branch needs an explicit `Dist <= Outer` guard — without it the branch runs
out where `sin(π) = 0`, and `Height = max(Height, 0)` silently erases craters
in a ring around every footfall. Truncation at `3σ` leaves about 4% of peak,
which is what the height leak is there to mop up.

```hlsl
OutStateTexture[Pixel] = float4(Height, RippleHeight, RippleVel, saturate(Disturbance));
```

---

# Pass 2: `SandNormalsCS`

```hlsl
float TotalHeight(int2 Pixel)
{
	const float4 State = StateTexture.Load(int3(Pixel, 0));
	return State.r + State.g;
}
```

The two layers are summed only here, at the point of display. Everywhere else
they stay independent so their solvers don't interfere.

```hlsl
const float DzDx = (HR - HL) / (2.0f * TexelWorld.x);
const float DzDy = (HD - HU) / (2.0f * TexelWorld.y);
const float3 Normal = normalize(float3(-DzDx * NormalStrength, -DzDy * NormalStrength, 1.0f));
```

Central differences — symmetric, so the normal isn't biased half a texel to
one side. Per-axis texel size, so a non-square field isn't skewed. `Load`
rather than `Sample` for exact texels, with neighbour reads clamped at the
border.

**No sign flip**, unlike snow: height here already *is* the surface Z in world
units, so the gradient is taken directly.

For a height field `z = f(x,y)` the surface tangents are `(1,0,dz/dx)` and
`(0,1,dz/dy)`; their cross product is `(−dz/dx, −dz/dy, 1)`, which is where
the form comes from. `NormalStrength` scales XY before normalising —
exaggerating apparent slope without touching the simulation.

```hlsl
OutSandDataTexture[Pixel] = float4(HC, Normal.x, Normal.y, Disturbance);
```

Only normal X and Y are stored; Z is always positive for a height field, so
the material reconstructs it and the freed channel carries the disturbance
mask. **These are world-space normals** — see
[the material gotcha](06-material-wiring.md#the-normal-gotcha).

---

Next: [C++ reference →](04-cpp-reference.md)
