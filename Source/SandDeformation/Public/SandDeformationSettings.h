// Copyright Matin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SandDeformationSettings.generated.h"

class UMaterialParameterCollection;
class UTextureRenderTarget2D;

/**
 * Project-wide sand simulation defaults, plus the optional assets that wire
 * the simulation into a material. Project Settings > Plugins > Sand Deformation.
 *
 * A world subsystem has no details panel of its own, so this is where the
 * simulation is actually tuned; USandDeformationSubsystem copies these values
 * when it initialises, and they stay writable at runtime from Blueprint.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Sand Deformation"))
class SANDDEFORMATION_API USandDeformationSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	USandDeformationSettings();

	virtual FName GetCategoryName() const override;

	static const USandDeformationSettings& Get();

	// --- Field ------------------------------------------------------------

	/** Resolution of the simulated surface. Ignored when SandDataRenderTarget is set - that asset's size wins. */
	UPROPERTY(config, EditAnywhere, Category = "Field")
	FIntPoint TextureResolution;

	/** World units covered by the whole texture, centred on the focus actor. */
	UPROPERTY(config, EditAnywhere, Category = "Field", meta = (ClampMin = "1.0"))
	float RegionSizeWorld;

	// --- Sand behaviour ---------------------------------------------------

	/**
	 * Steepest slope dry sand will hold before the face collapses. Around 34
	 * degrees for real sand; lower makes it behave more like a fluid, higher
	 * more like soil that holds its walls.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Sand", meta = (ClampMin = "1.0", ClampMax = "80.0", UIMin = "20.0", UIMax = "50.0"))
	float AngleOfReposeDegrees;

	/** How quickly an over-steep slope collapses. Higher = sand runs away almost instantly. */
	UPROPERTY(config, EditAnywhere, Category = "Sand", meta = (ClampMin = "0.0"))
	float SlumpRate;

	/** World units per second the surface drifts back to flat, standing in for wind. 0 = footprints last forever. */
	UPROPERTY(config, EditAnywhere, Category = "Sand", meta = (ClampMin = "0.0"))
	float HeightRestoreRate;

	// --- Ripples ----------------------------------------------------------

	/**
	 * Wave propagation speed in world units/second. Automatically clamped in
	 * the shader to whatever the texel size and frame time can carry
	 * stably (the CFL condition), so a large value here just means "as fast
	 * as this grid allows".
	 */
	UPROPERTY(config, EditAnywhere, Category = "Ripples", meta = (ClampMin = "0.0"))
	float RippleSpeed;

	/** Wave energy lost per second. Low = rings travel far; high = they die near the impact. */
	UPROPERTY(config, EditAnywhere, Category = "Ripples", meta = (ClampMin = "0.0"))
	float RippleDamping;

	/** How fast the 0..1 "recently disturbed" mask fades, per second. Drives material FX. */
	UPROPERTY(config, EditAnywhere, Category = "Ripples", meta = (ClampMin = "0.0"))
	float DisturbanceDecay;

	/**
	 * How many times the simulation runs per frame, each at DeltaTime/N.
	 *
	 * This is the setting that decides whether ripples actually travel. An
	 * explicit wave solver is stability-capped at 0.7 * texel / dt - about
	 * 168 cm/s at the default 4cm texels and 60fps - so with one step a ring
	 * dies less than a metre from the impact no matter how high you set
	 * RippleSpeed. Each sub-step raises that ceiling and advances the wave
	 * further, both linearly in N.
	 *
	 * Costs N dispatches of the simulate pass per frame. 4 is a good balance;
	 * 1 reverts to the old single-step behaviour.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Ripples", meta = (ClampMin = "1", ClampMax = "16"))
	int32 RippleSubSteps;

	/**
	 * Multiplies every ripple impulse, wherever it came from - footfalls,
	 * landings, explosions.
	 *
	 * Per-source impulses live on the deformer component, which means a
	 * Blueprint that already has the component keeps whatever value it was
	 * created with. This is the one place that scales all of them at once, so
	 * turning ripples up doesn't mean hunting through details panels.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Ripples", meta = (ClampMin = "0.0"))
	float RippleImpulseScale;

	/**
	 * Exaggerates the ripple layer in the output texture. 1 is physically
	 * honest; higher makes waves read strongly without touching the simulation.
	 *
	 * This is the safe way to make ripples obvious. It is applied in the
	 * normals pass on the way out and never fed back into the state texture,
	 * so unlike a bigger impulse it cannot affect stability at any value - and
	 * because the normals are derived from the scaled height, it deepens the
	 * shading as well as the displacement.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Ripples", meta = (ClampMin = "0.0"))
	float RippleVisualScale;

	// --- Shading ----------------------------------------------------------

	/** Scales the slope baked into the output normals. Pure look control. */
	UPROPERTY(config, EditAnywhere, Category = "Shading", meta = (ClampMin = "0.0"))
	float NormalStrength;

	/** Multiplier the material applies to height. Raise to exaggerate displacement without changing the simulation. */
	UPROPERTY(config, EditAnywhere, Category = "Shading", meta = (ClampMin = "0.0"))
	float HeightScale;

	// --- Material wiring --------------------------------------------------

	/**
	 * Optional, and the thing that makes material wiring possible at all: a
	 * material cannot reference a texture that only exists at runtime, so when
	 * this is set the simulation writes its output into this asset instead of
	 * an auto-created transient one.
	 *
	 * Create it as a Texture Render Target 2D with Render Target Format
	 * RGBA16f and "Can Create UAV" ticked.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Material Wiring", meta = (AllowedClasses = "/Script/Engine.TextureRenderTarget2D"))
	TSoftObjectPtr<UTextureRenderTarget2D> SandDataRenderTarget;

	/** Optional. When set, the region vector below is pushed into this collection every frame. */
	UPROPERTY(config, EditAnywhere, Category = "Material Wiring", meta = (AllowedClasses = "/Script/Engine.MaterialParameterCollection"))
	TSoftObjectPtr<UMaterialParameterCollection> ParameterCollection;

	/**
	 * Name of the vector parameter carrying everything a material needs:
	 *   R = region centre X, G = region centre Y,
	 *   B = region size (world units), A = height scale.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Material Wiring")
	FName RegionParameterName;

	/** Texture parameter name used by ApplySandParametersToMaterial / RegisterSandMaterial. */
	UPROPERTY(config, EditAnywhere, Category = "Material Wiring")
	FName SandDataParameterName;
};
