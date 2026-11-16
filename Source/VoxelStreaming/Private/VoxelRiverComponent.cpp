// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelRiverComponent.h"
#include "VoxelStreaming.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "Components/SplineComponent.h"
#include "Misc/Crc.h"
#include <limits>

namespace VoxelRiverPrivate
{
	/**
	 * The level of the body a place lies in, in a line's terms (a world Z, an altitude on a planet): the sea or a lake
	 * where the ground lies under its level, a river reach wherever its footprint covers the place (its bed will be
	 * there); the highest, where several do. Every body is asked, not the one the world's sample answers with: under a
	 * river that has come down to the sea's level the sample is the sea's.
	 * @param Water The water levels gathered over the river
	 * @param Place World (X, Y), or on a planet the unit direction from the centre
	 * @param GroundHeight The natural ground's height there
	 * @return The level, or NaN where the place lies in no body
	 */
	double GetBodyLevelAt(const FVoxelWaterLevels& Water, const FVector& Place, double GroundHeight)
	{
		const FVoxelWaterFrame& Frame = Water.GetFrame();
		const double LevelBase = Frame.bRadial ? Frame.DatumRadius : 0.0;
		double Best = std::numeric_limits<double>::quiet_NaN();
		const auto Take = [&Best](double Level) { Best = FMath::IsNaN(Best) ? Level : FMath::Max(Best, Level); };
		if (Water.GetSea().bEnabled)
		{
			const FVector GroundPoint = Frame.bRadial ? Frame.Center + Place * (Frame.DatumRadius + GroundHeight) : FVector(Place.X, Place.Y, GroundHeight);
			const double Depth = Water.GetSea().GetDepth(GroundPoint);
			if (Depth > 0.0)
			{
				Take(GroundHeight + Depth);
			}
		}
		for (const FVoxelWaterBody& Body : Water.GetBodies())
		{
			if (Body.Kind == EVoxelWaterBodyKind::Lake && Body.Lake.IsValid())
			{
				if ((Frame.bRadial ? Body.Lake->CoversDirection(Place) : Body.Lake->Covers(Place.X, Place.Y)) && GroundHeight + LevelBase < Body.Level)
				{
					Take(Body.Level - LevelBase);
				}
			}
			else if (Body.Kind == EVoxelWaterBodyKind::Reach && Body.Reach.IsValid())
			{
				FVoxelWaterReachSample Sample;
				if (Frame.bRadial ? Body.Reach->SampleOnSphere(Place, Frame.DatumRadius, Sample) : Body.Reach->SampleAt(Place.X, Place.Y, Sample))
				{
					Take(Sample.SurfaceLevel - LevelBase);
				}
			}
		}
		return Best;
	}
}

UVoxelRiverComponent::UVoxelRiverComponent()
{
	// A river's bed keeps caves from under it, as the plan's rivers do (8 voxels).
	Surface.MinCaveDepth = 8.0f;
}

int64 UVoxelRiverComponent::GetLineId() const
{
	// A terrain line's id: the order in the high half, a hash of the names in the low half.
	return (static_cast<int64>(Order) << 32) | static_cast<int64>(FCrc::StrCrc32(*GetBodyKey()));
}

bool UVoxelRiverComponent::BuildRiver(const UVoxelChunkManager& Manager, FVoxelRiverBuild& Out) const
{
	using FShape = FVoxelConditioningLineShape;
	Out = FVoxelRiverBuild();
	const UVoxelWorldConfiguration* Configuration = Manager.GetConfiguration();
	if (!Configuration || !Manager.IsInitialized())
	{
		return false;
	}

	// The vertices: the spline sampled as a terrain line samples it, a lower-only line with this river's widths, so a
	// spline that breaks the bend bound has its corners rounded as a road's are. At the spline's height the targets it
	// returns are the spline's own heights along the line as made; following the terrain they are not read.
	FVoxelTerrainLineSettings Settings;
	Settings.HeightMode = HeightMode;
	Settings.TerrainOffset = 0.0f;
	Settings.SmoothingDistance = SmoothingDistance;
	Settings.MaxGrade = 0.0f;
	Settings.HalfWidth = HalfWidth;
	Settings.FalloffWidth = BankWidth;
	Settings.Strength = 1.0f;
	Settings.Blend = EVoxelConditioningLineBlend::LowerOnly;
	Settings.Surface = Surface;
	Settings.VertexSpacingVoxels = VertexSpacingVoxels;
	Settings.Id = GetLineId();
	FVoxelConditioningLineParams Line;
	if (!UVoxelTerrainLineComponent::BuildLineParamsFromSpline(GetSpline(), Manager, Settings, Line, &Out.Bends) || Line.Vertices.Num() < 2)
	{
		return false;
	}
	const int32 Num = Line.Vertices.Num();
	Out.NumSampled = Num;
	const bool bPlanet = Line.bRadial;
	const FVoxelWaterFrame Frame = Manager.GetWaterLevels().GetFrame();
	const double VoxelSize = Configuration->VoxelSize;
	// A line's height is a world Z or an altitude; a water level is a world Z or a distance from the planet centre.
	const double LevelBase = bPlanet ? Frame.DatumRadius : 0.0;

	// The vertices where distances are measured: world X and Y, or the points on the datum sphere.
	TArray<FVector> Points;
	Points.Reserve(Num);
	for (const FVoxelConditioningLineVertex& Vertex : Line.Vertices)
	{
		Points.Add(bPlanet ? Vertex.Position * Frame.DatumRadius : Vertex.Position);
	}

	// The water the river meets: the sea, and the bodies the world holds over the river's length.
	FVoxelWaterLevels Water;
	if (bPlanet)
	{
		FVector Middle = FVector::ZeroVector;
		for (const FVoxelConditioningLineVertex& Vertex : Line.Vertices)
		{
			Middle += Vertex.Position;
		}
		Middle = Middle.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, Line.Vertices[0].Position);
		double Angle = 0.0;
		for (const FVoxelConditioningLineVertex& Vertex : Line.Vertices)
		{
			Angle = FMath::Max(Angle, FMath::Acos(FMath::Clamp(FVector::DotProduct(Middle, Vertex.Position), -1.0, 1.0)));
		}
		Water = Manager.GatherWaterLevelsOnSphere(Middle, Angle + (HalfWidth + BankWidth + VoxelSize) / FMath::Max(Frame.DatumRadius, 1.0), EVoxelConditioningGather::Complete);
	}
	else
	{
		FBox Box(ForceInit);
		for (const FVector& Point : Points)
		{
			Box += Point;
		}
		Box = Box.ExpandBy(FVector(HalfWidth + BankWidth + VoxelSize, HalfWidth + BankWidth + VoxelSize, 0.0));
		Box.Min.Z = -1.0e9;
		Box.Max.Z = 1.0e9;
		Water = Manager.GatherWaterLevels(Box, EVoxelConditioningGather::Complete);
	}

	// At each vertex: the natural ground on the line, and the level of the body the vertex lies in, if any.
	TArray<double> Ground;
	TArray<double> Heights;
	TArray<double> BodyLevels;
	Ground.Reserve(Num);
	BodyLevels.Reserve(Num);
	const bool bFollow = HeightMode == EVoxelTerrainLineHeight::FollowTerrain;
	if (!bFollow)
	{
		Heights.Reserve(Num);
	}
	bool bAnyBody = false;
	float MaxHalfWidth = 0.0f;
	for (int32 Index = 0; Index < Num; ++Index)
	{
		const FVoxelConditioningLineVertex& Vertex = Line.Vertices[Index];
		MaxHalfWidth = FMath::Max(MaxHalfWidth, Vertex.HalfWidth);
		const double Height = GetNaturalHeight(Manager, Vertex.Position);
		Ground.Add(Height);
		if (!bFollow)
		{
			Heights.Add(Vertex.TargetHeight);
		}
		const double BodyLevel = VoxelRiverPrivate::GetBodyLevelAt(Water, Vertex.Position, Height);
		BodyLevels.Add(BodyLevel);
		bAnyBody |= !FMath::IsNaN(BodyLevel);
	}
	if (!bAnyBody)
	{
		BodyLevels.Reset();
	}

	FShape::FRiverProfileParams Params;
	Params.Depth = Depth;
	Params.SurfaceUnderGround = SurfaceBelowGround;
	Params.BankMargin = BankMargin;
	Params.SmoothingDistance = SmoothingDistance;
	Params.HeadLength = HeadLength;
	Params.EndLength = HeadLength;
	Params.HalfWidth = MaxHalfWidth;
	Params.FalloffWidth = BankWidth;
	Params.VoxelSize = VoxelSize;
	if (Water.GetSea().bEnabled)
	{
		// The sea's level in a line's terms, from the first vertex's ground: a height and its depth under the sea.
		const FVoxelConditioningLineVertex& First = Line.Vertices[0];
		const FVector GroundPoint = bPlanet ? Frame.Center + First.Position * (Frame.DatumRadius + Ground[0]) : FVector(First.Position.X, First.Position.Y, Ground[0]);
		Params.MinSurface = Ground[0] + Water.GetSea().GetDepth(GroundPoint) - Params.BodyTuck;
	}

	// The lowest natural ground of each vertex's two banks: from the floor's edge out to the footprint's, a voxel
	// apart. Where a bank lies in another body (the shore of the sea beside a mouth, a lake's, a river's) that body's
	// surface is the bank: the river may stand as high as the water beside it. The floor narrows at a head and at an
	// end on land, so the banks are read where the river will have them: the taper depends on the vertices' places and
	// bodies alone, and a first profile over level banks gives it.
	TArray<double> BankGround;
	BankGround.Reserve(Num);
	{
		const TArray<double> Taper = FShape::MakeRiverProfile(Points, Ground, Ground, Heights, BodyLevels, Params).Taper;
		const int32 Steps = FMath::Max(1, FMath::CeilToInt32(BankWidth / FMath::Max(VoxelSize, 1.0)));
		for (int32 Index = 0; Index < Num; ++Index)
		{
			const FVoxelConditioningLineVertex& Vertex = Line.Vertices[Index];
			// The way across the river here, along the surface.
			const FVector Up = bPlanet ? Vertex.Position : FVector::UpVector;
			FVector AlongLine = Points[FMath::Min(Index + 1, Num - 1)] - Points[FMath::Max(Index - 1, 0)];
			AlongLine -= Up * FVector::DotProduct(AlongLine, Up);
			const FVector Across = FVector::CrossProduct(Up, AlongLine.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::ForwardVector));
			const double Floor = Vertex.HalfWidth * (Taper.IsValidIndex(Index) ? Taper[Index] : 1.0);
			double Lowest = TNumericLimits<double>::Max();
			for (int32 Step = 0; Step <= Steps; ++Step)
			{
				const double Distance = Floor + BankWidth * Step / Steps;
				for (const double Side : { -1.0, 1.0 })
				{
					FVector Place = Points[Index] + Across * (Side * Distance);
					if (bPlanet)
					{
						Place = Place.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, Up);
					}
					double Bank = GetNaturalHeight(Manager, Place);
					const double BodyLevel = VoxelRiverPrivate::GetBodyLevelAt(Water, Place, Bank);
					if (!FMath::IsNaN(BodyLevel))
					{
						Bank = FMath::Max(Bank, BodyLevel + BankMargin);
					}
					Lowest = FMath::Min(Lowest, Bank);
				}
			}
			BankGround.Add(Lowest);
		}
	}

	// The profile.
	Out.Profile = FShape::MakeRiverProfile(Points, Ground, BankGround, Heights, BodyLevels, Params);
	const FShape::FRiverProfile& Profile = Out.Profile;
	if (Profile.Num < 2)
	{
		return false;
	}

	// The bed and the river reach, from the vertices kept. A line's half width must be over nothing, so the bed's is a
	// unit where the river's own is nothing (a head, an end on land); the footprint there is the bank alone.
	Out.Bed = Line;
	Out.Bed.Vertices.Reset(Profile.Num);
	Out.Vertices.Reset(Profile.Num);
	for (int32 Index = Profile.First; Index < Profile.First + Profile.Num; ++Index)
	{
		const FVoxelConditioningLineVertex& Vertex = Line.Vertices[Index];
		const double Half = Vertex.HalfWidth * Profile.Taper[Index];
		Out.Bed.Vertices.Add(FVoxelConditioningLineVertex(Vertex.Position, static_cast<float>(Profile.Bed[Index]), static_cast<float>(FMath::Max(Half, 1.0))));
		Out.Vertices.Add(FVoxelWaterReachVertex(Vertex.Position, Profile.Bed[Index] + LevelBase, Profile.Surface[Index] + LevelBase, Half + BankWidth));
	}
	return true;
}

bool UVoxelRiverComponent::AddBody(UVoxelChunkManager& Manager, FVoxelWaterBody& OutBody, TArray<FString>& OutWarnings)
{
	const FString Name = GetBodyName();
	FVoxelRiverBuild Build;
	if (!BuildRiver(Manager, Build))
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("River %s: no river was made (%s)."), *Name,
			GetSpline() ? TEXT("its spline has no length") : TEXT("its actor has no spline component, or none with the name asked for"));
		return false;
	}
	const FVoxelConditioningLineRef Bed = FVoxelConditioningLine::Make(Build.Bed);
	const FVoxelWaterFrame Frame = Manager.GetWaterLevels().GetFrame();
	const TSharedRef<const FVoxelWaterReach> Reach = FVoxelWaterReach::Make(Build.Vertices, Build.Bed.bRadial, FlowSpeed);
	if (Reach->NumSegments() < 1)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("River %s: no river was made (its vertices are one place)."), *Name);
		return false;
	}
	OutBody = FVoxelWaterBody::MakeReach(GetWaterBodyId(), Reach, Frame);
	RegisteredBed = Bed;
	RiverBuild = MoveTemp(Build);
	const FVoxelConditioningLineShape::FRiverProfile& Profile = RiverBuild.Profile;
	const bool bFollow = HeightMode == EVoxelTerrainLineHeight::FollowTerrain;
	const double VoxelSize = Manager.GetConfiguration()->VoxelSize;

	// What was done about the spline's bends, then where the river is not what was drawn, then the limits its bed
	// breaks (the engine's own check of a line).
	if (FString Done = UVoxelTerrainLineComponent::DescribeBends(RiverBuild.Bends); !Done.IsEmpty())
	{
		OutWarnings.Add(MoveTemp(Done));
	}
	if (Profile.NumRises > 0)
	{
		OutWarnings.Add(FString::Printf(TEXT("the spline rises downstream at %d of its %d vertices, by up to %.0f uu: the river's surface is lowered there, since water does not run uphill (draw the spline from the head down to the mouth, or let the river follow the terrain)"),
			Profile.NumRises, RiverBuild.NumSampled, Profile.MostRise));
	}
	if (!bFollow && Profile.NumLowBanks > 0)
	{
		OutWarnings.Add(FString::Printf(TEXT("at %d of its %d vertices the spline is not %.0f uu under the ground of both banks: the surface is lowered there, by up to %.0f uu, and from there on, so the water stays between its banks"),
			Profile.NumLowBanks, RiverBuild.NumSampled, BankMargin, Profile.MostLowBank));
	}
	if (!Profile.bEndsInBody)
	{
		OutWarnings.Add(FString::Printf(TEXT("the river ends on land (its last vertex lies in no sea, lake or river): it thins to nothing over its last %.0f uu. Run the spline on into the water it should reach"),
			HeadLength));
	}
	const bool bTapered = (!Profile.bStartsInBody || !Profile.bEndsInBody) && HeadLength > 0.0f;
	for (FString& Warning : Manager.GetConditioningLineWarnings(*Bed))
	{
		// A head and an end on land narrow the bed, and its band with it, to nothing on purpose.
		if (bTapered && HalfWidth >= VoxelSize && (Warning.Contains(TEXT("the levelled core is")) || Warning.Contains(TEXT("the band is"))))
		{
			continue;
		}
		OutWarnings.Add(MoveTemp(Warning));
	}

	const double LevelBase = RiverBuild.Bed.bRadial ? Frame.DatumRadius : 0.0;
	UE_LOG(LogVoxelStreaming, Log,
		TEXT("River %s: %d vertices over %.0f uu of spline (%d sampled), %s; surface %.0f down to %.0f, the bed up to %.0f under the ground, %s, %s; %d lowered for a bank (up to %.0f), %d beds deepened through a bend (up to %.0f); %.0f uu/s; body %u, line %lld."),
		*Name, Profile.Num, GetSpline()->GetSplineLength(), RiverBuild.NumSampled,
		bFollow ? TEXT("following the terrain") : TEXT("at the spline's height"),
		Reach->GetMaxLevel() - LevelBase, Reach->GetMinLevel() - LevelBase, Profile.DeepestCut,
		Profile.bStartsInBody ? TEXT("its head in another body") : TEXT("a head"),
		Profile.bEndsInBody ? TEXT("a mouth in another body") : TEXT("an end on land"),
		Profile.NumLowBanks, Profile.MostLowBank, Profile.NumBendHolds, Profile.MostBendHold, FlowSpeed, OutBody.Id, Bed->GetId());

	// The bed first: the body's announcement redraws what reads the generated ground.
	Manager.AddConditioningLine(Bed, false);
	Manager.AddWaterBody(OutBody);
	return true;
}

AVoxelRiverActor::AVoxelRiverActor()
{
	PrimaryActorTick.bCanEverTick = false;

	Spline = CreateDefaultSubobject<USplineComponent>(TEXT("Spline"));
	SetRootComponent(Spline);
	// A river long enough to see: the default spline is 100 uu.
	Spline->SetSplinePoints({ FVector::ZeroVector, FVector(6000.0, 0.0, 0.0) }, ESplineCoordinateSpace::Local);

	River = CreateDefaultSubobject<UVoxelRiverComponent>(TEXT("River"));
}
