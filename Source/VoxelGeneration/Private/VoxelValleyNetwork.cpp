// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelValleyNetwork.h"
#include "InfinitePlaneWorldMode.h"
#include "IslandBowlWorldMode.h"
#include "VoxelBiomeSnapshot.h"
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

	/** A width that grows with the square root of the drainage, from its least at the valley drainage to its most at the full-width drainage. */
	float WidthFor(float Min, float Max, int32 Drainage, const FVoxelValleySettings& Settings)
	{
		const double A = FMath::Sqrt(static_cast<double>(Settings.ValleyDrainage));
		const double B = FMath::Sqrt(static_cast<double>(FMath::Max(Settings.FullWidthDrainage, Settings.ValleyDrainage + 1)));
		const double T = FMath::Clamp((FMath::Sqrt(static_cast<double>(Drainage)) - A) / (B - A), 0.0, 1.0);
		return static_cast<float>(FMath::Lerp(static_cast<double>(Min), static_cast<double>(Max), T));
	}

	FVector2D Quad(const FVector2D& A, const FVector2D& C, const FVector2D& B, double T)
	{
		return (1.0 - T) * (1.0 - T) * A + 2.0 * T * (1.0 - T) * C + T * T * B;
	}

	/** The working grid of a tile's build: the tile's cells and the margin around them. */
	struct FGrid
	{
		int32 Size = 0;
		FIntPoint Origin = FIntPoint::ZeroValue;   // the cell of index (0, 0)
		TArray<FVector2D> Position;
		TArray<double> Height;
		TArray<uint8> Sea;
		TArray<uint8> Sink;
		TArray<int32> Down;        // a grid index, or -1
		TArray<uint8> Breach;
		TArray<double> Floor;
		TArray<int32> Drainage;
		TArray<int32> MainUp;      // the valley link in with the most drainage, or -1

		int32 Index(int32 I, int32 J) const { return (I < 0 || J < 0 || I >= Size || J >= Size) ? -1 : J * Size + I; }
		FIntPoint CellAt(int32 K) const { return Origin + FIntPoint(K % Size, K / Size); }
	};

	struct FFloodItem
	{
		double H;
		int32 K;
		FIntPoint Cell;
		bool operator<(const FFloodItem& Other) const { return H != Other.H ? H < Other.H : CellLess(Cell, Other.Cell); }
	};
}

// ==================== The ground ====================

FVoxelValleyGround FVoxelValleyGround::FromConfig(const UVoxelWorldConfiguration& Config)
{
	FVoxelValleyGround Ground;
	const FWorldModeTerrainParams Terrain = FVoxelWorldModeFactory::MakeTerrainParams(Config);
	const FVoxelNoiseParams Noise = Config.GetNoiseParams();
	const FVoxelBiomeSnapshot Biome = FVoxelBiomeSnapshot::FromConfig(Config.BiomeConfiguration);
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
	Ground.Seed = Noise.Seed;
	return Ground;
}

// ==================== The tile ====================

const FVoxelValleyNode* FVoxelValleyTile::FindNode(const FIntPoint& Cell) const
{
	const FIntPoint Local = Cell - Tile * TileCells;
	if (Local.X < 0 || Local.Y < 0 || Local.X >= TileCells || Local.Y >= TileCells)
	{
		return nullptr;
	}
	return &Nodes[Local.Y * TileCells + Local.X];
}

// ==================== The network ====================

FVoxelValleyNetwork::FVoxelValleyNetwork(const FVoxelValleySettings& InSettings, FVoxelValleyGround InGround)
	: Settings(Sanitize(InSettings))
	, Ground(MoveTemp(InGround))
{
}

bool FVoxelValleyNetwork::IsEnabled(const UVoxelWorldConfiguration& Config)
{
	return Config.ValleyConfiguration != nullptr
		&& (Config.WorldMode == EWorldMode::InfinitePlane || Config.WorldMode == EWorldMode::IslandBowl)
		&& Config.MeshingMode != EMeshingMode::Cubic;
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
	if (!Ground.IsValid())
	{
		return Tile;
	}

	const int32 N = S.TileCells;
	const int32 M = GetMarginCells(S);
	const double L = S.CellSize;
	const double SeaZ = Ground.SeaLevel;

	FGrid G;
	G.Size = N + 2 * M;
	G.Origin = TileCoord * N - FIntPoint(M, M);
	const int32 Count = G.Size * G.Size;
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
		G.Position[K] = FVector2D((Cell.X + 0.5 + Ox) * L, (Cell.Y + 0.5 + Oy) * L);
		G.Height[K] = Ground.Height(G.Position[K].X, G.Position[K].Y);
		G.Sea[K] = Ground.bSea && G.Height[K] < SeaZ + S.MinLandHeight;
	}

	// ---- The links: the steepest fall among the eight neighbours; a land node with none is a sink ----
	for (int32 J = 1; J < G.Size - 1; ++J)
	{
		for (int32 I = 1; I < G.Size - 1; ++I)
		{
			const int32 K = G.Index(I, J);
			if (G.Sea[K])
			{
				continue;
			}
			double Best = 0.0;
			for (int32 DJ = -1; DJ <= 1; ++DJ)
			{
				for (int32 DI = -1; DI <= 1; ++DI)
				{
					if (DI == 0 && DJ == 0)
					{
						continue;
					}
					const int32 Q = G.Index(I + DI, J + DJ);
					const double Fall = (G.Height[K] - G.Height[Q]) / FVector2D::Distance(G.Position[K], G.Position[Q]);
					if (Fall > Best)
					{
						Best = Fall;
						G.Down[K] = Q;
					}
				}
			}
			G.Sink[K] = G.Down[K] == -1;
		}
	}

	// ---- The sinks: each breached through its lowest saddle (D180) ----
	// From each sink a priority-flood over the nodes within BreachCells of it, by the highest ground on the way, to the
	// first node lower than it; the nodes on that path link along it. A node on the paths of two sinks takes the lower
	// sink's (by height, then cell), so the steps never close a loop: along links, a node's height or its path's sink
	// falls at every step. A sink with no lower node within reach keeps no link (its hollow is too wide).
	const int32 B = S.BreachCells;
	TArray<int32> PathSink;     // the sink whose path a node takes, or -1
	TArray<int32> PathNext;
	PathSink.Init(-1, Count);
	PathNext.Init(-1, Count);
	auto SinkBefore = [&G](int32 A, int32 C)
	{
		return G.Height[A] != G.Height[C] ? G.Height[A] < G.Height[C] : CellLess(G.CellAt(A), G.CellAt(C));
	};
	for (int32 J = B + 1; J < G.Size - B - 1; ++J)
	{
		for (int32 I = B + 1; I < G.Size - B - 1; ++I)
		{
			const int32 Sk = G.Index(I, J);
			if (!G.Sink[Sk])
			{
				continue;
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
				const int32 FI = Item.K % G.Size, FJ = Item.K / G.Size;
				for (int32 DJ = -1; DJ <= 1; ++DJ)
				{
					for (int32 DI = -1; DI <= 1; ++DI)
					{
						if ((DI == 0 && DJ == 0) || FMath::Max(FMath::Abs(FI + DI - I), FMath::Abs(FJ + DJ - J)) > B)
						{
							continue;
						}
						const int32 Q = G.Index(FI + DI, FJ + DJ);
						if (Q < 0 || Parent.Contains(Q))
						{
							continue;
						}
						Parent.Add(Q, Item.K);
						Heap.HeapPush({ FMath::Max(Item.H, G.Height[Q]), Q, G.CellAt(Q) });
					}
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
	}
	for (int32 K = 0; K < Count; ++K)
	{
		if (PathSink[K] != -1)
		{
			G.Down[K] = PathNext[K];
			G.Breach[K] = 1;
		}
	}

	// ---- The floors: a share of the ground over the sea; on a breach the sink's; at a coast the sea's level ----
	auto OwnFloor = [&](int32 K) { return G.Sea[K] ? SeaZ : SeaZ + S.FloorFraction * FMath::Max(0.0, G.Height[K] - SeaZ); };
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
	Tile->Nodes.SetNum(N * N);
	for (int32 J = 0; J < N; ++J)
	{
		for (int32 I = 0; I < N; ++I)
		{
			const int32 K = G.Index(I + M, J + M);
			FVoxelValleyNode& Node = Tile->Nodes[J * N + I];
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
	}

	// ---- The valley links' curves ----
	// A node's point on the curve: its corner spread between its main link in and its link out. A link's curve is the
	// end of its upper node's corner piece and the start of its lower node's; a side valley (not the lower node's main
	// link in) runs straight from the link's middle onto the trunk's curve at the node.
	auto CornerPoint = [&G](int32 K) -> FVector2D
	{
		if (G.MainUp[K] == -1 || G.Down[K] == -1 || G.Sea[K])
		{
			return G.Position[K];
		}
		return 0.125 * G.Position[G.MainUp[K]] + 0.75 * G.Position[K] + 0.125 * G.Position[G.Down[K]];
	};
	const double Step = L / 12.0;
	for (int32 J = 0; J < N; ++J)
	{
		for (int32 I = 0; I < N; ++I)
		{
			const int32 K = G.Index(I + M, J + M);
			if (!IsValley(K))
			{
				continue;
			}
			const int32 D = G.Down[K];
			const FVector2D P = G.Position[K], PD = G.Position[D];
			const FVector2D Mid = 0.5 * (P + PD);
			const bool bHead = G.MainUp[K] == -1;
			const FVector2D InCorner = bHead ? P : 0.5 * (G.Position[G.MainUp[K]] + P);
			const bool bMainOfNext = G.MainUp[D] == K && G.Down[D] != -1 && !G.Sea[D];
			const FVector2D End = CornerPoint(D);
			const int32 Steps = FMath::Max(4, FMath::CeilToInt32(FVector2D::Distance(P, PD) / Step));
			const int32 First = Steps / 2;
			TArray<FVector2D> Points;
			for (int32 Q = 0; Q <= First; ++Q)
			{
				const double T = 0.5 + 0.5 * Q / First;
				Points.Add(bHead ? FMath::Lerp(P, Mid, (T - 0.5) * 2.0) : Quad(InCorner, P, Mid, T));
			}
			for (int32 Q = 1; Q <= Steps - First; ++Q)
			{
				const double T = 0.5 * Q / (Steps - First);
				Points.Add(bMainOfNext ? Quad(Mid, PD, 0.5 * (PD + G.Position[G.Down[D]]), T) : FMath::Lerp(Mid, End, T * 2.0));
			}
			TArray<double> Arc;
			Arc.Add(0.0);
			for (int32 Q = 1; Q < Points.Num(); ++Q)
			{
				Arc.Add(Arc.Last() + FVector2D::Distance(Points[Q - 1], Points[Q]));
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
				Tile->Bounds += Segment.A - FVector2D(ValleyHalf);
				Tile->Bounds += Segment.A + FVector2D(ValleyHalf);
				Tile->Bounds += Segment.B - FVector2D(ValleyHalf);
				Tile->Bounds += Segment.B + FVector2D(ValleyHalf);
			}
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
	// A link's curve lies within a cell and a half of its upper node, and its valley a half width beyond
	const double Reach = Settings.ValleyHalfWidthMax + 2.0 * Settings.CellSize;
	const FIntPoint Low = TileOf(CellOf(Bounds.Min - FVector2D(Reach)));
	const FIntPoint High = TileOf(CellOf(Bounds.Max + FVector2D(Reach)));
	for (int32 TY = Low.Y; TY <= High.Y; ++TY)
	{
		for (int32 TX = Low.X; TX <= High.X; ++TX)
		{
			const TSharedRef<const FVoxelValleyTile> Tile = GetTile(FIntPoint(TX, TY));
			if (!Tile->Bounds.bIsValid || !Tile->Bounds.Intersect(Bounds))
			{
				continue;
			}
			for (const FVoxelValleySegment& Segment : Tile->Segments)
			{
				FBox2D Box(ForceInit);
				Box += Segment.A;
				Box += Segment.B;
				if (Box.ExpandBy(Segment.ValleyHalfWidth).Intersect(Bounds))
				{
					Out.Add(Segment);
				}
			}
		}
	}
}

FIntPoint FVoxelValleyNetwork::CellOf(const FVector2D& Position) const
{
	return FIntPoint(FMath::FloorToInt32(Position.X / Settings.CellSize), FMath::FloorToInt32(Position.Y / Settings.CellSize));
}

FIntPoint FVoxelValleyNetwork::TileOf(const FIntPoint& Cell) const
{
	// Floor division: the tile of a negative cell is the one below it
	const int32 T = Settings.TileCells;
	auto Div = [T](int32 V) { return V >= 0 ? V / T : -((-V + T - 1) / T); };
	return FIntPoint(Div(Cell.X), Div(Cell.Y));
}

int32 FVoxelValleyNetwork::GetNumTiles() const
{
	FReadScopeLock ReadLock(Lock);
	return Tiles.Num();
}
