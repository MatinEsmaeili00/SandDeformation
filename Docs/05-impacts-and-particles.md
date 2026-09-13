# 5. Impacts & particles

[← C++ reference](04-cpp-reference.md) · [Docs index](README.md) · Next: [Material wiring →](06-material-wiring.md)

Footfalls are a *state* — which feet are touching the ground right now.
Landing and jumping are *events*. They need different handling, and that
distinction drives the whole design of
[`USandDeformerComponent`](../Source/SandDeformation/Private/SandDeformerComponent.cpp).

## Why landing can't be polled

The subsystem pulls from each component once per frame. That works perfectly
for "which feet are planted" — sampling a state at 60 Hz gives you the state.

It does **not** work for landing. A landing is instantaneous: the character is
falling, then it isn't. Poll at 60 Hz and a short fall can begin and end
between two polls, so you'd miss it entirely. Worse, by the time you notice
the character is grounded, the movement component has already zeroed the
velocity — the fall speed you need for impact strength is gone.

`ACharacter` already detects landing exactly, at the right moment, with the
hit result. So bind to it rather than re-deriving it badly:

```cpp
if (ACharacter* Character = Cast<ACharacter>(GetOwner()))
{
	Character->LandedDelegate.AddDynamic(this, &USandDeformerComponent::HandleLanded);
}
```

Unbound in `EndPlay`. The component still never ticks — an event binding costs
nothing while nothing happens, which keeps the
[pull model](01-architecture.md#why-each-piece-is-the-kind-of-object-it-is)
intact.

## Getting the fall speed

```cpp
FallSpeed = FMath::Abs(Movement->GetLastUpdateVelocity().Z);
```

`ACharacter::Landed` fires *after* the landing has been resolved, so
`GetVelocity()` already reads zero. `GetLastUpdateVelocity()` is the velocity
from before the movement component resolved the landing — the number you
actually want. This is the kind of detail that's invisible until every impact
comes out at strength 0.

```cpp
const float Range = FMath::Max(MaxLandingSpeed - MinLandingSpeed, 1.0f);
const float Strength = FMath::Clamp((FallSpeed - MinLandingSpeed) / Range, 0.0f, 1.0f);
if (Strength <= 0.0f) { return; }
```

Two thresholds rather than one, so the response is a ramp: stepping off a kerb
produces nothing at all, a normal jump produces a modest crater, and a long
fall saturates. `FMath::Max(..., 1.0f)` guards against someone setting min and
max equal and dividing by zero.

## Jump take-off

There's no delegate for pushing off — `ACharacter::OnJumped` is a
`BlueprintNativeEvent`, not a multicast you can bind to from outside. So this
one *is* derived, from the state transition:

```cpp
const bool bFalling = Movement->IsFalling();
if (bFalling && !bWasFalling && Movement->GetLastUpdateVelocity().Z > 0.0f)
{
	FireImpact(/* under the capsule */, JumpImpactScale, /*bWasLanding=*/false);
}
bWasFalling = bFalling;
```

The rising edge of "is falling" **plus upward velocity** is what distinguishes
a jump from walking off a ledge. Without the velocity test you'd get a puff of
sand every time the character stepped off a step.

Safe to poll, unlike landing, because take-off isn't instantaneous — the
character stays airborne for many frames afterwards, so the edge can't be
missed between polls.

The impact is placed at the **bottom of the capsule**, not the actor origin:

```cpp
Owner->GetActorLocation() - FVector(0.0f, 0.0f, Character->GetSimpleCollisionHalfHeight())
```

## Banking, and the one mid-gather case

Landings arrive whenever the movement component says so, which is not
necessarily during the subsystem's pull. They're banked:

```cpp
PendingImpacts.Add(Impact);
```

and drained at the start of the next `UpdateAndGatherContacts`. Jump take-off
is detected *inside* that function, though — so after firing it, the pending
list is drained a second time rather than leaving the impact to wait a frame:

```cpp
FireImpact(...);
OutContacts.Append(PendingImpacts);
PendingImpacts.Reset();
```

A frame of latency on a take-off puff would be visible; the character has
already left the ground.

## Footfalls kick the wave field on the rising edge only

A planted foot stays planted for many frames. Injecting the wave impulse on
every one of them doesn't just make walking loud — it drives the wave field to
a steady state:

```
v_{n+1} = (v_n + I)·(1 − d·Δt)      →      v* ≈ I / (d·Δt)
```

At `I = 90`, damping `0.6` and 60 fps that's about **9,000 units/second** —
standing still would ring harder than a landing, and lowering the damping
(which is what makes rings travel) makes it dramatically worse.

So the impulse fires only the frame a foot first touches down:

```cpp
const bool bJustPlanted = !PlantedSockets.Contains(Socket);
Contact.RippleImpulse = bJustPlanted ? FootprintRippleImpulse * Strength : 0.0f;
```

`PlantedSockets` is rebuilt each frame from the sockets that traced a hit, so a
foot that lifts drops out without a second pass. The *depression* still applies
every frame — that part is a state, and pressing a foot into sand continuously
is correct. Only the wave kick is an event.

This is the same state-versus-event distinction as landing, one level down.

## What an impact does

Three things, and they're deliberately separate:

```cpp
FSandDeformationContact Impact;
Impact.Radius = ImpactRadius;             // 1. displace sand: crater + rim
Impact.Depth = ImpactDepth;
Impact.RimHeight = ImpactRimHeight;
Impact.RippleImpulse = ImpactRippleImpulse;  // 2. kick the wave field
Impact.Strength = Strength;
PendingImpacts.Add(Impact);

OnSandImpact.Broadcast(WorldLocation, Strength, bWasLanding);   // 3. tell the world
```

**The ripple impulse is the one that reads.** The crater is a static change
you only notice afterwards; the expanding ring is motion, and motion is what
the eye catches. Injecting *velocity* rather than height is what makes it
travel outward instead of sitting there —
[why](02-sand-physics.md#coupling-impacts-into-the-wave-field).

## Particles

A height field fundamentally cannot represent sand **in the air**. It stores
one height per XY position; a grain thrown up off the surface isn't part of
the surface any more. So the airborne half of the effect has to come from
somewhere else, and that's what particles are for.

This is a good example of knowing what to drop: rather than making the field
simulation more elaborate, use the right tool for the part it can't express.

### The delegate first

```cpp
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FSandImpactSignature,
	FVector, Location, float, Strength, bool, bWasLanding);
```

`OnSandImpact` is `BlueprintAssignable`, so you can bind it and do anything —
Niagara, sound, camera shake, gameplay. **You are never forced to use the
built-in particle path.**

### The built-in Niagara path

```cpp
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Particles")
TSoftObjectPtr<UNiagaraSystem> ImpactEffect;
```

Soft, so a project that never assigns one doesn't load Niagara content. Spawned
with a scale that tracks strength, and handed two parameters:

```cpp
Spawned->SetVariableFloat(TEXT("User.ImpactStrength"), Strength);
Spawned->SetVariableFloat(TEXT("User.IsLanding"), bWasLanding ? 1.0f : 0.0f);
```

Setting a parameter a system doesn't declare is harmless — Niagara ignores it
— so the same code works with any system, including one that ignores both.

`Niagara` is a **private** module dependency: no public header mentions a
Niagara type, so the dependency doesn't propagate to anything that includes
our headers. See [doc 4](04-cpp-reference.md#sanddeformationbuildcs).

### Making a burst that looks right

Any Niagara system works. For sand specifically:

- **Sprite or mesh particles**, 30–80 per burst, scaled by
  `User.ImpactStrength`.
- **Cone velocity**, wide angle (60–80°), pointing up. Landing throws sand
  outward and up; a narrow cone reads as a fountain, not an impact.
- **Gravity** and a short lifetime (0.4–0.9 s). Grains should fall back fast.
- **Drag** so they decelerate — real sand doesn't fly ballistically, it's air
  resistance dominated at grain scale.
- Colour matched to your sand, with alpha fading out over life.

Use `User.IsLanding` to differentiate: a landing burst should be wider and
punchier, a take-off puff smaller and more upward.

### If you'd rather not use Niagara at all

Bind `OnSandImpact` and ignore `ImpactEffect` entirely. The simulation side —
crater, rim, ripple ring — is completely independent of the particle side and
works with nothing assigned.

## Triggering impacts yourself

```cpp
UFUNCTION(BlueprintCallable)
void TriggerSandImpact(FVector WorldLocation, float Strength, bool bWasLanding = true);
```

on the component, for a thrown prop or an animation notify. Or, with no
component at all, straight on the subsystem:

```cpp
UFUNCTION(BlueprintCallable)
void AddSandImpact(FVector WorldLocation, float Radius, float Depth, float RippleImpulse,
                   float RimHeight = 6.0f, float RimWidth = 45.0f, float Strength = 1.0f);
```

Explosions, artillery, a boulder landing. `StampSandAt` is the same call with
`RippleImpulse = 0` — a press with no wave.

---

Next: [Material wiring →](06-material-wiring.md)
