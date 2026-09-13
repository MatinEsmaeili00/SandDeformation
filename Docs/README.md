# Sand Deformation — the long explanation

These documents exist so you can **rebuild this plugin from nothing and
understand every decision in it**. Where there was a choice, they say what the
alternatives were and why this one won.

If you only want to *use* the plugin, the [top-level README](../README.md) is
enough. Come here to learn how it works.

## Read in this order

| # | Document | What you get out of it |
|---|---|---|
| 1 | [Architecture](01-architecture.md) | The scrolling world-anchored region, the four-channel state texture, and why sand can't go idle the way snow can. |
| 2 | [**Sand physics**](02-sand-physics.md) | The angle of repose and the wave equation, derived from scratch. The heart of this plugin — read this one even if you skip the rest. |
| 3 | [Shader walkthrough](03-shader-walkthrough.md) | `SandDeformation.usf` line by line. |
| 4 | [C++ reference](04-cpp-reference.md) | Every class and file, and why it's that kind of object. |
| 5 | [Impacts & particles](05-impacts-and-particles.md) | Why landing is an event and not a state, the ripple kick, and getting grains in the air. |
| 6 | [Material wiring](06-material-wiring.md) | Getting GPU output into a material. |
| 7 | [Tuning & troubleshooting](07-tuning-and-troubleshooting.md) | Every knob, and symptom → cause. |

## The one-paragraph version

A compute shader maintains a texture representing a square of sand around the
player, holding a surface height, a wave height, a wave velocity and a
disturbance mask per texel. Each frame it copies last frame's texture into
this frame's position, lets any slope steeper than sand's angle of repose
collapse, integrates a 2D wave equation so ripples spread, and stamps a
crater for every foot or landing. A second pass turns the result into normals.
Everything else is plumbing around those two passes.

## If you've read the snow docs

This plugin shares exactly one idea with its
[snow counterpart](https://github.com/MatinEsmaeili00/SnowDeformation): the
world-anchored scrolling region, which is re-derived in
[doc 1](01-architecture.md#the-scrolling-region). Everything else is
different, because snow and sand behave differently:

- Snow holds whatever shape you press into it. Sand **collapses** past ~34°.
- Snow is inert. Sand **carries waves**.
- Snow height is normalised and inverted (positive = pressed down). Sand
  height is **signed world units**, positive = piled up, because
  [slumping needs real slopes](02-sand-physics.md#why-world-units).
- Snow can stop simulating the moment nothing touches it. Sand
  [has to keep going](01-architecture.md#why-sand-cant-go-idle) while ripples
  ring out.

## Where to look in the code

1. [`SandDeformation.usf`](../Shaders/Private/SandDeformation.usf) — the
   algorithm. Start here. Explained in [doc 3](03-shader-walkthrough.md).
2. [`SandDeformerComponent.cpp`](../Source/SandDeformation/Private/SandDeformerComponent.cpp)
   — footfalls and impacts. Explained in [doc 5](05-impacts-and-particles.md).
3. [`SandDeformationSubsystem.cpp`](../Source/SandDeformation/Private/SandDeformationSubsystem.cpp)
   — the game-thread half. Explained in [doc 4](04-cpp-reference.md).
