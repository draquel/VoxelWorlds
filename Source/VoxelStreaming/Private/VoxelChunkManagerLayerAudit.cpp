// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelChunkManager.h"
#include "VoxelStreaming.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelCaveConfiguration.h"
#include "VoxelCaveQuery.h"
#include "VoxelChunkCodec.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelEditManager.h"
#include "VoxelEditTypes.h"
#include "VoxelTerrainConditioning.h"
#include "IVoxelWorldMode.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

// Layer-boundary audit (world modes review G34). See UVoxelChunkManager::AuditLayerBoundaries.

namespace VoxelLayerAuditPrivate
{
	/** A chunk's generated voxels without changing its residency: the resident array, or a decoded copy in Scratch. */
	const TArray<FVoxelData>* ReadGenerated(const FChunkDescriptor& Descriptor, TArray<FVoxelData>& Scratch)
	{
		if (Descriptor.IsVoxelDataResident())
		{
			return &Descriptor.VoxelData;
		}
		if (Descriptor.Residency == EVoxelDataResidency::Uniform)
		{
			Scratch.Init(Descriptor.UniformValue, Descriptor.GetTotalVoxels());
			return &Scratch;
		}
		if (Descriptor.Residency == EVoxelDataResidency::Compressed && FVoxelChunkCodec::Decompress(Descriptor.CompressedVoxelData, Scratch))
		{
			return &Scratch;
		}
		return nullptr;
	}

	const TCHAR* ResidencyName(const FChunkDescriptor& Descriptor)
	{
		switch (Descriptor.Residency)
		{
		case EVoxelDataResidency::Resident: return TEXT("resident");
		case EVoxelDataResidency::Uniform: return TEXT("uniform");
		case EVoxelDataResidency::Compressed: return TEXT("compressed");
		default: return TEXT("empty");
		}
	}
}

int64 UVoxelChunkManager::AuditLayerBoundaries(int32 MaxLines, int32 RegenCount, FString& OutReport)
{
	using namespace VoxelLayerAuditPrivate;
	OutReport.Reset();
	if (!bIsInitialized || !Configuration)
	{
		OutReport = TEXT("not initialized");
		return 0;
	}

	const int32 CS = Configuration->ChunkSize;
	const int32 Slice = CS * CS;
	const int32 Volume = Slice * CS;
	const float VoxelSize = Configuration->VoxelSize;
	const float ChunkWorldSize = Configuration->GetChunkWorldSize();

	// Ceilings (air under solid) and floors (solid under air) per plane: index 0 the plane against the chunk below (its top
	// voxel under this chunk's bottom voxel), index p >= 1 the plane between this chunk's voxels p - 1 and p.
	TArray<int64> GenCeil, GenFloor, MergedCeil, MergedFloor;
	GenCeil.SetNumZeroed(CS);
	GenFloor.SetNumZeroed(CS);
	MergedCeil.SetNumZeroed(CS);
	MergedFloor.SetNumZeroed(CS);
	// The same counts over the chunk pairs only (the chunk with the one below loaded), so the boundary compares with the planes
	// on either side of it in the same chunks: PairedCeil[p] the upper chunk's plane p, PairedBelowTop the lower chunk's top plane.
	TArray<int64> PairedCeil;
	PairedCeil.SetNumZeroed(CS);
	int64 PairedBelowTop = 0;
	TArray<int32> ChunkPlaneCeil;
	ChunkPlaneCeil.SetNumZeroed(CS);
	int64 ChunksScanned = 0, PairsScanned = 0, EditedChunks = 0;

	struct FChunkLine
	{
		FIntVector Coord;
		int32 GenCeilings = 0;
		int32 MergedCeilings = 0;
		int32 InteriorCeilingsMax = 0; // the most ceilings on one interior plane of this chunk (generated)
	};
	TArray<FChunkLine> Lines;

	auto MergeEdits = [this](const FIntVector& Coord, const TArray<FVoxelData>& Generated, TArray<FVoxelData>& Out) -> bool
	{
		const FChunkEditLayer* Layer = (EditManager && EditManager->ChunkHasEdits(Coord)) ? EditManager->GetEditLayer(Coord) : nullptr;
		if (!Layer || Layer->IsEmpty())
		{
			return false;
		}
		Out = Generated;
		for (const TPair<int32, FVoxelEdit>& Pair : Layer->Edits)
		{
			if (Out.IsValidIndex(Pair.Key))
			{
				Out[Pair.Key] = Pair.Value.ApplyToProceduralData(Out[Pair.Key]);
			}
		}
		return true;
	};

	TArray<FVoxelData> ScratchOwn, ScratchBelow, MergedOwn, MergedBelow;
	for (const TPair<FIntVector, FVoxelChunkState>& Pair : ChunkStates)
	{
		const FIntVector Coord = Pair.Key;
		const TArray<FVoxelData>* Own = ReadGenerated(Pair.Value.Descriptor, ScratchOwn);
		if (!Own || Own->Num() != Volume)
		{
			continue;
		}
		++ChunksScanned;
		const bool bOwnEdited = MergeEdits(Coord, *Own, MergedOwn);
		EditedChunks += bOwnEdited ? 1 : 0;
		const TArray<FVoxelData>& OwnMerged = bOwnEdited ? MergedOwn : *Own;

		FChunkLine Line;
		Line.Coord = Coord;
		for (int32 P = 1; P < CS; ++P)
		{
			int32 PlaneCeil = 0;
			for (int32 I = 0; I < Slice; ++I)
			{
				const bool bBelow = (*Own)[I + (P - 1) * Slice].IsSolid();
				const bool bAbove = (*Own)[I + P * Slice].IsSolid();
				PlaneCeil += (!bBelow && bAbove) ? 1 : 0;
				GenFloor[P] += (bBelow && !bAbove) ? 1 : 0;
				const bool bMBelow = OwnMerged[I + (P - 1) * Slice].IsSolid();
				const bool bMAbove = OwnMerged[I + P * Slice].IsSolid();
				MergedCeil[P] += (!bMBelow && bMAbove) ? 1 : 0;
				MergedFloor[P] += (bMBelow && !bMAbove) ? 1 : 0;
			}
			GenCeil[P] += PlaneCeil;
			ChunkPlaneCeil[P] = PlaneCeil;
			Line.InteriorCeilingsMax = FMath::Max(Line.InteriorCeilingsMax, PlaneCeil);
		}

		const FVoxelChunkState* BelowState = ChunkStates.Find(Coord - FIntVector(0, 0, 1));
		const TArray<FVoxelData>* Below = BelowState ? ReadGenerated(BelowState->Descriptor, ScratchBelow) : nullptr;
		if (Below && Below->Num() == Volume)
		{
			++PairsScanned;
			const bool bBelowEdited = MergeEdits(Coord - FIntVector(0, 0, 1), *Below, MergedBelow);
			const TArray<FVoxelData>& BelowMerged = bBelowEdited ? MergedBelow : *Below;
			for (int32 P = 1; P < CS; ++P)
			{
				PairedCeil[P] += ChunkPlaneCeil[P];
			}
			for (int32 I = 0; I < Slice; ++I)
			{
				const bool bBelow = (*Below)[I + (CS - 1) * Slice].IsSolid();
				const bool bAbove = (*Own)[I].IsSolid();
				PairedBelowTop += (!(*Below)[I + (CS - 2) * Slice].IsSolid() && bBelow) ? 1 : 0;
				Line.GenCeilings += (!bBelow && bAbove) ? 1 : 0;
				GenFloor[0] += (bBelow && !bAbove) ? 1 : 0;
				const bool bMBelow = BelowMerged[I + (CS - 1) * Slice].IsSolid();
				const bool bMAbove = OwnMerged[I].IsSolid();
				Line.MergedCeilings += (!bMBelow && bMAbove) ? 1 : 0;
				MergedFloor[0] += (bMBelow && !bMAbove) ? 1 : 0;
			}
			GenCeil[0] += Line.GenCeilings;
			MergedCeil[0] += Line.MergedCeilings;
			if (Line.GenCeilings > 0 || Line.MergedCeilings > 0)
			{
				Lines.Add(Line);
			}
		}
	}

	// Over the chunk pairs: the boundary against the planes on either side of it and the upper chunks' interior planes.
	int64 PairedInteriorSum = 0, PairedInteriorMax = 0, InteriorFloorSum = 0;
	int32 PairedInteriorMaxPlane = 1;
	for (int32 P = 1; P < CS; ++P)
	{
		PairedInteriorSum += PairedCeil[P];
		InteriorFloorSum += GenFloor[P];
		if (PairedCeil[P] > PairedInteriorMax)
		{
			PairedInteriorMax = PairedCeil[P];
			PairedInteriorMaxPlane = P;
		}
	}
	const double InteriorPlanes = FMath::Max(1, CS - 1);

	OutReport += FString::Printf(TEXT("generation %s; %lld chunk(s) with voxels (%lld with edits), %lld with the chunk below loaded (pairs)\n"),
		bUseGPUGenerationActive ? TEXT("GPU") : TEXT("CPU"), ChunksScanned, EditedChunks, PairsScanned);
	OutReport += FString::Printf(TEXT("ceilings (air under solid) over the pairs: boundary plane %lld (%lld with edits); the plane under it %lld, over it %lld; the upper chunks' planes mean %.0f, max %lld (plane %d)\n"),
		GenCeil[0], MergedCeil[0], PairedBelowTop, PairedCeil[1], PairedInteriorSum / InteriorPlanes, PairedInteriorMax, PairedInteriorMaxPlane);
	OutReport += FString::Printf(TEXT("floors (solid under air): boundary plane %lld over the pairs (%lld with edits); interior planes mean %.0f over every chunk\n"),
		GenFloor[0], MergedFloor[0], InteriorFloorSum / InteriorPlanes);
	{
		FString PlaneList;
		for (int32 P = 0; P < CS; ++P)
		{
			PlaneList += FString::Printf(TEXT("%s%lld"), P == 0 ? TEXT("") : TEXT(" "), GenCeil[P]);
		}
		OutReport += FString::Printf(TEXT("ceiling columns per plane over every chunk (0 = the boundary, over the pairs): %s\n"), *PlaneList);
	}

	Lines.Sort([](const FChunkLine& A, const FChunkLine& B) { return A.GenCeilings > B.GenCeilings; });
	const IVoxelWorldMode* WM = GetWorldMode();
	const bool bHeightmap = Configuration->WorldMode != EWorldMode::SphericalPlanet;
	const int32 Seed = Configuration->NoiseParams.Seed;
	const bool bCaves = Configuration->bEnableCaves && Configuration->CaveConfiguration;

	for (int32 L = 0; L < FMath::Min(MaxLines, Lines.Num()); ++L)
	{
		const FChunkLine& Line = Lines[L];
		const FIntVector Below = Line.Coord - FIntVector(0, 0, 1);
		const FVector Origin = Configuration->WorldOrigin + FVector(Line.Coord) * ChunkWorldSize;
		const FVoxelChunkState* OwnState = ChunkStates.Find(Line.Coord);
		const FVoxelChunkState* BelowState = ChunkStates.Find(Below);
		OutReport += FString::Printf(TEXT("chunk (%d, %d, %d) over (%d, %d, %d): %d boundary ceilings (%d with edits), most on one interior plane %d; residency %s / %s, LOD %d / %d; plane Z %.0f; vox.TP %.0f %.0f %.0f\n"),
			Line.Coord.X, Line.Coord.Y, Line.Coord.Z, Below.X, Below.Y, Below.Z, Line.GenCeilings, Line.MergedCeilings, Line.InteriorCeilingsMax,
			OwnState ? ResidencyName(OwnState->Descriptor) : TEXT("-"), BelowState ? ResidencyName(BelowState->Descriptor) : TEXT("-"),
			OwnState ? OwnState->LODLevel : -1, BelowState ? BelowState->LODLevel : -1, Origin.Z,
			Origin.X + ChunkWorldSize * 0.5, Origin.Y + ChunkWorldSize * 0.5, Origin.Z + ChunkWorldSize);

		if (L >= RegenCount || !OwnState || !BelowState)
		{
			continue;
		}

		// Regenerate both chunks with the CPU generator, as the streaming would request them now.
		TArray<FVoxelData> CpuOwn, CpuBelow, GenOwnCopy, GenBelowCopy;
		{
			FVoxelCPUNoiseGenerator Cpu;
			Cpu.Initialize();
			FVoxelNoiseGenerationRequest Request;
			BuildChunkGenerationRequest(Line.Coord, OwnState->LODLevel, Request);
			Cpu.GenerateChunkCPU(Request, CpuOwn);
			FVoxelNoiseGenerationRequest RequestBelow;
			BuildChunkGenerationRequest(Below, BelowState->LODLevel, RequestBelow);
			Cpu.GenerateChunkCPU(RequestBelow, CpuBelow);
			Cpu.Shutdown();
		}
		const TArray<FVoxelData>* GenOwn = ReadGenerated(OwnState->Descriptor, GenOwnCopy);
		const TArray<FVoxelData>* GenBelow = ReadGenerated(BelowState->Descriptor, GenBelowCopy);
		if (!GenOwn || !GenBelow || CpuOwn.Num() != Volume || CpuBelow.Num() != Volume)
		{
			continue;
		}
		int32 SolidDiffOwn = 0, SolidDiffBelow = 0, DensityDiffOwn = 0, DensityDiffBelow = 0, CpuBoundaryCeilings = 0;
		int32 SolidDiffOwnBottom = 0, SolidDiffBelowTop = 0;
		for (int32 I = 0; I < Volume; ++I)
		{
			const bool bOwnDiff = (*GenOwn)[I].IsSolid() != CpuOwn[I].IsSolid();
			const bool bBelowDiff = (*GenBelow)[I].IsSolid() != CpuBelow[I].IsSolid();
			SolidDiffOwn += bOwnDiff ? 1 : 0;
			SolidDiffBelow += bBelowDiff ? 1 : 0;
			DensityDiffOwn += (*GenOwn)[I].Density != CpuOwn[I].Density ? 1 : 0;
			DensityDiffBelow += (*GenBelow)[I].Density != CpuBelow[I].Density ? 1 : 0;
			SolidDiffOwnBottom += (bOwnDiff && I < Slice) ? 1 : 0;
			SolidDiffBelowTop += (bBelowDiff && I >= (CS - 1) * Slice) ? 1 : 0;
		}
		for (int32 I = 0; I < Slice; ++I)
		{
			CpuBoundaryCeilings += (!CpuBelow[I + (CS - 1) * Slice].IsSolid() && CpuOwn[I].IsSolid()) ? 1 : 0;
		}
		OutReport += FString::Printf(TEXT("    CPU regeneration: %d boundary ceilings; solid differs in %d voxels of the chunk (%d in its bottom layer) and %d of the one below (%d in its top layer); density differs in %d / %d\n"),
			CpuBoundaryCeilings, SolidDiffOwn, SolidDiffOwnBottom, SolidDiffBelow, SolidDiffBelowTop, DensityDiffOwn, DensityDiffBelow);

		if (!bHeightmap || !WM)
		{
			continue;
		}
		// The analytic side of each ceiling column: the generated terrain height there (conditioning zones applied) and the
		// cave carve at the voxel under the plane and the voxel on it.
		TArray<FVoxelConditioningZone> Zones;
		GatherConditioningZonesForChunk(Line.Coord, Zones);
		double SumHeight = 0.0, SumCarveBelow = 0.0, SumCarveAbove = 0.0, SumDensityBelow = 0.0, SumDensityAbove = 0.0;
		float MinHeight = TNumericLimits<float>::Max(), MaxHeight = TNumericLimits<float>::Lowest();
		int32 Columns = 0, CarvedBelow = 0, CarvedAbove = 0, CaveFlagBelow = 0, WaterFlagBelow = 0, UndergroundBelow = 0;
		int32 ProfileI = -1;
		for (int32 I = 0; I < Slice; ++I)
		{
			const FVoxelData& VBelow = (*GenBelow)[I + (CS - 1) * Slice];
			const FVoxelData& VAbove = (*GenOwn)[I];
			if (VBelow.IsSolid() || !VAbove.IsSolid())
			{
				continue;
			}
			const int32 X = I % CS, Y = I / CS;
			const FVector PAbove = Origin + FVector(X * VoxelSize, Y * VoxelSize, 0.0);
			const FVector PBelow = PAbove - FVector(0.0, 0.0, VoxelSize);
			float Height = WM->GetTerrainHeightAt(static_cast<float>(PAbove.X), static_cast<float>(PAbove.Y), Configuration->NoiseParams);
			if (Zones.Num() > 0)
			{
				Height = FVoxelTerrainConditioning::ApplyToHeight(PAbove.X, PAbove.Y, Height, Zones);
			}
			const bool bUnderwater = Configuration->bEnableWaterLevel && Height < Configuration->WaterLevel;
			const float CarveBelow = bCaves ? FVoxelCaveQuery::SampleCaveDensityAt(PBelow, Height, VoxelSize, VBelow.BiomeID, Configuration->CaveConfiguration, Seed, bUnderwater) : 0.0f;
			const float CarveAbove = bCaves ? FVoxelCaveQuery::SampleCaveDensityAt(PAbove, Height, VoxelSize, VAbove.BiomeID, Configuration->CaveConfiguration, Seed, bUnderwater) : 0.0f;
			SumHeight += Height;
			MinHeight = FMath::Min(MinHeight, Height);
			MaxHeight = FMath::Max(MaxHeight, Height);
			SumCarveBelow += CarveBelow;
			SumCarveAbove += CarveAbove;
			SumDensityBelow += VBelow.Density;
			SumDensityAbove += VAbove.Density;
			CarvedBelow += CarveBelow > 0.0f ? 1 : 0;
			CarvedAbove += CarveAbove > 0.0f ? 1 : 0;
			CaveFlagBelow += VBelow.HasCaveFlag() ? 1 : 0;
			WaterFlagBelow += VBelow.HasWaterFlag() ? 1 : 0;
			UndergroundBelow += VBelow.HasUndergroundFlag() ? 1 : 0;
			if (ProfileI < 0 || FMath::Abs(X - CS / 2) + FMath::Abs(Y - CS / 2) < FMath::Abs(ProfileI % CS - CS / 2) + FMath::Abs(ProfileI / CS - CS / 2))
			{
				ProfileI = I; // the ceiling column nearest the chunk's centre, profiled below
			}
			++Columns;
		}
		if (Columns == 0)
		{
			continue;
		}
		OutReport += FString::Printf(TEXT("    ceiling columns %d: terrain height mean %.0f (%.0f..%.0f); cave carve under the plane mean %.3f (%d > 0), on it %.3f (%d > 0); density under %.0f, on %.0f; flags under: cave %d, water %d, underground %d\n"),
			Columns, SumHeight / Columns, MinHeight, MaxHeight, SumCarveBelow / Columns, CarvedBelow, SumCarveAbove / Columns, CarvedAbove,
			SumDensityBelow / Columns, SumDensityAbove / Columns, CaveFlagBelow, WaterFlagBelow, UndergroundBelow);

		// One column's profile across the plane: generated / CPU density and the cave carve, 8 voxels each side.
		const int32 X = ProfileI % CS, Y = ProfileI / CS;
		const FVector Column = Origin + FVector(X * VoxelSize, Y * VoxelSize, 0.0);
		float Height = WM->GetTerrainHeightAt(static_cast<float>(Column.X), static_cast<float>(Column.Y), Configuration->NoiseParams);
		if (Zones.Num() > 0)
		{
			Height = FVoxelTerrainConditioning::ApplyToHeight(Column.X, Column.Y, Height, Zones);
		}
		const bool bUnderwater = Configuration->bEnableWaterLevel && Height < Configuration->WaterLevel;
		FString Profile;
		for (int32 DZ = -8; DZ < 8; ++DZ)
		{
			const bool bInBelow = DZ < 0;
			const int32 LZ = bInBelow ? CS + DZ : DZ;
			const int32 Index = X + Y * CS + LZ * Slice;
			const FVoxelData& G = bInBelow ? (*GenBelow)[Index] : (*GenOwn)[Index];
			const FVoxelData& C = bInBelow ? CpuBelow[Index] : CpuOwn[Index];
			const FVector P = Column + FVector(0.0, 0.0, DZ * VoxelSize);
			const float Carve = bCaves ? FVoxelCaveQuery::SampleCaveDensityAt(P, Height, VoxelSize, G.BiomeID, Configuration->CaveConfiguration, Seed, bUnderwater) : 0.0f;
			Profile += FString::Printf(TEXT(" [z%+d %d/%d c%.2f]"), DZ, G.Density, C.Density, Carve);
		}
		OutReport += FString::Printf(TEXT("    column (%.0f, %.0f) terrain %.0f, generated/CPU density and carve by voxel from the plane:%s\n"),
			Column.X, Column.Y, Height, *Profile);
	}
	return GenCeil[0];
}

// Console: voxel.Gen.LayerAudit [maxLines=10] [regen=4]
// World modes review G34: ceilings (air under solid) and floors per Z plane over every loaded chunk, the chunk-boundary plane
// separately, from the generated voxels and with edits; the chunks with the most boundary ceilings, regenerated on the CPU and
// compared with what streamed, with the cave carve on both sides of each such column. Logged at Warning on LogVoxelStreaming.
static FAutoConsoleCommandWithWorldAndArgs GVoxelGenLayerAuditCmd(
	TEXT("voxel.Gen.LayerAudit"),
	TEXT("Audit cave ceilings and floors per Z plane of the loaded chunks, the chunk-layer boundary separately, and compare the chunks ")
	TEXT("with the most boundary ceilings with a CPU regeneration (world modes review G34). Optional args: chunks to list, chunks to regenerate."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UWorld* TargetWorld = (World && (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game)) ? World : nullptr;
		if (!TargetWorld && GEngine)
		{
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				if (Ctx.World() && (Ctx.WorldType == EWorldType::PIE || Ctx.WorldType == EWorldType::Game))
				{
					TargetWorld = Ctx.World();
					break;
				}
			}
		}
		if (!TargetWorld)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Gen.LayerAudit: no PIE/Game world found (start PIE first)"));
			return;
		}
		const int32 MaxLines = Args.Num() > 0 ? FMath::Max(0, FCString::Atoi(*Args[0])) : 10;
		const int32 Regen = Args.Num() > 1 ? FMath::Max(0, FCString::Atoi(*Args[1])) : 4;
		for (TActorIterator<AActor> It(TargetWorld); It; ++It)
		{
			if (UVoxelChunkManager* CM = It->FindComponentByClass<UVoxelChunkManager>())
			{
				FString Report;
				CM->AuditLayerBoundaries(MaxLines, Regen, Report);
				TArray<FString> ReportLines;
				Report.ParseIntoArrayLines(ReportLines);
				for (const FString& ReportLine : ReportLines)
				{
					UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Gen.LayerAudit [%s]: %s"), *It->GetName(), *ReportLine);
				}
			}
		}
	}));
