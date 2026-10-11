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
			Params.WaterLevel = Config->WaterLevel + Config->WorldOrigin.Z;
		}
	}
	if (CachedCellSize != Params.CellSize)
	{
		Tiles.Reset();
		CachedCells = 0;
		TempBlocks.Reset();
		TempBlockedCells.Reset();
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
	FVoxelSurfacePathfinder Pathfinder(Params,
		[this](double X, double Y) { return HeightForCell(CellOf(FVector(X, Y, 0.0))); },
		[this](const FIntPoint& Cell) { return IsCellBlocked(Cell); });
	const bool bOk = Pathfinder.FindPath(From, To, OutPoints, &OutStats);
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
	const FIntPoint Cell = CellOf(Location);
	if (IsCellBlocked(Cell))
	{
		return false;
	}
	const float Height = HeightForCell(Cell);
	return !(Params.bHasWater && Height < Params.WaterLevel - Params.MaxWadeDepth);
}

int32 UVoxelSurfaceNavigationSubsystem::AddBlockedBox(const FBox& WorldBox)
{
	FBlockedBox Entry;
	Entry.Handle = NextBlockHandle++;
	Entry.Box = FBox2D(FVector2D(WorldBox.Min.X, WorldBox.Min.Y), FVector2D(WorldBox.Max.X, WorldBox.Max.Y));
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
	CachedCells = 0;
}

void UVoxelSurfaceNavigationSubsystem::InvalidateCells(const FIntPoint& MinCell, const FIntPoint& MaxCell, bool bOnlyAnalytic)
{
	for (int32 TX = FloorDiv(MinCell.X, TileSize); TX <= FloorDiv(MaxCell.X, TileSize); ++TX)
	{
		for (int32 TY = FloorDiv(MinCell.Y, TileSize); TY <= FloorDiv(MaxCell.Y, TileSize); ++TY)
		{
			TUniquePtr<FNavTile>* Tile = Tiles.Find(FIntPoint(TX, TY));
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
						--CachedCells;
					}
				}
			}
		}
	}
}

void UVoxelSurfaceNavigationSubsystem::InvalidateBox(const FBox& WorldBox)
{
	if (Params.CellSize <= 0.f)
	{
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
	InvalidateCells(CellOf(Bounds.Min), CellOf(Bounds.Max), true);
}

void UVoxelSurfaceNavigationSubsystem::HandleChunkEdited(const FIntVector& ChunkCoord, EEditSource Source, const FVector& EditCenter, float EditRadius)
{
	if (Params.CellSize <= 0.f)
	{
		return;
	}
	const float Pad = EditRadius + Params.CellSize;
	InvalidateCells(CellOf(EditCenter - FVector(Pad)), CellOf(EditCenter + FVector(Pad)), false);
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
		for (int32 p = 0; p < Path.Points.Num(); ++p)
		{
			const FVector P = Path.Points[p] + FVector(0, 0, 40.f);
			DrawDebugSphere(World, P, 20.f, 8, Color, false, 0.f, 0, 2.f);
			if (p > 0)
			{
				DrawDebugLine(World, Path.Points[p - 1] + FVector(0, 0, 40.f), P, Color, false, 0.f, 0, 4.f);
			}
		}
	}

	if (CVarNavDebug.GetValueOnGameThread() <= 0 || DebugFocusRadius <= 0.f || !ResolveChunkManager() || !IsReady())
	{
		return;
	}
	ApplyCVars();
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
			if (Params.bHasWater && H < Params.WaterLevel - Params.MaxWadeDepth)
			{
				Color = FColor::Red;
			}
			else if (IsCellBlocked(Cell))
			{
				Color = FColor::Magenta;
			}
			const FVector2D C = CellCentre(Cell);
			DrawDebugBox(World, FVector(C.X, C.Y, H + 10.f), FVector(Params.CellSize * 0.45f, Params.CellSize * 0.45f, 2.f), Color, false, 0.f, 0, 1.f);
		}
	}
#endif
}
