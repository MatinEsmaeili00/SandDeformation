// Copyright Matin. All Rights Reserved.
//
// Sand deformation compute passes.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

class FRHICommandListImmediate;
class FRHITexture;

namespace SandDeformation
{
	/** Compute thread group size. Shared so the HLSL defines and the dispatch group count can never drift apart. */
	static constexpr int32 ThreadGroupSize = 8;
}

/**
 * One thing pressing into the sand this frame. Layout must match
 * FSandDeformerGPU in SandDeformation.usf - all floats, tightly packed.
 *
 * Unlike the snow equivalent, Depth and RimHeight are in WORLD UNITS rather
 * than a normalised 0..1, because the slump solver compares real slopes
 * against a real angle of repose.
 */
struct FSandDeformerGPU
{
	FVector2f	LocalCenter = FVector2f::ZeroVector;
	float		Radius = 24.0f;
	float		Depth = 6.0f;
	float		RimHeight = 2.0f;
	float		RimWidth = 10.0f;
	float		RippleImpulse = 0.0f;
	float		Strength = 1.0f;
};
static_assert(sizeof(FSandDeformerGPU) == 32, "FSandDeformerGPU must stay tightly packed to match the HLSL struct");

/** Everything the render thread needs for one frame of sand simulation. */
struct FSandDeformationDispatchParams
{
	FRHITexture*	PrevStateTexture = nullptr;
	FRHITexture*	NextStateTexture = nullptr;
	FRHITexture*	SandDataTexture = nullptr;

	FIntPoint		TextureSize = FIntPoint(1024, 1024);
	float			RegionSize = 4096.0f;
	FVector2f		RegionOffsetFromPrev = FVector2f::ZeroVector;
	float			DeltaTime = 0.0f;
	float			ClearMask = 1.0f;

	float			TanAngleOfRepose = 0.675f;	// tan(34 degrees)
	float			SlumpRate = 6.0f;
	float			RippleSpeed = 120.0f;
	float			RippleDamping = 1.5f;
	float			DisturbanceDecay = 0.8f;
	float			HeightRestoreRate = 0.0f;
	float			NormalStrength = 1.0f;

	TArray<FSandDeformerGPU> Deformers;
};

// ---------------------------------------------------------------------------
// Pass 1: reproject + slump + ripple + stamp
// ---------------------------------------------------------------------------

BEGIN_SHADER_PARAMETER_STRUCT(FSandSimulateParams, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, PrevStateTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, PrevStateSampler)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutStateTexture)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FSandDeformerGPU>, Deformers)
	SHADER_PARAMETER(FIntPoint, TextureSize)
	SHADER_PARAMETER(float, RegionSize)
	SHADER_PARAMETER(FVector2f, RegionOffsetFromPrev)
	SHADER_PARAMETER(int32, NumDeformers)
	SHADER_PARAMETER(float, DeltaTime)
	SHADER_PARAMETER(float, ClearMask)
	SHADER_PARAMETER(float, TanAngleOfRepose)
	SHADER_PARAMETER(float, SlumpRate)
	SHADER_PARAMETER(float, RippleSpeed)
	SHADER_PARAMETER(float, RippleDamping)
	SHADER_PARAMETER(float, DisturbanceDecay)
	SHADER_PARAMETER(float, HeightRestoreRate)
END_SHADER_PARAMETER_STRUCT()

class FSandSimulateCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSandSimulateCS);
	SHADER_USE_PARAMETER_STRUCT(FSandSimulateCS, FGlobalShader);

	using FParameters = FSandSimulateParams;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		SET_SHADER_DEFINE(OutEnvironment, THREADS_X, SandDeformation::ThreadGroupSize);
		SET_SHADER_DEFINE(OutEnvironment, THREADS_Y, SandDeformation::ThreadGroupSize);
	}
};

// ---------------------------------------------------------------------------
// Pass 2: state -> packed height + normal + disturbance texture
// ---------------------------------------------------------------------------

BEGIN_SHADER_PARAMETER_STRUCT(FSandNormalsParams, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, StateTexture)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutSandDataTexture)
	SHADER_PARAMETER(FIntPoint, TextureSize)
	SHADER_PARAMETER(float, RegionSize)
	SHADER_PARAMETER(float, NormalStrength)
END_SHADER_PARAMETER_STRUCT()

class FSandNormalsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSandNormalsCS);
	SHADER_USE_PARAMETER_STRUCT(FSandNormalsCS, FGlobalShader);

	using FParameters = FSandNormalsParams;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		SET_SHADER_DEFINE(OutEnvironment, THREADS_X, SandDeformation::ThreadGroupSize);
		SET_SHADER_DEFINE(OutEnvironment, THREADS_Y, SandDeformation::ThreadGroupSize);
	}
};

namespace SandDeformation
{
	/** Builds and executes the render graph for one simulation step. Render thread only. */
	SANDDEFORMATION_API void Dispatch_RenderThread(FRHICommandListImmediate& RHICmdList, const FSandDeformationDispatchParams& Params);
}
