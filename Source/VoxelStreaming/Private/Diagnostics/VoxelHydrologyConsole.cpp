// Copyright Daniel Raquel. All Rights Reserved.

// The generated hydrology's survey (Documentation/HYDROLOGY_PLAN.md, H7): what the generator makes over a window of a
// play world, checked against the rules that keep a river watertight, with a digest two machines compare.

#include "VoxelChunkManager.h"
#include "VoxelStreaming.h"
#include "Diagnostics/VoxelConsoleHelpers.h"
#include "VoxelHydrologyNetwork.h"
#include "VoxelRiverCourse.h"
#include "VoxelWorldConfiguration.h"
#include "Async/ParallelFor.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"

#if !UE_BUILD_SHIPPING

namespace VoxelHydrologyConsole
{
	const TCHAR* NetName(const UWorld* World)
	{
		switch (World->GetNetMode())
		{
		case NM_DedicatedServer: return TEXT("Server");
		case NM_ListenServer: return TEXT("Listen");
		case NM_Client: return TEXT("Client");
		default: return TEXT("Standalone");
		}
	}

	/**
	 * The survey's window: a square of the world round a place. On a heightmap world its coordinates are world X and Y;
	 * on a planet the square lies on the plane tangent at the place and its coordinates run east and north of it, a
	 * point of it standing for the direction whose datum point lies over it.
	 */
	struct FWindow
	{
		bool bRadial = false;
		FVector Up = FVector::UpVector;
		FVector East = FVector::ForwardVector;
		FVector North = FVector::RightVector;
		double Radius = 1.0;
		FBox2D Box = FBox2D(ForceInit);

		FVector2D ToWindow(const FVector& Place) const
		{
			return bRadial ? FVector2D(Radius * FVector::DotProduct(Place, East), Radius * FVector::DotProduct(Place, North)) : FVector2D(Place.X, Place.Y);
		}

		FVector FromWindow(const FVector2D& Point) const
		{
			if (!bRadial)
			{
				return FVector(Point.X, Point.Y, 0.0);
			}
			const double U = FMath::Clamp(Point.X / Radius, -1.0, 1.0);
			const double V = FMath::Clamp(Point.Y / Radius, -1.0, 1.0);
			return East * U + North * V + Up * FMath::Sqrt(FMath::Max(0.0, 1.0 - U * U - V * V));
		}

		bool IsInside(const FVector& Place) const
		{
			return (!bRadial || FVector::DotProduct(Place, Up) > 0.0) && Box.IsInside(ToWindow(Place));
		}
	};

	void Survey(UVoxelChunkManager& Manager, FVoxelHydrologyNetwork& Network, const FVector& CentrePlace, double InHalf, const FString& Tag)
	{
		const UWorld* World = Manager.GetWorld();
		const UVoxelWorldConfiguration* Config = Manager.GetConfiguration();
		const FVoxelHydrologyGround& Ground = Network.GetGround();
		const FVoxelHydrologySettings& Settings = Network.GetSettings();
		const FVoxelHydrologyGrid Grid(Ground, Settings);
		const double Voxel = FMath::Max(Ground.VoxelSize, 1.0);
		const double BankWidth = Settings.BankWidthVoxels * Voxel;
		const double LevelBase = Ground.GetLevelBase();
		const FVoxelWaterFrame Frame = Ground.GetFrame();

		// The window, and the region that holds it: the box itself, or on a planet the cap round the square.
		FWindow Window;
		Window.bRadial = Ground.bRadial;
		double Half = InHalf;
		FVector2D Centre(CentrePlace.X, CentrePlace.Y);
		FVoxelConditioningRegion SurveyRegion;
		if (Ground.bRadial)
		{
			Window.Radius = FMath::Max(Ground.PlanetRadius, 1.0);
			Window.Up = CentrePlace.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
			FVoxelCubeSphere::GetTangentFrame(Window.Up, Window.East, Window.North);
			// A square on the tangent plane stands for less than half the planet.
			Half = FMath::Min(Half, 0.6 * Window.Radius);
			Centre = FVector2D::ZeroVector;
			SurveyRegion = FVoxelConditioningRegion::OfCap(Window.Up, FMath::Asin(FMath::Min(UE_DOUBLE_SQRT_2 * Half / Window.Radius, 1.0)));
		}
		Window.Box = FBox2D(Centre - FVector2D(Half, Half), Centre + FVector2D(Half, Half));
		const FBox2D Box = Window.Box;
		if (!Ground.bRadial)
		{
			SurveyRegion = FVoxelConditioningRegion::OfBox(Box);
		}
		// A place at a water level, as a world position.
		const auto WorldAt = [&Ground](const FVector& Place, double Level)
		{
			return Ground.bRadial ? Ground.PlanetCenter + Place * Level : FVector(Place.X, Place.Y, Level);
		};

		const double Start = FPlatformTime::Seconds();
		FVoxelHydrologyRegion Region;
		Network.Survey(SurveyRegion, Region);
		const double SurveyMs = (FPlatformTime::Seconds() - Start) * 1000.0;

		// Every body of the window, as one snapshot of the water: what a reach's end is tested against.
		TArray<FVoxelWaterBody> Bodies;
		for (const TSharedRef<const FVoxelHydrologyLake>& Lake : Region.Lakes)
		{
			Bodies.Add(Lake->Body);
		}
		int32 NumReaches = 0;
		int32 NumValleyRivers = 0;
		double ValleyRiverLength = 0.0;
		for (const TSharedRef<const FVoxelHydrologySystem>& System : Region.Systems)
		{
			for (const FVoxelHydrologyReach& Reach : System->Reaches)
			{
				Bodies.Add(Reach.Body);
				++NumReaches;
				if (System->bValleyRiver)
				{
					// A valley river's pieces (the valleys plan's V3): one reach a valley link
					++NumValleyRivers;
					ValleyRiverLength += Reach.Length;
				}
			}
		}
		const FVoxelSeaSurface Sea = Ground.GetSea();

		// The rules (the plan's section 5.3), read back from the records.
		int32 Rises = 0, EndsOnLand = 0, EndsOverBody = 0, BadHeads = 0, LineWarnings = 0, Conflicts = 0;
		double ReachLength = 0.0, DeepestCut = 0.0, LongestRiver = 0.0, LongestReach = 0.0;
		int32 MostFlow = 0;
		FString FirstWarning;
		uint32 Digest = 0;
		const auto Mix = [&Digest](const void* Data, int32 Size) { Digest = FCrc::MemCrc32(Data, Size, Digest); };
		const double Tuck = 10.0;
		for (const TSharedRef<const FVoxelHydrologySystem>& System : Region.Systems)
		{
			DeepestCut = FMath::Max(DeepestCut, System->DeepestCut);
			LongestRiver = FMath::Max(LongestRiver, System->LongestRiver);
			Conflicts += System->NumConflicts;
			if (FirstWarning.IsEmpty() && !System->ConflictNote.IsEmpty())
			{
				FirstWarning = System->ConflictNote;
			}
			for (const FVoxelHydrologyReach& Reach : System->Reaches)
			{
				const TArray<FVoxelWaterReachVertex>& Vertices = Reach.Body.Reach->GetVertices();
				ReachLength += Reach.Length;
				LongestReach = FMath::Max(LongestReach, Reach.Length);
				MostFlow = FMath::Max(MostFlow, Reach.Flow);
				for (int32 Index = 1; Index < Vertices.Num(); ++Index)
				{
					Rises += Vertices[Index].SurfaceLevel > Vertices[Index - 1].SurfaceLevel + 1.0e-6 ? 1 : 0;
				}
				// Rule 4: a head starts from nothing unless it lies in another body.
				if (!Reach.Profile.bStartsInBody && Vertices[0].HalfWidth > BankWidth + 1.0)
				{
					++BadHeads;
				}
				// Rule 3: the last vertex lies in a body (the sea, a lake, another reach) that stands no higher than a tuck
				// over the reach's last surface.
				TArray<FVoxelWaterBody> Others;
				Others.Reserve(Bodies.Num());
				for (const FVoxelWaterBody& Body : Bodies)
				{
					if (Body.Id != Reach.Body.Id)
					{
						Others.Add(Body);
					}
				}
				const FVoxelWaterLevels Water = FVoxelWaterLevels::Make(Sea, Frame, FVoxelWaterLevels::WholeWorld(), MoveTemp(Others));
				const FVoxelWaterReachVertex& Last = Vertices.Last();
				// The body's level comes back as a line's height (an altitude on a planet); the reach's are water levels.
				const double LastGround = Ground.HeightAt(Last.Position);
				const double LastSurface = Last.SurfaceLevel - LevelBase;
				const double Level = FVoxelRiverCourse::GetBodyLevelAt(Water, Last.Position, LastGround);
				if (!Window.IsInside(Last.Position))
				{
					// Its end lies outside the window, and so may the body it ends in.
				}
				else if (FMath::IsNaN(Level))
				{
					++EndsOnLand;
					if (FirstWarning.IsEmpty())
					{
						const FVector2D At = Window.ToWindow(Last.Position);
						FirstWarning = FString::Printf(TEXT("reach %u from cell (%d, %d) ends on land at (%.0f, %.0f): surface %.1f over ground %.1f, %s, %d vertices, flow %d"),
							Reach.Body.Id, Reach.StartCell.X, Reach.StartCell.Y, At.X, At.Y, LastSurface,
							LastGround, Reach.bEndsAtJoin ? TEXT("at a join") : TEXT("not at a join"), Vertices.Num(), Reach.Flow);
					}
				}
				else if (!Reach.bEndsAtJoin && (Level < LastSurface - 1.0e-3 || Level > LastSurface + Tuck + 1.0e-3))
				{
					++EndsOverBody;
					if (FirstWarning.IsEmpty())
					{
						const FVector2D At = Window.ToWindow(Last.Position);
						FVoxelWaterSample Sample;
						Water.Sample(WorldAt(Last.Position, LastGround + LevelBase), Sample);
						// The lake it lies in, when it is one: whether the system's own mouth is a cell of its hollow (else the
						// river came to it by a way its descents did not know: beside the hollow, over its footprint).
						FString LakeNote;
						FString Covering;
						for (const TSharedRef<const FVoxelHydrologyLake>& Lake : Region.Lakes)
						{
							// Every kept lake whose footprint covers the end: two there are two lakes of one hollow.
							if (Ground.bRadial ? Lake->Body.Lake->CoversDirection(Last.Position) : Lake->Body.Lake->Covers(Last.Position.X, Last.Position.Y))
							{
								Covering += FString::Printf(TEXT(" %u at %.1f (pit (%d, %d), closed %d)"), Lake->Body.Id, Lake->WaterLevel, Lake->Pit.X, Lake->Pit.Y, Lake->Closed);
							}
							if (Lake->Body.Id == Sample.BodyId)
							{
								LakeNote = FString::Printf(TEXT("; that lake: closed %d, %d cells, pit (%d, %d), the mouth %s a cell of its hollow, the reach %s as ending in a body"),
									Lake->Closed, Lake->Cells.Num(), Lake->Pit.X, Lake->Pit.Y, Lake->Cells.Contains(System->Mouth) ? TEXT("is") : TEXT("is not"),
									Reach.Profile.bEndsInBody ? TEXT("was made") : TEXT("was not made"));
							}
						}
						FirstWarning = FString::Printf(TEXT("reach %u from cell (%d, %d) ends at (%.0f, %.0f) with its surface at %.1f where the water it lies in stands at %.1f (body %u, kind %d; ground %.1f, sea %.1f), %d vertices, flow %d, mouth (%d, %d)%s"),
							Reach.Body.Id, Reach.StartCell.X, Reach.StartCell.Y, At.X, At.Y, LastSurface, Level, Sample.BodyId, static_cast<int32>(Sample.Kind), LastGround,
							Ground.SeaLevel, Vertices.Num(), Reach.Flow, System->Mouth.X, System->Mouth.Y, *LakeNote);
						FirstWarning += FString::Printf(TEXT("; the lakes whose footprints cover the end:%s"), Covering.IsEmpty() ? TEXT(" none") : *Covering);
					}
				}
				// Rule 8: the bed is a line and keeps a line's limits (the narrow core and band of a head are meant).
				for (const FString& Warning : Manager.GetConditioningLineWarnings(*Reach.Bed))
				{
					if (!Reach.Profile.bStartsInBody && (Warning.Contains(TEXT("the levelled core is")) || Warning.Contains(TEXT("the band is"))))
					{
						continue;
					}
					++LineWarnings;
					if (FirstWarning.IsEmpty())
					{
						FirstWarning = FString::Printf(TEXT("reach %u: %s"), Reach.Body.Id, *Warning);
					}
				}
				const int32 Num = Vertices.Num();
				const int64 Values[5] = { static_cast<int64>(Reach.Body.Id), Num, FMath::RoundToInt64(Vertices[0].SurfaceLevel * 100.0),
					FMath::RoundToInt64(Last.SurfaceLevel * 100.0), Reach.Bed->GetId() };
				Mix(Values, sizeof(Values));
			}
		}
		double LakeArea = 0.0, LargestLake = 0.0, DeepestLake = 0.0;
		for (const TSharedRef<const FVoxelHydrologyLake>& Lake : Region.Lakes)
		{
			LakeArea += Lake->Area;
			LargestLake = FMath::Max(LargestLake, Lake->Area);
			DeepestLake = FMath::Max(DeepestLake, Lake->Depth);
			const int64 Values[3] = { static_cast<int64>(Lake->Body.Id), FMath::RoundToInt64(Lake->WaterLevel * 100.0), Lake->Body.Lake->NumCells() };
			Mix(Values, sizeof(Values));
		}
		const int32 Violations = Rises + EndsOnLand + EndsOverBody + BadHeads + LineWarnings + Conflicts;

		// The picture and the land's share: the natural ground on a grid over the window.
		const int32 N = FMath::Clamp(FMath::CeilToInt32(2.0 * Half / (Settings.CellVoxels * Voxel)), 64, 1536);
		const double Pixel = 2.0 * Half / N;
		TArray<float> Heights;
		Heights.SetNumUninitialized(N * N);
		ParallelFor(N, [&](int32 Y)
		{
			for (int32 X = 0; X < N; ++X)
			{
				Heights[Y * N + X] = static_cast<float>(Ground.HeightAt(Window.FromWindow(FVector2D(Box.Min.X + (X + 0.5) * Pixel, Box.Min.Y + (Y + 0.5) * Pixel))));
			}
		});
		int64 Land = 0;
		float Highest = -MAX_flt;
		for (const float Height : Heights)
		{
			Land += (!Ground.bSea || Height > Ground.SeaLevel) ? 1 : 0;
			Highest = FMath::Max(Highest, Height);
		}
		const double LandKm2 = Land * Pixel * Pixel / 1.0e10;
		const float SeaZ = Ground.bSea ? static_cast<float>(Ground.SeaLevel) : 0.0f;

		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(N * N);
		const auto PixelAt = [&](int32 X, int32 Y) -> FColor& { return Pixels[(N - 1 - Y) * N + X]; };
		for (int32 Y = 0; Y < N; ++Y)
		{
			for (int32 X = 0; X < N; ++X)
			{
				const int32 XI = FMath::Clamp(X, 1, N - 2), YI = FMath::Clamp(Y, 1, N - 2);
				const float Height = Heights[Y * N + X];
				const float Shade = FMath::Clamp(0.5f + ((Heights[YI * N + XI - 1] - Heights[YI * N + XI + 1]) + (Heights[(YI - 1) * N + XI] - Heights[(YI + 1) * N + XI]))
					/ (4.0f * static_cast<float>(Pixel)) * 1.5f, 0.0f, 1.0f);
				const float T = Highest > SeaZ ? FMath::Clamp((Height - SeaZ) / (Highest - SeaZ), 0.0f, 1.0f) : 0.0f;
				FLinearColor Colour = FMath::Lerp(FLinearColor(0.20f, 0.33f, 0.14f), FLinearColor(0.62f, 0.58f, 0.52f), T) * (0.45f + 0.9f * Shade);
				if (Ground.bSea && Height <= SeaZ)
				{
					Colour = FMath::Lerp(FLinearColor(0.10f, 0.30f, 0.52f), FLinearColor(0.02f, 0.07f, 0.20f), FMath::Clamp((SeaZ - Height) / 6000.0f, 0.0f, 1.0f));
				}
				PixelAt(X, Y) = Colour.ToFColorSRGB();
			}
		}
		for (const TSharedRef<const FVoxelHydrologyLake>& Lake : Region.Lakes)
		{
			if (Ground.bRadial)
			{
				// A planet lake's footprint lies in its own tangent plane: each of its cells at the pixel its middle falls on.
				const FVoxelWaterLake& Shape = *Lake->Body.Lake;
				for (int32 CellY = 0; CellY < Shape.GetSize().Y; ++CellY)
				{
					for (int32 CellX = 0; CellX < Shape.GetSize().X; ++CellX)
					{
						if (!Shape.HasCell(CellX, CellY))
						{
							continue;
						}
						const FVector2D P = Window.ToWindow((Shape.GetCellCentre(CellX, CellY, Lake->Body.Level, Ground.PlanetCenter) - Ground.PlanetCenter).GetSafeNormal());
						const int32 X = FMath::FloorToInt32((P.X - Box.Min.X) / Pixel), Y = FMath::FloorToInt32((P.Y - Box.Min.Y) / Pixel);
						if (X >= 0 && Y >= 0 && X < N && Y < N && Heights[Y * N + X] < Lake->WaterLevel)
						{
							PixelAt(X, Y) = FColor(40, 200, 235);
						}
					}
				}
				continue;
			}
			const FBox2D Bounds = Lake->Body.Lake->GetBounds2D();
			const int32 X0 = FMath::Clamp(FMath::FloorToInt32((Bounds.Min.X - Box.Min.X) / Pixel), 0, N - 1), X1 = FMath::Clamp(FMath::FloorToInt32((Bounds.Max.X - Box.Min.X) / Pixel), 0, N - 1);
			const int32 Y0 = FMath::Clamp(FMath::FloorToInt32((Bounds.Min.Y - Box.Min.Y) / Pixel), 0, N - 1), Y1 = FMath::Clamp(FMath::FloorToInt32((Bounds.Max.Y - Box.Min.Y) / Pixel), 0, N - 1);
			for (int32 Y = Y0; Y <= Y1; ++Y)
			{
				for (int32 X = X0; X <= X1; ++X)
				{
					const double WX = Box.Min.X + (X + 0.5) * Pixel, WY = Box.Min.Y + (Y + 0.5) * Pixel;
					if (Lake->Body.Lake->Covers(WX, WY) && Heights[Y * N + X] < Lake->WaterLevel)
					{
						PixelAt(X, Y) = FColor(40, 200, 235);
					}
				}
			}
		}
		for (const TSharedRef<const FVoxelHydrologySystem>& System : Region.Systems)
		{
			for (const FVoxelHydrologyReach& Reach : System->Reaches)
			{
				const FColor Colour = FMath::Lerp(FLinearColor(0.30f, 0.62f, 1.0f), FLinearColor::White, FMath::Clamp(FMath::Log2(static_cast<float>(Reach.Flow)) / 4.0f, 0.0f, 1.0f)).ToFColorSRGB();
				const TArray<FVoxelWaterReachVertex>& Vertices = Reach.Body.Reach->GetVertices();
				for (int32 Index = 0; Index + 1 < Vertices.Num(); ++Index)
				{
					const FVector2D A = Window.ToWindow(Vertices[Index].Position), B = Window.ToWindow(Vertices[Index + 1].Position);
					const int32 Steps = FMath::Max(1, FMath::CeilToInt32(FVector2D::Distance(A, B) / (0.5 * Pixel)));
					for (int32 Step = 0; Step <= Steps; ++Step)
					{
						const FVector2D P = FMath::Lerp(A, B, static_cast<double>(Step) / Steps);
						const int32 X = FMath::FloorToInt32((P.X - Box.Min.X) / Pixel), Y = FMath::FloorToInt32((P.Y - Box.Min.Y) / Pixel);
						if (X >= 0 && Y >= 0 && X < N && Y < N)
						{
							PixelAt(X, Y) = Colour;
						}
					}
				}
			}
		}

		// The numbers, with the reaches and the lakes a script looks for places among.
		FString Json = TEXT("{\n");
		Json += FString::Printf(TEXT(" \"world\": \"%s\", \"net\": \"%s\", \"tag\": \"%s\", \"seed\": %d, \"voxel\": %.1f, \"sea\": %s, \"sea_level\": %.1f,\n"),
			Config ? *Config->GetName() : TEXT(""), NetName(World), *Tag, Ground.Seed, Voxel, Ground.bSea ? TEXT("true") : TEXT("false"), Ground.SeaLevel);
		Json += FString::Printf(TEXT(" \"centre\": [%.0f, %.0f], \"half\": %.0f, \"land_km2\": %.3f, \"land_pct\": %.1f, \"survey_ms\": %.1f,\n"),
			Centre.X, Centre.Y, Half, LandKm2, 100.0 * Land / (static_cast<double>(N) * N), SurveyMs);
		// The frame a script stands a camera in: the up at the window's middle, and east and north there. A planet's
		// places in the lists below are east and north of the middle; every "_world" is a world position at the water.
		Json += FString::Printf(TEXT(" \"radial\": %s, \"blocks\": %s, \"planet_radius\": %.1f, \"planet_centre\": [%.1f, %.1f, %.1f], \"up\": [%.6f, %.6f, %.6f], \"east\": [%.6f, %.6f, %.6f], \"north\": [%.6f, %.6f, %.6f], \"cell\": %.1f, \"cells_per_edge\": %d,\n"),
			Ground.bRadial ? TEXT("true") : TEXT("false"), Ground.bBlocks ? TEXT("true") : TEXT("false"), Ground.PlanetRadius,
			Ground.PlanetCenter.X, Ground.PlanetCenter.Y, Ground.PlanetCenter.Z,
			Ground.bRadial ? Window.Up.X : 0.0, Ground.bRadial ? Window.Up.Y : 0.0, Ground.bRadial ? Window.Up.Z : 1.0,
			Ground.bRadial ? Window.East.X : 1.0, Ground.bRadial ? Window.East.Y : 0.0, Ground.bRadial ? Window.East.Z : 0.0,
			Ground.bRadial ? Window.North.X : 0.0, Ground.bRadial ? Window.North.Y : 1.0, Ground.bRadial ? Window.North.Z : 0.0,
			Grid.GetCellSize(), Grid.GetCellsPerEdge());
		Json += FString::Printf(TEXT(" \"squares\": %d, \"springs\": %d, \"rivers\": %d, \"systems\": %d, \"refused\": %d, \"reaches\": %d, \"reach_km\": %.3f, \"longest_river_m\": %.1f, \"longest_reach_m\": %.1f, \"most_flow\": %d, \"deepest_cut_voxels\": %.2f, \"valley_rivers\": %d, \"valley_river_km\": %.3f,\n"),
			Region.NumSquares, Region.NumSprings, Region.NumRivers, Region.Systems.Num(), Region.NumRefused, NumReaches, ReachLength / 1.0e5, LongestRiver / 100.0,
			LongestReach / 100.0, MostFlow, DeepestCut / Voxel, NumValleyRivers, ValleyRiverLength / 1.0e5);
		Json += FString::Printf(TEXT(" \"lakes\": %d, \"lake_ha\": %.3f, \"largest_lake_ha\": %.3f, \"deepest_lake_m\": %.2f,\n"), Region.Lakes.Num(), LakeArea / 1.0e8, LargestLake / 1.0e8, DeepestLake / 100.0);
		Json += FString::Printf(TEXT(" \"violations\": %d, \"rises\": %d, \"ends_on_land\": %d, \"ends_over_body\": %d, \"bad_heads\": %d, \"line_warnings\": %d, \"conflicts\": %d, \"digest\": \"%08X\",\n"),
			Violations, Rises, EndsOnLand, EndsOverBody, BadHeads, LineWarnings, Conflicts, Digest);
		Json += TEXT(" \"lake_list\": [");
		for (int32 Index = 0; Index < Region.Lakes.Num(); ++Index)
		{
			const FVoxelHydrologyLake& Lake = Region.Lakes[Index].Get();
			// The lake's middle and its lowest cell as places, and how wide it is.
			FVector MiddlePlace = FVector(Lake.Body.Lake->GetBounds2D().GetCenter(), 0.0);
			FVector2D Size = Lake.Body.Lake->GetBounds2D().GetSize();
			if (Ground.bRadial)
			{
				double CapAngle = 0.0;
				Lake.Body.Lake->GetCap(MiddlePlace, CapAngle);
				Size = FVector2D(2.0 * CapAngle * Ground.PlanetRadius);
			}
			const FVector PitPlace = Grid.PlaceOf(Lake.LowestCell);
			const FVector2D Middle = Window.ToWindow(MiddlePlace);
			const FVector2D Pit = Ground.bRadial ? Window.ToWindow(PitPlace) : FVector2D(Lake.LowestCell.X * Settings.CellVoxels * Voxel, Lake.LowestCell.Y * Settings.CellVoxels * Voxel);
			const FVector MiddleWorld = WorldAt(MiddlePlace, Lake.Body.Level);
			const FVector PitWorld = WorldAt(PitPlace, Lake.Body.Level);
			Json += FString::Printf(TEXT("%s\n  {\"id\": %u, \"level\": %.1f, \"area_m2\": %.0f, \"depth\": %.1f, \"closed\": %d, \"lowered\": %.1f, \"middle\": [%.0f, %.0f], \"pit\": [%.0f, %.0f], \"size\": [%.0f, %.0f], \"middle_world\": [%.1f, %.1f, %.1f], \"pit_world\": [%.1f, %.1f, %.1f]}"),
				Index ? TEXT(",") : TEXT(""), Lake.Body.Id, Lake.WaterLevel, Lake.Area / 1.0e4, Lake.Depth, Lake.Closed, Lake.LoweredForEdge, Middle.X, Middle.Y, Pit.X, Pit.Y,
				Size.X, Size.Y, MiddleWorld.X, MiddleWorld.Y, MiddleWorld.Z, PitWorld.X, PitWorld.Y, PitWorld.Z);
		}
		Json += TEXT("\n ],\n \"reach_list\": [");
		bool bFirst = true;
		for (const TSharedRef<const FVoxelHydrologySystem>& System : Region.Systems)
		{
			for (const FVoxelHydrologyReach& Reach : System->Reaches)
			{
				const TArray<FVoxelWaterReachVertex>& Vertices = Reach.Body.Reach->GetVertices();
				const FVoxelWaterReachVertex& First = Vertices[0];
				const FVoxelWaterReachVertex& Middle = Vertices[Vertices.Num() / 2];
				const FVoxelWaterReachVertex& Last = Vertices.Last();
				const FVector2D FirstAt = Window.ToWindow(First.Position), MiddleAt = Window.ToWindow(Middle.Position), LastAt = Window.ToWindow(Last.Position);
				const FVector FirstWorld = WorldAt(First.Position, First.SurfaceLevel), MiddleWorld = WorldAt(Middle.Position, Middle.SurfaceLevel), LastWorld = WorldAt(Last.Position, Last.SurfaceLevel);
				Json += FString::Printf(TEXT("%s\n  {\"id\": %u, \"mouth\": [%d, %d], \"flow\": %d, \"length\": %.0f, \"fall\": %.1f, \"cut\": %.1f, \"grade_holds\": %d, \"join\": %s, \"head\": %s, \"valley\": %s, \"half_width\": %.1f, \"first\": [%.0f, %.0f, %.1f], \"middle\": [%.0f, %.0f, %.1f], \"last\": [%.0f, %.0f, %.1f], \"first_world\": [%.1f, %.1f, %.1f], \"middle_world\": [%.1f, %.1f, %.1f], \"last_world\": [%.1f, %.1f, %.1f]}"),
					bFirst ? TEXT("") : TEXT(","), Reach.Body.Id, System->Mouth.X, System->Mouth.Y, Reach.Flow, Reach.Length, Reach.Profile.Fall, Reach.Profile.DeepestCut,
					Reach.Profile.NumGradeHolds, Reach.bEndsAtJoin ? TEXT("true") : TEXT("false"), Reach.Profile.bStartsInBody ? TEXT("false") : TEXT("true"),
					System->bValleyRiver ? TEXT("true") : TEXT("false"), Middle.HalfWidth,
					FirstAt.X, FirstAt.Y, First.SurfaceLevel - LevelBase, MiddleAt.X, MiddleAt.Y, Middle.SurfaceLevel - LevelBase, LastAt.X, LastAt.Y, Last.SurfaceLevel - LevelBase,
					FirstWorld.X, FirstWorld.Y, FirstWorld.Z, MiddleWorld.X, MiddleWorld.Y, MiddleWorld.Z, LastWorld.X, LastWorld.Y, LastWorld.Z);
				bFirst = false;
			}
		}
		Json += TEXT("\n ]\n}\n");

		const FString Folder = FPaths::ProjectSavedDir() / TEXT("Hydro");
		IFileManager::Get().MakeDirectory(*Folder, true);
		const FString Base = Folder / FString::Printf(TEXT("%s_%s_%s"), Config ? *Config->GetName() : TEXT("world"), *Tag, NetName(World));
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(N, N, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		FFileHelper::SaveArrayToFile(Png, *(Base + TEXT(".png")));
		FFileHelper::SaveStringToFile(Json, *(Base + TEXT(".json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

		UE_LOG(LogVoxelStreaming, Warning,
			TEXT("voxel.Water.Hydrology.Survey [%s]: net=%s: (%.0f, %.0f) +- %.0f uu, %.2f km2 of land: %d spring(s), %d river(s), %d system(s) (%d refused), %d river reach(es) of %.2f km (the longest river %.0f m, most springs through one %d, deepest cut %.1f voxels), %d lake(s) of %.2f ha (the largest %.2f ha, the deepest %.1f m), %d valley river piece(s) of %.2f km; violations=%d (%d rise(s), %d end(s) on land, %d end(s) over the body, %d head(s), %d line warning(s), %d cell(s) with two ways on); digest=%08X; %.0f ms -> %s.json"),
			*World->GetName(), NetName(World), Centre.X, Centre.Y, Half, LandKm2, Region.NumSprings, Region.NumRivers, Region.Systems.Num(), Region.NumRefused, NumReaches,
			ReachLength / 1.0e5, LongestRiver / 100.0, MostFlow, DeepestCut / Voxel, Region.Lakes.Num(), LakeArea / 1.0e8, LargestLake / 1.0e8, DeepestLake / 100.0,
			NumValleyRivers, ValleyRiverLength / 1.0e5, Violations, Rises, EndsOnLand, EndsOverBody, BadHeads, LineWarnings, Conflicts, Digest, SurveyMs, *Base);
		if (!FirstWarning.IsEmpty())
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Hydrology.Survey [%s]: the first fault: %s"), *World->GetName(), *FirstWarning);
		}
		for (const TSharedRef<const FVoxelHydrologySystem>& System : Region.Refused)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Hydrology.Survey [%s]: the system at mouth (%d, %d), %d spring(s), is refused: %s"),
				*World->GetName(), System->Mouth.X, System->Mouth.Y, System->NumSprings, *System->Refused);
		}
	}
}

// Console: voxel.Water.Hydrology.Survey [x y | pawn] [half] [tag]
static FAutoConsoleCommandWithWorldAndArgs GVoxelHydrologySurveyCmd(
	TEXT("voxel.Water.Hydrology.Survey"),
	TEXT("The generated hydrology over a window of every play world that has it (the hydrology plan's H7 and H8): voxel.Water.Hydrology.Survey [x y | pawn] [half=300000] [tag]. ")
	TEXT("On a planet x and y are a latitude and a longitude in degrees and the window is a square on the plane tangent there. ")
	TEXT("Works the window's ponds and brooks out (whatever is streamed), checks each river reach against the rules that keep a river watertight, and logs the counts, ")
	TEXT("the violations and a digest of the bodies (the same on a server and its clients); a picture and the numbers go to Saved/Hydro/<configuration>_<tag>_<net>.png and .json."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UWorld* TargetWorld = VoxelEditAuditHelpers::FindGameWorld(World);
		if (!TargetWorld)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Hydrology.Survey: no PIE/Game world found (start PIE first)"));
			return;
		}
		// Where the window is: a world position (the pawn's), or two numbers: world X and Y, a planet's latitude and longitude.
		FVector2D Centre = FVector2D::ZeroVector;
		FVector PawnAt = FVector::ZeroVector;
		bool bPawn = false;
		int32 Next = 0;
		if (Args.Num() >= 1 && Args[0].Equals(TEXT("pawn"), ESearchCase::IgnoreCase))
		{
			const APlayerController* PC = TargetWorld->GetFirstPlayerController();
			if (PC && PC->GetPawn())
			{
				PawnAt = PC->GetPawn()->GetActorLocation();
				bPawn = true;
			}
			Next = 1;
		}
		else if (Args.Num() >= 2)
		{
			Centre = FVector2D(FCString::Atod(*Args[0]), FCString::Atod(*Args[1]));
			Next = 2;
		}
		const double Half = Args.Num() > Next ? FMath::Max(FCString::Atod(*Args[Next]), 10000.0) : 300000.0;
		const FString Tag = Args.Num() > Next + 1 ? Args[Next + 1] : TEXT("survey");
		int32 Surveyed = 0;
		for (TObjectIterator<UVoxelChunkManager> It; It; ++It)
		{
			const UWorld* PlayWorld = It->GetWorld();
			if (!PlayWorld || !PlayWorld->IsGameWorld())
			{
				continue;
			}
			if (const TSharedPtr<FVoxelHydrologyNetwork, ESPMode::ThreadSafe> Network = It->GetHydrologyNetwork())
			{
				const FVector Place = bPawn ? Network->GetPlaceOf(PawnAt)
					: (Network->GetGround().bRadial ? FVoxelCubeSphere::LatLonToDirection(Centre.X, Centre.Y) : FVector(Centre.X, Centre.Y, 0.0));
				VoxelHydrologyConsole::Survey(**It, *Network, Place, Half, Tag);
				++Surveyed;
			}
		}
		if (Surveyed == 0)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Hydrology.Survey: no play world generates hydrology (its world configuration names no hydrology configuration)"));
		}
	}));

#endif // !UE_BUILD_SHIPPING
