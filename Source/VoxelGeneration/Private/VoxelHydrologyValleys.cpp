// Copyright Daniel Raquel. All Rights Reserved.

// The valley rivers (Documentation/VALLEYS_PLAN.md, section 6.4; D179): a river reach a valley link, made from the valley
// network's curve and floors, so a valley's river has no length bound and is never refused. On a planet (the plan's V5)
// the course is worked on the datum sphere, every point brought back onto it; on a block world the bed steps a block a
// column at most, as a brook's does.

#include "VoxelHydrology.h"
#include "VoxelHash.h"
#include "VoxelValleyNetwork.h"

uint32 FVoxelHydrology::MakeValleyReachId(int32 Seed, const FIntPoint& Link)
{
	const uint32 Id = VoxelHash::Lattice(static_cast<uint32>(Link.X), static_cast<uint32>(Link.Y), 0x56414C59u, Seed);
	return Id != FVoxelWaterBody::SeaId ? Id : 1u;
}

int64 FVoxelHydrology::MakeValleyBedLineId(const FIntPoint& Link)
{
	// 21 bits a coordinate (valley cells to a million either way of the origin), counted up from -2^59: over every
	// brook's bed (from -2^60, MakeBedLineId), so a valley river's bed is applied after the brooks' beds, and under every
	// hand-placed line's (of order -2^27 or more).
	const auto Bits = [](int32 Coordinate) { return static_cast<int64>((Coordinate + (1 << 20)) & 0x1FFFFF); };
	return -(static_cast<int64>(1) << 59) + ((Bits(Link.X) << 21) | Bits(Link.Y));
}

TSharedRef<const FVoxelHydrologySystem> FVoxelHydrology::BuildValleyRiver(const FVoxelHydrologySettings& InSettings, const FVoxelHydrologyGround& Ground, const FIntPoint& Link)
{
	TSharedRef<FVoxelHydrologySystem> System = MakeShared<FVoxelHydrologySystem>();
	System->Mouth = Link;
	System->bValleyRiver = true;
	if (!Ground.Valleys.IsValid() || Ground.Valleys->IsRadial() != Ground.bRadial || !Ground.IsValid())
	{
		System->Refused = TEXT("no valleys");
		return System;
	}
	// A network point (world (X, Y, 0), or on the datum sphere) as a place (the column, or the direction), and a point
	// brought back onto the datum sphere on a planet (a mean of points on a sphere lies under it)
	const bool bRadial = Ground.bRadial;
	const double SphereRadius = bRadial ? Ground.Valleys->GetLattice().GetRadius() : 0.0;
	const auto PlaceOf = [bRadial](const FVector& Point) { return bRadial ? Point.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector) : Point; };
	const auto OntoSphere = [SphereRadius](TArray<FVector>& Points)
	{
		if (SphereRadius > 0.0)
		{
			for (FVector& Point : Points)
			{
				Point = Point.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector) * SphereRadius;
			}
		}
	};
	const auto HeightAt = [&Ground, &PlaceOf](const FVector& Point) { return Ground.HeightAt(PlaceOf(Point)); };
	const FVoxelHydrologySettings Settings = Sanitize(InSettings, Ground);
	const FVoxelValleyNetwork& Valleys = *Ground.Valleys;
	const FVoxelValleySettings& ValleySettings = Valleys.GetSettings();
	const FVoxelValleyNode Node = Valleys.GetNode(Link);
	if (!Node.bValley)
	{
		System->Refused = TEXT("no valley on the link");
		return System;
	}
	const FVoxelValleyNode Down = Valleys.GetNode(Node.Down);
	System->NumSprings = Node.Drainage;

	// The link's curve: its segments, upper node to lower
	TArray<FVoxelValleySegment> Segments;
	for (const FVoxelValleySegment& Segment : Valleys.GetTile(Valleys.TileOf(Link))->Segments)
	{
		if (Segment.Link == Link)
		{
			Segments.Add(Segment);
		}
	}
	if (Segments.Num() == 0)
	{
		System->Refused = TEXT("a valley link with no curve");
		return System;
	}

	const double Voxel = FMath::Max(Ground.VoxelSize, 1.0);
	const double Bank = Settings.BankWidthVoxels * Voxel;
	const double Under = Settings.SurfaceBelowGroundVoxels * Voxel;
	const double Depth = Settings.DepthVoxels * Voxel;
	const double Tuck = FVoxelConditioningLineShape::FRiverProfileParams().BodyTuck;
	const double RiverHalf = Segments[0].RiverHalfWidth;
	const double FloorHalf = Segments[0].FloorHalfWidth;
	// The room the river winds over: its banks stay on the floor
	const double Room = ValleySettings.RiverWinding * FMath::Max(0.0, FloorHalf - RiverHalf - Bank);
	// A head's floor is whole from half the link on (the valley's taper): its river starts there
	const double StartT = Node.bHead ? 0.5 : 0.0;

	// The centre curve's points, a vertex every two voxels, with where each lies along the link and the floor there
	struct FPoint
	{
		FVector Position;
		double T;
		double Floor;
	};
	TArray<FPoint> Points;
	const double Step = 2.0 * Voxel;
	for (const FVoxelValleySegment& Segment : Segments)
	{
		if (Segment.LinkB <= StartT)
		{
			continue;
		}
		const int32 Steps = FMath::Max(1, FMath::CeilToInt32(FVector::Distance(Segment.A, Segment.B) / Step));
		for (int32 Q = Points.Num() == 0 ? 0 : 1; Q <= Steps; ++Q)
		{
			const double U = static_cast<double>(Q) / Steps;
			const double T = FMath::Lerp(static_cast<double>(Segment.LinkA), static_cast<double>(Segment.LinkB), U);
			if (T < StartT)
			{
				continue;
			}
			FVector At = FMath::Lerp(Segment.A, Segment.B, U);
			if (bRadial)
			{
				At = At.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector) * SphereRadius;
			}
			Points.Add({ At, T, FMath::Lerp(Segment.FloorA, Segment.FloorB, U) });
		}
	}
	if (Points.Num() < 2)
	{
		System->Refused = TEXT("a valley river of one place");
		return System;
	}

	// A polyline of the points rounded (FVoxelConditioningLineShape::RoundCorners, the two ends kept): where each of its
	// points lies along the link, and the floor there, read back by the share of the length it is at
	const auto Reround = [&Points](const TArray<FVector>& From, const TArray<FVector>& Rounded)
	{
		TArray<double> FromAlong;
		FromAlong.SetNumUninitialized(From.Num());
		FromAlong[0] = 0.0;
		for (int32 Index = 1; Index < From.Num(); ++Index)
		{
			FromAlong[Index] = FromAlong[Index - 1] + FVector::Dist(From[Index - 1], From[Index]);
		}
		const double RoundLength = FMath::Max(FVoxelConditioningLineShape::GetLength(Rounded), UE_DOUBLE_SMALL_NUMBER);
		TArray<FPoint> Out;
		Out.SetNum(Rounded.Num());
		double Arc = 0.0;
		int32 Seg = 0;
		for (int32 Index = 0; Index < Rounded.Num(); ++Index)
		{
			if (Index > 0)
			{
				Arc += FVector::Dist(Rounded[Index - 1], Rounded[Index]);
			}
			const double At = FromAlong.Last() * FMath::Clamp(Arc / RoundLength, 0.0, 1.0);
			while (Seg + 2 < From.Num() && FromAlong[Seg + 1] < At)
			{
				++Seg;
			}
			const double Span = FromAlong[Seg + 1] - FromAlong[Seg];
			const double U = Span > UE_DOUBLE_SMALL_NUMBER ? FMath::Clamp((At - FromAlong[Seg]) / Span, 0.0, 1.0) : 0.0;
			Out[Index] = { Rounded[Index], FMath::Lerp(Points[Seg].T, Points[Seg + 1].T, U), FMath::Lerp(Points[Seg].Floor, Points[Seg + 1].Floor, U) };
		}
		Points = MoveTemp(Out);
	};
	const double RiverReach = RiverHalf + Bank;

	// The valley's centre curve rounded first, to a radius the winding and the river's reach fit inside: the network's
	// curve is a chain of chords, and a winding offset across a sharp corner folds back on itself on the corner's inside.
	// A node's corner is split between the piece in and the piece out, and rounding keeps a line's two ends where they
	// are, so each end leaves its node along a straight chord to the curve some way in, a corner rounding can reach. The
	// piece still ends at the node: its footprint and its bed are round there, and cover the corner's outside
	{
		TArray<FVector> Centre;
		Centre.Reserve(Points.Num());
		for (const FPoint& Point : Points)
		{
			Centre.Add(Point.Position);
		}
		{
			const double Length = FVoxelConditioningLineShape::GetLength(Centre);
			const double Cut = FMath::Min(2.0 * (Room + RiverReach), 0.3 * Length);
			TArray<double> Along;
			Along.SetNumUninitialized(Centre.Num());
			Along[0] = 0.0;
			for (int32 Index = 1; Index < Centre.Num(); ++Index)
			{
				Along[Index] = Along[Index - 1] + FVector::Dist(Centre[Index - 1], Centre[Index]);
			}
			TArray<FVector> Chorded;
			TArray<FPoint> Kept;
			for (int32 Index = 0; Index < Centre.Num(); ++Index)
			{
				const bool bEnd = Index == 0 || Index == Centre.Num() - 1;
				if (bEnd || (Along[Index] >= Cut && Along[Index] <= Length - Cut))
				{
					Chorded.Add(Centre[Index]);
					Kept.Add(Points[Index]);
				}
			}
			if (Chorded.Num() >= 2)
			{
				Centre = MoveTemp(Chorded);
				Points = MoveTemp(Kept);
			}
		}
		TArray<FVector> Smooth;
		FVoxelConditioningLineShape::RoundCorners(Centre, Step, FVoxelConditioningLineShape::GetMaxTurnForReach(Step, Room + RiverReach), 4.0 * Step, Smooth, 8);
		OntoSphere(Smooth);
		if (Smooth.Num() < 2)
		{
			System->Refused = TEXT("a valley river of one place");
			return System;
		}
		Reround(Centre, Smooth);
	}

	// The course: the curve with the winding across the floor, nothing at the two nodes, its corners rounded to the bend
	// bound's turn as a brook's are (the two ends stay at the nodes, so the pieces meet). Where the winding still folds
	// on a tight bend of the valley and the rounded course breaks the bound, the piece winds again over half the room,
	// down to none: a valley is never left dry
	const TArray<FPoint> Centre = Points;
	TArray<FVector> Wound;
	TArray<FVector> Places;
	for (int32 Try = 0; Try < 4; ++Try)
	{
		const double Winding = Try < 3 ? Room / static_cast<double>(1 << Try) : 0.0;
		Wound.SetNum(Centre.Num());
		for (int32 Index = 0; Index < Centre.Num(); ++Index)
		{
			const FVector Along = (Centre[FMath::Min(Index + 1, Centre.Num() - 1)].Position - Centre[FMath::Max(Index - 1, 0)].Position).GetSafeNormal();
			const double Offset = Winding * FMath::Sin(UE_DOUBLE_PI * ValleySettings.RiverHalfWaves * Centre[Index].T);
			// Across the course: on a planet on the plane tangent at the point (up x along), as (-along.y, along.x) is
			// on a heightmap world
			const FVector Side = bRadial
				? FVector::CrossProduct(Centre[Index].Position.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector), Along).GetSafeNormal()
				: FVector(-Along.Y, Along.X, 0.0);
			Wound[Index] = Centre[Index].Position + Side * Offset;
		}
		OntoSphere(Wound);
		const FVoxelConditioningLineShape::FRounding Rounding = FVoxelConditioningLineShape::RoundCorners(
			Wound, Step, FVoxelConditioningLineShape::GetMaxTurnForReach(Step, RiverReach), 4.0 * Step, Places, 8);
		OntoSphere(Places);
		if (Rounding.bWithinBound)
		{
			break;
		}
	}
	if (Places.Num() < 2)
	{
		System->Refused = TEXT("a valley river of one place");
		return System;
	}
	Points = Centre;
	Reround(Wound, Places);
	// A piece into a coast keeps only the few vertices past the shore that a brook keeps inside the body it ends in
	// (FRiverProfileParams::BodyVertices): its course runs on over the seabed to the node, and a bed there would stand
	// far over the ground, past the cut and fill cap the LOD slab assumes
	bool bStartsInSea = false;
	if (Down.bSea && Ground.bSea)
	{
		const int32 Kept = FVoxelConditioningLineShape::FRiverProfileParams().BodyVertices;
		for (int32 Index = 1; Index < Places.Num(); ++Index)
		{
			if (HeightAt(Places[Index]) < Ground.SeaLevel)
			{
				// A head with no more than two places on land is no river: a valley of one link at a coast, whose head
				// starts half way along where the drowned mouth has already put the ground under the sea. A piece that
				// another comes into is kept however short, so the piece above ends in its water (G268: refused, it left
				// the piece above ending on land)
				if (Index <= 2 && Node.bHead)
				{
					System->Refused = TEXT("a valley river that starts in the sea");
					return System;
				}
				bStartsInSea = Index <= 2;
				// It ends under the sea: a seabed that rises over the water again within the kept vertices (a bump past the
				// shore) ends it at the last of them still under it, never on land (the block world of the plan's V5)
				int32 Last = FMath::Min(Index + Kept - 1, Places.Num() - 1);
				while (Last > Index && HeightAt(Places[Last]) >= Ground.SeaLevel)
				{
					--Last;
				}
				Places.SetNum(Last + 1);
				Points.SetNum(Last + 1);
				break;
			}
		}
	}
	const int32 Num = Places.Num();
	if (Num < 2)
	{
		System->Refused = TEXT("a valley river of one place");
		return System;
	}

	// A node's level, the same whichever piece asks: under its floor or its ground there, whichever is lower, held at a
	// tuck under the sea at least, and no higher than the level of any valley link into it (a link joins one of its
	// eight neighbours), so it never stands over a node upstream of it. A piece starts at its upper node's level and ends
	// at its lower node's, so the pieces meet at each node at one level and the water never rises from one to the next.
	// Taken from the node's own ground alone, a node a neighbouring valley's drowned mouth had cut low left the piece
	// from it under the next (the demo, V4)
	const double MinLevel = Ground.bSea ? Ground.SeaLevel - Tuck : -TNumericLimits<double>::Max();
	TMap<FIntPoint, double> Levels;
	TFunction<double(const FIntPoint&, int32)> LevelOf;
	LevelOf = [&](const FIntPoint& Cell, int32 Depth) -> double
	{
		if (const double* Known = Levels.Find(Cell))
		{
			return *Known;
		}
		const FVoxelValleyNode At = Valleys.GetNode(Cell);
		double Level = FMath::Min(At.Floor, HeightAt(At.Position)) - Under;
		if (Depth < 4096)
		{
			for (int32 Neighbour = 0; Neighbour < 8; ++Neighbour)
			{
				FIntPoint InCell;
				if (!Valleys.GetNeighbour(Cell, Neighbour, InCell))
				{
					continue;
				}
				const FVoxelValleyNode In = Valleys.GetNode(InCell);
				if (In.bValley && In.bHasLink && In.Down == Cell)
				{
					Level = FMath::Min(Level, LevelOf(In.Cell, Depth + 1));
				}
			}
		}
		Level = FMath::Max(Level, MinLevel);
		Levels.Add(Cell, Level);
		return Level;
	};

	// The surface: under the floor or the ground there, whichever is lower, never rising, from the upper node's level
	// (a head's own first place) to the lower node's, down to a tuck under the sea at a coast
	TArray<double> Own;
	Own.SetNum(Num);
	double DeepestCut = 0.0;
	TArray<double> GroundAt;
	GroundAt.SetNum(Num);
	for (int32 Index = 0; Index < Num; ++Index)
	{
		GroundAt[Index] = HeightAt(Places[Index]);
		Own[Index] = FMath::Min(Points[Index].Floor, GroundAt[Index]) - Under;
		// Never below a tuck under the sea: a piece near a coast ends in the sea at the sea's level, and every piece at a
		// node computes the same value there, so the pieces still meet
		if (Ground.bSea)
		{
			Own[Index] = FMath::Max(Own[Index], Ground.SeaLevel - Tuck);
		}
	}
	const bool bToSea = Down.bSea;
	double EndLevel = bToSea ? (Ground.bSea ? Ground.SeaLevel - Tuck : Own[Num - 1]) : LevelOf(Down.Cell, 0);
	// A node one link from a coast whose own piece starts in the sea (a short piece kept for the pieces coming into it,
	// at the sea's level from its start): the piece into it ends at that piece's first level, not at the node's
	if (!bToSea && Ground.bSea && Down.bValley && Down.bHasLink && Valleys.GetNode(Down.Down).bSea)
	{
		const TSharedRef<const FVoxelHydrologySystem> Below = BuildValleyRiver(InSettings, Ground, Down.Cell);
		if (Below->Reaches.Num() == 1)
		{
			EndLevel = FMath::Min(EndLevel, Below->Reaches[0].Body.Reach->GetVertices()[0].SurfaceLevel - Ground.GetLevelBase());
		}
	}
	// A head starts at its own first place, half way along its link, and no lower than the node it runs to (whose
	// level is no higher than any node upstream, the head's own included). A piece that starts in the sea lies at the
	// sea's level from its start (G268: from its node's level it dropped to the sea in a step over the sea's water)
	const double StartLevel = bStartsInSea ? EndLevel : FMath::Max(Node.bHead ? Own[0] : LevelOf(Node.Cell, 0), EndLevel);
	TArray<double> Surface;
	Surface.SetNum(Num);
	Surface[0] = StartLevel;
	for (int32 Index = 1; Index < Num; ++Index)
	{
		Surface[Index] = FMath::Max(EndLevel, FMath::Min(Surface[Index - 1], Own[Index]));
	}
	Surface[Num - 1] = EndLevel;

	// A head starts from nothing: its width and depth grow over the head's length
	TArray<double> Ramp;
	Ramp.SetNum(Num);
	double Arc = 0.0;
	for (int32 Index = 0; Index < Num; ++Index)
	{
		if (Index > 0)
		{
			Arc += FVector::Dist(Places[Index - 1], Places[Index]);
		}
		Ramp[Index] = Node.bHead ? FMath::Clamp(Arc / FMath::Max(Settings.HeadLength, 1.0f), 0.0, 1.0) : 1.0;
	}

	FVoxelHydrologyReach Reach;
	Reach.StartCell = Link;
	Reach.Flow = Node.Drainage;
	Reach.bEndsAtJoin = !bToSea;
	FVoxelConditioningLineParams& Bed = Reach.BedParams;
	Bed.FalloffWidth = static_cast<float>(Bank);
	Bed.Strength = 1.0f;
	Bed.Blend = EVoxelConditioningLineBlend::LowerOnly;
	Bed.Id = MakeValleyBedLineId(Link);
	Bed.bRadial = bRadial;
	Bed.Surface.BandMaterialID = Settings.BedMaterialID;
	Bed.Surface.MinCaveDepth = Settings.BedMinCaveDepth;
	Bed.Surface.ClearHalfWidth = static_cast<float>(RiverHalf + 0.5 * Bank);
	TArray<FVoxelWaterReachVertex> Vertices;
	Vertices.Reserve(Num);
	FVoxelConditioningLineShape::FRiverProfile& Profile = Reach.Profile;
	Profile.First = 0;
	Profile.Num = Num;
	const double LevelBase = Ground.GetLevelBase();
	for (int32 Index = 0; Index < Num; ++Index)
	{
		const double Half = RiverHalf * Ramp[Index];
		const double BedLevel = Surface[Index] - Depth * Ramp[Index];
		DeepestCut = FMath::Max(DeepestCut, GroundAt[Index] - BedLevel);
		// A vertex's position is its place: the column, or on a planet the direction
		const FVector Place = PlaceOf(Places[Index]);
		Bed.Vertices.Add(FVoxelConditioningLineVertex(Place, static_cast<float>(BedLevel), static_cast<float>(FMath::Max(Half, 1.0))));
		Vertices.Add(FVoxelWaterReachVertex(Place, BedLevel + LevelBase, Surface[Index] + LevelBase, Half + Bank));
		Profile.Surface.Add(Surface[Index]);
		Profile.Bed.Add(BedLevel);
		Profile.Taper.Add(Ramp[Index]);
		if (Index > 0)
		{
			Reach.Length += FVector::Dist(Places[Index - 1], Places[Index]);
		}
	}
	// The bed's grade through a bend held to the bend bound, as a brook's is (rule 8)
	{
		FVoxelConditioningLineShape::FRiverProfileParams BendParams;
		BendParams.Depth = Depth;
		BendParams.HalfWidth = static_cast<float>(RiverHalf);
		BendParams.FalloffWidth = static_cast<float>(Bank);
		BendParams.VoxelSize = Voxel;
		FVoxelConditioningLineShape::HoldBedThroughBends(Places, Profile.Taper, BendParams, Profile.Bed);
		// A block world's bed steps a block a column at most, as a brook's does (FRiverProfileParams::MaxBedGrade): a
		// segment steeper than the bound has its higher end lowered, from the end up and from the start down
		if (Ground.bBlocks)
		{
			const double Grade = GetBlockBedGrade();
			for (int32 Index = Num - 2; Index >= 0; --Index)
			{
				Profile.Bed[Index] = FMath::Min(Profile.Bed[Index], Profile.Bed[Index + 1] + Grade * FVector::Dist(Places[Index], Places[Index + 1]));
			}
			for (int32 Index = 1; Index < Num; ++Index)
			{
				Profile.Bed[Index] = FMath::Min(Profile.Bed[Index], Profile.Bed[Index - 1] + Grade * FVector::Dist(Places[Index - 1], Places[Index]));
			}
		}
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Bed.Vertices[Index].TargetHeight = static_cast<float>(Profile.Bed[Index]);
			Vertices[Index].BedLevel = Profile.Bed[Index] + LevelBase;
			DeepestCut = FMath::Max(DeepestCut, GroundAt[Index] - Profile.Bed[Index]);
		}
	}
	Profile.bStartsInBody = !Node.bHead;
	Profile.bEndsInBody = bToSea;
	Profile.Fall = Surface[0] - Surface[Num - 1];
	Profile.DeepestCut = DeepestCut;

	const TSharedRef<const FVoxelWaterReach> Shape = FVoxelWaterReach::Make(MoveTemp(Vertices), bRadial, Settings.FlowSpeed);
	if (Shape->NumSegments() < 1)
	{
		System->Refused = TEXT("a valley river of one place");
		return System;
	}
	Reach.Bed = FVoxelConditioningLine::Make(Bed);
	// The engine's own check of the bed's line: kept as a note (a valley is never dry), read back by the survey
	FVoxelConditioningLineLimits Limits;
	Limits.VoxelSize = static_cast<float>(Voxel);
	Limits.bBlockWorld = Ground.bBlocks;
	Limits.PlanetRadius = bRadial ? Ground.PlanetRadius : 0.0;
	for (const FString& Warning : Reach.Bed->GetWarnings(Limits))
	{
		if (Node.bHead && Warning.Contains(TEXT("the levelled core is")))
		{
			continue;
		}
		System->ConflictNote = FString::Printf(TEXT("the bed of the valley river from link (%d, %d): %s"), Link.X, Link.Y, *Warning);
		break;
	}
	Reach.Body = FVoxelWaterBody::MakeReach(MakeValleyReachId(Ground.Seed, Link), Shape, Ground.GetFrame());
	Reach.Body.DrawDistance = Settings.ValleyRiverDrawDistance;
	System->DeepestCut = DeepestCut;
	System->LongestRiver = Reach.Length;
	System->Reaches.Add(MoveTemp(Reach));
	return System;
}

void FVoxelHydrology::GatherValleyRivers(const FVoxelHydrologySettings& Settings, const FVoxelHydrologyGround& Ground, const FVoxelConditioningRegion& Region,
	IVoxelHydrologyCache* Cache, TArray<TSharedRef<const FVoxelHydrologySystem>>& Out)
{
	if (!Ground.Valleys.IsValid() || Ground.Valleys->IsRadial() != Ground.bRadial || Region.bEverywhere || Region.bCap != Ground.bRadial
		|| (!Region.bCap && !Region.Box.bIsValid))
	{
		return;
	}
	// The links whose valley reaches the region: a river stays on its valley's floor. A planet's cap as a chord around
	// its middle on the datum sphere
	TArray<FVoxelValleySegment> Segments;
	if (Region.bCap)
	{
		const double Radius = Ground.Valleys->GetLattice().GetRadius();
		const double Chord = Region.AngularRadius >= UE_DOUBLE_PI ? -1.0 : 2.0 * Radius * FMath::Sin(0.5 * Region.AngularRadius);
		Ground.Valleys->GatherSegmentsNear(Region.Direction * Radius, Chord, Segments);
	}
	else
	{
		Ground.Valleys->GatherSegments(Region.Box, Segments);
	}
	TSet<FIntPoint> Seen;
	TArray<FIntPoint> Links;
	for (const FVoxelValleySegment& Segment : Segments)
	{
		if (!Seen.Contains(Segment.Link))
		{
			Seen.Add(Segment.Link);
			Links.Add(Segment.Link);
		}
	}
	Links.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.Y != B.Y ? A.Y < B.Y : A.X < B.X; });
	for (const FIntPoint& Link : Links)
	{
		TSharedPtr<const FVoxelHydrologySystem> River = Cache ? Cache->FindValleyRiver(Link) : nullptr;
		if (!River.IsValid())
		{
			const TSharedRef<const FVoxelHydrologySystem> Made = BuildValleyRiver(Settings, Ground, Link);
			River = Cache ? Cache->StoreValleyRiver(Made).ToSharedPtr() : Made.ToSharedPtr();
		}
		if (River->Reaches.Num() > 0)
		{
			Out.Add(River.ToSharedRef());
		}
	}
}
