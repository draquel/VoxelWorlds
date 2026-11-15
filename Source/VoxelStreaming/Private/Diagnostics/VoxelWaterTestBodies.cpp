// Copyright Daniel Raquel. All Rights Reserved.

// Lakes and river reaches laid by a script (voxel.Water.TestBodies): see VoxelWaterTestBodies.h.

#include "Diagnostics/VoxelWaterTestBodies.h"
#include "VoxelChunkManager.h"
#include "VoxelStreaming.h"
#include "VoxelConditioningLine.h"
#include "VoxelWaterLevels.h"
#include "VoxelWorldConfiguration.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"

#if !UE_BUILD_SHIPPING

// A text file of bodies, a line each:
//   lake id=<n> pit=<x>,<y>,<z> level=<level> radius=<uu> [cell=<uu>] [cave=<voxels>]
//       A lake flooded from a pit: the cells (cell uu wide, 4 voxels by default) within radius of the pit that the
//       water reaches from it, a cell being under the water where the generated ground at its centre lies under the
//       level; the footprint is those cells grown by one. No ground is moved.
//   reach id=<n> [speed=<uu/s>] [falloff=<uu>] [cave=<voxels>] [band=<material>] [banddepth=<voxels>]
//   v <x> <y> <z> <bed> <surface> <half>
//       A river reach, upstream to downstream, a "v" line a vertex after its "reach" line: where it is, its bed's
//       level, its surface's level and the bed's half width there (uu). Its bed is carved by a lower-only conditioning
//       line through the same vertices (the half width, the falloff, the band and the caves' depth), and the reach's
//       footprint is the bed's half width and the falloff.
// A position is a world position (on a planet its direction from the centre is what counts); a level is world Z on a
// heightmap world and an altitude over the datum on a planet. A line starting with # is a comment.
static TAutoConsoleVariable<FString> CVarWaterTestBodies(
	TEXT("voxel.Water.TestBodies"),
	TEXT(""),
	TEXT("A text file of lakes and river reaches a world adds at its start (development: the water shots lay their lake and ")
	TEXT("river with it until the hand-placed river and lake have actors). Empty: none. Read when a voxel world starts."),
	ECVF_Default);

namespace VoxelWaterTestBodiesPrivate
{
	/** The value of key=value among a line's words, or the default. */
	FString Value(const TArray<FString>& Words, const TCHAR* Key, const FString& Default = FString())
	{
		const FString Prefix = FString(Key) + TEXT("=");
		for (const FString& Word : Words)
		{
			if (Word.StartsWith(Prefix))
			{
				return Word.Mid(Prefix.Len());
			}
		}
		return Default;
	}

	double Number(const TArray<FString>& Words, const TCHAR* Key, double Default)
	{
		const FString Text = Value(Words, Key);
		return Text.IsEmpty() ? Default : FCString::Atod(*Text);
	}

	FVector Position(const FString& Text)
	{
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","));
		return FVector(Parts.IsValidIndex(0) ? FCString::Atod(*Parts[0]) : 0.0, Parts.IsValidIndex(1) ? FCString::Atod(*Parts[1]) : 0.0,
			Parts.IsValidIndex(2) ? FCString::Atod(*Parts[2]) : 0.0);
	}

	/** A reach being read: its line's words and its vertices so far. */
	struct FReach
	{
		TArray<FString> Words;
		TArray<FVector> Positions;
		TArray<double> Bed, Surface, Half;
	};

	/** Add a lake flooded from a pit. */
	void AddLake(UVoxelChunkManager& Manager, const TArray<FString>& Words)
	{
		const UVoxelWorldConfiguration* Config = Manager.GetConfiguration();
		const FVoxelWaterFrame Frame = Manager.GetWaterLevels().GetFrame();
		const uint32 Id = static_cast<uint32>(Number(Words, TEXT("id"), 9001.0));
		const FVector Pit = Position(Value(Words, TEXT("pit")));
		const double Radius = FMath::Max(Number(Words, TEXT("radius"), 10000.0), 1.0);
		const double Cell = FMath::Max(Number(Words, TEXT("cell"), 4.0 * Config->VoxelSize), 1.0);
		// The level in the frame's own measure: world Z, or the distance from the planet centre.
		const double Level = Number(Words, TEXT("level"), 0.0) + (Frame.bRadial ? Frame.DatumRadius : 0.0);
		const int32 Half = FMath::Clamp(FMath::CeilToInt32(Radius / Cell) + 1, 2, 400);
		const FIntPoint Size(2 * Half, 2 * Half);

		// The grid: around the pit, in world XY, or on the plane tangent to the datum sphere at the pit's direction.
		const FVector Up = Frame.UpAt(Pit);
		const TSharedRef<const FVoxelWaterLake> Whole = Frame.bRadial
			? FVoxelWaterLake::MakeOnSphere(Up, Frame.DatumRadius, Cell, Size, TBitArray<>(true, Size.X * Size.Y))
			: FVoxelWaterLake::Make(FVector2D(Pit.X - Half * Cell, Pit.Y - Half * Cell), Cell, Size, TBitArray<>(true, Size.X * Size.Y));
		// A cell is under the water where the generated ground at its centre lies under the level.
		const auto IsUnder = [&](int32 X, int32 Y)
		{
			const FVector Centre = Whole->GetCellCentre(X, Y, Level, Frame.Center);
			return Frame.HeightOf(Manager.GetGeneratedSurfacePoint(Centre)) < Level;
		};
		// The flood from the pit's cell, within the radius.
		TBitArray<> Wet(false, Size.X * Size.Y);
		TArray<FIntPoint> Queue;
		const FIntPoint Start(Half, Half);
		if (IsUnder(Start.X, Start.Y))
		{
			Wet[Start.Y * Size.X + Start.X] = true;
			Queue.Add(Start);
		}
		static const FIntPoint Steps[4] = { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) };
		for (int32 Head = 0; Head < Queue.Num(); ++Head)
		{
			for (const FIntPoint& Step : Steps)
			{
				const FIntPoint Next = Queue[Head] + Step;
				if (Next.X < 1 || Next.Y < 1 || Next.X >= Size.X - 1 || Next.Y >= Size.Y - 1 || Wet[Next.Y * Size.X + Next.X]
					|| FMath::Square((Next.X - Half + 0.5) * Cell) + FMath::Square((Next.Y - Half + 0.5) * Cell) > Radius * Radius
					|| !IsUnder(Next.X, Next.Y))
				{
					continue;
				}
				Wet[Next.Y * Size.X + Next.X] = true;
				Queue.Add(Next);
			}
		}
		// Grown by one cell, so the footprint reaches past the water's own columns.
		TBitArray<> Grown(false, Size.X * Size.Y);
		for (const FIntPoint& At : Queue)
		{
			for (int32 DY = -1; DY <= 1; ++DY)
			{
				for (int32 DX = -1; DX <= 1; ++DX)
				{
					Grown[(At.Y + DY) * Size.X + At.X + DX] = true;
				}
			}
		}
		if (Queue.Num() == 0)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.TestBodies: lake %u holds no water: the ground at its pit (%s) is not under its level %.0f."),
				Id, *Pit.ToCompactString(), Level);
			return;
		}
		const TSharedRef<const FVoxelWaterLake> Footprint = Frame.bRadial
			? FVoxelWaterLake::MakeOnSphere(Up, Frame.DatumRadius, Cell, Size, MoveTemp(Grown))
			: FVoxelWaterLake::Make(FVector2D(Pit.X - Half * Cell, Pit.Y - Half * Cell), Cell, Size, MoveTemp(Grown));
		UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.Water.TestBodies: lake %u flooded from its pit: %d cells of %.0f uu under level %.0f (%d in its footprint)."),
			Id, Queue.Num(), Cell, Level, Footprint->NumCells());
		Manager.AddWaterBody(FVoxelWaterBody::MakeLake(Id, Level, Footprint, Frame, 8.0 * Config->GetChunkWorldSize(),
			static_cast<float>(Number(Words, TEXT("cave"), 0.0))));
	}

	/** Add a river reach and its bed. */
	void AddReach(UVoxelChunkManager& Manager, const FReach& In)
	{
		if (In.Positions.Num() < 2)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.TestBodies: a river reach needs two vertices or more."));
			return;
		}
		const FVoxelWaterFrame Frame = Manager.GetWaterLevels().GetFrame();
		const uint32 Id = static_cast<uint32>(Number(In.Words, TEXT("id"), 9002.0));
		const float Falloff = static_cast<float>(Number(In.Words, TEXT("falloff"), 300.0));
		const double LevelBase = Frame.bRadial ? Frame.DatumRadius : 0.0;

		// The bed: a lower-only line through the same vertices (a line's target is a height, an altitude on a planet).
		FVoxelConditioningLineParams Bed;
		Bed.bRadial = Frame.bRadial;
		Bed.Blend = EVoxelConditioningLineBlend::LowerOnly;
		Bed.FalloffWidth = Falloff;
		Bed.Id = static_cast<int64>(Id);
		Bed.Surface.BandMaterialID = static_cast<int32>(Number(In.Words, TEXT("band"), -1.0));
		Bed.Surface.BandDepth = static_cast<float>(Number(In.Words, TEXT("banddepth"), 4.0));
		Bed.Surface.MinCaveDepth = static_cast<float>(Number(In.Words, TEXT("cave"), 8.0));
		TArray<FVoxelWaterReachVertex> Vertices;
		for (int32 Index = 0; Index < In.Positions.Num(); ++Index)
		{
			const FVector Place = Frame.bRadial ? Frame.UpAt(In.Positions[Index]) : FVector(In.Positions[Index].X, In.Positions[Index].Y, 0.0);
			// A line's half width must be over nothing; the reach's own tapers to the falloff alone at its head.
			Bed.Vertices.Add(FVoxelConditioningLineVertex(Place, static_cast<float>(In.Bed[Index]), static_cast<float>(FMath::Max(In.Half[Index], 1.0))));
			Vertices.Add(FVoxelWaterReachVertex(Place, In.Bed[Index] + LevelBase, In.Surface[Index] + LevelBase, FMath::Max(In.Half[Index], 0.0) + Falloff));
		}
		Manager.AddConditioningLine(Bed);
		Manager.AddWaterBody(FVoxelWaterBody::MakeReach(Id, FVoxelWaterReach::Make(MoveTemp(Vertices), Frame.bRadial,
			static_cast<float>(Number(In.Words, TEXT("speed"), 0.0))), Frame));
	}
}

void VoxelWaterTestBodies::Apply(UVoxelChunkManager& Manager)
{
	using namespace VoxelWaterTestBodiesPrivate;
	const FString Path = CVarWaterTestBodies.GetValueOnGameThread();
	if (Path.IsEmpty() || !Manager.GetConfiguration())
	{
		return;
	}
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *Path))
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.TestBodies: could not read %s."), *Path);
		return;
	}
	int32 Lakes = 0, Reaches = 0;
	FReach Reach;
	bool bInReach = false;
	const auto EndReach = [&]()
	{
		if (bInReach)
		{
			AddReach(Manager, Reach);
			++Reaches;
		}
		bInReach = false;
		Reach = FReach();
	};
	for (const FString& Line : Lines)
	{
		TArray<FString> Words;
		Line.TrimStartAndEnd().ParseIntoArrayWS(Words);
		if (Words.Num() == 0 || Words[0].StartsWith(TEXT("#")))
		{
			continue;
		}
		if (Words[0] == TEXT("lake"))
		{
			EndReach();
			AddLake(Manager, Words);
			++Lakes;
		}
		else if (Words[0] == TEXT("reach"))
		{
			EndReach();
			bInReach = true;
			Reach.Words = Words;
		}
		else if (Words[0] == TEXT("v") && bInReach && Words.Num() >= 7)
		{
			Reach.Positions.Add(FVector(FCString::Atod(*Words[1]), FCString::Atod(*Words[2]), FCString::Atod(*Words[3])));
			Reach.Bed.Add(FCString::Atod(*Words[4]));
			Reach.Surface.Add(FCString::Atod(*Words[5]));
			Reach.Half.Add(FCString::Atod(*Words[6]));
		}
	}
	EndReach();
	UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.Water.TestBodies: %d lake(s) and %d river reach(es) from %s; the world holds %d water bodies."),
		Lakes, Reaches, *Path, Manager.GetNumWaterBodies());
}

#else

void VoxelWaterTestBodies::Apply(UVoxelChunkManager&)
{
}

#endif // !UE_BUILD_SHIPPING
