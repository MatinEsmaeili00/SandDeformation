// Copyright Matin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SandSurfaceComponent.generated.h"

class UMaterialInstanceDynamic;

/**
 * Put this on the actor whose mesh *is* the sand.
 *
 * It swaps that mesh's materials for dynamic instances at BeginPlay and hands
 * them to the subsystem, which then keeps their SandData / SandRegion
 * parameters current every frame. That is the whole of the wiring: the
 * material needs a texture parameter named SandData and a vector parameter
 * named SandRegion (names are configurable in Project Settings), and nothing
 * else has to be assigned anywhere.
 *
 * Use this when you'd rather not point the plugin at a render-target asset in
 * Project Settings - it works against the transient target the simulation
 * creates for itself.
 */
UCLASS(ClassGroup = (SandDeformation), meta = (BlueprintSpawnableComponent))
class SANDDEFORMATION_API USandSurfaceComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USandSurfaceComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/**
	 * Only these material slots are converted. Leave empty to convert every
	 * slot on every primitive the owner has.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation")
	TArray<int32> MaterialSlots;

private:
	/** Held strongly: these instances exist only because we made them. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> CreatedMaterials;
};
