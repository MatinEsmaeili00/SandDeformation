// Copyright Matin. All Rights Reserved.

#include "SandDeformationComputePass.h"

#include "PooledRenderTarget.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIStaticStates.h"

IMPLEMENT_GLOBAL_SHADER(FSandSimulateCS, "/SandDeformationShaders/Private/SandDeformation.usf", "SandSimulateCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSandNormalsCS, "/SandDeformationShaders/Private/SandDeformation.usf", "SandNormalsCS", SF_Compute);

void SandDeformation::Dispatch_RenderThread(FRHICommandListImmediate& RHICmdList, const FSandDeformationDispatchParams& Params)
{
	check(IsInRenderingThread());

	if (!Params.PrevStateTexture || !Params.NextStateTexture || !Params.SandDataTexture)
	{
		return;
	}

	FRDGBuilder GraphBuilder(RHICmdList);

	const FRDGTextureRef StateARDG   = RegisterExternalTexture(GraphBuilder, Params.PrevStateTexture, TEXT("SandStateA"));
	const FRDGTextureRef StateBRDG   = RegisterExternalTexture(GraphBuilder, Params.NextStateTexture, TEXT("SandStateB"));
	const FRDGTextureRef SandDataRDG = RegisterExternalTexture(GraphBuilder, Params.SandDataTexture,  TEXT("SandData"));

	const FRDGTextureUAVRef StateAUAV   = GraphBuilder.CreateUAV(StateARDG);
	const FRDGTextureUAVRef StateBUAV   = GraphBuilder.CreateUAV(StateBRDG);
	const FRDGTextureUAVRef SandDataUAV = GraphBuilder.CreateUAV(SandDataRDG);

	// Structured buffers can't be zero-sized, so fall back to a single inert
	// entry when nothing is pressing into the sand this frame.
	const int32 NumDeformers = Params.Deformers.Num();
	FRDGBufferRef DeformerBuffer;
	if (NumDeformers > 0)
	{
		DeformerBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("SandDeformers"),
			sizeof(FSandDeformerGPU),
			NumDeformers,
			Params.Deformers.GetData(),
			sizeof(FSandDeformerGPU) * NumDeformers);
	}
	else
	{
		static const FSandDeformerGPU DummyDeformer;
		DeformerBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("SandDeformersDummy"),
			sizeof(FSandDeformerGPU),
			1,
			&DummyDeformer,
			sizeof(FSandDeformerGPU));
	}
	const FRDGBufferSRVRef DeformerSRV = GraphBuilder.CreateSRV(DeformerBuffer);

	const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(
		Params.TextureSize,
		FIntPoint(SandDeformation::ThreadGroupSize, SandDeformation::ThreadGroupSize));

	// --- Pass 1: reproject + slump + ripple + stamp ------------------------
	//
	// Run SubSteps times at DeltaTime/N, ping-ponging between the two state
	// textures. Sub-stepping has to be separate dispatches rather than a loop
	// inside the shader: each iteration reads its neighbours, and there is no
	// way to synchronise every thread in the grid mid-kernel.
	//
	// Reprojection and stamping belong to the frame, not to each sub-step, so
	// they happen on the first iteration only. After that the region offset is
	// zero (which makes LoadReprojected an exact identity sample at texel
	// centres) and there are no deformers left to stamp.
	const int32 SubSteps = FMath::Clamp(Params.SubSteps, 1, 16);
	const float SubDeltaTime = Params.DeltaTime / static_cast<float>(SubSteps);

	FRDGTextureRef    SrcTexture = StateARDG;
	FRDGTextureRef    DstTexture = StateBRDG;
	FRDGTextureUAVRef DstUAV     = StateBUAV;
	FRDGTextureUAVRef SrcUAV     = StateAUAV;

	for (int32 Step = 0; Step < SubSteps; ++Step)
	{
		const bool bFirstStep = (Step == 0);

		FSandSimulateCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSandSimulateCS::FParameters>();
		PassParameters->PrevStateTexture = SrcTexture;
		PassParameters->PrevStateSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		PassParameters->OutStateTexture = DstUAV;
		PassParameters->Deformers = DeformerSRV;
		PassParameters->TextureSize = Params.TextureSize;
		PassParameters->RegionSize = Params.RegionSize;
		PassParameters->RegionOffsetFromPrev = bFirstStep ? Params.RegionOffsetFromPrev : FVector2f::ZeroVector;
		PassParameters->NumDeformers = bFirstStep ? NumDeformers : 0;
		PassParameters->DeltaTime = SubDeltaTime;
		PassParameters->ClearMask = bFirstStep ? Params.ClearMask : 1.0f;
		PassParameters->TanAngleOfRepose = Params.TanAngleOfRepose;
		PassParameters->SlumpRate = Params.SlumpRate;
		PassParameters->RippleSpeed = Params.RippleSpeed;
		PassParameters->RippleDamping = Params.RippleDamping;
		PassParameters->DisturbanceDecay = Params.DisturbanceDecay;
		PassParameters->HeightRestoreRate = Params.HeightRestoreRate;

		TShaderMapRef<FSandSimulateCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("SandSimulate(%dx%d, step %d/%d, %d deformers)",
				Params.TextureSize.X, Params.TextureSize.Y, Step + 1, SubSteps, bFirstStep ? NumDeformers : 0),
			ComputeShader,
			PassParameters,
			GroupCount);

		// After the swap SrcTexture holds the newest state, which is what the
		// next iteration reads and what the normals pass ends up consuming.
		Swap(SrcTexture, DstTexture);
		Swap(SrcUAV, DstUAV);
	}

	// --- Pass 2: state -> height + normal + disturbance --------------------
	{
		FSandNormalsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSandNormalsCS::FParameters>();
		PassParameters->StateTexture = SrcTexture;
		PassParameters->OutSandDataTexture = SandDataUAV;
		PassParameters->TextureSize = Params.TextureSize;
		PassParameters->RegionSize = Params.RegionSize;
		PassParameters->NormalStrength = Params.NormalStrength;

		TShaderMapRef<FSandNormalsCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("SandNormals(%dx%d)", Params.TextureSize.X, Params.TextureSize.Y),
			ComputeShader,
			PassParameters,
			GroupCount);
	}

	GraphBuilder.Execute();
}
