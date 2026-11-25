// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "HAL/IConsoleManager.h"
#include "VoxelWorldsSettings.generated.h"

/**
 * The voxel engine's project-wide tuning (Project Settings > Plugins > Voxel Worlds; code review CR4, the plan's §10.4):
 * values a shipped game would set once for every world. Each has a console variable of the same meaning that overrides it
 * once something sets the variable (the console, an ini, a test), so A/B measurement works as before; a world's own
 * scale-dependent values are on UVoxelWorldConfiguration. The defaults are the variables' defaults.
 */
UCLASS(config = Engine, defaultconfig, meta = (DisplayName = "Voxel Worlds"))
class VOXELCORE_API UVoxelWorldsSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/** @return The settings (the class default object) */
	static const UVoxelWorldsSettings& Get() { return *GetDefault<UVoxelWorldsSettings>(); }

	// ==================== Collision ====================

	/** Seconds of the focus's velocity added to the collision radius ahead of a moving pawn (voxel.Collision.LeadSeconds) */
	UPROPERTY(config, EditAnywhere, Category = "Collision", meta = (ClampMin = "0"))
	float CollisionLeadSeconds = 0.75f;

	/** Re-run the collision decision sweep at least this often, in seconds, while the focus stands still (voxel.Collision.ResweepSeconds) */
	UPROPERTY(config, EditAnywhere, Category = "Collision", meta = (ClampMin = "0"))
	float CollisionResweepSeconds = 1.0f;

	/** Minimum focus movement (uu) before the collision decisions are swept again (voxel.Collision.MoveUpdateThreshold) */
	UPROPERTY(config, EditAnywhere, Category = "Collision", meta = (ClampMin = "1"))
	float CollisionMoveUpdateThreshold = 200.0f;

	/**
	 * A focus that moves more than a chunk in one tick has jumped (a teleport, a respawn): the collision shell takes no
	 * speed from the jump, so its radius stays its own instead of reaching back over the place left; off = the jump counts
	 * as speed, as before G282 (voxel.Collision.JumpIsCut)
	 */
	UPROPERTY(config, EditAnywhere, Category = "Collision")
	bool bCollisionJumpIsCut = true;

	/** Most cooked collision meshes registered with physics per frame, 1 to 16 (voxel.Collision.MaxAppliesPerFrame) */
	UPROPERTY(config, EditAnywhere, Category = "Collision", meta = (ClampMin = "1", ClampMax = "16"))
	int32 CollisionMaxAppliesPerFrame = 4;

	/** Chunks ahead of a moving pawn whose collision is cooked first, 0 to 8 (voxel.Collision.LookAheadChunks) */
	UPROPERTY(config, EditAnywhere, Category = "Collision", meta = (ClampMin = "0", ClampMax = "8"))
	int32 CollisionLookAheadChunks = 2;

	/**
	 * A chunk whose cook would read no surface (its voxels and the planes it reads of its neighbours all air or all solid)
	 * is ready-empty without the mesher; off = every such chunk is meshed, as before G283 (voxel.Collision.SkipNoSurface)
	 */
	UPROPERTY(config, EditAnywhere, Category = "Collision")
	bool bCollisionSkipNoSurface = true;

	/**
	 * Decision D196 (open, the user's): a chunk's first cook away from every focus waits while a neighbour is still being
	 * generated, trading when its collision arrives for the re-cook it saves; off = no chunk waits, as before G283
	 * (voxel.Collision.NeighborWait)
	 */
	UPROPERTY(config, EditAnywhere, Category = "Collision")
	bool bCollisionNeighborWait = false;

	/** With the neighbour wait, chunks within this many chunk widths of a focus never wait (voxel.Collision.NeighborWaitChunks) */
	UPROPERTY(config, EditAnywhere, Category = "Collision", meta = (ClampMin = "0"))
	float CollisionNeighborWaitChunks = 2.0f;

	/** With the neighbour wait, the longest a first cook waits for its neighbours, in seconds from its queueing (voxel.Collision.NeighborWaitMaxSeconds) */
	UPROPERTY(config, EditAnywhere, Category = "Collision", meta = (ClampMin = "0"))
	float CollisionNeighborWaitMaxSeconds = 3.0f;

	// ==================== Surface navigation ====================

	/** Surface navigation grid cell size (uu); changing it drops the height cache (voxel.Nav.CellSize) */
	UPROPERTY(config, EditAnywhere, Category = "Navigation", meta = (ClampMin = "25"))
	float NavCellSize = 150.0f;

	/** Node budget per surface path search (voxel.Nav.MaxNodes) */
	UPROPERTY(config, EditAnywhere, Category = "Navigation", meta = (ClampMin = "16"))
	int32 NavMaxNodes = 4096;

	/** Largest height difference between neighbouring cells a walker may take (uu; voxel.Nav.MaxStepRise) */
	UPROPERTY(config, EditAnywhere, Category = "Navigation", meta = (ClampMin = "0"))
	float NavMaxStepRise = 100.0f;

	/** Cells this far below the water surface are still walkable (uu; voxel.Nav.MaxWadeDepth) */
	UPROPERTY(config, EditAnywhere, Category = "Navigation", meta = (ClampMin = "0"))
	float NavMaxWadeDepth = 60.0f;

	/** Cells farther than this from the start are never expanded (uu; voxel.Nav.MaxRadius) */
	UPROPERTY(config, EditAnywhere, Category = "Navigation", meta = (ClampMin = "0"))
	float NavMaxRadius = 20000.0f;

	/**
	 * Milliseconds a frame the path searches begun with StartPathSearch take together; a search that has not ended goes on
	 * in the next frames, with the same route; 0 = each runs to its end at once (voxel.Nav.SearchBudgetMs, G281)
	 */
	UPROPERTY(config, EditAnywhere, Category = "Navigation", meta = (ClampMin = "0"))
	float NavSearchBudgetMs = 2.0f;

	// ==================== Rendering ====================

	// ==================== Streaming ====================

	/**
	 * Milliseconds a chunk manager tick may take before the work that can wait a tick waits (FVoxelTickBudget, G282): the
	 * far field's build launches, the lakes' and river reaches' meshes, the collision components of chunks that left the
	 * shell, each at least one piece a tick, nearest first; the collision cooks and the meshes' submits are not held by it;
	 * 0 = no budget (voxel.Stream.TickBudgetMs)
	 */
	UPROPERTY(config, EditAnywhere, Category = "Streaming", meta = (ClampMin = "0"))
	float StreamTickBudgetMs = 4.0f;

	/** Crossfade duration of a chunk mesh swap, in seconds (voxel.MeshFade.Duration) */
	UPROPERTY(config, EditAnywhere, Category = "Rendering", meta = (ClampMin = "0"))
	float MeshFadeDuration = 0.35f;

	/** Nearest distance (uu) at which a new chunk fades in; nearer, it appears at once (voxel.MeshFade.AppearMinDistance) */
	UPROPERTY(config, EditAnywhere, Category = "Rendering", meta = (ClampMin = "0"))
	float MeshFadeAppearMinDistance = 7000.0f;

	// ==================== Scatter ====================

	/** A scatter HISM applies its held instance changes at most this often, in seconds; 0 = at once (voxel.Scatter.HISMBatchSeconds) */
	UPROPERTY(config, EditAnywhere, Category = "Scatter", meta = (ClampMin = "0"))
	float ScatterHISMBatchSeconds = 0.1f;

	/**
	 * Milliseconds a tick the due scatter HISM batches may take, the rest waiting for the next tick (the overdue, then the
	 * nearest first); one compaction a tick at most; 0 = every due batch at once (voxel.Scatter.HISMWriteBudgetMs, G281)
	 */
	UPROPERTY(config, EditAnywhere, Category = "Scatter", meta = (ClampMin = "0"))
	float ScatterHISMWriteBudgetMs = 2.0f;

	/** A scatter HISM whose pooled (released) instances reach this and outweigh its live ones is compacted (voxel.Scatter.HISMCompactMinPooled) */
	UPROPERTY(config, EditAnywhere, Category = "Scatter", meta = (ClampMin = "0"))
	int32 ScatterHISMCompactMinPooled = 1024;

	// ==================== Editing ====================

	/** Memory budget of the edit undo and redo history, in MB; the oldest operations go first (voxel.Edit.UndoHistoryMB) */
	UPROPERTY(config, EditAnywhere, Category = "Editing", meta = (ClampMin = "0"))
	float EditUndoHistoryMB = 32.0f;
};

/** Resolving a console variable that overrides a setting or a configured value (code review CR4, the plan's §10.4). */
namespace VoxelSettings
{
	/** @return Whether something set the variable (the console, an ini, code), so its value overrides the configured one */
	inline bool IsOverridden(const IConsoleVariable* Var)
	{
		return Var && (static_cast<uint32>(Var->GetFlags()) & static_cast<uint32>(ECVF_SetByMask)) > static_cast<uint32>(ECVF_SetByConstructor);
	}

	/** @return The variable's value when something set it, else the configured value (game thread) */
	template <typename T>
	T Resolve(const TAutoConsoleVariable<T>& Var, T Configured)
	{
		return IsOverridden(Var.AsVariable()) ? Var.GetValueOnGameThread() : Configured;
	}

	/** @return The variable's value when something set it, else the configured value (any thread) */
	template <typename T>
	T ResolveAnyThread(const TAutoConsoleVariable<T>& Var, T Configured)
	{
		return IsOverridden(Var.AsVariable()) ? Var.GetValueOnAnyThread() : Configured;
	}
}
