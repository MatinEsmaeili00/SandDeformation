// Copyright Matin. All Rights Reserved.

#include "SandDeformationSubsystem.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollection.h"
#include "SandDeformation.h"
#include "SandDeformationSettings.h"
#include "SandDeformerComponent.h"
#include "TextureResource.h"

void USandDeformationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const USandDeformationSettings& Settings = USandDeformationSettings::Get();

	TextureResolution    = Settings.TextureResolution;
	RegionSizeWorld      = Settings.RegionSizeWorld;
	AngleOfReposeDegrees = Settings.AngleOfReposeDegrees;
	SlumpRate            = Settings.SlumpRate;
	RippleSpeed          = Settings.RippleSpeed;
	RippleDamping        = Settings.RippleDamping;
	DisturbanceDecay     = Settings.DisturbanceDecay;
	HeightRestoreRate    = Settings.HeightRestoreRate;
	NormalStrength       = Settings.NormalStrength;
	HeightScale          = Settings.HeightScale;

	RegionParameterName   = Settings.RegionParameterName;
	SandDataParameterName = Settings.SandDataParameterName;

	// Both wiring assets are soft references, so a project that doesn't use
	// them never pays to load them. Resolving and validating here rather than
	// per frame is also what keeps a mis-configured asset to one warning.
	if (!Settings.SandDataRenderTarget.IsNull())
	{
		ConfiguredOutputAsset = Settings.SandDataRenderTarget.LoadSynchronous();
		if (ConfiguredOutputAsset)
		{
			PrepareOutputAsset(ConfiguredOutputAsset);
		}
	}

	if (!Settings.ParameterCollection.IsNull())
	{
		ParameterCollection = Settings.ParameterCollection.LoadSynchronous();
		if (ParameterCollection)
		{
			bRegionParameterValid = ParameterCollection->GetVectorParameterByName(RegionParameterName) != nullptr;
			if (!bRegionParameterValid)
			{
				UE_LOG(LogSandDeformation, Warning,
					TEXT("Material Parameter Collection %s has no vector parameter named %s. Add one ")
					TEXT("(RG = region centre XY, B = region size, A = height scale), or clear the collection ")
					TEXT("reference in Project Settings > Plugins > Sand Deformation."),
					*ParameterCollection->GetName(), *RegionParameterName.ToString());
			}
		}
	}
}

void USandDeformationSubsystem::Deinitialize()
{
	RegisteredDeformers.Reset();
	RegisteredMaterials.Reset();
	PendingOneShotDeformers.Reset();
	ReleaseRenderTargets();
	ConfiguredOutputAsset = nullptr;
	ParameterCollection = nullptr;

	Super::Deinitialize();
}

TStatId USandDeformationSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USandDeformationSubsystem, STATGROUP_Tickables);
}

bool USandDeformationSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void USandDeformationSubsystem::RegisterDeformer(USandDeformerComponent* Component)
{
	if (Component)
	{
		RegisteredDeformers.AddUnique(Component);
	}
}

void USandDeformationSubsystem::UnregisterDeformer(USandDeformerComponent* Component)
{
	RegisteredDeformers.RemoveAll([Component](const TWeakObjectPtr<USandDeformerComponent>& WeakComponent)
	{
		return WeakComponent.Get() == Component;
	});
}

void USandDeformationSubsystem::StampSandAt(FVector WorldLocation, float Radius, float Depth, float RimHeight, float RimWidth, float Strength)
{
	AddSandImpact(WorldLocation, Radius, Depth, /*RippleImpulse=*/0.0f, RimHeight, RimWidth, Strength);
}

void USandDeformationSubsystem::AddSandImpact(FVector WorldLocation, float Radius, float Depth, float RippleImpulse, float RimHeight, float RimWidth, float Strength)
{
	FSandDeformerGPU Deformer;
	// Banked in world space: the region may still move before the next Tick,
	// which is where this gets rebased into region-local space.
	Deformer.LocalCenter = FVector2f(FVector2D(WorldLocation));
	Deformer.Radius = Radius;
	Deformer.Depth = Depth;
	Deformer.RimHeight = RimHeight;
	Deformer.RimWidth = RimWidth;
	Deformer.RippleImpulse = RippleImpulse;
	Deformer.Strength = Strength;
	PendingOneShotDeformers.Add(Deformer);
}

void USandDeformationSubsystem::SetFocusActor(AActor* NewFocusActor)
{
	FocusActor = NewFocusActor;
}

AActor* USandDeformationSubsystem::ResolveFocusActor() const
{
	if (AActor* Focus = FocusActor.Get())
	{
		return Focus;
	}

	if (const UWorld* World = GetWorld())
	{
		return UGameplayStatics::GetPlayerPawn(World, 0);
	}

	return nullptr;
}

// ---------------------------------------------------------------------------
// Material wiring
// ---------------------------------------------------------------------------

FLinearColor USandDeformationSubsystem::GetSandRegionParameter() const
{
	return FLinearColor(
		static_cast<float>(RegionCenterWorld.X),
		static_cast<float>(RegionCenterWorld.Y),
		RegionSizeWorld,
		HeightScale);
}

void USandDeformationSubsystem::ApplySandParametersToMaterial(UMaterialInstanceDynamic* Material) const
{
	if (!Material)
	{
		return;
	}

	if (SandDataRenderTarget)
	{
		Material->SetTextureParameterValue(SandDataParameterName, SandDataRenderTarget);
	}
	Material->SetVectorParameterValue(RegionParameterName, GetSandRegionParameter());
}

void USandDeformationSubsystem::RegisterSandMaterial(UMaterialInstanceDynamic* Material)
{
	if (Material)
	{
		RegisteredMaterials.AddUnique(Material);
		ApplySandParametersToMaterial(Material);
	}
}

void USandDeformationSubsystem::UnregisterSandMaterial(UMaterialInstanceDynamic* Material)
{
	RegisteredMaterials.RemoveAll([Material](const TWeakObjectPtr<UMaterialInstanceDynamic>& WeakMaterial)
	{
		return WeakMaterial.Get() == Material;
	});
}

void USandDeformationSubsystem::PushMaterialParameters()
{
	if (ParameterCollection && bRegionParameterValid)
	{
		UKismetMaterialLibrary::SetVectorParameterValue(this, ParameterCollection, RegionParameterName, GetSandRegionParameter());
	}

	for (auto It = RegisteredMaterials.CreateIterator(); It; ++It)
	{
		if (UMaterialInstanceDynamic* Material = It->Get())
		{
			ApplySandParametersToMaterial(Material);
		}
		else
		{
			It.RemoveCurrent();
		}
	}
}

// ---------------------------------------------------------------------------
// Render targets
// ---------------------------------------------------------------------------

void USandDeformationSubsystem::PrepareOutputAsset(UTextureRenderTarget2D* Asset)
{
	check(Asset);

	if (Asset->RenderTargetFormat != RTF_RGBA16f)
	{
		UE_LOG(LogSandDeformation, Warning,
			TEXT("Sand output render target %s is not RGBA16f. Heights are signed world units - sand piled ")
			TEXT("above rest is positive, dug out is negative - so a fixed-point format clips half of it away. ")
			TEXT("Set Render Target Format to RGBA16f on the asset."),
			*Asset->GetName());
	}

	if (!Asset->bCanCreateUAV)
	{
		UE_LOG(LogSandDeformation, Warning,
			TEXT("Sand output render target %s did not have Can Create UAV set; enabling it for this ")
			TEXT("session. Tick it on the asset to avoid the runtime fixup."),
			*Asset->GetName());
		Asset->bCanCreateUAV = true;
		Asset->UpdateResource();
	}
}

void USandDeformationSubsystem::ReleaseRenderTargets()
{
	StateRenderTargets[0] = nullptr;
	StateRenderTargets[1] = nullptr;
	SandDataRenderTarget = nullptr;
	bUsingExternalOutput = false;
	bTargetsInitialised = false;
	bFirstFrame = true;
}

void USandDeformationSubsystem::EnsureRenderTargets()
{
	UTextureRenderTarget2D* OutputAsset = ConfiguredOutputAsset;
	if (OutputAsset)
	{
		TextureResolution = FIntPoint(OutputAsset->SizeX, OutputAsset->SizeY);
	}

	TextureResolution.X = FMath::Max(TextureResolution.X, 1);
	TextureResolution.Y = FMath::Max(TextureResolution.Y, 1);

	const bool bMatchesRequest =
		bTargetsInitialised
		&& StateRenderTargets[0] && StateRenderTargets[1] && SandDataRenderTarget
		&& bUsingExternalOutput == (OutputAsset != nullptr)
		&& (!OutputAsset || SandDataRenderTarget == OutputAsset)
		&& StateRenderTargets[0]->SizeX == TextureResolution.X
		&& StateRenderTargets[0]->SizeY == TextureResolution.Y;

	if (bMatchesRequest)
	{
		return;
	}

	auto MakeRenderTarget = [this](ETextureRenderTargetFormat Format) -> UTextureRenderTarget2D*
	{
		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(this);
		RT->RenderTargetFormat = Format;
		RT->ClearColor = FLinearColor::Black;
		RT->bAutoGenerateMips = false;
		RT->bCanCreateUAV = true;
		RT->InitAutoFormat(TextureResolution.X, TextureResolution.Y);
		RT->UpdateResourceImmediate(true);
		return RT;
	};

	// Full float for the simulation state. Half would be tempting for
	// bandwidth, but ripple velocity is integrated across frames and half's
	// precision loss accumulates into visible drift within seconds.
	StateRenderTargets[0] = MakeRenderTarget(RTF_RGBA32f);
	StateRenderTargets[1] = MakeRenderTarget(RTF_RGBA32f);

	if (OutputAsset)
	{
		SandDataRenderTarget = OutputAsset;
		bUsingExternalOutput = true;
	}
	else
	{
		SandDataRenderTarget = MakeRenderTarget(RTF_RGBA16f);
		bUsingExternalOutput = false;
	}

	CurrentStateIndex = 0;
	bFirstFrame = true;
	bTargetsInitialised = true;

	UE_LOG(LogSandDeformation, Log,
		TEXT("Sand surface allocated at %dx%d over %.0f world units; output target: %s."),
		TextureResolution.X, TextureResolution.Y, RegionSizeWorld,
		bUsingExternalOutput ? *SandDataRenderTarget->GetName() : TEXT("transient"));
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void USandDeformationSubsystem::Tick(float DeltaTime)
{
	if (!IsInitialized())
	{
		return;
	}

	EnsureRenderTargets();
	if (!bTargetsInitialised)
	{
		return;
	}

	const AActor* Focus = ResolveFocusActor();
	const FVector2D NewRegionCenter = Focus ? FVector2D(Focus->GetActorLocation()) : RegionCenterWorld;

	TArray<FSandDeformationContact> Contacts;
	for (auto It = RegisteredDeformers.CreateIterator(); It; ++It)
	{
		if (USandDeformerComponent* Component = It->Get())
		{
			Component->UpdateAndGatherContacts(DeltaTime, Contacts);
		}
		else
		{
			It.RemoveCurrent();
		}
	}

	const int32 NumPresses = Contacts.Num() + PendingOneShotDeformers.Num();

	// Sand keeps moving after you stop touching it: ripples ring out and
	// over-steep slopes finish collapsing. So the settle window is set from
	// how long a wave takes to damp to roughly 1% of its amplitude
	// (exp(-damping * t) = 0.01), with a floor for the slump to finish.
	if (NumPresses > 0)
	{
		const float RippleSettle = 4.6f / FMath::Max(RippleDamping, 0.1f);
		SettleTimeRemaining = FMath::Max(RippleSettle, 2.0f);
	}
	else
	{
		SettleTimeRemaining = FMath::Max(SettleTimeRemaining - DeltaTime, 0.0f);
	}

	// Skipping is only safe while the region is stationary: the texture is
	// anchored to RegionCenterWorld, so a move has to be reprojected or every
	// existing mark would slide along with the player.
	const bool bRegionMoved = !NewRegionCenter.Equals(RegionCenterWorld, 0.01);
	const bool bNeedsDispatch = bFirstFrame || bRegionMoved || NumPresses > 0 || SettleTimeRemaining > 0.0f;

	if (!bNeedsDispatch)
	{
		PushMaterialParameters();
		return;
	}

	FTextureRenderTargetResource* PrevResource = StateRenderTargets[CurrentStateIndex]->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* NextResource = StateRenderTargets[1 - CurrentStateIndex]->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* DataResource = SandDataRenderTarget->GameThread_GetRenderTargetResource();
	if (!PrevResource || !NextResource || !DataResource)
	{
		// Resources are created on the render thread; nothing to do until they
		// land. Checked before any state is advanced so no motion is lost.
		return;
	}

	const FVector2D OffsetFromPrev = NewRegionCenter - RegionCenterWorld;
	RegionCenterWorld = NewRegionCenter;

	TArray<FSandDeformerGPU> GPUDeformers;
	GPUDeformers.Reserve(NumPresses);
	for (const FSandDeformationContact& Contact : Contacts)
	{
		FSandDeformerGPU Deformer;
		Deformer.LocalCenter = FVector2f(FVector2D(Contact.WorldLocation) - RegionCenterWorld);
		Deformer.Radius = Contact.Radius;
		Deformer.Depth = Contact.Depth;
		Deformer.RimHeight = Contact.RimHeight;
		Deformer.RimWidth = Contact.RimWidth;
		Deformer.RippleImpulse = Contact.RippleImpulse;
		Deformer.Strength = Contact.Strength;
		GPUDeformers.Add(Deformer);
	}

	const FVector2f RegionCenterFloat(RegionCenterWorld);
	for (FSandDeformerGPU& OneShot : PendingOneShotDeformers)
	{
		OneShot.LocalCenter -= RegionCenterFloat;
		GPUDeformers.Add(OneShot);
	}
	PendingOneShotDeformers.Reset();

	FSandDeformationDispatchParams Params;
	Params.TextureSize = TextureResolution;
	Params.RegionSize = RegionSizeWorld;
	Params.RegionOffsetFromPrev = FVector2f(OffsetFromPrev);
	Params.DeltaTime = DeltaTime;
	Params.ClearMask = bFirstFrame ? 0.0f : 1.0f;
	Params.TanAngleOfRepose = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(AngleOfReposeDegrees, 1.0f, 80.0f)));
	Params.SlumpRate = SlumpRate;
	Params.RippleSpeed = RippleSpeed;
	Params.RippleDamping = RippleDamping;
	Params.DisturbanceDecay = DisturbanceDecay;
	Params.HeightRestoreRate = HeightRestoreRate;
	Params.NormalStrength = NormalStrength;
	Params.Deformers = MoveTemp(GPUDeformers);

	// The RHI textures are resolved on the render thread rather than here: a
	// resource can swap the texture underneath a game-thread read (a resize, a
	// device reset) between now and when the command actually runs.
	ENQUEUE_RENDER_COMMAND(SandDeformationDispatch)(
		[Params, PrevResource, NextResource, DataResource](FRHICommandListImmediate& RHICmdList) mutable
		{
			Params.PrevStateTexture = PrevResource->GetRenderTargetTexture();
			Params.NextStateTexture = NextResource->GetRenderTargetTexture();
			Params.SandDataTexture  = DataResource->GetRenderTargetTexture();

			SandDeformation::Dispatch_RenderThread(RHICmdList, Params);
		});

	CurrentStateIndex = 1 - CurrentStateIndex;
	bFirstFrame = false;

	PushMaterialParameters();
}
