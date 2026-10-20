// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Materials/Material.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelEditManager.h"
#include "VoxelEditTypes.h"
#include "VoxelSeamRegistry.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelWaterPropagation.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G58 (WORLD_MODES_REVIEW_PLAN.md section 14.1, section 19.14). Edit-driven water propagation (UVoxelWaterPropagation) read
// the generated voxels without the edit layer: a voxel dug under the sea still read solid, so a dig never flooded, and a
// flood would have set each voxel to its generated value plus the flag, undoing an edit there. Only brushes were scanned
// for seeds; an operation of single-voxel edits (a POI's terraform, a dungeon stamp) reports radius 0 and seeded nothing.
//
// - Streaming.WaterPropagation.Plane / .Planet: a real chunk manager streams a small world with a sea (the plane's
//   WorldOrigin off the voxel grid by a fraction of a voxel). At three seabed sites a few voxels under the sea:
//   A. A player dig on the seabed (a sphere brush): every voxel it dug floods, and keeps the dig's density and material.
//   B. A system edit digs a pocket under the seabed (air with a density and material of its own), another fills its
//      bottom: the sealed pocket stays dry. A system shaft (single-voxel edits, radius 0) opens it to the sea: the pocket
//      and the shaft flood, the fill stays solid, every voxel keeps its density and material.
//   C. A player fill (a brush) plugs the sea over a spot of seabed; a system edit digs a cavity under the plug. The plug
//      keeps the water flag of the sea it filled but is solid, so the cavity stays dry and the plug solid.
//   Around each site no voxel changes but by taking the flag, and only air below the sea takes it.
//   D. A system dig of a seabed chunk 12,000 uu away, which has no voxel data yet (a POI's terraform at stream-in): its scan
//      waits for the chunk's water (its first mesh, which pulls the flags of a chunk under a heightmap sea from its
//      neighbours), and once the viewer streams the chunk in, the dig floods with its density and material.
//
// G59 (section 19.15). A flood's water lives in the edit layer, which the water tiles and the chunk manager's pull across
// chunk faces (PropagateWaterFromNeighbors) did not read: a dug channel's water drew no tile, a flood that stopped at a
// chunk whose water was not in place did not continue into it when it loaded, and a dry pocket dug beside such a chunk did
// not flood from the water it loaded with.
//
// - Streaming.WaterPropagation.ChunkArrival.Plane / .Planet: two sites on one chunk face, A on this side and N on the
//   other, in the sea at least 5 voxels deep. (i) With both chunks loaded, a sealed dry pocket is dug in N against the face,
//   behind a solid door on A's side; the viewer moves so N unloads and A stays; the door is dug open, so A's sea floods it
//   and the flood stops at the face. (ii) With N unloaded, a dry pocket walled in on A's side is dug against the face. Once
//   the viewer comes back and N loads, its water settles: the flood continues through the face into N's pocket, and A's
//   pocket floods from N's sea. No voxel changes but by taking the flag.
// - Streaming.WaterPropagation.Tiles.Plane: a channel dug from the sea inland, 3 voxels below the sea, floods; its inland
//   columns, which no water tile covered (the tiles dilate the wet columns by 3), are covered by the tile afterwards.
// ---------------------------------------------------------------------------

namespace VoxelWaterPropagationTest
{
	FVector OriginOf(const VoxelTesting::FTestChunkWorld& W) { return W.Config->WorldOrigin; }

	FVector UpAt(const VoxelTesting::FTestChunkWorld& W, const FVector& P, bool bPlanet)
	{
		return bPlanet ? (P - OriginOf(W)).GetSafeNormal() : FVector::UpVector;
	}

	/** The voxel holding a world position, by its global index relative to WorldOrigin (the edit manager's mapping). */
	FIntVector KeyOf(const VoxelTesting::FTestChunkWorld& W, const FVector& P)
	{
		const FVector Rel = (P - OriginOf(W)) / W.Config->VoxelSize;
		return FIntVector(FMath::FloorToInt32(Rel.X), FMath::FloorToInt32(Rel.Y), FMath::FloorToInt32(Rel.Z));
	}

	FVector CenterOf(const VoxelTesting::FTestChunkWorld& W, const FIntVector& Key)
	{
		return OriginOf(W) + (FVector(Key) + FVector(0.5)) * W.Config->VoxelSize;
	}

	FVoxelData Merged(const VoxelTesting::FTestChunkWorld& W, const FIntVector& Key)
	{
		return W.Manager->GetEditMergedVoxelAtWorldPosition(CenterOf(W, Key));
	}

	FVoxelData Generated(const VoxelTesting::FTestChunkWorld& W, const FIntVector& Key)
	{
		return W.Manager->GetVoxelAtWorldPosition(CenterOf(W, Key));
	}

	bool HasData(const VoxelTesting::FTestChunkWorld& W, const FIntVector& Key)
	{
		return W.Manager->HasChunkVoxelData(W.Manager->WorldToChunkCoord(CenterOf(W, Key)));
	}

	/** Below the sea as generation tests it: the voxel's sample point (its cell's min corner), on the grid for the plane. */
	bool BelowSea(const VoxelTesting::FTestChunkWorld& W, const FIntVector& Key, bool bPlanet)
	{
		const FVector GridPos = FVector(Key) * W.Config->VoxelSize;
		return bPlanet ? GridPos.Size() <= W.Config->WaterRadius : GridPos.Z <= W.Config->WaterLevel;
	}

	/** Water: water-flagged air. */
	bool IsWater(const FVoxelData& V) { return V.IsAir() && V.HasWaterFlag(); }

	/** Every voxel whose cell centre lies within RadiusVoxels of Center. */
	TArray<FIntVector> Ball(const VoxelTesting::FTestChunkWorld& W, const FVector& Center, float RadiusVoxels)
	{
		const float VS = W.Config->VoxelSize;
		const int32 Reach = FMath::CeilToInt32(RadiusVoxels) + 1;
		const FIntVector C = KeyOf(W, Center);
		TArray<FIntVector> Keys;
		for (int32 Z = -Reach; Z <= Reach; ++Z)
		for (int32 Y = -Reach; Y <= Reach; ++Y)
		for (int32 X = -Reach; X <= Reach; ++X)
		{
			const FIntVector Key = C + FIntVector(X, Y, Z);
			if (FVector::DistSquared(CenterOf(W, Key), Center) <= FMath::Square(RadiusVoxels * VS))
			{
				Keys.Add(Key);
			}
		}
		return Keys;
	}

	/** The voxels a sphere brush of RadiusVoxels at Center edits: Center's voxel plus every whole-voxel offset within the radius (UVoxelEditManager::ApplyBrushEdit). */
	TArray<FIntVector> BrushKeys(const VoxelTesting::FTestChunkWorld& W, const FVector& Center, float RadiusVoxels)
	{
		const int32 Reach = FMath::FloorToInt32(RadiusVoxels);
		const FIntVector C = KeyOf(W, Center);
		TArray<FIntVector> Keys;
		for (int32 Z = -Reach; Z <= Reach; ++Z)
		for (int32 Y = -Reach; Y <= Reach; ++Y)
		for (int32 X = -Reach; X <= Reach; ++X)
		{
			if (FVector(X, Y, Z).Size() <= RadiusVoxels)
			{
				Keys.Add(C + FIntVector(X, Y, Z));
			}
		}
		return Keys;
	}

	/** Every voxel whose cell centre lies within RadiusVoxels of the segment A-B. */
	TArray<FIntVector> Tube(const VoxelTesting::FTestChunkWorld& W, const FVector& A, const FVector& B, float RadiusVoxels)
	{
		const float VS = W.Config->VoxelSize;
		TSet<FIntVector> Keys;
		const int32 Steps = FMath::Max(1, FMath::CeilToInt32(FVector::Dist(A, B) / (0.25f * VS)));
		for (int32 s = 0; s <= Steps; ++s)
		{
			for (const FIntVector& Key : Ball(W, FMath::Lerp(A, B, double(s) / Steps), RadiusVoxels))
			{
				Keys.Add(Key);
			}
		}
		return Keys.Array();
	}

	/** Tick until streaming (generation, meshing, uploads, seams) and the water propagation have been idle for 90 ticks. */
	bool SettleWater(VoxelTesting::FTestChunkWorld& W, double TimeoutSeconds = 120.0)
	{
		const UVoxelWaterPropagation* Water = W.Manager->GetWaterPropagation();
		const FVoxelSeamRegistry* Seams = W.Manager->GetSeamRegistry();
		int32 IdleTicks = 0;
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (IdleTicks < 90 && FPlatformTime::Seconds() < Deadline)
		{
			W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			const bool bIdle = W.Manager->GetPendingGenerationCount() == 0 && W.Manager->GetPendingMeshingCount() == 0
				&& W.Manager->GetAsyncGenerationInProgressCount() == 0 && W.Manager->GetAsyncMeshingInProgressCount() == 0
				&& W.Manager->GetPendingMeshUploadCount() == 0 && W.Manager->GetSeamJobsInFlightCount() == 0
				&& (!Seams || Seams->GetStats().JobQueueDepth == 0)
				&& (!Water || !Water->HasPendingWork());
			IdleTicks = bIdle ? IdleTicks + 1 : 0;
			FPlatformProcess::Sleep(0.002f);
		}
		return IdleTicks >= 90;
	}

	/** One system operation (single-voxel Set edits, so the broadcast has no radius) writing Make(key) at each key. */
	int32 SystemSet(VoxelTesting::FTestChunkWorld& W, const TArray<FIntVector>& Keys, TFunctionRef<FVoxelData(const FIntVector&)> Make)
	{
		UVoxelEditManager* Edits = W.Manager->GetEditManager();
		const EEditSource Previous = Edits->GetEditSource();
		Edits->SetEditSource(EEditSource::System);
		Edits->BeginEditOperation(TEXT("G58 test"));
		int32 Count = 0;
		for (const FIntVector& Key : Keys)
		{
			Count += Edits->ApplyEdit(CenterOf(W, Key), Make(Key), EEditMode::Set) ? 1 : 0;
		}
		Edits->EndEditOperation();
		Edits->SetEditSource(Previous);
		return Count;
	}

	/**
	 * A player's sphere brush at full strength: Subtract digs, Add fills with stone. Its density change falls off from the
	 * centre (Sharp: (1 - t)^2; Smooth: Hermite, half at half the radius), so only the voxels near the centre turn.
	 */
	int32 PlayerBrush(VoxelTesting::FTestChunkWorld& W, const FVector& Center, float RadiusVoxels, EEditMode Mode, EVoxelBrushFalloff Falloff)
	{
		UVoxelEditManager* Edits = W.Manager->GetEditManager();
		const EEditSource Previous = Edits->GetEditSource();
		Edits->SetEditSource(EEditSource::Player);
		FVoxelBrushParams Brush;
		Brush.Shape = EVoxelBrushShape::Sphere;
		Brush.Radius = RadiusVoxels * W.Config->VoxelSize;
		Brush.Strength = 1.0f;
		Brush.FalloffType = Falloff;
		Brush.DensityDelta = 255;
		Brush.MaterialID = EVoxelMaterial::Stone;
		const int32 Count = Edits->ApplyBrushEdit(Center, Brush, Mode);
		Edits->SetEditSource(Previous);
		return Count;
	}

	/** The edit-merged voxels of some keys. */
	TMap<FIntVector, FVoxelData> Capture(const VoxelTesting::FTestChunkWorld& W, const TArray<FIntVector>& Keys)
	{
		TMap<FIntVector, FVoxelData> Out;
		for (const FIntVector& Key : Keys)
		{
			Out.Add(Key, Merged(W, Key));
		}
		return Out;
	}

	/**
	 * Check that between two captures no voxel changed but by taking the water flag, and that only air below the sea
	 * without it took it.
	 *
	 * @return The number of voxels that took the flag
	 */
	int32 CheckOnlyFlagsAdded(FAutomationTestBase& Test, const TCHAR* What, const VoxelTesting::FTestChunkWorld& W, bool bPlanet,
		const TMap<FIntVector, FVoxelData>& Before, const TMap<FIntVector, FVoxelData>& After)
	{
		int32 Gained = 0, Changed = 0, Wrong = 0, Lost = 0;
		for (const TPair<FIntVector, FVoxelData>& Pair : Before)
		{
			const FVoxelData* Now = After.Find(Pair.Key);
			if (!Now)
			{
				continue;
			}
			const FVoxelData& Was = Pair.Value;
			FVoxelData WasFlagged = Was;
			WasFlagged.SetWaterFlag(true);
			FVoxelData NowFlagged = *Now;
			NowFlagged.SetWaterFlag(true);
			if (!(WasFlagged == NowFlagged))
			{
				++Changed; // density, material, biome or other metadata
			}
			if (Was.HasWaterFlag() && !Now->HasWaterFlag())
			{
				++Lost;
			}
			if (!Was.HasWaterFlag() && Now->HasWaterFlag())
			{
				++Gained;
				Wrong += (Was.IsAir() && BelowSea(W, Pair.Key, bPlanet)) ? 0 : 1;
			}
		}
		Test.TestEqual(FString::Printf(TEXT("%s: voxels whose density, material or biome changed"), What), Changed, 0);
		Test.TestEqual(FString::Printf(TEXT("%s: voxels that lost the water flag"), What), Lost, 0);
		Test.TestEqual(FString::Printf(TEXT("%s: voxels that took the flag but were not air below the sea"), What), Wrong, 0);
		return Gained;
	}

	double DepthUnderSea(const VoxelTesting::FTestChunkWorld& W, bool bPlanet, const FVector& Surface)
	{
		return bPlanet ? W.Config->WaterRadius - (Surface - OriginOf(W)).Size() : W.Config->WaterLevel - Surface.Z;
	}

	/**
	 * Seabed sites on a 200 uu grid, 31 x 31, around a base point (the plane's origin; the planet's +X face at the equator)
	 * moved Shift uu along the first axis across local up, nearest first: the seabed 5-10 voxels under the sea, the ground
	 * within 75 uu of the site's altitude 150 uu either way across local up (a slope under 0.5, so the cavities dug below
	 * stay closed by rock at their sides); with bInterior, the site's voxel at least 5 voxels inside its chunk on every axis.
	 */
	TArray<FVector> FindSeabedSites(const VoxelTesting::FTestChunkWorld& W, bool bPlanet, double Shift, bool bInterior)
	{
		const float VS = W.Config->VoxelSize;
		const int32 CS = W.Config->ChunkSize;
		struct FCandidate { FVector Site; double Score; };
		TArray<FCandidate> Candidates;
		for (int32 j = -15; j <= 15; ++j)
		for (int32 i = -15; i <= 15; ++i)
		{
			const FVector Base = bPlanet ? OriginOf(W) + FVector(W.Config->WorldRadius, Shift + i * 200.0, j * 200.0)
				: OriginOf(W) + FVector(Shift + i * 200.0, j * 200.0, 0.0);
			const FVector Site = W.Manager->GetGeneratedSurfacePoint(Base);
			const double Depth = DepthUnderSea(W, bPlanet, Site);
			if (Depth < 5.0 * VS || Depth > 10.0 * VS)
			{
				continue;
			}
			bool bFlat = true;
			for (const FVector& Across : { FVector(0, 150, 0), FVector(0, -150, 0), FVector(0, 0, 150), FVector(0, 0, -150) })
			{
				const FVector Offset = bPlanet ? Across : FVector(Across.Y, Across.Z, 0.0);
				bFlat &= FMath::Abs(DepthUnderSea(W, bPlanet, W.Manager->GetGeneratedSurfacePoint(Base + Offset)) - Depth) <= 75.0;
			}
			if (bInterior)
			{
				const FIntVector Key = KeyOf(W, Site);
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					const int32 Local = Key[Axis] - FMath::FloorToInt32(double(Key[Axis]) / CS) * CS;
					bFlat &= Local >= 5 && Local <= CS - 6;
				}
			}
			if (bFlat)
			{
				Candidates.Add({ Site, FMath::Square(double(i)) + FMath::Square(double(j)) });
			}
		}
		Candidates.Sort([](const FCandidate& L, const FCandidate& R) { return L.Score < R.Score; });
		TArray<FVector> Sites;
		for (const FCandidate& C : Candidates)
		{
			Sites.Add(C.Site);
		}
		return Sites;
	}

	/** Configure and start a small world with a sea (the plane's at Z 0 with WorldOrigin off the voxel grid, the planet's at the datum). */
	void StartSeaWorld(VoxelTesting::FTestChunkWorld& W, bool bPlanet, const TCHAR* WorldName, bool bWaterTiles = false)
	{
		W.Create(WorldName);
		if (bPlanet)
		{
			W.ConfigureSmallPlanet();
		}
		else
		{
			W.ConfigureSmallPlane();
			// Off the voxel grid by a fraction of a voxel in X and Y (Z stays 0: the two sea-plane conventions agree, G19), so
			// voxel keys relative to the origin and world positions differ.
			W.Config->WorldOrigin = FVector(3112.5, -1937.5, 0.0);
		}
		W.Config->bEnableWaterLevel = true;
		if (bPlanet)
		{
			W.Config->WaterRadius = W.Config->WorldRadius; // a sea at the datum
			W.Config->WaterLevel = 1.0e9f;                  // the Z plane plays no part on a planet
		}
		else
		{
			W.Config->WaterLevel = 0.0f;
		}
		if (bWaterTiles)
		{
			// The 2D water tiles run with a water material in a heightmap world (UVoxelChunkManager::UsesWaterTiles).
			W.Config->WaterMeshMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		}
		W.StartManager();
	}

	/** Configure and start a small world with a sea; three flat seabed sites 5-10 voxels under it, pairwise at least 900 uu apart. */
	bool StartWorld(VoxelTesting::FTestChunkWorld& W, bool bPlanet, TArray<FVector>& OutSites)
	{
		StartSeaWorld(W, bPlanet, bPlanet ? TEXT("WaterPropagationPlanet") : TEXT("WaterPropagationPlane"));

		for (const FVector& Site : FindSeabedSites(W, bPlanet, 0.0, false))
		{
			// Pairwise 900 uu apart, and within 2,000 of the first, so one viewer streams all three.
			bool bFar = OutSites.Num() == 0 || FVector::Dist(OutSites[0], Site) <= 2000.0;
			for (const FVector& S : OutSites)
			{
				bFar &= FVector::Dist(S, Site) >= 900.0;
			}
			if (bFar)
			{
				OutSites.Add(Site);
				if (OutSites.Num() == 3)
				{
					break;
				}
			}
		}
		if (OutSites.Num() < 3)
		{
			return false;
		}
		const FVector Mid = (OutSites[0] + OutSites[1] + OutSites[2]) / 3.0;
		W.SetViewer(Mid + UpAt(W, Mid, bPlanet) * 300.0);
		return true;
	}

	void RunWorld(FAutomationTestBase& Test, bool bPlanet)
	{
		const TCHAR* Name = bPlanet ? TEXT("Planet") : TEXT("Plane");
		VoxelTesting::FTestChunkWorld W;
		TArray<FVector> Sites;
		if (!StartWorld(W, bPlanet, Sites))
		{
			Test.AddError(FString::Printf(TEXT("%s: no three seabed sites 5-10 voxels under the sea"), Name));
			W.Stop();
			return;
		}
		Test.TestTrue(FString::Printf(TEXT("%s: the world settles"), Name), SettleWater(W));
		UVoxelWaterPropagation* Water = W.Manager->GetWaterPropagation();
		if (!Water)
		{
			Test.AddError(FString::Printf(TEXT("%s: no water propagation with bEnableWaterLevel"), Name));
			W.Stop();
			return;
		}
		const float VS = W.Config->VoxelSize;

		// ---- A: a player dig on the seabed floods, and keeps the dig's density and material. ----
		{
			const FVector S = Sites[0];
			const FVector Up = UpAt(W, S, bPlanet);
			const FIntVector Above = KeyOf(W, S + Up * 1.5 * VS);
			Test.TestTrue(FString::Printf(TEXT("%s A: the sea is over the site (water-flagged air a voxel and a half up)"), Name), IsWater(Merged(W, Above)));

			const TArray<FIntVector> Region = Ball(W, S, 5.5f);
			bool bLoaded = true;
			for (const FIntVector& Key : Region)
			{
				bLoaded &= HasData(W, Key);
			}
			Test.TestTrue(FString::Printf(TEXT("%s A: the site's chunks are loaded"), Name), bLoaded);

			// The brush's density change falls off, so it digs the rock near its centre to air with densities of its own (the
			// generated value would be solid again); the rock it only softened stays solid.
			const UVoxelWaterPropagation::FStats Stats0 = Water->GetStats();
			PlayerBrush(W, S, 2.5f, EEditMode::Subtract, EVoxelBrushFalloff::Sharp);
			const TMap<FIntVector, FVoxelData> AfterDig = Capture(W, Region);
			TArray<FIntVector> ToDig;
			int32 DugBelow = 0, DugPartial = 0;
			for (const FIntVector& Key : BrushKeys(W, S, 2.5f))
			{
				const FVoxelData* V = AfterDig.Find(Key);
				if (V && Generated(W, Key).IsSolid() && V->IsAir())
				{
					ToDig.Add(Key);
					DugBelow += (!V->HasWaterFlag() && BelowSea(W, Key, bPlanet)) ? 1 : 0;
					DugPartial += V->Density > 0 ? 1 : 0;
				}
			}
			Test.TestTrue(FString::Printf(TEXT("%s A: the dig opened dry air below the sea (%d voxels, %d with a density above 0)"), Name, ToDig.Num(), DugPartial),
				ToDig.Num() > 0 && DugBelow == ToDig.Num());

			Test.TestTrue(FString::Printf(TEXT("%s A: settles after the dig"), Name), SettleWater(W));
			const TMap<FIntVector, FVoxelData> After = Capture(W, Region);
			int32 Flooded = 0;
			for (const FIntVector& Key : ToDig)
			{
				Flooded += IsWater(After[Key]) ? 1 : 0;
			}
			Test.TestEqual(FString::Printf(TEXT("%s A: dug voxels flooded"), Name), Flooded, ToDig.Num());
			const int32 Gained = CheckOnlyFlagsAdded(Test, *FString::Printf(TEXT("%s A"), Name), W, bPlanet, AfterDig, After);
			const UVoxelWaterPropagation::FStats& Stats1 = Water->GetStats();
			Test.TestTrue(FString::Printf(TEXT("%s A: seeds found"), Name), Stats1.Seeds > Stats0.Seeds);
			Test.TestEqual(FString::Printf(TEXT("%s A: floods stopped at the limit"), Name), Stats1.CapHits, Stats0.CapHits);
			Test.AddInfo(FString::Printf(TEXT("%s A: dug %d rock voxels to air under the sea (%d with a density above 0), %d flooded (%d flags in the region), %lld seeds, %lld voxels flooded in %lld flood(s), %lld scan reads"),
				Name, ToDig.Num(), DugPartial, Flooded, Gained, Stats1.Seeds - Stats0.Seeds, Stats1.FloodedVoxels - Stats0.FloodedVoxels,
				Stats1.FloodsFinished - Stats0.FloodsFinished, Stats1.ScanReads - Stats0.ScanReads));
		}

		// ---- B: a sealed pocket under the seabed stays dry; a system shaft opens it to the sea and it floods; a fill in it stays solid. ----
		{
			const FVector S = Sites[1];
			const FVector Up = UpAt(W, S, bPlanet);
			const FVector C = S - Up * 8.0 * VS;
			const TArray<FIntVector> Pocket = Ball(W, C, 2.2f);
			TArray<FIntVector> Fill, PocketAir;
			for (const FIntVector& Key : Pocket)
			{
				(FVector::DotProduct(CenterOf(W, Key) - C, Up) < -1.0 * VS ? Fill : PocketAir).Add(Key);
			}
			const FVector ShaftTop = S + Up * 2.0 * VS;
			TArray<FIntVector> Shaft;
			for (const FIntVector& Key : Tube(W, C, ShaftTop, 1.0f))
			{
				if (!Fill.Contains(Key) && !PocketAir.Contains(Key))
				{
					Shaft.Add(Key);
				}
			}
			TArray<FIntVector> Region = Tube(W, C - Up * 3.0 * VS, ShaftTop + Up * 2.0 * VS, 4.5f);

			int32 PocketRock = 0;
			bool bLoaded = true;
			for (const FIntVector& Key : Region)
			{
				bLoaded &= HasData(W, Key);
			}
			for (const FIntVector& Key : Pocket)
			{
				PocketRock += Generated(W, Key).IsSolid() ? 1 : 0;
			}
			Test.TestTrue(FString::Printf(TEXT("%s B: the pocket's chunks are loaded"), Name), bLoaded);
			Test.TestEqual(FString::Printf(TEXT("%s B: the pocket is dug from rock"), Name), PocketRock, Pocket.Num());

			// The pocket: air with a density and material of its own (a dug cave), then a fill of its bottom (sandstone, 220).
			const uint8 PocketDensity = 40, FillDensity = 220;
			SystemSet(W, PocketAir, [&](const FIntVector& Key) { return FVoxelData(EVoxelMaterial::Dirt, PocketDensity, Generated(W, Key).BiomeID, 0); });
			SystemSet(W, Fill, [&](const FIntVector& Key) { return FVoxelData(EVoxelMaterial::Sandstone, FillDensity, Generated(W, Key).BiomeID, 0); });
			const UVoxelWaterPropagation::FStats Stats0 = Water->GetStats();
			Test.TestTrue(FString::Printf(TEXT("%s B: settles after the pocket"), Name), SettleWater(W));
			int32 PocketWet = 0;
			for (const FIntVector& Key : Pocket)
			{
				PocketWet += Merged(W, Key).HasWaterFlag() ? 1 : 0;
			}
			Test.TestEqual(FString::Printf(TEXT("%s B: the sealed pocket stays dry"), Name), PocketWet, 0);
			Test.TestEqual(FString::Printf(TEXT("%s B: no seed for the sealed pocket"), Name), Water->GetStats().Seeds, Stats0.Seeds);

			// The shaft: single-voxel system edits (radius 0) from the pocket up into the sea. Its voxels in the sea lose the flag
			// (a Set to plain air) and take it again.
			SystemSet(W, Shaft, [](const FIntVector&) { return FVoxelData::Air(); });
			const TMap<FIntVector, FVoxelData> Before = Capture(W, Region);
			Test.TestTrue(FString::Printf(TEXT("%s B: settles after the shaft"), Name), SettleWater(W));
			const TMap<FIntVector, FVoxelData> After = Capture(W, Region);

			int32 PocketFlooded = 0, ShaftFlooded = 0, ShaftBelow = 0, FillSolid = 0;
			for (const FIntVector& Key : PocketAir)
			{
				const FVoxelData& V = After[Key];
				PocketFlooded += (IsWater(V) && V.Density == PocketDensity && V.MaterialID == EVoxelMaterial::Dirt) ? 1 : 0;
			}
			for (const FIntVector& Key : Shaft)
			{
				if (BelowSea(W, Key, bPlanet))
				{
					++ShaftBelow;
					ShaftFlooded += IsWater(After[Key]) ? 1 : 0;
				}
			}
			for (const FIntVector& Key : Fill)
			{
				const FVoxelData& V = After[Key];
				FillSolid += (V.IsSolid() && !V.HasWaterFlag() && V.Density == FillDensity && V.MaterialID == EVoxelMaterial::Sandstone) ? 1 : 0;
			}
			Test.TestEqual(FString::Printf(TEXT("%s B: pocket voxels flooded with the pocket's density and material"), Name), PocketFlooded, PocketAir.Num());
			Test.TestEqual(FString::Printf(TEXT("%s B: shaft voxels below the sea flooded"), Name), ShaftFlooded, ShaftBelow);
			Test.TestEqual(FString::Printf(TEXT("%s B: the fill stays solid, dry, with its density and material"), Name), FillSolid, Fill.Num());
			const int32 Gained = CheckOnlyFlagsAdded(Test, *FString::Printf(TEXT("%s B"), Name), W, bPlanet, Before, After);
			const UVoxelWaterPropagation::FStats& Stats1 = Water->GetStats();
			Test.TestTrue(FString::Printf(TEXT("%s B: the shaft (a system edit, radius 0) seeded"), Name), Stats1.Seeds > Stats0.Seeds);
			Test.AddInfo(FString::Printf(TEXT("%s B: pocket %d voxels (%d air, %d fill), shaft %d (%d below the sea); %d pocket and %d shaft voxels flooded, %d flags in the region; %lld seeds, %lld flooded"),
				Name, Pocket.Num(), PocketAir.Num(), Fill.Num(), Shaft.Num(), ShaftBelow, PocketFlooded, ShaftFlooded, Gained,
				Stats1.Seeds - Stats0.Seeds, Stats1.FloodedVoxels - Stats0.FloodedVoxels));
		}

		// ---- C: a player fill plugs the sea over the seabed; a cavity dug under the plug stays dry, the plug solid. ----
		{
			const FVector S = Sites[2];
			const FVector Up = UpAt(W, S, bPlanet);
			// A Smooth fill of radius 7 turns the sea solid within about 3.5 voxels of its centre (half its change there); the
			// sea further out only thickens, staying water.
			const FVector PlugCenter = S + Up * 1.5 * VS;
			const TArray<FIntVector> PlugBall = BrushKeys(W, PlugCenter, 7.0f);
			const TArray<FIntVector> Region = Ball(W, S, 8.0f);
			const UVoxelWaterPropagation::FStats Stats0 = Water->GetStats();
			PlayerBrush(W, PlugCenter, 7.0f, EEditMode::Add, EVoxelBrushFalloff::Smooth);
			Test.TestTrue(FString::Printf(TEXT("%s C: settles after the fill"), Name), SettleWater(W));

			int32 PlugWithFlag = 0;
			for (const FIntVector& Key : PlugBall)
			{
				const FVoxelData V = Merged(W, Key);
				PlugWithFlag += (V.IsSolid() && V.HasWaterFlag()) ? 1 : 0;
			}
			Test.TestTrue(FString::Printf(TEXT("%s C: the fill took in sea voxels (solid, the flag kept: %d)"), Name, PlugWithFlag), PlugWithFlag > 0);

			// The cavity: across the seabed under the plug's core, its top in the sea the plug filled, its neighbours plug or rock.
			const TArray<FIntVector> Cavity = Ball(W, S - Up * 0.5 * VS, 1.2f);
			SystemSet(W, Cavity, [](const FIntVector&) { return FVoxelData::Air(); });
			const TMap<FIntVector, FVoxelData> Before = Capture(W, Region);
			Test.TestTrue(FString::Printf(TEXT("%s C: settles after the cavity"), Name), SettleWater(W));
			const TMap<FIntVector, FVoxelData> After = Capture(W, Region);

			int32 CavityWet = 0, NextToPlug = 0;
			for (const FIntVector& Key : Cavity)
			{
				CavityWet += After[Key].HasWaterFlag() ? 1 : 0;
				for (const FIntVector& Offset : { FIntVector(1, 0, 0), FIntVector(-1, 0, 0), FIntVector(0, 1, 0), FIntVector(0, -1, 0), FIntVector(0, 0, 1), FIntVector(0, 0, -1) })
				{
					const FVoxelData N = Merged(W, Key + Offset);
					NextToPlug += (N.IsSolid() && N.HasWaterFlag()) ? 1 : 0;
				}
			}
			Test.TestTrue(FString::Printf(TEXT("%s C: the cavity touches the plug's flagged voxels (%d faces)"), Name, NextToPlug), NextToPlug > 0);
			Test.TestEqual(FString::Printf(TEXT("%s C: the cavity under the plug stays dry"), Name), CavityWet, 0);
			const int32 Gained = CheckOnlyFlagsAdded(Test, *FString::Printf(TEXT("%s C"), Name), W, bPlanet, Before, After);
			Test.TestEqual(FString::Printf(TEXT("%s C: flags taken around the plug"), Name), Gained, 0);
			Test.TestEqual(FString::Printf(TEXT("%s C: no seed"), Name), Water->GetStats().Seeds, Stats0.Seeds);
			Test.AddInfo(FString::Printf(TEXT("%s C: plug %d voxels with the flag, cavity %d voxels, %d faces against them"), Name, PlugWithFlag, Cavity.Num(), NextToPlug));
		}

		// ---- D: a system dig of a seabed chunk with no voxel data yet (a POI's terraform at stream-in) floods once it loads. ----
		{
			const TArray<FVector> Far = FindSeabedSites(W, bPlanet, 12000.0, true);
			if (Far.Num() == 0)
			{
				Test.AddError(FString::Printf(TEXT("%s D: no seabed site 12,000 uu away inside its chunk"), Name));
			}
			else
			{
				const FVector S = Far[0];
				const FVector Up = UpAt(W, S, bPlanet);
				const FIntVector Chunk = W.Manager->WorldToChunkCoord(S);
				Test.TestFalse(FString::Printf(TEXT("%s D: the far chunk has no voxel data before the edit"), Name), W.Manager->HasChunkVoxelData(Chunk));

				const uint8 DigDensity = 30;
				const TArray<FIntVector> Dig = Ball(W, S, 2.0f);
				const UVoxelWaterPropagation::FStats Stats0 = Water->GetStats();
				SystemSet(W, Dig, [DigDensity](const FIntVector&) { return FVoxelData(EVoxelMaterial::Dirt, DigDensity, 0, 0); });
				const UVoxelWaterPropagation::FStats Stats1 = Water->GetStats();
				Test.TestTrue(FString::Printf(TEXT("%s D: the edit's scan waits for the chunk's water"), Name), Stats1.ScansDeferred > Stats0.ScansDeferred);

				W.SetViewer(S + Up * 300.0);
				Test.TestTrue(FString::Printf(TEXT("%s D: settles at the far site"), Name), SettleWater(W));
				Test.TestTrue(FString::Printf(TEXT("%s D: the far chunk loaded, its water in place"), Name), W.Manager->IsChunkWaterSettled(Chunk));

				int32 Rock = 0, Flooded = 0;
				for (const FIntVector& Key : Dig)
				{
					const FVoxelData V = Merged(W, Key);
					Rock += Generated(W, Key).IsSolid() ? 1 : 0;
					Flooded += (IsWater(V) && V.Density == DigDensity && V.MaterialID == EVoxelMaterial::Dirt) ? 1 : 0;
				}
				Test.TestTrue(FString::Printf(TEXT("%s D: the dig took rock (%d voxels)"), Name, Rock), Rock > 0);
				Test.TestEqual(FString::Printf(TEXT("%s D: dug voxels flooded with the dig's density and material"), Name), Flooded, Dig.Num());
				const UVoxelWaterPropagation::FStats& Stats2 = Water->GetStats();
				Test.AddInfo(FString::Printf(TEXT("%s D: dig %d voxels (%d rock) before the chunk loaded, %d flooded after; %lld seeds, %lld flooded"),
					Name, Dig.Num(), Rock, Flooded, Stats2.Seeds - Stats0.Seeds, Stats2.FloodedVoxels - Stats0.FloodedVoxels));
			}
		}

		W.Stop();
	}

	// ---- G59 ----

	/** Voxel keys from their coordinates along an up axis, an axis across a chunk face, and the third axis. */
	struct FAxes
	{
		int32 Up = 2;
		int32 Face = 0;
		int32 Third = 1;

		FIntVector Key(int32 U, int32 F, int32 T) const
		{
			FIntVector K = FIntVector::ZeroValue;
			K[Up] = U;
			K[Face] = F;
			K[Third] = T;
			return K;
		}
	};

	int32 FloorDiv(int32 A, int32 B)
	{
		return FMath::FloorToInt32(double(A) / B);
	}

	FIntVector ChunkOfKey(const VoxelTesting::FTestChunkWorld& W, const FIntVector& Key)
	{
		const int32 CS = W.Config->ChunkSize;
		return FIntVector(FloorDiv(Key.X, CS), FloorDiv(Key.Y, CS), FloorDiv(Key.Z, CS));
	}

	/** The chunks holding some keys. */
	TSet<FIntVector> ChunksOf(const VoxelTesting::FTestChunkWorld& W, const TArray<FIntVector>& Keys)
	{
		TSet<FIntVector> Chunks;
		for (const FIntVector& Key : Keys)
		{
			Chunks.Add(ChunkOfKey(W, Key));
		}
		return Chunks;
	}

	/** Every key of the box [Min, Max] along (up, face, third), less those of another box (an empty one when Max < Min). */
	TArray<FIntVector> Box(const FAxes& Ax, const FIntVector& Min, const FIntVector& Max, const FIntVector& HoleMin = FIntVector(1), const FIntVector& HoleMax = FIntVector(0))
	{
		TArray<FIntVector> Keys;
		for (int32 U = Min.X; U <= Max.X; ++U)
		for (int32 F = Min.Y; F <= Max.Y; ++F)
		for (int32 T = Min.Z; T <= Max.Z; ++T)
		{
			const bool bHole = U >= HoleMin.X && U <= HoleMax.X && F >= HoleMin.Y && F <= HoleMax.Y && T >= HoleMin.Z && T <= HoleMax.Z;
			if (!bHole)
			{
				Keys.Add(Ax.Key(U, F, T));
			}
		}
		return Keys;
	}

	/** Tick until Done holds (and then until the pipeline and the water propagation are idle). */
	bool TickUntil(VoxelTesting::FTestChunkWorld& W, TFunctionRef<bool()> Done, double TimeoutSeconds = 120.0)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (!Done() && FPlatformTime::Seconds() < Deadline)
		{
			W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			FPlatformProcess::Sleep(0.002f);
		}
		return Done() && SettleWater(W, TimeoutSeconds);
	}

	/** The highest voxel of the column (F, T) along the up axis that is below the sea. */
	int32 TopBelowSea(const VoxelTesting::FTestChunkWorld& W, bool bPlanet, const FAxes& Ax, int32 F, int32 T)
	{
		const double VS = W.Config->VoxelSize;
		if (!bPlanet)
		{
			return FMath::FloorToInt32(W.Config->WaterLevel / VS);
		}
		// The planet's sites are at the +X face's equator, up along +X.
		const double Across = (FMath::Square(double(F)) + FMath::Square(double(T))) * VS * VS;
		int32 U = FMath::FloorToInt32(FMath::Sqrt(FMath::Max(0.0, FMath::Square(double(W.Config->WaterRadius)) - Across)) / VS);
		while (BelowSea(W, Ax.Key(U + 1, F, T), true))
		{
			++U;
		}
		while (!BelowSea(W, Ax.Key(U, F, T), true))
		{
			--U;
		}
		return U;
	}

	/**
	 * The seabed under the column (F, T): walking down along the up axis from the sea's top, the first voxel that is not
	 * water (generated, its flags pulled). False when it is not solid or a chunk on the way has no data.
	 */
	bool FindSeabed(const VoxelTesting::FTestChunkWorld& W, bool bPlanet, const FAxes& Ax, int32 F, int32 T, int32& OutTop, int32& OutBed)
	{
		OutTop = TopBelowSea(W, bPlanet, Ax, F, T);
		for (int32 U = OutTop; U > OutTop - 40; --U)
		{
			const FIntVector Key = Ax.Key(U, F, T);
			if (!HasData(W, Key))
			{
				return false;
			}
			const FVoxelData V = Generated(W, Key);
			if (!IsWater(V))
			{
				OutBed = U;
				return V.IsSolid();
			}
		}
		return false;
	}

	/** Two footprints on one chunk face, at T0 and T1 along the third axis, each over its highest seabed voxel Bed0 / Bed1. */
	struct FFaceSites
	{
		int32 F = 0; // the neighbour's first layer along the face axis; this side's last is F - 1
		int32 T0 = 0;
		int32 Bed0 = 0;
		int32 T1 = 0;
		int32 Bed1 = 0;
	};

	/**
	 * Faces of the anchor's chunk and its neighbours along the face axis, and on them two footprints 8 to 20 voxels apart
	 * where the columns F-3..F+2 x T-2..T+2 stand in water at least 5 voxels deep over the footprint's highest seabed (the
	 * walls and pockets sit from 1 to 4 voxels over it, so all of them are in the sea).
	 */
	bool FindFaceSites(const VoxelTesting::FTestChunkWorld& W, bool bPlanet, const FAxes& Ax, const FIntVector& Anchor, FFaceSites& Out, FString& OutWhy)
	{
		int32 NoBed = 0, Shallow = 0, Fit = 0;
		const int32 CS = W.Config->ChunkSize;
		const int32 AnchorChunk = FloorDiv(Anchor[Ax.Face], CS);
		for (const int32 K : { 1, 0, 2, -1 })
		{
			const int32 F = (AnchorChunk + K) * CS;
			TArray<TPair<int32, int32>> Good; // (T, Bed)
			for (int32 DT = -30; DT <= 30; ++DT)
			{
				const int32 T = Anchor[Ax.Third] + DT;
				int32 BedMax = MIN_int32, TopMin = MAX_int32;
				bool bOk = true;
				for (int32 FF = F - 3; FF <= F + 2 && bOk; ++FF)
				{
					for (int32 TT = T - 2; TT <= T + 2 && bOk; ++TT)
					{
						int32 Top = 0, Bed = 0;
						bOk = FindSeabed(W, bPlanet, Ax, FF, TT, Top, Bed);
						BedMax = FMath::Max(BedMax, Bed);
						TopMin = FMath::Min(TopMin, Top);
					}
				}
				if (bOk && TopMin - BedMax >= 5)
				{
					Good.Add({ T, BedMax });
				}
				NoBed += bOk ? 0 : 1;
				Shallow += (bOk && TopMin - BedMax < 5) ? 1 : 0;
			}
			Fit += Good.Num();
			for (int32 i = 0; i < Good.Num(); ++i)
			{
				for (int32 j = i + 1; j < Good.Num(); ++j)
				{
					// 8 to 20 apart: the footprints' walls do not touch, and one viewer away from the face keeps both in reach.
					if (Good[j].Key - Good[i].Key >= 8 && Good[j].Key - Good[i].Key <= 20)
					{
						Out.F = F;
						Out.T0 = Good[i].Key;
						Out.Bed0 = Good[i].Value;
						Out.T1 = Good[j].Key;
						Out.Bed1 = Good[j].Value;
						return true;
					}
				}
			}
		}
		OutWhy = FString::Printf(TEXT("footprints: %d without a solid seabed under water in reach, %d under less than 5 voxels of water, %d fit (none a pair 8 to 20 apart)"),
			NoBed, Shallow, Fit);
		return false;
	}

	void RunChunkArrival(FAutomationTestBase& Test, bool bPlanet)
	{
		const TCHAR* Name = bPlanet ? TEXT("Planet") : TEXT("Plane");
		VoxelTesting::FTestChunkWorld W;
		StartSeaWorld(W, bPlanet, bPlanet ? TEXT("WaterArrivalPlanet") : TEXT("WaterArrivalPlane"));
		const float VS = W.Config->VoxelSize;
		const int32 CS = W.Config->ChunkSize;
		FAxes Ax;
		if (bPlanet)
		{
			// The +X face's equator: up along X, the face across Y.
			Ax.Up = 0;
			Ax.Face = 1;
			Ax.Third = 2;
		}

		// Two footprints on one chunk face near a seabed site, the viewer over the first site that has them.
		FFaceSites Sites;
		bool bFound = false;
		const TArray<FVector> Anchors = FindSeabedSites(W, bPlanet, 0.0, false);
		for (int32 i = 0; i < FMath::Min(4, Anchors.Num()) && !bFound; ++i)
		{
			W.SetViewer(Anchors[i] + UpAt(W, Anchors[i], bPlanet) * 300.0);
			Test.TestTrue(FString::Printf(TEXT("%s: the world settles at seabed site %d"), Name, i), SettleWater(W));
			FString Why;
			bFound = FindFaceSites(W, bPlanet, Ax, KeyOf(W, Anchors[i]), Sites, Why);
			if (!bFound)
			{
				Test.AddInfo(FString::Printf(TEXT("%s: seabed site %d has no face pair: %s"), Name, i, *Why));
			}
		}
		if (!bFound)
		{
			Test.AddError(FString::Printf(TEXT("%s: no chunk face with two footprints in the sea"), Name));
			W.Stop();
			return;
		}
		UVoxelWaterPropagation* Water = W.Manager->GetWaterPropagation();
		if (!Water)
		{
			Test.AddError(FString::Printf(TEXT("%s: no water propagation with bEnableWaterLevel"), Name));
			W.Stop();
			return;
		}
		const int32 F = Sites.F;
		auto Solid = [&W](const FIntVector& Key) { return FVoxelData(EVoxelMaterial::Sandstone, 220, Generated(W, Key).BiomeID, 0); };
		auto Air = [](const FIntVector&) { return FVoxelData::Air(); };

		// (i) at T1: a pocket in N against the face (F, F+1), walled in on N's side, a door on A's side (F - 1).
		const int32 B1 = Sites.Bed1, T1 = Sites.T1;
		const TArray<FIntVector> PocketN = Box(Ax, FIntVector(B1 + 2, F, T1 - 1), FIntVector(B1 + 3, F + 1, T1 + 1));
		const TArray<FIntVector> WallN = Box(Ax, FIntVector(B1 + 1, F, T1 - 2), FIntVector(B1 + 4, F + 2, T1 + 2),
			FIntVector(B1 + 2, F, T1 - 1), FIntVector(B1 + 3, F + 1, T1 + 1));
		const TArray<FIntVector> Door = Box(Ax, FIntVector(B1 + 2, F - 1, T1 - 1), FIntVector(B1 + 3, F - 1, T1 + 1));
		// (ii) at T0: a pocket in A against the face (F - 2, F - 1), walled in on A's side; N's sea across the face.
		const int32 B0 = Sites.Bed0, T0 = Sites.T0;
		const TArray<FIntVector> PocketA = Box(Ax, FIntVector(B0 + 2, F - 2, T0 - 1), FIntVector(B0 + 3, F - 1, T0 + 1));
		const TArray<FIntVector> WallA = Box(Ax, FIntVector(B0 + 1, F - 3, T0 - 2), FIntVector(B0 + 4, F - 1, T0 + 2),
			FIntVector(B0 + 2, F - 2, T0 - 1), FIntVector(B0 + 3, F - 1, T0 + 1));
		const TArray<FIntVector> SeaAcrossA = Box(Ax, FIntVector(B0 + 2, F, T0 - 1), FIntVector(B0 + 3, F, T0 + 1));
		// The regions checked for changes: each footprint and a voxel around it, on either side of the face.
		const TArray<FIntVector> RegionN = Box(Ax, FIntVector(B1, F, T1 - 3), FIntVector(B1 + 5, F + 3, T1 + 3));
		const TArray<FIntVector> RegionA = Box(Ax, FIntVector(B0, F - 4, T0 - 3), FIntVector(B0 + 5, F - 1, T0 + 3));

		TArray<FIntVector> NKeys = PocketN;
		NKeys.Append(SeaAcrossA);
		const TSet<FIntVector> ChunksN = ChunksOf(W, NKeys);
		TArray<FIntVector> AKeys = PocketA;
		AKeys.Append(Door);
		const TSet<FIntVector> ChunksA = ChunksOf(W, AKeys);
		auto AllHaveNoData = [&W](const TSet<FIntVector>& Chunks)
		{
			for (const FIntVector& C : Chunks)
			{
				if (W.Manager->HasChunkVoxelData(C))
				{
					return false;
				}
			}
			return true;
		};
		auto AllSettled = [&W](const TSet<FIntVector>& Chunks)
		{
			for (const FIntVector& C : Chunks)
			{
				if (!W.Manager->IsChunkWaterSettled(C))
				{
					return false;
				}
			}
			return true;
		};
		bool bOverlap = false;
		for (const FIntVector& C : ChunksN)
		{
			bOverlap |= ChunksA.Contains(C);
		}
		Test.TestFalse(FString::Printf(TEXT("%s: the two sides of the face are different chunks"), Name), bOverlap);

		const int32 TMid = (T0 + T1) / 2;
		const FVector Home = CenterOf(W, Ax.Key(FMath::Max(B0, B1) + 1, F, TMid));
		const FVector HomeViewer = Home + UpAt(W, Home, bPlanet) * 300.0;
		// 2,400 uu from A's centre away from the face: N's centre 4,000 away, beyond the unload horizon (3,600).
		const FVector Away = W.Manager->GetGeneratedSurfacePoint(CenterOf(W, Ax.Key(FMath::Max(B0, B1) + 1, F - CS / 2 - FMath::RoundToInt32(2400.0f / VS), TMid)));
		const FVector AwayViewer = Away + UpAt(W, Away, bPlanet) * 300.0;

		// ---- Phase 1, both sides loaded: (i)'s pocket in N, sealed by the door. ----
		W.SetViewer(HomeViewer);
		Test.TestTrue(FString::Printf(TEXT("%s: settles at the face"), Name), SettleWater(W));
		Test.TestTrue(FString::Printf(TEXT("%s: both sides' chunks are loaded"), Name), AllSettled(ChunksN) && AllSettled(ChunksA));
		const UVoxelWaterPropagation::FStats Stats0 = Water->GetStats();
		TArray<FIntVector> WallAndDoor = WallN;
		WallAndDoor.Append(Door);
		SystemSet(W, WallAndDoor, Solid);
		SystemSet(W, PocketN, Air);
		Test.TestTrue(FString::Printf(TEXT("%s (i): settles after the pocket"), Name), SettleWater(W));
		int32 PocketNWet = 0;
		for (const FIntVector& Key : PocketN)
		{
			PocketNWet += Merged(W, Key).HasWaterFlag() ? 1 : 0;
		}
		Test.TestEqual(FString::Printf(TEXT("%s (i): the sealed pocket in N stays dry"), Name), PocketNWet, 0);
		Test.TestEqual(FString::Printf(TEXT("%s (i): no seed for it"), Name), Water->GetStats().Seeds, Stats0.Seeds);
		const TMap<FIntVector, FVoxelData> BeforeN = Capture(W, RegionN);

		// ---- Phase 2, N unloaded: (ii)'s pocket in A against the face; (i)'s door dug open, A's sea floods it. ----
		W.SetViewer(AwayViewer);
		Test.TestTrue(FString::Printf(TEXT("%s: N unloads, A stays"), Name), TickUntil(W, [&] { return AllHaveNoData(ChunksN); }));
		Test.TestTrue(FString::Printf(TEXT("%s: A's water stays in place"), Name), AllSettled(ChunksA));
		const UVoxelWaterPropagation::FStats Stats1 = Water->GetStats();
		SystemSet(W, WallA, Solid);
		SystemSet(W, PocketA, Air);
		Test.TestTrue(FString::Printf(TEXT("%s (ii): settles after the pocket"), Name), SettleWater(W));
		int32 PocketAWet = 0;
		for (const FIntVector& Key : PocketA)
		{
			PocketAWet += Merged(W, Key).HasWaterFlag() ? 1 : 0;
		}
		Test.TestEqual(FString::Printf(TEXT("%s (ii): the pocket in A is dry while N is not loaded"), Name), PocketAWet, 0);
		Test.TestEqual(FString::Printf(TEXT("%s (ii): no seed while N is not loaded"), Name), Water->GetStats().Seeds, Stats1.Seeds);

		SystemSet(W, Door, Air);
		Test.TestTrue(FString::Printf(TEXT("%s (i): settles after the door opened"), Name), SettleWater(W));
		int32 DoorWet = 0;
		for (const FIntVector& Key : Door)
		{
			DoorWet += IsWater(Merged(W, Key)) ? 1 : 0;
		}
		Test.TestEqual(FString::Printf(TEXT("%s (i): the door floods from A's sea"), Name), DoorWet, Door.Num());
		Test.TestTrue(FString::Printf(TEXT("%s (i): N still not loaded"), Name), AllHaveNoData(ChunksN));
		const TMap<FIntVector, FVoxelData> BeforeA = Capture(W, RegionA);
		const UVoxelWaterPropagation::FStats Stats2 = Water->GetStats();

		// ---- Phase 3, back at the face: N loads, its water settles. ----
		W.SetViewer(HomeViewer);
		Test.TestTrue(FString::Printf(TEXT("%s: N loads again"), Name), TickUntil(W, [&] { return AllSettled(ChunksN); }));
		int32 PocketNFlooded = 0, PocketAFlooded = 0, SeaAcross = 0;
		for (const FIntVector& Key : PocketN)
		{
			PocketNFlooded += IsWater(Merged(W, Key)) ? 1 : 0;
		}
		for (const FIntVector& Key : PocketA)
		{
			PocketAFlooded += IsWater(Merged(W, Key)) ? 1 : 0;
		}
		for (const FIntVector& Key : SeaAcrossA)
		{
			SeaAcross += IsWater(Merged(W, Key)) ? 1 : 0;
		}
		Test.TestEqual(FString::Printf(TEXT("%s (ii): N's sea is across the face from A's pocket"), Name), SeaAcross, SeaAcrossA.Num());
		Test.TestEqual(FString::Printf(TEXT("%s (i): the flood continues through the face into N's pocket"), Name), PocketNFlooded, PocketN.Num());
		Test.TestEqual(FString::Printf(TEXT("%s (ii): A's pocket floods from N's sea"), Name), PocketAFlooded, PocketA.Num());
		CheckOnlyFlagsAdded(Test, *FString::Printf(TEXT("%s (i), N's side"), Name), W, bPlanet, BeforeN, Capture(W, RegionN));
		CheckOnlyFlagsAdded(Test, *FString::Printf(TEXT("%s (ii), A's side"), Name), W, bPlanet, BeforeA, Capture(W, RegionA));
		const UVoxelWaterPropagation::FStats& Stats3 = Water->GetStats();
		Test.TestTrue(FString::Printf(TEXT("%s: seeds on the face"), Name), Stats3.FaceSeeds > Stats2.FaceSeeds);
		Test.AddInfo(FString::Printf(TEXT("%s: face at %d (axis %d), N's pocket %d voxels (%d flooded), A's pocket %d (%d flooded), door %d (%d flooded before N loaded); after N loaded: %lld faces scanned, %lld face seeds, %lld seeds, %lld voxels flooded"),
			Name, F, Ax.Face, PocketN.Num(), PocketNFlooded, PocketA.Num(), PocketAFlooded, Door.Num(), DoorWet,
			Stats3.FaceScansQueued - Stats2.FaceScansQueued, Stats3.FaceSeeds - Stats2.FaceSeeds, Stats3.Seeds - Stats2.Seeds,
			Stats3.FloodedVoxels - Stats2.FloodedVoxels));
		W.Stop();
	}

	/** The ground's height over the column (GX, GY) on the plane, relative to WorldOrigin (the generated surface). */
	double GroundZ(const VoxelTesting::FTestChunkWorld& W, int32 GX, int32 GY)
	{
		const float VS = W.Config->VoxelSize;
		return W.Manager->GetGeneratedSurfacePoint(OriginOf(W) + FVector((GX + 0.5) * VS, (GY + 0.5) * VS, 0.0)).Z - OriginOf(W).Z;
	}

	/** Whether the water tile over a column covers its centre (the tile's quads, local to its corner). */
	bool TileCovers(const VoxelTesting::FTestChunkWorld& W, int32 GX, int32 GY)
	{
		const int32 CS = W.Config->ChunkSize;
		const float VS = W.Config->VoxelSize;
		const FIntVector2 Tile(FloorDiv(GX, CS), FloorDiv(GY, CS));
		const FChunkMeshData* Mesh = W.Renderer.WaterTiles.Find(Tile);
		if (!Mesh)
		{
			return false;
		}
		const float PX = (GX - Tile.X * CS + 0.5f) * VS;
		const float PY = (GY - Tile.Y * CS + 0.5f) * VS;
		for (int32 i = 0; i + 2 < Mesh->Indices.Num(); i += 3)
		{
			// The quads are axis-aligned rectangles, each two triangles over the same box.
			FBox2f Bounds(ForceInit);
			for (int32 k = 0; k < 3; ++k)
			{
				const FVector3f& P = Mesh->Positions[Mesh->Indices[i + k]];
				Bounds += FVector2f(P.X, P.Y);
			}
			if (PX >= Bounds.Min.X && PX <= Bounds.Max.X && PY >= Bounds.Min.Y && PY <= Bounds.Max.Y)
			{
				return true;
			}
		}
		return false;
	}

	void RunInlandChannelTile(FAutomationTestBase& Test)
	{
		VoxelTesting::FTestChunkWorld W;
		StartSeaWorld(W, false, TEXT("WaterTilesPlane"), true);
		const float VS = W.Config->VoxelSize;

		// A shore: three columns of sea, then land (the ground over the sea, so no water in the column) up to 12 voxels high
		// for 11 columns and 4 rows either side, so no wet column lies within the tiles' 3-column dilation of the channel's
		// inland end (columns 3 to 7).
		TMap<FIntPoint, double> Heights;
		auto H = [&](int32 X, int32 Y)
		{
			if (const double* Known = Heights.Find(FIntPoint(X, Y)))
			{
				return *Known;
			}
			return Heights.Add(FIntPoint(X, Y), GroundZ(W, X, Y));
		};
		int32 ShoreX = 0, ShoreY = 0, Dir = 0;
		double BestScore = TNumericLimits<double>::Max(), MaxGround = 0.0;
		for (int32 GY = -120; GY <= 120; GY += 2)
		{
			for (int32 GX = -120; GX <= 120; ++GX)
			{
				for (const int32 D : { 1, -1 })
				{
					bool bOk = true;
					for (int32 DY = -1; DY <= 1 && bOk; ++DY)
					{
						for (int32 i = 1; i <= 3 && bOk; ++i)
						{
							bOk = H(GX - D * i, GY + DY) <= -0.2 * VS;
						}
					}
					double Top = 0.0;
					for (int32 DY = -4; DY <= 4 && bOk; ++DY)
					{
						for (int32 i = 0; i <= 10 && bOk; ++i)
						{
							const double Ground = H(GX + D * i, GY + DY);
							bOk = Ground >= 0.3 * VS && Ground <= 12.0 * VS;
							Top = FMath::Max(Top, Ground);
						}
					}
					const double Score = FMath::Square(double(GX)) + FMath::Square(double(GY));
					if (bOk && Score < BestScore)
					{
						BestScore = Score;
						ShoreX = GX;
						ShoreY = GY;
						Dir = D;
						MaxGround = Top;
					}
				}
			}
		}
		if (Dir == 0)
		{
			Test.AddError(TEXT("Tiles: no shore within 6,000 uu"));
			W.Stop();
			return;
		}
		UVoxelWaterPropagation* Water = W.Manager->GetWaterPropagation();
		W.SetViewer(OriginOf(W) + FVector((ShoreX + 0.5) * VS, (ShoreY + 0.5) * VS, 300.0));
		Test.TestTrue(TEXT("Tiles: the world settles at the shore"), SettleWater(W));
		if (!Water)
		{
			Test.AddError(TEXT("Tiles: no water propagation with bEnableWaterLevel"));
			W.Stop();
			return;
		}

		// The channel's inland columns, 3 to 7 past the shore: land, and no tile over them yet.
		TArray<FIntPoint> Inland;
		for (int32 DY = -1; DY <= 1; ++DY)
		{
			for (int32 i = 3; i <= 7; ++i)
			{
				Inland.Add(FIntPoint(ShoreX + Dir * i, ShoreY + DY));
			}
		}
		int32 CoveredBefore = 0;
		for (const FIntPoint& C : Inland)
		{
			CoveredBefore += TileCovers(W, C.X, C.Y) ? 1 : 0;
		}
		Test.TestTrue(TEXT("Tiles: the sea's tile covers the shore"), TileCovers(W, ShoreX - Dir, ShoreY));
		Test.TestEqual(TEXT("Tiles: no tile over the channel's inland columns before the dig"), CoveredBefore, 0);

		// The channel: from 3 columns out in the sea to 7 inland, 3 wide, down to 3 voxels under the sea. Only rock is dug.
		const int32 TopZ = FMath::CeilToInt32(MaxGround / VS) + 1;
		const int32 BottomZ = FMath::FloorToInt32(W.Config->WaterLevel / VS) - 3;
		TArray<FIntVector> Dig;
		for (int32 i = -3; i <= 7; ++i)
		{
			for (int32 DY = -1; DY <= 1; ++DY)
			{
				for (int32 GZ = BottomZ; GZ <= TopZ; ++GZ)
				{
					const FIntVector Key(ShoreX + Dir * i, ShoreY + DY, GZ);
					if (Merged(W, Key).IsSolid())
					{
						Dig.Add(Key);
					}
				}
			}
		}
		const UVoxelWaterPropagation::FStats Stats0 = Water->GetStats();
		SystemSet(W, Dig, [](const FIntVector&) { return FVoxelData::Air(); });
		Test.TestTrue(TEXT("Tiles: settles after the channel"), SettleWater(W));

		int32 DugBelow = 0, Flooded = 0;
		TSet<FIntPoint> InlandWet;
		for (const FIntVector& Key : Dig)
		{
			if (BelowSea(W, Key, false))
			{
				++DugBelow;
				const bool bWater = IsWater(Merged(W, Key));
				Flooded += bWater ? 1 : 0;
				if (bWater && Inland.Contains(FIntPoint(Key.X, Key.Y)))
				{
					InlandWet.Add(FIntPoint(Key.X, Key.Y));
				}
			}
		}
		const int32 InlandFlooded = InlandWet.Num();
		int32 CoveredAfter = 0;
		for (const FIntPoint& C : Inland)
		{
			CoveredAfter += TileCovers(W, C.X, C.Y) ? 1 : 0;
		}
		Test.TestEqual(TEXT("Tiles: the channel's voxels under the sea flooded"), Flooded, DugBelow);
		Test.TestEqual(TEXT("Tiles: the channel's inland columns holding water"), InlandFlooded, Inland.Num());
		Test.TestEqual(TEXT("Tiles: the tile covers the channel's inland columns"), CoveredAfter, Inland.Num());
		const UVoxelWaterPropagation::FStats& Stats1 = Water->GetStats();
		Test.AddInfo(FString::Printf(TEXT("Tiles: shore at column (%d, %d) heading %+d, ground up to %.0f; channel %d voxels dug (%d under the sea, %d flooded; %d inland columns with water); inland columns covered %d -> %d of %d; %lld seeds, %lld flooded"),
			ShoreX, ShoreY, Dir, MaxGround, Dig.Num(), DugBelow, Flooded, InlandFlooded, CoveredBefore, CoveredAfter, Inland.Num(),
			Stats1.Seeds - Stats0.Seeds, Stats1.FloodedVoxels - Stats0.FloodedVoxels));
		W.Stop();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterPropagationPlaneTest, "VoxelWorlds.Streaming.WaterPropagation.Plane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelWaterPropagationPlaneTest::RunTest(const FString& Parameters)
{
	VoxelWaterPropagationTest::RunWorld(*this, false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterPropagationPlanetTest, "VoxelWorlds.Streaming.WaterPropagation.Planet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelWaterPropagationPlanetTest::RunTest(const FString& Parameters)
{
	VoxelWaterPropagationTest::RunWorld(*this, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterChunkArrivalPlaneTest, "VoxelWorlds.Streaming.WaterPropagation.ChunkArrival.Plane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelWaterChunkArrivalPlaneTest::RunTest(const FString& Parameters)
{
	VoxelWaterPropagationTest::RunChunkArrival(*this, false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterChunkArrivalPlanetTest, "VoxelWorlds.Streaming.WaterPropagation.ChunkArrival.Planet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelWaterChunkArrivalPlanetTest::RunTest(const FString& Parameters)
{
	VoxelWaterPropagationTest::RunChunkArrival(*this, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterTilesPlaneTest, "VoxelWorlds.Streaming.WaterPropagation.Tiles.Plane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelWaterTilesPlaneTest::RunTest(const FString& Parameters)
{
	VoxelWaterPropagationTest::RunInlandChannelTile(*this);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
