// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelTerrainLineComponent.h"
#include "VoxelStreaming.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "IVoxelWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelConditioningLineShape.h"
#include "Components/SplineComponent.h"
#include "Misc/Crc.h"

namespace VoxelTerrainLinePrivate
{
	/**
	 * How a line stands against the bend bound: the largest, over its inner vertices, of the vertex's turn over the turn
	 * the bound allows there. The bound allows the lesser of two turns (TERRAIN_LINES_PLAN.md, section 4.1): the one whose
	 * bisector meets a neighbour's outside the reach, and, on a line that levels, the one whose step of the target across
	 * the bisector, times the weight at its peak, is a tenth of a voxel on the steeper of the vertex's two segments. Over
	 * 1 the line breaks the bound. These are the two bend checks of FVoxelConditioningLine::GetWarnings, on the same
	 * vertices, lengths and grades, as one number; the warnings stay that function's.
	 * @param Line The line
	 * @param VoxelSize The world's voxel size
	 * @param PlanetRadius The datum radius, for a planet line
	 * @return The ratio (0 on a line with no turn)
	 */
	double GetBendRatio(const FVoxelConditioningLine& Line, double VoxelSize, double PlanetRadius)
	{
		const TArray<FVoxelConditioningLineVertex>& Vertices = Line.GetVertices();
		const bool bRadial = Line.IsRadial();
		const double Scale = bRadial ? FMath::Max(PlanetRadius, 1.0) : 1.0;
		const double Voxel = FMath::Max(VoxelSize, 1.0);
		double Worst = 0.0;
		for (int32 Index = 1; Index + 1 < Vertices.Num(); ++Index)
		{
			FVector In = (Vertices[Index].Position - Vertices[Index - 1].Position) * Scale;
			FVector Out = (Vertices[Index + 1].Position - Vertices[Index].Position) * Scale;
			const double LengthIn = In.Size();
			const double LengthOut = Out.Size();
			if (bRadial)
			{
				// Along the surface: the chords of a great circle turn toward the centre, which is no bend.
				const FVector& Up = Vertices[Index].Position;
				In -= Up * FVector::DotProduct(In, Up);
				Out -= Up * FVector::DotProduct(Out, Up);
			}
			const FVector InDirection = In.GetSafeNormal();
			const FVector OutDirection = Out.GetSafeNormal();
			const double Turn = FMath::Atan2(FVector::CrossProduct(InDirection, OutDirection).Size(), FVector::DotProduct(InDirection, OutDirection));
			if (Turn <= UE_DOUBLE_SMALL_NUMBER)
			{
				continue;
			}
			double Allowed = FVoxelConditioningLineShape::GetMaxTurnForReach(FMath::Min(LengthIn, LengthOut), Line.GetHeightReach());
			if (Line.GetStrength() > 0.0f)
			{
				const double RiseIn = FMath::Abs(static_cast<double>(Vertices[Index].TargetHeight) - Vertices[Index - 1].TargetHeight);
				const double RiseOut = FMath::Abs(static_cast<double>(Vertices[Index + 1].TargetHeight) - Vertices[Index].TargetHeight);
				const double Grade = FMath::Max(LengthIn > UE_DOUBLE_SMALL_NUMBER ? RiseIn / LengthIn : 0.0, LengthOut > UE_DOUBLE_SMALL_NUMBER ? RiseOut / LengthOut : 0.0);
				Allowed = FMath::Min(Allowed, FVoxelConditioningLineShape::GetMaxTurnForGrade(Grade, Vertices[Index].HalfWidth, Line.GetFalloffWidth(), Voxel, Line.GetStrength()));
			}
			Worst = FMath::Max(Worst, Turn / FMath::Max(Allowed, UE_DOUBLE_SMALL_NUMBER));
		}
		return Worst;
	}

	/**
	 * How far a rounded line runs from its spline, both ways: the farthest any vertex lies from the spline, or any point
	 * of the spline from the line (a rounded corner's move, the arc's middle from the spline's corner, which the spline's
	 * samples may cut). The spline is read every eighth of a step; each vertex is held against the stretch of it around
	 * its own place along the spline, as far each way as a corner's arc reaches.
	 * @param Spline The spline
	 * @param Points The line's vertices (world X and Y with Z 0; on a planet the points on the datum sphere)
	 * @param Along Each vertex's place along the spline (world units)
	 * @param Step The spline's sampling step
	 * @param Radius The corners' radius
	 * @param Planet The planet, for a planet line (else null)
	 * @return The distance (world units)
	 */
	double GetFarthestFromSpline(const USplineComponent& Spline, TConstArrayView<FVector> Points, TConstArrayView<double> Along, double Step, double Radius,
		const FSphericalPlanetParams* Planet)
	{
		const double Length = Spline.GetSplineLength();
		const double Fine = 0.125 * Step;
		const int32 Num = FMath::Max(1, FMath::CeilToInt32(Length / Fine));
		TArray<FVector> Read;
		Read.Reserve(Num + 1);
		for (int32 Index = 0; Index <= Num; ++Index)
		{
			const FVector World = Spline.GetLocationAtDistanceAlongSpline(static_cast<float>(Length * Index / Num), ESplineCoordinateSpace::World);
			Read.Add(Planet ? (World - Planet->PlanetCenter).GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector) * Planet->PlanetRadius : FVector(World.X, World.Y, 0.0));
		}
		// An arc of up to 160 degrees reaches its radius x tan(80 degrees), under six radii, each way along its corner.
		const double Reach = 6.0 * Radius + 2.0 * Step;
		const double PerRead = Length / Num;
		double Farthest = 0.0;
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			const int32 First = FMath::Max(0, FMath::FloorToInt32((Along[Index] - Reach) / PerRead));
			const int32 Last = FMath::Min(Num, FMath::CeilToInt32((Along[Index] + Reach) / PerRead));
			double Nearest = TNumericLimits<double>::Max();
			for (int32 Segment = First; Segment < Last; ++Segment)
			{
				Nearest = FMath::Min(Nearest, FMath::PointDistToSegmentSquared(Points[Index], Read[Segment], Read[Segment + 1]));
			}
			Farthest = FMath::Max(Farthest, Nearest < TNumericLimits<double>::Max() ? FMath::Sqrt(Nearest) : 0.0);
		}
		int32 FirstVertex = 0;
		for (int32 Index = 0; Index <= Num; ++Index)
		{
			const double At = Length * Index / Num;
			while (FirstVertex + 1 < Points.Num() && Along[FirstVertex + 1] < At - Reach)
			{
				++FirstVertex;
			}
			double Nearest = TNumericLimits<double>::Max();
			for (int32 Vertex = FirstVertex; Vertex + 1 < Points.Num() && Along[Vertex] <= At + Reach; ++Vertex)
			{
				Nearest = FMath::Min(Nearest, FMath::PointDistToSegmentSquared(Read[Index], Points[Vertex], Points[Vertex + 1]));
			}
			Farthest = FMath::Max(Farthest, Nearest < TNumericLimits<double>::Max() ? FMath::Sqrt(Nearest) : 0.0);
		}
		return Farthest;
	}
}

UVoxelTerrainLineComponent::UVoxelTerrainLineComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

USplineComponent* UVoxelTerrainLineComponent::GetSpline() const
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return nullptr;
	}
	TInlineComponentArray<USplineComponent*> Splines(Owner);
	for (USplineComponent* Candidate : Splines)
	{
		if (SplineName.IsNone() || Candidate->GetFName() == SplineName)
		{
			return Candidate;
		}
	}
	return nullptr;
}

int64 UVoxelTerrainLineComponent::GetLineId() const
{
	// The names, not the paths: a PIE world's package is renamed, its level's actors are not, and a client's copy of a
	// level actor carries the server's name.
	const AActor* Owner = GetOwner();
	const FString Key = (Owner ? Owner->GetName() : FString()) + TEXT(".") + GetName();
	const uint32 Hash = FCrc::StrCrc32(*Key);
	return (static_cast<int64>(Order) << 32) | static_cast<int64>(Hash);
}

FVoxelTerrainLineSettings UVoxelTerrainLineComponent::GetLineSettings() const
{
	FVoxelTerrainLineSettings Settings;
	Settings.HeightMode = HeightMode;
	Settings.TerrainOffset = TerrainOffset;
	Settings.SmoothingDistance = SmoothingDistance;
	Settings.MaxGrade = MaxGrade;
	Settings.HalfWidth = HalfWidth;
	Settings.FalloffWidth = FalloffWidth;
	Settings.Strength = Strength;
	Settings.Blend = Blend;
	Settings.Surface = Surface;
	Settings.VertexSpacingVoxels = VertexSpacingVoxels;
	Settings.Id = GetLineId();
	return Settings;
}

bool UVoxelTerrainLineComponent::BuildLineParams(const UVoxelChunkManager& Manager, FVoxelConditioningLineParams& OutParams, FVoxelTerrainLineBends* OutBends) const
{
	return BuildLineParamsFromSpline(GetSpline(), Manager, GetLineSettings(), OutParams, OutBends);
}

bool UVoxelTerrainLineComponent::BuildLineParamsFromSpline(const USplineComponent* Spline, const UVoxelChunkManager& Manager, const FVoxelTerrainLineSettings& Settings,
	FVoxelConditioningLineParams& OutParams, FVoxelTerrainLineBends* OutBends)
{
	using namespace VoxelTerrainLinePrivate;
	using FShape = FVoxelConditioningLineShape;
	const UVoxelWorldConfiguration* Configuration = Manager.GetConfiguration();
	const IVoxelWorldMode* WorldMode = Manager.GetWorldMode();
	if (!Spline || !Configuration || !WorldMode || !Manager.IsInitialized())
	{
		return false;
	}
	const double Length = Spline->GetSplineLength();
	if (Length <= UE_KINDA_SMALL_NUMBER)
	{
		return false;
	}

	const bool bPlanet = Configuration->WorldMode == EWorldMode::SphericalPlanet;
	const FSphericalPlanetParams PlanetParams = bPlanet ? FVoxelWorldModeFactory::MakePlanetParams(*Configuration) : FSphericalPlanetParams();
	const FVoxelNoiseParams NoiseParams = Configuration->GetNoiseParams();

	// A vertex every Spacing along the spline, the last one on its end. The spline's length is an approximation (a
	// straight run of 3,000 uu reads a fraction of a unit long), which must not add a step.
	const double Spacing = FMath::Max(Settings.VertexSpacingVoxels, 0.25f) * Configuration->VoxelSize;
	const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Length / Spacing - 0.01));
	const double Step = Length / Steps;

	// The natural terrain at a vertex's position: a world Z at (X, Y), or the altitude along a direction on a planet.
	const auto NaturalAt = [&](const FVector& Position) -> double
	{
		return bPlanet
			? static_cast<double>(static_cast<const FSphericalPlanetWorldMode*>(WorldMode)->GetTerrainAltitude(Position, NoiseParams))
			: WorldMode->GetSurfaceOnUpLine(FVector(Position.X, Position.Y, 0.0), NoiseParams).Position.Z;
	};

	// Where each vertex is (world X, Y or a direction), the spline's own height there and the natural terrain's.
	TArray<FVector> Positions;
	TArray<float> SplineHeights;
	TArray<float> Natural;
	TArray<float> HalfWidths;
	Positions.Reserve(Steps + 1);
	SplineHeights.Reserve(Steps + 1);
	Natural.Reserve(Steps + 1);
	HalfWidths.Reserve(Steps + 1);
	for (int32 Index = 0; Index <= Steps; ++Index)
	{
		const float Distance = static_cast<float>(FMath::Min(Index * Step, Length));
		const FVector World = Spline->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
		if (bPlanet)
		{
			const FVector FromCentre = World - PlanetParams.PlanetCenter;
			const FVector Direction = FromCentre.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
			Positions.Add(Direction);
			SplineHeights.Add(static_cast<float>(FromCentre.Size() - PlanetParams.PlanetRadius));
			Natural.Add(static_cast<const FSphericalPlanetWorldMode*>(WorldMode)->GetTerrainAltitude(Direction, NoiseParams));
		}
		else
		{
			Positions.Add(FVector(World.X, World.Y, 0.0));
			SplineHeights.Add(static_cast<float>(World.Z));
			Natural.Add(static_cast<float>(WorldMode->GetSurfaceOnUpLine(FVector(World.X, World.Y, 0.0), NoiseParams).Position.Z));
		}
		HalfWidths.Add(Settings.HalfWidth * FMath::Abs(static_cast<float>(Spline->GetScaleAtDistanceAlongSpline(Distance).Y)));
	}

	// The targets.
	TArray<float> Targets;
	if (Settings.HeightMode == EVoxelTerrainLineHeight::SplineHeight)
	{
		Targets = MoveTemp(SplineHeights);
	}
	else
	{
		// The natural height averaged over the smoothing distance each way along the line, then held to the grade from both
		// ends, then the offset. The average reaches as far back as forward, so it narrows toward each end and the line
		// meets the ground there: an average of what lies ahead alone would start a road on a rise with a mound.
		const int32 Window = FMath::Max(0, FMath::RoundToInt32(Settings.SmoothingDistance / Step));
		Targets.SetNumUninitialized(Natural.Num());
		for (int32 Index = 0; Index < Natural.Num(); ++Index)
		{
			const int32 Reach = FMath::Min3(Window, Index, Natural.Num() - 1 - Index);
			double Sum = 0.0;
			for (int32 Other = Index - Reach; Other <= Index + Reach; ++Other)
			{
				Sum += Natural[Other];
			}
			Targets[Index] = static_cast<float>(Sum / (2 * Reach + 1));
		}
		if (Settings.MaxGrade > 0.0f)
		{
			const float MaxStep = static_cast<float>(Settings.MaxGrade * Step);
			for (int32 Index = 1; Index < Targets.Num(); ++Index)
			{
				Targets[Index] = FMath::Clamp(Targets[Index], Targets[Index - 1] - MaxStep, Targets[Index - 1] + MaxStep);
			}
			for (int32 Index = Targets.Num() - 2; Index >= 0; --Index)
			{
				Targets[Index] = FMath::Clamp(Targets[Index], Targets[Index + 1] - MaxStep, Targets[Index + 1] + MaxStep);
			}
		}
		for (float& Target : Targets)
		{
			Target += Settings.TerrainOffset;
		}
	}

	OutParams = FVoxelConditioningLineParams();
	OutParams.Vertices.Reserve(Positions.Num());
	for (int32 Index = 0; Index < Positions.Num(); ++Index)
	{
		OutParams.Vertices.Add(FVoxelConditioningLineVertex(Positions[Index], Targets[Index], HalfWidths[Index]));
	}
	OutParams.FalloffWidth = Settings.FalloffWidth;
	OutParams.Strength = Settings.Strength;
	OutParams.Blend = Settings.Blend;
	OutParams.Id = Settings.Id;
	OutParams.bRadial = bPlanet;
	OutParams.Surface = Settings.Surface;

	// The bends (D134). The line above is the spline's, as sampled: when it keeps the bend bound it is the line, vertex
	// for vertex. When it does not, the terrain would step on the inside of a bend, and the line is made again below.
	FVoxelTerrainLineBends Bends;
	const double VoxelSize = Configuration->VoxelSize;
	const double PlanetRadius = bPlanet ? PlanetParams.PlanetRadius : 0.0;
	const FVoxelConditioningLineRef Sampled = FVoxelConditioningLine::Make(OutParams);
	Bends.DrawnBendRatio = GetBendRatio(*Sampled, VoxelSize, PlanetRadius);
	Bends.BendRatio = Bends.DrawnBendRatio;
	Bends.bBrokeBound = Bends.DrawnBendRatio > 1.0;
	if (Bends.bBrokeBound)
	{
		const bool bFollow = Settings.HeightMode == EVoxelTerrainLineHeight::FollowTerrain;

		// The sampled points where distances are measured: world X and Y on a heightmap world, the points on the datum
		// sphere on a planet (the rounding is done on these, before they become directions again).
		TArray<FVector> Drawn;
		Drawn.Reserve(Positions.Num());
		for (const FVector& Position : Positions)
		{
			Drawn.Add(bPlanet ? Position * PlanetRadius : Position);
		}

		// A try: the vertices' points, and for each how far along the spline its half width and (at the spline's height)
		// its target are read. The first try is the sampled points themselves.
		TArray<FVector> Points = Drawn;
		TArray<double> Along;
		Along.Reserve(Points.Num());
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			Along.Add(FMath::Min(Index * Step, Length));
		}
		FVoxelConditioningLineParams Try = OutParams;
		// The try as a line, and how it stands against the bound. A line that follows the terrain is judged level here,
		// by its turns alone: its profile is made afterwards, and holds the grade to whatever turn is left.
		const auto JudgeTry = [&]()
		{
			Try.Vertices.Reset(Points.Num());
			for (int32 Index = 0; Index < Points.Num(); ++Index)
			{
				const float Distance = static_cast<float>(Along[Index]);
				float Target = 0.0f;
				if (!bFollow)
				{
					const FVector World = Spline->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
					Target = bPlanet ? static_cast<float>((World - PlanetParams.PlanetCenter).Size() - PlanetParams.PlanetRadius) : static_cast<float>(World.Z);
				}
				Try.Vertices.Add(FVoxelConditioningLineVertex(bPlanet ? Points[Index].GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector) : Points[Index], Target,
					Settings.HalfWidth * FMath::Abs(static_cast<float>(Spline->GetScaleAtDistanceAlongSpline(Distance).Y))));
			}
			return GetBendRatio(*FVoxelConditioningLine::Make(Try), VoxelSize, PlanetRadius);
		};

		// Round the corners (G225): an arc fitted into each corner that turns more than an arc of the radius does, the
		// radius the reach asks for with the shape functions' margin (FitCornerArcs; an arc turns evenly, where the mean of
		// the points around a corner that L5 took turns most at its middle). A line that follows the terrain is then within
		// the bound: its profile holds the grade to the turn. At the spline's own height the turn must also fit the
		// spline's grade there, so a try still outside the bound takes a wider radius, by as much as it was outside, up
		// to three times the reach's radius; a line still outside the bound there (a corner on a steep grade, a hairpin,
		// an S too tight for two arcs) takes the try that came nearest and keeps its warning. The sampled points are
		// tried first as they are: a line that follows the terrain and only steps on its grade keeps them, and gets the
		// flatter profile alone.
		const double BaseRadius = FShape::GetCornerRadius(Spacing, FShape::GetMaxTurnForReach(Spacing, Sampled->GetHeightReach()));
		const double MaxRadius = 3.0 * BaseRadius;
		constexpr int32 MaxTries = 8;
		double Ratio = JudgeTry();
		double BestRatio = Ratio;
		double BestRadius = 0.0;
		TArray<FVector> BestPoints = Points;
		TArray<double> BestAlong = Along;
		TArray<double> Places;
		double Radius = BaseRadius;
		for (int32 Attempt = 0; Ratio > 1.0 && Attempt < MaxTries; ++Attempt)
		{
			const FShape::FCornerArcs Arcs = FShape::FitCornerArcs(Drawn, Spacing, Radius, Points, &Places);
			if (Arcs.NumArcs == 0)
			{
				// No corner could take an arc (a hairpin; legs too short for one on both sides): a wider radius fits none either.
				break;
			}
			if (bPlanet)
			{
				// The arc of a corner on the sphere is drawn in the plane of its two legs, a little under the sphere.
				for (FVector& Point : Points)
				{
					Point = Point.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector) * PlanetRadius;
				}
			}
			// A vertex's place along the sampled points is its place along the spline, in steps.
			Along.SetNumUninitialized(Points.Num());
			for (int32 Index = 0; Index < Points.Num(); ++Index)
			{
				Along[Index] = FMath::Min(Places[Index] * Step, Length);
			}
			Along[0] = 0.0;
			Along.Last() = Length;
			Ratio = JudgeTry();
			if (Ratio < BestRatio)
			{
				BestRatio = Ratio;
				BestRadius = Radius;
				BestPoints = Points;
				BestAlong = Along;
			}
			if (Radius >= MaxRadius)
			{
				break;
			}
			Radius = FMath::Min(MaxRadius, Radius * FMath::Max(1.25, Ratio / FShape::BoundMargin));
		}
		// The try that came nearest the bound: the last one when it is within it, else the nearest (the sampled points
		// themselves when no arc helped).
		Points = MoveTemp(BestPoints);
		Along = MoveTemp(BestAlong);
		JudgeTry();
		if (BestRadius > 0.0)
		{
			Bends.CornerRadius = BestRadius;
			Bends.FarthestFromSpline = GetFarthestFromSpline(*Spline, Points, Along, Step, BestRadius, bPlanet ? &PlanetParams : nullptr);
		}

		if (bFollow)
		{
			// The profile again, on the vertices as they are now: the component's own (the ground averaged along the line
			// and held to the grade), and at a vertex that turns held also to the grade the bend bound leaves there.
			TArray<double> Ground;
			Ground.Reserve(Try.Vertices.Num());
			float MaxHalfWidth = 0.0f;
			for (const FVoxelConditioningLineVertex& Vertex : Try.Vertices)
			{
				Ground.Add(NaturalAt(Vertex.Position));
				MaxHalfWidth = FMath::Max(MaxHalfWidth, Vertex.HalfWidth);
			}
			FShape::FProfileParams Profile;
			Profile.SmoothingDistance = Settings.SmoothingDistance;
			Profile.MaxGrade = Settings.MaxGrade;
			Profile.HalfWidth = MaxHalfWidth;
			Profile.FalloffWidth = Settings.FalloffWidth;
			Profile.Strength = FMath::Clamp(Settings.Strength, 0.0f, 1.0f);
			Profile.VoxelSize = VoxelSize;
			TArray<double> Flattened;
			FShape::MakeProfile(Points, Ground, Profile, Flattened);
			for (int32 Index = 0; Index < Try.Vertices.Num(); ++Index)
			{
				Try.Vertices[Index].TargetHeight = static_cast<float>(Flattened[Index] + Settings.TerrainOffset);
			}
			Bends.bProfileFlattened = true;
		}
		Bends.BendRatio = GetBendRatio(*FVoxelConditioningLine::Make(Try), VoxelSize, PlanetRadius);
		OutParams.Vertices = MoveTemp(Try.Vertices);
	}
	if (OutBends)
	{
		*OutBends = Bends;
	}
	return true;
}

FString UVoxelTerrainLineComponent::DescribeBends(const FVoxelTerrainLineBends& Bends)
{
	if (!Bends.WasRounded() && !Bends.bProfileFlattened)
	{
		return FString();
	}
	FString Done = FString::Printf(TEXT("the spline breaks the bend bound (its sharpest bend turns %.1f times what the bound allows there): "), Bends.DrawnBendRatio);
	if (Bends.WasRounded())
	{
		Done += FString::Printf(TEXT("the line's corners are rounded to arcs of %.0f uu radius, so the line runs up to %.0f uu from its spline, %s"),
			Bends.CornerRadius, Bends.FarthestFromSpline,
			Bends.bProfileFlattened ? TEXT("and its profile is flatter through the bends") : TEXT("at the spline's heights along it"));
	}
	else
	{
		Done += TEXT("the line runs where the spline does, and its profile is flatter through the bends");
	}
	if (Bends.BendRatio > 1.0)
	{
		Done += FString::Printf(TEXT("; the line still breaks the bound (its sharpest bend turns %.1f times what the bound allows): a corner's radius is at most three times what the reach asks for, and a hairpin or an S too tight for two arcs is left as drawn"), Bends.BendRatio);
	}
	return Done;
}

void UVoxelTerrainLineComponent::BeginPlay()
{
	Super::BeginPlay();

	// The voxel world started before this actor began play: register now (every level actor begins play before the
	// world's first tick, so before the chunk manager's first generation request). Else wait for it to start.
	if (UVoxelChunkManager* Manager = UVoxelChunkManager::Find(GetWorld()))
	{
		RegisterLine(*Manager);
	}
	else
	{
		InitializedHandle = UVoxelChunkManager::OnInitialized.AddUObject(this, &UVoxelTerrainLineComponent::HandleChunkManagerInitialized);
	}
}

void UVoxelTerrainLineComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (InitializedHandle.IsValid())
	{
		UVoxelChunkManager::OnInitialized.Remove(InitializedHandle);
		InitializedHandle.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

void UVoxelTerrainLineComponent::HandleChunkManagerInitialized(UVoxelChunkManager& Manager)
{
	if (Manager.GetWorld() != GetWorld())
	{
		return;
	}
	UVoxelChunkManager::OnInitialized.Remove(InitializedHandle);
	InitializedHandle.Reset();
	RegisterLine(Manager);
}

void UVoxelTerrainLineComponent::RegisterLine(UVoxelChunkManager& Manager)
{
	if (RegisteredLine.IsValid())
	{
		return;
	}
	const FString Name = GetOwner() ? GetOwner()->GetName() : GetName();
	FVoxelConditioningLineParams Params;
	FVoxelTerrainLineBends Bends;
	if (!BuildLineParams(Manager, Params, &Bends))
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("Terrain line %s: no line was made (%s)."), *Name,
			GetSpline() ? TEXT("its spline has no length") : TEXT("its actor has no spline component, or none with the name asked for"));
		return;
	}
	const FVoxelConditioningLineRef Line = FVoxelConditioningLine::Make(Params);
	RegisteredLine = Line;
	LineBends = Bends;

	// What was done about the spline's bends first, then the limits the line as made breaks (the engine's own check: a
	// bend the rounding could not bring within the bound is named there).
	LineWarnings.Reset();
	if (FString Done = DescribeBends(Bends); !Done.IsEmpty())
	{
		LineWarnings.Add(MoveTemp(Done));
	}
	LineWarnings.Append(Manager.GetConditioningLineWarnings(*Line));

	float MinHalfWidth = TNumericLimits<float>::Max(), MaxHalfWidth = 0.0f;
	float MinTarget = TNumericLimits<float>::Max(), MaxTarget = TNumericLimits<float>::Lowest();
	for (const FVoxelConditioningLineVertex& Vertex : Params.Vertices)
	{
		MinHalfWidth = FMath::Min(MinHalfWidth, Vertex.HalfWidth);
		MaxHalfWidth = FMath::Max(MaxHalfWidth, Vertex.HalfWidth);
		MinTarget = FMath::Min(MinTarget, Vertex.TargetHeight);
		MaxTarget = FMath::Max(MaxTarget, Vertex.TargetHeight);
	}
	UE_LOG(LogVoxelStreaming, Log, TEXT("Terrain line %s: %d vertices over %.0f uu of spline, %s, half width %.0f to %.0f, target %.0f to %.0f, id %lld."),
		*Name, Params.Vertices.Num(), GetSpline()->GetSplineLength(),
		HeightMode == EVoxelTerrainLineHeight::FollowTerrain ? TEXT("following the terrain") : TEXT("at the spline's height"),
		MinHalfWidth, MaxHalfWidth, MinTarget, MaxTarget, Params.Id);
	for (const FString& Warning : LineWarnings)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("Terrain line %s: %s."), *Name, *Warning);
	}
	Manager.AddConditioningLine(Line, false);
}

AVoxelTerrainLineActor::AVoxelTerrainLineActor()
{
	PrimaryActorTick.bCanEverTick = false;

	Spline = CreateDefaultSubobject<USplineComponent>(TEXT("Spline"));
	SetRootComponent(Spline);
	// A line long enough to see: the default spline is 100 uu.
	Spline->SetSplinePoints({ FVector::ZeroVector, FVector(3000.0, 0.0, 0.0) }, ESplineCoordinateSpace::Local);

	TerrainLine = CreateDefaultSubobject<UVoxelTerrainLineComponent>(TEXT("TerrainLine"));
}
