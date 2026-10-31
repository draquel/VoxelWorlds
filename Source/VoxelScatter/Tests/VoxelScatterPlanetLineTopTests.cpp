// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Testing/VoxelScatterTestDefinitions.h"
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
#include "VoxelCubeSphere.h"
#include "VoxelCaveConfiguration.h"
#include "VoxelCaveTypes.h"
#include "VoxelBiomeConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// World modes review G29 and G31 (WORLD_MODES_REVIEW_PLAN.md §14.1, §19.6, §19.9): where a planet's surface crosses a chunk
// face. UVoxelScatterManager::ExtractSurfacePointsAlongUp samples the planet's radial lines through the cube-sphere grid's
// cells (one family for every chunk) and each chunk keeps the crossings in its own box; its last cell on each axis reads
// the voxels past its +X, +Y and +Z faces as the planet generator makes them, with the neighbours' edits. Before G31 each
// chunk stepped digital lines along local up at its own centre, which did not continue into a neighbour's: strips along
// faces got too few points (G31) and, before G29, a line top doubled the neighbour's point. Real planet chunks from the CPU
// generator (a 250,000 uu planet like Review_Planet, voxel 75, chunk 32, as VoxelScatterPlanetTests).
//
// - Duplicates: the 27 chunks around the poles, 45 degrees, the equator, a cube corner and the sites where the old lines'
//   drift rounded worst (slopes of a quarter and half a voxel a step, 30N 40E and the live corner and south stops): no two
//   open points on one up-line, and the local density has no doubled strip and no strip without points. With the shell
//   past the faces taken for air the band where the surface crosses a face gets second points (the test is sensitive).
// - FlatPad: a flat pad at the north pole at heights around a chunk face (inside the lower chunk's top cell, at the face,
//   in the band, past it): every ray through the pad has exactly one open point, on the pad, and the lower chunk's top
//   layer has none where the pad lies over the face. With the shell taken for air the band's rays have two.
// - Edits: the neighbours the shell reads (the seven on the +X, +Y and +Z sides); the voxel outside the chunk equals the
//   neighbour's generated and edit-merged voxel on every touching layer; a dig through the upper chunk's bottom layer
//   exposes the face (open surface) and a build over a face that was the surface covers it; the scatter manager's own
//   pipeline reads the neighbour's edit layer (SetEditLayerSource).
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

	/** Every voxel of the shell past a chunk's +X, +Y and +Z faces set to air: the ground beyond the faces taken for air. */
	TSharedPtr<const TMap<FIntVector, FVoxelEdit>> AllAirTouching()
	{
		TArray<FIntVector> Offsets;
		UVoxelScatterManager::GetPlanetShellNeighbours(Offsets);
		TMap<FIntVector, FVoxelEdit> Map;
		for (int32 Z = -1; Z <= ChunkSize; ++Z)
		for (int32 Y = -1; Y <= ChunkSize; ++Y)
		for (int32 X = -1; X <= ChunkSize; ++X)
		{
			const FIntVector Offset(X < 0 ? -1 : (X >= ChunkSize ? 1 : 0), Y < 0 ? -1 : (Y >= ChunkSize ? 1 : 0), Z < 0 ? -1 : (Z >= ChunkSize ? 1 : 0));
			if (Offsets.Contains(Offset))
			{
				const FIntVector Local(X, Y, Z);
				Map.Add(Local, FVoxelEdit(Local - Offset * ChunkSize, FVoxelData::Air(), EEditMode::Set));
			}
		}
		return MakeShared<const TMap<FIntVector, FVoxelEdit>>(MoveTemp(Map));
	}

	enum class EVoxelAbove : uint8
	{
		/** The shell is the generator's voxel past the faces with the neighbours' edits (the scatter manager's context). */
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
	 * the scatter manager's: ContextZones and the neighbours' edits on the voxels past the faces (GatherPlanetTouchingEdits over Edits), or an
	 * all-air shell.
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
			Context.OutsideEdits = Above == EVoxelAbove::AllAir
				? AllAirTouching()
				: UVoxelScatterManager::GatherPlanetTouchingEdits(Coord, ChunkSize, [&Edits](const FIntVector& C) { return Edits.Find(C); });
			const TArray<FVoxelData> Voxels = GenerateChunk(Generator, Config, Coord, GenerationZones, Edits.Find(Coord));
			FChunkSurfaceData Surface;
			UVoxelScatterManager::ExtractChunkSurfacePoints(false, Voxels, Coord, ChunkOrigin(Config, Coord), ChunkSize, VoxelSize, VoxelSize, {}, Context, Surface);
			Out.Add({ Coord, MoveTemp(Surface.SurfacePoints) });
		}
		Generator.Shutdown();
		return Out;
	}

	/**
	 * Pairs of open points on one up-line: closer than half a voxel across local up and within two voxels along it (a
	 * second point where one belongs; neighbouring rays lie about a voxel apart across). Also the probe's measure (§19.4):
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
	 * rays' spacing is flat over any regular sampling, so a doubled strip shows as samples over 1.2 and a strip
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

	FVector LatLon(double Lat, double Lon)
	{
		const double A = FMath::DegreesToRadians(Lat), B = FMath::DegreesToRadians(Lon);
		return FVector(FMath::Cos(A) * FMath::Cos(B), FMath::Cos(A) * FMath::Sin(B), FMath::Sin(A));
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

	/**
	 * Where the per-chunk digital lines (before G31) rounded their drift worst: slopes of a half and a quarter voxel a step
	 * across the stepping axis, a site between them, and the stops of the §19.6 A/B on Review_Planet near a cube corner
	 * and in the south.
	 */
	TArray<TPair<const TCHAR*, FVector>> SlopeSites()
	{
		return {
			{ TEXT("half slope 26.57N 0E"), LatLon(26.565, 0.0) },
			{ TEXT("quarter slope 14.04N 0E"), LatLon(14.036, 0.0) },
			{ TEXT("30N 40E"), LatLon(30.0, 40.0) },
			{ TEXT("live corner 36.80N 43.06E"), LatLon(36.804, 43.057) },
			{ TEXT("live south 59.93S 115.61W"), LatLon(-59.927, -115.606) },
		};
	}

	/** The rays' grid: the extraction's at one point per voxel. */
	const FVoxelCubeSphereGrid& RayGrid()
	{
		static const FVoxelCubeSphereGrid Grid(FVoxelCubeSphereGrid::CellsPerEdgeFor(Radius, VoxelSize));
		return Grid;
	}

	/** The ray (grid cell) a point lies on. */
	FIntPoint RayOf(const UVoxelWorldConfiguration* Config, const FVector& Position)
	{
		return RayGrid().CellOfDirection((Position - Config->WorldOrigin).GetSafeNormal());
	}

	/** Where a ray reaches an altitude over the datum. */
	FVector RayAt(const UVoxelWorldConfiguration* Config, const FIntPoint& Ray, double Altitude)
	{
		return Config->WorldOrigin + RayGrid().CellCentreDirection(Ray) * (Radius + Altitude);
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

	TArray<FChunkPoints> ExtractPad(const UVoxelWorldConfiguration* Config, const IVoxelWorldMode& Mode, const FVoxelConditioningZone& Pad,
		const TMap<FIntVector, FChunkEditLayer>& Edits, EVoxelAbove Above, bool bEditsInContext = true)
	{
		const TArray<FVoxelConditioningZone> Zones = { Pad };
		if (bEditsInContext)
		{
			return ExtractChunks(Config, Mode, TArray<FIntVector>(PadChunks, UE_ARRAY_COUNT(PadChunks)), Zones, Zones, Edits, Above);
		}
		// The voxels carry the edits but the context does not (edit-blind voxels past the faces).
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
			// finds nothing for it (it reads only its seven +X, +Y and +Z neighbours).
			Out.Append(MoveTemp(One));
		}
		return Out;
	}

	/** The rays through the pad within PadJudged of the pole (where they reach the face's altitude). */
	TArray<FIntPoint> PadRays()
	{
		TArray<FIntPoint> Rays;
		RayGrid().GatherWithinAngle(FVector(0, 0, 1), PadJudged / (Radius + FaceAltitude), 100000, Rays);
		return Rays;
	}

	/** The column a ray stands in where it reaches the face's altitude. */
	FIntPoint ColumnOfRay(const UVoxelWorldConfiguration* Config, const FIntPoint& Ray)
	{
		return ColumnOf(Config, RayAt(Config, Ray, FaceAltitude));
	}

	/** Over the pad's judged rays: open points per ray, the worst distance from the expected surface, the lower top layer. */
	struct FPadResult
	{
		int32 Rays = 0;
		int32 RaysWithOne = 0;
		int32 RaysWithTwo = 0;
		int32 RaysWithTwoOrMore = 0;
		int32 MaxOpenOnRay = 0;
		double WorstError = 0.0;
		/** The lower chunks' points (any class) in their top voxel cell on rays where the pad lies over the face within the band. */
		int32 BandTopLayer = 0;
	};

	/**
	 * ExpectedAltitude(Ray): the surface each ray should cross, as an altitude (the pad's, or the edit-merged surface).
	 * Judged: the rays judged (PadRays, or a part of them).
	 */
	FPadResult JudgePad(const UVoxelWorldConfiguration* Config, const IVoxelWorldMode& Mode, const TArray<FChunkPoints>& Chunks,
		TFunctionRef<double(const FIntPoint&)> ExpectedAltitude, double PadOffsetVoxels, const TArray<FIntPoint>& Judged)
	{
		const TSet<FIntPoint> JudgedSet(Judged);
		TMap<FIntPoint, TArray<double>> OpenAltitudes;
		FPadResult Result;
		// The pad over the face within the band on a ray: where the ray meets the face's plane, the face rises away from the
		// pole, the pad does not.
		auto PadInBand = [&](const FIntPoint& Ray)
		{
			const FVector Direction = RayGrid().CellCentreDirection(Ray);
			const double FaceZ = Config->WorldOrigin.Z + (LowerLayer + 1) * ChunkWorld;
			const double T = (FaceZ - Config->WorldOrigin.Z) / Direction.Z;
			const double Over = FaceAltitude + PadOffsetVoxels * VoxelSize - (T - Radius);
			return Over >= 0.0 && Over <= Band;
		};
		for (const FChunkPoints& Chunk : Chunks)
		{
			const double LowerTop = ChunkOrigin(Config, Chunk.Coord).Z + ChunkWorld;
			for (const FVoxelSurfacePoint& Point : Chunk.Points)
			{
				const FIntPoint Ray = RayOf(Config, Point.Position);
				if (!JudgedSet.Contains(Ray))
				{
					continue;
				}
				if (!Point.bIsUnderground)
				{
					OpenAltitudes.FindOrAdd(Ray).Add(Mode.GetAltitude(Point.Position));
				}
				if (Chunk.Coord.Z == LowerLayer && Point.Position.Z > LowerTop - VoxelSize + 0.01 && PadInBand(Ray))
				{
					++Result.BandTopLayer;
				}
			}
		}
		for (const FIntPoint& Ray : Judged)
		{
			++Result.Rays;
			const TArray<double>* Altitudes = OpenAltitudes.Find(Ray);
			const int32 Count = Altitudes ? Altitudes->Num() : 0;
			Result.RaysWithOne += Count == 1 ? 1 : 0;
			Result.RaysWithTwo += Count == 2 ? 1 : 0;
			Result.RaysWithTwoOrMore += Count >= 2 ? 1 : 0;
			Result.MaxOpenOnRay = FMath::Max(Result.MaxOpenOnRay, Count);
			if (Altitudes)
			{
				const double Expected = ExpectedAltitude(Ray);
				for (const double Altitude : *Altitudes)
				{
					Result.WorstError = FMath::Max(Result.WorstError, FMath::Abs(Altitude - Expected));
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
			Layer.ApplyEdit(FVoxelEdit(Local, NewData, EEditMode::Set));
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
			VoxelScatterTesting::AddSmoothDefinitions(*Manager);
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
	TArray<TPair<const TCHAR*, FVector>> AllSites = Sites();
	AllSites.Append(SlopeSites());
	for (const TPair<const TCHAR*, FVector>& Site : AllSites)
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
		AddInfo(FString::Printf(TEXT("%s: %d open points; pairs on one up-line from two chunks %d, within one chunk %d; within half a voxel from two chunks %d. With the shell taken for air: %d open points, %d pairs from two chunks (%d within one)"),
			Site.Key, Now.Open, Now.CrossChunk, Now.SameChunk, Now.NearCrossChunk, Old.Open, Old.CrossChunk, Old.SameChunk));
		AddInfo(FString::Printf(TEXT("%s: local density over %d samples: mean %.3f, %d under 0.8, %d over 1.2; with the shell taken for air mean %.3f, %d under, %d over"),
			Site.Key, NowDensity.Samples, NowDensity.Mean, NowDensity.Under, NowDensity.Over, OldDensity.Mean, OldDensity.Under, OldDensity.Over));
		TestEqual(*FString::Printf(TEXT("%s: no strip of doubled points (local density over 1.2)"), Site.Key), NowDensity.Over, 0);
		TestEqual(*FString::Printf(TEXT("%s: no strip without points (local density under 0.8)"), Site.Key), NowDensity.Under, 0);
		TestTrue(*FString::Printf(TEXT("%s: the surface has points (non-vacuous)"), Site.Key), Now.Open >= 2000);
		// One family of rays for every chunk, each crossing kept by the chunk it lies in: no ray has two open points.
		TestEqual(*FString::Printf(TEXT("%s: no two chunks put open points on one up-line"), Site.Key), Now.CrossChunk, 0);
		TestEqual(*FString::Printf(TEXT("%s: one chunk never puts two open points on one up-line"), Site.Key), Now.SameChunk, 0);
	}
	AddInfo(FString::Printf(TEXT("Over the sites, taking the shell for air doubles %d crossings (%d samples over 1.2)"), OldPairs, OldOver));
	TestTrue(TEXT("Taking the shell past the faces for air doubles the crossings where the surface crosses a chunk face (the test is sensitive)"),
		OldPairs >= 500 && OldOver > 0);
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
	const TArray<FIntPoint> Rays = PadRays();
	for (const double Offset : { -0.9, -0.5, -0.1, -0.01, 0.0, 0.01, 0.3, 0.7, 1.0, 1.4, 1.6, 3.0 })
	{
		const FVoxelConditioningZone Pad = MakePad(Offset);
		const double PadAltitude = Pad.TargetHeight;
		auto OnPad = [PadAltitude](const FIntPoint&) { return PadAltitude; };
		const FPadResult Now = JudgePad(Config, *Mode, ExtractPad(Config, *Mode, Pad, NoEdits, EVoxelAbove::Decided), OnPad, Offset, Rays);
		const FPadResult Old = JudgePad(Config, *Mode, ExtractPad(Config, *Mode, Pad, NoEdits, EVoxelAbove::AllAir), OnPad, Offset, Rays);
		AddInfo(FString::Printf(TEXT("Pad %+.2f voxels over the face: %d of %d rays with one open point (most %d), worst %.2f uu off the pad, %d points on the lower top layer under the band's pad; the shell taken for air: %d rays with two or more"),
			Offset, Now.RaysWithOne, Now.Rays, Now.MaxOpenOnRay, Now.WorstError, Now.BandTopLayer, Old.RaysWithTwoOrMore));
		TestTrue(*FString::Printf(TEXT("Pad %+.2f: rays judged (non-vacuous)"), Offset), Now.Rays >= 2000);
		TestEqual(*FString::Printf(TEXT("Pad %+.2f: one open point per ray"), Offset), Now.RaysWithOne, Now.Rays);
		TestTrue(*FString::Printf(TEXT("Pad %+.2f: on the pad (worst %.2f uu)"), Offset, Now.WorstError), Now.WorstError <= 2.0);
		TestEqual(*FString::Printf(TEXT("Pad %+.2f: no point on the lower chunks' top layer under the pad"), Offset), Now.BandTopLayer, 0);
		if (Offset > 0.05 && Offset <= 1.0)
		{
			// The shell taken for air puts a second crossing in the lower chunk's top cell under the face (not quite on every ray:
			// next to the side faces between the four chunk columns at the pole the air past them bends the lower crossing away).
			TestTrue(*FString::Printf(TEXT("Pad %+.2f: taking the shell for air gave nine rays in ten two or more open points (the test is sensitive)"), Offset),
				Old.RaysWithTwoOrMore * 10 >= Old.Rays * 9);
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

	// The neighbours the shell reads: the seven on the +X, +Y and +Z sides, wherever local up points.
	{
		TArray<FIntVector> Offsets;
		UVoxelScatterManager::GetPlanetShellNeighbours(Offsets);
		const bool bSeven = Offsets.Num() == 7 && Algo::AllOf(Offsets, [](const FIntVector& O)
		{
			return O.GetMin() >= 0 && O.GetMax() == 1;
		});
		TestTrue(TEXT("The shell reads the seven neighbours on the +X, +Y and +Z sides"), bSeven);
	}

	// The voxel outside a chunk is the neighbour's voxel: generated (natural terrain), and with edits of every mode on the
	// touching layers (and edits deeper in the neighbours, and in a neighbour the shell does not reach, which must not count).
	{
		FVoxelCPUNoiseGenerator Generator;
		Generator.Initialize();
		const TArray<FVoxelConditioningZone> NoZones;
		for (const TPair<const TCHAR*, FVector>& Site : Sites())
		{
			const FVoxelTerrainSurfacePoint SiteSurface = Mode->GetSurfacePoint(Config->WorldOrigin + Site.Value, Config->NoiseParams);
			const FIntVector Coord = ChunkOf(Config, SiteSurface.Position);
			TArray<FIntVector> Offsets;
			UVoxelScatterManager::GetPlanetShellNeighbours(Offsets);
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
					case 0: Edit = FVoxelEdit(FIntVector(X, Y, Z), FVoxelData::Air(), EEditMode::Set); break;
					case 1: Edit = FVoxelEdit(FIntVector(X, Y, Z), FVoxelData::Solid(EVoxelMaterial::Stone), EEditMode::Set); break;
					case 2: Edit = FVoxelEdit(FIntVector(X, Y, Z), EEditMode::Subtract, 90, 0); break;
					default: Edit = FVoxelEdit(FIntVector(X, Y, Z), EEditMode::Add, 90, EVoxelMaterial::Stone); break;
					}
					Layer.ApplyEdit(Edit);
					TouchingEditCount += bTouching ? 1 : 0;
				}
			}
			// A neighbour the shell does not reach (on the -X side), edited on its layer touching the chunk.
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
				UnreadLayer.ApplyEdit(FVoxelEdit(Local, FVoxelData::Air(), EEditMode::Set));
			}

			FScatterExtractionContext Context;
			Context.WorldMode = &*Mode;
			Context.NoiseParams = Config->NoiseParams;
			Context.OutsideEdits = UVoxelScatterManager::GatherPlanetTouchingEdits(Coord, ChunkSize, [&Edits](const FIntVector& C) { return Edits.Find(C); });
			TestEqual(*FString::Printf(TEXT("%s: the context holds exactly the edits on the touching layers the shell reads"), Site.Key),
				Context.OutsideEdits.IsValid() ? Context.OutsideEdits->Num() : 0, TouchingEditCount);

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
					const FVoxelData Actual = Context.GetVoxelOutsideChunk(Local, ChunkOrigin(Config, Coord) + FVector(Local) * VoxelSize, VoxelSize);
					++Compared;
					EditedCompared += Context.OutsideEdits.IsValid() && Context.OutsideEdits->Contains(Local) ? 1 : 0;
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

	// The pole pad: a patch of columns in chunk (0, 0), the edits on the upper chunk's bottom layer. Rays are judged a voxel
	// clear of its edge, where the trilinear surface is flat: inside the patch shrunk by one column, outside it grown by one.
	TSet<FIntPoint> Patch;
	for (int32 Y = 4; Y <= 11; ++Y)
	{
		for (int32 X = 4; X <= 11; ++X)
		{
			Patch.Add(FIntPoint(X, Y));
		}
	}
	TArray<FIntPoint> InRays, OutRays;
	for (const FIntPoint& Ray : PadRays())
	{
		const FIntPoint Column = ColumnOfRay(Config, Ray);
		if (Column.X >= 5 && Column.X <= 10 && Column.Y >= 5 && Column.Y <= 10)
		{
			InRays.Add(Ray);
		}
		else if (Column.X < 3 || Column.X > 12 || Column.Y < 3 || Column.Y > 12)
		{
			OutRays.Add(Ray);
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
		auto Merged = [&](const FIntPoint& Ray) { return MergedSurfaceAltitude(Config, *Mode, Voxels, ColumnOfRay(Config, Ray)); };
		const TArray<FChunkPoints> With = ExtractPad(Config, *Mode, Pad, Dig, EVoxelAbove::Decided);
		const TArray<FChunkPoints> Blind = ExtractPad(Config, *Mode, Pad, Dig, EVoxelAbove::Decided, false);
		const FPadResult In = JudgePad(Config, *Mode, With, Merged, 0.5, InRays);
		const FPadResult Out = JudgePad(Config, *Mode, With, Merged, 0.5, OutRays);
		const FPadResult InBlind = JudgePad(Config, *Mode, Blind, Merged, 0.5, InRays);
		AddInfo(FString::Printf(TEXT("Dig: dug rays %d of %d with one open point (worst %.2f uu), the rest %d of %d; without the edits %d of %d"),
			In.RaysWithOne, In.Rays, In.WorstError, Out.RaysWithOne, Out.Rays, InBlind.RaysWithOne, InBlind.Rays));
		TestTrue(TEXT("Dig: the patch has rays (non-vacuous)"), In.Rays >= 25);
		TestEqual(TEXT("Dig: the exposed face is open surface"), In.RaysWithOne, In.Rays);
		TestTrue(TEXT("Dig: on the dug surface"), In.WorstError <= 2.0);
		TestEqual(TEXT("Dig: elsewhere one point per ray"), Out.RaysWithOne, Out.Rays);
		TestEqual(TEXT("Dig: without the edits the dug rays have no surface (edit-blind)"), InBlind.RaysWithOne, 0);
	}

	// Build: the pad half a voxel under the face (the lower chunk's top cell holds the surface); built over the patch into
	// the upper chunk's bottom layer.
	{
		const FVoxelConditioningZone Pad = MakePad(-0.5);
		const TMap<FIntVector, FChunkEditLayer> Build = MakePatchEdits(Patch, LowerLayer + 1, 0, FVoxelData::Solid(EVoxelMaterial::Stone));
		const TMap<FIntVector, TArray<FVoxelData>> Voxels = MergedVoxels(Pad, Build);
		auto Merged = [&](const FIntPoint& Ray) { return MergedSurfaceAltitude(Config, *Mode, Voxels, ColumnOfRay(Config, Ray)); };
		const TArray<FChunkPoints> With = ExtractPad(Config, *Mode, Pad, Build, EVoxelAbove::Decided);
		const TArray<FChunkPoints> Blind = ExtractPad(Config, *Mode, Pad, Build, EVoxelAbove::Decided, false);
		const FPadResult In = JudgePad(Config, *Mode, With, Merged, -0.5, InRays);
		const FPadResult Out = JudgePad(Config, *Mode, With, Merged, -0.5, OutRays);
		const FPadResult InBlind = JudgePad(Config, *Mode, Blind, Merged, -0.5, InRays);
		AddInfo(FString::Printf(TEXT("Build: built rays %d of %d with one open point (worst %.2f uu, most %d), the rest %d of %d; without the edits most %d"),
			In.RaysWithOne, In.Rays, In.WorstError, In.MaxOpenOnRay, Out.RaysWithOne, Out.Rays, InBlind.MaxOpenOnRay));
		TestEqual(TEXT("Build: the built-over rays have one open point (the build's top)"), In.RaysWithOne, In.Rays);
		TestTrue(TEXT("Build: on the build's top"), In.WorstError <= 2.0);
		TestEqual(TEXT("Build: elsewhere one point per ray"), Out.RaysWithOne, Out.Rays);
		TestEqual(TEXT("Build: without the edits the covered face stayed open surface under the build"), InBlind.RaysWithTwo, InBlind.Rays);
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
			const TSet<FIntPoint> InSet(InRays), OutSet(OutRays);
			int32 PatchOpen = 0, OutsideOpen = 0;
			if (Surface)
			{
				for (const FVoxelSurfacePoint& Point : Surface->SurfacePoints)
				{
					const FIntPoint Ray = RayOf(Config, Point.Position);
					if (!Point.bIsUnderground)
					{
						PatchOpen += InSet.Contains(Ray) ? 1 : 0;
						OutsideOpen += OutSet.Contains(Ray) ? 1 : 0;
					}
				}
			}
			AddInfo(FString::Printf(TEXT("%s: %d open points on the %d rays over the dig, %d elsewhere in the lower chunk"),
				bSource ? TEXT("With the edit layer source") : TEXT("Without"), PatchOpen, InRays.Num(), OutsideOpen));
			TestEqual(TEXT("The lower chunk has no open point outside the dig (its rays enter it under the pad)"), OutsideOpen, 0);
			TestEqual(bSource ? TEXT("With the edit layer source the dug face is open surface") : TEXT("Without it the dug face has no point"),
				PatchOpen, bSource ? InRays.Num() : 0);
		}
	}
	Config->RemoveFromRoot();
	return true;
}

// ---------------------------------------------------------------------------
// World modes review G72 (WORLD_MODES_REVIEW_PLAN.md §14.1, §20.23): a planet chunk's rays read the voxels past its +X, +Y and
// +Z faces in its last cells, made by FScatterExtractionContext::GetVoxelOutsideChunk (GetPlanetVoxelOutsideChunk then). Those were the generated
// altitude's alone, so where a cave opens such a voxel under the ground it was read as rock: the cave floor in that cell
// got no point, and where the cave meets the face the rock made a floor in the air. They are now carved as generation
// carves them (the cave configuration's layers, its biome overrides by the biome generation gives the direction, the sea's
// minimum depth).
//
// - FaceCaves: the planet of the tests above with biomes, a sea at the datum and the default cave layers; chunks 15 and 35
//   voxels under the ground at four sites, each with its seven neighbours on the +X, +Y and +Z sides generated on the CPU.
//   Every chunk extracted with the face voxels made matches, point for point (position and classification), the same
//   extraction with the neighbours' own voxels there (OutsideVoxelSource). With the cave carve off (as before G72) points
//   are missed and added. Again with biome overrides (caves only in the commonest biome, from a shallower depth).
// ---------------------------------------------------------------------------

namespace VoxelScatterPlanetFaceCavesTestUtils
{
	using namespace VoxelScatterLineTopTestUtils;

	struct FCaveSetup
	{
		UVoxelWorldConfiguration* Config = nullptr;
		UVoxelBiomeConfiguration* Biomes = nullptr;
		UVoxelCaveConfiguration* Caves = nullptr;
		TSharedPtr<IVoxelWorldMode> Mode;

		FCaveSetup()
		{
			Config = MakePlanetConfig();
			Config->bEnableBiomes = true;
			Biomes = NewObject<UVoxelBiomeConfiguration>();
			Biomes->AddToRoot();
			Biomes->InitializeDefaults();
			Config->BiomeConfiguration = Biomes;
			Config->bEnableCaves = true;
			Caves = NewObject<UVoxelCaveConfiguration>(); // the default Cheese, Spaghetti and Noodle layers
			Caves->AddToRoot();
			Caves->bEnableCaves = true;
			Config->CaveConfiguration = Caves;
			Config->bEnableWaterLevel = true;
			Config->WaterRadius = Radius;
			Mode = FVoxelWorldModeFactory::Create(*Config);
		}

		~FCaveSetup()
		{
			Caves->RemoveFromRoot();
			Biomes->RemoveFromRoot();
			Config->RemoveFromRoot();
		}

		TArray<FVoxelData> Generate(FVoxelCPUNoiseGenerator& Generator, const FIntVector& Coord) const
		{
			FVoxelNoiseGenerationRequest Request;
			Request.ChunkCoord = Coord;
			Request.ChunkSize = ChunkSize;
			Request.VoxelSize = VoxelSize;
			Request.NoiseParams = Config->NoiseParams;
			FVoxelWorldModeFactory::FillGenerationRequest(*Config, Request);
			Request.bEnableBiomes = Config->bEnableBiomes;
			Request.BiomeConfiguration = Config->BiomeConfiguration;
			Request.bEnableCaves = Config->bEnableCaves;
			Request.CaveConfiguration = Config->CaveConfiguration;
			Request.bEnableWaterLevel = Config->bEnableWaterLevel;
			Request.WaterLevel = Config->WaterLevel;
			Request.WaterRadius = Config->WaterRadius;
			TArray<FVoxelData> Voxels;
			Generator.GenerateChunkCPU(Request, Voxels);
			return Voxels;
		}

		FScatterExtractionContext MakeContext() const
		{
			FScatterExtractionContext Context;
			Context.WorldMode = Mode.Get();
			Context.NoiseParams = Config->NoiseParams;
			Context.Sea = FVoxelSeaSurface::FromConfig(*Config);
			Context.CaveConfiguration = Caves;
			Context.bEnableBiomes = Config->bEnableBiomes;
			return Context;
		}
	};

	struct FFaceResult
	{
		int32 Chunks = 0;
		int64 Reference = 0;
		int64 ReferenceUnderground = 0;
		int64 Missed = 0;
		int64 Spurious = 0;
		int64 Reclassified = 0;
		void Add(const FFaceResult& O)
		{
			Chunks += O.Chunks; Reference += O.Reference; ReferenceUnderground += O.ReferenceUnderground; Missed += O.Missed; Spurious += O.Spurious;
			Reclassified += O.Reclassified;
		}
	};

	/** A chunk and its seven +X/+Y/+Z neighbours, generated. */
	struct FBlock
	{
		FIntVector Coord;
		TArray<FVoxelData> Voxels;
		TArray<TArray<FVoxelData>> Neighbours;
	};

	TArray<FBlock> MakeBlocks(const FCaveSetup& Setup)
	{
		TArray<FIntVector> Shell;
		UVoxelScatterManager::GetPlanetShellNeighbours(Shell);
		FVoxelCPUNoiseGenerator Generator;
		Generator.Initialize();
		TArray<FBlock> Blocks;
		const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(*Setup.Mode);
		for (const FVector& Direction : { LatLon(45.0, 20.0), LatLon(0.0, 100.0), LatLon(-30.0, -60.0), FVector(1, 1, 1).GetSafeNormal() })
		{
			const float Ground = Planet.GetTerrainAltitude(Direction, Setup.Config->NoiseParams);
			for (const double Depth : { 15.0, 35.0 })
			{
				FBlock& Block = Blocks.AddDefaulted_GetRef();
				Block.Coord = ChunkOf(Setup.Config, Setup.Config->WorldOrigin + Direction * (Radius + Ground - Depth * VoxelSize));
				Block.Voxels = Setup.Generate(Generator, Block.Coord);
				for (const FIntVector& Offset : Shell)
				{
					Block.Neighbours.Add(Setup.Generate(Generator, Block.Coord + Offset));
				}
			}
		}
		Generator.Shutdown();
		return Blocks;
	}

	FFaceResult Compare(const FCaveSetup& Setup, const FBlock& Block)
	{
		TArray<FIntVector> Shell;
		UVoxelScatterManager::GetPlanetShellNeighbours(Shell);
		const FVector Origin = ChunkOrigin(Setup.Config, Block.Coord);
		FChunkSurfaceData Made, Reference;
		UVoxelScatterManager::ExtractChunkSurfacePoints(false, Block.Voxels, Block.Coord, Origin, ChunkSize, VoxelSize, VoxelSize, {}, Setup.MakeContext(), Made);
		FScatterExtractionContext RefContext = Setup.MakeContext();
		RefContext.OutsideVoxelSource = [&Block, &Shell](const FIntVector& Local, FVoxelData& Out)
		{
			const FIntVector Offset(Local.X >= ChunkSize ? 1 : 0, Local.Y >= ChunkSize ? 1 : 0, Local.Z >= ChunkSize ? 1 : 0);
			const int32 Index = Shell.IndexOfByKey(Offset);
			if (Index == INDEX_NONE)
			{
				return false;
			}
			const FIntVector N = Local - Offset * ChunkSize;
			Out = Block.Neighbours[Index][N.X + N.Y * ChunkSize + N.Z * ChunkSize * ChunkSize];
			return true;
		};
		UVoxelScatterManager::ExtractChunkSurfacePoints(false, Block.Voxels, Block.Coord, Origin, ChunkSize, VoxelSize, VoxelSize, {}, RefContext, Reference);

		auto Key = [](const FVoxelSurfacePoint& P)
		{
			return FIntVector(static_cast<int32>(FMath::RoundToInt64(P.Position.X * 10.0)), static_cast<int32>(FMath::RoundToInt64(P.Position.Y * 10.0)),
				static_cast<int32>(FMath::RoundToInt64(P.Position.Z * 10.0)));
		};
		TMap<FIntVector, bool> MadeKeys;
		for (const FVoxelSurfacePoint& P : Made.SurfacePoints)
		{
			MadeKeys.Add(Key(P), P.bIsUnderground);
		}
		FFaceResult Result;
		Result.Chunks = 1;
		TSet<FIntVector> RefKeys;
		for (const FVoxelSurfacePoint& P : Reference.SurfacePoints)
		{
			const FIntVector K = Key(P);
			RefKeys.Add(K);
			++Result.Reference;
			Result.ReferenceUnderground += P.bIsUnderground ? 1 : 0;
			const bool* Class = MadeKeys.Find(K);
			Result.Missed += Class ? 0 : 1;
			Result.Reclassified += (Class && *Class != P.bIsUnderground) ? 1 : 0;
		}
		for (const TPair<FIntVector, bool>& Pair : MadeKeys)
		{
			Result.Spurious += RefKeys.Contains(Pair.Key) ? 0 : 1;
		}
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterPlanetFaceCavesTest,
	"VoxelWorlds.Scatter.Planet.FaceCaves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterPlanetFaceCavesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterPlanetFaceCavesTestUtils;
	FCaveSetup Setup;

	auto RunAll = [this, &Setup](const TArray<FBlock>& Blocks, const TCHAR* Label)
	{
		FFaceResult Total;
		for (const FBlock& Block : Blocks)
		{
			Total.Add(Compare(Setup, Block));
		}
		AddInfo(FString::Printf(TEXT("%s: %d chunks, %lld reference points (%lld underground); missed %lld, spurious %lld, reclassified %lld"),
			Label, Total.Chunks, Total.Reference, Total.ReferenceUnderground, Total.Missed, Total.Spurious, Total.Reclassified));
		return Total;
	};

	const TArray<FBlock> Blocks = MakeBlocks(Setup);
	const FFaceResult New = RunAll(Blocks, TEXT("Default caves"));
	TestEqual(TEXT("Default caves: no reference point missed"), New.Missed, static_cast<int64>(0));
	TestEqual(TEXT("Default caves: no point the reference lacks"), New.Spurious, static_cast<int64>(0));
	TestEqual(TEXT("Default caves: no point classified differently"), New.Reclassified, static_cast<int64>(0));
	TestTrue(TEXT("Default caves: cave floors in the chunks (not vacuous)"), New.ReferenceUnderground > 100);

	// Biome overrides: caves only in the commonest biome of the chunks, from a shallower depth.
	{
		TMap<uint8, int32> BiomeCounts;
		for (const FBlock& Block : Blocks)
		{
			for (const FVoxelData& V : Block.Voxels)
			{
				BiomeCounts.FindOrAdd(V.BiomeID) += 1;
			}
		}
		uint8 Common = 0;
		int32 CommonCount = -1;
		for (const TPair<uint8, int32>& Entry : BiomeCounts)
		{
			if (Entry.Value > CommonCount)
			{
				Common = Entry.Key;
				CommonCount = Entry.Value;
			}
		}
		Setup.Caves->BiomeOverrides.Empty();
		for (const TPair<uint8, int32>& Entry : BiomeCounts)
		{
			FBiomeCaveOverride Override;
			Override.BiomeID = Entry.Key;
			Override.CaveScale = Entry.Key == Common ? 1.5f : 0.0f;
			Override.MinDepthOverride = Entry.Key == Common ? 6.0f : -1.0f;
			Setup.Caves->BiomeOverrides.Add(Override);
		}
		const TArray<FBlock> OverrideBlocks = MakeBlocks(Setup);
		const FFaceResult Overrides = RunAll(OverrideBlocks, *FString::Printf(TEXT("Biome overrides (%d biomes, caves in %d)"), BiomeCounts.Num(), Common));
		TestEqual(TEXT("Biome overrides: no point missed"), Overrides.Missed, static_cast<int64>(0));
		TestEqual(TEXT("Biome overrides: no point added"), Overrides.Spurious, static_cast<int64>(0));
		TestEqual(TEXT("Biome overrides: no point classified differently"), Overrides.Reclassified, static_cast<int64>(0));
		Setup.Caves->BiomeOverrides.Empty();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
