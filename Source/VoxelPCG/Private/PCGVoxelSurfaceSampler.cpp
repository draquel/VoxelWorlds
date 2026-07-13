// Copyright Daniel Raquel. All Rights Reserved.

#include "PCGVoxelSurfaceSampler.h"
#include "VoxelPCG.h"

#include "PCGContext.h"
#include "PCGPin.h"
#include "PCGCommon.h"
#include "Data/PCGBasePointData.h"
#include "Data/PCGSpatialData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttributeTpl.h"
#include "Helpers/PCGHelpers.h"

#include "VoxelSurfaceQuery.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelNoiseTypes.h"
#include "IVoxelWorldMode.h"

#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Math/RotationMatrix.h"

#define LOCTEXT_NAMESPACE "PCGVoxelSurfaceSampler"

namespace
{
	/**
	 * Resolved voxel-world generator context needed to re-sample the terrain surface.
	 *
	 * LIFETIME CONTRACT — this struct BORROWS, it does not own. WorldMode aliases the chunk
	 * manager's TUniquePtr<IVoxelWorldMode>, which UVoxelChunkManager::EndPlay -> Shutdown()
	 * frees (WorldMode.Reset()); BiomeConfig/ChunkManager are UObjects kept alive by the world.
	 * Borrowing is safe here ONLY because resolve and every use are confined to one synchronous
	 * game-thread ExecuteInternal invocation:
	 *  - FPCGVoxelSurfaceSamplerElement::CanExecuteOnlyOnMainThread() returns true, and the PCG
	 *    executor assert-enforces it (IPCGElement::Execute), so this runs on the game thread —
	 *    the same thread that tears the chunk manager down, which therefore cannot interleave.
	 *  - Every ExecuteInternal path returns true (never time-sliced/paused), so this stack-local
	 *    context never outlives the invocation that resolved it. A manager torn down before the
	 *    task runs fails ResolveVoxelContext's IsInitialized()/null checks instead.
	 *
	 * Do NOT capture this struct (or its pointers) into async/background work, store it on the
	 * FPCGContext across frames, or return false from ExecuteInternal while holding one — that
	 * recreates the PIE-stop use-after-free fixed in the VoxelMap tile generator (PR #29). An
	 * async/time-sliced sampler must instead OWN its generator state: see
	 * UVoxelMapSubsystem::CachedWorldMode (standalone, UObject-free
	 * TSharedPtr<const IVoxelWorldMode> captured by value into each task).
	 */
	struct FVoxelPCGSampleContext
	{
		const IVoxelWorldMode* WorldMode = nullptr;
		FVoxelNoiseParams NoiseParams;
		const UVoxelBiomeConfiguration* BiomeConfig = nullptr;
		int32 WorldSeed = 0;
		float VoxelSize = 100.0f;
		bool bWaterEnabled = false;
		float WaterLevel = 0.0f;
		// Live chunk manager for the edit-aware near band (edit-merged voxel surface query).
		const UVoxelChunkManager* ChunkManager = nullptr;
		bool bValid = false;
	};

	/**
	 * Resolve the active voxel world's generator context (world mode + noise/biome/water
	 * config) by finding an initialized UVoxelChunkManager in the world. Mirrors how
	 * UVoxelMapSubsystem::ResolveChunkManager locates the manager — but unlike that subsystem
	 * (which feeds async tile tasks and must OWN a standalone world mode, see PR #29), this
	 * borrows the manager's instance under the synchronous same-call contract documented on
	 * FVoxelPCGSampleContext. Game-thread only (actor iteration).
	 */
	FVoxelPCGSampleContext ResolveVoxelContext(UWorld* World)
	{
		FVoxelPCGSampleContext Out;
		if (!World)
		{
			return Out;
		}

		UVoxelChunkManager* ChunkMgr = nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			ChunkMgr = It->FindComponentByClass<UVoxelChunkManager>();
			if (ChunkMgr && ChunkMgr->IsInitialized())
			{
				break;
			}
			ChunkMgr = nullptr;
		}

		if (!ChunkMgr)
		{
			return Out;
		}

		const UVoxelWorldConfiguration* Config = ChunkMgr->GetConfiguration();
		const IVoxelWorldMode* WorldMode = ChunkMgr->GetWorldMode();
		if (!Config || !WorldMode)
		{
			return Out;
		}

		Out.WorldMode = WorldMode;
		Out.NoiseParams = Config->NoiseParams;
		Out.WorldSeed = Config->WorldSeed;
		Out.VoxelSize = Config->VoxelSize;
		Out.bWaterEnabled = Config->bEnableWaterLevel;
		Out.WaterLevel = Config->WaterLevel;
		Out.BiomeConfig = Config->bEnableBiomes ? Config->BiomeConfiguration : nullptr;
		Out.ChunkManager = ChunkMgr;
		Out.bValid = true;
		return Out;
	}
}

#if WITH_EDITOR
FText UPCGVoxelSurfaceSamplerSettings::GetNodeTooltipText() const
{
	return LOCTEXT("NodeTooltip",
		"Generates surface points on the runtime voxel terrain by re-sampling the procedural generator. "
		"Outputs Normal, Slope, MaterialID and BiomeID attributes per point for downstream filtering.");
}
#endif

TArray<FPCGPinProperties> UPCGVoxelSurfaceSamplerSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	// Required: PCG only schedules this generator when it is on the graph's execution path. Wire the
	// graph Input node (or a bounding shape) here. The shape also supplies the sampling XY footprint;
	// when it resolves to the executing actor's bounds, that is the grid cell for a runtime partition.
	FPCGPinProperties& BoundingShapePin = PinProperties.Emplace_GetRef(
		PCGVoxelSurfaceSamplerConstants::BoundingShapeLabel, EPCGDataType::Spatial,
		/*bAllowMultipleConnections=*/false, /*bAllowMultipleData=*/false,
		LOCTEXT("BoundingShapeTooltip",
			"Required. Wire the graph Input node (or a bounding shape) here so the node executes. "
			"Supplies the sampling XY footprint; resolves to the executing actor's bounds (the grid "
			"cell for a runtime PCG partition)."));
	BoundingShapePin.SetRequiredPin();
	return PinProperties;
}

TArray<FPCGPinProperties> UPCGVoxelSurfaceSamplerSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PinProperties.Emplace(PCGPinConstants::DefaultOutputLabel, EPCGDataType::Point);
	return PinProperties;
}

FPCGElementPtr UPCGVoxelSurfaceSamplerSettings::CreateElement() const
{
	return MakeShared<FPCGVoxelSurfaceSamplerElement>();
}

bool FPCGVoxelSurfaceSamplerElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGVoxelSurfaceSamplerElement::Execute);
	check(Context);

	const UPCGVoxelSurfaceSamplerSettings* Settings = Context->GetInputSettings<UPCGVoxelSurfaceSamplerSettings>();
	check(Settings);

	if (Settings->PointSpacing <= 0.0f)
	{
		PCGE_LOG(Warning, GraphAndLog, LOCTEXT("InvalidSpacing", "Voxel Surface Sampler: PointSpacing must be > 0."));
		return true;
	}

	// Resolve the world + voxel generator context.
	AActor* TargetActor = Context->GetTargetActor(nullptr);
	UWorld* World = TargetActor ? TargetActor->GetWorld() : nullptr;
	const FVoxelPCGSampleContext VoxelCtx = ResolveVoxelContext(World);
	if (!VoxelCtx.bValid)
	{
		PCGE_LOG(Warning, GraphAndLog, LOCTEXT("NoVoxelWorld",
			"Voxel Surface Sampler: no initialized VoxelChunkManager found in world; no points generated."));
		return true;
	}

	// Determine the XY sampling bounds: Bounding Shape input, else the executing actor's bounds.
	FBox Bounds(EForceInit::ForceInit);
	for (const FPCGTaggedData& Tagged : Context->InputData.GetInputsByPin(PCGVoxelSurfaceSamplerConstants::BoundingShapeLabel))
	{
		if (const UPCGSpatialData* Spatial = Cast<UPCGSpatialData>(Tagged.Data))
		{
			const FBox SpatialBounds = Spatial->GetBounds();
			if (SpatialBounds.IsValid)
			{
				Bounds += SpatialBounds;
			}
		}
	}

	if (!Bounds.IsValid && !Settings->bUnbounded && TargetActor)
	{
		Bounds = TargetActor->GetComponentsBoundingBox(/*bNonColliding=*/true);
	}

	if (!Bounds.IsValid)
	{
		PCGE_LOG(Warning, GraphAndLog, LOCTEXT("NoBounds",
			"Voxel Surface Sampler: no valid bounds (connect a Bounding Shape or disable Unbounded); no points generated."));
		return true;
	}

	// Grid stepping over the XY footprint, snapped to a world-anchored grid for determinism.
	const float Spacing = Settings->PointSpacing;
	const int32 MinX = FMath::CeilToInt(Bounds.Min.X / Spacing);
	const int32 MaxX = FMath::FloorToInt(Bounds.Max.X / Spacing);
	const int32 MinY = FMath::CeilToInt(Bounds.Min.Y / Spacing);
	const int32 MaxY = FMath::FloorToInt(Bounds.Max.Y / Spacing);

	const int64 NumX = static_cast<int64>(MaxX) - MinX + 1;
	const int64 NumY = static_cast<int64>(MaxY) - MinY + 1;
	if (NumX <= 0 || NumY <= 0)
	{
		return true;
	}

	const int64 NumCells = NumX * NumY;
	constexpr int64 MaxPoints = 4'000'000;
	if (NumCells > MaxPoints)
	{
		PCGE_LOG(Error, GraphAndLog, FText::Format(
			LOCTEXT("TooMany", "Voxel Surface Sampler: {0} cells exceeds the {1} cap; increase PointSpacing or reduce bounds."),
			NumCells, MaxPoints));
		return true;
	}

	// Sample the surface at each grid cell.
	TArray<FTransform> Transforms;
	TArray<FVoxelSurfaceSample> Samples;
	TArray<FIntPoint> CellIndices;
	Transforms.Reserve(NumCells);
	Samples.Reserve(NumCells);
	CellIndices.Reserve(NumCells);

	for (int32 IX = MinX; IX <= MaxX; ++IX)
	{
		const double WorldX = static_cast<double>(IX) * Spacing;
		for (int32 IY = MinY; IY <= MaxY; ++IY)
		{
			const double WorldY = static_cast<double>(IY) * Spacing;

			// Hybrid surface source: edit-merged voxel data near loaded chunks (edit-aware + exact
			// surface), the procedural generator beyond them (stream-independent reach).
			FVoxelSurfaceSample Sample;
			bool bGotNearBand = false;
			if (Settings->bEditAwareNearby && VoxelCtx.ChunkManager)
			{
				float NearHeight = 0.0f;
				FVector NearNormal = FVector::UpVector;
				float NearSlope = 0.0f;
				uint8 NearMaterial = 0;
				uint8 NearBiome = 0;
				if (VoxelCtx.ChunkManager->QueryEditMergedSurface(WorldX, WorldY, NearHeight, NearNormal, NearSlope, NearMaterial, NearBiome))
				{
					Sample.Height = NearHeight;
					Sample.Normal = NearNormal;
					Sample.SlopeDegrees = NearSlope;
					Sample.MaterialID = NearMaterial;
					Sample.BiomeID = NearBiome;
					bGotNearBand = true;
				}
			}

			if (!bGotNearBand)
			{
				Sample = FVoxelSurfaceQuery::SampleSurface(
					*VoxelCtx.WorldMode,
					static_cast<float>(WorldX), static_cast<float>(WorldY),
					VoxelCtx.VoxelSize, VoxelCtx.NoiseParams,
					VoxelCtx.BiomeConfig, VoxelCtx.WorldSeed,
					VoxelCtx.bWaterEnabled, VoxelCtx.WaterLevel);
			}

			const FVector Position(WorldX, WorldY, static_cast<double>(Sample.Height));
			const FQuat Rotation = Settings->bAlignToSurfaceNormal
				? FRotationMatrix::MakeFromZ(Sample.Normal).ToQuat()
				: FQuat::Identity;

			Transforms.Emplace(Rotation, Position);
			Samples.Add(Sample);
			CellIndices.Emplace(IX, IY);
		}
	}

	const int32 NumPoints = Transforms.Num();

	// Build the output point data.
	UPCGBasePointData* PointData = FPCGContext::NewPointData_AnyThread(Context);
	PointData->SetNumPoints(NumPoints, /*bInitializeValues=*/false);
	PointData->AllocateProperties(
		EPCGPointNativeProperties::Transform
		| EPCGPointNativeProperties::Density
		| EPCGPointNativeProperties::Seed
		| EPCGPointNativeProperties::MetadataEntry);
	PointData->SetSteepness(1.0f);

	UPCGMetadata* Meta = PointData->Metadata.Get();
	FPCGMetadataAttribute<FVector>* NormalAttr = Meta->FindOrCreateAttribute<FVector>(
		PCGVoxelSurfaceSamplerConstants::NormalAttribute, FVector::UpVector, /*bAllowsInterpolation=*/false, /*bOverrideParent=*/true);
	FPCGMetadataAttribute<double>* SlopeAttr = Meta->FindOrCreateAttribute<double>(
		PCGVoxelSurfaceSamplerConstants::SlopeAttribute, 0.0, false, true);
	FPCGMetadataAttribute<int32>* MaterialAttr = Meta->FindOrCreateAttribute<int32>(
		PCGVoxelSurfaceSamplerConstants::MaterialIDAttribute, 0, false, true);
	FPCGMetadataAttribute<int32>* BiomeAttr = Meta->FindOrCreateAttribute<int32>(
		PCGVoxelSurfaceSamplerConstants::BiomeIDAttribute, 0, false, true);

	TPCGValueRange<FTransform> TransformRange = PointData->GetTransformValueRange(/*bAllocate=*/false);
	TPCGValueRange<float> DensityRange = PointData->GetDensityValueRange(/*bAllocate=*/false);
	TPCGValueRange<int32> SeedRange = PointData->GetSeedValueRange(/*bAllocate=*/false);
	TPCGValueRange<int64> MetadataEntryRange = PointData->GetMetadataEntryValueRange(/*bAllocate=*/false);

	for (int32 i = 0; i < NumPoints; ++i)
	{
		TransformRange[i] = Transforms[i];
		DensityRange[i] = 1.0f;
		SeedRange[i] = PCGHelpers::ComputeSeed(CellIndices[i].X, CellIndices[i].Y);

		const int64 EntryKey = Meta->AddEntry();
		MetadataEntryRange[i] = EntryKey;

		const FVoxelSurfaceSample& Sample = Samples[i];
		if (NormalAttr) { NormalAttr->SetValue(EntryKey, Sample.Normal); }
		if (SlopeAttr) { SlopeAttr->SetValue(EntryKey, static_cast<double>(Sample.SlopeDegrees)); }
		if (MaterialAttr) { MaterialAttr->SetValue(EntryKey, static_cast<int32>(Sample.MaterialID)); }
		if (BiomeAttr) { BiomeAttr->SetValue(EntryKey, static_cast<int32>(Sample.BiomeID)); }
	}

	// Emit on the default output pin.
	FPCGTaggedData& Output = Context->OutputData.TaggedData.Emplace_GetRef();
	Output.Data = PointData;
	Output.Pin = PCGPinConstants::DefaultOutputLabel;

	PCGE_LOG(Verbose, LogOnly, FText::Format(
		LOCTEXT("Generated", "Voxel Surface Sampler: generated {0} points over [{1},{2}]x[{3},{4}]"),
		NumPoints, MinX, MaxX, MinY, MaxY));

	return true;
}

#undef LOCTEXT_NAMESPACE
