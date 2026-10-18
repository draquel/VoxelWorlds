// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Algo/AllOf.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterTypes.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelSeaSurface.h"
#include "VoxelEditTypes.h"
#include "VoxelMaterialRegistry.h"
#include "IVoxelWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// World modes review G29 (WORLD_MODES_REVIEW_PLAN.md §14.1, §19.6): a planet chunk's line top. The lines of
// UVoxelScatterManager::ExtractSurfacePointsAlongUp step along the axis nearest local up; a line's first solid voxel whose
// voxel above lies outside the chunk (over the upper face, or on a side the line drifts in through) was kept whenever its
// crossing lay within 1.5 voxels of the analytic surface, and put on that surface. Where the ground continues into the
// neighbour that voxel is solid and the neighbour makes the point, so the band where the surface crosses a chunk face got
// two points on it. That voxel is now the one the planet generator made there (the generated altitude along its
// direction), with the neighbour's edits applied. Real planet chunks from the CPU generator (a 250,000 uu planet like
// Review_Planet, voxel 75, chunk 32, as VoxelScatterPlanetTests).
//
// - Duplicates: the 27 chunks around the poles, 45 degrees, the equator and a cube corner: no two chunks put an open point
//   on the same up-line (but rarely where their lines do not continue into each other), and the local density has no
//   doubled strip, nor a strip without points but at the cube corner (G31). With the voxel above taken for air (an all-air
//   touching layer, the old rule's decision) the band's line tops come back as second points.
// - FlatPad: a flat pad at the north pole at heights around a chunk face (inside the lower chunk's top cell, at the face,
//   in the band, past it): every column of the pad has exactly one open point, on the pad, and the lower chunk's top layer
//   has none where the pad lies over the face. With the voxel above taken for air the band's columns have two.
// - Edits: the neighbours a line top reads (the upper face, the sides the lines drift in through, their edge); the voxel
//   outside the chunk equals the neighbour's generated and edit-merged voxel on every touching layer; a dig through the
//   upper chunk's bottom layer exposes the face (open surface) and a build over a face that was the surface covers it;
//   the scatter manager's own pipeline reads the neighbour's edit layer (SetEditLayerSource).
// ---------------------------------------------------------------------------

namespace VoxelScatterLineTopTestUtils
{
	constexpr float Radius = 250000.0f;
	constexpr float VoxelSize = 75.0f;
	constexpr int32 ChunkSize = 32;
	constexpr double ChunkWorld = ChunkSize * VoxelSize;

	/** VoxelScatterPlanetTests' planet: no biomes, caves or water. */
	UVoxelWorldConfiguration* MakePlanetConfig()
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::SphericalPlanet;
		Config->WorldOrigin = FVector(1000.0, -2000.0, 500.0);
		Config->WorldRadius = Radius;
		Config->PlanetHeightScale = 3000.0f;
		Config->BaseHeight = 0.0f;
		Config->PlanetMaxTerrainHeight = 4000.0f;
		Config->PlanetMaxTerrainDepth = 4000.0f;
		Config->VoxelSize = VoxelSize;
		Config->ChunkSize = ChunkSize;
		Config->bEnableBiomes = false;
		Config->bEnableCaves = false;
		Config->bEnableWaterLevel = false;
		Config->NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
		Config->NoiseParams.Seed = 248675;
		Config->NoiseParams.Octaves = 3;
		Config->NoiseParams.Frequency = 0.000025f;
		Config->NoiseParams.Lacunarity = 1.681f;
		Config->NoiseParams.Persistence = 0.5f;
		return Config;
	}

	FVector ChunkOrigin(const UVoxelWorldConfiguration* Config, const FIntVector& Coord)
	{
		return Config->WorldOrigin + FVector(Coord) * ChunkWorld;
	}

	FIntVector ChunkOf(const UVoxelWorldConfiguration* Config, const FVector& WorldPos)
	{
		const FVector Local = (WorldPos - Config->WorldOrigin) / ChunkWorld;
		return FIntVector(FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), FMath::FloorToInt32(Local.Z));
	}

	FVector UpAtChunk(const UVoxelWorldConfiguration* Config, const IVoxelWorldMode& Mode, const FIntVector& Coord)
	{
		return Mode.GetUpVector(ChunkOrigin(Config, Coord) + FVector(0.5 * ChunkWorld));
	}

	TArray<FIntVector> ChunksAround(const FIntVector& Center)
	{
		TArray<FIntVector> Coords;
		for (int32 DZ = -1; DZ <= 1; ++DZ)
		for (int32 DY = -1; DY <= 1; ++DY)
		for (int32 DX = -1; DX <= 1; ++DX)
		{
			Coords.Add(Center + FIntVector(DX, DY, DZ));
		}
		return Coords;
	}

	/** A CPU-generated planet chunk with its edit layer merged in, as the chunk manager hands scatter its voxels. */
	TArray<FVoxelData> GenerateChunk(FVoxelCPUNoiseGenerator& Generator, const UVoxelWorldConfiguration* Config, const FIntVector& Coord,
		const TArray<FVoxelConditioningZone>& Zones, const FChunkEditLayer* Layer)
	{
		FVoxelNoiseGenerationRequest Request;
		Request.ChunkCoord = Coord;
		Request.ChunkSize = ChunkSize;
		Request.VoxelSize = VoxelSize;
		Request.NoiseParams = Config->NoiseParams;
		FVoxelWorldModeFactory::FillGenerationRequest(*Config, Request);
		Request.ConditioningZones = Zones;
		TArray<FVoxelData> Voxels;
		Generator.GenerateChunkCPU(Request, Voxels);
		if (Layer)
		{
			for (const TPair<int32, FVoxelEdit>& Pair : Layer->Edits)
			{
				Voxels[Pair.Key] = Pair.Value.ApplyToProceduralData(Voxels[Pair.Key]);
			}
		}
		return Voxels;
	}

	/** Every voxel of the layers a chunk's line tops read set to air: the voxel above always open, the old rule's decision. */
	TSharedPtr<const TMap<FIntVector, FVoxelEdit>> AllAirTouching(const FVector& Up)
	{
		TArray<FIntVector> Offsets;
		UVoxelScatterManager::GetPlanetLineTopNeighbours(Up, ChunkSize, Offsets);
		TMap<FIntVector, FVoxelEdit> Map;
		for (int32 Z = -1; Z <= ChunkSize; ++Z)
		for (int32 Y = -1; Y <= ChunkSize; ++Y)
		for (int32 X = -1; X <= ChunkSize; ++X)
		{
			const FIntVector Offset(X < 0 ? -1 : (X >= ChunkSize ? 1 : 0), Y < 0 ? -1 : (Y >= ChunkSize ? 1 : 0), Z < 0 ? -1 : (Z >= ChunkSize ? 1 : 0));
			if (Offsets.Contains(Offset))
			{
				const FIntVector Local(X, Y, Z);
				Map.Add(Local, FVoxelEdit(Local - Offset * ChunkSize, FVoxelData::Air(), FVoxelData::Air(), EEditMode::Set));
			}
		}
		return MakeShared<const TMap<FIntVector, FVoxelEdit>>(MoveTemp(Map));
	}

	enum class EVoxelAbove : uint8
	{
		/** The line top reads the generator's voxel outside the chunk with the neighbours' edits (the scatter manager's context). */
		Decided,
		/** Every voxel there taken for air (AllAirTouching). */
		AllAir,
	};

	struct FChunkPoints
	{
		FIntVector Coord;
		TArray<FVoxelSurfacePoint> Points;
	};

	/**
	 * Each chunk generated (GenerationZones, its layer in Edits merged in) and extracted along local up with a context like
	 * the scatter manager's: ContextZones and the neighbours' touching edits (GatherPlanetTouchingEdits over Edits), or an
	 * all-air touching layer.
	 */
	TArray<FChunkPoints> ExtractChunks(const UVoxelWorldConfiguration* Config, const IVoxelWorldMode& Mode, const TArray<FIntVector>& Coords,
		const TArray<FVoxelConditioningZone>& GenerationZones, const TArray<FVoxelConditioningZone>& ContextZones,
		const TMap<FIntVector, FChunkEditLayer>& Edits, EVoxelAbove Above)
	{
		FVoxelCPUNoiseGenerator Generator;
		Generator.Initialize();
		TArray<FChunkPoints> Out;
		for (const FIntVector& Coord : Coords)
		{
			FScatterExtractionContext Context;
			Context.WorldMode = &Mode;
			Context.NoiseParams = Config->NoiseParams;
			Context.Sea = FVoxelSeaSurface::FromConfig(*Config);
			if (ContextZones.Num() > 0)
			{
				Context.ConditioningZones = MakeShared<const TArray<FVoxelConditioningZone>>(ContextZones);
			}
			const FVector Up = UpAtChunk(Config, Mode, Coord);
			Context.TouchingEdits = Above == EVoxelAbove::AllAir
				? AllAirTouching(Up)
				: UVoxelScatterManager::GatherPlanetTouchingEdits(Up, Coord, ChunkSize, [&Edits](const FIntVector& C) { return Edits.Find(C); });
			const TArray<FVoxelData> Voxels = GenerateChunk(Generator, Config, Coord, GenerationZones, Edits.Find(Coord));
			FChunkSurfaceData Surface;
			UVoxelScatterManager::ExtractPlanetSurfacePoints(Voxels, Coord, ChunkOrigin(Config, Coord), ChunkSize, VoxelSize, VoxelSize, Context, Surface);
			Out.Add({ Coord, MoveTemp(Surface.SurfacePoints) });
		}
		Generator.Shutdown();
		return Out;
	}

	/**
	 * Pairs of open points on one up-line: closer than half a voxel across local up and within two voxels along it (a
	 * second point on a line top; neighbouring lines lie about a voxel apart across). Also the probe's measure (§19.4):
	 * open points from different chunks within half a voxel.
	 */
	struct FPairCounts
	{
		int32 Open = 0;
		int32 CrossChunk = 0;
		int32 SameChunk = 0;
		int32 NearCrossChunk = 0;
	};

	FPairCounts CountUpLinePairs(const TArray<FChunkPoints>& Chunks)
	{
		struct FEntry { FVector Position; FVector Up; int32 Chunk; };
		TArray<FEntry> Entries;
		for (int32 C = 0; C < Chunks.Num(); ++C)
		{
			for (const FVoxelSurfacePoint& Point : Chunks[C].Points)
			{
				if (!Point.bIsUnderground)
				{
					Entries.Add({ Point.Position, Point.LocalUp, C });
				}
			}
		}
		const double Cell = 2.0 * VoxelSize;
		auto CellOf = [Cell](const FVector& P)
		{
			return FIntVector(FMath::FloorToInt32(P.X / Cell), FMath::FloorToInt32(P.Y / Cell), FMath::FloorToInt32(P.Z / Cell));
		};
		TMap<FIntVector, TArray<int32>> Grid;
		for (int32 I = 0; I < Entries.Num(); ++I)
		{
			Grid.FindOrAdd(CellOf(Entries[I].Position)).Add(I);
		}
		FPairCounts Counts;
		Counts.Open = Entries.Num();
		for (int32 I = 0; I < Entries.Num(); ++I)
		{
			const FEntry& A = Entries[I];
			const FIntVector Home = CellOf(A.Position);
			for (int32 DZ = -1; DZ <= 1; ++DZ)
			for (int32 DY = -1; DY <= 1; ++DY)
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				const TArray<int32>* Bucket = Grid.Find(Home + FIntVector(DX, DY, DZ));
				if (!Bucket)
				{
					continue;
				}
				for (const int32 J : *Bucket)
				{
					if (J <= I)
					{
						continue;
					}
					const FEntry& B = Entries[J];
					const FVector D = B.Position - A.Position;
					const double Along = FVector::DotProduct(D, A.Up);
					const double Across = (D - A.Up * Along).Size();
					if (A.Chunk != B.Chunk && D.Size() < 0.5 * VoxelSize)
					{
						++Counts.NearCrossChunk;
					}
					if (Across < 0.5 * VoxelSize && FMath::Abs(Along) < 2.0 * VoxelSize)
					{
						(A.Chunk == B.Chunk ? Counts.SameChunk : Counts.CrossChunk) += 1;
					}
				}
			}
		}
		return Counts;
	}

	/**
	 * The open points' weighted density at samples on the surface every half voxel over a disc of 2,200 uu around a site
	 * (inside the 27 chunks, clear of their outer faces), per the column scan's one point per voxel square: each point's
	 * spawn weight under a Gaussian of one voxel across local up (within 3 voxels across and along). A kernel as wide as the
	 * lines' spacing is flat over any regular sampling, so a strip of doubled line tops shows as samples over 1.2 and a strip
	 * without points as samples under 0.8.
	 */
	struct FLocalDensity
	{
		int32 Samples = 0;
		double Mean = 0.0;
		int32 Under = 0;
		int32 Over = 0;
	};

	FLocalDensity MeasureLocalDensity(const UVoxelWorldConfiguration* Config, const IVoxelWorldMode& Mode, const FVector& SitePoint,
		const TArray<FChunkPoints>& Chunks)
	{
		const FVector N = Mode.GetUpVector(SitePoint);
		const FVector T1 = FVector::CrossProduct(FMath::Abs(N.Z) < 0.9 ? FVector::UpVector : FVector::ForwardVector, N).GetSafeNormal();
		const FVector T2 = FVector::CrossProduct(N, T1);
		const double Disc = 2200.0, Sigma = VoxelSize, Reach = 3.0 * VoxelSize, Step = 0.5 * VoxelSize, Cell = 3.0 * VoxelSize;
		auto CellOf = [&](const FVector& P)
		{
			const FVector O = P - SitePoint;
			return FIntPoint(FMath::FloorToInt32(FVector::DotProduct(O, T1) / Cell), FMath::FloorToInt32(FVector::DotProduct(O, T2) / Cell));
		};
		TMap<FIntPoint, TArray<const FVoxelSurfacePoint*>> Grid;
		for (const FChunkPoints& Chunk : Chunks)
		{
			for (const FVoxelSurfacePoint& Point : Chunk.Points)
			{
				if (!Point.bIsUnderground)
				{
					Grid.FindOrAdd(CellOf(Point.Position)).Add(&Point);
				}
			}
		}
		FLocalDensity Result;
		double Sum = 0.0;
		// One point per voxel square under the kernel: 2 pi sigma^2 / VoxelSize^2.
		const double PerSample = 2.0 * UE_DOUBLE_PI * Sigma * Sigma / (VoxelSize * VoxelSize);
		for (double U = -Disc; U <= Disc; U += Step)
		{
			for (double W = -Disc; W <= Disc; W += Step)
			{
				if (U * U + W * W > Disc * Disc)
				{
					continue;
				}
				const FVector Sample = Mode.GetSurfacePoint(SitePoint + T1 * U + T2 * W, Config->NoiseParams).Position;
				const FVector Up = Mode.GetUpVector(Sample);
				const FIntPoint Home = CellOf(Sample);
				double Weighted = 0.0;
				for (int32 DY = -1; DY <= 1; ++DY)
				for (int32 DX = -1; DX <= 1; ++DX)
				{
					if (const TArray<const FVoxelSurfacePoint*>* Bucket = Grid.Find(Home + FIntPoint(DX, DY)))
					{
						for (const FVoxelSurfacePoint* Point : *Bucket)
						{
							const FVector D = Point->Position - Sample;
							const double Along = FVector::DotProduct(D, Up);
							const double Across = (D - Up * Along).Size();
							if (Across <= Reach && FMath::Abs(Along) < 3.0 * VoxelSize)
							{
								Weighted += Point->SpawnWeight * FMath::Exp(-0.5 * FMath::Square(Across / Sigma));
							}
						}
					}
				}
				const double Ratio = Weighted / PerSample;
				++Result.Samples;
				Sum += Ratio;
				Result.Under += Ratio < 0.8 ? 1 : 0;
				Result.Over += Ratio > 1.2 ? 1 : 0;
			}
		}
		Result.Mean = Result.Samples > 0 ? Sum / Result.Samples : 0.0;
		return Result;
	}

	TArray<TPair<const TCHAR*, FVector>> Sites()
	{
		const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
		return {
			{ TEXT("north pole"), FVector(0, 0, 1) },
			{ TEXT("south pole"), FVector(0, 0, -1) },
			{ TEXT("45 degrees"), FVector(C45, 0, C45) },
			{ TEXT("equator"), FVector(0, 1, 0) },
			{ TEXT("cube corner"), FVector(1, 1, 1).GetSafeNormal() },
		};
	}

	// ---- The flat pad at the north pole ----

	/** The pole lies on the corner of four chunk columns; the face between chunk layers 103 and 104 crosses it. */
	const FIntVector PadChunks[] = {
		FIntVector(-1, -1, 103), FIntVector(0, -1, 103), FIntVector(-1, 0, 103), FIntVector(0, 0, 103),
		FIntVector(-1, -1, 104), FIntVector(0, -1, 104), FIntVector(-1, 0, 104), FIntVector(0, 0, 104) };
	constexpr int32 LowerLayer = 103;
	/** The face's altitude over the pole (it rises by d^2 / 2R at a distance d from the pole: 4 uu at the pad's edge). */
	constexpr double FaceAltitude = (LowerLayer + 1) * ChunkWorld - Radius;
	/** The pad's flat core and the columns judged (within PadJudged of the pole). */
	constexpr float PadCore = 2500.0f;
	constexpr double PadJudged = 2000.0;
	constexpr float Band = 1.5f * VoxelSize;

	FVoxelConditioningZone MakePad(double OffsetVoxels)
	{
		return FVoxelConditioningZone::MakeRadial(FVector(0, 0, 1), PadCore, 1000.0f, static_cast<float>(FaceAltitude + OffsetVoxels * VoxelSize));
	}

	/** The column (vertical voxel line, as the lines are around the pole) a position stands in, relative to the world origin. */
	FIntPoint ColumnOf(const UVoxelWorldConfiguration* Config, const FVector& Position)
	{
		return FIntPoint(FMath::RoundToInt32((Position.X - Config->WorldOrigin.X) / VoxelSize), FMath::RoundToInt32((Position.Y - Config->WorldOrigin.Y) / VoxelSize));
	}

	bool IsJudged(const FIntPoint& Column)
	{
		return FMath::Square(Column.X * VoxelSize) + FMath::Square(Column.Y * VoxelSize) <= FMath::Square(PadJudged);
	}

	TArray<FChunkPoints> ExtractPad(const UVoxelWorldConfiguration* Config, const IVoxelWorldMode& Mode, const FVoxelConditioningZone& Pad,
		const TMap<FIntVector, FChunkEditLayer>& Edits, EVoxelAbove Above, bool bEditsInContext = true)
	{
		const TArray<FVoxelConditioningZone> Zones = { Pad };
		if (bEditsInContext)
		{
			return ExtractChunks(Config, Mode, TArray<FIntVector>(PadChunks, UE_ARRAY_COUNT(PadChunks)), Zones, Zones, Edits, Above);
		}
		// The voxels carry the edits but the context does not (an edit-blind line top).
		TArray<FChunkPoints> Out;
		for (const FIntVector& Coord : PadChunks)
		{
			TMap<FIntVector, FChunkEditLayer> Own;
			if (const FChunkEditLayer* Layer = Edits.Find(Coord))
			{
				Own.Add(Coord, *Layer);
			}
			TArray<FChunkPoints> One = ExtractChunks(Config, Mode, { Coord }, Zones, Zones, Own, Above);
			// The chunk's own layer must not reach its neighbours' contexts: with only itself in Own, GatherPlanetTouchingEdits
			// finds nothing for a pole chunk (its line tops read the chunk above).
			Out.Append(MoveTemp(One));
		}
		return Out;
	}

	/** Over the pad's judged columns: open points per column, the worst distance from the expected surface, the lower top layer. */
	struct FPadResult
	{
		int32 Columns = 0;
		int32 ColumnsWithOne = 0;
		int32 ColumnsWithTwo = 0;
		int32 MaxOpenInColumn = 0;
		double WorstError = 0.0;
		/** The lower chunks' points (any class) in their top voxel cell in columns where the pad lies over the face within the band. */
		int32 BandTopLayer = 0;
	};

	/**
	 * ExpectedAltitude(Column): the surface each column should have, as an altitude (the pad's, or the edit-merged surface).
	 * OnlyColumns: judge only these (by default every judged column).
	 */
	FPadResult JudgePad(const UVoxelWorldConfiguration* Config, const IVoxelWorldMode& Mode, const TArray<FChunkPoints>& Chunks,
		TFunctionRef<double(const FIntPoint&)> ExpectedAltitude, double PadOffsetVoxels, const TSet<FIntPoint>* OnlyColumns = nullptr)
	{
		TMap<FIntPoint, TArray<double>> OpenAltitudes;
		FPadResult Result;
		// The pad over the face within the band at a column: the face rises away from the pole, the pad does not.
		auto PadInBand = [&](const FIntPoint& Column)
		{
			const FVector FacePoint = Config->WorldOrigin + FVector(Column.X * VoxelSize, Column.Y * VoxelSize, (LowerLayer + 1) * ChunkWorld);
			const double Over = FaceAltitude + PadOffsetVoxels * VoxelSize - Mode.GetAltitude(FacePoint);
			return Over >= 0.0 && Over <= Band;
		};
		for (const FChunkPoints& Chunk : Chunks)
		{
			const double LowerTop = ChunkOrigin(Config, Chunk.Coord).Z + ChunkWorld;
			for (const FVoxelSurfacePoint& Point : Chunk.Points)
			{
				const FIntPoint Column = ColumnOf(Config, Point.Position);
				if (!IsJudged(Column) || (OnlyColumns && !OnlyColumns->Contains(Column)))
				{
					continue;
				}
				if (!Point.bIsUnderground)
				{
					OpenAltitudes.FindOrAdd(Column).Add(Mode.GetAltitude(Point.Position));
				}
				if (Chunk.Coord.Z == LowerLayer && Point.Position.Z > LowerTop - VoxelSize + 0.01 && PadInBand(Column))
				{
					++Result.BandTopLayer;
				}
			}
		}
		for (int32 Y = -ChunkSize; Y < ChunkSize; ++Y)
		{
			for (int32 X = -ChunkSize; X < ChunkSize; ++X)
			{
				const FIntPoint Column(X, Y);
				if (!IsJudged(Column) || (OnlyColumns && !OnlyColumns->Contains(Column)))
				{
					continue;
				}
				++Result.Columns;
				const TArray<double>* Altitudes = OpenAltitudes.Find(Column);
				const int32 Count = Altitudes ? Altitudes->Num() : 0;
				Result.ColumnsWithOne += Count == 1 ? 1 : 0;
				Result.ColumnsWithTwo += Count == 2 ? 1 : 0;
				Result.MaxOpenInColumn = FMath::Max(Result.MaxOpenInColumn, Count);
				if (Altitudes)
				{
					const double Expected = ExpectedAltitude(Column);
					for (const double Altitude : *Altitudes)
					{
						Result.WorstError = FMath::Max(Result.WorstError, FMath::Abs(Altitude - Expected));
					}
				}
			}
		}
		return Result;
	}

	/** An edit layer on a pad chunk's layer at local Z (the upper chunks' bottom layer: 0) over the columns in Patch. */
	TMap<FIntVector, FChunkEditLayer> MakePatchEdits(const TSet<FIntPoint>& Patch, int32 ChunkLayer, int32 LocalZ, const FVoxelData& NewData)
	{
		TMap<FIntVector, FChunkEditLayer> Edits;
		for (const FIntPoint& Column : Patch)
		{
			const FIntVector Coord(FMath::FloorToInt32(Column.X / static_cast<float>(ChunkSize)), FMath::FloorToInt32(Column.Y / static_cast<float>(ChunkSize)), ChunkLayer);
			FChunkEditLayer& Layer = Edits.FindOrAdd(Coord, FChunkEditLayer(Coord, ChunkSize));
			const FIntVector Local(Column.X - Coord.X * ChunkSize, Column.Y - Coord.Y * ChunkSize, LocalZ);
			Layer.ApplyEdit(FVoxelEdit(Local, NewData, FVoxelData::Air(), EEditMode::Set));
		}
		return Edits;
	}

	/**
	 * The edit-merged surface's altitude over a pad column (the two chunk layers' voxels on that vertical line, the crossing
	 * under the highest solid voxel interpolated as the mesher does).
	 */
	double MergedSurfaceAltitude(const UVoxelWorldConfiguration* Config, const IVoxelWorldMode& Mode, const TMap<FIntVector, TArray<FVoxelData>>& Voxels,
		const FIntPoint& Column)
	{
		const FIntVector Lower(FMath::FloorToInt32(Column.X / static_cast<float>(ChunkSize)), FMath::FloorToInt32(Column.Y / static_cast<float>(ChunkSize)), LowerLayer);
		const int32 LX = Column.X - Lower.X * ChunkSize, LY = Column.Y - Lower.Y * ChunkSize;
		auto At = [&](int32 Z) -> const FVoxelData&
		{
			const FIntVector Coord = Z < ChunkSize ? Lower : Lower + FIntVector(0, 0, 1);
			return Voxels.FindChecked(Coord)[LX + LY * ChunkSize + (Z % ChunkSize) * ChunkSize * ChunkSize];
		};
		for (int32 Z = 2 * ChunkSize - 2; Z >= 0; --Z)
		{
			if (At(Z).IsSolid() && !At(Z + 1).IsSolid())
			{
				const float S = At(Z).Density, A = At(Z + 1).Density;
				const double Fraction = FMath::Clamp((static_cast<float>(VOXEL_SURFACE_THRESHOLD) - S) / (A - S), 0.0f, 1.0f);
				return Mode.GetAltitude(ChunkOrigin(Config, Lower) + FVector(LX, LY, Z + Fraction) * VoxelSize);
			}
		}
		return TNumericLimits<double>::Lowest();
	}

	/** Private game world + initialized scatter manager (CPU extraction) on the planet, torn down on destruction. */
	struct FPlanetHarness
	{
		UWorld* World = nullptr;
		UVoxelWorldConfiguration* Config = nullptr;
		UVoxelScatterManager* Manager = nullptr;

		explicit FPlanetHarness(UVoxelWorldConfiguration* InConfig)
			: Config(InConfig)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelScatterLineTopTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			Config->ScatterRadius = 100000.0f;
			Config->MeshingMode = EMeshingMode::MarchingCubes; // planets are smooth (the config's default is cubic)
			Manager = NewObject<UVoxelScatterManager>();
			Manager->AddToRoot();
			Manager->Initialize(Config, World);
		}

		~FPlanetHarness()
		{
			if (Manager)
			{
				Manager->SetWorldMode(nullptr);
				Manager->Shutdown();
				Manager->RemoveFromRoot();
			}
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
		}

		/** Submit a chunk as if its mesh just arrived, with the viewer at its centre, and pump until its extraction is done. */
		const FChunkSurfaceData* Run(const FIntVector& Coord, const TArray<FVoxelData>& Voxels)
		{
			const FVector Viewer = ChunkOrigin(Config, Coord) + FVector(0.5 * ChunkWorld);
			Manager->Update(Viewer, 0.0f);
			Manager->OnChunkMeshDataReady(Coord, Voxels, ChunkSize, VoxelSize);
			const double Deadline = FPlatformTime::Seconds() + 15.0;
			while (FPlatformTime::Seconds() < Deadline)
			{
				Manager->Update(Viewer, 0.05f);
				if (Manager->GetPendingGenerationCount() == 0)
				{
					break;
				}
			}
			return Manager->GetChunkSurfaceData(Coord);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterPlanetLineTopDuplicatesTest,
	"VoxelWorlds.Scatter.Planet.LineTop.Duplicates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterPlanetLineTopDuplicatesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterLineTopTestUtils;
	UVoxelWorldConfiguration* Config = MakePlanetConfig();
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	const TMap<FIntVector, FChunkEditLayer> NoEdits;
	int32 OldPairs = 0, OldOver = 0;
	for (const TPair<const TCHAR*, FVector>& Site : Sites())
	{
		const FVoxelTerrainSurfacePoint SiteSurface = Mode->GetSurfacePoint(Config->WorldOrigin + Site.Value, Config->NoiseParams);
		const TArray<FIntVector> Coords = ChunksAround(ChunkOf(Config, SiteSurface.Position));
		const TArray<FChunkPoints> NowChunks = ExtractChunks(Config, *Mode, Coords, {}, {}, NoEdits, EVoxelAbove::Decided);
		const TArray<FChunkPoints> OldChunks = ExtractChunks(Config, *Mode, Coords, {}, {}, NoEdits, EVoxelAbove::AllAir);
		const FPairCounts Now = CountUpLinePairs(NowChunks);
		const FPairCounts Old = CountUpLinePairs(OldChunks);
		const FLocalDensity NowDensity = MeasureLocalDensity(Config, *Mode, SiteSurface.Position, NowChunks);
		const FLocalDensity OldDensity = MeasureLocalDensity(Config, *Mode, SiteSurface.Position, OldChunks);
		OldPairs += Old.CrossChunk;
		OldOver += OldDensity.Over;
		AddInfo(FString::Printf(TEXT("%s: %d open points; pairs on one up-line from two chunks %d, within one chunk %d; within half a voxel from two chunks %d. With the voxel above taken for air: %d open points, %d pairs from two chunks (%d within one)"),
			Site.Key, Now.Open, Now.CrossChunk, Now.SameChunk, Now.NearCrossChunk, Old.Open, Old.CrossChunk, Old.SameChunk));
		AddInfo(FString::Printf(TEXT("%s: local density over %d samples: mean %.3f, %d under 0.8, %d over 1.2; with the voxel above taken for air mean %.3f, %d under, %d over"),
			Site.Key, NowDensity.Samples, NowDensity.Mean, NowDensity.Under, NowDensity.Over, OldDensity.Mean, OldDensity.Under, OldDensity.Over));
		TestEqual(*FString::Printf(TEXT("%s: no strip of doubled points (local density over 1.2)"), Site.Key), NowDensity.Over, 0);
		if (FCString::Strcmp(Site.Key, TEXT("cube corner")) != 0)
		{
			// At the cube corner neighbouring chunks step their lines along different axes, and strips along some faces get no
			// point from either (G31, WORLD_MODES_REVIEW_PLAN.md section 19.6): reported above, not judged here.
			TestEqual(*FString::Printf(TEXT("%s: no strip without points (local density under 0.8)"), Site.Key), NowDensity.Under, 0);
		}
		TestTrue(*FString::Printf(TEXT("%s: the surface has points (non-vacuous)"), Site.Key), Now.Open >= 2000);
		// Each chunk's lines follow local up at its own centre, so a neighbour's lines do not always continue into its own:
		// rarely two lines from two chunks cross the surface within a voxel where it lies along their face (an interior
		// crossing in one, a line top that correctly found air above in the other). That is not a line top's decision.
		TestTrue(*FString::Printf(TEXT("%s: two chunks put open points on one up-line at most once per 1,000 points (%d)"), Site.Key, Now.CrossChunk),
			Now.CrossChunk * 1000 <= Now.Open);
		TestEqual(*FString::Printf(TEXT("%s: one chunk never puts two open points on one up-line"), Site.Key), Now.SameChunk, 0);
	}
	AddInfo(FString::Printf(TEXT("Over the sites, taking the voxel above for air doubles %d line tops (%d samples over 1.2)"), OldPairs, OldOver));
	TestTrue(TEXT("Taking the voxel above for air doubles line tops where the surface crosses a chunk face (the test reproduces G29)"), OldPairs >= 500 && OldOver > 0);
	Config->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterPlanetLineTopFlatPadTest,
	"VoxelWorlds.Scatter.Planet.LineTop.FlatPad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterPlanetLineTopFlatPadTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterLineTopTestUtils;
	UVoxelWorldConfiguration* Config = MakePlanetConfig();
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	const TMap<FIntVector, FChunkEditLayer> NoEdits;

	// The pad's height over the face, in voxels: inside the lower chunk's top cell, at the face, in the band, past it.
	for (const double Offset : { -0.9, -0.5, -0.1, -0.01, 0.0, 0.01, 0.3, 0.7, 1.0, 1.4, 1.6, 3.0 })
	{
		const FVoxelConditioningZone Pad = MakePad(Offset);
		const double PadAltitude = Pad.TargetHeight;
		auto OnPad = [PadAltitude](const FIntPoint&) { return PadAltitude; };
		const FPadResult Now = JudgePad(Config, *Mode, ExtractPad(Config, *Mode, Pad, NoEdits, EVoxelAbove::Decided), OnPad, Offset);
		const FPadResult Old = JudgePad(Config, *Mode, ExtractPad(Config, *Mode, Pad, NoEdits, EVoxelAbove::AllAir), OnPad, Offset);
		AddInfo(FString::Printf(TEXT("Pad %+.2f voxels over the face: %d of %d columns with one open point (most %d), worst %.2f uu off the pad, %d points on the lower top layer under the band's pad; the voxel above taken for air: %d columns with two"),
			Offset, Now.ColumnsWithOne, Now.Columns, Now.MaxOpenInColumn, Now.WorstError, Now.BandTopLayer, Old.ColumnsWithTwo));
		TestTrue(*FString::Printf(TEXT("Pad %+.2f: columns judged (non-vacuous)"), Offset), Now.Columns >= 2000);
		TestEqual(*FString::Printf(TEXT("Pad %+.2f: one open point per column"), Offset), Now.ColumnsWithOne, Now.Columns);
		TestTrue(*FString::Printf(TEXT("Pad %+.2f: on the pad (worst %.2f uu)"), Offset, Now.WorstError), Now.WorstError <= 2.0);
		TestEqual(*FString::Printf(TEXT("Pad %+.2f: no point on the lower chunks' top layer under the pad"), Offset), Now.BandTopLayer, 0);
		if (Offset > 0.05 && Offset <= 1.0)
		{
			// The old rule kept a band crossing whose assumed-air interpolation lay within 1.5 voxels of the pad: over the face
			// by up to a voxel (the crossing sits half a voxel under the face).
			TestEqual(*FString::Printf(TEXT("Pad %+.2f: taking the voxel above for air gave every column two open points (the test reproduces G29)"), Offset),
				Old.ColumnsWithTwo, Old.Columns);
		}
	}
	Config->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterPlanetLineTopEditsTest,
	"VoxelWorlds.Scatter.Planet.LineTop.Edits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterPlanetLineTopEditsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterLineTopTestUtils;
	UVoxelWorldConfiguration* Config = MakePlanetConfig();
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);

	// The neighbours a line top reads: over the upper face along the axis nearest up, the sides the lines drift in through
	// (against the down direction's slope), and their edge.
	{
		const double C30 = FMath::Cos(FMath::DegreesToRadians(30.0)), S30 = FMath::Sin(FMath::DegreesToRadians(30.0));
		struct FCase
		{
			FVector Up;
			TArray<FIntVector> Expected;
		};
		const FCase Cases[] = {
			{ FVector(0, 0, 1), { FIntVector(0, 0, 1) } },
			{ FVector(0, 0, -1), { FIntVector(0, 0, -1) } },
			// Lines step along X and drift toward -Z (0.58 voxels a step): they enter through the +Z side, and the step before a
			// line's first voxel lies over the upper face's edge with it.
			{ FVector(C30, 0, S30), { FIntVector(1, 0, 0), FIntVector(1, 0, 1), FIntVector(0, 0, 1) } },
			// Drifting a voxel a step on both other axes: both sides, their edge, and the upper face's edges and corner.
			{ FVector(1, 1, 1).GetSafeNormal(), { FIntVector(1, 0, 0), FIntVector(1, 1, 0), FIntVector(1, 0, 1), FIntVector(1, 1, 1),
				FIntVector(0, 1, 0), FIntVector(0, 0, 1), FIntVector(0, 1, 1) } },
			{ FVector(-1, 1, -1).GetSafeNormal(), { FIntVector(-1, 0, 0), FIntVector(-1, 1, 0), FIntVector(-1, 0, -1), FIntVector(-1, 1, -1),
				FIntVector(0, 1, 0), FIntVector(0, 0, -1), FIntVector(0, 1, -1) } },
		};
		for (const FCase& Case : Cases)
		{
			TArray<FIntVector> Offsets;
			UVoxelScatterManager::GetPlanetLineTopNeighbours(Case.Up, ChunkSize, Offsets);
			const bool bSame = Offsets.Num() == Case.Expected.Num() && Algo::AllOf(Case.Expected, [&Offsets](const FIntVector& O) { return Offsets.Contains(O); });
			TestTrue(*FString::Printf(TEXT("Up %s: the line tops read %d neighbour(s) as expected"), *Case.Up.ToString(), Case.Expected.Num()), bSame);
		}
	}

	// The voxel outside a chunk is the neighbour's voxel: generated (natural terrain), and with edits of every mode on the
	// touching layers (and edits deeper in the neighbours, and in a neighbour no line top reads, which must not count).
	{
		FVoxelCPUNoiseGenerator Generator;
		Generator.Initialize();
		const TArray<FVoxelConditioningZone> NoZones;
		for (const TPair<const TCHAR*, FVector>& Site : Sites())
		{
			const FVoxelTerrainSurfacePoint SiteSurface = Mode->GetSurfacePoint(Config->WorldOrigin + Site.Value, Config->NoiseParams);
			const FIntVector Coord = ChunkOf(Config, SiteSurface.Position);
			const FVector Up = UpAtChunk(Config, *Mode, Coord);
			TArray<FIntVector> Offsets;
			UVoxelScatterManager::GetPlanetLineTopNeighbours(Up, ChunkSize, Offsets);
			TMap<FIntVector, FChunkEditLayer> Edits;
			int32 TouchingEditCount = 0;
			for (const FIntVector& Offset : Offsets)
			{
				FChunkEditLayer& Layer = Edits.Add(Coord + Offset, FChunkEditLayer(Coord + Offset, ChunkSize));
				int32 N = 0;
				for (int32 Z = 0; Z < ChunkSize; ++Z)
				for (int32 Y = 0; Y < ChunkSize; ++Y)
				for (int32 X = 0; X < ChunkSize; ++X)
				{
					const FIntVector InThis = FIntVector(X, Y, Z) + Offset * ChunkSize;
					const bool bTouching = InThis.GetMin() >= -1 && InThis.GetMax() <= ChunkSize;
					const bool bDeeper = !bTouching && InThis.GetMin() >= -3 && InThis.GetMax() <= ChunkSize + 2;
					if ((!bTouching && !bDeeper) || (X + 2 * Y + 3 * Z) % 5 != 0)
					{
						continue;
					}
					FVoxelEdit Edit;
					switch (N++ % 4)
					{
					case 0: Edit = FVoxelEdit(FIntVector(X, Y, Z), FVoxelData::Air(), FVoxelData::Air(), EEditMode::Set); break;
					case 1: Edit = FVoxelEdit(FIntVector(X, Y, Z), FVoxelData::Solid(EVoxelMaterial::Stone), FVoxelData::Air(), EEditMode::Set); break;
					case 2: Edit = FVoxelEdit(FIntVector(X, Y, Z), EEditMode::Subtract, 90, 0); break;
					default: Edit = FVoxelEdit(FIntVector(X, Y, Z), EEditMode::Add, 90, EVoxelMaterial::Stone); break;
					}
					Layer.ApplyEdit(Edit);
					TouchingEditCount += bTouching ? 1 : 0;
				}
			}
			// A neighbour no line top reads (opposite the upper face), edited on its layer touching the chunk.
			const FIntVector Unread = Offsets[0] * -1;
			FChunkEditLayer& UnreadLayer = Edits.Add(Coord + Unread, FChunkEditLayer(Coord + Unread, ChunkSize));
			for (int32 I = 0; I < ChunkSize; ++I)
			{
				FIntVector Local(I);
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					if (Unread[Axis] != 0)
					{
						Local[Axis] = Unread[Axis] < 0 ? ChunkSize - 1 : 0;
					}
				}
				UnreadLayer.ApplyEdit(FVoxelEdit(Local, FVoxelData::Air(), FVoxelData::Air(), EEditMode::Set));
			}

			FScatterExtractionContext Context;
			Context.WorldMode = &*Mode;
			Context.NoiseParams = Config->NoiseParams;
			Context.TouchingEdits = UVoxelScatterManager::GatherPlanetTouchingEdits(Up, Coord, ChunkSize, [&Edits](const FIntVector& C) { return Edits.Find(C); });
			TestEqual(*FString::Printf(TEXT("%s: the context holds exactly the edits on the touching layers the line tops read"), Site.Key),
				Context.TouchingEdits.IsValid() ? Context.TouchingEdits->Num() : 0, TouchingEditCount);

			int32 Compared = 0, SolidMismatch = 0, DensityOff = 0, EditedCompared = 0;
			for (const FIntVector& Offset : Offsets)
			{
				const FIntVector Neighbour = Coord + Offset;
				const TArray<FVoxelData> Voxels = GenerateChunk(Generator, Config, Neighbour, NoZones, Edits.Find(Neighbour));
				for (int32 Z = -1; Z <= ChunkSize; ++Z)
				for (int32 Y = -1; Y <= ChunkSize; ++Y)
				for (int32 X = -1; X <= ChunkSize; ++X)
				{
					const FIntVector Local(X, Y, Z);
					const FIntVector In = Local - Offset * ChunkSize;
					if (In.GetMin() < 0 || In.GetMax() >= ChunkSize)
					{
						continue;
					}
					const FVoxelData& Expected = Voxels[In.X + In.Y * ChunkSize + In.Z * ChunkSize * ChunkSize];
					const FVoxelData Actual = Context.GetPlanetVoxelOutsideChunk(Local, ChunkOrigin(Config, Coord) + FVector(Local) * VoxelSize, VoxelSize);
					++Compared;
					EditedCompared += Context.TouchingEdits.IsValid() && Context.TouchingEdits->Contains(Local) ? 1 : 0;
					SolidMismatch += Expected.IsSolid() != Actual.IsSolid() ? 1 : 0;
					DensityOff += FMath::Abs(static_cast<int32>(Expected.Density) - static_cast<int32>(Actual.Density)) > 1 ? 1 : 0;
				}
			}
			AddInfo(FString::Printf(TEXT("%s: %d neighbour(s), %d touching voxels compared (%d edited): %d disagree on solid, %d differ by more than 1 in density"),
				Site.Key, Offsets.Num(), Compared, EditedCompared, SolidMismatch, DensityOff));
			TestTrue(*FString::Printf(TEXT("%s: voxels compared, edits among them (non-vacuous)"), Site.Key), Compared >= ChunkSize * ChunkSize && EditedCompared > 0);
			TestEqual(*FString::Printf(TEXT("%s: the voxel outside the chunk is the neighbour's, solid or air"), Site.Key), SolidMismatch, 0);
			TestEqual(*FString::Printf(TEXT("%s: and its density"), Site.Key), DensityOff, 0);
		}
		Generator.Shutdown();
	}

	// The pole pad: a patch of columns in chunk (0, 0), the edits on the upper chunk's bottom layer.
	TSet<FIntPoint> Patch, Outside;
	for (int32 Y = -ChunkSize; Y < ChunkSize; ++Y)
	{
		for (int32 X = -ChunkSize; X < ChunkSize; ++X)
		{
			const bool bIn = X >= 4 && X <= 11 && Y >= 4 && Y <= 11;
			(bIn ? Patch : Outside).Add(FIntPoint(X, Y));
		}
	}
	auto MergedVoxels = [&](const FVoxelConditioningZone& Pad, const TMap<FIntVector, FChunkEditLayer>& Edits)
	{
		FVoxelCPUNoiseGenerator Generator;
		Generator.Initialize();
		TMap<FIntVector, TArray<FVoxelData>> Voxels;
		for (const FIntVector& Coord : PadChunks)
		{
			Voxels.Add(Coord, GenerateChunk(Generator, Config, Coord, { Pad }, Edits.Find(Coord)));
		}
		Generator.Shutdown();
		return Voxels;
	};

	// Dig: the pad half a voxel over the face (the band); the upper chunk's bottom layer dug away over the patch, exposing
	// the lower chunk's top voxel there.
	{
		const FVoxelConditioningZone Pad = MakePad(0.5);
		const TMap<FIntVector, FChunkEditLayer> Dig = MakePatchEdits(Patch, LowerLayer + 1, 0, FVoxelData::Air());
		const TMap<FIntVector, TArray<FVoxelData>> Voxels = MergedVoxels(Pad, Dig);
		auto Merged = [&](const FIntPoint& Column) { return MergedSurfaceAltitude(Config, *Mode, Voxels, Column); };
		const TArray<FChunkPoints> With = ExtractPad(Config, *Mode, Pad, Dig, EVoxelAbove::Decided);
		const TArray<FChunkPoints> Blind = ExtractPad(Config, *Mode, Pad, Dig, EVoxelAbove::Decided, false);
		const FPadResult In = JudgePad(Config, *Mode, With, Merged, 0.5, &Patch);
		const FPadResult Out = JudgePad(Config, *Mode, With, Merged, 0.5, &Outside);
		const FPadResult InBlind = JudgePad(Config, *Mode, Blind, Merged, 0.5, &Patch);
		AddInfo(FString::Printf(TEXT("Dig: dug columns %d of %d with one open point (worst %.2f uu), the rest %d of %d; without the edits %d of %d"),
			In.ColumnsWithOne, In.Columns, In.WorstError, Out.ColumnsWithOne, Out.Columns, InBlind.ColumnsWithOne, InBlind.Columns));
		TestEqual(TEXT("Dig: the patch has columns (non-vacuous)"), In.Columns, Patch.Num());
		TestEqual(TEXT("Dig: the exposed face is open surface"), In.ColumnsWithOne, In.Columns);
		TestTrue(TEXT("Dig: on the dug surface"), In.WorstError <= 2.0);
		TestEqual(TEXT("Dig: elsewhere one point per column"), Out.ColumnsWithOne, Out.Columns);
		TestEqual(TEXT("Dig: without the edits the dug columns have no surface (edit-blind)"), InBlind.ColumnsWithOne, 0);
	}

	// Build: the pad half a voxel under the face (the lower chunk's top cell holds the surface); built over the patch into
	// the upper chunk's bottom layer.
	{
		const FVoxelConditioningZone Pad = MakePad(-0.5);
		const TMap<FIntVector, FChunkEditLayer> Build = MakePatchEdits(Patch, LowerLayer + 1, 0, FVoxelData::Solid(EVoxelMaterial::Stone));
		const TMap<FIntVector, TArray<FVoxelData>> Voxels = MergedVoxels(Pad, Build);
		auto Merged = [&](const FIntPoint& Column) { return MergedSurfaceAltitude(Config, *Mode, Voxels, Column); };
		const TArray<FChunkPoints> With = ExtractPad(Config, *Mode, Pad, Build, EVoxelAbove::Decided);
		const TArray<FChunkPoints> Blind = ExtractPad(Config, *Mode, Pad, Build, EVoxelAbove::Decided, false);
		const FPadResult In = JudgePad(Config, *Mode, With, Merged, -0.5, &Patch);
		const FPadResult Out = JudgePad(Config, *Mode, With, Merged, -0.5, &Outside);
		const FPadResult InBlind = JudgePad(Config, *Mode, Blind, Merged, -0.5, &Patch);
		AddInfo(FString::Printf(TEXT("Build: built columns %d of %d with one open point (worst %.2f uu, most %d), the rest %d of %d; without the edits most %d"),
			In.ColumnsWithOne, In.Columns, In.WorstError, In.MaxOpenInColumn, Out.ColumnsWithOne, Out.Columns, InBlind.MaxOpenInColumn));
		TestEqual(TEXT("Build: the built-over columns have one open point (the build's top)"), In.ColumnsWithOne, In.Columns);
		TestTrue(TEXT("Build: on the build's top"), In.WorstError <= 2.0);
		TestEqual(TEXT("Build: elsewhere one point per column"), Out.ColumnsWithOne, Out.Columns);
		TestEqual(TEXT("Build: without the edits the covered face stayed open surface under the build"), InBlind.ColumnsWithTwo, InBlind.Columns);
	}

	// Wiring: the scatter manager's own pipeline asks its edit layer source for the neighbours (the dig case).
	{
		const FVoxelConditioningZone Pad = MakePad(0.5);
		const TMap<FIntVector, FChunkEditLayer> Dig = MakePatchEdits(Patch, LowerLayer + 1, 0, FVoxelData::Air());
		const FIntVector Lower(0, 0, LowerLayer);
		FVoxelCPUNoiseGenerator Generator;
		Generator.Initialize();
		const TArray<FVoxelData> LowerVoxels = GenerateChunk(Generator, Config, Lower, { Pad }, nullptr);
		Generator.Shutdown();
		for (const bool bSource : { true, false })
		{
			FPlanetHarness Harness(Config);
			Harness.Manager->SetWorldMode(&*Mode);
			Harness.Manager->SetConditioningGatherer([&Pad](const FBox&, TArray<FVoxelConditioningZone>& OutZones) { OutZones.Add(Pad); });
			if (bSource)
			{
				Harness.Manager->SetEditLayerSource([&Dig](const FIntVector& ChunkCoord) { return Dig.Find(ChunkCoord); });
			}
			const FChunkSurfaceData* Surface = Harness.Run(Lower, LowerVoxels);
			int32 PatchOpen = 0, OutsideOpen = 0;
			if (Surface)
			{
				for (const FVoxelSurfacePoint& Point : Surface->SurfacePoints)
				{
					const FIntPoint Column = ColumnOf(Config, Point.Position);
					if (!Point.bIsUnderground && IsJudged(Column))
					{
						(Patch.Contains(Column) ? PatchOpen : OutsideOpen) += 1;
					}
				}
			}
			AddInfo(FString::Printf(TEXT("%s: %d open points over the dig, %d elsewhere in the lower chunk"), bSource ? TEXT("With the edit layer source") : TEXT("Without"),
				PatchOpen, OutsideOpen));
			TestEqual(TEXT("The lower chunk has no open point outside the dig (its line tops are covered by the pad)"), OutsideOpen, 0);
			TestEqual(bSource ? TEXT("With the edit layer source the dug face is open surface") : TEXT("Without it the dug face has no point"),
				PatchOpen, bSource ? Patch.Num() : 0);
		}
	}
	Config->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
