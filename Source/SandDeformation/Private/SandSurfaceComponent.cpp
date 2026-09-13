// Copyright Matin. All Rights Reserved.

#include "SandSurfaceComponent.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "SandDeformation.h"
#include "SandDeformationSubsystem.h"

USandSurfaceComponent::USandSurfaceComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	bAutoActivate = true;
}

void USandSurfaceComponent::BeginPlay()
{
	Super::BeginPlay();

	AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!Owner || !World)
	{
		return;
	}

	USandDeformationSubsystem* Subsystem = World->GetSubsystem<USandDeformationSubsystem>();
	if (!Subsystem)
	{
		return;
	}

	TArray<UPrimitiveComponent*> Primitives;
	Owner->GetComponents<UPrimitiveComponent>(Primitives);

	for (UPrimitiveComponent* Primitive : Primitives)
	{
		const int32 NumSlots = Primitive->GetNumMaterials();
		for (int32 Slot = 0; Slot < NumSlots; ++Slot)
		{
			if (MaterialSlots.Num() > 0 && !MaterialSlots.Contains(Slot))
			{
				continue;
			}

			if (UMaterialInstanceDynamic* Dynamic = Primitive->CreateAndSetMaterialInstanceDynamic(Slot))
			{
				Subsystem->RegisterSandMaterial(Dynamic);
				CreatedMaterials.Add(Dynamic);
			}
		}
	}

	if (CreatedMaterials.Num() == 0)
	{
		UE_LOG(LogSandDeformation, Warning,
			TEXT("%s has a Sand Surface component but no material slots were converted. ")
			TEXT("The actor needs a mesh with at least one material for the sand texture to land anywhere."),
			*Owner->GetName());
	}
	else
	{
		UE_LOG(LogSandDeformation, Log,
			TEXT("Sand surface %s: %d material instance(s) now tracking the simulation."),
			*Owner->GetName(), CreatedMaterials.Num());
	}
}

void USandSurfaceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (const UWorld* World = GetWorld())
	{
		if (USandDeformationSubsystem* Subsystem = World->GetSubsystem<USandDeformationSubsystem>())
		{
			for (UMaterialInstanceDynamic* Dynamic : CreatedMaterials)
			{
				Subsystem->UnregisterSandMaterial(Dynamic);
			}
		}
	}
	CreatedMaterials.Reset();

	Super::EndPlay(EndPlayReason);
}
