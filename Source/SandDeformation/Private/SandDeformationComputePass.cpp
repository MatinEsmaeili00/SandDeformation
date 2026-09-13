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

	const FRDGTextureRef PrevStateRDG = RegisterExternalTexture(GraphBuilder, Params.PrevStateTexture, TEXT("SandPrevState"));
	const FRDGTextureRef NextStateRDG = RegisterExternalTexture(GraphBuilder, Params.NextStateTexture, TEXT("SandNextState"));
	const FRDGTextureRef SandDataRDG  = RegisterExternalTexture(GraphBuilder, Params.SandDataTexture,  TEXT("SandData"));

	const FRDGTextureUAVRef NextStateUAV = GraphBuilder.CreateUAV(NextStateRDG);
	const FRDGTextureUAVRef SandDataUAV  = GraphBuilder.CreateUAV(SandDataRDG);

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
	{
		FSandSimulateCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSandSimulateCS::FParameters>();
		PassParameters->PrevStateTexture = PrevStateRDG;
		PassParameters->PrevStateSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		PassParameters->OutStateTexture = NextStateUAV;
		PassParameters->Deformers = DeformerSRV;
		PassParameters->TextureSize = Params.TextureSize;
		PassParameters->RegionSize = Params.RegionSize;
		PassParameters->RegionOffsetFromPrev = Params.RegionOffsetFromPrev;
		PassParameters->NumDeformers = NumDeformers;
		PassParameters->DeltaTime = Params.DeltaTime;
		PassParameters->ClearMask = Params.ClearMask;
		PassParameters->TanAngleOfRepose = Params.TanAngleOfRepose;
		PassParameters->SlumpRate = Params.SlumpRate;
		PassParameters->RippleSpeed = Params.RippleSpeed;
		PassParameters->RippleDamping = Params.RippleDamping;
		PassParameters->DisturbanceDecay = Params.DisturbanceDecay;
		PassParameters->HeightRestoreRate = Params.HeightRestoreRate;

		TShaderMapRef<FSandSimulateCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("SandSimulate(%dx%d, %d deformers)", Params.TextureSize.X, Params.TextureSize.Y, NumDeformers),
			ComputeShader,
			PassParameters,
			GroupCount);
	}

	// --- Pass 2: state -> height + normal + disturbance --------------------
	{
		FSandNormalsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSandNormalsCS::FParameters>();
		PassParameters->StateTexture = NextStateRDG;
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
