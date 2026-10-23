// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelSurfaceNavigationSubsystem.h"
#include "VoxelNavigation.h"

#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "VoxelChunkManager.h"
#include "VoxelCoordinates.h"
#include "VoxelEditManager.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelWorldConfiguration.h"

// ---------------------------------------------------------------------------
// CVars
// ---------------------------------------------------------------------------

static TAutoConsoleVariable<float> CVarNavCellSize(TEXT("voxel.Nav.CellSize"), 150.f,
	TEXT("Surface navigation grid cell size (world units). Changing it drops the height cache."));
static TAutoConsoleVariable<int32> CVarNavMaxNodes(TEXT("voxel.Nav.MaxNodes"), 4096,
	TEXT("Node budget per surface path search."));
static TAutoConsoleVariable<float> CVarNavMaxStepRise(TEXT("voxel.Nav.MaxStepRise"), 100.f,
	TEXT("Largest height difference between neighbouring cells a walker may take (100 at the 150 cell = ~34 deg)."));
static TAutoConsoleVariable<float> CVarNavMaxWadeDepth(TEXT("voxel.Nav.MaxWadeDepth"), 60.f,
	TEXT("Cells this far below the water surface are still walkable."));
static TAutoConsoleVariable<float> CVarNavMaxRadius(TEXT("voxel.Nav.MaxRadius"), 20000.f,
	TEXT("Cells farther than this from the start are never expanded."));
static TAutoConsoleVariable<int32> CVarNavDebug(TEXT("voxel.Nav.Debug"), 0,
	TEXT("1 = draw the walkability grid around the debug focus and recent routes."));

/** A planet's search regions are cells of a cube-sphere grid about this wide (four 4,800 uu chunks). */
static constexpr double PlanetNavRegionSize = 19200.0;

/** A planet region's height cache is dropped after this many seconds unused. */
static constexpr double PlanetNavRegionLifetime = 60.0;

// ---------------------------------------------------------------------------
// Tile
// ---------------------------------------------------------------------------

UVoxelSurfaceNavigationSubsystem::FNavTile::FNavTile()
{
	Heights.SetNumZeroed(TileSize * TileSize);
	Sampled.Init(false, TileSize * TileSize);
	FromLoaded.Init(false, TileSize * TileSize);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool UVoxelSurfaceNavigationSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UVoxelSurfaceNavigationSubsystem::Deinitialize()
{
	if (UVoxelChunkManager* ChunkMgr = ChunkManager.Get())
	{
		if (bBound)
		{
			ChunkMgr->OnChunkLoaded.RemoveDynamic(this, &UVoxelSurfaceNavigationSubsystem::HandleChunkLoaded);
		}
		if (bEditBound && ChunkMgr->GetEditManager())
		{
			ChunkMgr->GetEditManager()->OnChunkEdited.Remove(EditHandle);
		}
	}
	Tiles.Reset();
	Regions.Reset();
	Super::Deinitialize();
}

UVoxelSurfaceNavigationSubsystem* UVoxelSurfaceNavigationSubsystem::Get(const UObject* WorldContextObject)
{
	const UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
	return World ? World->GetSubsystem<UVoxelSurfaceNavigationSubsystem>() : nullptr;
}

void UVoxelSurfaceNavigationSubsystem::Tick(float DeltaTime)
{
	// Expire temporary blocks.
	if (TempBlocks.Num() > 0)
	{
		const double Now = GetWorld()->GetTimeSeconds();
		bool bChanged = false;
		for (int32 i = TempBlocks.Num() - 1; i >= 0; --i)
		{
			if (TempBlocks[i].ExpireTime <= Now)
			{
				TempBlocks.RemoveAtSwap(i);
				bChanged = true;
			}
		}
		if (bChanged)
		{
			TempBlockedCells.Reset();
			for (const FTempBlock& Block : TempBlocks)
			{
				TempBlockedCells.Add(Block.Cell);
			}
		}
	}

	// Planets: expire temporary blocks and regions unused for a minute.
	if (TempSpheres.Num() > 0)
	{
		const double Now = GetWorld()->GetTimeSeconds();
		TempSpheres.RemoveAllSwap([Now](const FTempSphere& Block) { return Block.ExpireTime <= Now; });
	}
	if (Regions.Num() > 0)
	{
		const double Now = FPlatformTime::Seconds();
		for (auto It = Regions.CreateIterator(); It; ++It)
		{
			if (It.Value() && Now - It.Value()->LastUsedSeconds > PlanetNavRegionLifetime)
			{
				CachedCells -= It.Value()->Cells;
				It.RemoveCurrent();
			}
		}
	}

	if (CVarNavDebug.GetValueOnGameThread() > 0 || DebugPaths.Num() > 0)
	{
		DrawDebug();
	}
}

// ---------------------------------------------------------------------------
// Chunk manager
// ---------------------------------------------------------------------------

void UVoxelSurfaceNavigationSubsystem::SetChunkManager(UVoxelChunkManager* InChunkManager)
{
	if (InChunkManager && InChunkManager != ChunkManager.Get())
	{
		ChunkManager = InChunkManager;
		bBound = false;
		bEditBound = false;
		InvalidateAll();
		BindChunkManager(InChunkManager);
	}
}

UVoxelChunkManager* UVoxelSurfaceNavigationSubsystem::ResolveChunkManager()
{
	if (UVoxelChunkManager* Cached = ChunkManager.Get())
	{
		if (!bEditBound)
		{
			BindChunkManager(Cached); // the edit manager may have appeared since
		}
		return Cached;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (UVoxelChunkManager* Found = It->FindComponentByClass<UVoxelChunkManager>())
		{
			SetChunkManager(Found);
			return Found;
		}
	}
	return nullptr;
}

void UVoxelSurfaceNavigationSubsystem::BindChunkManager(UVoxelChunkManager* ChunkMgr)
{
	if (!ChunkMgr)
	{
		return;
	}
	if (!bBound)
	{
		ChunkMgr->OnChunkLoaded.AddDynamic(this, &UVoxelSurfaceNavigationSubsystem::HandleChunkLoaded);
		bBound = true;
	}
	if (!bEditBound)
	{
		if (UVoxelEditManager* Edits = ChunkMgr->GetEditManager())
		{
			EditHandle = Edits->OnChunkEdited.AddUObject(this, &UVoxelSurfaceNavigationSubsystem::HandleChunkEdited);
			bEditBound = true;
		}
	}
}

bool UVoxelSurfaceNavigationSubsystem::IsReady() const
{
	const UVoxelChunkManager* ChunkMgr = ChunkManager.Get();
	return ChunkMgr && ChunkMgr->IsInitialized() && ChunkMgr->GetConfiguration();
}

// ---------------------------------------------------------------------------
// Params / cells
// ---------------------------------------------------------------------------

void UVoxelSurfaceNavigationSubsystem::ApplyCVars()
{
	Params.CellSize = FMath::Max(CVarNavCellSize.GetValueOnGameThread(), 25.f);
	Params.MaxNodes = FMath::Max(CVarNavMaxNodes.GetValueOnGameThread(), 16);
	Params.MaxStepRise = FMath::Max(CVarNavMaxStepRise.GetValueOnGameThread(), 0.f);
	Params.MaxWadeDepth = FMath::Max(CVarNavMaxWadeDepth.GetValueOnGameThread(), 0.f);
	Params.MaxSearchRadius = FMath::Max(CVarNavMaxRadius.GetValueOnGameThread(), 0.f);
	if (const UVoxelChunkManager* ChunkMgr = ChunkManager.Get())
	{
		if (const UVoxelWorldConfiguration* Config = ChunkMgr->GetConfiguration())
		{
			Params.bHasWater = Config->bEnableWaterLevel;
			// The test itself goes through the sea (SP-E1): the WaterLevel plane generation floods (G19), the sea sphere
			// on a planet.
			Params.Sea = ChunkMgr->GetSeaSurface();
			Params.WaterLevel = Params.Sea.bRadial ? Config->WaterLevel : static_cast<float>(Params.Sea.PlaneZ);

			// A planet searches in region frames (WM-SY11).
			const bool bWasPlanet = bPlanet;
			bPlanet = Config->WorldMode == EWorldMode::SphericalPlanet;
			if (bPlanet && !bWasPlanet)
			{
				PlanetCenter = Config->WorldOrigin;
				PlanetRadius = FMath::Max(static_cast<double>(Config->WorldRadius), 1.0);
				PlanetSea = Params.Sea;
				RegionGrid = FVoxelCubeSphereGrid(FVoxelCubeSphereGrid::CellsPerEdgeFor(PlanetRadius, PlanetNavRegionSize));
				Regions.Reset();
			}
		}
	}
	if (CachedCellSize != Params.CellSize)
	{
		Tiles.Reset();
		CachedCells = 0;
		TempBlocks.Reset();
		TempBlockedCells.Reset();
		Regions.Reset();
		TempSpheres.Reset();
		CachedCellSize = Params.CellSize;
	}
}

FVoxelSurfaceNavParams UVoxelSurfaceNavigationSubsystem::GetParams() const
{
	return Params;
}

FIntPoint UVoxelSurfaceNavigationSubsystem::CellOf(const FVector& World) const
{
	const float Size = FMath::Max(Params.CellSize, 1.f);
	return FIntPoint(FMath::FloorToInt(World.X / Size), FMath::FloorToInt(World.Y / Size));
}

FVector2D UVoxelSurfaceNavigationSubsystem::CellCentre(const FIntPoint& Cell) const
{
	return FVector2D((Cell.X + 0.5) * Params.CellSize, (Cell.Y + 0.5) * Params.CellSize);
}

static FORCEINLINE int32 FloorDiv(int32 A, int32 B)
{
	return (A >= 0) ? A / B : -((-A + B - 1) / B);
}

float UVoxelSurfaceNavigationSubsystem::SampleHeight(double WorldX, double WorldY)
{
	UVoxelChunkManager* ChunkMgr = ChunkManager.Get();
	if (!ChunkMgr)
	{
		return 0.f;
	}
	float Height = 0.f;
	FVector Normal;
	float Slope = 0.f;
	uint8 Material = 0, Biome = 0;
	if (ChunkMgr->QueryEditMergedSurface(WorldX, WorldY, Height, Normal, Slope, Material, Biome))
	{
		return Height;
	}
	return ChunkMgr->GetGeneratedSurfaceHeight(WorldX, WorldY);
}

float UVoxelSurfaceNavigationSubsystem::HeightForCell(const FIntPoint& Cell)
{
	const FIntPoint TileKey(FloorDiv(Cell.X, TileSize), FloorDiv(Cell.Y, TileSize));
	TUniquePtr<FNavTile>& Tile = Tiles.FindOrAdd(TileKey);
	if (!Tile)
	{
		Tile = MakeUnique<FNavTile>();
	}
	const int32 LX = Cell.X - TileKey.X * TileSize;
	const int32 LY = Cell.Y - TileKey.Y * TileSize;
	const int32 Index = LY * TileSize + LX;
	if (Tile->Sampled[Index])
	{
		return Tile->Heights[Index];
	}
	// Loaded or analytic? Ask the chunk manager which it used by probing the edit-merged query first.
	const FVector2D Centre = CellCentre(Cell);
	UVoxelChunkManager* ChunkMgr = ChunkManager.Get();
	float Height = 0.f;
	bool bLoaded = false;
	if (ChunkMgr)
	{
		FVector Normal;
		float Slope = 0.f;
		uint8 Material = 0, Biome = 0;
		bLoaded = ChunkMgr->QueryEditMergedSurface(Centre.X, Centre.Y, Height, Normal, Slope, Material, Biome);
		if (!bLoaded)
		{
			Height = ChunkMgr->GetGeneratedSurfaceHeight(Centre.X, Centre.Y);
		}
	}
	Tile->Heights[Index] = Height;
	Tile->Sampled[Index] = true;
	Tile->FromLoaded[Index] = bLoaded;
	++CachedCells;
	return Height;
}

bool UVoxelSurfaceNavigationSubsystem::IsCellBlocked(const FIntPoint& Cell) const
{
	if (TempBlockedCells.Contains(Cell))
	{
		return true;
	}
	// Outside the world is blocked, water on or off (WM-I2, KI9): past an island's R + falloff generation writes only
	// a flat floor, which with water off would otherwise read as walkable ground.
	if (const UVoxelChunkManager* ChunkMgr = ChunkManager.Get())
	{
		const FVector2D Centre = CellCentre(Cell);
		if (!ChunkMgr->IsInsideWorld(Centre.X, Centre.Y))
		{
			return true;
		}
	}
	if (BlockedBoxes.Num() > 0)
	{
		const FVector2D Centre = CellCentre(Cell);
		for (const FBlockedBox& Box : BlockedBoxes)
		{
			if (Box.Box.IsInside(Centre))
			{
				return true;
			}
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool UVoxelSurfaceNavigationSubsystem::FindPathWithStats(const FVector& From, const FVector& To, TArray<FVector>& OutPoints, FVoxelSurfacePathStats& OutStats)
{
	OutPoints.Reset();
	OutStats = FVoxelSurfacePathStats();
	if (!ResolveChunkManager() || !IsReady())
	{
		return false;
	}
	ApplyCVars();
	bool bOk = false;
	if (bPlanet)
	{
		// A planet: the search runs in the frame of the region its start lies in, altitudes for heights and the sea as
		// the altitude plane WaterRadius - PlanetRadius (exactly the sea sphere's depth along local up).
		FRegionCache& Region = RegionFor(From);
		FVector FrameFrom, FrameTo;
		if (Region.Frame.ToFrame(From, FrameFrom) && Region.Frame.ToFrame(To, FrameTo))
		{
			FVoxelSurfaceNavParams FrameParams = Params;
			FrameParams.bHasWater = false;
			FrameParams.Sea = FVoxelSeaSurface();
			if (PlanetSea.bEnabled && PlanetSea.bRadial)
			{
				FrameParams.Sea = FVoxelSeaSurface::MakePlane(PlanetSea.Radius - PlanetRadius);
			}
			FVoxelSurfacePathfinder Pathfinder(FrameParams,
				[this, &Region](double X, double Y) { return HeightForPlanetCell(Region, CellOf(FVector(X, Y, 0.0))); },
				[this, &Region](const FIntPoint& Cell) { return IsPlanetCellBlocked(Region, Cell); });
			TArray<FVector> FramePoints;
			bOk = Pathfinder.FindPath(FrameFrom, FrameTo, FramePoints, &OutStats);
			for (const FVector& FramePoint : FramePoints)
			{
				OutPoints.Add(Region.Frame.ToWorld(FramePoint));
			}
		}
	}
	else
	{
		FVoxelSurfacePathfinder Pathfinder(Params,
			[this](double X, double Y) { return HeightForCell(CellOf(FVector(X, Y, 0.0))); },
			[this](const FIntPoint& Cell) { return IsCellBlocked(Cell); });
		bOk = Pathfinder.FindPath(From, To, OutPoints, &OutStats);
	}
	LastStats = OutStats;
	++SearchCount;
	if (CVarNavDebug.GetValueOnGameThread() > 0)
	{
		if (bOk)
		{
			TArray<FVector> Drawn = OutPoints;
			Drawn.Insert(From, 0);
			DrawPath(Drawn, 6.f, OutStats.bPartial ? FLinearColor::Yellow : FLinearColor::Green);
		}
		UE_LOG(LogVoxelNavigation, Verbose, TEXT("FindPath %s -> %s: %s, %d nodes, %d samples, %d raw cells -> %d points%s"),
			*From.ToCompactString(), *To.ToCompactString(), bOk ? TEXT("ok") : TEXT("FAILED"), OutStats.NodesExpanded,
			OutStats.HeightSamples, OutStats.RawCells, OutPoints.Num(), OutStats.bPartial ? TEXT(" (partial)") : TEXT(""));
	}
	return bOk;
}

bool UVoxelSurfaceNavigationSubsystem::FindPath(const FVector& From, const FVector& To, TArray<FVector>& OutPoints)
{
	FVoxelSurfacePathStats Stats;
	return FindPathWithStats(From, To, OutPoints, Stats);
}

bool UVoxelSurfaceNavigationSubsystem::ProjectToSurface(const FVector& Location, FVector& OutOnSurface)
{
	if (!ResolveChunkManager() || !IsReady())
	{
		OutOnSurface = Location;
		return false;
	}
	ApplyCVars();
	if (bPlanet)
	{
		// Along local up: the edit-merged surface when loaded, the analytic one otherwise.
		UVoxelChunkManager* ChunkMgr = ChunkManager.Get();
		FVoxelSurfaceSample Sample;
		OutOnSurface = ChunkMgr->QueryEditMergedSurfacePoint(Location, Sample) ? Sample.Position : ChunkMgr->GetGeneratedSurfacePoint(Location);
		return true;
	}
	OutOnSurface = FVector(Location.X, Location.Y, SampleHeight(Location.X, Location.Y));
	return true;
}

bool UVoxelSurfaceNavigationSubsystem::IsWalkableAt(const FVector& Location)
{
	if (!ResolveChunkManager() || !IsReady())
	{
		return false;
	}
	ApplyCVars();
	if (bPlanet)
	{
		FRegionCache& Region = RegionFor(Location);
		FVector FramePoint;
		if (!Region.Frame.ToFrame(Location, FramePoint))
		{
			return false;
		}
		const FIntPoint PlanetCell = CellOf(FramePoint);
		if (IsPlanetCellBlocked(Region, PlanetCell))
		{
			return false;
		}
		const float Altitude = HeightForPlanetCell(Region, PlanetCell);
		return !(PlanetSea.bEnabled && PlanetSea.bRadial && PlanetRadius + Altitude < PlanetSea.Radius - Params.MaxWadeDepth);
	}
	const FIntPoint Cell = CellOf(Location);
	if (IsCellBlocked(Cell))
	{
		return false;
	}
	const float Height = HeightForCell(Cell);
	const FVector2D Centre = CellCentre(Cell);
	return !Params.IsUnderwater(Centre.X, Centre.Y, Height);
}

int32 UVoxelSurfaceNavigationSubsystem::AddBlockedBox(const FBox& WorldBox)
{
	FBlockedBox Entry;
	Entry.Handle = NextBlockHandle++;
	Entry.Box = FBox2D(FVector2D(WorldBox.Min.X, WorldBox.Min.Y), FVector2D(WorldBox.Max.X, WorldBox.Max.Y));
	Entry.Box3D = WorldBox;
	BlockedBoxes.Add(Entry);
	return Entry.Handle;
}

void UVoxelSurfaceNavigationSubsystem::RemoveBlockedBox(int32 Handle)
{
	BlockedBoxes.RemoveAll([Handle](const FBlockedBox& Box) { return Box.Handle == Handle; });
}

void UVoxelSurfaceNavigationSubsystem::BlockCellsTemporarily(const FVector& Location, float Radius, float Seconds)
{
	if (!GetWorld())
	{
		return;
	}
	ApplyCVars();
	const double Expire = GetWorld()->GetTimeSeconds() + FMath::Max(Seconds, 0.1f);
	if (bPlanet)
	{
		TempSpheres.Add(FTempSphere{ Location, Radius, Expire });
		return;
	}
	const FIntPoint Centre = CellOf(Location);
	const int32 R = FMath::Max(0, FMath::CeilToInt(Radius / Params.CellSize));
	for (int32 DX = -R; DX <= R; ++DX)
	{
		for (int32 DY = -R; DY <= R; ++DY)
		{
			const FIntPoint Cell(Centre.X + DX, Centre.Y + DY);
			if (FVector2D::Distance(CellCentre(Cell), FVector2D(Location.X, Location.Y)) <= Radius + Params.CellSize * 0.5f)
			{
				TempBlocks.Add(FTempBlock{Cell, Expire});
				TempBlockedCells.Add(Cell);
			}
		}
	}
}

void UVoxelSurfaceNavigationSubsystem::InvalidateAll()
{
	Tiles.Reset();
	Regions.Reset();
	CachedCells = 0;
}

void UVoxelSurfaceNavigationSubsystem::InvalidateCells(const FIntPoint& MinCell, const FIntPoint& MaxCell, bool bOnlyAnalytic)
{
	CachedCells -= InvalidateTileCells(Tiles, MinCell, MaxCell, bOnlyAnalytic);
}

int32 UVoxelSurfaceNavigationSubsystem::InvalidateTileCells(TMap<FIntPoint, TUniquePtr<FNavTile>>& InTiles, const FIntPoint& MinCell,
	const FIntPoint& MaxCell, bool bOnlyAnalytic)
{
	int32 Cleared = 0;
	const FIntPoint MinTile(FloorDiv(MinCell.X, TileSize), FloorDiv(MinCell.Y, TileSize));
	const FIntPoint MaxTile(FloorDiv(MaxCell.X, TileSize), FloorDiv(MaxCell.Y, TileSize));
	if (MaxTile.X < MinTile.X || MaxTile.Y < MinTile.Y)
	{
		return 0;
	}
	// The tiles to visit: the rectangle's, or the cached ones inside it when the rectangle holds more tiles than are cached
	// (a box projected across a planet's region frame spans millions of tiles; spherical Phase 6, P6.5).
	TArray<FIntPoint, TInlineAllocator<64>> Keys;
	const int64 RectTiles = (static_cast<int64>(MaxTile.X) - MinTile.X + 1) * (static_cast<int64>(MaxTile.Y) - MinTile.Y + 1);
	if (RectTiles > InTiles.Num())
	{
		for (const TPair<FIntPoint, TUniquePtr<FNavTile>>& Pair : InTiles)
		{
			if (Pair.Key.X >= MinTile.X && Pair.Key.X <= MaxTile.X && Pair.Key.Y >= MinTile.Y && Pair.Key.Y <= MaxTile.Y)
			{
				Keys.Add(Pair.Key);
			}
		}
	}
	else
	{
		for (int32 TX = MinTile.X; TX <= MaxTile.X; ++TX)
		{
			for (int32 TY = MinTile.Y; TY <= MaxTile.Y; ++TY)
			{
				Keys.Add(FIntPoint(TX, TY));
			}
		}
	}
	for (const FIntPoint& Key : Keys)
	{
		const int32 TX = Key.X, TY = Key.Y;
		TUniquePtr<FNavTile>* Tile = InTiles.Find(Key);
		if (!Tile || !*Tile)
		{
			continue;
		}
		const int32 X0 = FMath::Max(MinCell.X - TX * TileSize, 0);
		const int32 X1 = FMath::Min(MaxCell.X - TX * TileSize, TileSize - 1);
		const int32 Y0 = FMath::Max(MinCell.Y - TY * TileSize, 0);
		const int32 Y1 = FMath::Min(MaxCell.Y - TY * TileSize, TileSize - 1);
		for (int32 Y = Y0; Y <= Y1; ++Y)
		{
			for (int32 X = X0; X <= X1; ++X)
			{
				const int32 Index = Y * TileSize + X;
				if ((*Tile)->Sampled[Index] && (!bOnlyAnalytic || !(*Tile)->FromLoaded[Index]))
				{
					(*Tile)->Sampled[Index] = false;
					++Cleared;
				}
			}
		}
	}
	return Cleared;
}

void UVoxelSurfaceNavigationSubsystem::InvalidateBox(const FBox& WorldBox)
{
	if (Params.CellSize <= 0.f)
	{
		return;
	}
	if (bPlanet)
	{
		InvalidatePlanetBox(WorldBox, false);
		return;
	}
	InvalidateCells(CellOf(WorldBox.Min), CellOf(WorldBox.Max), false);
}

void UVoxelSurfaceNavigationSubsystem::HandleChunkLoaded(FIntVector ChunkCoord)
{
	const UVoxelChunkManager* ChunkMgr = ChunkManager.Get();
	const UVoxelWorldConfiguration* Config = ChunkMgr ? ChunkMgr->GetConfiguration() : nullptr;
	if (!Config || Params.CellSize <= 0.f)
	{
		return;
	}
	// Cells over this chunk that were estimated analytically now have real (edit-merged) data.
	const FBox Bounds = FVoxelCoordinates::ChunkToWorldBounds(ChunkCoord, Config->ChunkSize, Config->VoxelSize).ShiftBy(Config->WorldOrigin);
	if (bPlanet)
	{
		InvalidatePlanetBox(Bounds, true);
		return;
	}
	InvalidateCells(CellOf(Bounds.Min), CellOf(Bounds.Max), true);
}

void UVoxelSurfaceNavigationSubsystem::HandleChunkEdited(const FIntVector& ChunkCoord, EEditSource Source, const FVector& EditCenter, float EditRadius)
{
	if (Params.CellSize <= 0.f)
	{
		return;
	}
	// System edits (stamps, region edits, a load) give no centre (zero, radius 0): the edited chunk is the area. On a planet
	// the zero vector is its centre, and a box there projected into every cached region's frame spanned the planet: a
	// dungeon stamp on the 1,000 km test planet held the game thread for minutes (spherical Phase 6, P6.5).
	if (EditRadius <= 0.f && EditCenter.IsZero())
	{
		const UVoxelChunkManager* ChunkMgr = ChunkManager.Get();
		const UVoxelWorldConfiguration* Config = ChunkMgr ? ChunkMgr->GetConfiguration() : nullptr;
		if (!Config)
		{
			return;
		}
		const FBox Bounds = FVoxelCoordinates::ChunkToWorldBounds(ChunkCoord, Config->ChunkSize, Config->VoxelSize).ShiftBy(Config->WorldOrigin).ExpandBy(Params.CellSize);
		if (bPlanet)
		{
			InvalidatePlanetBox(Bounds, false);
			return;
		}
		InvalidateCells(CellOf(Bounds.Min), CellOf(Bounds.Max), false);
		return;
	}
	const float Pad = EditRadius + Params.CellSize;
	if (bPlanet)
	{
		InvalidatePlanetBox(FBox(EditCenter - FVector(Pad), EditCenter + FVector(Pad)), false);
		return;
	}
	InvalidateCells(CellOf(EditCenter - FVector(Pad)), CellOf(EditCenter + FVector(Pad)), false);
}

// ---------------------------------------------------------------------------
// Planets (spherical Phase 4, WM-SY11)
// ---------------------------------------------------------------------------

FVoxelPlanetNavFrame FVoxelPlanetNavFrame::Make(const FVector& InCenter, double InRadius, const FVector& Direction, const FIntPoint& InRegion)
{
	FVoxelPlanetNavFrame Frame;
	Frame.Region = InRegion;
	Frame.Center = InCenter;
	Frame.Radius = FMath::Max(InRadius, 1.0);
	Frame.Up = Direction.IsNearlyZero() ? FVector::UpVector : Direction.GetSafeNormal();
	FVoxelCubeSphere::GetTangentFrame(Frame.Up, Frame.East, Frame.North);
	return Frame;
}

bool FVoxelPlanetNavFrame::ToFrame(const FVector& World, FVector& OutFramePoint) const
{
	const FVector ToPoint = World - Center;
	const double Distance = ToPoint.Size();
	if (Distance < UE_KINDA_SMALL_NUMBER)
	{
		return false;
	}
	const FVector Direction = ToPoint / Distance;
	const double Along = FVector::DotProduct(Direction, Up);
	if (Along < 0.5)
	{
		return false;
	}
	// The direction's line meets the tangent plane at Radius / Along along it.
	const double Scale = Radius / Along;
	OutFramePoint = FVector(FVector::DotProduct(Direction, East) * Scale, FVector::DotProduct(Direction, North) * Scale, Distance - Radius);
	return true;
}

FVector FVoxelPlanetNavFrame::DirectionAt(double X, double Y) const
{
	return (Up * Radius + East * X + North * Y).GetSafeNormal();
}

FVector FVoxelPlanetNavFrame::ToWorld(const FVector& FramePoint) const
{
	return Center + DirectionAt(FramePoint.X, FramePoint.Y) * (Radius + FramePoint.Z);
}

UVoxelSurfaceNavigationSubsystem::FRegionCache& UVoxelSurfaceNavigationSubsystem::RegionFor(const FVector& World)
{
	const FIntPoint Region = RegionGrid.CellOfDirection((World - PlanetCenter).GetSafeNormal());
	TUniquePtr<FRegionCache>& Cache = Regions.FindOrAdd(Region);
	if (!Cache)
	{
		Cache = MakeUnique<FRegionCache>();
		Cache->Frame = FVoxelPlanetNavFrame::Make(PlanetCenter, PlanetRadius, RegionGrid.CellCentreDirection(Region), Region);
	}
	Cache->LastUsedSeconds = FPlatformTime::Seconds();
	return *Cache;
}

float UVoxelSurfaceNavigationSubsystem::HeightForPlanetCell(FRegionCache& Region, const FIntPoint& Cell)
{
	const FIntPoint TileKey(FloorDiv(Cell.X, TileSize), FloorDiv(Cell.Y, TileSize));
	TUniquePtr<FNavTile>& Tile = Region.Tiles.FindOrAdd(TileKey);
	if (!Tile)
	{
		Tile = MakeUnique<FNavTile>();
	}
	const int32 Index = (Cell.Y - TileKey.Y * TileSize) * TileSize + (Cell.X - TileKey.X * TileSize);
	if (Tile->Sampled[Index])
	{
		return Tile->Heights[Index];
	}
	// The surface along the cell centre's direction: edit-merged when its chunk is loaded, analytic otherwise.
	const FVector2D Centre = CellCentre(Cell);
	const FVector Probe = PlanetCenter + Region.Frame.DirectionAt(Centre.X, Centre.Y) * PlanetRadius;
	float Altitude = 0.f;
	bool bLoaded = false;
	if (UVoxelChunkManager* ChunkMgr = ChunkManager.Get())
	{
		FVoxelSurfaceSample Sample;
		bLoaded = ChunkMgr->QueryEditMergedSurfacePoint(Probe, Sample);
		Altitude = bLoaded ? Sample.Altitude : ChunkMgr->GetAltitudeAt(ChunkMgr->GetGeneratedSurfacePoint(Probe));
	}
	Tile->Heights[Index] = Altitude;
	Tile->Sampled[Index] = true;
	Tile->FromLoaded[Index] = bLoaded;
	++Region.Cells;
	++CachedCells;
	return Altitude;
}

bool UVoxelSurfaceNavigationSubsystem::IsPlanetCellBlocked(FRegionCache& Region, const FIntPoint& Cell)
{
	if (TempSpheres.Num() == 0 && BlockedBoxes.Num() == 0)
	{
		return false;
	}
	const FVector2D Centre = CellCentre(Cell);
	const FVector Ground = Region.Frame.ToWorld(FVector(Centre.X, Centre.Y, HeightForPlanetCell(Region, Cell)));
	for (const FTempSphere& Block : TempSpheres)
	{
		if (FVector::Dist(Ground, Block.Center) <= Block.Radius + Params.CellSize * 0.5f)
		{
			return true;
		}
	}
	for (const FBlockedBox& Box : BlockedBoxes)
	{
		if (Box.Box3D.IsInsideOrOn(Ground))
		{
			return true;
		}
	}
	return false;
}

void UVoxelSurfaceNavigationSubsystem::InvalidatePlanetBox(const FBox& WorldBox, bool bOnlyAnalytic)
{
	for (TPair<FIntPoint, TUniquePtr<FRegionCache>>& Pair : Regions)
	{
		FRegionCache* Region = Pair.Value.Get();
		if (!Region)
		{
			continue;
		}
		// The box's corners in the frame give a cell rectangle (its sides along up project inside it). Corners deep inside
		// the planet are no surface any frame shows (their directions fan out over the whole sphere).
		FBox2D Rect(ForceInit);
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector Point((Corner & 1) ? WorldBox.Max.X : WorldBox.Min.X, (Corner & 2) ? WorldBox.Max.Y : WorldBox.Min.Y,
				(Corner & 4) ? WorldBox.Max.Z : WorldBox.Min.Z);
			FVector FramePoint;
			if (FVector::DistSquared(Point, PlanetCenter) >= FMath::Square(0.5 * PlanetRadius) && Region->Frame.ToFrame(Point, FramePoint))
			{
				Rect += FVector2D(FramePoint.X, FramePoint.Y);
			}
		}
		if (!Rect.bIsValid)
		{
			continue;
		}
		const int32 Cleared = InvalidateTileCells(Region->Tiles, CellOf(FVector(Rect.Min, 0.0)), CellOf(FVector(Rect.Max, 0.0)), bOnlyAnalytic);
		Region->Cells -= Cleared;
		CachedCells -= Cleared;
	}
}

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------

void UVoxelSurfaceNavigationSubsystem::SetDebugFocus(const FVector& Location, float Radius)
{
	DebugFocus = Location;
	DebugFocusRadius = Radius;
}

void UVoxelSurfaceNavigationSubsystem::DrawPath(const TArray<FVector>& Points, float Seconds, FLinearColor Color)
{
	if (!GetWorld() || Points.Num() == 0)
	{
		return;
	}
	FDebugPath Path;
	Path.Points = Points;
	Path.ExpireTime = GetWorld()->GetTimeSeconds() + Seconds;
	Path.Color = Color;
	DebugPaths.Add(MoveTemp(Path));
}

void UVoxelSurfaceNavigationSubsystem::DrawDebug()
{
#if ENABLE_DRAW_DEBUG
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();
	for (int32 i = DebugPaths.Num() - 1; i >= 0; --i)
	{
		const FDebugPath& Path = DebugPaths[i];
		if (Path.ExpireTime <= Now)
		{
			DebugPaths.RemoveAtSwap(i);
			continue;
		}
		const FColor Color = Path.Color.ToFColor(true);
		// 40 uu above the ground along local up (radial on a planet).
		auto Lift = [this](const FVector& Point)
		{
			return Point + (bPlanet ? (Point - PlanetCenter).GetSafeNormal() : FVector::UpVector) * 40.0;
		};
		for (int32 p = 0; p < Path.Points.Num(); ++p)
		{
			const FVector P = Lift(Path.Points[p]);
			DrawDebugSphere(World, P, 20.f, 8, Color, false, 0.f, 0, 2.f);
			if (p > 0)
			{
				DrawDebugLine(World, Lift(Path.Points[p - 1]), P, Color, false, 0.f, 0, 4.f);
			}
		}
	}

	if (CVarNavDebug.GetValueOnGameThread() <= 0 || DebugFocusRadius <= 0.f || !ResolveChunkManager() || !IsReady())
	{
		return;
	}
	ApplyCVars();
	if (bPlanet)
	{
		// The region frame's cells around the focus, drawn on the ground in the frame's orientation.
		FRegionCache& Region = RegionFor(DebugFocus);
		FVector FocusPoint;
		if (!Region.Frame.ToFrame(DebugFocus, FocusPoint))
		{
			return;
		}
		const FIntPoint FocusCell = CellOf(FocusPoint);
		const int32 PlanetR = FMath::Clamp(FMath::CeilToInt(DebugFocusRadius / Params.CellSize), 1, 24);
		for (int32 DX = -PlanetR; DX <= PlanetR; ++DX)
		{
			for (int32 DY = -PlanetR; DY <= PlanetR; ++DY)
			{
				const FIntPoint Cell(FocusCell.X + DX, FocusCell.Y + DY);
				const float Altitude = HeightForPlanetCell(Region, Cell);
				const FVector2D C = CellCentre(Cell);
				const FVector Ground = Region.Frame.ToWorld(FVector(C.X, C.Y, Altitude));
				const FVector Up = (Ground - PlanetCenter).GetSafeNormal();
				FColor Color = FColor::Yellow;
				const FIntPoint TileKey(FloorDiv(Cell.X, TileSize), FloorDiv(Cell.Y, TileSize));
				if (const TUniquePtr<FNavTile>* Tile = Region.Tiles.Find(TileKey))
				{
					const int32 Index = (Cell.Y - TileKey.Y * TileSize) * TileSize + (Cell.X - TileKey.X * TileSize);
					Color = (*Tile && (*Tile)->FromLoaded[Index]) ? FColor::Green : FColor::Yellow;
				}
				if (IsPlanetCellBlocked(Region, Cell))
				{
					Color = FColor::Magenta;
				}
				else if (PlanetSea.bEnabled && PlanetSea.bRadial && PlanetRadius + Altitude < PlanetSea.Radius - Params.MaxWadeDepth)
				{
					Color = FColor::Red;
				}
				const FQuat Rotation = FRotationMatrix::MakeFromXZ(Region.Frame.East, Up).ToQuat();
				DrawDebugBox(World, Ground + Up * 10.0, FVector(Params.CellSize * 0.45f, Params.CellSize * 0.45f, 2.f), Rotation, Color, false, 0.f, 0, 1.f);
			}
		}
		return;
	}
	const FIntPoint Centre = CellOf(DebugFocus);
	const int32 R = FMath::Clamp(FMath::CeilToInt(DebugFocusRadius / Params.CellSize), 1, 24);
	for (int32 DX = -R; DX <= R; ++DX)
	{
		for (int32 DY = -R; DY <= R; ++DY)
		{
			const FIntPoint Cell(Centre.X + DX, Centre.Y + DY);
			const float H = HeightForCell(Cell);
			const FIntPoint TileKey(FloorDiv(Cell.X, TileSize), FloorDiv(Cell.Y, TileSize));
			const TUniquePtr<FNavTile>* Tile = Tiles.Find(TileKey);
			const int32 Index = (Cell.Y - TileKey.Y * TileSize) * TileSize + (Cell.X - TileKey.X * TileSize);
			const bool bLoaded = Tile && *Tile && (*Tile)->FromLoaded[Index];
			FColor Color = bLoaded ? FColor::Green : FColor::Yellow;
			if (IsCellBlocked(Cell)) // blocked boxes, repath blocks, outside the world
			{
				Color = FColor::Magenta;
			}
			else if (Params.IsUnderwater(CellCentre(Cell).X, CellCentre(Cell).Y, H))
			{
				Color = FColor::Red;
			}
			const FVector2D C = CellCentre(Cell);
			DrawDebugBox(World, FVector(C.X, C.Y, H + 10.f), FVector(Params.CellSize * 0.45f, Params.CellSize * 0.45f, 2.f), Color, false, 0.f, 0, 1.f);
		}
	}
#endif
}
