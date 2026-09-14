# 4. C++ reference

[← Shader walkthrough](03-shader-walkthrough.md) · [Docs index](README.md) · Next: [Impacts & particles →](05-impacts-and-particles.md)

File by file. The *shape* of the design is argued in
[doc 1](01-architecture.md); this is the detail.

## File map

| File | Role |
|---|---|
| `SandDeformation.Build.cs` | Module dependencies |
| `SandDeformation.h` / `.cpp` | Module entry, shader path mapping, log category |
| `SandDeformationSettings.h` / `.cpp` | Project Settings page |
| `SandDeformationComputePass.h` / `.cpp` | Shader declarations + RDG dispatch |
| `SandDeformationSubsystem.h` / `.cpp` | The simulation, game-thread half |
| `SandDeformerComponent.h` / `.cpp` | Footfalls and impacts — see [doc 5](05-impacts-and-particles.md) |
| `SandSurfaceComponent.h` / `.cpp` | Auto-wires a mesh's materials |

## `SandDeformation.Build.cs`

```csharp
PublicDependencyModuleNames:  Core, CoreUObject, Engine, DeveloperSettings
PrivateDependencyModuleNames: Projects, RHI, RenderCore, Niagara
```

**`DeveloperSettings` is public** because `USandDeformationSettings` derives
from `UDeveloperSettings` in a public header — a dependent module couldn't
compile otherwise.

**`Niagara` is private.** No public header exposes a Niagara type, so a
consumer never has to care that we use it. Keeping it private means the
dependency doesn't propagate.

**`Renderer` is absent.** Everything used here — RDG, `FGlobalShader`,
`FComputeShaderUtils`, `CreateRenderTarget` — lives in `RenderCore`. Reaching
into `Renderer`'s private headers is a portability trap that breaks
installed-engine and packaged builds.

## The compute pass

Declaration and dispatch follow the same pattern as the snow plugin: a
parameter struct whose member names must match the HLSL globals **exactly**
(a mismatch silently reads as zero — no error), `DECLARE_GLOBAL_SHADER` +
`SHADER_USE_PARAMETER_STRUCT`, `IMPLEMENT_GLOBAL_SHADER` binding the virtual
shader path, and RDG building a two-pass graph.

Sand-specific notes:

**Both passes share one group count.**

```cpp
const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(
	Params.TextureSize,
	FIntPoint(SandDeformation::ThreadGroupSize, SandDeformation::ThreadGroupSize));
```

`GetGroupCount` returns `FIntVector`, not `FIntPoint` — storing it in the
wrong type is a compile error, which is the good outcome; the equivalent
mistake in the HLSL thread-group size would be silent. That's why the group
size is a single shared constant used by both `GetGroupCount` and the
`THREADS_X`/`THREADS_Y` defines.

**Pass 2 reads what pass 1 wrote.** The final state texture is the UAV output
of the last simulate dispatch and the SRV input of the normals pass. RDG sees
that dependency from the parameter declarations and inserts the barrier and
ordering itself — the main reason to use RDG rather than raw RHI dispatches.
With sub-stepping there are N such dependencies chained back to back, and RDG
handles the whole chain without any explicit barriers in our code.

### The ping-pong parity

The simulate pass is dispatched `SubSteps` times, alternating source and
destination:

```cpp
for (int32 Step = 0; Step < SubSteps; ++Step)
{
    /* dispatch SrcTexture -> DstUAV */
    Swap(SrcTexture, DstTexture);
    Swap(SrcUAV, DstUAV);
}
// after the final swap, SrcTexture holds the newest state
```

Because it swaps once per sub-step, **an even count lands the newest state
back in the texture the frame started from.** So the CPU-side index can't flip
unconditionally:

```cpp
if (Params.SubSteps % 2 == 1)
{
    CurrentStateIndex = 1 - CurrentStateIndex;
}
```

Getting this wrong doesn't crash — it silently reads a one-sub-step-stale
buffer every frame, which looks like ripples that stutter or lose energy for no
apparent reason. Exactly the class of bug that is miserable to track down from
the symptom, which is why it's worth stating plainly.

Why [sub-stepping exists at all](02-sand-physics.md#the-cfl-condition-is-also-why-ripples-need-sub-stepping).

## `USandDeformationSettings`

```cpp
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Sand Deformation"))
```

Exists out of necessity: **a world subsystem has no details panel anywhere**,
so `EditAnywhere` on the subsystem is unreachable and the simulation could not
otherwise be tuned. `config = Game` + `defaultconfig` persists to
`DefaultGame.ini` at project level; `GetCategoryName()` returns `Plugins`.

Asset references are `TSoftObjectPtr` — a project not using them never loads
them. Resolved once in the subsystem's `Initialize`.

## `USandDeformationSubsystem`

### Lifecycle

`Initialize` seeds runtime values from the settings, resolves and validates
the wiring assets. `Deinitialize` clears registrations, releases targets,
drops resolved assets.

`DoesSupportWorldType` restricts to `Game` and `PIE` — without it the
subsystem spins up for editor preview worlds and asset thumbnails.

### Render targets

```cpp
StateRenderTargets[0] = MakeRenderTarget(RTF_RGBA32f);
StateRenderTargets[1] = MakeRenderTarget(RTF_RGBA32f);
```

Full float, ping-ponged. Half precision would halve bandwidth but ripple
velocity is integrated across frames and the error accumulates into visible
drift within seconds. The **output** target is `RTF_RGBA16f` — consumed once
per frame, never fed back, so precision loss can't compound.

`NewObject` is called with no name: re-allocating on a resolution change would
otherwise collide with the outgoing object still holding it.

### `PrepareOutputAsset`

Runs **once**, at `Initialize`, not inside `EnsureRenderTargets` — that runs
every frame, so a mis-configured asset would log the same warning sixty times
a second.

It force-enables `bCanCreateUAV` (without it UAV creation fails and the field
is silently blank) and warns about a non-RGBA16f format (heights are signed,
so fixed-point clips everything below rest level). The format is only warned
about, never overridden — that would stomp a deliberate choice.

A supplied asset also **decides the simulation resolution** rather than being
resized to match `TextureResolution`. Silently resizing someone's asset is a
worse surprise than ignoring a setting.

### Tick order

```
1. EnsureRenderTargets()
2. resolve focus actor → NewRegionCenter        (not yet committed)
2b. SNAP NewRegionCenter to the texel grid
3. pull contacts + impacts from every component
4. update SettleTimeRemaining
5. decide bNeedsDispatch — early out if false
6. check render target resources live — early out if not
7. commit RegionCenterWorld, convert to region-local
8. ENQUEUE_RENDER_COMMAND, flip ping-pong index
9. PushMaterialParameters()
```

**`RegionCenterWorld` is only committed at step 7.** Bailing at 5 or 6 leaves
it where the texture is still anchored, so next frame's offset is measured
from the right place and no motion is lost. Committing earlier and then
bailing would desynchronise the texture from its own centre.

### Step 2b: snapping the region to the texel grid

```cpp
const double TexelWorldX = static_cast<double>(RegionSizeWorld) / FMath::Max(TextureResolution.X, 1);
const double TexelWorldY = static_cast<double>(RegionSizeWorld) / FMath::Max(TextureResolution.Y, 1);
NewRegionCenter.X = FMath::RoundToDouble(NewRegionCenter.X / TexelWorldX) * TexelWorldX;
NewRegionCenter.Y = FMath::RoundToDouble(NewRegionCenter.Y / TexelWorldY) * TexelWorldY;
```

Four lines of CPU arithmetic that decide whether the ripple field survives
being walked across. The shader resamples last frame's state through a
bilinear tap; at a fractional texel offset that tap is a low-pass filter over
the entire field, every frame. Snapping makes every offset an integer texel
count, so the tap lands on a texel centre and returns it untouched.
[Full derivation](03-shader-walkthrough.md#why-the-region-centre-is-snapped-to-whole-texels).

**Order matters.** This has to happen before step 5, not after. `bNeedsDispatch`
compares `NewRegionCenter` against the committed centre, and it must compare
*snapped to snapped* — otherwise sub-texel jitter reports a move every frame
and the sim never idles while the player breathes on the analogue stick.

It must also happen before step 7's conversion to region-local space, so
deformer positions are rebased against the same snapped centre the texture is
anchored to. `GetSandRegionParameter` publishes that centre to the material,
so the CPU, the GPU and the shader all agree on where the region is.

Note the `double` arithmetic. `FVector2D` is double-precision in UE5, and
rounding through `float` in a level built far from the origin would reintroduce
exactly the sub-texel error the snap exists to remove.

### The settle window

```cpp
const float RippleSettle = 4.6f / FMath::Max(RippleDamping, 0.1f);
SettleTimeRemaining = FMath::Max(RippleSettle, 2.0f);
```

Sand keeps moving after you stop touching it, so unlike snow it can't stop the
instant nothing is pressing on it. The window is derived from the damping
constant rather than guessed —
[derivation](02-sand-physics.md#how-long-do-ripples-take-to-die).

A **moving region always dispatches** regardless, because the texture is
world-anchored and a move must be reprojected. Easiest condition to get wrong.

### Banking world space

```cpp
void USandDeformationSubsystem::AddSandImpact(FVector WorldLocation, ...)
{
	Deformer.LocalCenter = FVector2f(FVector2D(WorldLocation));   // world, not local
	PendingOneShotDeformers.Add(Deformer);
}
```

then in `Tick`, after the centre is committed:

```cpp
const FVector2f RegionCenterFloat(RegionCenterWorld);
for (FSandDeformerGPU& OneShot : PendingOneShotDeformers)
{
	OneShot.LocalCenter -= RegionCenterFloat;
	GPUDeformers.Add(OneShot);
}
```

Converting to region-local **at call time** would use whatever centre was left
over from the previous tick; `Tick` then moves the region and hands the shader
a value it interprets against the *new* centre. Every impact would land offset
by exactly how far the player moved that frame. Banking world space and
rebasing once, at the moment the centre is known, is the fix.

(This is not hypothetical — it's a real bug that shipped in the snow plugin
before being caught.)

### Rendering thread safety

```cpp
FTextureRenderTargetResource* PrevResource = StateRenderTargets[...]->GameThread_GetRenderTargetResource();
...
ENQUEUE_RENDER_COMMAND(SandDeformationDispatch)(
	[Params, PrevResource, NextResource, DataResource](FRHICommandListImmediate& RHICmdList) mutable
	{
		Params.PrevStateTexture = PrevResource->GetRenderTargetTexture();
		...
	});
```

The **resource** is captured, not the `FRHITexture*`. A resource can swap its
underlying texture — a resize, a device reset — between the game thread
reading it and the command running. Resolving inside the lambda always gets
the texture valid at execution time.

`Params` is captured by value so the render thread owns its own copy of the
deformer array; `mutable` only so those three pointers can be filled in.

### `PushMaterialParameters`

Runs every frame **including frames where the dispatch is skipped** — a
material instance created this frame still needs its parameters. Pushes
`SandRegion` into the collection (only if it actually declares that parameter,
checked once at `Initialize`, otherwise the engine errors every frame), then
refreshes registered MIDs, pruning collected ones.

```cpp
FLinearColor GetSandRegionParameter() const
{
	return FLinearColor(RegionCenterWorld.X, RegionCenterWorld.Y, RegionSizeWorld, HeightScale);
}
```

Four values in one vector — one collection entry, one set call per frame, and
they're always mutually consistent because they're written together.

## `USandSurfaceComponent`

At `BeginPlay`, converts the owner's material slots to dynamic instances and
registers them with the subsystem, which then keeps `SandData` and
`SandRegion` current. Unregisters and drops them at `EndPlay`.

Held **strongly** (`TObjectPtr`), unlike the subsystem's registration list,
because these instances exist only because this component made them — nothing
else keeps them alive. The subsystem's list is weak, so a collected MID drops
out there on its own.

---

Next: [Impacts & particles →](05-impacts-and-particles.md)
