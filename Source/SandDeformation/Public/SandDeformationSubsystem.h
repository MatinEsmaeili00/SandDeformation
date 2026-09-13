// Copyright Matin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SandDeformationComputePass.h"
#include "SandDeformationSubsystem.generated.h"

class UMaterialInstanceDynamic;
class UMaterialParameterCollection;
class UTextureRenderTarget2D;
class USandDeformerComponent;

/**
 * Runs the sand simulation for a world: one persistent, world-anchored
 * surface that follows a focus actor (the local player by default), fed every
 * frame by whatever USandDeformerComponents are registered.
 *
 * Tuning lives in Project Settings > Plugins > Sand Deformation
 * (USandDeformationSettings); the values below are seeded from there on
 * Initialize and can be overridden at runtime from Blueprint.
 */
UCLASS()
class SANDDEFORMATION_API USandDeformationSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

	/** Called by USandDeformerComponent::BeginPlay - no need to call directly. */
	void RegisterDeformer(USandDeformerComponent* Component);
	void UnregisterDeformer(USandDeformerComponent* Component);

	/** A press into the sand not tied to a component - a dropped prop, a vehicle wheel. Lands next tick. */
	UFUNCTION(BlueprintCallable, Category = "Sand Deformation")
	void StampSandAt(FVector WorldLocation, float Radius, float Depth, float RimHeight = 2.0f, float RimWidth = 10.0f, float Strength = 1.0f);

	/**
	 * A crater plus a ring of ripples spreading outwards - an explosion, a
	 * boulder, anything that hits hard. RippleImpulse is world units/second
	 * injected into the wave field.
	 */
	UFUNCTION(BlueprintCallable, Category = "Sand Deformation")
	void AddSandImpact(FVector WorldLocation, float Radius, float Depth, float RippleImpulse, float RimHeight = 6.0f, float RimWidth = 45.0f, float Strength = 1.0f);

	/** The region tracks this actor's XY every frame. Defaults to player pawn 0 when unset. */
	UFUNCTION(BlueprintCallable, Category = "Sand Deformation")
	void SetFocusActor(AActor* NewFocusActor);

	// --- Material wiring --------------------------------------------------

	/** R = surface height in world units, GB = normal.xy, A = 0..1 recently-disturbed mask. */
	UFUNCTION(BlueprintPure, Category = "Sand Deformation|Material")
	UTextureRenderTarget2D* GetSandDataRenderTarget() const { return SandDataRenderTarget; }

	/**
	 * Everything a material needs to place the sand texture in the world,
	 * packed into one vector: RG = region centre XY, B = region size,
	 * A = height scale.
	 */
	UFUNCTION(BlueprintPure, Category = "Sand Deformation|Material")
	FLinearColor GetSandRegionParameter() const;

	/** Pushes SandData + SandRegion onto a dynamic material instance once. */
	UFUNCTION(BlueprintCallable, Category = "Sand Deformation|Material")
	void ApplySandParametersToMaterial(UMaterialInstanceDynamic* Material) const;

	/** Keeps a dynamic material instance's parameters current every frame. Held weakly. */
	UFUNCTION(BlueprintCallable, Category = "Sand Deformation|Material")
	void RegisterSandMaterial(UMaterialInstanceDynamic* Material);

	UFUNCTION(BlueprintCallable, Category = "Sand Deformation|Material")
	void UnregisterSandMaterial(UMaterialInstanceDynamic* Material);

	UFUNCTION(BlueprintPure, Category = "Sand Deformation")
	FVector2D GetRegionCenter() const { return RegionCenterWorld; }

	UFUNCTION(BlueprintPure, Category = "Sand Deformation")
	float GetRegionSize() const { return RegionSizeWorld; }

	// --- Runtime overrides (seeded from USandDeformationSettings) ----------

	UPROPERTY(BlueprintReadWrite, Category = "Sand Deformation|Settings")
	FIntPoint TextureResolution = FIntPoint(1024, 1024);

	UPROPERTY(BlueprintReadWrite, Category = "Sand Deformation|Settings")
	float RegionSizeWorld = 4096.0f;

	/** Steepest slope dry sand holds before collapsing, in degrees. */
	UPROPERTY(BlueprintReadWrite, Category = "Sand Deformation|Settings")
	float AngleOfReposeDegrees = 34.0f;

	UPROPERTY(BlueprintReadWrite, Category = "Sand Deformation|Settings")
	float SlumpRate = 6.0f;

	UPROPERTY(BlueprintReadWrite, Category = "Sand Deformation|Settings")
	float RippleSpeed = 120.0f;

	UPROPERTY(BlueprintReadWrite, Category = "Sand Deformation|Settings")
	float RippleDamping = 1.5f;

	UPROPERTY(BlueprintReadWrite, Category = "Sand Deformation|Settings")
	float DisturbanceDecay = 0.8f;

	UPROPERTY(BlueprintReadWrite, Category = "Sand Deformation|Settings")
	float HeightRestoreRate = 0.0f;

	UPROPERTY(BlueprintReadWrite, Category = "Sand Deformation|Settings")
	float NormalStrength = 1.0f;

	UPROPERTY(BlueprintReadWrite, Category = "Sand Deformation|Settings")
	float HeightScale = 1.0f;

private:
	void EnsureRenderTargets();
	void ReleaseRenderTargets();
	AActor* ResolveFocusActor() const;
	void PrepareOutputAsset(UTextureRenderTarget2D* Asset);
	void PushMaterialParameters();

	/** Ping-ponged simulation state: R height, G ripple height, B ripple velocity, A disturbance. */
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> StateRenderTargets[2] = { nullptr, nullptr };

	/** Either the asset from the settings, or a transient one we made ourselves. */
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> SandDataRenderTarget = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> ConfiguredOutputAsset = nullptr;

	bool bUsingExternalOutput = false;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> ParameterCollection = nullptr;

	UPROPERTY(Transient)
	TWeakObjectPtr<AActor> FocusActor;

	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<USandDeformerComponent>> RegisteredDeformers;

	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<UMaterialInstanceDynamic>> RegisteredMaterials;

	FName RegionParameterName;
	FName SandDataParameterName;
	bool bRegionParameterValid = false;

	TArray<FSandDeformerGPU> PendingOneShotDeformers;

	FVector2D RegionCenterWorld = FVector2D::ZeroVector;
	int32 CurrentStateIndex = 0;
	bool bTargetsInitialised = false;
	bool bFirstFrame = true;

	/**
	 * Seconds of simulation still owed after the last disturbance. Sand keeps
	 * moving after you stop touching it - ripples have to ring out and slopes
	 * have to finish collapsing - so unlike a snow field this cannot go idle
	 * the instant nothing is pressing on it.
	 */
	float SettleTimeRemaining = 0.0f;
};
