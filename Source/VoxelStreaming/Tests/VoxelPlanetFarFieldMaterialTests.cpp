// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"
#include "VoxelPlanetFarField.h"
#include "VoxelCubeSphere.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "SphericalPlanetWorldMode.h"
#include "IVoxelWorldMode.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G45 (WORLD_MODES_REVIEW_PLAN.md §14.1, §20.27): the planet far field's material borders. Each quad used to carry one material,
// the most common of its corners' picks, so a border between materials was a staircase of quads (a few km a step at the
// horizon, every biome border from orbit). Now (voxel.World.FarFieldMaterialBlend 1) a vertex samples the surface's
// material mix (FVoxelFarFieldSurface::SampleMaterialMix: a blend zone's two leading materials and the second's share,
// what the voxels' dithered picks cover there) and a quad carries its corners' two leading materials, the weight
// interpolating across it; the terrain material dithers between them (M_VoxelMaster's FarFieldMaterialPick).
//
// - MaterialMix: in blend zones a vertex's mix is what the voxels around it take: the share of the per-voxel picks
//   (FVoxelFarFieldSurface::SampleConditions, generation's) over 48 x 48 voxels taking the second material, of the two,
//   is the mix's weight, and the two materials hold nearly all the picks.
// - MaterialBlend: the patch mesh keeps both materials flat per triangle (UV1.x and colour R the same at its three
//   vertices), a vertex whose quad's pair holds its mix carries the mix's weight, and blend quads exist (grid points where
//   two quads draw a material differently, three materials meeting, are counted); with the cvar at 0 the old encoding
//   (colour R the material, alpha 1) and one material a quad.
// ---------------------------------------------------------------------------

namespace VoxelFarFieldMaterialTestUtils
{
	/** Sets voxel.World.FarFieldMaterialBlend for a scope. */
	struct FScopedBlend
	{
		IConsoleVariable* Var = nullptr;
		int32 Previous = 1;

		explicit FScopedBlend(int32 Value)
			: Var(IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.World.FarFieldMaterialBlend")))
		{
			if (Var)
			{
				Previous = Var->GetInt();
				Var->Set(Value, ECVF_SetByConsole);
			}
		}

		~FScopedBlend()
		{
			if (Var)
			{
				Var->Set(Previous, ECVF_SetByConsole);
			}
		}
	};

	/**
	 * Review_Planet's size and terrain (R 250,000, the demo's noise, +-6,500) with the default biomes blending widely
	 * (BiomeBlendWidth 0.5) and its sea: a far field and its surface for building single patches.
	 */
	struct FRig
	{
		UVoxelWorldConfiguration* Config = nullptr;
		UVoxelBiomeConfiguration* Biomes = nullptr;
		TSharedPtr<IVoxelWorldMode> Mode;
		TSharedPtr<const FVoxelFarFieldSurface> Surface;
		FVoxelFarFieldSettings Settings;
		const TArray<FVoxelConditioningZone> NoZones;

		FRig()
		{
			Biomes = NewObject<UVoxelBiomeConfiguration>();
			Biomes->AddToRoot();
			Biomes->InitializeDefaults();
			Biomes->BiomeBlendWidth = 0.5f;

			Config = NewObject<UVoxelWorldConfiguration>();
			Config->AddToRoot();
			Config->WorldMode = EWorldMode::SphericalPlanet;
			Config->MeshingMode = EMeshingMode::DualContouring;
			Config->VoxelSize = 75.0f;
			Config->ChunkSize = 64;
			Config->ViewDistance = 40000.0f;
			Config->WorldRadius = 250000.0f;
			Config->PlanetMaxTerrainHeight = 6500.0f;
			Config->PlanetMaxTerrainDepth = 6500.0f;
			Config->PlanetHeightScale = 6000.0f;
			Config->NoiseParams.Frequency = 2.5e-5f;
			Config->NoiseParams.Octaves = 3;
			Config->bEnableBiomes = true;
			Config->BiomeConfiguration = Biomes;
			Config->bEnableWaterLevel = true;
			Config->WaterRadius = 250000.0f - 500.0f;

			Mode = FVoxelWorldModeFactory::Create(*Config);
			Surface = FVoxelFarFieldSurface::Make(*Config, Mode);
			const FSphericalPlanetWorldMode& Planet = Surface->GetPlanet();
			Settings.Center = Planet.GetPlanetParams().PlanetCenter;
			Settings.Radius = Planet.GetPlanetParams().PlanetRadius;
			Settings.GridOrigin = Config->WorldOrigin;
			Settings.ChunkWorldSize = Config->GetChunkWorldSize();
			Settings.VoxelSize = Config->VoxelSize;
			Settings.PatchQuads = 16;
			Settings.Sink = Config->VoxelSize;
			float MinAltitude = 0.0f, MaxAltitude = 0.0f;
			FSphericalPlanetWorldMode::GetDisplacementBounds(Planet.GetTerrainParams(), &Planet.GetBiomeSnapshot(), MinAltitude, MaxAltitude);
			Settings.MinAltitude = MinAltitude;
			Settings.MaxAltitude = MaxAltitude;
			Settings.bSea = true;
			Settings.SeaRadius = Config->WaterRadius;
			Settings.SeaMargin = 2.0 * Config->VoxelSize;
			Settings.BandLevel = FVoxelFarFieldSettings::ComputeBandLevel(Settings.Radius, Settings.ChunkWorldSize);
			Settings.ZoneGatherMax = 32.0 * Settings.ChunkWorldSize / Settings.Radius;
		}

		~FRig()
		{
			Config->RemoveFromRoot();
			Biomes->RemoveFromRoot();
		}

		FVoxelSurfaceMaterialMix Mix(const FVector& Direction) const
		{
			float Continentalness = 0.0f;
			const float Altitude = Surface->SampleAltitude(FVoxelFarFieldSample::OfDirection(Direction), NoZones, &Continentalness);
			return Surface->SampleMaterialMix(FVoxelFarFieldSample::OfDirection(Direction), Altitude, Continentalness);
		}

		uint8 Pick(const FVector& Direction) const
		{
			float Continentalness = 0.0f;
			const float Altitude = Surface->SampleAltitude(FVoxelFarFieldSample::OfDirection(Direction), NoZones, &Continentalness);
			uint8 Material = 0, Biome = 0;
			Surface->SampleConditions(FVoxelFarFieldSample::OfDirection(Direction), Altitude, Continentalness, Material, Biome);
			return Material;
		}

		/** Directions in blend zones (a mix weight in [MinWeight, 0.5]), from a fixed random sweep of the sphere. */
		TArray<FVector> FindBlendDirections(int32 Count, float MinWeight) const
		{
			TArray<FVector> Out;
			FRandomStream Random(4501);
			for (int32 i = 0; i < 20000 && Out.Num() < Count; ++i)
			{
				const FVector Direction = Random.GetUnitVector();
				const FVoxelSurfaceMaterialMix M = Mix(Direction);
				if (M.Materials[0] != M.Materials[1] && M.Weight >= MinWeight)
				{
					Out.Add(Direction);
				}
			}
			return Out;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldPlanetMaterialMixTest,
	"VoxelWorlds.FarField.Planet.MaterialMix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldPlanetMaterialMixTest::RunTest(const FString& Parameters)
{
	using namespace VoxelFarFieldMaterialTestUtils;
	FRig Rig;
	const TArray<FVector> Sites = Rig.FindBlendDirections(24, 0.15f);
	AddInfo(FString::Printf(TEXT("%d blend-zone sites (mix weight 0.15 to 0.5) found"), Sites.Num()));
	if (!TestTrue(TEXT("Blend zones on the test planet (non-vacuous)"), Sites.Num() >= 12))
	{
		return true;
	}

	// Around each site, a 48 x 48 grid of directions a voxel apart in its tangent plane: the voxels' picks there.
	constexpr int32 Grid = 48;
	const double Step = Rig.Config->VoxelSize / Rig.Settings.Radius;
	double SumError = 0.0, WorstError = 0.0, WorstOutside = 0.0;
	for (const FVector& Site : Sites)
	{
		const FVoxelSurfaceMaterialMix M = Rig.Mix(Site);
		FVector East, North;
		FVoxelCubeSphere::GetTangentFrame(Site, East, North);
		int32 First = 0, Second = 0, Outside = 0;
		for (int32 J = 0; J < Grid; ++J)
		{
			for (int32 I = 0; I < Grid; ++I)
			{
				const FVector Direction = (Site + East * ((I - Grid / 2) * Step) + North * ((J - Grid / 2) * Step)).GetSafeNormal();
				const uint8 Material = Rig.Pick(Direction);
				First += Material == M.Materials[0] ? 1 : 0;
				Second += Material == M.Materials[1] ? 1 : 0;
				Outside += (Material != M.Materials[0] && Material != M.Materials[1]) ? 1 : 0;
			}
		}
		// The second's share of the two (the mix's weight is of the two: a third biome's material is left out).
		const double Share = (First + Second) > 0 ? static_cast<double>(Second) / (First + Second) : 0.0;
		const double Error = FMath::Abs(Share - M.Weight);
		SumError += Error;
		WorstError = FMath::Max(WorstError, Error);
		WorstOutside = FMath::Max(WorstOutside, static_cast<double>(Outside) / (Grid * Grid));
	}
	const double MeanError = SumError / Sites.Num();
	AddInfo(FString::Printf(TEXT("The second material's share of the voxels' picks against the mix's weight: off by %.3f on average, %.3f at worst; ")
		TEXT("picks of neither material up to %.3f of a site"), MeanError, WorstError, WorstOutside));
	TestTrue(TEXT("The mix's weight is the share of the voxels taking the second material (mean error under 0.05)"), MeanError < 0.05);
	TestTrue(TEXT("... at every site (error under 0.15)"), WorstError < 0.15);
	TestTrue(TEXT("The two materials hold the picks (other materials under 10%)"), WorstOutside < 0.1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFarFieldPlanetMaterialBlendTest,
	"VoxelWorlds.FarField.Planet.MaterialBlend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFarFieldPlanetMaterialBlendTest::RunTest(const FString& Parameters)
{
	using namespace VoxelFarFieldMaterialTestUtils;
	FRig Rig;
	const TArray<FVector> Sites = Rig.FindBlendDirections(4, 0.2f);
	if (!TestTrue(TEXT("Blend zones on the test planet (non-vacuous)"), Sites.Num() >= 2))
	{
		return true;
	}

	int32 SeamsByMode[2] = { 0, 0 };
	for (int32 Blend = 0; Blend < 2; ++Blend)
	{
		FScopedBlend Scoped(Blend);
		int32 Triangles = 0, MixedPrimary = 0, MixedSecondary = 0, BlendTriangles = 0, Weighted = 0, Vertices = 0;
		int32 OldEncoding = 0, Checked = 0, WrongWeight = 0, Shared = 0, Seams = 0;
		for (const FVector& Site : Sites)
		{
			const FVoxelSurfaceCoord Coord = FVoxelCubeSphere::DirectionToSurface(Site);
			for (const int32 Level : { FMath::Max(Rig.Settings.BandLevel - 6, 2), FMath::Max(Rig.Settings.BandLevel - 3, 3) })
			{
				const int32 Cells = 1 << Level;
				const FVoxelFarFieldPatchKey Key(Coord.Face, Level,
					FMath::Clamp(FMath::FloorToInt32((Coord.UV.X + 1.0) / 2.0 * Cells), 0, Cells - 1),
					FMath::Clamp(FMath::FloorToInt32((Coord.UV.Y + 1.0) / 2.0 * Cells), 0, Cells - 1));
				FVoxelFarFieldPatchMesh Mesh;
				FVoxelPlanetFarField::BuildPatch(Rig.Settings, *Rig.Surface, Rig.NoZones, Key, Mesh);
				const FVoxelFarFieldSection& Terrain = Mesh.Terrain;

				// Per triangle: both materials flat; a blend triangle where the corners' weights differ.
				for (int32 T = 0; T + 2 < Terrain.Triangles.Num(); T += 3)
				{
					const int32 A = Terrain.Triangles[T], B = Terrain.Triangles[T + 1], C = Terrain.Triangles[T + 2];
					++Triangles;
					MixedPrimary += (Terrain.UV1[A].X != Terrain.UV1[B].X || Terrain.UV1[A].X != Terrain.UV1[C].X) ? 1 : 0;
					MixedSecondary += (Terrain.Colors[A].R != Terrain.Colors[B].R || Terrain.Colors[A].R != Terrain.Colors[C].R) ? 1 : 0;
					BlendTriangles += (Terrain.Colors[A].A != Terrain.Colors[B].A || Terrain.Colors[A].A != Terrain.Colors[C].A) ? 1 : 0;
				}
				// Per vertex: the old encoding; with the blend, a vertex whose quad's pair holds its own mix carries the mix's weight
				// (what the voxels take there). Per grid point: where two quads draw a material's share differently by over a
				// quarter (three materials meeting: a quad keeps two).
				TMap<FVector3f, TArray<int32>> AtPoint;
				for (int32 V = 0; V < Mesh.TerrainSurfaceVertices; ++V)
				{
					++Vertices;
					const FColor& Color = Terrain.Colors[V];
					Weighted += Color.A < 255 ? 1 : 0;
					OldEncoding += (Color.A == 255 && static_cast<float>(Color.R) == Terrain.UV1[V].X) ? 1 : 0;
					AtPoint.FindOrAdd(Terrain.Positions[V]).Add(V);
					const uint8 P = static_cast<uint8>(Terrain.UV1[V].X), S = Color.R;
					if (Blend && P != S)
					{
						const FVector Direction = (Mesh.Origin + FVector(Terrain.Positions[V]) - Rig.Settings.Center).GetSafeNormal();
						const FVoxelSurfaceMaterialMix M = Rig.Mix(Direction);
						const bool bTwo = M.Materials[0] != M.Materials[1];
						const bool bHeld = (M.Materials[0] == P || M.Materials[0] == S) && (!bTwo || M.Materials[1] == P || M.Materials[1] == S);
						if (bHeld)
						{
							const float ShareS = (M.Materials[0] == S ? 1.0f - (bTwo ? M.Weight : 0.0f) : 0.0f) + (bTwo && M.Materials[1] == S ? M.Weight : 0.0f);
							++Checked;
							const float W = (255 - Color.A) / 255.0f;
							// A sliver of tolerance: the vertex's float position re-derives its direction a rounding off the grid's.
							WrongWeight += FMath::Abs(W - ShareS) > 2.5f / 255.0f ? 1 : 0;
						}
					}
				}
				for (const TPair<FVector3f, TArray<int32>>& Point : AtPoint)
				{
					if (Point.Value.Num() < 2)
					{
						continue;
					}
					++Shared;
					auto ShareOf = [&](int32 V, uint8 Material)
					{
						const uint8 P = static_cast<uint8>(Terrain.UV1[V].X), S = Terrain.Colors[V].R;
						const float W = P != S ? (255 - Terrain.Colors[V].A) / 255.0f : 0.0f;
						return (P == Material ? 1.0f - W : 0.0f) + (S == Material && S != P ? W : 0.0f);
					};
					float Worst = 0.0f;
					for (int32 i = 0; i < Point.Value.Num(); ++i)
					{
						for (int32 j = i + 1; j < Point.Value.Num(); ++j)
						{
							const int32 Vi = Point.Value[i], Vj = Point.Value[j];
							for (const uint8 Material : { static_cast<uint8>(Terrain.UV1[Vi].X), Terrain.Colors[Vi].R, static_cast<uint8>(Terrain.UV1[Vj].X), Terrain.Colors[Vj].R })
							{
								Worst = FMath::Max(Worst, FMath::Abs(ShareOf(Vi, Material) - ShareOf(Vj, Material)));
							}
						}
					}
					Seams += Worst > 0.25f ? 1 : 0;
				}
			}
		}
		AddInfo(FString::Printf(TEXT("%s: %d triangles (%d with the material or %d the second material mixed, %d blending), %d surface vertices (%d weighted, %d in the old encoding), ")
			TEXT("%d vertices whose pair holds their mix (%d with another weight), %d grid points shared by quads (%d where a material's share differs by over 0.25)"),
			Blend ? TEXT("Blend") : TEXT("No blend"), Triangles, MixedPrimary, MixedSecondary, BlendTriangles, Vertices, Weighted, OldEncoding, Checked, WrongWeight, Shared, Seams));
		SeamsByMode[Blend] = Seams;
		TestEqual(*FString::Printf(TEXT("%s: one material per triangle"), Blend ? TEXT("Blend") : TEXT("No blend")), MixedPrimary, 0);
		TestEqual(*FString::Printf(TEXT("%s: one second material per triangle"), Blend ? TEXT("Blend") : TEXT("No blend")), MixedSecondary, 0);
		if (Blend)
		{
			TestTrue(TEXT("Blend: quads blend across the zones (non-vacuous)"), BlendTriangles > 0 && Weighted > 0);
			TestTrue(TEXT("Blend: vertices whose pair holds their mix (non-vacuous)"), Checked > 0);
			TestEqual(TEXT("Blend: such a vertex carries its mix's weight"), WrongWeight, 0);
		}
		else
		{
			TestEqual(TEXT("No blend: every vertex in the old encoding (colour R the material, alpha 1)"), OldEncoding, Vertices);
			TestEqual(TEXT("No blend: no triangle blends"), BlendTriangles, 0);
		}
	}
	// Every material border was a hard edge between quads; with the blend only where three materials meet.
	TestTrue(*FString::Printf(TEXT("Hard edges between quads: under a quarter of before (%d against %d)"), SeamsByMode[1], SeamsByMode[0]),
		SeamsByMode[0] > 0 && SeamsByMode[1] * 4 < SeamsByMode[0]);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
