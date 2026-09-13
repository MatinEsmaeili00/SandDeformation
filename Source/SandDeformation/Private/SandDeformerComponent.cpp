// Copyright Matin. All Rights Reserved.

#include "SandDeformerComponent.h"

#include "CollisionQueryParams.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "SandDeformation.h"
#include "SandDeformationSubsystem.h"

USandDeformerComponent::USandDeformerComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	bAutoActivate = true;
}

void USandDeformerComponent::BeginPlay()
{
	Super::BeginPlay();

	if (UWorld* World = GetWorld())
	{
		if (USandDeformationSubsystem* Subsystem = World->GetSubsystem<USandDeformationSubsystem>())
		{
			Subsystem->RegisterDeformer(this);
		}
	}

	// Landing is an event, not a state we can sample reliably: a short fall
	// can begin and end between two of our polls. ACharacter already detects
	// it exactly, so bind rather than re-derive it.
	if (ACharacter* Character = Cast<ACharacter>(GetOwner()))
	{
		Character->LandedDelegate.AddDynamic(this, &USandDeformerComponent::HandleLanded);
	}
}

void USandDeformerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ACharacter* Character = Cast<ACharacter>(GetOwner()))
	{
		Character->LandedDelegate.RemoveDynamic(this, &USandDeformerComponent::HandleLanded);
	}

	if (UWorld* World = GetWorld())
	{
		if (USandDeformationSubsystem* Subsystem = World->GetSubsystem<USandDeformationSubsystem>())
		{
			Subsystem->UnregisterDeformer(this);
		}
	}

	Super::EndPlay(EndPlayReason);
}

USkeletalMeshComponent* USandDeformerComponent::FindSkeletalMesh() const
{
	const AActor* Owner = GetOwner();
	return Owner ? Owner->FindComponentByClass<USkeletalMeshComponent>() : nullptr;
}

// ---------------------------------------------------------------------------
// Impacts
// ---------------------------------------------------------------------------

void USandDeformerComponent::HandleLanded(const FHitResult& Hit)
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// ACharacter::Landed fires after the velocity has been zeroed, so the fall
	// speed has to come from the movement component's last update rather than
	// from GetVelocity().
	float FallSpeed = 0.0f;
	if (const ACharacter* Character = Cast<ACharacter>(Owner))
	{
		if (const UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
		{
			FallSpeed = FMath::Abs(Movement->GetLastUpdateVelocity().Z);
		}
	}

	const float Range = FMath::Max(MaxLandingSpeed - MinLandingSpeed, 1.0f);
	const float Strength = FMath::Clamp((FallSpeed - MinLandingSpeed) / Range, 0.0f, 1.0f);
	if (Strength <= 0.0f)
	{
		return;   // stepped down a kerb; not worth a crater
	}

	FireImpact(Hit.ImpactPoint, Strength, /*bWasLanding=*/true);
}

void USandDeformerComponent::TriggerSandImpact(FVector WorldLocation, float Strength, bool bWasLanding)
{
	FireImpact(WorldLocation, FMath::Clamp(Strength, 0.0f, 1.0f), bWasLanding);
}

void USandDeformerComponent::FireImpact(const FVector& WorldLocation, float Strength, bool bWasLanding)
{
	if (Strength <= 0.0f)
	{
		return;
	}

	FSandDeformationContact Impact;
	Impact.WorldLocation = WorldLocation;
	Impact.Radius = ImpactRadius;
	Impact.Depth = ImpactDepth;
	Impact.RimHeight = ImpactRimHeight;
	Impact.RimWidth = ImpactRimWidth;
	Impact.RippleImpulse = ImpactRippleImpulse;
	Impact.Strength = Strength;
	PendingImpacts.Add(Impact);

	OnSandImpact.Broadcast(WorldLocation, Strength, bWasLanding);

	if (!ImpactEffect.IsNull())
	{
		if (UNiagaraSystem* Effect = ImpactEffect.LoadSynchronous())
		{
			const float Scale = ImpactEffectScale * FMath::Lerp(0.5f, 1.0f, Strength);
			UNiagaraComponent* Spawned = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
				this, Effect, WorldLocation, FRotator::ZeroRotator, FVector(Scale), /*bAutoDestroy=*/true);

			if (Spawned)
			{
				// Harmless if the system doesn't declare these - Niagara just
				// ignores a parameter that isn't there.
				Spawned->SetVariableFloat(TEXT("User.ImpactStrength"), Strength);
				Spawned->SetVariableFloat(TEXT("User.IsLanding"), bWasLanding ? 1.0f : 0.0f);
			}
		}
	}

	UE_LOG(LogSandDeformation, Verbose, TEXT("Sand impact at %s, strength %.2f, landing %d"),
		*WorldLocation.ToCompactString(), Strength, bWasLanding ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Per-frame contacts
// ---------------------------------------------------------------------------

void USandDeformerComponent::UpdateAndGatherContacts(float DeltaTime, TArray<FSandDeformationContact>& OutContacts)
{
	AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!Owner || !World)
	{
		return;
	}

	// Impacts banked since the last pull (the landing delegate fires whenever
	// the character movement component says so, not on our schedule).
	OutContacts.Append(PendingImpacts);
	PendingImpacts.Reset();

	// --- Jump take-off ----------------------------------------------------
	// Landing has a delegate; pushing off doesn't, so watch for the frame the
	// character leaves the ground moving upward.
	if (const ACharacter* Character = Cast<ACharacter>(Owner))
	{
		if (const UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
		{
			const bool bFalling = Movement->IsFalling();
			if (bFalling && !bWasFalling && Movement->GetLastUpdateVelocity().Z > 0.0f)
			{
				FireImpact(Owner->GetActorLocation() - FVector(0.0f, 0.0f, Character->GetSimpleCollisionHalfHeight()),
					JumpImpactScale, /*bWasLanding=*/false);

				// Fired mid-gather, so fold it in rather than waiting a frame.
				OutContacts.Append(PendingImpacts);
				PendingImpacts.Reset();
			}
			bWasFalling = bFalling;
		}
	}

	// --- Planted feet -----------------------------------------------------
	const float Speed = Owner->GetVelocity().Size2D();
	const float SpeedAlpha = MinSpeedForFullStrength > KINDA_SMALL_NUMBER
		? FMath::Clamp(Speed / MinSpeedForFullStrength, 0.0f, 1.0f)
		: 1.0f;
	// Never fully fade out: a planted foot should still leave a mark even
	// while standing still, just a lighter one than a running stride.
	const float Strength = FMath::Max(SpeedAlpha, 0.35f);

	USkeletalMeshComponent* SkelMesh = FindSkeletalMesh();
	bool bAnyFootContact = false;

	// Sockets planted this frame, so the set can be swapped in at the end and
	// any foot that lifted is dropped without a second pass.
	TSet<FName> PlantedThisFrame;

	if (SkelMesh)
	{
		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(SandDeformerFootTrace), /*bTraceComplex=*/false, Owner);
		QueryParams.AddIgnoredActor(Owner);

		for (const FName& Socket : FootSocketNames)
		{
			if (!SkelMesh->DoesSocketExist(Socket))
			{
				continue;
			}

			const FVector SocketLoc = SkelMesh->GetSocketLocation(Socket);
			const FVector TraceEnd = SocketLoc - FVector(0.0f, 0.0f, TraceDownDistance);

			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, SocketLoc, TraceEnd, TraceChannel, QueryParams))
			{
				if (SocketLoc.Z - Hit.Location.Z <= ContactHeight)
				{
					bAnyFootContact = true;
					PlantedThisFrame.Add(Socket);

					// Only the frame the foot first touches down kicks the wave
					// field. A planted foot stays planted for many frames, and
					// re-injecting each one drives the wave to a steady state of
					// roughly impulse/(damping*dt) - so standing still would ring
					// harder than a landing.
					const bool bJustPlanted = !PlantedSockets.Contains(Socket);

					FSandDeformationContact Contact;
					Contact.WorldLocation = Hit.Location;
					Contact.Radius = FootprintRadius;
					Contact.Depth = FootprintDepth;
					Contact.RimHeight = FootprintRimHeight;
					Contact.RimWidth = FootprintRimWidth;
					Contact.RippleImpulse = bJustPlanted ? FootprintRippleImpulse * Strength : 0.0f;
					Contact.Strength = Strength;
					OutContacts.Add(Contact);
				}
			}
		}
	}

	if (!bAnyFootContact && SimpleContactFallbackRadius > 0.0f)
	{
		FVector Origin, BoxExtent;
		Owner->GetActorBounds(false, Origin, BoxExtent);

		FSandDeformationContact Contact;
		Contact.WorldLocation = FVector(Origin.X, Origin.Y, Origin.Z - BoxExtent.Z);
		Contact.Radius = SimpleContactFallbackRadius;
		Contact.Depth = FootprintDepth;
		Contact.RimHeight = FootprintRimHeight;
		Contact.RimWidth = FootprintRimWidth;
		// Same rising-edge rule as the skeletal path.
		Contact.RippleImpulse = bFallbackWasPlanted ? 0.0f : FootprintRippleImpulse * Strength;
		Contact.Strength = Strength;
		OutContacts.Add(Contact);

		bFallbackWasPlanted = true;
	}
	else
	{
		bFallbackWasPlanted = false;
	}

	PlantedSockets = MoveTemp(PlantedThisFrame);
}
