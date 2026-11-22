// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelValleyNetwork.h"
#include "VoxelChunkValleySegments.h"
#include "IVoxelWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "IslandBowlWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelWaterLevels.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"

namespace VoxelValleyNetworkPrivate
{
	/** A hash of a cell and the seed: where the cell's node lies in it. */
	uint32 HashCell(int32 X, int32 Y, int32 Seed)
	{
		uint32 H = static_cast<uint32>(X) * 0x8da6b343u ^ static_cast<uint32>(Y) * 0xd8163841u ^ static_cast<uint32>(Seed) * 0xcb1ab31fu;
		H ^= H >> 13;
		H *= 0x5bd1e995u;
		H ^= H >> 15;
		return H;
	}

	/** Cells in a fixed order (row, then column), for every tie. */
	bool CellLess(const FIntPoint& A, const FIntPoint& B)
	{
		return A.Y != B.Y ? A.Y < B.Y : A.X < B.X;
	}

	/** The neighbours' steps, in the order every tie is broken by (row, then column: CellLess on a heightmap world) */
	constexpr int32 NeighbourSteps[8][2] = { { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 }, { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } };

	/** A width that grows with the square root of the drainage, from its least at the valley drainage to its most at the full-width drainage. */
	float WidthFor(float Min, float Max, int32 Drainage, const FVoxelValleySettings& Settings)
	{
		const double A = FMath::Sqrt(static_cast<double>(Settings.ValleyDrainage));
		const double B = FMath::Sqrt(static_cast<double>(FMath::Max(Settings.FullWidthDrainage, Settings.ValleyDrainage + 1)));
		const double T = FMath::Clamp((FMath::Sqrt(static_cast<double>(Drainage)) - A) / (B - A), 0.0, 1.0);
		return static_cast<float>(FMath::Lerp(static_cast<double>(Min), static_cast<double>(Max), T));
	}

	FVector Quad(const FVector& A, const FVector& C, const FVector& B, double T)
	{
		return (1.0 - T) * (1.0 - T) * A + 2.0 * T * (1.0 - T) * C + T * T * B;
	}

	/** The squared distance from a point to a segment */
	double SegmentDistanceSquared(const FVector& P, const FVector& A, const FVector& B)
	{
		const FVector AB = B - A;
		const double Length2 = AB.SizeSquared();
		const double T = Length2 > 0.0 ? FMath::Clamp(FVector::DotProduct(P - A, AB) / Length2, 0.0, 1.0) : 0.0;
		return FVector::DistSquared(P, A + AB * T);
	}

	/**
	 * The working graph of a tile's build: the tile's cells and every cell within the margin of them, each with its
	 * eight neighbours (-1 where a planet's cell has none, or the neighbour lies past the margin) and its distance in
	 * steps from the tile (Depth; 0 for the tile's own cells). On a heightmap world the margin is the square around the
	 * tile, a planet's the cells a breadth-first walk reaches in as many steps: the same cells on a square grid.
	 */
	struct FGrid
	{
		TArray<FIntPoint> Cells;
		TArray<int32> Depth;
		TArray<int32> Neighbours;  // 8 a node
		TArray<int32> Core;        // the tile's own nodes, row by row
		TArray<FVector> Position;
		TArray<double> Height;
		TArray<uint8> Sea;
		TArray<uint8> Sink;
		TArray<int32> Down;        // a node's index, or -1
		TArray<uint8> Breach;
		TArray<double> Floor;
		TArray<int32> Drainage;
		TArray<int32> MainUp;      // the valley link in with the most drainage, or -1

		int32 Num() const { return Cells.Num(); }
		FIntPoint CellAt(int32 K) const { return Cells[K]; }
		int32 NeighbourOf(int32 K, int32 Index) const { return Neighbours[K * 8 + Index]; }
	};

	struct FFloodItem
	{
		double H;
		int32 K;
		FIntPoint Cell;
		bool operator<(const FFloodItem& Other) const { return H != Other.H ? H < Other.H : CellLess(Cell, Other.Cell); }
	};

	/** A heightmap world's grid: the square of the tile's cells and the margin, its neighbours by index. */
	void MakeSquareGrid(const FIntPoint& FirstCell, int32 N, int32 M, FGrid& G)
	{
		const int32 Size = N + 2 * M;
		const FIntPoint Origin = FirstCell - FIntPoint(M, M);
		const int32 Count = Size * Size;
		G.Cells.SetNumUninitialized(Count);
		G.Depth.SetNumUninitialized(Count);
		G.Neighbours.SetNumUninitialized(Count * 8);
		for (int32 J = 0; J < Size; ++J)
		{
			for (int32 I = 0; I < Size; ++I)
			{
				const int32 K = J * Size + I;
				G.Cells[K] = Origin + FIntPoint(I, J);
				G.Depth[K] = FMath::Max(FMath::Max3(0, M - I, I - (M + N - 1)), FMath::Max(M - J, J - (M + N - 1)));
				for (int32 Index = 0; Index < 8; ++Index)
				{
					const int32 QI = I + NeighbourSteps[Index][0], QJ = J + NeighbourSteps[Index][1];
					G.Neighbours[K * 8 + Index] = (QI < 0 || QJ < 0 || QI >= Size || QJ >= Size) ? -1 : QJ * Size + QI;
				}
			}
		}
		for (int32 J = 0; J < N; ++J)
		{
			for (int32 I = 0; I < N; ++I)
			{
				G.Core.Add((J + M) * Size + I + M);
			}
		}
	}

	/** A planet's grid: the tile's cells, then a breadth-first walk through the lattice's neighbours out to the margin. */
	void MakeSphereGrid(const FVoxelValleyLattice& Lattice, const FIntPoint& FirstCell, const FIntPoint& TileSize, int32 M, FGrid& G)
	{
		TMap<FIntPoint, int32> IndexOf;
		for (int32 J = 0; J < TileSize.Y; ++J)
		{
			for (int32 I = 0; I < TileSize.X; ++I)
			{
				const FIntPoint Cell = FirstCell + FIntPoint(I, J);
				IndexOf.Add(Cell, G.Cells.Num());
				G.Core.Add(G.Cells.Num());
				G.Cells.Add(Cell);
				G.Depth.Add(0);
			}
		}
		for (int32 K = 0; K < G.Cells.Num(); ++K)
		{
			if (G.Depth[K] >= M)
			{
				continue;
			}
			for (int32 Index = 0; Index < 8; ++Index)
			{
				FIntPoint Next;
				if (Lattice.GetNeighbour(G.Cells[K], Index, Next) && !IndexOf.Contains(Next))
				{
					IndexOf.Add(Next, G.Cells.Num());
					G.Cells.Add(Next);
					G.Depth.Add(G.Depth[K] + 1);
				}
			}
		}
		G.Neighbours.Init(-1, G.Cells.Num() * 8);
		for (int32 K = 0; K < G.Cells.Num(); ++K)
		{
			for (int32 Index = 0; Index < 8; ++Index)
			{
				FIntPoint Next;
				if (Lattice.GetNeighbour(G.Cells[K], Index, Next))
				{
					if (const int32* Found = IndexOf.Find(Next))
					{
						G.Neighbours[K * 8 + Index] = *Found;
					}
				}
			}
		}
	}
}

// ==================== The ground ====================

FVoxelValleyGround FVoxelValleyGround::FromConfig(const UVoxelWorldConfiguration& Config)
{
	FVoxelValleyGround Ground;
	const FVoxelNoiseParams Noise = Config.GetNoiseParams();
	const FVoxelBiomeSnapshot Biome = FVoxelBiomeSnapshot::FromConfig(Config.BiomeConfiguration);
	Ground.Seed = Noise.Seed;
	if (Config.WorldMode == EWorldMode::SphericalPlanet)
	{
		// The planet's surface function (FSphericalPlanetWorldMode::SampleRadialDisplacement, what GetTerrainAltitude
		// gives before the valleys), from plain values captured by copy; its sea an altitude
		const FWorldModeTerrainParams Terrain = FVoxelWorldModeFactory::MakePlanetTerrainParams(Config);
		const double Radius = FMath::Max(static_cast<double>(FVoxelWorldModeFactory::MakePlanetParams(Config).PlanetRadius), 1.0);
		Ground.bRadial = true;
		Ground.PlanetRadius = Radius;
		Ground.Altitude = [Terrain, Noise, Biome, Radius](const FVector& Direction) -> double
		{
			return FSphericalPlanetWorldMode::SampleRadialDisplacement(Direction, Radius, Terrain, Noise, &Biome);
		};
		const FVoxelSeaSurface Sea = FVoxelWaterLevels::FromConfig(Config).GetSea();
		Ground.bSea = Sea.bEnabled && Sea.bRadial;
		Ground.SeaLevel = Ground.bSea ? Sea.Radius - Radius : 0.0;
		return Ground;
	}
	const FWorldModeTerrainParams Terrain = FVoxelWorldModeFactory::MakeTerrainParams(Config);
	const bool bIsland = Config.WorldMode == EWorldMode::IslandBowl;
	const FIslandBowlParams Island = bIsland ? FVoxelWorldModeFactory::MakeIslandParams(Config) : FIslandBowlParams();
	// The world mode's natural height (FInfinitePlaneWorldMode::SampleTerrainHeight, the island's falloff composed on
	// it), from plain values captured by copy: the valleys are not in it.
	Ground.Height = [Terrain, Noise, Biome, bIsland, Island](double X, double Y) -> double
	{
		const float FX = static_cast<float>(X), FY = static_cast<float>(Y);
		float Continentalness = 0.0f;
		const FWorldModeTerrainParams Effective = FInfinitePlaneWorldMode::ComputeEffectiveTerrainParams(FX, FY, Terrain, Noise, &Biome, Continentalness);
		float Height = FInfinitePlaneWorldMode::NoiseToTerrainHeight(FInfinitePlaneWorldMode::SampleTerrainNoise2D(FX, FY, Noise), Effective);
		if (bIsland)
		{
			Height = FIslandBowlWorldMode::ComposeTerrainHeight(FX, FY, Height, Island);
		}
		return Height;
	};
	Ground.bSea = Config.bEnableWaterLevel;
	Ground.SeaLevel = Ground.bSea ? Config.WaterLevel : Config.SeaLevel;
	return Ground;
}

// ==================== The lattice ====================

FVoxelValleyLattice::FVoxelValleyLattice(const FVoxelValleySettings& Settings, const FVoxelValleyGround& Ground)
	: bRadial(Ground.bRadial)
	, CellSize(Settings.CellSize)
	, Radius(Ground.bRadial ? FMath::Max(Ground.PlanetRadius, 1.0) : 0.0)
	, TileCells(FMath::Max(Settings.TileCells, 1))
{
	if (bRadial)
	{
		// As many cells along an edge as bring a cell nearest to CellSize at a face's middle, where a cell spans R pi/2 / N
		const int32 N = FMath::Max(1, FMath::RoundToInt32(UE_DOUBLE_HALF_PI * Radius / FMath::Max(static_cast<double>(Settings.CellSize), 1.0)));
		Cells = FVoxelCubeSphereGrid(N);
		CellSize = UE_DOUBLE_HALF_PI * Radius / N;
		TilesPerEdge = (N + TileCells - 1) / TileCells;
	}
}

FIntPoint FVoxelValleyLattice::CellOf(const FVector& Point) const
{
	if (bRadial)
	{
		return Cells.CellOfDirection(Point.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector));
	}
	return FIntPoint(FMath::FloorToInt32(Point.X / CellSize), FMath::FloorToInt32(Point.Y / CellSize));
}

FIntPoint FVoxelValleyLattice::TileOf(const FIntPoint& Cell) const
{
	// Floor division: the tile of a negative cell is the one below it
	const int32 T = TileCells;
	auto Div = [T](int32 V) { return V >= 0 ? V / T : -((-V + T - 1) / T); };
	if (bRadial)
	{
		int32 Face = 0, IU = 0, IV = 0;
		if (!Cells.FromAtlas(Cell, Face, IU, IV))
		{
			return FIntPoint(-1, -1);
		}
		return FIntPoint(Face * TilesPerEdge + IU / T, IV / T);
	}
	return FIntPoint(Div(Cell.X), Div(Cell.Y));
}

bool FVoxelValleyLattice::GetTileCells(const FIntPoint& Tile, FIntPoint& OutFirst, FIntPoint& OutSize) const
{
	if (!bRadial)
	{
		OutFirst = Tile * TileCells;
		OutSize = FIntPoint(TileCells, TileCells);
		return true;
	}
	const int32 N = Cells.GetCellsPerEdge();
	if (Tile.X < 0 || Tile.Y < 0 || Tile.X >= 6 * TilesPerEdge || Tile.Y >= TilesPerEdge)
	{
		return false;
	}
	const int32 Face = Tile.X / TilesPerEdge;
	const int32 IU = (Tile.X % TilesPerEdge) * TileCells, IV = Tile.Y * TileCells;
	OutFirst = Cells.ToAtlas(Face, IU, IV);
	OutSize = FIntPoint(FMath::Min(TileCells, N - IU), FMath::Min(TileCells, N - IV));
	return true;
}

bool FVoxelValleyLattice::GetNeighbour(const FIntPoint& Cell, int32 Index, FIntPoint& OutNeighbour) const
{
	using namespace VoxelValleyNetworkPrivate;
	const int32 DX = NeighbourSteps[Index & 7][0], DY = NeighbourSteps[Index & 7][1];
	if (bRadial)
	{
		return Cells.GetAdjacentCell(Cell, DX, DY, OutNeighbour);
	}
	OutNeighbour = Cell + FIntPoint(DX, DY);
	return true;
}

FVector FVoxelValleyLattice::PointAt(const FIntPoint& Cell, double FU, double FV) const
{
	if (bRadial)
	{
		return Cells.DirectionAt(Cell, FU, FV) * Radius;
	}
	return FVector((Cell.X + FU) * CellSize, (Cell.Y + FV) * CellSize, 0.0);
}

// ==================== The tile ====================

const FVoxelValleyNode* FVoxelValleyTile::FindNode(const FIntPoint& Cell) const
{
	const FIntPoint Local = Cell - FirstCell;
	if (Local.X < 0 || Local.Y < 0 || Local.X >= Size.X || Local.Y >= Size.Y)
	{
		return nullptr;
	}
	return &Nodes[Local.Y * Size.X + Local.X];
}

// ==================== The network ====================

FVoxelValleyNetwork::FVoxelValleyNetwork(const FVoxelValleySettings& InSettings, FVoxelValleyGround InGround)
	: Settings(Sanitize(InSettings))
	, Ground(MoveTemp(InGround))
	, Lattice(Settings, Ground)
{
}

bool FVoxelValleyNetwork::IsEnabled(const UVoxelWorldConfiguration& Config)
{
	return Config.ValleyConfiguration != nullptr
		&& (Config.WorldMode == EWorldMode::InfinitePlane || Config.WorldMode == EWorldMode::IslandBowl || Config.WorldMode == EWorldMode::SphericalPlanet);
}

FString FVoxelValleyNetwork::GetFingerprintTerm(const UVoxelWorldConfiguration& Config)
{
	if (!IsEnabled(Config))
	{
		return FString();
	}
	return FString::Printf(TEXT("Valleys=%d:%08X"), Version, Sanitize(Config.ValleyConfiguration->Settings).GetGenerationHash());
}

TSharedPtr<FVoxelValleyNetwork> FVoxelValleyNetwork::Create(const UVoxelWorldConfiguration& Config)
{
	if (!IsEnabled(Config))
	{
		return nullptr;
	}
	return MakeShared<FVoxelValleyNetwork>(Config.ValleyConfiguration->Settings, FVoxelValleyGround::FromConfig(Config));
}

FVoxelValleySettings FVoxelValleyNetwork::Sanitize(const FVoxelValleySettings& In)
{
	FVoxelValleySettings Out = In;
	Out.CellSize = FMath::Max(Out.CellSize, 5000.0f);
	Out.Jitter = FMath::Clamp(Out.Jitter, 0.0f, 0.95f);
	Out.MinLandHeight = FMath::Max(Out.MinLandHeight, 0.0f);
	Out.BreachCells = FMath::Clamp(Out.BreachCells, 1, 16);
	Out.DrainageReach = FMath::Clamp(Out.DrainageReach, 1, 16);
	Out.ValleyDrainage = FMath::Clamp(Out.ValleyDrainage, 1, 64);
	Out.FullWidthDrainage = FMath::Clamp(Out.FullWidthDrainage, Out.ValleyDrainage + 1, 1024);
	Out.FloorFraction = FMath::Clamp(Out.FloorFraction, 0.0f, 1.0f);
	Out.MinFloorHeight = FMath::Max(Out.MinFloorHeight, 0.0f);
	Out.FloorHalfWidthMin = FMath::Max(Out.FloorHalfWidthMin, 0.0f);
	Out.FloorHalfWidthMax = FMath::Max(Out.FloorHalfWidthMax, Out.FloorHalfWidthMin);
	// A side of at least a little width: the valley meets the hills smoothly
	Out.ValleyHalfWidthMin = FMath::Max(Out.ValleyHalfWidthMin, Out.FloorHalfWidthMin + 1000.0f);
	Out.ValleyHalfWidthMax = FMath::Max(Out.ValleyHalfWidthMax, FMath::Max(Out.FloorHalfWidthMax + 1000.0f, Out.ValleyHalfWidthMin));
	Out.RiverHalfWidthMin = FMath::Max(Out.RiverHalfWidthMin, 0.0f);
	Out.RiverHalfWidthMax = FMath::Max(Out.RiverHalfWidthMax, Out.RiverHalfWidthMin);
	Out.RiverWinding = FMath::Clamp(Out.RiverWinding, 0.0f, 1.0f);
	Out.RiverHalfWaves = FMath::Clamp(Out.RiverHalfWaves, 1, 8);
	Out.TileCells = FMath::Clamp(Out.TileCells, 2, 64);
	return Out;
}

int32 FVoxelValleyNetwork::GetMarginCells(const FVoxelValleySettings& InSettings)
{
	// A node's link reads its neighbours and the breaches of the sinks within BreachCells of it, each sink's flood
	// BreachCells further (2B + 1); its drainage the links DrainageReach upstream (R); its curve and its main link in a
	// node beyond each end (2); one more to spare.
	return InSettings.DrainageReach + 2 * InSettings.BreachCells + 4;
}

TSharedRef<FVoxelValleyTile> FVoxelValleyNetwork::BuildTile(const FVoxelValleySettings& S, const FVoxelValleyGround& Ground, const FIntPoint& TileCoord)
{
	using namespace VoxelValleyNetworkPrivate;

	TSharedRef<FVoxelValleyTile> Tile = MakeShared<FVoxelValleyTile>();
	Tile->Tile = TileCoord;
	Tile->TileCells = S.TileCells;
	const FVoxelValleyLattice Lattice(S, Ground);
	if (!Ground.IsValid() || !Lattice.GetTileCells(TileCoord, Tile->FirstCell, Tile->Size))
	{
		return Tile;
	}
	const bool bRadial = Ground.bRadial;
	const int32 M = GetMarginCells(S);
	const double L = Lattice.GetCellSize();
	const double SeaZ = Ground.SeaLevel;

	FGrid G;
	if (bRadial)
	{
		MakeSphereGrid(Lattice, Tile->FirstCell, Tile->Size, M, G);
	}
	else
	{
		MakeSquareGrid(Tile->FirstCell, S.TileCells, M, G);
	}
	const int32 Count = G.Num();
	G.Position.SetNumUninitialized(Count);
	G.Height.SetNumUninitialized(Count);
	G.Sea.SetNumZeroed(Count);
	G.Sink.SetNumZeroed(Count);
	G.Down.Init(-1, Count);
	G.Breach.SetNumZeroed(Count);
	G.Floor.SetNumZeroed(Count);
	G.Drainage.Init(1, Count);
	G.MainUp.Init(-1, Count);

	// ---- The nodes: placed by a hash, their ground ----
	for (int32 K = 0; K < Count; ++K)
	{
		const FIntPoint Cell = G.CellAt(K);
		const uint32 H = HashCell(Cell.X, Cell.Y, Ground.Seed);
		const double Ox = ((H & 0xffff) / 65535.0 - 0.5) * S.Jitter;
		const double Oy = (((H >> 16) & 0xffff) / 65535.0 - 0.5) * S.Jitter;
		G.Position[K] = bRadial ? Lattice.PointAt(Cell, 0.5 + Ox, 0.5 + Oy) : FVector((Cell.X + 0.5 + Ox) * L, (Cell.Y + 0.5 + Oy) * L, 0.0);
		G.Height[K] = Ground.HeightAt(G.Position[K]);
		G.Sea[K] = Ground.bSea && G.Height[K] < SeaZ + S.MinLandHeight;
	}

	// ---- The links: the steepest fall among the eight neighbours; a land node with none is a sink ----
	// Ties by the neighbour's cell (row, then column), which on a heightmap world is the order the neighbours are read in
	for (int32 K = 0; K < Count; ++K)
	{
		if (G.Sea[K] || G.Depth[K] > M - 1)
		{
			continue;
		}
		double Best = 0.0;
		for (int32 Index = 0; Index < 8; ++Index)
		{
			const int32 Q = G.NeighbourOf(K, Index);
			if (Q < 0)
			{
				continue;
			}
			const double Fall = (G.Height[K] - G.Height[Q]) / FVector::Distance(G.Position[K], G.Position[Q]);
			if (Fall > Best || (bRadial && Fall == Best && Best > 0.0 && CellLess(G.CellAt(Q), G.CellAt(G.Down[K]))))
			{
				Best = Fall;
				G.Down[K] = Q;
			}
		}
		G.Sink[K] = G.Down[K] == -1;
	}

	// ---- The sinks: each breached through its lowest saddle (D180) ----
	// From each sink a priority-flood over the nodes within BreachCells steps of it, by the highest ground on the way, to
	// the first node lower than it; the nodes on that path link along it. A node on the paths of two sinks takes the
	// lower sink's (by height, then cell), so the steps never close a loop: along links, a node's height or its path's
	// sink falls at every step. A sink with no lower node within reach keeps no link (its hollow is too wide).
	const int32 B = S.BreachCells;
	TArray<int32> PathSink;     // the sink whose path a node takes, or -1
	TArray<int32> PathNext;
	PathSink.Init(-1, Count);
	PathNext.Init(-1, Count);
	auto SinkBefore = [&G](int32 A, int32 C)
	{
		return G.Height[A] != G.Height[C] ? G.Height[A] < G.Height[C] : CellLess(G.CellAt(A), G.CellAt(C));
	};
	TMap<int32, int32> Ball;
	TArray<int32> BallOrder;
	for (int32 Sk = 0; Sk < Count; ++Sk)
	{
		if (!G.Sink[Sk] || G.Depth[Sk] > M - B - 1)
		{
			continue;
		}
		// The nodes within B steps of the sink (on a square grid the square of side 2B + 1 around it)
		Ball.Reset();
		BallOrder.Reset();
		Ball.Add(Sk, 0);
		BallOrder.Add(Sk);
		for (int32 At = 0; At < BallOrder.Num(); ++At)
		{
			const int32 K = BallOrder[At];
			const int32 Steps = Ball[K];
			if (Steps >= B)
			{
				continue;
			}
			for (int32 Index = 0; Index < 8; ++Index)
			{
				const int32 Q = G.NeighbourOf(K, Index);
				if (Q >= 0 && !Ball.Contains(Q))
				{
					Ball.Add(Q, Steps + 1);
					BallOrder.Add(Q);
				}
			}
		}
		TMap<int32, int32> Parent;
		TArray<FFloodItem> Heap;
		Heap.HeapPush({ G.Height[Sk], Sk, G.CellAt(Sk) });
		Parent.Add(Sk, -1);
		int32 Target = -1;
		while (Heap.Num() > 0)
		{
			FFloodItem Item;
			Heap.HeapPop(Item);
			if (Item.K != Sk && G.Height[Item.K] < G.Height[Sk])
			{
				Target = Item.K;
				break;
			}
			for (int32 Index = 0; Index < 8; ++Index)
			{
				const int32 Q = G.NeighbourOf(Item.K, Index);
				if (Q < 0 || !Ball.Contains(Q) || Parent.Contains(Q))
				{
					continue;
				}
				Parent.Add(Q, Item.K);
				Heap.HeapPush({ FMath::Max(Item.H, G.Height[Q]), Q, G.CellAt(Q) });
			}
		}
		if (Target == -1)
		{
			continue;
		}
		// The path from the sink to the target, then each of its nodes but the target takes it unless a lower sink's has
		TArray<int32> Path;
		for (int32 C = Target; C != -1; C = Parent[C])
		{
			Path.Insert(C, 0);
		}
		for (int32 P = 0; P + 1 < Path.Num(); ++P)
		{
			const int32 Q = Path[P];
			if (PathSink[Q] == -1 || SinkBefore(Sk, PathSink[Q]))
			{
				PathSink[Q] = Sk;
				PathNext[Q] = Path[P + 1];
			}
		}
	}
	for (int32 K = 0; K < Count; ++K)
	{
		if (PathSink[K] != -1)
		{
			G.Down[K] = PathNext[K];
			G.Breach[K] = 1;
		}
	}

	// ---- The floors: a share of the ground over the sea, at least MinFloorHeight over it (or the ground, if lower);
	// on a breach the sink's; at a coast MinFloorHeight under the sea's level, so a valley comes down to a drowned mouth
	// and not to a flat at the sea's very level, which the sea does not cover (the sea stands where the ground is under
	// it) and a river reaching it would end on land (the valleys plan's V3). Each term grows with the ground, so a
	// floor falls along a link
	auto OwnFloor = [&](int32 K)
	{
		const double Over = FMath::Max(0.0, G.Height[K] - SeaZ);
		return G.Sea[K] ? SeaZ - static_cast<double>(S.MinFloorHeight)
			: SeaZ + FMath::Max(S.FloorFraction * Over, FMath::Min(Over, static_cast<double>(S.MinFloorHeight)));
	};
	for (int32 K = 0; K < Count; ++K)
	{
		G.Floor[K] = PathSink[K] != -1 ? OwnFloor(PathSink[K]) : OwnFloor(K);
	}

	// ---- The drainage: each node adds itself to the nodes DrainageReach links downstream ----
	for (int32 K = 0; K < Count; ++K)
	{
		int32 C = G.Down[K];
		for (int32 Step = 0; Step < S.DrainageReach && C != -1; ++Step)
		{
			++G.Drainage[C];
			C = G.Down[C];
		}
	}

	// ---- The valleys, and each node's main valley link in ----
	auto IsValley = [&](int32 K) { return !G.Sea[K] && G.Down[K] != -1 && G.Drainage[K] >= S.ValleyDrainage; };
	for (int32 K = 0; K < Count; ++K)
	{
		if (!IsValley(K))
		{
			continue;
		}
		const int32 D = G.Down[K];
		const int32 Current = G.MainUp[D];
		if (Current == -1 || G.Drainage[K] > G.Drainage[Current] || (G.Drainage[K] == G.Drainage[Current] && CellLess(G.CellAt(K), G.CellAt(Current))))
		{
			G.MainUp[D] = K;
		}
	}

	// ---- The tile's nodes ----
	Tile->Nodes.SetNum(G.Core.Num());
	for (int32 Local = 0; Local < G.Core.Num(); ++Local)
	{
		const int32 K = G.Core[Local];
		FVoxelValleyNode& Node = Tile->Nodes[Local];
		Node.Cell = G.CellAt(K);
		Node.Position = G.Position[K];
		Node.Height = G.Height[K];
		Node.Floor = G.Floor[K];
		Node.bHasLink = G.Down[K] != -1;
		Node.Down = Node.bHasLink ? G.CellAt(G.Down[K]) : FIntPoint::ZeroValue;
		Node.bSea = G.Sea[K] != 0;
		Node.bSink = G.Sink[K] != 0;
		Node.bBreach = G.Breach[K] != 0;
		Node.Drainage = G.Drainage[K];
		Node.bValley = IsValley(K);
		Node.bHead = Node.bValley && G.MainUp[K] == -1;
	}

	// ---- The valley links' curves ----
	// A node's point on the curve: its corner spread between its main link in and its link out. A link's curve is the
	// end of its upper node's corner piece and the start of its lower node's; a side valley (not the lower node's main
	// link in) runs straight from the link's middle onto the trunk's curve at the node. On a planet every point of the
	// curve is brought back onto the datum sphere (a mean of points on a sphere lies under it); the control points are
	// the plane's means, not brought back, so a node's corner point is the same from the link in and the link out
	auto CornerPoint = [&G, &Lattice](int32 K) -> FVector
	{
		if (G.MainUp[K] == -1 || G.Down[K] == -1 || G.Sea[K])
		{
			return G.Position[K];
		}
		return Lattice.OntoSurface(0.125 * G.Position[G.MainUp[K]] + 0.75 * G.Position[K] + 0.125 * G.Position[G.Down[K]]);
	};
	const double Step = L / 12.0;
	for (const int32 K : G.Core)
	{
		if (!IsValley(K))
		{
			continue;
		}
		const int32 D = G.Down[K];
		const FVector P = G.Position[K], PD = G.Position[D];
		const FVector Mid = 0.5 * (P + PD);
		const bool bHead = G.MainUp[K] == -1;
		const FVector InCorner = bHead ? P : 0.5 * (G.Position[G.MainUp[K]] + P);
		const bool bMainOfNext = G.MainUp[D] == K && G.Down[D] != -1 && !G.Sea[D];
		const FVector End = CornerPoint(D);
		const int32 Steps = FMath::Max(4, FMath::CeilToInt32(FVector::Distance(P, PD) / Step));
		const int32 First = Steps / 2;
		TArray<FVector> Points;
		for (int32 Q = 0; Q <= First; ++Q)
		{
			const double T = 0.5 + 0.5 * Q / First;
			Points.Add(Lattice.OntoSurface(bHead ? FMath::Lerp(P, Mid, (T - 0.5) * 2.0) : Quad(InCorner, P, Mid, T)));
		}
		for (int32 Q = 1; Q <= Steps - First; ++Q)
		{
			const double T = 0.5 * Q / (Steps - First);
			const FVector Out = bMainOfNext ? Quad(Mid, PD, 0.5 * (PD + G.Position[G.Down[D]]), T) : FMath::Lerp(Mid, End, T * 2.0);
			Points.Add(Lattice.OntoSurface(Out));
		}
		TArray<double> Arc;
		Arc.Add(0.0);
		for (int32 Q = 1; Q < Points.Num(); ++Q)
		{
			Arc.Add(Arc.Last() + FVector::Distance(Points[Q - 1], Points[Q]));
		}
		const double Length = FMath::Max(Arc.Last(), 1.0);
		const float FloorHalf = WidthFor(S.FloorHalfWidthMin, S.FloorHalfWidthMax, G.Drainage[K], S);
		const float ValleyHalf = FMath::Max(FloorHalf + 1000.0f, WidthFor(S.ValleyHalfWidthMin, S.ValleyHalfWidthMax, G.Drainage[K], S));
		const float RiverHalf = FMath::Min(WidthFor(S.RiverHalfWidthMin, S.RiverHalfWidthMax, G.Drainage[K], S), 0.8f * FloorHalf);
		for (int32 Q = 0; Q + 1 < Points.Num(); ++Q)
		{
			FVoxelValleySegment Segment;
			Segment.A = Points[Q];
			Segment.B = Points[Q + 1];
			Segment.LinkA = static_cast<float>(Arc[Q] / Length);
			Segment.LinkB = static_cast<float>(Arc[Q + 1] / Length);
			Segment.FloorA = FMath::Lerp(G.Floor[K], G.Floor[D], static_cast<double>(Segment.LinkA));
			Segment.FloorB = FMath::Lerp(G.Floor[K], G.Floor[D], static_cast<double>(Segment.LinkB));
			Segment.FloorHalfWidth = FloorHalf;
			Segment.ValleyHalfWidth = ValleyHalf;
			Segment.RiverHalfWidth = RiverHalf;
			Segment.TaperA = bHead ? static_cast<float>(FMath::SmoothStep(0.0, 0.5, static_cast<double>(Segment.LinkA))) : 1.0f;
			Segment.TaperB = bHead ? static_cast<float>(FMath::SmoothStep(0.0, 0.5, static_cast<double>(Segment.LinkB))) : 1.0f;
			Segment.Link = G.CellAt(K);
			Tile->Segments.Add(Segment);
			Tile->Bounds += Segment.A - FVector(ValleyHalf);
			Tile->Bounds += Segment.A + FVector(ValleyHalf);
			Tile->Bounds += Segment.B - FVector(ValleyHalf);
			Tile->Bounds += Segment.B + FVector(ValleyHalf);
		}
	}
	return Tile;
}

TSharedRef<const FVoxelValleyTile> FVoxelValleyNetwork::GetTile(const FIntPoint& TileCoord) const
{
	{
		FReadScopeLock ReadLock(Lock);
		if (const TSharedRef<const FVoxelValleyTile>* Found = Tiles.Find(TileCoord))
		{
			return *Found;
		}
	}
	// Built outside the lock: a tile asked for twice at once is built twice, the same both times, and the first kept
	const TSharedRef<const FVoxelValleyTile> Built = BuildTile(Settings, Ground, TileCoord);
	FWriteScopeLock WriteLock(Lock);
	return Tiles.FindOrAdd(TileCoord, Built);
}

FVoxelValleyNode FVoxelValleyNetwork::GetNode(const FIntPoint& Cell) const
{
	const TSharedRef<const FVoxelValleyTile> Tile = GetTile(TileOf(Cell));
	const FVoxelValleyNode* Node = Tile->FindNode(Cell);
	return Node ? *Node : FVoxelValleyNode();
}

void FVoxelValleyNetwork::GatherSegments(const FBox2D& Bounds, TArray<FVoxelValleySegment>& Out) const
{
	if (Ground.bRadial || !Bounds.bIsValid)
	{
		return;
	}
	// A link's curve lies within a cell and a half of its upper node, and its valley a half width beyond
	const double Reach = Settings.ValleyHalfWidthMax + 2.0 * Settings.CellSize;
	const FIntPoint Low = TileOf(CellOf(Bounds.Min - FVector2D(Reach)));
	const FIntPoint High = TileOf(CellOf(Bounds.Max + FVector2D(Reach)));
	for (int32 TY = Low.Y; TY <= High.Y; ++TY)
	{
		for (int32 TX = Low.X; TX <= High.X; ++TX)
		{
			const TSharedRef<const FVoxelValleyTile> Tile = GetTile(FIntPoint(TX, TY));
			if (!Tile->Bounds.IsValid || !FBox2D(FVector2D(Tile->Bounds.Min), FVector2D(Tile->Bounds.Max)).Intersect(Bounds))
			{
				continue;
			}
			for (const FVoxelValleySegment& Segment : Tile->Segments)
			{
				FBox2D Box(ForceInit);
				Box += FVector2D(Segment.A);
				Box += FVector2D(Segment.B);
				if (Box.ExpandBy(Segment.ValleyHalfWidth).Intersect(Bounds))
				{
					Out.Add(Segment);
				}
			}
		}
	}
}

void FVoxelValleyNetwork::GetTilesNear(const FVector& Point, double Distance, TArray<FIntPoint>& OutTiles) const
{
	OutTiles.Reset();
	const FVoxelCubeSphereGrid& Cells = Lattice.GetCells();
	if (!Cells.IsValid())
	{
		return;
	}
	const double Radius = Lattice.GetRadius();
	TArray<FIntPoint> Near;
	if (Distance < 0.0)
	{
		Cells.GatherCentresWithinAngle(FVector::UpVector, UE_DOUBLE_PI, Near);
	}
	else
	{
		// A link's curve lies within a cell and a half of its upper node, and its valley a half width beyond; a cell's
		// centre within a cell's diagonal of every point of it
		const double Reach = Distance + Settings.ValleyHalfWidthMax + 3.5 * Lattice.GetCellSize();
		const double Angle = Reach >= 2.0 * Radius ? UE_DOUBLE_PI : 2.0 * FMath::Asin(Reach / (2.0 * Radius));
		Cells.GatherCentresWithinAngle(Point.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector), Angle, Near);
	}
	TSet<FIntPoint> Seen;
	for (const FIntPoint& Cell : Near)
	{
		const FIntPoint Tile = TileOf(Cell);
		if (!Seen.Contains(Tile))
		{
			Seen.Add(Tile);
			OutTiles.Add(Tile);
		}
	}
	OutTiles.Sort(VoxelValleyNetworkPrivate::CellLess);
}

void FVoxelValleyNetwork::GatherSegmentsNear(const FVector& Point, double Distance, TArray<FVoxelValleySegment>& Out) const
{
	if (!Ground.bRadial)
	{
		const FVector2D At(Point.X, Point.Y);
		GatherSegments(FBox2D(At - FVector2D(Distance), At + FVector2D(Distance)), Out);
		return;
	}
	TArray<FIntPoint> Near;
	GetTilesNear(Point, Distance, Near);
	const bool bAll = Distance < 0.0;
	for (const FIntPoint& TileCoord : Near)
	{
		const TSharedRef<const FVoxelValleyTile> Tile = GetTile(TileCoord);
		if (!Tile->Bounds.IsValid || (!bAll && Tile->Bounds.ComputeSquaredDistanceToPoint(Point) > FMath::Square(Distance)))
		{
			continue;
		}
		for (const FVoxelValleySegment& Segment : Tile->Segments)
		{
			if (bAll || VoxelValleyNetworkPrivate::SegmentDistanceSquared(Point, Segment.A, Segment.B) <= FMath::Square(Distance + Segment.ValleyHalfWidth))
			{
				Out.Add(Segment);
			}
		}
	}
}

int32 FVoxelValleyNetwork::GetNumTiles() const
{
	FReadScopeLock ReadLock(Lock);
	return Tiles.Num();
}

TSharedRef<const FVoxelValleyNetwork::FReach> FVoxelValleyNetwork::GetReach(const FIntPoint& TileCoord) const
{
	{
		FReadScopeLock ReadLock(ReachLock);
		if (const TSharedRef<const FReach>* Found = Reaches.Find(TileCoord))
		{
			return *Found;
		}
	}
	TSharedRef<FReach> Reach = MakeShared<FReach>();
	TArray<FVoxelValleySegment> Segments;
	TArray<TArray<int32>> Lists;
	if (Ground.bRadial)
	{
		// A planet: the tile's cells are the buckets. The frame's origin is the tile's middle on the datum sphere; a
		// segment goes in every cell its valley comes within the cell's reach of (its corners' farthest from its middle)
		if (!Lattice.GetTileCells(TileCoord, Reach->FirstCell, Reach->Size))
		{
			Reach->First.Init(0, 2);
			FWriteScopeLock WriteLock(ReachLock);
			return Reaches.FindOrAdd(TileCoord, Reach);
		}
		Reach->Origin = Lattice.PointAt(Reach->FirstCell, 0.5 * Reach->Size.X, 0.5 * Reach->Size.Y);
		double TileReach = 0.0;
		for (const FVector2D& Corner : { FVector2D(0, 0), FVector2D(1, 0), FVector2D(0, 1), FVector2D(1, 1) })
		{
			TileReach = FMath::Max(TileReach, FVector::Distance(Reach->Origin, Lattice.PointAt(Reach->FirstCell, Corner.X * Reach->Size.X, Corner.Y * Reach->Size.Y)));
		}
		GatherSegmentsNear(Reach->Origin, TileReach, Segments);
		Reach->Buckets = Reach->Size.X * Reach->Size.Y;
		Lists.SetNum(Reach->Buckets);
		TArray<FVector> Middles;
		TArray<double> Reaches2;
		for (int32 J = 0; J < Reach->Size.Y; ++J)
		{
			for (int32 I = 0; I < Reach->Size.X; ++I)
			{
				const FIntPoint Cell = Reach->FirstCell + FIntPoint(I, J);
				const FVector Middle = Lattice.PointAt(Cell, 0.5, 0.5);
				double CellReach = 0.0;
				for (const FVector2D& Corner : { FVector2D(0, 0), FVector2D(1, 0), FVector2D(0, 1), FVector2D(1, 1) })
				{
					CellReach = FMath::Max(CellReach, FVector::Distance(Middle, Lattice.PointAt(Cell, Corner.X, Corner.Y)));
				}
				Middles.Add(Middle);
				Reaches2.Add(CellReach);
			}
		}
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FVoxelValleySegment& Segment = Segments[Index];
			VoxelValleyCarve::AppendRows(Segment, Reach->Origin, Reach->Rows);
			for (int32 Bucket = 0; Bucket < Reach->Buckets; ++Bucket)
			{
				if (VoxelValleyNetworkPrivate::SegmentDistanceSquared(Middles[Bucket], Segment.A, Segment.B) <= FMath::Square(Reaches2[Bucket] + Segment.ValleyHalfWidth))
				{
					Lists[Bucket].Add(Index);
				}
			}
		}
	}
	else
	{
		// The tile's square, its segments and the buckets: a segment goes in every bucket its valley overlaps
		const double Side = static_cast<double>(Settings.TileCells) * Settings.CellSize;
		Reach->Origin = FVector(TileCoord.X * Side, TileCoord.Y * Side, 0.0);
		Reach->Buckets = 8;
		Reach->BucketSize = Side / Reach->Buckets;
		const FVector2D Origin2D(Reach->Origin.X, Reach->Origin.Y);
		GatherSegments(FBox2D(Origin2D, Origin2D + FVector2D(Side)), Segments);
		Lists.SetNum(Reach->Buckets * Reach->Buckets);
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FVoxelValleySegment& Segment = Segments[Index];
			VoxelValleyCarve::AppendRows(Segment, Reach->Origin, Reach->Rows);
			const double R = Segment.ValleyHalfWidth;
			const FVector2D Low = FVector2D(FMath::Min(Segment.A.X, Segment.B.X) - R, FMath::Min(Segment.A.Y, Segment.B.Y) - R) - Origin2D;
			const FVector2D High = FVector2D(FMath::Max(Segment.A.X, Segment.B.X) + R, FMath::Max(Segment.A.Y, Segment.B.Y) + R) - Origin2D;
			const int32 X0 = FMath::Clamp(FMath::FloorToInt32(Low.X / Reach->BucketSize), 0, Reach->Buckets - 1);
			const int32 X1 = FMath::Clamp(FMath::FloorToInt32(High.X / Reach->BucketSize), 0, Reach->Buckets - 1);
			const int32 Y0 = FMath::Clamp(FMath::FloorToInt32(Low.Y / Reach->BucketSize), 0, Reach->Buckets - 1);
			const int32 Y1 = FMath::Clamp(FMath::FloorToInt32(High.Y / Reach->BucketSize), 0, Reach->Buckets - 1);
			for (int32 Y = Y0; Y <= Y1; ++Y)
			{
				for (int32 X = X0; X <= X1; ++X)
				{
					Lists[Y * Reach->Buckets + X].Add(Index);
				}
			}
		}
	}
	Reach->First.Reserve(Lists.Num() + 1);
	for (const TArray<int32>& List : Lists)
	{
		Reach->First.Add(Reach->Items.Num());
		Reach->Items.Append(List);
	}
	Reach->First.Add(Reach->Items.Num());
	FWriteScopeLock WriteLock(ReachLock);
	return Reaches.FindOrAdd(TileCoord, Reach);
}

double FVoxelValleyNetwork::Evaluate(const FReach& Reach, int32 Bucket, const FVector3f& Local, double Natural)
{
	if (Reach.First[Bucket] == Reach.First[Bucket + 1])
	{
		return Natural;
	}
	// In float, in the tile's frame: the generators' evaluation
	const float N = static_cast<float>(Natural);
	float Cut = 0.0f, Fill = 0.0f;
	for (int32 Item = Reach.First[Bucket]; Item < Reach.First[Bucket + 1]; ++Item)
	{
		const FVector4f* Rows = &Reach.Rows[Reach.Items[Item] * VoxelValleyCarve::SegmentStride];
		float Weight = 0.0f, Floor = 0.0f;
		VoxelValleyCarve::SegmentAt(Local, Rows[0], Rows[1], Rows[2], Weight, Floor);
		if (Weight > 0.0f)
		{
			VoxelValleyCarve::Fold(Weight, Floor, N, Cut, Fill);
		}
	}
	return Cut == 0.0f && Fill == 0.0f ? Natural : static_cast<double>(VoxelValleyCarve::Finish(N, Cut, Fill));
}

double FVoxelValleyNetwork::ApplyToHeight(double X, double Y, double Natural) const
{
	if (Ground.bRadial || (Ground.bSea && Natural < Ground.SeaLevel))
	{
		return Natural;
	}
	const TSharedRef<const FReach> Reach = GetReach(TileOf(CellOf(FVector2D(X, Y))));
	const FVector2D Local = FVector2D(X, Y) - FVector2D(Reach->Origin.X, Reach->Origin.Y);
	const int32 BX = FMath::Clamp(FMath::FloorToInt32(Local.X / Reach->BucketSize), 0, Reach->Buckets - 1);
	const int32 BY = FMath::Clamp(FMath::FloorToInt32(Local.Y / Reach->BucketSize), 0, Reach->Buckets - 1);
	return Evaluate(*Reach, BY * Reach->Buckets + BX, FVector3f(static_cast<float>(Local.X), static_cast<float>(Local.Y), 0.0f), Natural);
}

double FVoxelValleyNetwork::ApplyToAltitude(const FVector& Direction, double Natural) const
{
	if (!Ground.bRadial || (Ground.bSea && Natural < Ground.SeaLevel))
	{
		return Natural;
	}
	const FVector Unit = Direction.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
	const FIntPoint Cell = Lattice.CellOf(Unit);
	const TSharedRef<const FReach> Reach = GetReach(TileOf(Cell));
	const FIntPoint Local = Cell - Reach->FirstCell;
	if (Local.X < 0 || Local.Y < 0 || Local.X >= Reach->Size.X || Local.Y >= Reach->Size.Y)
	{
		return Natural;
	}
	return Evaluate(*Reach, Local.Y * Reach->Size.X + Local.X, FVector3f(Unit * Lattice.GetRadius() - Reach->Origin), Natural);
}

void IVoxelWorldMode::AddValleyFloorsToRange(const FVector2D& MinXY, const FVector2D& MaxXY, FFloatInterval& InOutRange) const
{
	const TSharedPtr<const FVoxelValleyNetwork> Valleys = GetValleyNetwork();
	double FloorMin = 0.0, FloorMax = 0.0;
	if (Valleys.IsValid() && Valleys->GetFloorRange(FBox2D(MinXY, MaxXY), FloorMin, FloorMax))
	{
		InOutRange.Min = FMath::Min(InOutRange.Min, static_cast<float>(FloorMin));
		InOutRange.Max = FMath::Max(InOutRange.Max, static_cast<float>(FloorMax));
	}
}

bool FVoxelValleyNetwork::GetFloorRange(const FBox2D& Bounds, double& OutMin, double& OutMax) const
{
	TArray<FVoxelValleySegment> Segments;
	GatherSegments(Bounds, Segments);
	OutMin = MAX_dbl;
	OutMax = -MAX_dbl;
	for (const FVoxelValleySegment& Segment : Segments)
	{
		OutMin = FMath::Min3(OutMin, Segment.FloorA, Segment.FloorB);
		OutMax = FMath::Max3(OutMax, Segment.FloorA, Segment.FloorB);
	}
	return Segments.Num() > 0;
}

bool FVoxelValleyNetwork::GetFloorRangeNear(const FVector& Point, double Distance, double& OutMin, double& OutMax) const
{
	TArray<FVoxelValleySegment> Segments;
	GatherSegmentsNear(Point, Distance, Segments);
	OutMin = MAX_dbl;
	OutMax = -MAX_dbl;
	for (const FVoxelValleySegment& Segment : Segments)
	{
		OutMin = FMath::Min3(OutMin, Segment.FloorA, Segment.FloorB);
		OutMax = FMath::Max3(OutMax, Segment.FloorA, Segment.FloorB);
	}
	return Segments.Num() > 0;
}
