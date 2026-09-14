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

### The CFL condition is also why ripples need sub-stepping

The clamp keeps the solver *stable*, but it has a second consequence that is
easy to miss and ruins the effect: **it caps how far a ripple can travel.**

Work it out at the defaults — 4 cm texels, 60 fps:

```
speed cap  = 0.7 × 4 cm / (1/60 s)  ≈ 168 cm/s
wave life  = 1 / damping = 1 / 1.5  ≈ 0.67 s
distance   = 168 × 0.67             ≈ 110 cm
```

**A ring dies about a metre from the impact**, and no amount of raising
`RippleSpeed` helps, because the clamp throws the extra away. That's not a
tuning problem, it's a structural one.

Worse, the obvious fix makes it worse: raising resolution or shrinking the
region gives *smaller* texels, which *lowers* the cap. Finer detail costs wave
speed.

The way out is to shorten the timestep instead. Run the simulation **N times
per frame** at `Δt/N`:

```
speed cap  = 0.7 × Δx / (Δt/N)  =  N × (0.7 × Δx / Δt)
```

The ceiling and the distance covered per frame both scale linearly with N. At
N = 4 the cap goes from 168 to about 670 cm/s, and with damping at 0.6 a ring
now travels roughly `450 × (1/0.6) ≈ 7.5 metres` — across the whole visible
area, which is what you actually want to see.

**Sub-stepping has to be N separate dispatches, not a loop inside the shader.**
Each iteration reads its neighbours, and there is no way to synchronise every
thread across the whole grid mid-kernel — thread A cannot wait for thread B to
finish writing before reading it. So the pass is dispatched N times, ping-ponging
between the two state textures each time.

Two details fall out of that:

- **Reprojection and stamping belong to the frame, not to the sub-step.** They
  happen on the first iteration only. After that the region offset is zero
  (which makes the reprojection an exact identity sample at texel centres) and
  there are no deformers left to stamp. Stamping on every sub-step would inject
  the impulse N times.
- **The ping-pong parity matters.** With an even N the newest state ends up
  back in the texture the frame started from, so the CPU-side index must only
  flip when N is odd — see
  [doc 4](04-cpp-reference.md#the-ping-pong-parity).

Everything else — slump, damping, disturbance decay, wind — is rate-per-second
and integrated with the same `Δt/N`, so running N times produces the same total
change. Only the wave solver actually gains from the finer steps.

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

### The one thing velocity damping cannot do

That argument is right about *energy*, and it is exactly why velocity damping
is the main mechanism. But it has a blind spot, and the blind spot is the
uniform mode.

Damping velocity removes motion. It does not remove **position**. If the field
has picked up a flat offset, damping the velocity brings the drift to a stop
and then leaves the offset sitting there — permanently, because the `k = 0`
mode has zero Laplacian and so the wave equation applies no restoring force to
it ([the full argument](#why-the-impulse-must-integrate-to-zero)). "Waves die
out flat" is true; it just doesn't promise they die out at *zero*.

So the solver also leaks the height itself, at a fifth of the velocity
damping:

```hlsl
RippleHeight *= saturate(1.0f - (RippleDamping * 0.2f + EdgeDamping) * Dt);
```

Kept deliberately gentle, because unlike velocity damping this *does* bias the
surface toward zero — it attenuates genuine travelling rings along with the
offset. It is a correction for what the impulse profile fails to cancel, not a
primary mechanism. Turn it up and ripples die young.

### Absorbing the boundary

The 5-point stencil clamps its reads at the texture border. A clamped boundary
is a perfectly reflecting wall: a ring reaching the edge inverts and travels
back inward, and since the region is centred on the player, it converges on
them from all sides. Ramping the damping up steeply over the outer band of
texels absorbs the wave before it gets there — a cheap stand-in for a proper
radiating boundary condition. The falloff is squared rather than linear
because an abrupt change in damping is itself an impedance discontinuity, and
impedance discontinuities are precisely what reflect waves.

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
const float U = (Dist * Dist) / (2.0f * RippleSigma * RippleSigma);
const float Profile = (1.0f - U) * exp(-U);
RippleVel += Def.RippleImpulse * Def.Strength * Profile;
```

Injecting **velocity** rather than height is what produces a spreading ring.
Adding height would raise a static bump that then sloshes. Adding velocity
gives the surface outward momentum, and the wave equation carries it away as a
travelling ring — which is the effect you actually recognise as an impact.

### Why the impulse must integrate to zero

Look again at the wave equation: `∂²h/∂t² = c²∇²h`. The forcing on any mode is
proportional to its Laplacian. For a spatially uniform mode — `k = 0`, a flat
lift of the entire field — the Laplacian is identically zero, so **the uniform
mode has no restoring force whatsoever.** It is not weakly restored; it is not
restored at all.

The consequence is easy to miss and impossible to unsee. Any impulse whose
integral over the plane is non-zero injects a permanent, irremovable offset
into `RippleHeight`. Damping the velocity does not help: it just brings the
drift to a halt and leaves the offset frozen. Every footstep adds a little
more, so walking builds a ridge of ghost material along your path.

So the profile is chosen to make that integral vanish. With
`u = r²/2σ²`, so that `r dr = σ² du`:

```
∫₀^∞ (1−u)·e^(−u) · 2πr dr = 2πσ² ∫₀^∞ (1−u)e^(−u) du
                           = 2πσ² · (1 − 1)
                           = 0
```

using `∫₀^∞ e^(−u)du = 1` and `∫₀^∞ u·e^(−u)du = 1`. Those two integrals being
equal is what makes the cancellation exact rather than approximate — the
positive core is paid for precisely by the negative annulus surrounding it.

This is the normalised 2D Laplacian-of-Gaussian, the same operator used as a
blob detector in image processing, and for the same underlying reason: it is
the shape that responds to local structure while ignoring any constant
background.

Drop the `(1−u)` and you have a plain Gaussian, whose integral is `2πσ²` —
every bit of it net injection. The naive linear cone `1 − r/Outer` is worse
still, being single-signed across its whole support.

Physically the net-zero condition is just conservation: a footstep pushes sand
down in one place and up in a ring around it. It does not create material. The
maths and the intuition agree, which is usually the sign you have the right
model rather than a tuned hack.

A residual leak on `RippleHeight` still exists in the solver as a safety net,
because truncating the annulus at a finite radius leaves a few percent of the
integral uncancelled. But it is a mop, not a fix — the fix is the profile.

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
