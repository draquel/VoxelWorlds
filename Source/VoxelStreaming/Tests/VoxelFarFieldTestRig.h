// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "VoxelPlanetFarField.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "IVoxelWorldMode.h"
#include "IVoxelMeshRenderer.h"
#include "ChunkRenderData.h"
#include "DistanceBandLODStrategy.h"

/**
 * The far-field suites' one rig (code review CR2, STREAM-22): a far field over a world configuration with synchronous
 * builds, its view, convergence and window for the sphere and the plane alike through the far field's frame
 * (FVoxelFarFieldFrame). The planet and heightmap suites derive their rigs from it with their own settings and the
 * helpers of their geometry (a patch's edges on the sphere, on the plane).
 */
namespace VoxelFarFieldTestRig
{
	/**
	 * Sets a console variable for a scope at the priority it already holds: a set below that priority is ignored (a test
	 * that set it by console earlier in a suite run), and a set above it would outrank the other tests' code-priority sets.
	 */
	struct FScopedCVar
	{
		IConsoleVariable* Var = nullptr;
		FString Previous;
		EConsoleVariableFlags Priority = ECVF_SetByCode;
		FScopedCVar(const TCHAR* Name, const TCHAR* Value)
		{
			Var = IConsoleManager::Get().FindConsoleVariable(Name);
			if (Var)
			{
				Previous = Var->GetString();
				Priority = static_cast<EConsoleVariableFlags>(FMath::Max(static_cast<uint32>(Var->GetFlags() & ECVF_SetByMask), static_cast<uint32>(ECVF_SetByCode)));
				Var->Set(Value, Priority);
			}
		}
		~FScopedCVar()
		{
			if (Var)
			{
				Var->Set(*Previous, Priority);
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
		virtual void SetMaterial(UMaterialInterface* Material) override {}
		virtual UMaterialInterface* GetMaterial() const override { return nullptr; }
		virtual void UpdateMaterialParameters() override {}
		virtual void UpdateSeamMeshFromCPU(const FIntVector& OwnerChunkCoord, uint8 SeamSlot, int32 LODLevel, FChunkMeshData&& MeshData, bool bCrossfade) override {}
		virtual void RemoveSeamMesh(const FIntVector& OwnerChunkCoord, uint8 SeamSlot) override {}
		virtual void ClearAllSeamMeshes() override {}
		virtual void GetSeamMeshOwners(TArray<FIntVector>& OutOwners) const override {}
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

	/** A far field over a world configuration (rooted; released with the rig) with synchronous builds, and the selection's view for a viewer. */
	struct FRig
	{
		UVoxelWorldConfiguration* Config = nullptr;
		TSharedPtr<IVoxelWorldMode> Mode;
		TSharedPtr<const FVoxelFarFieldSurface> Surface;
		FVoxelFarFieldSettings Settings;
		FVoxelPlanetFarField FarField;
		const TArray<FVoxelConditioningZone> NoZones;

		/** The voxel window the views describe: its load and unload distances */
		double ViewDistance = 0.0;
		double UnloadDistance = 0.0;

		FRig(UVoxelWorldConfiguration* InConfig, TFunctionRef<FVoxelFarFieldSettings(const UVoxelWorldConfiguration&, const IVoxelWorldMode&, const FVoxelFarFieldSurface&)> MakeSettings,
			double InViewDistance, double InUnloadDistance)
			: Config(InConfig), ViewDistance(InViewDistance), UnloadDistance(InUnloadDistance)
		{
			Mode = FVoxelWorldModeFactory::Create(*Config);
			Surface = FVoxelFarFieldSurface::Make(*Config, Mode);
			Settings = MakeSettings(*Config, *Mode, *Surface);
			FarField.Initialize(Settings, Surface);
			FarField.SetSynchronousBuilds(true);
			FarField.SetMaxBuildsInFlight(256);
		}

		~FRig()
		{
			FarField.Reset();
			Config->RemoveFromRoot();
		}

		const FVoxelFarFieldFrame& Frame() const { return FarField.GetFrame(); }

		/** The surface's altitude at a sample (no zones), and the surface point there. */
		double Altitude(const FVoxelFarFieldSample& Sample) const { return Surface->SampleAltitude(Sample, NoZones); }
		FVector SurfacePoint(const FVoxelFarFieldSample& Sample) const { return Frame().PointAt(Sample, Altitude(Sample)); }

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
			// The ground radius under the viewer on a planet (the analytic terrain along its direction); a plane has none.
			View.GroundRadius = Frame().IsHeightmap() ? 0.0 : Settings.Radius + Altitude(FVoxelFarFieldSample::OfDirection((Viewer - Settings.Center).GetSafeNormal()));
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

		/**
		 * A loaded voxel window: a chunk is drawn when its centre lies within Radius of the viewer's ground point, along the
		 * ground on a planet (the strategy's load rule, WM-BP8), on the plane by its column's centre.
		 */
		bool InWindow(const FVoxelFarFieldView& View, const FIntVector& Coord, double Radius) const
		{
			if (Frame().IsHeightmap())
			{
				const FVector2D ColumnCentre(Settings.GridOrigin.X + (Coord.X + 0.5) * Settings.ChunkWorldSize, Settings.GridOrigin.Y + (Coord.Y + 0.5) * Settings.ChunkWorldSize);
				return FVector2D::Distance(ColumnCentre, FVector2D(View.Viewer.X, View.Viewer.Y)) <= Radius;
			}
			return FDistanceBandLODStrategy::GetPlanetGroundDistance(Settings.Center, View.GroundRadius, View.Viewer, ChunkCentre(Coord)) <= Radius;
		}
	};
}
