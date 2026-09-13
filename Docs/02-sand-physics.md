# 2. Sand physics

[← Architecture](01-architecture.md) · [Docs index](README.md) · Next: [Shader walkthrough →](03-shader-walkthrough.md)

Two solvers make this sand rather than snow: **slumping**, which collapses
slopes that are too steep, and a **wave equation**, which makes ripples spread
out from impacts. Both are derived here from what they're modelling.

---

# Part 1: The angle of repose

## What it is

Pour dry sand into a pile and it forms a cone with a specific slope — around
**34°** for sand, steeper for gravel, shallower for fine powder. Pour more on
and the cone gets wider, not steeper: any excess slope triggers a small
avalanche down the face. That limiting slope is the **angle of repose**, and
it's the single property that makes sand read as sand rather than as clay.

Physically it's where gravity's pull along the slope balances friction between
grains. A grain on a steeper face slides; on a shallower one it stays.

## Turning it into something a texel can compute

Work in slopes rather than angles. Between two texels `Dx` world units apart,
a slope of angle θ means a height difference of:

```
MaxDiff = tan(θ) · Dx
```

`tan` is computed once on the CPU and passed in as `TanAngleOfRepose` — no
point recomputing a constant a million times a frame.

For each neighbour, any height difference **beyond** `MaxDiff` is excess, and
excess is what avalanches:

```hlsl
const float Diff = Height - Neighbour;
if (Diff > MaxDiff)
{
    Flow -= (Diff - MaxDiff) * Rate;    // too high: shed sand
}
else if (Diff < -MaxDiff)
{
    Flow += (-Diff - MaxDiff) * Rate;   // neighbour too high: receive it
}
```

Only the amount *over* the limit moves. A slope sitting exactly at the angle
of repose is stable and nothing happens — which is what you want, otherwise
every pile would slowly flatten into nothing.

## Why this conserves sand

This matters. A solver that quietly creates or destroys material will drift:
craters slowly fill from nowhere, or the whole field sinks.

Look at a pair of adjacent texels A and B, each running on its own thread. A
computes `Diff_AB = H_A − H_B`. B computes `Diff_BA = H_B − H_A = −Diff_AB`.

Say A is higher by more than the limit. Then:

- **A** takes the first branch and loses `(Diff_AB − MaxDiff) · Rate`.
- **B** sees `Diff_BA < −MaxDiff`, takes the second branch, and gains
  `(−Diff_BA − MaxDiff) · Rate = (Diff_AB − MaxDiff) · Rate`.

Identical magnitude, opposite sign. **Whatever A gives away, B receives.**
Summed over the grid the total height is unchanged, so the solver moves sand
around without ever inventing it.

This only works because both texels read the *same* previous state — a Jacobi
iteration. If each thread wrote into the buffer the others were reading
(Gauss-Seidel), the pair would disagree about `H_A` and conservation would
break, along with determinism. It's another reason the simulation
[ping-pongs between two textures](01-architecture.md#the-state-texture).

## The 0.25

```hlsl
const float Rate = saturate(SlumpRate * Dt) * 0.25f;
```

Four neighbours each pull independently, so without the quarter a texel could
shed up to four times its excess in one step and overshoot into oscillation.
`saturate` caps `SlumpRate · Dt` at 1, so even an absurd rate or a frame hitch
can only move the full excess — never more.

## One iteration per frame

A real relaxation solver would iterate until converged. This does one pass per
frame and lets convergence happen over the following frames.

That's not a compromise here — it *is* the look. A crater whose walls sag over
the next half-second reads as sand settling. Converging instantly would look
like the sand had already finished moving before you saw it.

## Why world units

The snow plugin stores height as a normalised 0..1 with a separate depth
multiplier. Sand can't: `tan(θ) · Dx` compares a height difference against a
horizontal distance, and that comparison is only meaningful if both are in the
same real units. So sand heights are **signed world units** throughout —
positive piled up, negative dug out.

That also flips the sign convention relative to snow, where positive meant
*pressed down*. "Material flows downhill" is only simple to write when up is
positive.

---

# Part 2: Ripples

## The wave equation

A disturbance spreading across a surface at constant speed is the classic wave
equation:

```
∂²h/∂t² = c² ∇²h
```

The acceleration of the surface at a point is proportional to how much that
point deviates from the average of its neighbours. A texel sitting above its
neighbours gets pulled down; one sitting below gets pushed up. Overshoot is
what turns that into oscillation, and oscillation travelling is a wave.

## Discretising it

**The Laplacian** ∇²h, on a regular grid, is the standard 5-point stencil:

```hlsl
const float Laplacian = (L + R + U + D - 4.0f * Centre) / (Dx * Dx);
```

That's "the average of my four neighbours, minus me" (times 4), divided by
spacing squared. Positive when the texel sits in a dip.

**Second-order in time** is awkward — it wants two previous states. Instead,
split it into two first-order steps by carrying velocity as state:

```hlsl
RippleVel += Speed * Speed * Laplacian * Dt;   // accelerate
RippleVel *= saturate(1.0f - RippleDamping * Dt);
RippleHeight += RippleVel * Dt;                 // integrate
```

This is semi-implicit (symplectic) Euler: velocity updates first, then
position uses the *new* velocity. It costs one extra channel (`B` in the state
texture) instead of a whole extra texture, and it's dramatically more stable
for oscillatory systems than plain forward Euler, which pumps energy in and
diverges.

## Stability: the CFL condition

An explicit wave solver is only stable while a wave cannot cross more than
about one cell per timestep. That's the **Courant–Friedrichs–Lewy** condition:

```
C = c · Δt / Δx  ≤  ~0.7   (in 2D)
```

Exceed it and the simulation doesn't degrade gracefully — it diverges to
infinity within a handful of frames, and because the state is fed back, the
entire field turns to NaN and never recovers.

Three things can break it: a large `RippleSpeed`, a small texel (high
resolution or small region), or a long frame. All three are things a user can
change without realising the connection.

So rather than trusting the number, the shader clamps it:

```hlsl
const float MaxStableSpeed = 0.7f * Dx / max(Dt, 1e-5f);
const float Speed = min(RippleSpeed, MaxStableSpeed);
```

`RippleSpeed` becomes "as fast as you like, up to what this grid can carry".
At the defaults — 4 cm texels, 60 fps — the ceiling is about
`0.7 × 4 × 60 ≈ 168 cm/s`, so the default 120 is comfortably inside it.

And the timestep itself is clamped:

```hlsl
const float Dt = min(DeltaTime, 1.0f / 30.0f);
```

A hitch, a breakpoint, or a loading stall would otherwise hand the integrator
a huge `Δt`. Clamping makes a stall look like brief slow motion instead of an
explosion the field never recovers from. **Any feedback simulation driven by
frame time needs this**; it's the cheapest bug prevention in the file.

## Damping

```hlsl
RippleVel *= saturate(1.0f - RippleDamping * Dt);
```

Real sand isn't elastic — ripples lose energy fast. Damping velocity (not
height) removes energy without biasing the surface toward any particular
height, so waves die out *flat* rather than being dragged toward zero.

`saturate` keeps the multiplier in 0..1, so a large damping value or long
frame can at worst stop the wave dead rather than invert it.

### How long do ripples take to die

Amplitude decays as `exp(−damping · t)`. Solving for 1% remaining:

```
exp(−d·t) = 0.01   →   t = ln(100)/d ≈ 4.6/d
```

which is exactly the settle window the subsystem uses to decide when it can
stop simulating ([doc 1](01-architecture.md#why-sand-cant-go-idle)). The
physics and the optimisation are derived from the same number rather than
guessed independently.

## Coupling impacts into the wave field

A landing does two separate things:

1. **Displaces sand** — digs a crater in `Height` and piles a rim. Permanent.
2. **Kicks the wave field** — adds velocity to `RippleVel`. Transient.

```hlsl
const float Falloff = 1.0f - saturate(Dist / max(Outer, 0.001f));
RippleVel += Def.RippleImpulse * Def.Strength * Falloff;
```

Injecting **velocity** rather than height is what produces a spreading ring.
Adding height would raise a static bump that then sloshes. Adding velocity
gives the surface outward momentum, and the wave equation carries it away as a
travelling ring — which is the effect you actually recognise as an impact.

---

## Why not a full granular simulation?

Real sand is millions of discrete grains with friction and contact forces.
That's a particle simulation, and it is not affordable per frame at this
scale.

What's implemented is a **height field with two behavioural rules borrowed
from granular physics**: material can't hold a slope past its angle of repose,
and the surface carries waves. Those two rules capture nearly everything the
eye reads as "sand" from a walking-around distance. Height fields can't do
overhangs, arches or sand thrown into the air — which is precisely why
[impacts spawn particles](05-impacts-and-particles.md) for the part the field
can't represent.

Knowing which parts of the physics you can drop is most of real-time
simulation.

---

Next: [Shader walkthrough →](03-shader-walkthrough.md)
