// Copyright Matin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "SandDeformerComponent.generated.h"

class UNiagaraSystem;
class USkeletalMeshComponent;

/**
 * One world-space press into the sand, produced by a component this frame.
 * The subsystem converts these into GPU-ready FSandDeformerGPU entries once
 * it knows where the simulated region is currently centred.
 *
 * Depth and RimHeight are WORLD UNITS, not a normalised 0..1: the slump
 * solver compares real slopes against a real angle of repose, so the sizes
 * here have to be real too.
 */
USTRUCT(BlueprintType)
struct FSandDeformationContact
{
	GENERATED_BODY()

	/** Where the press lands, in world space. Only X/Y matter to the simulation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation")
	FVector WorldLocation = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation")
	float Radius = 24.0f;

	/** World units pressed down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation")
	float Depth = 6.0f;

	/** World units of sand piled outside the core. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation")
	float RimHeight = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation")
	float RimWidth = 10.0f;

	/** World units/second injected into the wave field. This is what makes a ring spread outwards. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation")
	float RippleImpulse = 0.0f;

	/** 0..1 master multiplier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation")
	float Strength = 1.0f;
};

/** Broadcast when this actor hits the sand hard enough to throw grains. Strength is 0..1. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FSandImpactSignature, FVector, Location, float, Strength, bool, bWasLanding);

/**
 * Attach to a character (or anything else that should disturb the sand).
 *
 * Two things happen here:
 *
 *   - Continuous contact. Every frame the subsystem asks for the feet that
 *     are currently planted, by tracing down from each name in
 *     FootSocketNames. There is no ticking on this component; the subsystem
 *     pulls, so an idle component costs nothing.
 *
 *   - Impacts. Landing from a fall, and pushing off into a jump, are
 *     discrete events rather than continuous contact. They dig a wider
 *     crater, kick the ripple field so a ring spreads outwards, and fire
 *     OnSandImpact / spawn a Niagara burst.
 */
UCLASS(ClassGroup = (SandDeformation), meta = (BlueprintSpawnableComponent))
class SANDDEFORMATION_API USandDeformerComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USandDeformerComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/**
	 * Advances this component's own state (jump/landing edge detection) and
	 * appends the presses active this frame. Called by the subsystem.
	 */
	void UpdateAndGatherContacts(float DeltaTime, TArray<FSandDeformationContact>& OutContacts);

	/** Fired when this actor lands in or pushes off the sand hard enough to matter. */
	UPROPERTY(BlueprintAssignable, Category = "Sand Deformation|Impact")
	FSandImpactSignature OnSandImpact;

	/** Triggers an impact by hand - a thrown prop, an explosion, a landing you detected yourself. */
	UFUNCTION(BlueprintCallable, Category = "Sand Deformation|Impact")
	void TriggerSandImpact(FVector WorldLocation, float Strength, bool bWasLanding = true);

	// --- Footfalls --------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Footfall")
	TArray<FName> FootSocketNames = { TEXT("foot_l"), TEXT("foot_r") };

	/** How far below each foot socket to trace looking for ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Footfall", meta = (ClampMin = "1.0"))
	float TraceDownDistance = 60.0f;

	/**
	 * A foot counts as planted when the ground hit is within this many units
	 * of the socket. Foot BONES sit at the ankle, usually 10-15 units above
	 * the sole, so this has to be comfortably larger than that or the foot is
	 * never considered planted on flat ground.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Footfall", meta = (ClampMin = "0.0"))
	float ContactHeight = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Footfall")
	float FootprintRadius = 22.0f;

	/** World units a footfall presses down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Footfall")
	float FootprintDepth = 5.0f;

	/** World units of sand pushed up around a footfall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Footfall")
	float FootprintRimHeight = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Footfall")
	float FootprintRimWidth = 10.0f;

	/** Small wave kick per footfall, so walking leaves faint ripples. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Footfall")
	float FootprintRippleImpulse = 12.0f;

	/** Horizontal speed (cm/s) at which footfalls reach full strength. Below this they fade but never vanish. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Footfall", meta = (ClampMin = "1.0"))
	float MinSpeedForFullStrength = 80.0f;

	/** Used only when no skeletal mesh with the named sockets is found. 0 disables the fallback. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Footfall", meta = (ClampMin = "0.0"))
	float SimpleContactFallbackRadius = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Footfall")
	TEnumAsByte<ECollisionChannel> TraceChannel = ECC_WorldStatic;

	// --- Impacts ----------------------------------------------------------

	/** Downward speed (cm/s) at which a landing starts to register at all. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Impact", meta = (ClampMin = "0.0"))
	float MinLandingSpeed = 250.0f;

	/** Downward speed (cm/s) at which a landing is at full strength. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Impact", meta = (ClampMin = "1.0"))
	float MaxLandingSpeed = 1200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Impact")
	float ImpactRadius = 70.0f;

	/** World units a full-strength landing digs out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Impact")
	float ImpactDepth = 14.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Impact")
	float ImpactRimHeight = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Impact")
	float ImpactRimWidth = 45.0f;

	/** Wave kick from a full-strength landing. This is the ring that spreads out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Impact")
	float ImpactRippleImpulse = 260.0f;

	/** Relative size of the kick-off burst when the character jumps, against a full landing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Impact", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float JumpImpactScale = 0.45f;

	// --- Particles --------------------------------------------------------

	/**
	 * Optional burst spawned at every impact. Soft, so a project that doesn't
	 * use it never loads it. The spawned component gets two float parameters
	 * set on it, if the system declares them as User parameters:
	 *   User.ImpactStrength  0..1
	 *   User.IsLanding       1 on landing, 0 on jump take-off
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Particles")
	TSoftObjectPtr<UNiagaraSystem> ImpactEffect;

	/** Effect scale at full strength. Weaker impacts scale down proportionally. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sand Deformation|Particles", meta = (ClampMin = "0.0"))
	float ImpactEffectScale = 1.0f;

private:
	UFUNCTION()
	void HandleLanded(const FHitResult& Hit);

	USkeletalMeshComponent* FindSkeletalMesh() const;

	/** Queues the crater + ripple kick, fires the delegate, spawns the burst. */
	void FireImpact(const FVector& WorldLocation, float Strength, bool bWasLanding);

	/** Rising edge of "is falling" with upward velocity = the character just jumped. */
	bool bWasFalling = false;

	/** Landings arrive on a delegate, which can fire outside the subsystem's pull. Banked until then. */
	TArray<FSandDeformationContact> PendingImpacts;
};
