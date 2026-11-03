// Copyright Daniel Raquel. All Rights Reserved.
//
// SPIKE (D-VP4): see VoxelTerrainRepresentationSpike.h.

#include "VoxelTerrainRepresentationSpike.h"
#include "VoxelRendering.h"
#include "VoxelData.h"
#include "VoxelCoreTypes.h"
#include "DistanceFieldAtlas.h"
#include "PrimitiveSceneProxy.h"
#include "RenderDeferredCleanup.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"

namespace
{
	TAutoConsoleVariable<int32> CVarSpikeRayTracing(
		TEXT("voxel.Render.Spike.RayTracing"),
		0,
		TEXT("SPIKE (D-VP4): build ray tracing geometry for every chunk and seam mesh uploaded from now on, and give the scene ")
		TEXT("one ray tracing instance per mesh (Lumen's hardware tracing, ray traced shadows). Read when a region's proxy is ")
		TEXT("created and at each upload: set it before the world streams in, or recreate the proxies."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarSpikeDistanceField(
		TEXT("voxel.Render.Spike.DistanceField"),
		0,
		TEXT("SPIKE (D-VP4): build a mesh distance field from each chunk's voxels beside its mesh and register it with a ")
		TEXT("distance-field-only primitive (Lumen's software tracing, the global distance field, distance field shadows and AO)."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarSpikeDistanceFieldResolution(
		TEXT("voxel.Render.Spike.DistanceFieldResolution"),
		1.0f,
		TEXT("SPIKE (D-VP4): distance field voxels per terrain voxel (1 = one per metre at the demo's voxel size)."),
		ECVF_Default);

	/** Holds a distance field past the proxy's deletion until the renderer can no longer read it. */
	struct FVoxelDistanceFieldCleanup final : public FDeferredCleanupInterface
	{
		TSharedPtr<FDistanceFieldVolumeData, ESPMode::ThreadSafe> DistanceField;
	};

	/** A proxy that draws nothing and gives the distance field scene one chunk's distance field. */
	class FVoxelDistanceFieldSceneProxy final : public FPrimitiveSceneProxy
	{
	public:
		FVoxelDistanceFieldSceneProxy(const UVoxelChunkDistanceFieldComponent* InComponent,
			TSharedPtr<FDistanceFieldVolumeData, ESPMode::ThreadSafe> InDistanceField)
			: FPrimitiveSceneProxy(InComponent)
			, DistanceField(MoveTemp(InDistanceField))
		{
			bVerifyUsedMaterials = false;
			bCastDynamicShadow = true;
			bCastStaticShadow = false;
			bAffectDistanceFieldLighting = true;
			bAffectDynamicIndirectLighting = true;
			bSupportsDistanceFieldRepresentation = DistanceField.IsValid() && DistanceField->IsValid();
		}

		virtual ~FVoxelDistanceFieldSceneProxy() override
		{
			if (DistanceField.IsValid())
			{
				VoxelTerrainSpike::Counters().DFObjectsLive.fetch_sub(1);
				VoxelTerrainSpike::Counters().DFBytesLive.fetch_sub(DistanceField->AlwaysLoadedMip.Num());
				FVoxelDistanceFieldCleanup* Cleanup = new FVoxelDistanceFieldCleanup();
				Cleanup->DistanceField = MoveTemp(DistanceField);
				BeginCleanup(Cleanup);
			}
		}

		virtual SIZE_T GetTypeHash() const override
		{
			static size_t UniquePointer;
			return reinterpret_cast<size_t>(&UniquePointer);
		}

		virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
		{
			// Nothing to draw: the region component draws the chunk; this one only exists in the distance field scene.
			FPrimitiveViewRelevance Relevance;
			Relevance.bDrawRelevance = false;
			Relevance.bShadowRelevance = false;
			Relevance.bDynamicRelevance = false;
			return Relevance;
		}

		virtual void GetDistanceFieldAtlasData(const FDistanceFieldVolumeData*& OutDistanceFieldData, float& SelfShadowBias) const override
		{
			OutDistanceFieldData = DistanceField.Get();
			SelfShadowBias = 0.0f;
		}

		virtual bool HasDistanceFieldRepresentation() const override
		{
			return CastsDynamicShadow() && AffectsDistanceFieldLighting() && DistanceField.IsValid();
		}

		virtual bool HasDynamicIndirectShadowCasterRepresentation() const override
		{
			return HasDistanceFieldRepresentation();
		}

		virtual uint32 GetMemoryFootprint() const override
		{
			return sizeof(*this) + GetAllocatedSize();
		}

	private:
		TSharedPtr<FDistanceFieldVolumeData, ESPMode::ThreadSafe> DistanceField;
	};

	/**
	 * Squared Euclidean distance transform of one line (Felzenszwalb and Huttenlocher, 2012), in place.
	 *
	 * @param F       The line's values: 0 at a seed, Big elsewhere; receives the squared distances
	 * @param Stride  Elements between consecutive samples of the line
	 * @param N       Samples
	 * @param Scratch At least 3 * N + 1 doubles
	 */
	void DistanceTransformLine(double* F, int32 Stride, int32 N, double* Scratch)
	{
		double* Line = Scratch;
		double* Z = Scratch + N;
		int32* V = reinterpret_cast<int32*>(Scratch + 2 * N + 1);
		for (int32 Q = 0; Q < N; ++Q)
		{
			Line[Q] = F[Q * Stride];
		}

		int32 K = 0;
		V[0] = 0;
		Z[0] = -UE_DOUBLE_BIG_NUMBER;
		Z[1] = UE_DOUBLE_BIG_NUMBER;
		for (int32 Q = 1; Q < N; ++Q)
		{
			double S = ((Line[Q] + Q * Q) - (Line[V[K]] + V[K] * V[K])) / (2.0 * Q - 2.0 * V[K]);
			while (S <= Z[K])
			{
				--K;
				S = ((Line[Q] + Q * Q) - (Line[V[K]] + V[K] * V[K])) / (2.0 * Q - 2.0 * V[K]);
			}
			++K;
			V[K] = Q;
			Z[K] = S;
			Z[K + 1] = UE_DOUBLE_BIG_NUMBER;
		}

		K = 0;
		for (int32 Q = 0; Q < N; ++Q)
		{
			while (Z[K + 1] < Q)
			{
				++K;
			}
			F[Q * Stride] = (Q - V[K]) * (Q - V[K]) + Line[V[K]];
		}
	}
}

bool VoxelTerrainSpike::IsRayTracingEnabled()
{
	return CVarSpikeRayTracing.GetValueOnAnyThread() != 0;
}

bool VoxelTerrainSpike::IsDistanceFieldEnabled()
{
	return CVarSpikeDistanceField.GetValueOnAnyThread() != 0;
}

float VoxelTerrainSpike::GetDistanceFieldResolution()
{
	return FMath::Clamp(CVarSpikeDistanceFieldResolution.GetValueOnAnyThread(), 0.125f, 4.0f);
}

VoxelTerrainSpike::FCounters& VoxelTerrainSpike::Counters()
{
	static FCounters Instance;
	return Instance;
}

TSharedPtr<FDistanceFieldVolumeData, ESPMode::ThreadSafe> VoxelTerrainSpike::BuildChunkDistanceField(
	const TArray<FVoxelData>& Voxels, int32 ChunkSize, float VoxelSize, float Resolution, int32& OutBricks)
{
	OutBricks = 0;
	const int32 N = ChunkSize;
	const int32 Count = N * N * N;
	if (N < 2 || Voxels.Num() != Count)
	{
		return nullptr;
	}

	const double StartSeconds = FPlatformTime::Seconds();
	auto Index = [N](int32 X, int32 Y, int32 Z) { return X + Y * N + Z * N * N; };

	// The chunk holds a surface only if it has voxels on both sides of it.
	TArray<uint8> Inside;
	Inside.SetNumUninitialized(Count);
	int32 InsideCount = 0;
	for (int32 I = 0; I < Count; ++I)
	{
		Inside[I] = Voxels[I].Density >= VOXEL_SURFACE_THRESHOLD ? 1 : 0;
		InsideCount += Inside[I];
	}
	if (InsideCount == 0 || InsideCount == Count)
	{
		Counters().DFSkippedNoSurface.fetch_add(1);
		return nullptr;
	}

	// Seeds: voxels with a 6-neighbour on the other side. Their density is unsaturated or nearly so and gives their
	// distance to the surface (SignedDistanceToDensity: +-1 voxel maps to 0 / 255).
	constexpr double Big = 1.0e6;
	TArray<double> DistanceSq;
	DistanceSq.SetNumUninitialized(Count);
	TArray<float> SeedDistance;
	SeedDistance.SetNumUninitialized(Count);
	for (int32 Z = 0; Z < N; ++Z)
	{
		for (int32 Y = 0; Y < N; ++Y)
		{
			for (int32 X = 0; X < N; ++X)
			{
				const int32 I = Index(X, Y, Z);
				const uint8 Side = Inside[I];
				const bool bSeed = (X > 0 && Inside[I - 1] != Side) || (X < N - 1 && Inside[I + 1] != Side)
					|| (Y > 0 && Inside[I - N] != Side) || (Y < N - 1 && Inside[I + N] != Side)
					|| (Z > 0 && Inside[I - N * N] != Side) || (Z < N - 1 && Inside[I + N * N] != Side);
				DistanceSq[I] = bSeed ? 0.0 : Big;
				SeedDistance[I] = bSeed ? FMath::Abs(Voxels[I].Density - 127.5f) / 127.5f : -1.0f;
			}
		}
	}

	// Squared distance (in voxels) to the nearest seed, separable over the three axes.
	{
		TArray<double> Scratch;
		Scratch.SetNumUninitialized(3 * N + 2);
		for (int32 Z = 0; Z < N; ++Z)
		{
			for (int32 Y = 0; Y < N; ++Y)
			{
				DistanceTransformLine(&DistanceSq[Index(0, Y, Z)], 1, N, Scratch.GetData());
			}
		}
		for (int32 Z = 0; Z < N; ++Z)
		{
			for (int32 X = 0; X < N; ++X)
			{
				DistanceTransformLine(&DistanceSq[Index(X, 0, Z)], N, N, Scratch.GetData());
			}
		}
		for (int32 Y = 0; Y < N; ++Y)
		{
			for (int32 X = 0; X < N; ++X)
			{
				DistanceTransformLine(&DistanceSq[Index(X, Y, 0)], N * N, N, Scratch.GetData());
			}
		}
	}

	// Signed distance in local units, negative inside the solid (the engine's convention).
	TArray<float> Signed;
	Signed.SetNumUninitialized(Count);
	for (int32 I = 0; I < Count; ++I)
	{
		const float Unsigned = SeedDistance[I] >= 0.0f ? SeedDistance[I] : static_cast<float>(FMath::Sqrt(DistanceSq[I]));
		Signed[I] = (Inside[I] ? -Unsigned : Unsigned) * VoxelSize;
	}

	auto Sample = [&](const FVector3f& LocalPosition) -> float
	{
		const FVector3f G = LocalPosition / VoxelSize;
		const float GX = FMath::Clamp(G.X, 0.0f, static_cast<float>(N - 1));
		const float GY = FMath::Clamp(G.Y, 0.0f, static_cast<float>(N - 1));
		const float GZ = FMath::Clamp(G.Z, 0.0f, static_cast<float>(N - 1));
		const int32 X0 = FMath::Min(FMath::FloorToInt32(GX), N - 2);
		const int32 Y0 = FMath::Min(FMath::FloorToInt32(GY), N - 2);
		const int32 Z0 = FMath::Min(FMath::FloorToInt32(GZ), N - 2);
		const float TX = GX - X0, TY = GY - Y0, TZ = GZ - Z0;
		const int32 I = Index(X0, Y0, Z0);
		const float C00 = FMath::Lerp(Signed[I], Signed[I + 1], TX);
		const float C10 = FMath::Lerp(Signed[I + N], Signed[I + N + 1], TX);
		const float C01 = FMath::Lerp(Signed[I + N * N], Signed[I + N * N + 1], TX);
		const float C11 = FMath::Lerp(Signed[I + N * N + N], Signed[I + N * N + N + 1], TX);
		return FMath::Lerp(FMath::Lerp(C00, C10, TY), FMath::Lerp(C01, C11, TY), TZ);
	};

	// The chunk's own box: half a voxel past its outer voxels, where the next chunk's box begins.
	const float HalfVoxel = 0.5f * VoxelSize;
	const FBox3f LocalSpaceMeshBounds(FVector3f(-HalfVoxel), FVector3f((N - 0.5f) * VoxelSize));

	// MeshDistanceFieldUtilities.cpp's encoding (BuildSignedDistanceField), one mip.
	const float NumVoxelsPerLocalSpaceUnit = Resolution / VoxelSize;
	const int32 MaxNumBlocksOneDim = DistanceField::MaxIndirectionDimension - 1;
	const float LocalToVolumeScale = 1.0f / LocalSpaceMeshBounds.GetExtent().GetMax();
	const FVector3f DesiredDimensions = LocalSpaceMeshBounds.GetSize() * NumVoxelsPerLocalSpaceUnit / static_cast<float>(DistanceField::UniqueDataBrickSize);
	const FInt32Vector Dimensions(
		FMath::Clamp(FMath::RoundToInt(DesiredDimensions.X), 1, MaxNumBlocksOneDim),
		FMath::Clamp(FMath::RoundToInt(DesiredDimensions.Y), 1, MaxNumBlocksOneDim),
		FMath::Clamp(FMath::RoundToInt(DesiredDimensions.Z), 1, MaxNumBlocksOneDim));

	const FVector3f TexelObjectSpaceSize = LocalSpaceMeshBounds.GetSize()
		/ FVector3f(Dimensions * DistanceField::UniqueDataBrickSize - FInt32Vector(2 * DistanceField::MeshDistanceFieldObjectBorder));
	const FBox3f VolumeBounds = LocalSpaceMeshBounds.ExpandBy(TexelObjectSpaceSize);
	const FVector3f IndirectionVoxelSize = VolumeBounds.GetSize() / FVector3f(Dimensions);
	const FVector3f DistanceFieldVoxelSize = IndirectionVoxelSize / static_cast<float>(DistanceField::UniqueDataBrickSize);
	const FVector3f VolumeSpaceDistanceFieldVoxelSize = IndirectionVoxelSize * LocalToVolumeScale / static_cast<float>(DistanceField::UniqueDataBrickSize);
	const float MaxDistanceForEncoding = VolumeSpaceDistanceFieldVoxelSize.Size() * DistanceField::BandSizeInVoxels;
	const FVector2f DistanceFieldToVolumeScaleBias(2.0f * MaxDistanceForEncoding, -MaxDistanceForEncoding);
	const float LocalSpaceTraceDistance = MaxDistanceForEncoding / LocalToVolumeScale;

	constexpr int32 BrickVoxels = DistanceField::BrickSize * DistanceField::BrickSize * DistanceField::BrickSize;
	const int32 NumIndirection = Dimensions.X * Dimensions.Y * Dimensions.Z;
	TArray<uint32> IndirectionTable;
	IndirectionTable.Init(DistanceField::InvalidBrickIndex, NumIndirection);
	TArray<uint8> BrickData;
	uint8 Brick[BrickVoxels];

	for (int32 BZ = 0; BZ < Dimensions.Z; ++BZ)
	{
		for (int32 BY = 0; BY < Dimensions.Y; ++BY)
		{
			for (int32 BX = 0; BX < Dimensions.X; ++BX)
			{
				const FVector3f BrickMin = VolumeBounds.Min + FVector3f(BX, BY, BZ) * IndirectionVoxelSize;

				// Early out: a brick whose voxels (and one beyond) all lie past the band on one side stores nothing
				{
					const FVector3f BrickMax = BrickMin + IndirectionVoxelSize;
					const FIntVector Lo(FMath::Clamp(FMath::FloorToInt32(BrickMin.X / VoxelSize) - 1, 0, N - 1),
						FMath::Clamp(FMath::FloorToInt32(BrickMin.Y / VoxelSize) - 1, 0, N - 1),
						FMath::Clamp(FMath::FloorToInt32(BrickMin.Z / VoxelSize) - 1, 0, N - 1));
					const FIntVector Hi(FMath::Clamp(FMath::CeilToInt32(BrickMax.X / VoxelSize) + 1, 0, N - 1),
						FMath::Clamp(FMath::CeilToInt32(BrickMax.Y / VoxelSize) + 1, 0, N - 1),
						FMath::Clamp(FMath::CeilToInt32(BrickMax.Z / VoxelSize) + 1, 0, N - 1));
					float MinSigned = UE_BIG_NUMBER;
					float MaxSigned = -UE_BIG_NUMBER;
					for (int32 Z = Lo.Z; Z <= Hi.Z; ++Z)
					{
						for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y)
						{
							const float* Row = &Signed[Index(0, Y, Z)];
							for (int32 X = Lo.X; X <= Hi.X; ++X)
							{
								MinSigned = FMath::Min(MinSigned, Row[X]);
								MaxSigned = FMath::Max(MaxSigned, Row[X]);
							}
						}
					}
					if (MinSigned >= LocalSpaceTraceDistance || MaxSigned <= -LocalSpaceTraceDistance)
					{
						continue;
					}
				}
				uint8 BrickMinDistance = MAX_uint8;
				uint8 BrickMaxDistance = MIN_uint8;
				for (int32 Z = 0; Z < DistanceField::BrickSize; ++Z)
				{
					for (int32 Y = 0; Y < DistanceField::BrickSize; ++Y)
					{
						for (int32 X = 0; X < DistanceField::BrickSize; ++X)
						{
							const FVector3f Position = FVector3f(X, Y, Z) * DistanceFieldVoxelSize + BrickMin;
							const float VolumeSpaceDistance = Sample(Position) * LocalToVolumeScale;
							const float Rescaled = (VolumeSpaceDistance - DistanceFieldToVolumeScaleBias.Y) / DistanceFieldToVolumeScaleBias.X;
							const uint8 Quantized = static_cast<uint8>(FMath::Clamp<int32>(FMath::FloorToInt(Rescaled * 255.0f + 0.5f), 0, 255));
							Brick[(Z * DistanceField::BrickSize + Y) * DistanceField::BrickSize + X] = Quantized;
							BrickMinDistance = FMath::Min(BrickMinDistance, Quantized);
							BrickMaxDistance = FMath::Max(BrickMaxDistance, Quantized);
						}
					}
				}

				// A brick wholly outside the band either way is left out, as the engine's builder does.
				if (BrickMinDistance < MAX_uint8 && BrickMaxDistance > MIN_uint8)
				{
					IndirectionTable[(BZ * Dimensions.Y + BY) * Dimensions.X + BX] = static_cast<uint32>(OutBricks);
					BrickData.Append(Brick, BrickVoxels);
					++OutBricks;
				}
			}
		}
	}

	TSharedPtr<FDistanceFieldVolumeData, ESPMode::ThreadSafe> Result = MakeShared<FDistanceFieldVolumeData, ESPMode::ThreadSafe>();
	Result->LocalSpaceMeshBounds = LocalSpaceMeshBounds;
	Result->bMostlyTwoSided = false;

	const int32 IndirectionBytes = IndirectionTable.Num() * IndirectionTable.GetTypeSize();
	Result->AlwaysLoadedMip.SetNumUninitialized(IndirectionBytes + BrickData.Num());
	FMemory::Memcpy(Result->AlwaysLoadedMip.GetData(), IndirectionTable.GetData(), IndirectionBytes);
	if (BrickData.Num() > 0)
	{
		FMemory::Memcpy(Result->AlwaysLoadedMip.GetData() + IndirectionBytes, BrickData.GetData(), BrickData.Num());
	}

	FSparseDistanceFieldMip Mip;
	Mip.IndirectionDimensions = Dimensions;
	Mip.NumDistanceFieldBricks = OutBricks;
	Mip.DistanceFieldToVolumeScaleBias = DistanceFieldToVolumeScaleBias;
	const FVector3f VirtualUVMin = FVector3f(DistanceField::MeshDistanceFieldObjectBorder) / FVector3f(Dimensions * DistanceField::UniqueDataBrickSize);
	const FVector3f VirtualUVSize = FVector3f(Dimensions * DistanceField::UniqueDataBrickSize - FInt32Vector(2 * DistanceField::MeshDistanceFieldObjectBorder))
		/ FVector3f(Dimensions * DistanceField::UniqueDataBrickSize);
	const FVector3f VolumePositionExtent = LocalSpaceMeshBounds.GetExtent() * LocalToVolumeScale;
	Mip.VolumeToVirtualUVScale = VirtualUVSize / (2 * VolumePositionExtent);
	Mip.VolumeToVirtualUVAdd = VolumePositionExtent * Mip.VolumeToVirtualUVScale + VirtualUVMin;

	// Every mip slot describes the one mip; only the last (always loaded) has data, and the streamer never requests a mip
	// whose BulkSize is 0 (DistanceFieldStreaming.cpp), so nothing is read from disk: this works in a cooked game, where
	// runtime bulk data could not be streamed.
	for (int32 MipIndex = 0; MipIndex < DistanceField::NumMips; ++MipIndex)
	{
		Result->Mips[MipIndex] = Mip;
		Result->Mips[MipIndex].BulkOffset = 0;
		Result->Mips[MipIndex].BulkSize = 0;
	}

	const int64 Micros = static_cast<int64>((FPlatformTime::Seconds() - StartSeconds) * 1.0e6);
	FCounters& C = Counters();
	C.DFBuilds.fetch_add(1);
	C.DFBricksBuilt.fetch_add(OutBricks);
	C.DFBuildMicrosTotal.fetch_add(Micros);
	int64 PreviousMax = C.DFBuildMicrosMax.load();
	while (Micros > PreviousMax && !C.DFBuildMicrosMax.compare_exchange_weak(PreviousMax, Micros))
	{
	}
	return Result;
}

UVoxelChunkDistanceFieldComponent::UVoxelChunkDistanceFieldComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	CastShadow = true;
	bCastDynamicShadow = true;
	bCastStaticShadow = false;
	bAffectDistanceFieldLighting = true;
	bAffectDynamicIndirectLighting = true;
	bReceivesDecals = false;
	Mobility = EComponentMobility::Static;
}

void UVoxelChunkDistanceFieldComponent::SetDistanceField(TSharedPtr<FDistanceFieldVolumeData, ESPMode::ThreadSafe> InDistanceField)
{
	DistanceField = MoveTemp(InDistanceField);
	UpdateBounds();
	MarkRenderStateDirty();
}

FPrimitiveSceneProxy* UVoxelChunkDistanceFieldComponent::CreateSceneProxy()
{
	if (!DistanceField.IsValid() || !DistanceField->IsValid())
	{
		return nullptr;
	}
	VoxelTerrainSpike::Counters().DFObjectsLive.fetch_add(1);
	VoxelTerrainSpike::Counters().DFBytesLive.fetch_add(DistanceField->AlwaysLoadedMip.Num());
	return new FVoxelDistanceFieldSceneProxy(this, DistanceField);
}

FBoxSphereBounds UVoxelChunkDistanceFieldComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (!DistanceField.IsValid())
	{
		return FBoxSphereBounds(FBox(FVector(-1.0), FVector(1.0))).TransformBy(LocalToWorld);
	}
	return FBoxSphereBounds(FBox(DistanceField->LocalSpaceMeshBounds)).TransformBy(LocalToWorld);
}

namespace
{
	FAutoConsoleCommand GSpikeStatsCommand(
		TEXT("voxel.Render.Spike.Stats"),
		TEXT("SPIKE (D-VP4): the ray tracing and distance field prototypes' counters. 'reset' clears the build totals."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			VoxelTerrainSpike::FCounters& C = VoxelTerrainSpike::Counters();
			if (Args.Num() > 0 && Args[0] == TEXT("reset"))
			{
				C.RTBuilds = 0;
				C.RTTrianglesBuilt = 0;
				C.DFBuilds = 0;
				C.DFSkippedNoSurface = 0;
				C.DFBricksBuilt = 0;
				C.DFBuildMicrosTotal = 0;
				C.DFBuildMicrosMax = 0;
				C.DFRegisterMicrosTotal = 0;
				C.DFRegisters = 0;
			}
			const int64 DFBuilds = C.DFBuilds.load();
			const int64 DFRegisters = C.DFRegisters.load();
			UE_LOG(LogVoxelRendering, Display,
				TEXT("[Spike] RT: %lld geometries live, %.1f MB BLAS, %lld builds, %lld triangles built, %lld instances last frame | ")
				TEXT("DF: %lld objects live, %.1f MB, %lld builds (%lld skipped, no surface), %.1f bricks/build, build avg %.3f ms max %.3f ms, ")
				TEXT("register avg %.3f ms over %lld"),
				C.RTGeometriesLive.load(), C.RTBytesLive.load() / (1024.0 * 1024.0), C.RTBuilds.load(), C.RTTrianglesBuilt.load(),
				C.RTInstancesLastFrame.load(),
				C.DFObjectsLive.load(), C.DFBytesLive.load() / (1024.0 * 1024.0), DFBuilds, C.DFSkippedNoSurface.load(),
				DFBuilds > 0 ? double(C.DFBricksBuilt.load()) / DFBuilds : 0.0,
				DFBuilds > 0 ? C.DFBuildMicrosTotal.load() / 1000.0 / DFBuilds : 0.0, C.DFBuildMicrosMax.load() / 1000.0,
				DFRegisters > 0 ? C.DFRegisterMicrosTotal.load() / 1000.0 / DFRegisters : 0.0, DFRegisters);
		}));
	FAutoConsoleCommand GSpikeDFVisibleCommand(
		TEXT("voxel.Render.Spike.DFVisible"),
		TEXT("SPIKE (D-VP4): 0|1, hide or show every chunk distance field component (an A/B in one session)."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const bool bVisible = Args.Num() == 0 || Args[0] != TEXT("0");
			int32 Count = 0;
			for (TObjectIterator<UVoxelChunkDistanceFieldComponent> It; It; ++It)
			{
				if (It->IsRegistered())
				{
					It->SetVisibility(bVisible);
					++Count;
				}
			}
			UE_LOG(LogVoxelRendering, Display, TEXT("[Spike] %d distance field components %s"), Count, bVisible ? TEXT("shown") : TEXT("hidden"));
		}));
	FAutoConsoleCommand GSpikeBenchDFCommand(
		TEXT("voxel.Render.Spike.BenchDF"),
		TEXT("SPIKE (D-VP4): [n=20] [resolution=1] [chunk=64] time BuildChunkDistanceField on this thread over a synthetic terrain chunk ")
		TEXT("(a sloped, rolling surface through the chunk with an overhang), uncontended; logs min / median / max ms, bricks and bytes."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const int32 Runs = Args.Num() > 0 ? FMath::Max(1, FCString::Atoi(*Args[0])) : 20;
			const float Resolution = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 1.0f;
			const int32 N = Args.Num() > 2 ? FMath::Clamp(FCString::Atoi(*Args[2]), 8, 128) : 64;
			const float VoxelSize = 75.0f;
			TArray<FVoxelData> Voxels;
			Voxels.SetNum(N * N * N);
			for (int32 Z = 0; Z < N; ++Z)
			{
				for (int32 Y = 0; Y < N; ++Y)
				{
					for (int32 X = 0; X < N; ++X)
					{
						const float Height = N * 0.45f + 0.2f * X + 4.0f * FMath::Sin(X * 0.15f) * FMath::Cos(Y * 0.11f);
						float SignedDistance = (Height - Z);
						// An overhang: a slab above the surface over a quarter of the chunk
						const float SlabDistance = 2.5f - FMath::Abs(Z - (Height + 9.0f));
						if (X < N / 2 && Y < N / 2)
						{
							SignedDistance = FMath::Max(SignedDistance, SlabDistance);
						}
						const float Normalized = FMath::Clamp(SignedDistance, -1.0f, 1.0f);
						Voxels[X + Y * N + Z * N * N].Density = static_cast<uint8>(FMath::Clamp((Normalized + 1.0f) * 127.5f, 0.0f, 255.0f));
					}
				}
			}
			TArray<double> Ms;
			int32 Bricks = 0;
			int64 Bytes = 0;
			for (int32 Run = 0; Run < Runs; ++Run)
			{
				const double T0 = FPlatformTime::Seconds();
				TSharedPtr<FDistanceFieldVolumeData, ESPMode::ThreadSafe> Field = VoxelTerrainSpike::BuildChunkDistanceField(Voxels, N, VoxelSize, Resolution, Bricks);
				Ms.Add((FPlatformTime::Seconds() - T0) * 1000.0);
				Bytes = Field.IsValid() ? Field->AlwaysLoadedMip.Num() : 0;
			}
			Ms.Sort();
			UE_LOG(LogVoxelRendering, Display, TEXT("[Spike] BenchDF %d^3 resolution %.2f: min %.2f ms, median %.2f ms, max %.2f ms over %d; %d bricks, %.1f KB"),
				N, Resolution, Ms[0], Ms[Ms.Num() / 2], Ms.Last(), Runs, Bricks, Bytes / 1024.0);
		}));
}
