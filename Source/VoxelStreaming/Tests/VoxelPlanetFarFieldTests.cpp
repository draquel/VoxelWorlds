// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Camera/CameraActor.h"
#include "Camera/PlayerCameraManager.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "ProceduralMeshComponent.h"
#include "VoxelChunkManager.h"
#include "VoxelPlanetFarField.h"
#include "VoxelPlanetOcean.h"
#include "VoxelCubeSphere.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "SphericalPlanetWorldMode.h"
#include "DistanceBandLODStrategy.h"
#include "IVoxelMeshRenderer.h"
#include "ChunkRenderData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The planet's far field (spherical Phase 6, WM-BP9; WORLD_MODES_REVIEW_PLAN.md §20.1's verification row; SP-C5, SP-C6).
// Pure logic (synchronous builds), runs headless; the last test drives a chunk manager's components.
//
// - Quadtree: from 300 uu over the ground at the pole, 45 degrees, the equator and a cube corner of the small planet and
//   of a 1,000 km one with Review_Planet_L's terrain noise, every drawn patch outside the voxel window's band meets the
//   screen-space error bound, neighbouring drawn patches differ by at most SkirtLevels levels, and wherever two drawn
//   patches share an edge the higher one's skirt reaches the lower (no crack).
// - Hole: with the voxel window loaded (every chunk within the view distance along the ground drawn), no drawn terrain
//   patch lies wholly inside it, and every direction out to three view distances is drawn by a patch or a voxel chunk (no
//   gap); the hole follows the window when the viewer moves, with no patch rebuilt; chunks missing inside the window
//   (catch-up) are drawn by patches.
// - MatchesSurface: every surface vertex of a patch at its analytic altitude minus the sink, at levels from a face's
//   quarter to the band; one material per triangle; normals outward; offsets of the patch's size; skirts under their
//   edges; the sea at the sea radius within the sag reported.
// - ChunkManager: on a big planet the far field draws the sea instead of the ocean shell, has a component per built patch
//   and shows what it draws; voxel.World.FarField 0 brings the shell back; a small planet keeps its shell.
// ---------------------------------------------------------------------------

namespace VoxelPlanetFarFieldTestUtils
{
	constexpr double ChunkWorld = 4800.0;
	constexpr double ViewDistance = 40000.0;
	constexpr double UnloadDistance = 48000.0;

	struct FPlanetCase
	{
		const TCHAR* Name;
		double Radius;
		float Shell;
		float Frequency;
		int32 Octaves;
	};

	TArray<FPlanetCase> GetPlanetCases()
	{
		return {
			{ TEXT("small planet (R 250,000, terrain +-6,500)"), 250000.0, 6500.0f, 2.5e-5f, 3 },
			{ TEXT("1,000 km planet (R 1e8, Review_Planet_L's noise, terrain +-192,000)"), 1.0e8, 192000.0f, 3.91e-7f, 9 },
		};
	}

	struct FSite
	{
		const TCHAR* Name;
		FVector Up;
	};

	TArray<FSite> GetSites()
	{
		const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
		return {
			{ TEXT("north pole"), FVector(0, 0, 1) },
			{ TEXT("45 degrees"), FVector(C45, 0, C45) },
			{ TEXT("equator"), FVector(1, 0, 0) },
			{ TEXT("cube corner"), FVector(-1, 1, 1).GetSafeNormal() },
		};
	}

	/** The demo's chunk geometry on a planet whose terrain reaches its shell (as the streaming window's tests). Rooted. */
	UVoxelWorldConfiguration* MakeConfig(const FPlanetCase& Case)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::SphericalPlanet;
		Config->MeshingMode = EMeshingMode::DualContouring;
		Config->VoxelSize = 75.0f;
		Config->ChunkSize = 64;
		Config->ViewDistance = ViewDistance;
		Config->WorldRadius = Case.Radius;
		Config->PlanetMaxTerrainHeight = Case.Shell;
		Config->PlanetMaxTerrainDepth = Case.Shell;
		Config->PlanetHeightScale = 2.0f * Case.Shell;
		Config->BaseHeight = 0.0f;
		Config->NoiseParams.Frequency = Case.Frequency;
		Config->NoiseParams.Octaves = Case.Octaves;
		Config->bEnableBiomes = false;
		Config->bEnableWaterLevel = true;
		Config->WaterRadius = Case.Radius - 0.25 * Case.Shell;
		return Config;
	}

	/** A far field on a planet case, with synchronous builds and the selection's view for a viewer. */
	struct FRig
	{
		UVoxelWorldConfiguration* Config = nullptr;
		TSharedPtr<IVoxelWorldMode> Mode;
		TSharedPtr<const FVoxelFarFieldSurface> Surface;
		FVoxelFarFieldSettings Settings;
		FVoxelPlanetFarField FarField;
		const TArray<FVoxelConditioningZone> NoZones;

		FRig(const FPlanetCase& Case, int32 PatchQuads, bool bSea)
		{
			Config = MakeConfig(Case);
			Mode = FVoxelWorldModeFactory::Create(*Config);
			Surface = FVoxelFarFieldSurface::Make(*Config, Mode);
			const FSphericalPlanetWorldMode& Planet = Surface->GetPlanet();
			Settings.Center = Planet.GetPlanetParams().PlanetCenter;
			Settings.Radius = Planet.GetPlanetParams().PlanetRadius;
			Settings.GridOrigin = Config->WorldOrigin;
			Settings.ChunkWorldSize = Config->GetChunkWorldSize();
			Settings.VoxelSize = Config->VoxelSize;
			Settings.PatchQuads = PatchQuads;
			Settings.Sink = Config->VoxelSize;
			float MinAltitude = 0.0f, MaxAltitude = 0.0f;
			FSphericalPlanetWorldMode::GetDisplacementBounds(Planet.GetTerrainParams(), &Planet.GetBiomeSnapshot(), MinAltitude, MaxAltitude);
			Settings.MinAltitude = MinAltitude;
			Settings.MaxAltitude = MaxAltitude;
			Settings.bSea = bSea;
			Settings.SeaRadius = Config->WaterRadius;
			Settings.SeaMargin = 2.0 * Config->VoxelSize;
			Settings.BandLevel = FVoxelFarFieldSettings::ComputeBandLevel(Settings.Radius, Settings.ChunkWorldSize);
			Settings.ZoneGatherMaxAngle = 32.0 * Settings.ChunkWorldSize / Settings.Radius;
			FarField.Initialize(Settings, Surface);
			FarField.SetSynchronousBuilds(true);
			FarField.SetMaxBuildsInFlight(256);
		}

		~FRig()
		{
			FarField.Reset();
			Config->RemoveFromRoot();
		}

		double Altitude(const FVector& Direction) const { return Surface->SampleAltitude(Direction, NoZones); }
		FVector SurfacePoint(const FVector& Direction) const { return Settings.Center + Direction * (Settings.Radius + Altitude(Direction)); }

		FIntVector ChunkOf(const FVector& Point) const
		{
			const FVector Local = (Point - Settings.GridOrigin) / Settings.ChunkWorldSize;
			return FIntVector(FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), FMath::FloorToInt32(Local.Z));
		}

		FVector ChunkCentre(const FIntVector& Coord) const { return Settings.GridOrigin + (FVector(Coord) + FVector(0.5)) * Settings.ChunkWorldSize; }

		FVoxelFarFieldView MakeView(const FVector& Viewer) const
		{
			FVoxelFarFieldView View;
			View.Camera = Viewer;
			View.Viewer = Viewer;
			View.PixelsPerRadian = 300.0;
			View.MaxScreenError = 4.0;
			View.GroundRadius = Settings.Radius + Altitude((Viewer - Settings.Center).GetSafeNormal());
			View.BandRadius = UnloadDistance + 0.5 * UE_DOUBLE_SQRT_3 * Settings.ChunkWorldSize;
			View.PrebuildRadius = 0.75 * ViewDistance;
			View.Time = 0.0;
			View.HideDelay = 0.0;
			View.ChunkStateVersion = -1;
			return View;
		}

		/** Update until nothing is left to build and an update changes nothing. Returns the updates it took (-1: never settled). */
		int32 Converge(const FVoxelFarFieldView& View, TFunctionRef<bool(const FIntVector&)> IsDrawn, int32 MaxUpdates = 300)
		{
			FVoxelFarFieldChanges Changes;
			for (int32 Update = 1; Update <= MaxUpdates; ++Update)
			{
				FarField.Update(View, IsDrawn, Changes);
				if (FarField.GetBuildsPending() == 0 && Changes.Built.Num() == 0 && Changes.Removed.Num() == 0 && Changes.Visibility.Num() == 0)
				{
					return Update;
				}
			}
			return -1;
		}

		/** Directions out to three view distances around a view's ground point not drawn by a chunk or a patch. */
		int32 CountGaps(const FVoxelFarFieldView& View, TFunctionRef<bool(const FIntVector&)> IsDrawn) const
		{
			const FVector GroundDirection = (View.Viewer - Settings.Center).GetSafeNormal();
			FVector East, North;
			FVoxelCubeSphere::GetTangentFrame(GroundDirection, East, North);
			int32 Gaps = 0;
			for (double Distance = 0.0; Distance <= 3.0 * ViewDistance; Distance += ChunkWorld / 2.0)
			{
				const int32 Around = FMath::Max(8, FMath::CeilToInt32(UE_DOUBLE_TWO_PI * Distance / ChunkWorld));
				for (int32 Step = 0; Step < Around; ++Step)
				{
					const double Theta = UE_DOUBLE_TWO_PI * Step / Around;
					const double Angle = Distance / View.GroundRadius;
					const FVector Tangent = East * FMath::Cos(Theta) + North * FMath::Sin(Theta);
					const FVector Direction = (GroundDirection * FMath::Cos(Angle) + Tangent * FMath::Sin(Angle)).GetSafeNormal();
					if (!IsDrawn(ChunkOf(SurfacePoint(Direction))) && !FarField.FindDrawnPatch(Direction))
					{
						++Gaps;
					}
				}
			}
			return Gaps;
		}

		/** Ground distance from a viewer's ground point (the window's horizons, WM-BP8). */
		double GroundDistance(const FVoxelFarFieldView& View, const FVector& Point) const
		{
			return FDistanceBandLODStrategy::GetPlanetGroundDistance(Settings.Center, View.GroundRadius, View.Viewer, Point);
		}

		/** A loaded voxel window: a chunk is drawn when its centre lies within Radius along the ground (the strategy's load rule). */
		bool InWindow(const FVoxelFarFieldView& View, const FIntVector& Coord, double Radius) const
		{
			return GroundDistance(View, ChunkCentre(Coord)) <= Radius;
		}

		/** A drawn point of a patch's edge: its edge polyline (chords between its vertices) at a fraction along the edge. */
		FVector EdgePoint(const FVoxelFarFieldPatchKey& Key, int32 Edge, double Along) const
		{
			const int32 Q = Settings.PatchQuads;
			const double S = FMath::Clamp(Along, 0.0, 1.0) * Q;
			const int32 K = FMath::Clamp(FMath::FloorToInt32(S), 0, Q - 1);
			const double T = S - K;
			auto Vertex = [&](int32 Index)
			{
				const double F = static_cast<double>(Index) / Q;
				double FU = F, FV = F;
				switch (Edge)
				{
				case 0: FV = 0.0; break;
				case 1: FU = 1.0; break;
				case 2: FV = 1.0; break;
				default: FU = 0.0; break;
				}
				const FVector Direction = Key.GetDirection(FU, FV);
				return Settings.Center + Direction * (Settings.Radius + Altitude(Direction) - Settings.Sink);
			};
			return FMath::Lerp(Vertex(K), Vertex(K + 1), T);
		}

		/** The edge of a patch a direction on its boundary lies on, and how far along it. False when it is not on one. */
		bool FindEdge(const FVoxelFarFieldPatchKey& Key, const FVector& Direction, int32& OutEdge, double& OutAlong) const
		{
			FVector2D UV;
			if (!FVoxelCubeSphere::ProjectToFace(Key.Face, Direction, UV, 0.0))
			{
				return false;
			}
			const FVector2D Min = Key.GetUVMin();
			const double Size = Key.GetUVSize();
			const double FU = (UV.X - Min.X) / Size;
			const double FV = (UV.Y - Min.Y) / Size;
			const double D[4] = { FMath::Abs(FV), FMath::Abs(FU - 1.0), FMath::Abs(FV - 1.0), FMath::Abs(FU) };
			int32 Best = 0;
			for (int32 Edge = 1; Edge < 4; ++Edge)
			{
				Best = D[Edge] < D[Best] ? Edge : Best;
			}
			if (D[Best] > 1.0e-3)
			{
				return false;
			}
			OutEdge = Best;
			OutAlong = (Best == 0 || Best == 2) ? FU : FV;
			return OutAlong >= -1.0e-6 && OutAlong <= 1.0 + 1.0e-6;
		}
	};

	/** Fraction pairs of an edge's points and of the points just past it, outward. */
	void EdgeFractions(int32 Edge, double Along, double Outside, double& OutFU, double& OutFV, double& OutOutsideFU, double& OutOutsideFV)
	{
		switch (Edge)
		{
		case 0: OutFU = Along; OutFV = 0.0; OutOutsideFU = Along; OutOutsideFV = -Outside; break;
		case 1: OutFU = 1.0; OutFV = Along; OutOutsideFU = 1.0 + Outside; OutOutsideFV = Along; break;
		case 2: OutFU = Along; OutFV = 1.0; OutOutsideFU = Along; OutOutsideFV = 1.0 + Outside; break;
		default: OutFU = 0.0; OutFV = Along; OutOutsideFU = -Outside; OutOutsideFV = Along; break;
		}
	}

	/** A console variable set for a scope (restored after). */
	struct FScopedCVar
	{
		IConsoleVariable* Var = nullptr;
		FString Previous;
		FScopedCVar(const TCHAR* Name, const TCHAR* Value)
		{
			Var = IConsoleManager::Get().FindConsoleVariable(Name);
			if (Var)
			{
				Previous = Var->GetString();
				Var->Set(Value, ECVF_SetByConsole);
			}
		}
		~FScopedCVar()
		{
			if (Var)
			{
				Var->Set(*Previous, ECVF_SetByConsole);
			}
		}
	};

	/** A renderer that draws nothing (the chunk manager needs one to initialise). */
	class FNullMeshRenderer : public IVoxelMeshRenderer
	{
	public:
		virtual void Initialize(UWorld* World, const UVoxelWorldConfiguration* WorldConfig) override { bInitialized = true; }
		virtual void Shutdown() override { bInitialized = false; }
		virtual bool IsInitialized() const override { return bInitialized; }
		virtual void UpdateChunkMeshFromCPU(const FIntVector& ChunkCoord, int32 LODLevel, const FChunkMeshData& MeshData) override {}
		virtual void RemoveChunk(const FIntVector& ChunkCoord) override {}
		virtual void ClearAllChunks() override {}
		virtual void SetChunkVisible(const FIntVector& ChunkCoord, bool bVisible) override {}
		virtual void SetAllChunksVisible(bool bVisible) override {}
		virtual void SetMaterial(UMaterialInterface* Material) override {}
		virtual UMaterialInterface* GetMaterial() const override { return nullptr; }
		virtual void UpdateMaterialParameters() override {}
		virtual void UpdateSeamMeshFromCPU(const FIntVector& OwnerChunkCoord, uint8 SeamSlot, int32 LODLevel, FChunkMeshData&& MeshData, bool bCrossfade) override {}
		virtual void RemoveSeamMesh(const FIntVector& OwnerChunkCoord, uint8 SeamSlot) override {}
		virtual void ClearAllSeamMeshes() override {}
		virtual void GetSeamMeshOwners(TArray<FIntVector>& OutOwners) const override {}
		virtual void UpdateLODTransition(const FIntVector& ChunkCoord, float MorphFactor) override {}
		virtual bool IsChunkLoaded(const FIntVector& ChunkCoord) const override { return false; }
		virtual int32 GetLoadedChunkCount() const override { return 0; }
		virtual void GetLoadedChunks(TArray<FIntVector>& OutChunks) const override {}
		virtual int64 GetGPUMemoryUsage() const override { return 0; }
		virtual int64 GetTotalVertexCount() const override { return 0; }
		virtual int64 GetTotalTriangleCount() const override { return 0; }
		virtual bool GetChunkBounds(const FIntVector& ChunkCoord, FBox& OutBounds) const override { return false; }
		virtual FBox GetTotalBounds() const override { return FBox(ForceInit); }
		virtual FString GetDebugStats() const override { return FString(); }
		virtual FString GetRendererTypeName() const override { return TEXT("Null (test)"); }

	private:
		bool bInitialized = false;
	};
}

/**
 * WM-BP9: the quadtree. Nothing of the voxel window is loaded (every patch draws), from 300 uu over the ground at four
 * sites on two planets: the selection settles; every drawn patch outside the band meets the error bound; drawn
 * neighbours differ by at most SkirtLevels levels; along every shared edge the gap between the two patches' edges is
 * within the higher one's skirt.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldPlanetQuadtreeTest,
	"VoxelWorlds.FarField.Planet.Quadtree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldPlanetQuadtreeTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetFarFieldTestUtils;

	for (const FPlanetCase& Case : GetPlanetCases())
	{
		for (const FSite& Site : GetSites())
		{
			FRig Rig(Case, 16, /*bSea=*/true);
			const FVector Viewer = Rig.SurfacePoint(Site.Up) + Site.Up * 300.0;
			const FVoxelFarFieldView View = Rig.MakeView(Viewer);
			const double Start = FPlatformTime::Seconds();
			const int32 Updates = Rig.Converge(View, [](const FIntVector&) { return false; });
			const double Seconds = FPlatformTime::Seconds() - Start;
			TestTrue(*FString::Printf(TEXT("%s, %s: the selection settles"), Case.Name, Site.Name), Updates > 0);

			// The error bound, outside the band (inside it patches split to the band level whatever their error).
			const FVector GroundDirection = Site.Up;
			int32 Drawn = 0, OverBound = 0, InBand = 0, MinLevel = 64, MaxLevel = 0;
			double WorstPixels = 0.0;
			TArray<const FVoxelPlanetFarField::FPatchState*> DrawnPatches;
			Rig.FarField.ForEachPatch([&](const FVoxelPlanetFarField::FPatchState& Patch)
			{
				if (!Patch.bDrawTerrain && !Patch.bDrawSea)
				{
					return;
				}
				++Drawn;
				DrawnPatches.Add(&Patch);
				MinLevel = FMath::Min(MinLevel, Patch.Key.Level);
				MaxLevel = FMath::Max(MaxLevel, Patch.Key.Level);
				const double Angle = FMath::Atan2(FVector::CrossProduct(GroundDirection, Patch.CapDirection).Size(),
					FVector::DotProduct(GroundDirection, Patch.CapDirection)) - Patch.CapAngle;
				if (View.GroundRadius * FMath::Max(Angle, 0.0) < View.BandRadius || Patch.Key.Level >= Rig.Settings.BandLevel)
				{
					++InBand;
					return;
				}
				const double Distance = FMath::Max(FVector::Dist(View.Camera, Patch.BoundsCenter) - Patch.BoundsRadius, 1.0);
				const double Error = FMath::Max(Patch.Error, Patch.bHasSea ? Patch.SeaSag : 0.0);
				const double Pixels = Error * View.PixelsPerRadian / Distance;
				WorstPixels = FMath::Max(WorstPixels, Pixels);
				OverBound += Pixels > View.MaxScreenError + 1.0e-6 ? 1 : 0;
			});

			// Cracks: walk every drawn terrain patch's edges and compare with the drawn neighbour just across.
			int32 EdgeSamples = 0, Cracks = 0, LevelJumps = 0, MaxLevelDifference = 0;
			double WorstGap = 0.0, WorstUncovered = 0.0;
			const int32 Q = Rig.Settings.PatchQuads;
			for (const FVoxelPlanetFarField::FPatchState* A : DrawnPatches)
			{
				if (!A->bDrawTerrain)
				{
					continue;
				}
				for (int32 Edge = 0; Edge < 4; ++Edge)
				{
					for (int32 S = 1; S < 4 * Q; ++S)
					{
						const double Along = static_cast<double>(S) / (4 * Q);
						double FU, FV, OU, OV;
						EdgeFractions(Edge, Along, 1.0e-4, FU, FV, OU, OV);
						const FVector Direction = A->Key.GetDirection(FU, FV);
						const FVoxelPlanetFarField::FPatchState* B = Rig.FarField.FindDrawnPatch(A->Key.GetDirection(OU, OV));
						if (!B || B == A)
						{
							continue;
						}
						int32 EdgeB = 0;
						double AlongB = 0.0;
						if (!Rig.FindEdge(B->Key, Direction, EdgeB, AlongB))
						{
							continue;
						}
						++EdgeSamples;
						const int32 Difference = FMath::Abs(A->Key.Level - B->Key.Level);
						MaxLevelDifference = FMath::Max(MaxLevelDifference, Difference);
						LevelJumps += Difference > Rig.Settings.SkirtLevels ? 1 : 0;
						const double RA = FVector::Dist(Rig.EdgePoint(A->Key, Edge, Along), Rig.Settings.Center);
						const double RB = FVector::Dist(Rig.EdgePoint(B->Key, EdgeB, AlongB), Rig.Settings.Center);
						const double Gap = FMath::Abs(RA - RB);
						const double Skirt = RA > RB ? A->SkirtDepth : B->SkirtDepth;
						WorstGap = FMath::Max(WorstGap, Gap);
						if (Gap > Skirt + 1.0e-3)
						{
							++Cracks;
							WorstUncovered = FMath::Max(WorstUncovered, Gap - Skirt);
						}
					}
				}
			}

			AddInfo(FString::Printf(TEXT("%s, %s: %d patches, %d drawn (levels %d-%d, %d in the band), settled in %d updates (%.2f s, %lld built, %.2f ms each); worst %.2f px (bound %.1f), %d over; %d edge samples, worst gap %.1f uu, %d cracks past the skirt (worst %.2f uu), level difference up to %d"),
				Case.Name, Site.Name, Rig.FarField.GetPatchCount(), Drawn, MinLevel, MaxLevel, InBand, Updates, Seconds,
				Rig.FarField.GetTotalBuilt(), Rig.FarField.GetTotalBuilt() > 0 ? Rig.FarField.GetTotalBuildMs() / Rig.FarField.GetTotalBuilt() : 0.0,
				WorstPixels, View.MaxScreenError, OverBound, EdgeSamples, WorstGap, Cracks, WorstUncovered, MaxLevelDifference));
			TestTrue(*FString::Printf(TEXT("%s, %s: patches drawn"), Case.Name, Site.Name), Drawn > 6);
			TestEqual(*FString::Printf(TEXT("%s, %s: every patch outside the band meets the error bound"), Case.Name, Site.Name), OverBound, 0);
			TestTrue(*FString::Printf(TEXT("%s, %s: shared edges were found"), Case.Name, Site.Name), EdgeSamples > 100);
			TestEqual(*FString::Printf(TEXT("%s, %s: no crack past a skirt"), Case.Name, Site.Name), Cracks, 0);
			TestEqual(*FString::Printf(TEXT("%s, %s: neighbours within the skirts' levels"), Case.Name, Site.Name), LevelJumps, 0);
		}
	}
	return true;
}

/**
 * WM-BP9: the hole. With the voxel window loaded around a viewer 300 uu over the ground (every chunk whose centre lies
 * within the view distance along the ground is drawn): no drawn terrain patch lies wholly inside the window (each reaches
 * past the view distance less a chunk's diagonal); every direction out to three view distances is drawn by a patch or
 * a voxel chunk; after the viewer moves three chunks the same holds and no patch that stayed was rebuilt; with chunks
 * missing inside the window (catch-up), their directions are drawn by patches.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldPlanetHoleTest,
	"VoxelWorlds.FarField.Planet.Hole",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldPlanetHoleTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetFarFieldTestUtils;

	const double ChunkDiagonal = UE_DOUBLE_SQRT_3 * ChunkWorld;
	for (const FPlanetCase& Case : GetPlanetCases())
	{
		const TArray<FSite> Sites = GetSites();
		for (int32 SiteIndex : { 1, 3 })
		{
			const FSite& Site = Sites[SiteIndex];
			FRig Rig(Case, 16, /*bSea=*/false);

			// Checks a settled selection against a drawn-chunk rule: no drawn patch wholly inside the window, no gap.
			auto Check = [&](const TCHAR* Stage, const FVoxelFarFieldView& View, TFunctionRef<bool(const FIntVector&)> IsDrawn)
			{
				const FVector GroundDirection = (View.Viewer - Rig.Settings.Center).GetSafeNormal();
				int32 DrawnTerrain = 0, Inside = 0, HiddenBand = 0;
				Rig.FarField.ForEachPatch([&](const FVoxelPlanetFarField::FPatchState& Patch)
				{
					if (Patch.Key.Level >= Rig.Settings.BandLevel && !Patch.bDrawTerrain && Patch.bCovered)
					{
						++HiddenBand;
					}
					if (!Patch.bDrawTerrain)
					{
						return;
					}
					++DrawnTerrain;
					const double FarAngle = FMath::Atan2(FVector::CrossProduct(GroundDirection, Patch.CapDirection).Size(),
						FVector::DotProduct(GroundDirection, Patch.CapDirection)) + Patch.CapAngle;
					Inside += View.GroundRadius * FarAngle < ViewDistance - ChunkDiagonal ? 1 : 0;
				});

				// Every direction on a polar grid out to three view distances: a drawn patch's or a drawn chunk's.
				FVector East, North;
				FVoxelCubeSphere::GetTangentFrame(GroundDirection, East, North);
				int32 Samples = 0, Gaps = 0, ByVoxels = 0;
				for (double Distance = 0.0; Distance <= 3.0 * ViewDistance; Distance += ChunkWorld / 4.0)
				{
					const int32 Around = FMath::Max(8, FMath::CeilToInt32(UE_DOUBLE_TWO_PI * Distance / (ChunkWorld / 2.0)));
					for (int32 Step = 0; Step < Around; ++Step)
					{
						const double Theta = UE_DOUBLE_TWO_PI * Step / Around;
						const double Angle = Distance / View.GroundRadius;
						const FVector Tangent = East * FMath::Cos(Theta) + North * FMath::Sin(Theta);
						const FVector Direction = (GroundDirection * FMath::Cos(Angle) + Tangent * FMath::Sin(Angle)).GetSafeNormal();
						++Samples;
						if (IsDrawn(Rig.ChunkOf(Rig.SurfacePoint(Direction))))
						{
							++ByVoxels;
							continue;
						}
						Gaps += Rig.FarField.FindDrawnPatch(Direction) ? 0 : 1;
					}
				}
				AddInfo(FString::Printf(TEXT("%s, %s, %s: %d terrain patches drawn, %d band patches hidden by the window, %d wholly inside it; %d directions, %d drawn by voxels, %d gaps"),
					Case.Name, Site.Name, Stage, DrawnTerrain, HiddenBand, Inside, Samples, ByVoxels, Gaps));
				TestEqual(*FString::Printf(TEXT("%s, %s, %s: no terrain patch drawn wholly inside the window"), Case.Name, Site.Name, Stage), Inside, 0);
				TestEqual(*FString::Printf(TEXT("%s, %s, %s: no gap"), Case.Name, Site.Name, Stage), Gaps, 0);
				TestTrue(*FString::Printf(TEXT("%s, %s, %s: the window hides band patches"), Case.Name, Site.Name, Stage), HiddenBand > 0);
				TestTrue(*FString::Printf(TEXT("%s, %s, %s: the voxels draw the middle"), Case.Name, Site.Name, Stage), ByVoxels > 0);
			};

			// The window loaded.
			const FVector Viewer = Rig.SurfacePoint(Site.Up) + Site.Up * 300.0;
			const FVoxelFarFieldView View = Rig.MakeView(Viewer);
			auto Loaded = [&Rig, &View](const FIntVector& Coord) { return Rig.InWindow(View, Coord, ViewDistance); };
			TestTrue(*FString::Printf(TEXT("%s, %s: settles"), Case.Name, Site.Name), Rig.Converge(View, Loaded) > 0);
			Check(TEXT("loaded"), View, Loaded);

			// The viewer moves three chunks east: the hole follows, nothing that stayed is rebuilt.
			TMap<uint64, uint32> Serials;
			Rig.FarField.ForEachPatch([&Serials](const FVoxelPlanetFarField::FPatchState& Patch)
			{
				if (Patch.bBuilt)
				{
					Serials.Add(Patch.Key.Pack(), Patch.BuildSerial);
				}
			});
			FVector East, North;
			FVoxelCubeSphere::GetTangentFrame(Site.Up, East, North);
			const double Move = 3.0 * ChunkWorld / Rig.Settings.Radius;
			const FVector MovedUp = (Site.Up * FMath::Cos(Move) + East * FMath::Sin(Move)).GetSafeNormal();
			const FVector MovedViewer = Rig.SurfacePoint(MovedUp) + MovedUp * 300.0;
			const FVoxelFarFieldView MovedView = Rig.MakeView(MovedViewer);
			auto MovedLoaded = [&Rig, &MovedView](const FIntVector& Coord) { return Rig.InWindow(MovedView, Coord, ViewDistance); };
			TestTrue(*FString::Printf(TEXT("%s, %s: settles after the move"), Case.Name, Site.Name), Rig.Converge(MovedView, MovedLoaded) > 0);
			Check(TEXT("moved three chunks"), MovedView, MovedLoaded);
			int32 Stayed = 0, Rebuilt = 0;
			Rig.FarField.ForEachPatch([&](const FVoxelPlanetFarField::FPatchState& Patch)
			{
				if (const uint32* Serial = Serials.Find(Patch.Key.Pack()))
				{
					++Stayed;
					Rebuilt += Patch.BuildSerial != *Serial ? 1 : 0;
				}
			});
			AddInfo(FString::Printf(TEXT("%s, %s: after the move %d built patches stayed, %d rebuilt"), Case.Name, Site.Name, Stayed, Rebuilt));
			TestTrue(*FString::Printf(TEXT("%s, %s: patches stayed"), Case.Name, Site.Name), Stayed > 0);
			TestEqual(*FString::Printf(TEXT("%s, %s: the hole moved without a rebuild"), Case.Name, Site.Name), Rebuilt, 0);

			// Catch-up: the chunks within two chunks of the viewer's ground point are missing.
			const FVector GroundPoint = Rig.SurfacePoint(MovedUp);
			auto CatchUp = [&Rig, &MovedView, GroundPoint](const FIntVector& Coord)
			{
				return Rig.InWindow(MovedView, Coord, ViewDistance) && FVector::Dist(Rig.ChunkCentre(Coord), GroundPoint) > 2.0 * ChunkWorld;
			};
			TestTrue(*FString::Printf(TEXT("%s, %s: settles with chunks missing"), Case.Name, Site.Name), Rig.Converge(MovedView, CatchUp) > 0);
			int32 Missing = 0, MissingDrawn = 0;
			FVoxelCubeSphere::GetTangentFrame(MovedUp, East, North);
			for (int32 I = -6; I <= 6; ++I)
			{
				for (int32 J = -6; J <= 6; ++J)
				{
					const double Angle = FMath::Sqrt(static_cast<double>(I * I + J * J)) * 0.25 * ChunkWorld / MovedView.GroundRadius;
					const FVector Tangent = (East * I + North * J).GetSafeNormal();
					const FVector Direction = Tangent.IsNearlyZero() ? MovedUp
						: (MovedUp * FMath::Cos(Angle) + Tangent * FMath::Sin(Angle)).GetSafeNormal();
					if (CatchUp(Rig.ChunkOf(Rig.SurfacePoint(Direction))))
					{
						continue;
					}
					++Missing;
					MissingDrawn += Rig.FarField.FindDrawnPatch(Direction) ? 1 : 0;
				}
			}
			AddInfo(FString::Printf(TEXT("%s, %s: with chunks missing near the viewer, %d of %d directions over them drawn by patches"),
				Case.Name, Site.Name, MissingDrawn, Missing));
			TestTrue(*FString::Printf(TEXT("%s, %s: chunks are missing"), Case.Name, Site.Name), Missing > 0);
			TestEqual(*FString::Printf(TEXT("%s, %s: patches draw where chunks are missing"), Case.Name, Site.Name), MissingDrawn, Missing);
		}
	}
	return true;
}

/**
 * Visual polish VP4: no coarse stand-in over the voxel window while the viewer walks. With the window loaded around a
 * viewer at 45 degrees on the small planet, settled past the release delay (ancestors hidden under their children are
 * released), the viewer walks 7,500 uu east at 1,500 uu/s, the window following, one update a frame at 30 fps: with
 * View.bAvoidStandIns no update draws a stand-in over the window (under the rule before, covered band patches the viewer
 * leaves past the prebuild radius, and a patch coming back over the horizon, need a build, and their nearest built
 * ancestor, up to a face's root, stands in for a frame); every direction out to three view distances stays drawn by a
 * chunk or a patch; and
 * the window shrunk to 0.85 of the view distance (its edge chunks unloading) leaves no gap in the very next update (the
 * band patches near the edge were built meanwhile).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldPlanetNoStandInTest,
	"VoxelWorlds.FarField.Planet.NoStandInOverWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldPlanetNoStandInTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetFarFieldTestUtils;

	const FPlanetCase Case = GetPlanetCases()[0];
	const FSite Site = GetSites()[1];
	for (const bool bAvoidStandIns : { false, true })
	{
		const TCHAR* Rule = bAvoidStandIns ? TEXT("avoiding stand-ins") : TEXT("the rule before");
		FRig Rig(Case, 16, /*bSea=*/false);
		auto MakeView = [&Rig, bAvoidStandIns](const FVector& Viewer, double Time)
		{
			FVoxelFarFieldView View = Rig.MakeView(Viewer);
			View.Time = Time;
			View.HideDelay = 0.45;
			View.bAvoidStandIns = bAvoidStandIns;
			return View;
		};
		auto Viewer = [&Rig](const FVector& Up) { return Rig.SurfacePoint(Up) + Up * 170.0; };

		// Settled with the window loaded, then again past the release delay (hidden ancestors released).
		double Time = 0.0;
		FVoxelFarFieldView View = MakeView(Viewer(Site.Up), Time);
		auto Loaded = [&Rig, &View](const FIntVector& Coord) { return Rig.InWindow(View, Coord, ViewDistance); };
		TestTrue(*FString::Printf(TEXT("%s: settles"), Rule), Rig.Converge(View, Loaded) > 0);
		Time = 20.0;
		View = MakeView(Viewer(Site.Up), Time);
		TestTrue(*FString::Printf(TEXT("%s: settles past the release delay"), Rule), Rig.Converge(View, Loaded) > 0);
		const int64 ReleasedBefore = Rig.FarField.GetTotalReleased();

		// The walk: 1,500 uu/s east, an update a frame at 30 fps, the window following.
		FVector East, North;
		FVoxelCubeSphere::GetTangentFrame(Site.Up, East, North);
		const int32 Frames = 150;
		const double StepAngle = 50.0 / Rig.Settings.Radius;
		int32 FramesWithStandIns = 0, StandIns = 0, CoarsestLevel = 64, UnreadyCovered = 0, WalkGaps = 0;
		FVoxelFarFieldChanges Changes;
		for (int32 Frame = 1; Frame <= Frames; ++Frame)
		{
			const double Angle = Frame * StepAngle;
			const FVector Up = (Site.Up * FMath::Cos(Angle) + East * FMath::Sin(Angle)).GetSafeNormal();
			Time += 1.0 / 30.0;
			View = MakeView(Viewer(Up), Time);
			Rig.FarField.Update(View, Loaded, Changes);
			const int32 Count = Rig.FarField.GetStandInCount();
			StandIns += Count;
			FramesWithStandIns += Count > 0 ? 1 : 0;
			if (Count > 0)
			{
				CoarsestLevel = FMath::Min(CoarsestLevel, Rig.FarField.GetStandInMinLevel());
				// The band patches this update asked to build (their builds land next update), and where they lie.
				const FVector GroundDirection = Up;
				FString Waiting;
				Rig.FarField.ForEachPatch([&](const FVoxelPlanetFarField::FPatchState& Patch)
				{
					if (!Patch.bBuilding)
					{
						return;
					}
					const double Angle = FMath::Atan2(FVector::CrossProduct(GroundDirection, Patch.CapDirection).Size(),
						FVector::DotProduct(GroundDirection, Patch.CapDirection));
					Waiting += FString::Printf(TEXT(" [level %d, children %d, ground %.0f-%.0f, built before %d, covered %d since %.2f]"),
						Patch.Key.Level, Patch.bHasChildren ? 1 : 0,
						View.GroundRadius * FMath::Max(Angle - Patch.CapAngle, 0.0), View.GroundRadius * (Angle + Patch.CapAngle),
						Patch.bBuilt ? 1 : 0, Patch.bCovered ? 1 : 0, Patch.CoveredSince >= 0.0 ? Time - Patch.CoveredSince : -1.0);
				});
				FIntVector Undrawn;
				const bool bUndrawn = Rig.FarField.GetUndrawnChunk(Undrawn);
				AddInfo(FString::Printf(TEXT("%s: frame %d: %d stand-in(s), level %d; unready band patches %d covered, %d uncovered%s; building:%s"),
					Rule, Frame, Count, Rig.FarField.GetStandInMinLevel(), Rig.FarField.GetUnreadyBandCount(true), Rig.FarField.GetUnreadyBandCount(false),
					bUndrawn ? *FString::Printf(TEXT(" (a chunk not drawn %.0f from the ground point)"), Rig.GroundDistance(View, Rig.ChunkCentre(Undrawn))) : TEXT(""),
					*Waiting));
			}
			UnreadyCovered += Rig.FarField.GetUnreadyBandCount(true);
			if (Frame % 30 == 0)
			{
				WalkGaps += Rig.CountGaps(View, Loaded);
			}
		}
		AddInfo(FString::Printf(TEXT("%s: walking 7,500 uu, %d of %d frames drew a stand-in over the window (%d in all, coarsest level %d, band %d); covered band patches not ready %d; %lld patches released while settling; %d gaps at five checks"),
			Rule, FramesWithStandIns, Frames, StandIns, FramesWithStandIns > 0 ? CoarsestLevel : -1, Rig.Settings.BandLevel, UnreadyCovered,
			ReleasedBefore, WalkGaps));
		TestTrue(*FString::Printf(TEXT("%s: hidden patches were released while settling"), Rule), ReleasedBefore > 0);
		TestEqual(*FString::Printf(TEXT("%s: no gap while walking"), Rule), WalkGaps, 0);
		if (bAvoidStandIns)
		{
			TestEqual(*FString::Printf(TEXT("%s: no stand-in over the window while walking"), Rule), FramesWithStandIns, 0);
			TestEqual(*FString::Printf(TEXT("%s: no covered band patch waits for a build"), Rule), UnreadyCovered, 0);

			// The window's edge unloads: the band patches there were built meanwhile and draw in the very next update.
			TestTrue(*FString::Printf(TEXT("%s: settles after the walk"), Rule), Rig.Converge(View, Loaded) > 0);
			auto Shrunk = [&Rig, &View](const FIntVector& Coord) { return Rig.InWindow(View, Coord, 0.85 * ViewDistance); };
			Rig.FarField.Update(View, Shrunk, Changes);
			const int32 EdgeGaps = Rig.CountGaps(View, Shrunk);
			AddInfo(FString::Printf(TEXT("%s: the window shrunk to 0.85 of the view distance: %d gaps in the next update, %d stand-ins"),
				Rule, EdgeGaps, Rig.FarField.GetStandInCount()));
			TestEqual(*FString::Printf(TEXT("%s: the edge's patches draw at once when its chunks unload"), Rule), EdgeGaps, 0);

		}
		else
		{
			// Documents the flicker this replaces (VP4): if this fails the rule before no longer shows it, and the test needs
			// a new walk.
			TestTrue(*FString::Printf(TEXT("%s: stand-ins over the window while walking"), Rule), FramesWithStandIns > 0);
		}

		// A teleport 40 degrees toward the pole, settled past the release delay first: the destination was under the
		// horizon (culled, released) on a face whose root is built, and nothing is loaded there yet. The first update
		// after the cut has no gap: an ancestor stands in as before (the horizon rule holds for motion only). (A face
		// never seen: TeleportOntoUnseenFace, F4.2.)
		Time += 20.0;
		TestTrue(*FString::Printf(TEXT("%s: settles before the teleport"), Rule), Rig.Converge(MakeView(View.Viewer, Time), Loaded) > 0);
		const double Jump = FMath::DegreesToRadians(40.0);
		const FVector Far = (Site.Up * FMath::Cos(Jump) + North * FMath::Sin(Jump)).GetSafeNormal();
		Time += 1.0 / 30.0;
		View = MakeView(Viewer(Far), Time);
		auto Nothing = [](const FIntVector&) { return false; };
		Rig.FarField.Update(View, Nothing, Changes);
		const int32 TeleportGaps = Rig.CountGaps(View, Nothing);
		AddInfo(FString::Printf(TEXT("%s: a teleport 40 degrees toward the pole: %d gaps in the first update, %d stand-ins over the window, coarsest level %d"), Rule, TeleportGaps, Rig.FarField.GetStandInCount(), Rig.FarField.GetStandInMinLevel()));
		TestEqual(*FString::Printf(TEXT("%s: no gap after a teleport"), Rule), TeleportGaps, 0);
	}
	return true;
}

/**
 * Visual polish F4.2: a teleport onto a cube face the camera never saw. The window loaded around a viewer at 45 degrees on
 * the small planet (between the +X and +Z faces), settled past the release delay; then a teleport 90 degrees east, onto
 * the middle of a face that lay under the horizon all along, where nothing of the window is loaded. With
 * View.bBuildAllRoots every root is built while settling (culled ones drawing nothing: the site draws the same patches as
 * under the rule before) and the destination face's root stands in from the first update after the teleport: no gap out
 * to three view distances, then or in the updates after it while the face refines. Under the rule before that root was
 * never built: nothing of the far field was drawn there in the first update.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldPlanetUnseenFaceTest,
	"VoxelWorlds.FarField.Planet.TeleportOntoUnseenFace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldPlanetUnseenFaceTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetFarFieldTestUtils;

	const FPlanetCase Case = GetPlanetCases()[0];
	const FSite Site = GetSites()[1];
	FVector East, North;
	FVoxelCubeSphere::GetTangentFrame(Site.Up, East, North);
	const FVector Far = East.GetSafeNormal();
	const int32 FarFace = FVoxelCubeSphere::DirectionToSurface(Far).Face;
	const uint64 FarRoot = FVoxelFarFieldPatchKey(FarFace, 0, 0, 0).Pack();
	TSet<uint64> DrawnAtSiteBefore;
	for (const bool bBuildAllRoots : { false, true })
	{
		const TCHAR* Rule = bBuildAllRoots ? TEXT("building every root") : TEXT("the rule before");
		FRig Rig(Case, 16, /*bSea=*/false);
		auto MakeView = [&Rig, bBuildAllRoots](const FVector& Viewer, double Time)
		{
			FVoxelFarFieldView View = Rig.MakeView(Viewer);
			View.Time = Time;
			View.HideDelay = 0.45;
			View.bBuildAllRoots = bBuildAllRoots;
			return View;
		};
		auto Viewer = [&Rig](const FVector& Up) { return Rig.SurfacePoint(Up) + Up * 170.0; };

		// Settled with the window loaded, then again past the release delay.
		double Time = 0.0;
		FVoxelFarFieldView View = MakeView(Viewer(Site.Up), Time);
		auto Loaded = [&Rig, &View](const FIntVector& Coord) { return Rig.InWindow(View, Coord, ViewDistance); };
		TestTrue(*FString::Printf(TEXT("%s: settles"), Rule), Rig.Converge(View, Loaded) > 0);
		Time = 20.0;
		View = MakeView(Viewer(Site.Up), Time);
		TestTrue(*FString::Printf(TEXT("%s: settles past the release delay"), Rule), Rig.Converge(View, Loaded) > 0);

		// The roots built, what each holds, and what the site draws.
		int32 RootsBuilt = 0;
		SIZE_T RootBytes = 0;
		for (int32 Face = 0; Face < 6; ++Face)
		{
			const FVoxelPlanetFarField::FPatchState* Root = Rig.FarField.FindPatch(FVoxelFarFieldPatchKey(Face, 0, 0, 0).Pack());
			if (Root && Root->bResident)
			{
				++RootsBuilt;
				RootBytes = FMath::Max(RootBytes, Root->Mesh.IsValid() ? Root->Mesh->GetAllocatedSize() : SIZE_T(0));
			}
		}
		TSet<uint64> DrawnAtSite;
		Rig.FarField.ForEachPatch([&DrawnAtSite](const FVoxelPlanetFarField::FPatchState& Patch)
		{
			if (Patch.bDrawTerrain || Patch.bDrawSea)
			{
				DrawnAtSite.Add(Patch.Key.Pack());
			}
		});
		const FVoxelPlanetFarField::FPatchState* Destination = Rig.FarField.FindPatch(FarRoot);
		TestTrue(*FString::Printf(TEXT("%s: the destination's face lay under the horizon from the site"), Rule), Destination && Destination->bCulled);
		AddInfo(FString::Printf(TEXT("%s: settled at 45 degrees: %d of 6 roots built (the largest %.1f KB), %d patches drawn; the destination's face %d"),
			Rule, RootsBuilt, RootBytes / 1024.0, DrawnAtSite.Num(), FarFace));
		if (bBuildAllRoots)
		{
			TestEqual(*FString::Printf(TEXT("%s: every root is built"), Rule), RootsBuilt, 6);
			TestTrue(*FString::Printf(TEXT("%s: the site draws what it drew under the rule before"), Rule),
				DrawnAtSite.Num() == DrawnAtSiteBefore.Num() && DrawnAtSite.Includes(DrawnAtSiteBefore));
		}
		else
		{
			// Documents the gap this closes (F4.2): if this fails the destination's root is built under the rule before,
			// and the test needs a face the site does not see.
			TestTrue(*FString::Printf(TEXT("%s: the destination's root was never built"), Rule), Destination && !Destination->bBuilt);
			DrawnAtSiteBefore = DrawnAtSite;
		}

		// The teleport: nothing of the window loaded there; the first update after it, and a second of updates after that.
		auto Nothing = [](const FIntVector&) { return false; };
		FVoxelFarFieldChanges Changes;
		int32 FirstGaps = 0, LaterGaps = 0, LaterUpdatesWithGaps = 0;
		for (int32 Frame = 1; Frame <= 30; ++Frame)
		{
			Time += 1.0 / 30.0;
			View = MakeView(Viewer(Far), Time);
			Rig.FarField.Update(View, Nothing, Changes);
			const int32 Gaps = Rig.CountGaps(View, Nothing);
			if (Frame == 1)
			{
				FirstGaps = Gaps;
				const FVoxelPlanetFarField::FPatchState* Drawn = Rig.FarField.FindDrawnPatch(Far);
				AddInfo(FString::Printf(TEXT("%s: a teleport 90 degrees east onto face %d: %d gaps in the first update, %d patches drawn, under the viewer %s"),
					Rule, FarFace, Gaps, Rig.FarField.GetDrawnPatchCount(), Drawn ? *Drawn->Key.ToString() : TEXT("nothing")));
			}
			else
			{
				LaterGaps += Gaps;
				LaterUpdatesWithGaps += Gaps > 0 ? 1 : 0;
			}
		}
		AddInfo(FString::Printf(TEXT("%s: the 29 updates after the first: %d with gaps (%d gaps in all)"), Rule, LaterUpdatesWithGaps, LaterGaps));
		if (bBuildAllRoots)
		{
			TestEqual(*FString::Printf(TEXT("%s: no gap in the first update after the teleport"), Rule), FirstGaps, 0);
			TestEqual(*FString::Printf(TEXT("%s: no gap in the updates after it"), Rule), LaterGaps, 0);
		}
		else
		{
			TestTrue(*FString::Printf(TEXT("%s: the first update after the teleport has gaps"), Rule), FirstGaps > 0);
		}
	}
	return true;
}

/**
 * WM-BP9: patches match the analytic surface. Patches holding the 45-degree site and the cube corner at levels from a
 * face's quarter to the band, on both planets, with the sea: every surface vertex at its direction's altitude minus the
 * sink; every terrain triangle one material; normals unit and outward; offsets no larger than the patch; skirt vertices
 * under their edge; sea vertices at the sea radius and the sea's reported sag at least the largest measured.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldPlanetMatchesSurfaceTest,
	"VoxelWorlds.FarField.Planet.MatchesSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldPlanetMatchesSurfaceTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetFarFieldTestUtils;

	const TArray<FSite> Sites = GetSites();
	for (const FPlanetCase& Case : GetPlanetCases())
	{
		FRig Rig(Case, 32, /*bSea=*/true);
		const FVoxelFarFieldSettings& Settings = Rig.Settings;
		const int32 Band = Settings.BandLevel;
		for (int32 SiteIndex : { 1, 3 })
		{
			const FSite& Site = Sites[SiteIndex];
			const FVoxelSurfaceCoord Coord = FVoxelCubeSphere::DirectionToSurface(Site.Up);
			for (const int32 Level : { 1, FMath::Max(Band - 6, 2), FMath::Max(Band - 3, 3), Band })
			{
				const int32 Cells = 1 << Level;
				const FVoxelFarFieldPatchKey Key(Coord.Face, Level,
					FMath::Clamp(FMath::FloorToInt32((Coord.UV.X + 1.0) / 2.0 * Cells), 0, Cells - 1),
					FMath::Clamp(FMath::FloorToInt32((Coord.UV.Y + 1.0) / 2.0 * Cells), 0, Cells - 1));
				FVoxelFarFieldPatchMesh Mesh;
				FVoxelPlanetFarField::BuildPatch(Settings, *Rig.Surface, Rig.NoZones, Key, Mesh);

				// Float offsets of the patch's size: the tolerance scales with it.
				const double Arc = Settings.GetPatchArc(Level);
				const double Tolerance = 4.0 * FMath::Pow(2.0, -24.0) * Arc * 1.5 + 0.05;
				double WorstSurface = 0.0, WorstOffset = 0.0, WorstNormal = 1.0;
				const FVoxelFarFieldSection& Terrain = Mesh.Terrain;
				// A vertex's own grid direction: its direction's place on the patch, rounded to the nearest grid point. (Not the
				// direction of its float position: on a cube edge that can fall a rounding off the plane the terrain steps
				// across, G43.)
				const int32 Q = Settings.PatchQuads;
				auto GridDirectionOf = [&Key, Q](const FVector& Direction)
				{
					FVector2D UV;
					FVoxelCubeSphere::ProjectToFace(Key.Face, Direction, UV, 0.0);
					const FVector2D Min = Key.GetUVMin();
					const double Size = Key.GetUVSize();
					const int32 I = FMath::Clamp(FMath::RoundToInt32((UV.X - Min.X) / Size * Q), 0, Q);
					const int32 J = FMath::Clamp(FMath::RoundToInt32((UV.Y - Min.Y) / Size * Q), 0, Q);
					return Key.GetDirection(static_cast<double>(I) / Q, static_cast<double>(J) / Q);
				};
				for (int32 V = 0; V < Mesh.TerrainSurfaceVertices; ++V)
				{
					const FVector World = Mesh.Origin + FVector(Terrain.Positions[V]);
					const FVector GridDirection = GridDirectionOf((World - Settings.Center).GetSafeNormal());
					const FVector Expected = Settings.Center + GridDirection * (Settings.Radius + Rig.Altitude(GridDirection) - Settings.Sink);
					WorstSurface = FMath::Max(WorstSurface, FVector::Dist(World, Expected));
					WorstOffset = FMath::Max(WorstOffset, static_cast<double>(Terrain.Positions[V].Size()));
					WorstNormal = FMath::Min(WorstNormal, FVector::DotProduct(FVector(Terrain.Normals[V]), GridDirection));
				}
				// One material per triangle (the terrain material reads it from the interpolated UV1).
				int32 MixedTriangles = 0;
				for (int32 T = 0; T + 2 < Terrain.Triangles.Num(); T += 3)
				{
					const float M0 = Terrain.UV1[Terrain.Triangles[T]].X;
					MixedTriangles += (Terrain.UV1[Terrain.Triangles[T + 1]].X != M0 || Terrain.UV1[Terrain.Triangles[T + 2]].X != M0) ? 1 : 0;
				}
				// Skirt vertices: under the surface by up to the skirt depth.
				double WorstSkirt = 0.0;
				for (int32 V = Mesh.TerrainSurfaceVertices; V < Terrain.Positions.Num(); ++V)
				{
					const FVector World = Mesh.Origin + FVector(Terrain.Positions[V]);
					const FVector Direction = GridDirectionOf((World - Settings.Center).GetSafeNormal());
					const double Below = (Settings.Radius + Rig.Altitude(Direction) - Settings.Sink) - FVector::Dist(World, Settings.Center);
					WorstSkirt = FMath::Max(WorstSkirt, Below - Mesh.SkirtDepth);
				}
				// The sea at its radius; the sag reported covers the measured one.
				double WorstSea = 0.0, MeasuredSag = 0.0;
				const FVoxelFarFieldSection& Sea = Mesh.Sea;
				for (const FVector3f& Position : Sea.Positions)
				{
					WorstSea = FMath::Max(WorstSea, FMath::Abs(FVector::Dist(Mesh.Origin + FVector(Position), Settings.Center) - Settings.SeaRadius));
				}
				for (int32 T = 0; T + 2 < Sea.Triangles.Num(); T += 6)
				{
					const FVector A = Mesh.Origin + FVector(Sea.Positions[Sea.Triangles[T]]);
					const FVector D = Mesh.Origin + FVector(Sea.Positions[Sea.Triangles[T + 1]]);
					MeasuredSag = FMath::Max(MeasuredSag, Settings.SeaRadius - FVector::Dist((A + D) * 0.5, Settings.Center));
				}

				AddInfo(FString::Printf(TEXT("%s, %s, level %d (%.0f uu across): %d surface vertices within %.4f uu (tolerance %.3f), offsets up to %.0f, normals >= %.3f, %d mixed triangles, skirt %.1f uu (worst past it %.3f), sea %d vertices within %.4f uu, sag %.3f (measured %.3f), error %.2f uu, %d surface chunks, %.1f ms"),
					Case.Name, Site.Name, Level, Arc, Mesh.TerrainSurfaceVertices, WorstSurface, Tolerance, WorstOffset, WorstNormal,
					MixedTriangles, Mesh.SkirtDepth, WorstSkirt, Sea.Positions.Num(), WorstSea, Mesh.SeaSag, MeasuredSag, Mesh.Error,
					Mesh.SurfaceChunks.Num(), Mesh.BuildMs));
				const FString Name = FString::Printf(TEXT("%s, %s, level %d"), Case.Name, Site.Name, Level);
				TestTrue(*FString::Printf(TEXT("%s: surface vertices on the analytic surface less the sink"), *Name), WorstSurface <= Tolerance);
				TestTrue(*FString::Printf(TEXT("%s: offsets of the patch's size"), *Name), WorstOffset <= 1.2 * Arc + 2.0 * Case.Shell);
				TestTrue(*FString::Printf(TEXT("%s: normals outward"), *Name), WorstNormal > 0.0);
				TestEqual(*FString::Printf(TEXT("%s: one material per triangle"), *Name), MixedTriangles, 0);
				TestTrue(*FString::Printf(TEXT("%s: skirts within their depth"), *Name), WorstSkirt <= Tolerance);
				TestTrue(*FString::Printf(TEXT("%s: sea vertices at the sea radius"), *Name), WorstSea <= Tolerance);
				TestTrue(*FString::Printf(TEXT("%s: the sea's sag reported"), *Name), Mesh.SeaSag + Tolerance >= MeasuredSag);
				TestTrue(*FString::Printf(TEXT("%s: band patches know their chunks"), *Name), Level < Band || Mesh.SurfaceChunks.Num() > 0);
			}
		}

		// A cube edge from both faces: the 45-degree site (0.707, 0, 0.707) lies on the edge between +X and +Z, the plane
		// x = z, across which the terrain steps (G43). The band patches on either side share their edge vertices exactly.
		{
			const FVector OnEdge = Sites[1].Up;
			const FVector Across = FVector(0.0, 0.0, 1.0e-4);
			FVoxelFarFieldPatchKey Keys[2];
			FVoxelFarFieldPatchMesh Meshes[2];
			for (int32 Side = 0; Side < 2; ++Side)
			{
				const FVoxelSurfaceCoord Coord = FVoxelCubeSphere::DirectionToSurface((OnEdge + (Side == 0 ? -Across : Across)).GetSafeNormal());
				const int32 Cells = 1 << Band;
				Keys[Side] = FVoxelFarFieldPatchKey(Coord.Face, Band,
					FMath::Clamp(FMath::FloorToInt32((Coord.UV.X + 1.0) / 2.0 * Cells), 0, Cells - 1),
					FMath::Clamp(FMath::FloorToInt32((Coord.UV.Y + 1.0) / 2.0 * Cells), 0, Cells - 1));
				FVoxelPlanetFarField::BuildPatch(Settings, *Rig.Surface, Rig.NoZones, Keys[Side], Meshes[Side]);
			}
			int32 Shared = 0;
			double WorstShared = 0.0;
			for (int32 V = 0; V < Meshes[0].TerrainSurfaceVertices; ++V)
			{
				const FVector World = Meshes[0].Origin + FVector(Meshes[0].Terrain.Positions[V]);
				const FVector Direction = (World - Settings.Center).GetSafeNormal();
				if (FMath::Abs(Direction.X - Direction.Z) > 1.0e-6)
				{
					continue; // not on the edge
				}
				double Nearest = TNumericLimits<double>::Max();
				for (int32 W = 0; W < Meshes[1].TerrainSurfaceVertices; ++W)
				{
					Nearest = FMath::Min(Nearest, FVector::Dist(World, Meshes[1].Origin + FVector(Meshes[1].Terrain.Positions[W])));
				}
				++Shared;
				WorstShared = FMath::Max(WorstShared, Nearest);
			}
			AddInfo(FString::Printf(TEXT("%s: the cube edge x = z between %s and %s: %d edge vertices, the other face's nearest within %.4f uu"),
				Case.Name, *Keys[0].ToString(), *Keys[1].ToString(), Shared, WorstShared));
			TestTrue(*FString::Printf(TEXT("%s: the two patches are on different faces"), Case.Name), Keys[0].Face != Keys[1].Face);
			TestTrue(*FString::Printf(TEXT("%s: edge vertices found"), Case.Name), Shared > Settings.PatchQuads / 2);
			TestTrue(*FString::Printf(TEXT("%s: a cube edge's vertices are the same from both faces"), Case.Name), WorstShared <= 0.05);
		}
	}
	return true;
}

/**
 * WM-BP9 in a chunk manager: a game world on a planet of R 1e6 (the ocean shell would sag past its budget) draws its far
 * field with the sea in it and no ocean shell, has a component per built patch with the drawn sections shown, and
 * reports its figures; voxel.World.FarField 0 drops it and brings the shell back. A small planet (R 20,000) keeps its
 * ocean shell beside its far field.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldPlanetChunkManagerTest,
	"VoxelWorlds.FarField.Planet.ChunkManager",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldPlanetChunkManagerTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetFarFieldTestUtils;

	auto RunWorld = [this](const TCHAR* Name, double Radius, bool bExpectFarFieldSea)
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, Name);
		FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
		WorldContext.SetCurrentWorld(World);
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::SphericalPlanet;
		Config->WorldOrigin = FVector::ZeroVector;
		Config->WorldRadius = Radius;
		Config->PlanetHeightScale = 1600.0f;
		Config->BaseHeight = 0.0f;
		Config->PlanetMaxTerrainHeight = 1000.0f;
		Config->PlanetMaxTerrainDepth = 1000.0f;
		Config->MeshingMode = EMeshingMode::DualContouring;
		Config->ChunkSize = 32;
		Config->VoxelSize = 50.0f;
		Config->ViewDistance = 3000.0f;
		Config->bUseGPUGeneration = false;
		Config->bGenerateCollision = false;
		Config->bEnableScatter = false;
		Config->bEnableCaves = false;
		Config->bEnableBiomes = false;
		Config->bEnableWaterLevel = true;
		Config->WaterRadius = Radius - 200.0;
		Config->WaterMeshMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		Config->NoiseParams.Seed = 9001;
		Config->NoiseParams.Frequency = 0.0004f;
		Config->NoiseParams.Octaves = 3;

		AActor* Owner = World->SpawnActor<AActor>();
		UVoxelChunkManager* Manager = NewObject<UVoxelChunkManager>(Owner);
		Manager->RegisterComponent();
		FNullMeshRenderer Renderer;
		Renderer.Initialize(World, Config);
		Manager->Initialize(Config, new FDistanceBandLODStrategy(), &Renderer);

		const FString Prefix(Name);
		TestTrue(*FString::Printf(TEXT("%s: a planet in a game world has a far field"), *Prefix), Manager->HasFarField());
		TestEqual(*FString::Printf(TEXT("%s: the far field draws the sea only at scale"), *Prefix), Manager->DoesFarFieldDrawSea(), bExpectFarFieldSea);
		TestEqual(*FString::Printf(TEXT("%s: the ocean shell draws it otherwise"), *Prefix), Manager->HasPlanetOcean(), !bExpectFarFieldSea);
		if (FVoxelPlanetFarField* FarField = Manager->GetFarFieldMutable())
		{
			FarField->SetSynchronousBuilds(true);
			FarField->SetMaxBuildsInFlight(256);
			const FVector Up = FVector(1, 1, 1).GetSafeNormal();
			const FVector Viewer = Up * (Radius + 1200.0);
			for (int32 Update = 0; Update < 200; ++Update)
			{
				Manager->UpdateFarFieldForViewer(Viewer);
				if (FarField->GetBuildsPending() == 0 && Update > 2)
				{
					break;
				}
			}
			Manager->UpdateFarFieldForViewer(Viewer);
			int32 Built = 0, Drawn = 0, Shown = 0, Hidden = 0, Missing = 0, SeaShown = 0;
			FarField->ForEachPatch([&](const FVoxelPlanetFarField::FPatchState& Patch)
			{
				if (!Patch.bResident)
				{
					return;
				}
				++Built;
				UProceduralMeshComponent* Component = Manager->GetFarFieldComponent(Patch.Key.Pack());
				if (!Component)
				{
					++Missing;
					return;
				}
				Drawn += Patch.bDrawTerrain ? 1 : 0;
				const bool bShown = Component->GetNumSections() > 0 && Component->IsMeshSectionVisible(0);
				Shown += (Patch.bDrawTerrain && bShown) ? 1 : 0;
				Hidden += (!Patch.bDrawTerrain && !bShown) ? 1 : 0;
				SeaShown += (Patch.bDrawSea && Component->GetNumSections() > 1 && Component->IsMeshSectionVisible(1)) ? 1 : 0;
			});
			const FVoxelFarFieldStats Stats = Manager->GetFarFieldStats();
			AddInfo(FString::Printf(TEXT("%s: %d built patches, %d components (%d missing), %d drawn and shown of %d, %d hidden, %d seas shown; stats: %s"),
				*Prefix, Built, Manager->GetFarFieldComponentCount(), Missing, Shown, Drawn, Hidden, SeaShown, *Manager->GetFarFieldDebugString()));
			TestTrue(*FString::Printf(TEXT("%s: patches built"), *Prefix), Built > 6);
			TestEqual(*FString::Printf(TEXT("%s: a component per built patch"), *Prefix), Missing, 0);
			TestEqual(*FString::Printf(TEXT("%s: components for resident patches only"), *Prefix), Manager->GetFarFieldComponentCount(), Built);
			TestEqual(*FString::Printf(TEXT("%s: drawn patches shown"), *Prefix), Shown, Drawn);
			TestTrue(*FString::Printf(TEXT("%s: the stats report it"), *Prefix), Stats.bActive && Stats.DrawnPatches > 0 && Stats.MemoryMB > 0.0f);
			if (bExpectFarFieldSea)
			{
				TestTrue(*FString::Printf(TEXT("%s: sea sections shown"), *Prefix), SeaShown > 0);
			}
		}

		// voxel.World.FarField 0: no far field, the shell draws the sea; and back.
		Manager->SetFarFieldEnabled(false);
		TestFalse(*FString::Printf(TEXT("%s: off, no far field"), *Prefix), Manager->HasFarField());
		TestEqual(*FString::Printf(TEXT("%s: off, its components are gone"), *Prefix), Manager->GetFarFieldComponentCount(), 0);
		TestTrue(*FString::Printf(TEXT("%s: off, the ocean shell draws the sea"), *Prefix), Manager->HasPlanetOcean());
		Manager->SetFarFieldEnabled(true);
		TestTrue(*FString::Printf(TEXT("%s: on again"), *Prefix), Manager->HasFarField());
		TestEqual(*FString::Printf(TEXT("%s: on again, the sea as before"), *Prefix), Manager->HasPlanetOcean(), !bExpectFarFieldSea);

		Manager->Shutdown();
		Owner->Destroy();
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		Config->RemoveFromRoot();
	};

	RunWorld(TEXT("FarFieldBigPlanetWorld"), 1.0e6, /*bExpectFarFieldSea=*/true);
	RunWorld(TEXT("FarFieldSmallPlanetWorld"), 20000.0, /*bExpectFarFieldSea=*/false);
	return true;
}

/**
 * Visual polish F4.3: the far field for the camera a frame renders with. A chunk manager on a small planet (R 20,000,
 * terrain +-1,000) in a game world with a player controller whose view target is a camera actor, frames stepped in
 * UWorld::Tick's order: the chunk manager's tick (TG_PrePhysics, reading the camera manager's camera), the camera managers,
 * then the far field's camera tick (TG_PostUpdateWork). The window settles around the camera 1,200 over the datum at 45
 * degrees between +X and +Z; the camera then jumps 135 degrees, over the middle of the -X face, which lay under the
 * horizon all along. With voxel.World.FarFieldCameraUpdate that very frame draws the far field there: every direction out
 * to 9,000 uu around the camera falls on a drawn patch whose component shows it. Under the rule before that frame drew
 * the selection for the old camera, nothing there, and the next frame drew it. Moving under a chunk a frame runs no
 * second update; the camera tick is registered in TG_PostUpdateWork after the primary tick.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldPlanetCameraAfterCutTest,
	"VoxelWorlds.FarField.Planet.CameraAfterCut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldPlanetCameraAfterCutTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetFarFieldTestUtils;

	constexpr double Radius = 20000.0;
	constexpr double Altitude = 1200.0;
	constexpr float FrameSeconds = 1.0f / 60.0f;
	const FVector SiteUp = FVector(1, 0, 1).GetSafeNormal();
	const FVector FarUp = FVector(-1, 0, 0);
	for (const bool bCameraUpdate : { false, true })
	{
		const TCHAR* Rule = bCameraUpdate ? TEXT("updating for the frame's camera") : TEXT("the rule before");
		const FScopedCVar CameraUpdate(TEXT("voxel.World.FarFieldCameraUpdate"), bCameraUpdate ? TEXT("1") : TEXT("0"));

		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("FarFieldCameraAfterCutWorld"));
		FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
		WorldContext.SetCurrentWorld(World);
		// Actors initialised, so a player controller spawns its camera manager and joins the world's controllers.
		World->InitializeActorsForPlay(FURL());
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::SphericalPlanet;
		Config->WorldOrigin = FVector::ZeroVector;
		Config->WorldRadius = Radius;
		Config->PlanetHeightScale = 1600.0f;
		Config->BaseHeight = 0.0f;
		Config->PlanetMaxTerrainHeight = 1000.0f;
		Config->PlanetMaxTerrainDepth = 1000.0f;
		Config->MeshingMode = EMeshingMode::DualContouring;
		Config->ChunkSize = 32;
		Config->VoxelSize = 50.0f;
		Config->ViewDistance = 3000.0f;
		Config->bUseGPUGeneration = false;
		Config->bGenerateCollision = false;
		Config->bEnableScatter = false;
		Config->bEnableCaves = false;
		Config->bEnableBiomes = false;
		Config->bEnableWaterLevel = true;
		Config->WaterRadius = Radius - 200.0;
		Config->WaterMeshMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		Config->NoiseParams.Seed = 9001;
		Config->NoiseParams.Frequency = 0.0004f;
		Config->NoiseParams.Octaves = 3;

		AActor* Owner = World->SpawnActor<AActor>();
		UVoxelChunkManager* Manager = NewObject<UVoxelChunkManager>(Owner);
		Manager->RegisterComponent();
		FNullMeshRenderer Renderer;
		Renderer.Initialize(World, Config);
		Manager->Initialize(Config, new FDistanceBandLODStrategy(), &Renderer);

		// The camera: a camera actor, the controller's view target (as vox.Orbit's camera is).
		APlayerController* PC = World->SpawnActor<APlayerController>();
		ACameraActor* Camera = World->SpawnActor<ACameraActor>(SiteUp * (Radius + Altitude), FRotator::ZeroRotator);
		FVoxelPlanetFarField* FarField = Manager->GetFarFieldMutable();
		const bool bReady = FarField && PC && PC->PlayerCameraManager && Camera && World->GetFirstPlayerController() == PC;
		TestTrue(*FString::Printf(TEXT("%s: a far field, and a controller with a camera manager, the world's first"), Rule), bReady);
		if (bReady)
		{
			FarField->SetSynchronousBuilds(true);
			FarField->SetMaxBuildsInFlight(256);
			// A test controller has no local player: its camera manager updates only without client-side updates.
			PC->PlayerCameraManager->bUseClientSideCameraUpdates = false;
			PC->SetViewTarget(Camera);

			// The camera tick: after the primary tick, after the camera managers.
			Manager->RegisterAllComponentTickFunctions(true);
			FVoxelFarFieldCameraTickFunction& CameraTick = Manager->GetFarFieldCameraTickFunction();
			bool bAfterPrimary = false;
			for (const FTickPrerequisite& Prerequisite : CameraTick.GetPrerequisites())
			{
				bAfterPrimary |= Prerequisite.PrerequisiteTickFunction == &Manager->PrimaryComponentTick;
			}
			TestTrue(*FString::Printf(TEXT("%s: the camera tick is registered"), Rule), CameraTick.IsTickFunctionRegistered());
			TestTrue(*FString::Printf(TEXT("%s: the camera tick runs in TG_PostUpdateWork, the primary in TG_PrePhysics"), Rule),
				CameraTick.TickGroup == TG_PostUpdateWork && Manager->PrimaryComponentTick.TickGroup == TG_PrePhysics);
			TestTrue(*FString::Printf(TEXT("%s: the camera tick follows the primary tick"), Rule), bAfterPrimary);

			// A frame as UWorld::Tick runs it.
			auto Frame = [Manager, PC, &CameraTick, FrameSeconds]()
			{
				Manager->TickComponent(FrameSeconds, LEVELTICK_All, nullptr);
				PC->UpdateCameraManager(FrameSeconds);
				CameraTick.ExecuteTick(FrameSeconds, LEVELTICK_All, ENamedThreads::GameThread, FGraphEventRef());
			};
			// Directions out to 9,000 uu around a point on the ground with no drawn patch, or one whose component hides it.
			auto CountGaps = [Manager, FarField, Radius](const FVector& Up, int32& OutSamples)
			{
				FVector East, North;
				FVoxelCubeSphere::GetTangentFrame(Up, East, North);
				int32 Gaps = 0;
				OutSamples = 0;
				for (double Distance = 0.0; Distance <= 9000.0; Distance += 800.0)
				{
					const int32 Around = FMath::Max(8, FMath::CeilToInt32(UE_DOUBLE_TWO_PI * Distance / 800.0));
					for (int32 Step = 0; Step < Around; ++Step)
					{
						const double Theta = UE_DOUBLE_TWO_PI * Step / Around;
						const double Angle = Distance / Radius;
						const FVector Tangent = East * FMath::Cos(Theta) + North * FMath::Sin(Theta);
						const FVector Direction = (Up * FMath::Cos(Angle) + Tangent * FMath::Sin(Angle)).GetSafeNormal();
						const FVoxelPlanetFarField::FPatchState* Patch = FarField->FindDrawnPatch(Direction);
						const UProceduralMeshComponent* Component = Patch ? Manager->GetFarFieldComponent(Patch->Key.Pack()) : nullptr;
						++OutSamples;
						Gaps += (!Component || Component->GetNumSections() == 0 || !Component->IsMeshSectionVisible(0)) ? 1 : 0;
					}
				}
				return Gaps;
			};

			// Settle: the window loaded and the far field built around the camera.
			int32 IdleFrames = 0;
			const double Deadline = FPlatformTime::Seconds() + 120.0;
			while (IdleFrames < 30 && FPlatformTime::Seconds() < Deadline)
			{
				Frame();
				FPlatformProcess::Sleep(0.002f);
				const bool bIdle = Manager->GetPendingGenerationCount() == 0 && Manager->GetPendingMeshingCount() == 0
					&& Manager->GetAsyncGenerationInProgressCount() == 0 && Manager->GetAsyncMeshingInProgressCount() == 0
					&& Manager->GetPendingMeshUploadCount() == 0 && Manager->GetSeamJobsInFlightCount() == 0 && FarField->GetBuildsPending() == 0;
				IdleFrames = bIdle ? IdleFrames + 1 : 0;
			}
			TestTrue(*FString::Printf(TEXT("%s: settles"), Rule), IdleFrames >= 30);
			TestTrue(*FString::Printf(TEXT("%s: the camera manager's camera is the camera actor"), Rule),
				PC->PlayerCameraManager->GetCameraLocation().Equals(Camera->GetActorLocation(), 1.0));
			const FVoxelPlanetFarField::FPatchState* FarRoot = FarField->FindPatch(FVoxelFarFieldPatchKey(FVoxelCubeSphere::DirectionToSurface(FarUp).Face, 0, 0, 0).Pack());
			TestTrue(*FString::Printf(TEXT("%s: the destination's face lies under the horizon"), Rule), FarRoot && FarRoot->bCulled);

			// Moving under a chunk a frame: the tick's selection holds, no second update.
			FVector East, North;
			FVoxelCubeSphere::GetTangentFrame(SiteUp, East, North);
			const int64 UpdatesBefore = Manager->GetFarFieldStats().CameraUpdates;
			for (int32 Step = 1; Step <= 5; ++Step)
			{
				Camera->SetActorLocation(SiteUp * (Radius + Altitude) + East * (400.0 * Step));
				Frame();
			}
			TestEqual(*FString::Printf(TEXT("%s: no second update while moving 400 uu a frame"), Rule),
				Manager->GetFarFieldStats().CameraUpdates, UpdatesBefore);

			// The cut: the camera over the -X face's middle.
			Camera->SetActorLocation(FarUp * (Radius + Altitude));
			Frame();
			int32 Samples = 0;
			const int32 FirstGaps = CountGaps(FarUp, Samples);
			const int64 CutUpdates = Manager->GetFarFieldStats().CameraUpdates - UpdatesBefore;
			const FVoxelPlanetFarField::FPatchState* Drawn = FarField->FindDrawnPatch(FarUp);
			Frame();
			int32 SecondSamples = 0;
			const int32 SecondGaps = CountGaps(FarUp, SecondSamples);
			AddInfo(FString::Printf(TEXT("%s: the frame of the cut: %d of %d directions without the far field (under the camera %s), %lld update(s) for the frame's camera; the next frame: %d"),
				Rule, FirstGaps, Samples, Drawn ? *Drawn->Key.ToString() : TEXT("nothing"), CutUpdates, SecondGaps));
			TestEqual(*FString::Printf(TEXT("%s: the frame after the cut draws the far field there"), Rule), SecondGaps, 0);
			if (bCameraUpdate)
			{
				TestEqual(*FString::Printf(TEXT("%s: the frame of the cut draws the far field there"), Rule), FirstGaps, 0);
				TestEqual(*FString::Printf(TEXT("%s: one update for the frame's camera"), Rule), CutUpdates, int64(1));
			}
			else
			{
				// Documents the frame this closes (F4.3): if this fails the rule before no longer lags the camera here.
				TestEqual(*FString::Printf(TEXT("%s: the frame of the cut draws the old camera's selection, nothing there"), Rule), FirstGaps, Samples);
				TestEqual(*FString::Printf(TEXT("%s: no update for the frame's camera"), Rule), CutUpdates, int64(0));
			}
		}

		Manager->Shutdown();
		if (PC)
		{
			PC->Destroy();
		}
		if (Camera)
		{
			Camera->Destroy();
		}
		Owner->Destroy();
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		Config->RemoveFromRoot();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
