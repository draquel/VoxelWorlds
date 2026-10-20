// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
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

	/** Configure and start a small world with a sea; three flat seabed sites 5-10 voxels under it, pairwise at least 900 uu apart. */
	bool StartWorld(VoxelTesting::FTestChunkWorld& W, bool bPlanet, TArray<FVector>& OutSites)
	{
		W.Create(bPlanet ? TEXT("WaterPropagationPlanet") : TEXT("WaterPropagationPlane"));
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
		W.StartManager();

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

#endif // WITH_DEV_AUTOMATION_TESTS
