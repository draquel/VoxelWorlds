// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelChunkMeshCards.h"
#include "VoxelRendering.h"
#include "MeshCardBuild.h"
#include "MeshCardRepresentation.h"

#include <atomic>

namespace
{
	constexpr int32 NumCardDirections = 6;
	constexpr int32 MaxCardsPerMesh = 32;
	constexpr int32 MaxFittedResolution = 64;
	/** A grid cell holds surface (bit 7) and which of the six directions some of it faces (bits 0-5) */
	constexpr uint8 OccupiedBit = 0x80;
	/** How far a card's box may reach past the mesh's box along its depth, as the engine's builder leaves (LOD, displacement) */
	constexpr double CardDepthMargin = 10.0;

	/** A direction's card space, as the engine's builder lays it out (InitClusteringParams): U and V across, depth from the
	 *  box's face on the direction's side into the mesh */
	struct FCardBasis
	{
		int32 Axis = 0;
		int32 U = 0;
		int32 V = 0;
		bool bPositive = false;

		explicit FCardBasis(int32 Direction)
			: Axis(Direction / 2)
			, U(Axis == 0 ? 1 : 0)
			, V(Axis == 2 ? 1 : 2)
			, bPositive((Direction & 1) != 0)
		{
		}
	};

	/** A surface element: a column's cell (U, V, depth Z) where surface facing the direction follows empty space, and the
	 *  nearest near plane a card may take to capture it (one past the last surface in front of it) */
	struct FCardSurfel
	{
		uint8 U = 0;
		uint8 V = 0;
		uint8 Z = 0;
		uint8 MinZ = 0;
	};

	/** A cluster of surface elements one card captures: its near plane and its cells' bounds (U, V, depth) */
	struct FCardCluster
	{
		int32 Direction = 0;
		int32 NearPlane = 0;
		int32 Coverage = 0;
		FIntVector Min = FIntVector(MAX_int32);
		FIntVector Max = FIntVector(MIN_int32);

		void Add(const FCardSurfel& Surfel)
		{
			++Coverage;
			Min = FIntVector(FMath::Min<int32>(Min.X, Surfel.U), FMath::Min<int32>(Min.Y, Surfel.V), FMath::Min<int32>(Min.Z, Surfel.Z));
			Max = FIntVector(FMath::Max<int32>(Max.X, Surfel.U), FMath::Max<int32>(Max.Y, Surfel.V), FMath::Max<int32>(Max.Z, Surfel.Z));
		}

		int32 GetFaceArea() const
		{
			return Coverage > 0 ? (Max.X - Min.X + 1) * (Max.Y - Min.Y + 1) : 0;
		}
	};

	std::atomic<int64> GCardMeshes{ 0 };
	std::atomic<int64> GCards{ 0 };
	std::atomic<int64> GFittedMeshes{ 0 };
	std::atomic<int64> GSurfels{ 0 };
	std::atomic<int64> GCoveredSurfels{ 0 };
	std::atomic<int64> GCapped{ 0 };
	/** Build times in microseconds */
	std::atomic<int64> GBuildMicros{ 0 };
	std::atomic<int64> GMaxBuildMicros{ 0 };

	FAutoConsoleCommand CmdVoxelLumenCardStats(
		TEXT("voxel.Render.LumenCardStats"),
		TEXT("The chunk meshes' Lumen cards (G116) since the start or the last reset: meshes built with cards, cards a mesh, and ")
		TEXT("for the fitted cards (voxel.Render.LumenCards 2) the share of the sampled surface the cards capture, the meshes ")
		TEXT("whose clusters MaxCards cut and the build's time (average and longest). 'reset' clears the counts."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const FVoxelChunkMeshCardCounters::FCounts C = FVoxelChunkMeshCardCounters::Get();
			UE_LOG(LogVoxelRendering, Display,
				TEXT("voxel.Render.LumenCardStats: %lld meshes with cards, %.2f cards a mesh; fitted %lld meshes, surface covered %.1f %% (%lld of %lld elements), %lld capped; build %.1f us a mesh, longest %.1f us, %.1f ms in all"),
				C.Meshes, C.Meshes > 0 ? double(C.Cards) / C.Meshes : 0.0, C.FittedMeshes,
				C.Surfels > 0 ? 100.0 * C.CoveredSurfels / C.Surfels : 0.0, C.CoveredSurfels, C.Surfels, C.Capped,
				C.Meshes > 0 ? 1.0e6 * C.Seconds / C.Meshes : 0.0, 1.0e6 * C.MaxSeconds, 1.0e3 * C.Seconds);
			if (Args.Num() > 0 && Args[0].Equals(TEXT("reset"), ESearchCase::IgnoreCase))
			{
				FVoxelChunkMeshCardCounters::Reset();
			}
		}));
}

// ==================== BuildVoxelChunkMeshFittedCards ====================

void BuildVoxelChunkMeshFittedCards(TConstArrayView<FVoxelVertex> Vertices, TConstArrayView<uint32> Indices, const FBox& LocalBounds,
	const FVoxelChunkMeshCardSettings& Settings, FMeshCardsBuildData& OutCardData, FVoxelChunkMeshCardBuildStats* OutStats)
{
	OutCardData.CardBuildData.Reset();
	FVoxelChunkMeshCardBuildStats Stats;
	if (!LocalBounds.IsValid || Indices.Num() < 3)
	{
		if (OutStats)
		{
			*OutStats = Stats;
		}
		return;
	}

	// The grid: cubic cells, Resolution of them along the box's longest side (the engine's builder takes 10 uu cells up to
	// 64 a side; a chunk mesh's grid sits at about two of its voxels a cell with the default 16, so a layer two voxels below
	// another is told apart). The grid starts a quarter cell before the box, so that faces on the box's own grid (a cubic
	// mesh's, two voxels a cell) lie inside cells, never on the boundary a card's near plane takes
	const int32 Resolution = FMath::Clamp(Settings.Resolution, 4, MaxFittedResolution);
	const FVector BoxSize = LocalBounds.GetSize();
	const double CellSize = FMath::Max(BoxSize.GetMax(), 1.0) / (Resolution - 0.5);
	const float InvCellSize = static_cast<float>(1.0 / CellSize);
	FIntVector Dims;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		Dims[Axis] = FMath::Clamp(FMath::CeilToInt32((BoxSize[Axis] + 0.5 * CellSize) / CellSize - 1.0e-3), 1, Resolution);
	}
	const FVector3f GridMin(LocalBounds.Min - FVector(0.25 * CellSize));
	TArray<uint8> Cells;
	Cells.SetNumZeroed(Dims.X * Dims.Y * Dims.Z);
	auto CellIndex = [&Dims](int32 X, int32 Y, int32 Z) { return X + Dims.X * (Y + Dims.Y * Z); };
	auto MarkCell = [&](const FVector3f& Point, uint8 Bits)
	{
		const FVector3f Local = (Point - GridMin) * InvCellSize;
		const int32 X = FMath::Clamp(FMath::FloorToInt32(Local.X), 0, Dims.X - 1);
		const int32 Y = FMath::Clamp(FMath::FloorToInt32(Local.Y), 0, Dims.Y - 1);
		const int32 Z = FMath::Clamp(FMath::FloorToInt32(Local.Z), 0, Dims.Z - 1);
		Cells[CellIndex(X, Y, Z)] |= Bits;
	};

	// Sample every triangle into the grid at half a cell's spacing: each cell it touches holds surface, and faces the
	// directions the triangle faces by at least the engine's normal threshold (a slope counts for two directions, as there).
	// The facing comes from the triangle's own normal, turned to agree with its vertices' (whatever the winding)
	const float NormalThreshold = MeshCardRepresentation::GetNormalTreshold();
	const int32 NumVertices = Vertices.Num();
	for (int32 Index = 0; Index + 2 < Indices.Num(); Index += 3)
	{
		const uint32 I0 = Indices[Index];
		const uint32 I1 = Indices[Index + 1];
		const uint32 I2 = Indices[Index + 2];
		if (I0 >= static_cast<uint32>(NumVertices) || I1 >= static_cast<uint32>(NumVertices) || I2 >= static_cast<uint32>(NumVertices))
		{
			continue;
		}
		const FVector3f P0 = Vertices[I0].Position;
		const FVector3f E1 = Vertices[I1].Position - P0;
		const FVector3f E2 = Vertices[I2].Position - P0;
		FVector3f Normal = FVector3f::CrossProduct(E1, E2);
		const float Length = Normal.Size();
		if (Length < UE_KINDA_SMALL_NUMBER)
		{
			continue;
		}
		Normal /= Length;
		const FVector3f VertexNormals = Vertices[I0].GetNormal() + Vertices[I1].GetNormal() + Vertices[I2].GetNormal();
		if (FVector3f::DotProduct(Normal, VertexNormals) < 0.0f)
		{
			Normal = -Normal;
		}

		uint8 Bits = OccupiedBit;
		for (int32 Direction = 0; Direction < NumCardDirections; ++Direction)
		{
			const float Along = (Direction & 1) ? Normal[Direction / 2] : -Normal[Direction / 2];
			if (Along >= NormalThreshold)
			{
				Bits |= 1 << Direction;
			}
		}

		const float LongestEdge = FMath::Sqrt(FMath::Max3(E1.SizeSquared(), E2.SizeSquared(), (E2 - E1).SizeSquared()));
		const int32 Steps = FMath::Clamp(FMath::CeilToInt32(2.0f * LongestEdge * InvCellSize), 1, 4 * MaxFittedResolution);
		const float InvSteps = 1.0f / Steps;
		for (int32 I = 0; I <= Steps; ++I)
		{
			for (int32 J = 0; I + J <= Steps; ++J)
			{
				MarkCell(P0 + E1 * (I * InvSteps) + E2 * (J * InvSteps), Bits);
			}
		}
		MarkCell(P0 + (E1 + E2) * (1.0f / 3.0f), Bits);
	}

	// Per direction: the surface elements, walking each column from the box's face on that side (the engine's rays: a hit
	// facing the direction that follows empty space starts an element; one right behind the last surface is the same
	// surface's thickness, not a new layer)
	const float MinDensity = MeshCardRepresentation::GetMinDensity() / 3.0f;
	TArray<FCardCluster> Clusters;
	TArray<FCardSurfel> Surfels;
	TBitArray<> Assigned;
	for (int32 Direction = 0; Direction < NumCardDirections; ++Direction)
	{
		const FCardBasis Basis(Direction);
		const int32 DimU = Dims[Basis.U];
		const int32 DimV = Dims[Basis.V];
		const int32 DimZ = Dims[Basis.Axis];
		const uint8 FacingBit = 1 << Direction;

		Surfels.Reset();
		for (int32 V = 0; V < DimV; ++V)
		{
			for (int32 U = 0; U < DimU; ++U)
			{
				int32 LastHit = -2;
				FIntVector Coord;
				Coord[Basis.U] = U;
				Coord[Basis.V] = V;
				for (int32 Z = 0; Z < DimZ; ++Z)
				{
					Coord[Basis.Axis] = Basis.bPositive ? DimZ - 1 - Z : Z;
					const uint8 Cell = Cells[CellIndex(Coord.X, Coord.Y, Coord.Z)];
					if (!(Cell & OccupiedBit))
					{
						continue;
					}
					if ((Cell & FacingBit) && Z > LastHit + 1)
					{
						FCardSurfel& Surfel = Surfels.AddDefaulted_GetRef();
						Surfel.U = static_cast<uint8>(U);
						Surfel.V = static_cast<uint8>(V);
						Surfel.Z = static_cast<uint8>(Z);
						Surfel.MinZ = static_cast<uint8>(LastHit >= 0 ? LastHit + 1 : 0);
					}
					LastHit = Z;
				}
			}
		}
		Stats.Surfels += Surfels.Num();
		if (Surfels.Num() == 0)
		{
			continue;
		}

		// Clusters, greedily: the outer layer first (near plane 0, as the engine's builder), then each round the near plane
		// that captures most of what is left, while a cluster covers enough cells (MinCells; the outer one half a face's at
		// most) densely enough (the engine's minimum density over three)
		const int32 FaceCells = (DimU * DimV + DimU * DimZ + DimV * DimZ) / 3;
		const int32 MinInnerCoverage = FMath::Max(Settings.MinCells, 1);
		const int32 MinOuterCoverage = FMath::Clamp(FaceCells / 2, 1, MinInnerCoverage);
		Assigned.Init(false, Surfels.Num());
		FCardCluster PlaneClusters[MaxFittedResolution];
		bool bOuter = true;
		while (true)
		{
			for (int32 Plane = 0; Plane < DimZ; ++Plane)
			{
				PlaneClusters[Plane] = FCardCluster();
				PlaneClusters[Plane].Direction = Direction;
				PlaneClusters[Plane].NearPlane = Plane;
			}
			for (int32 SurfelIndex = 0; SurfelIndex < Surfels.Num(); ++SurfelIndex)
			{
				if (!Assigned[SurfelIndex])
				{
					const FCardSurfel& Surfel = Surfels[SurfelIndex];
					for (int32 Plane = Surfel.MinZ; Plane <= Surfel.Z; ++Plane)
					{
						PlaneClusters[Plane].Add(Surfel);
					}
				}
			}

			auto IsValid = [&](const FCardCluster& Cluster)
			{
				return Cluster.Coverage >= (Cluster.NearPlane == 0 ? MinOuterCoverage : MinInnerCoverage)
					&& Cluster.Coverage >= MinDensity * Cluster.GetFaceArea();
			};
			int32 BestPlane = INDEX_NONE;
			for (int32 Plane = bOuter ? 0 : 1; Plane < (bOuter ? 1 : DimZ); ++Plane)
			{
				if (IsValid(PlaneClusters[Plane]) && (BestPlane == INDEX_NONE || PlaneClusters[Plane].Coverage > PlaneClusters[BestPlane].Coverage))
				{
					BestPlane = Plane;
				}
			}
			if (BestPlane == INDEX_NONE)
			{
				if (bOuter)
				{
					bOuter = false;
					continue;
				}
				break;
			}
			bOuter = false;

			for (int32 SurfelIndex = 0; SurfelIndex < Surfels.Num(); ++SurfelIndex)
			{
				const FCardSurfel& Surfel = Surfels[SurfelIndex];
				if (!Assigned[SurfelIndex] && Surfel.MinZ <= BestPlane && BestPlane <= Surfel.Z)
				{
					Assigned[SurfelIndex] = true;
				}
			}
			Clusters.Add(PlaneClusters[BestPlane]);
		}
	}
	Stats.Clusters = Clusters.Num();

	// Lumen takes 32 cards a mesh: keep the MaxCards that cover most
	const int32 MaxCards = FMath::Clamp(Settings.MaxCards, 1, MaxCardsPerMesh);
	if (Clusters.Num() > MaxCards)
	{
		Clusters.StableSort([](const FCardCluster& A, const FCardCluster& B) { return A.Coverage > B.Coverage; });
		Clusters.SetNum(MaxCards);
	}

	// A card over each cluster's cells: across, clamped to the mesh's box; along its depth from the near plane (the cell
	// boundary behind the last surface in front, so that surface stays out of the box) to half a cell past the deepest
	// element, as the engine's builder
	OutCardData.CardBuildData.Reserve(Clusters.Num());
	for (const FCardCluster& Cluster : Clusters)
	{
		const FCardBasis Basis(Cluster.Direction);
		const double Face = Basis.bPositive ? GridMin[Basis.Axis] + Dims[Basis.Axis] * CellSize : GridMin[Basis.Axis];
		const double Sign = Basis.bPositive ? -1.0 : 1.0;

		FVector Min;
		FVector Max;
		Min[Basis.U] = FMath::Max(GridMin[Basis.U] + Cluster.Min.X * CellSize, LocalBounds.Min[Basis.U]);
		Max[Basis.U] = FMath::Min(GridMin[Basis.U] + (Cluster.Max.X + 1) * CellSize, LocalBounds.Max[Basis.U]);
		Min[Basis.V] = FMath::Max(GridMin[Basis.V] + Cluster.Min.Y * CellSize, LocalBounds.Min[Basis.V]);
		Max[Basis.V] = FMath::Min(GridMin[Basis.V] + (Cluster.Max.Y + 1) * CellSize, LocalBounds.Max[Basis.V]);
		const double Near = Face + Sign * Cluster.NearPlane * CellSize;
		const double Far = Face + Sign * (Cluster.Max.Z + 1.5) * CellSize;
		Min[Basis.Axis] = FMath::Max(FMath::Min(Near, Far), LocalBounds.Min[Basis.Axis] - CardDepthMargin);
		Max[Basis.Axis] = FMath::Min(FMath::Max(Near, Far), LocalBounds.Max[Basis.Axis] + CardDepthMargin);

		FVector3f AxisX = FVector3f::ZeroVector;
		FVector3f AxisY = FVector3f::ZeroVector;
		FVector3f AxisZ = FVector3f::ZeroVector;
		AxisX[Basis.U] = 1.0f;
		AxisY[Basis.V] = 1.0f;
		AxisZ[Basis.Axis] = Basis.bPositive ? 1.0f : -1.0f;
		const FVector Extent = (Max - Min) * 0.5;

		FLumenCardBuildData& Card = OutCardData.CardBuildData.AddDefaulted_GetRef();
		Card.OBB.Origin = FVector3f((Min + Max) * 0.5);
		Card.OBB.AxisX = AxisX;
		Card.OBB.AxisY = AxisY;
		Card.OBB.AxisZ = AxisZ;
		Card.OBB.Extent = FVector3f(Extent[Basis.U], Extent[Basis.V], Extent[Basis.Axis]);
		Card.AxisAlignedDirectionIndex = static_cast<uint8>(Cluster.Direction);
		Card.DilationMode = ELumenCardDilationMode::Disabled;
		Stats.CoveredSurfels += Cluster.Coverage;
	}

	if (OutStats)
	{
		*OutStats = Stats;
	}
}

// ==================== FVoxelChunkMeshCardCounters ====================

FVoxelChunkMeshCardCounters::FCounts FVoxelChunkMeshCardCounters::Get()
{
	FCounts Counts;
	Counts.Meshes = GCardMeshes.load(std::memory_order_relaxed);
	Counts.Cards = GCards.load(std::memory_order_relaxed);
	Counts.FittedMeshes = GFittedMeshes.load(std::memory_order_relaxed);
	Counts.Surfels = GSurfels.load(std::memory_order_relaxed);
	Counts.CoveredSurfels = GCoveredSurfels.load(std::memory_order_relaxed);
	Counts.Capped = GCapped.load(std::memory_order_relaxed);
	Counts.Seconds = GBuildMicros.load(std::memory_order_relaxed) * 1.0e-6;
	Counts.MaxSeconds = GMaxBuildMicros.load(std::memory_order_relaxed) * 1.0e-6;
	return Counts;
}

void FVoxelChunkMeshCardCounters::Reset()
{
	GCardMeshes = 0;
	GCards = 0;
	GFittedMeshes = 0;
	GSurfels = 0;
	GCoveredSurfels = 0;
	GCapped = 0;
	GBuildMicros = 0;
	GMaxBuildMicros = 0;
}

void FVoxelChunkMeshCardCounters::AddBuild(int32 NumCards, double Seconds, const FVoxelChunkMeshCardBuildStats* Stats, int32 MaxCards)
{
	GCardMeshes.fetch_add(1, std::memory_order_relaxed);
	GCards.fetch_add(NumCards, std::memory_order_relaxed);
	if (Stats)
	{
		GFittedMeshes.fetch_add(1, std::memory_order_relaxed);
		GSurfels.fetch_add(Stats->Surfels, std::memory_order_relaxed);
		GCoveredSurfels.fetch_add(Stats->CoveredSurfels, std::memory_order_relaxed);
		if (Stats->Clusters > MaxCards)
		{
			GCapped.fetch_add(1, std::memory_order_relaxed);
		}
	}
	const int64 Micros = static_cast<int64>(Seconds * 1.0e6);
	GBuildMicros.fetch_add(Micros, std::memory_order_relaxed);
	int64 Longest = GMaxBuildMicros.load(std::memory_order_relaxed);
	while (Micros > Longest && !GMaxBuildMicros.compare_exchange_weak(Longest, Micros, std::memory_order_relaxed))
	{
	}
}
