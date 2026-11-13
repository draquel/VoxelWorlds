// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelConditioningLine.h"
#include "VoxelTerrainConditioning.h"

namespace VoxelConditioningLinePrivate
{
	/**
	 * The nearest place on the segment AB to P.
	 * @param OutT Where along AB it is, 0 at A to 1 at B (0 for a segment of no length)
	 * @return The squared distance from P to it
	 */
	template <typename VectorType>
	double NearestOnSegment(const VectorType& P, const VectorType& A, const VectorType& B, double& OutT)
	{
		const VectorType AB = B - A;
		const VectorType AP = P - A;
		const double LengthSq = AB | AB;
		OutT = LengthSq > UE_DOUBLE_SMALL_NUMBER ? FMath::Clamp((AP | AB) / LengthSq, 0.0, 1.0) : 0.0;
		const VectorType D = AP - AB * OutT;
		return D | D;
	}

	/** The arc on a sphere of radius R under a chord: 2 R asin(chord / 2R), as a planet zone measures its distance. */
	double ChordToArc(double Chord, double Radius)
	{
		return 2.0 * Radius * FMath::Asin(FMath::Min(1.0, Chord / (2.0 * Radius)));
	}

	/** The angle between two unit directions, well conditioned at small angles. */
	double AngleBetween(const FVector& A, const FVector& B)
	{
		return FMath::Atan2(FVector::CrossProduct(A, B).Size(), FVector::DotProduct(A, B));
	}

	/** The distance between two segments, as the least of the four end-to-segment distances (0 when they cross in the plane). */
	template <typename VectorType>
	double SegmentDistanceSq(const VectorType& A0, const VectorType& A1, const VectorType& B0, const VectorType& B1)
	{
		double T = 0.0;
		return FMath::Min(
			FMath::Min(NearestOnSegment(A0, B0, B1, T), NearestOnSegment(A1, B0, B1, T)),
			FMath::Min(NearestOnSegment(B0, A0, A1, T), NearestOnSegment(B1, A0, A1, T)));
	}

	/**
	 * Whether two segments in the plane cross: each one's ends lie on opposite sides of the other's line, by more than
	 * a thousandth of a unit (two segments of one straight line do not cross; touching ends are caught by the distance).
	 */
	bool SegmentsCross(const FVector2D& A0, const FVector2D& A1, const FVector2D& B0, const FVector2D& B1)
	{
		// The signed distance of R from the line through P and Q.
		const auto Side = [](const FVector2D& P, const FVector2D& Q, const FVector2D& R)
		{
			const FVector2D Along = (Q - P).GetSafeNormal();
			return FVector2D::CrossProduct(Along, R - P);
		};
		constexpr double Clear = 1.0e-3;
		const double D1 = Side(B0, B1, A0);
		const double D2 = Side(B0, B1, A1);
		const double D3 = Side(A0, A1, B0);
		const double D4 = Side(A0, A1, B1);
		const auto Opposite = [](double U, double V) { return (U > Clear && V < -Clear) || (U < -Clear && V > Clear); };
		return Opposite(D1, D2) && Opposite(D3, D4);
	}
}

FVoxelConditioningLineRef FVoxelConditioningLine::Make(const FVoxelConditioningLineParams& Params)
{
	TSharedRef<FVoxelConditioningLine> Line = MakeShared<FVoxelConditioningLine>();
	Line->FalloffWidth = FMath::Max(Params.FalloffWidth, 0.0f);
	Line->Strength = FMath::Clamp(Params.Strength, 0.0f, 1.0f);
	Line->Blend = Params.Blend;
	Line->Id = Params.Id;
	Line->bRadial = Params.bRadial;

	// Two vertices at one place make a segment of no length, which has no direction to measure along: merged here.
	const double SamePlace = Params.bRadial ? 1.0e-10 : 1.0e-3;
	Line->Vertices.Reserve(Params.Vertices.Num());
	for (const FVoxelConditioningLineVertex& In : Params.Vertices)
	{
		FVoxelConditioningLineVertex Vertex = In;
		Vertex.HalfWidth = FMath::Max(In.HalfWidth, 0.0f);
		if (Params.bRadial)
		{
			Vertex.Position = In.Position.GetSafeNormal();
			if (Vertex.Position.IsZero())
			{
				continue;
			}
		}
		else
		{
			Vertex.Position.Z = 0.0;
		}
		if (Line->Vertices.Num() > 0 && Line->Vertices.Last().Position.Equals(Vertex.Position, SamePlace))
		{
			continue;
		}
		Line->Vertices.Add(Vertex);
	}
	if (Line->Vertices.Num() < 2)
	{
		Line->Vertices.Reset();
	}
	Line->BuildBounds();
	return Line;
}

void FVoxelConditioningLine::BuildBounds()
{
	float MaxHalfWidth = 0.0f;
	for (const FVoxelConditioningLineVertex& Vertex : Vertices)
	{
		MaxHalfWidth = FMath::Max(MaxHalfWidth, Vertex.HalfWidth);
	}
	// The weight's profile holds the falloff at 1 or more (WeightAtDistance), so the reach does too.
	Reach = static_cast<double>(MaxHalfWidth) + FMath::Max(1.0, static_cast<double>(FalloffWidth));

	Bounds = FBox2D(ForceInit);
	BlockBounds.Reset();
	CapCentre = FVector::UpVector;
	CapChordRadius = 0.0;
	BlockBalls.Reset();
	const int32 Segments = NumSegments();
	if (Segments == 0)
	{
		return;
	}
	const int32 Blocks = FMath::DivideAndRoundUp(Segments, BlockSegments);

	if (!bRadial)
	{
		BlockBounds.Reserve(Blocks);
		for (int32 Block = 0; Block < Blocks; ++Block)
		{
			// A block's segments [First, Last) read the vertices [First, Last].
			const int32 First = Block * BlockSegments;
			const int32 Last = FMath::Min(First + BlockSegments, Segments);
			FBox2D Box(ForceInit);
			for (int32 Index = First; Index <= Last; ++Index)
			{
				Box += FVector2D(Vertices[Index].Position.X, Vertices[Index].Position.Y);
			}
			BlockBounds.Add(Box);
			Bounds += Box;
		}
		return;
	}

	// A ball around a run of unit directions: the normalized sum as the centre, the largest chord to a vertex as the radius.
	const auto BallOf = [this](int32 FirstVertex, int32 LastVertex)
	{
		FVector Sum = FVector::ZeroVector;
		for (int32 Index = FirstVertex; Index <= LastVertex; ++Index)
		{
			Sum += Vertices[Index].Position;
		}
		const FVector Centre = Sum.IsNearlyZero() ? Vertices[FirstVertex].Position : Sum.GetUnsafeNormal();
		double Radius = 0.0;
		for (int32 Index = FirstVertex; Index <= LastVertex; ++Index)
		{
			Radius = FMath::Max(Radius, (Vertices[Index].Position - Centre).Size());
		}
		return FVector4d(Centre.X, Centre.Y, Centre.Z, Radius);
	};
	BlockBalls.Reserve(Blocks);
	for (int32 Block = 0; Block < Blocks; ++Block)
	{
		const int32 First = Block * BlockSegments;
		BlockBalls.Add(BallOf(First, FMath::Min(First + BlockSegments, Segments)));
	}
	const FVector4d Whole = BallOf(0, Segments);
	CapCentre = FVector(Whole.X, Whole.Y, Whole.Z);
	CapChordRadius = Whole.W;
}

FBox2D FVoxelConditioningLine::GetBounds2D() const
{
	if (bRadial || NumSegments() == 0)
	{
		return FBox2D(ForceInit);
	}
	return Bounds.ExpandBy(Reach);
}

bool FVoxelConditioningLine::GetCap(double PlanetRadius, FVector& OutDirection, double& OutAngle) const
{
	if (!bRadial || NumSegments() == 0)
	{
		return false;
	}
	OutDirection = CapCentre;
	OutAngle = 2.0 * FMath::Asin(FMath::Min(1.0, 0.5 * CapChordRadius)) + Reach / FMath::Max(PlanetRadius, 1.0);
	return true;
}

bool FVoxelConditioningLine::MeetsRegion(const FBox2D& Region) const
{
	if (bRadial || NumSegments() == 0 || !Bounds.ExpandBy(Reach).Intersect(Region))
	{
		return false;
	}
	for (const FBox2D& Box : BlockBounds)
	{
		if (Box.ExpandBy(Reach).Intersect(Region))
		{
			return true;
		}
	}
	return false;
}

bool FVoxelConditioningLine::MeetsCap(const FVector& CapDirection, double CapAngle, double PlanetRadius) const
{
	using namespace VoxelConditioningLinePrivate;
	if (!bRadial || NumSegments() == 0)
	{
		return false;
	}
	if (CapAngle >= UE_DOUBLE_PI)
	{
		return true;
	}
	const double ReachAngle = Reach / FMath::Max(PlanetRadius, 1.0);
	const auto BallMeets = [&](const FVector& Centre, double ChordRadius)
	{
		return AngleBetween(CapDirection, Centre) <= CapAngle + 2.0 * FMath::Asin(FMath::Min(1.0, 0.5 * ChordRadius)) + ReachAngle;
	};
	if (!BallMeets(CapCentre, CapChordRadius))
	{
		return false;
	}
	for (const FVector4d& Ball : BlockBalls)
	{
		if (BallMeets(FVector(Ball.X, Ball.Y, Ball.Z), Ball.W))
		{
			return true;
		}
	}
	return false;
}

bool FVoxelConditioningLine::FindNearest(double X, double Y, int32& OutSegment, double& OutT, double& OutDistSq) const
{
	using namespace VoxelConditioningLinePrivate;
	if (bRadial || NumSegments() == 0
		|| X < Bounds.Min.X - Reach || X > Bounds.Max.X + Reach || Y < Bounds.Min.Y - Reach || Y > Bounds.Max.Y + Reach)
	{
		return false;
	}
	// Only a segment nearer than the reach can give a weight, so the search starts there: a block farther than the reach
	// cannot hold the nearest segment of a point the line changes.
	const FVector2D P(X, Y);
	const int32 Segments = NumSegments();
	double Best = Reach * Reach;
	bool bFound = false;
	for (int32 Block = 0; Block < BlockBounds.Num(); ++Block)
	{
		const FBox2D& Box = BlockBounds[Block];
		if (X < Box.Min.X - Reach || X > Box.Max.X + Reach || Y < Box.Min.Y - Reach || Y > Box.Max.Y + Reach)
		{
			continue;
		}
		const int32 First = Block * BlockSegments;
		const int32 Last = FMath::Min(First + BlockSegments, Segments);
		for (int32 Segment = First; Segment < Last; ++Segment)
		{
			double T = 0.0;
			const double DistSq = NearestOnSegment(P,
				FVector2D(Vertices[Segment].Position.X, Vertices[Segment].Position.Y),
				FVector2D(Vertices[Segment + 1].Position.X, Vertices[Segment + 1].Position.Y), T);
			// Strictly nearer: of two segments at the same distance the earlier one along the line answers, as in the
			// generators' loop.
			if (DistSq < Best)
			{
				Best = DistSq;
				OutSegment = Segment;
				OutT = T;
				bFound = true;
			}
		}
	}
	OutDistSq = Best;
	return bFound;
}

bool FVoxelConditioningLine::FindNearestOnSphere(const FVector& Point, double PlanetRadius, int32& OutSegment, double& OutT, double& OutDistSq) const
{
	using namespace VoxelConditioningLinePrivate;
	if (!bRadial || NumSegments() == 0 || PlanetRadius <= 0.0)
	{
		return false;
	}
	// On the unit sphere: a point's chord to a ball's centre is at most the ball's radius plus its chord to the nearest
	// segment, and a chord is no longer than its arc, so a ball farther than its radius plus the reach holds no segment
	// within reach.
	const double ReachChord = Reach / PlanetRadius;
	if ((Point - CapCentre).SizeSquared() > FMath::Square(CapChordRadius + ReachChord))
	{
		return false;
	}
	const int32 Segments = NumSegments();
	double Best = ReachChord * ReachChord;
	bool bFound = false;
	for (int32 Block = 0; Block < BlockBalls.Num(); ++Block)
	{
		const FVector4d& Ball = BlockBalls[Block];
		if ((Point - FVector(Ball.X, Ball.Y, Ball.Z)).SizeSquared() > FMath::Square(Ball.W + ReachChord))
		{
			continue;
		}
		const int32 First = Block * BlockSegments;
		const int32 Last = FMath::Min(First + BlockSegments, Segments);
		for (int32 Segment = First; Segment < Last; ++Segment)
		{
			double T = 0.0;
			const double DistSq = NearestOnSegment(Point, Vertices[Segment].Position, Vertices[Segment + 1].Position, T);
			if (DistSq < Best)
			{
				Best = DistSq;
				OutSegment = Segment;
				OutT = T;
				bFound = true;
			}
		}
	}
	// A chord on the datum sphere, in world units.
	OutDistSq = Best * PlanetRadius * PlanetRadius;
	return bFound;
}

bool FVoxelConditioningLine::Evaluate(double X, double Y, float& OutTarget, float& OutWeight) const
{
	int32 Segment = 0;
	double T = 0.0;
	double DistSq = 0.0;
	if (!FindNearest(X, Y, Segment, T, DistSq))
	{
		return false;
	}
	const FVoxelConditioningLineVertex& A = Vertices[Segment];
	const FVoxelConditioningLineVertex& B = Vertices[Segment + 1];
	const float Along = static_cast<float>(T);
	OutTarget = A.TargetHeight + Along * (B.TargetHeight - A.TargetHeight);
	OutWeight = WeightAtDistance(static_cast<float>(FMath::Sqrt(DistSq)), A.HalfWidth + Along * (B.HalfWidth - A.HalfWidth), FalloffWidth, Strength);
	return OutWeight > 0.0f;
}

bool FVoxelConditioningLine::EvaluateOnSphere(const FVector& Direction, double PlanetRadius, float& OutTarget, float& OutWeight) const
{
	int32 Segment = 0;
	double T = 0.0;
	double DistSq = 0.0;
	if (!FindNearestOnSphere(Direction, PlanetRadius, Segment, T, DistSq))
	{
		return false;
	}
	const FVoxelConditioningLineVertex& A = Vertices[Segment];
	const FVoxelConditioningLineVertex& B = Vertices[Segment + 1];
	const float Along = static_cast<float>(T);
	const double Arc = VoxelConditioningLinePrivate::ChordToArc(FMath::Sqrt(DistSq), PlanetRadius);
	OutTarget = A.TargetHeight + Along * (B.TargetHeight - A.TargetHeight);
	OutWeight = WeightAtDistance(static_cast<float>(Arc), A.HalfWidth + Along * (B.HalfWidth - A.HalfWidth), FalloffWidth, Strength);
	return OutWeight > 0.0f;
}

float FVoxelConditioningLine::PeakOfDistanceTimesWeight(float HalfWidth, float InFalloffWidth)
{
	// d x weight(d) rises to the core's edge and peaks a little into the falloff; sampled, the peak is flat there.
	const float Falloff = FMath::Max(1.0f, InFalloffWidth);
	float Peak = HalfWidth;
	constexpr int32 Steps = 64;
	for (int32 Step = 1; Step < Steps; ++Step)
	{
		const float Dist = HalfWidth + Falloff * static_cast<float>(Step) / static_cast<float>(Steps);
		Peak = FMath::Max(Peak, Dist * WeightAtDistance(Dist, HalfWidth, InFalloffWidth, 1.0f));
	}
	return Peak;
}

void FVoxelConditioningLine::AppendSegment(int32 Segment, const FVector3f& A, const FVector3f& B, int32 LineIndex, TArray<FVector4f>& Out) const
{
	const FVoxelConditioningLineVertex& VertexA = Vertices[Segment];
	const FVoxelConditioningLineVertex& VertexB = Vertices[Segment + 1];
	Out.Add(FVector4f(A, VertexA.TargetHeight));
	Out.Add(FVector4f(B, VertexB.TargetHeight));
	Out.Add(FVector4f(VertexA.HalfWidth, VertexB.HalfWidth, FalloffWidth, Strength));
	Out.Add(FVector4f(static_cast<float>(LineIndex), static_cast<float>(static_cast<uint8>(Blend)), 0.0f, 0.0f));
}

void FVoxelConditioningLine::AppendChunkSegments(const FBox2D& ChunkBox, const FVector2D& Origin, int32 LineIndex, TArray<FVector4f>& Out,
	FBox3f& OutBounds) const
{
	if (bRadial || NumSegments() == 0)
	{
		return;
	}
	const FBox2D Grown = ChunkBox.ExpandBy(Reach);
	if (!Bounds.Intersect(Grown))
	{
		return;
	}
	const int32 Segments = NumSegments();
	const FVector3f ReachExtent(static_cast<float>(Reach), static_cast<float>(Reach), 0.0f);
	for (int32 Block = 0; Block < BlockBounds.Num(); ++Block)
	{
		if (!BlockBounds[Block].Intersect(Grown))
		{
			continue;
		}
		const int32 First = Block * BlockSegments;
		const int32 Last = FMath::Min(First + BlockSegments, Segments);
		for (int32 Segment = First; Segment < Last; ++Segment)
		{
			const FVector2D A(Vertices[Segment].Position.X, Vertices[Segment].Position.Y);
			const FVector2D B(Vertices[Segment + 1].Position.X, Vertices[Segment + 1].Position.Y);
			if (FMath::Max(A.X, B.X) < Grown.Min.X || FMath::Min(A.X, B.X) > Grown.Max.X
				|| FMath::Max(A.Y, B.Y) < Grown.Min.Y || FMath::Min(A.Y, B.Y) > Grown.Max.Y)
			{
				continue;
			}
			// Relative to the chunk's origin, formed in double: no float here is of the size of a world position.
			const FVector3f LocalA(static_cast<float>(A.X - Origin.X), static_cast<float>(A.Y - Origin.Y), 0.0f);
			const FVector3f LocalB(static_cast<float>(B.X - Origin.X), static_cast<float>(B.Y - Origin.Y), 0.0f);
			AppendSegment(Segment, LocalA, LocalB, LineIndex, Out);
			OutBounds += LocalA.ComponentMin(LocalB) - ReachExtent;
			OutBounds += LocalA.ComponentMax(LocalB) + ReachExtent;
		}
	}
}

void FVoxelConditioningLine::AppendChunkSegmentsOnSphere(const FVector& AnchorDirection, double PlanetRadius, double CapChord, int32 LineIndex,
	TArray<FVector4f>& Out, FBox3f& OutBounds) const
{
	using namespace VoxelConditioningLinePrivate;
	if (!bRadial || NumSegments() == 0 || PlanetRadius <= 0.0)
	{
		return;
	}
	// In world units around the anchor's datum point: the chunk's datum points lie within CapChord of it, so a segment
	// farther than CapChord plus the reach changes none of them.
	const bool bAll = CapChord < 0.0;
	const double Near = CapChord + Reach;
	const FVector AnchorPoint = AnchorDirection * PlanetRadius;
	const int32 Segments = NumSegments();
	const FVector3f ReachExtent(static_cast<float>(Reach));
	for (int32 Block = 0; Block < BlockBalls.Num(); ++Block)
	{
		const FVector4d& Ball = BlockBalls[Block];
		if (!bAll && (FVector(Ball.X, Ball.Y, Ball.Z) * PlanetRadius - AnchorPoint).SizeSquared() > FMath::Square(Ball.W * PlanetRadius + Near))
		{
			continue;
		}
		const int32 First = Block * BlockSegments;
		const int32 Last = FMath::Min(First + BlockSegments, Segments);
		for (int32 Segment = First; Segment < Last; ++Segment)
		{
			// The vertices' datum points less the anchor's, in double (WM-BP6): the shader measures short chords.
			const FVector A = Vertices[Segment].Position * PlanetRadius - AnchorPoint;
			const FVector B = Vertices[Segment + 1].Position * PlanetRadius - AnchorPoint;
			double T = 0.0;
			if (!bAll && NearestOnSegment(FVector::ZeroVector, A, B, T) > Near * Near)
			{
				continue;
			}
			const FVector3f LocalA(A);
			const FVector3f LocalB(B);
			AppendSegment(Segment, LocalA, LocalB, LineIndex, Out);
			OutBounds += LocalA.ComponentMin(LocalB) - ReachExtent;
			OutBounds += LocalA.ComponentMax(LocalB) + ReachExtent;
		}
	}
}

TArray<FString> FVoxelConditioningLine::GetWarnings(const FVoxelConditioningLineLimits& Limits) const
{
	using namespace VoxelConditioningLinePrivate;
	TArray<FString> Warnings;
	const int32 Segments = NumSegments();
	if (Segments == 0)
	{
		Warnings.Add(TEXT("the line has fewer than two distinct vertices: it has no segment and conditions nothing"));
		return Warnings;
	}
	const double Voxel = FMath::Max(static_cast<double>(Limits.VoxelSize), 1.0);
	const bool bLevels = Strength > 0.0f;
	// World positions of the vertices: XY on a heightmap world, the points on the datum sphere on a planet.
	const double Scale = bRadial ? FMath::Max(Limits.PlanetRadius, 1.0) : 1.0;
	const auto PointOf = [&](int32 Index) { return Vertices[Index].Position * Scale; };
	if (bRadial && Limits.PlanetRadius <= 0.0)
	{
		Warnings.Add(TEXT("a planet line in a world with no planet radius: it conditions nothing"));
		return Warnings;
	}

	// The cut and fill cap: the LOD strategy's surface slab assumes conditioning stays near the natural surface.
	if (bLevels && Limits.MaxCutFill > 0.0f && Limits.NaturalHeight)
	{
		int32 Over = 0;
		float Worst = 0.0f;
		for (const FVoxelConditioningLineVertex& Vertex : Vertices)
		{
			const float Offset = FMath::Abs(Vertex.TargetHeight - Limits.NaturalHeight(Vertex.Position));
			if (Offset > Limits.MaxCutFill)
			{
				++Over;
				Worst = FMath::Max(Worst, Offset);
			}
		}
		if (Over > 0)
		{
			Warnings.Add(FString::Printf(TEXT("%d of %d vertices have a target more than %.0f uu from the natural surface (the cut and fill cap; the farthest %.0f uu): the LOD surface slab does not look at lines, so chunks there may not stream"),
				Over, Vertices.Num(), Limits.MaxCutFill, Worst));
		}
	}

	// A levelled core and a falloff of at least two voxels each, so a mesher's lattice always samples both.
	if (bLevels)
	{
		float MinHalfWidth = TNumericLimits<float>::Max();
		for (const FVoxelConditioningLineVertex& Vertex : Vertices)
		{
			MinHalfWidth = FMath::Min(MinHalfWidth, Vertex.HalfWidth);
		}
		if (2.0 * MinHalfWidth < 2.0 * Voxel)
		{
			Warnings.Add(FString::Printf(TEXT("the levelled core is %.0f uu wide at its narrowest, under two voxels (%.0f uu)"), 2.0f * MinHalfWidth, 2.0 * Voxel));
		}
		if (FalloffWidth < 2.0 * Voxel)
		{
			Warnings.Add(FString::Printf(TEXT("the falloff is %.0f uu wide, under two voxels (%.0f uu)"), FalloffWidth, 2.0 * Voxel));
		}
	}

	// Per segment: its length and grade; on a planet its chord's sag; on a block world the step between columns.
	TArray<double> Lengths;
	TArray<double> Grades;
	Lengths.SetNumUninitialized(Segments);
	Grades.SetNumUninitialized(Segments);
	int32 Sagging = 0;
	double WorstSag = 0.0;
	int32 SteepBlocks = 0;
	double SteepestBlock = 0.0;
	for (int32 Segment = 0; Segment < Segments; ++Segment)
	{
		const double Length = (PointOf(Segment + 1) - PointOf(Segment)).Size();
		Lengths[Segment] = Length;
		Grades[Segment] = Length > UE_DOUBLE_SMALL_NUMBER
			? FMath::Abs(static_cast<double>(Vertices[Segment + 1].TargetHeight) - Vertices[Segment].TargetHeight) / Length : 0.0;
		if (bRadial)
		{
			const double Sag = Length * Length / (8.0 * Scale);
			if (Sag > 0.1 * Voxel)
			{
				++Sagging;
				WorstSag = FMath::Max(WorstSag, Sag);
			}
		}
		// Adjacent columns, diagonals included, are up to a voxel x sqrt 2 apart along the line.
		if (Limits.bBlockWorld && bLevels && Grades[Segment] > UE_DOUBLE_INV_SQRT_2)
		{
			++SteepBlocks;
			SteepestBlock = FMath::Max(SteepestBlock, Grades[Segment]);
		}
	}
	if (Sagging > 0)
	{
		Warnings.Add(FString::Printf(TEXT("%d of %d segments are long enough for their chord to sag more than a tenth of a voxel under the datum sphere (the most %.1f uu): the distance to the line reads long over their middle; add vertices"),
			Sagging, Segments, WorstSag));
	}
	if (SteepBlocks > 0)
	{
		Warnings.Add(FString::Printf(TEXT("%d of %d segments are steeper than one block a column on a block world (the steepest %.0f %%): the block tops along the line step by more than one block"),
			SteepBlocks, Segments, 100.0 * SteepestBlock));
	}

	// The bend bound at each inner vertex: the nearest segment changes across the bisector on the inside of a bend, and
	// with it the target, by 2 x grade x d x tan(turn / 2) at a distance d from the line. (1) That step times the weight,
	// at its peak, stays under a tenth of a voxel. (2) The bisectors of neighbouring vertices meet outside the reach.
	int32 Stepping = 0;
	double WorstStep = 0.0;
	int32 TightTurns = 0;
	double WorstTurn = 0.0;
	for (int32 Index = 1; Index < Segments; ++Index)
	{
		FVector In = PointOf(Index) - PointOf(Index - 1);
		FVector OutDir = PointOf(Index + 1) - PointOf(Index);
		if (bRadial)
		{
			// On a planet the turn is measured along the surface: the chords of a great circle turn toward the centre by
			// their length over the radius, which is no bend.
			const FVector& Up = Vertices[Index].Position;
			In -= Up * FVector::DotProduct(In, Up);
			OutDir -= Up * FVector::DotProduct(OutDir, Up);
		}
		const double Turn = AngleBetween(In.GetSafeNormal(), OutDir.GetSafeNormal());
		if (Turn <= UE_DOUBLE_SMALL_NUMBER)
		{
			continue;
		}
		const double HalfTan = FMath::Tan(FMath::Min(0.5 * Turn, 0.49 * UE_DOUBLE_PI));
		if (bLevels)
		{
			const double Grade = FMath::Max(Grades[Index - 1], Grades[Index]);
			const double Step = 2.0 * Grade * HalfTan * Strength * PeakOfDistanceTimesWeight(Vertices[Index].HalfWidth, FalloffWidth);
			if (Step > 0.1 * Voxel)
			{
				++Stepping;
				WorstStep = FMath::Max(WorstStep, Step);
			}
		}
		if (HalfTan > FMath::Min(Lengths[Index - 1], Lengths[Index]) / (2.0 * Reach))
		{
			++TightTurns;
			WorstTurn = FMath::Max(WorstTurn, Turn);
		}
	}
	if (Stepping > 0)
	{
		Warnings.Add(FString::Printf(TEXT("%d vertices break the bend bound: the target steps by up to %.1f uu across the bend's bisector, over a tenth of a voxel (%.1f uu); turn less per vertex or flatten the profile through the bend"),
			Stepping, WorstStep, 0.1 * Voxel));
	}
	if (TightTurns > 0)
	{
		Warnings.Add(FString::Printf(TEXT("%d vertices turn so sharply (up to %.0f degrees) that their bisector meets a neighbour's inside the reach (%.0f uu): round the corner to a radius of the reach or more"),
			TightTurns, FMath::RadiansToDegrees(WorstTurn), Reach));
	}

	// No part of the line within twice its reach of another part, neighbouring segments aside: the nearest segment
	// would change there with a step no bound on the turn limits. Two segments are neighbours while less than half a
	// circle of the smallest radius the bend bound allows (pi x reach) lies between them along the line.
	{
		TArray<double> Along;
		Along.SetNumUninitialized(Segments + 1);
		Along[0] = 0.0;
		for (int32 Segment = 0; Segment < Segments; ++Segment)
		{
			Along[Segment + 1] = Along[Segment] + Lengths[Segment];
		}
		const double Apart = UE_DOUBLE_PI * Reach;
		const double Close = 0.99 * 2.0 * Reach;
		const int32 Blocks = FMath::DivideAndRoundUp(Segments, BlockSegments);
		int32 ClosePairs = 0;
		for (int32 BlockA = 0; BlockA < Blocks; ++BlockA)
		{
			for (int32 BlockB = BlockA; BlockB < Blocks; ++BlockB)
			{
				// Blocks farther apart than the test's distance hold no pair.
				if (bRadial)
				{
					const FVector4d& BallA = BlockBalls[BlockA];
					const FVector4d& BallB = BlockBalls[BlockB];
					if ((FVector(BallA.X, BallA.Y, BallA.Z) - FVector(BallB.X, BallB.Y, BallB.Z)).Size() * Scale > (BallA.W + BallB.W) * Scale + Close)
					{
						continue;
					}
				}
				else if (!BlockBounds[BlockA].ExpandBy(Close).Intersect(BlockBounds[BlockB]))
				{
					continue;
				}
				const int32 LastA = FMath::Min((BlockA + 1) * BlockSegments, Segments);
				const int32 LastB = FMath::Min((BlockB + 1) * BlockSegments, Segments);
				for (int32 A = BlockA * BlockSegments; A < LastA; ++A)
				{
					for (int32 B = FMath::Max(BlockB * BlockSegments, A + 1); B < LastB; ++B)
					{
						if (Along[B] - Along[A + 1] <= Apart)
						{
							continue;
						}
						const FVector A0 = PointOf(A);
						const FVector A1 = PointOf(A + 1);
						const FVector B0 = PointOf(B);
						const FVector B1 = PointOf(B + 1);
						const bool bClose = SegmentDistanceSq(A0, A1, B0, B1) < Close * Close
							|| (!bRadial && SegmentsCross(FVector2D(A0), FVector2D(A1), FVector2D(B0), FVector2D(B1)));
						if (bClose)
						{
							++ClosePairs;
						}
					}
				}
			}
		}
		if (ClosePairs > 0)
		{
			Warnings.Add(FString::Printf(TEXT("%d pairs of segments lie within twice the reach (%.0f uu) of each other without being neighbours: the line comes back on itself or crosses itself, and the nearest segment changes there with a step"),
				ClosePairs, 2.0 * Reach));
		}
	}
	return Warnings;
}

float FVoxelConditioning::ApplyLinesToHeight(double X, double Y, float BaseHeight) const
{
	float H = BaseHeight;
	for (const FVoxelConditioningLineRef& Line : Lines)
	{
		float Target = 0.0f;
		float Weight = 0.0f;
		if (Line->Evaluate(X, Y, Target, Weight))
		{
			H = FVoxelConditioningLine::BlendHeight(Line->GetBlend(), H, Target, Weight);
		}
	}
	return H;
}

float FVoxelConditioning::ApplyLinesToAltitude(const FVector& Direction, double PlanetRadius, float BaseAltitude) const
{
	float A = BaseAltitude;
	for (const FVoxelConditioningLineRef& Line : Lines)
	{
		float Target = 0.0f;
		float Weight = 0.0f;
		if (Line->EvaluateOnSphere(Direction, PlanetRadius, Target, Weight))
		{
			A = FVoxelConditioningLine::BlendHeight(Line->GetBlend(), A, Target, Weight);
		}
	}
	return A;
}

void FVoxelConditioning::SortLines()
{
	if (Lines.Num() < 2)
	{
		return;
	}
	// By id, the order of the gather kept between equal ids; a line gathered twice (two sources, or a region gathered in
	// parts) counts once.
	Lines.StableSort([](const FVoxelConditioningLineRef& A, const FVoxelConditioningLineRef& B) { return A->GetId() < B->GetId(); });
	for (int32 Index = Lines.Num() - 1; Index > 0; --Index)
	{
		for (int32 Earlier = Index - 1; Earlier >= 0 && Lines[Earlier]->GetId() == Lines[Index]->GetId(); --Earlier)
		{
			if (Lines[Earlier] == Lines[Index])
			{
				Lines.RemoveAt(Index);
				break;
			}
		}
	}
}
