// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWorldComponentOwners.h"
#include "VoxelStreaming.h"
#include "VoxelWorldBoundary.h"
#include "ChunkRenderData.h"
#include "ProceduralMeshComponent.h"
#include "Components/BoxComponent.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"

void FVoxelSeaUnderside::Update(bool bBelow, bool& bShown, TFunctionRef<void(bool)> Apply)
{
	// A sea's underside shows only while the viewer is under its surface (code review CR2, CM-19: the one rule for the
	// far ocean, the planet ocean and the far field's sea).
	if (bBelow == bShown)
	{
		return;
	}
	bShown = bBelow;
	Apply(bBelow);
}

// ==================== World Boundary (WM-I5) ====================

namespace VoxelWorldBoundaryPrivate
{
	/** Walls 5 m thick: a sweep cannot tunnel through, and the outer side never matters */
	constexpr double WallThickness = 500.0;

	/** A circle's chords stay within 50 uu of R + F (63 walls for island S, 158 for M, 223 for L) */
	constexpr double MaxChordError = 50.0;

	/** Height of the walls above the highest terrain: higher than a pawn can climb, build or jump */
	constexpr double HeightAboveTerrain = 50000.0;

	/** Keep a mode's "no bound" height range (IVoxelWorldMode's default) from making boxes millions of km tall */
	constexpr double MaxAbsZ = 2.0e6;
}

void FVoxelWorldBoundaryOwner::Build(AActor* Owner, const FVoxelWorldBounds& Bounds, double ChunkWorldSize, bool bCollision)
{
	using namespace VoxelWorldBoundaryPrivate;
	Destroy();

	const double BottomZ = FMath::Max(static_cast<double>(Bounds.MinZ) - ChunkWorldSize, -MaxAbsZ);
	const double TopZ = FMath::Min(static_cast<double>(Bounds.MaxZ) + HeightAboveTerrain, MaxAbsZ);
	TArray<FVoxelWorldBoundaryWall> BuiltWalls;
	FVoxelWorldBoundary::BuildWalls(Bounds, WallThickness, MaxChordError, BottomZ, TopZ, BuiltWalls);
	if (BuiltWalls.Num() == 0)
	{
		return;
	}

	Walls.Reserve(BuiltWalls.Num());
	for (int32 i = 0; i < BuiltWalls.Num(); ++i)
	{
		const FVoxelWorldBoundaryWall& Wall = BuiltWalls[i];
		UBoxComponent* Box = NewObject<UBoxComponent>(Owner, *FString::Printf(TEXT("VoxelWorldBoundary_%d"), i), RF_Transient);
		Box->SetUsingAbsoluteLocation(true);
		Box->SetUsingAbsoluteRotation(true);
		Box->SetUsingAbsoluteScale(true);
		Box->SetupAttachment(Owner->GetRootComponent());
		Box->SetBoxExtent(Wall.HalfExtent, /*bUpdateOverlaps=*/false);
		Box->SetHiddenInGame(true);
		Box->SetCanEverAffectNavigation(false);
		Box->SetGenerateOverlapEvents(false);
		// Blocks everything that moves (pawns, NPCs, ragdolls, dropped items); traces for sight, interaction and the
		// camera pass, so nothing aimed at the sea stops on an invisible wall.
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Box->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
		Box->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
		Box->SetCollisionEnabled(bCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
		Box->SetWorldLocationAndRotation(Wall.Center, Wall.Rotation);
		Box->RegisterComponent();
		Walls.Add(Box);
	}

	UE_LOG(LogVoxelStreaming, Log,
		TEXT("World boundary: %d wall(s) at the world's edge (%s, centre (%.0f, %.0f), half extent (%.0f, %.0f)), Z %.0f to %.0f, collision %s"),
		Walls.Num(), *StaticEnum<EVoxelWorldBoundsShape>()->GetNameStringByValue(static_cast<int64>(Bounds.Shape)),
		Bounds.Center.X, Bounds.Center.Y, Bounds.HalfExtent.X, Bounds.HalfExtent.Y, BottomZ, TopZ, bCollision ? TEXT("on") : TEXT("off"));
}

void FVoxelWorldBoundaryOwner::Destroy()
{
	for (UBoxComponent* Box : Walls)
	{
		if (IsValid(Box))
		{
			Box->DestroyComponent();
		}
	}
	Walls.Reset();
}

void FVoxelWorldBoundaryOwner::SetCollisionEnabled(bool bEnabled)
{
	for (UBoxComponent* Box : Walls)
	{
		if (IsValid(Box))
		{
			Box->SetCollisionEnabled(bEnabled ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
		}
	}
	if (Walls.Num() > 0)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("World boundary: %d wall(s), collision %s"), Walls.Num(), bEnabled ? TEXT("on") : TEXT("off"));
	}
}

// ==================== Far Ocean (WM-E1) ====================

namespace VoxelFarOceanManagerPrivate
{
	/** Hand a mesh to a procedural mesh section (no collision). Clears the section when the mesh is empty. */
	void SetSection(UProceduralMeshComponent* Component, int32 Section, const FChunkMeshData& Mesh, UMaterialInterface* Material)
	{
		if (!Component)
		{
			return;
		}
		if (Mesh.Positions.Num() == 0 || Mesh.Indices.Num() == 0)
		{
			Component->ClearMeshSection(Section);
			return;
		}
		const int32 N = Mesh.Positions.Num();
		TArray<FVector> Vertices;
		TArray<FVector> Normals;
		TArray<FVector2D> UV0;
		TArray<FVector2D> UV1;
		TArray<FProcMeshTangent> Tangents;
		Vertices.SetNumUninitialized(N);
		Normals.SetNumUninitialized(N);
		UV0.SetNumUninitialized(N);
		UV1.SetNumUninitialized(N);
		Tangents.Init(FProcMeshTangent(1.0f, 0.0f, 0.0f), N);
		for (int32 i = 0; i < N; ++i)
		{
			Vertices[i] = FVector(Mesh.Positions[i]);
			Normals[i] = FVector(Mesh.Normals[i]);
			UV0[i] = FVector2D(Mesh.UVs[i]);
			UV1[i] = FVector2D(Mesh.UV1s[i]);
		}
		TArray<int32> Triangles;
		Triangles.SetNumUninitialized(Mesh.Indices.Num());
		for (int32 i = 0; i < Mesh.Indices.Num(); ++i)
		{
			Triangles[i] = static_cast<int32>(Mesh.Indices[i]);
		}
		const TArray<FVector2D> NoUV;
		Component->CreateMeshSection(Section, Vertices, Triangles, Normals, UV0, UV1, NoUV, NoUV, Mesh.Colors, Tangents,
			/*bCreateCollision=*/false);
		if (Material)
		{
			Component->SetMaterial(Section, Material);
		}
	}

	UProceduralMeshComponent* MakeComponent(AActor* Owner, const TCHAR* Name, const FVector& Location)
	{
		UProceduralMeshComponent* Component = NewObject<UProceduralMeshComponent>(Owner, Name, RF_Transient);
		Component->SetUsingAbsoluteLocation(true);
		Component->SetUsingAbsoluteRotation(true);
		Component->SetUsingAbsoluteScale(true);
		Component->SetupAttachment(Owner->GetRootComponent());
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetCanEverAffectNavigation(false);
		Component->SetGenerateOverlapEvents(false);
		Component->SetCastShadow(false);
		Component->bUseAsyncCooking = false;
		Component->SetWorldLocationAndRotation(Location, FRotator::ZeroRotator);
		Component->RegisterComponent();
		return Component;
	}
}

void FVoxelFarOceanOwner::Build(AActor* Owner, const FVoxelFarOceanSettings& InSettings, UMaterialInterface* SeabedMaterial,
	UMaterialInterface* WaterMaterial, bool bVisible, const FVector& ViewerPosition, double StartTime)
{
	using namespace VoxelFarOceanManagerPrivate;
	Destroy();
	Settings = InSettings;
	const FVector Origin = Settings.GetMeshOrigin();

	// The seabed and the open sea never change: one component. The seabed takes the terrain's own material instance
	// (the atlas parameters), so it reads as the voxel seabed it continues.
	Component = MakeComponent(Owner, TEXT("VoxelFarOceanStatic"), Origin);
	FChunkMeshData Mesh;
	const int32 SeabedTriangles = FVoxelFarOcean::BuildSeabed(Settings, Mesh);
	SetSection(Component, 0, Mesh, SeabedMaterial);

	// The water needs the water tiles' material (without it the world actor draws its fallback plane). Inside the bounds box
	// the far field draws the terrain and the sea (WM-MP1, D37); the open sea starts at the box.
	const bool bWater = WaterMaterial != nullptr;
	int32 OpenSeaQuads = 0;
	if (bWater)
	{
		FChunkMeshData Underside;
		OpenSeaQuads = FVoxelFarOcean::BuildOpenSeaWater(Settings, Mesh, &Underside);
		SetSection(Component, 1, Mesh, WaterMaterial);
		SetSection(Component, 2, Underside, WaterMaterial);
		Component->SetTranslucentSortPriority(0);
	}
	Component->SetVisibility(bVisible);
	bUndersideShown = true; // force the first update to hide it
	UpdateUnderside(ViewerPosition);

	FIntPoint GridMin, GridMax;
	FVoxelFarOcean::GetGridColumns(Settings, GridMin, GridMax);
	UE_LOG(LogVoxelStreaming, Log,
		TEXT("Far ocean: seabed %d triangle(s) (material %d, %.0f uu under the profile), %s from the %dx%d column box; reaches %.0f uu past the edge; built in %.2f ms%s"),
		SeabedTriangles, Settings.SeabedMaterialID, Settings.SeabedOffset,
		bWater ? *FString::Printf(TEXT("open sea %d quad(s)"), OpenSeaQuads) : TEXT("no water (needs bEnableWaterLevel and WaterMeshMaterial)"),
		GridMax.X - GridMin.X + 1, GridMax.Y - GridMin.Y + 1, Settings.OuterDistance,
		(FPlatformTime::Seconds() - StartTime) * 1000.0, bVisible ? TEXT("") : TEXT(" (hidden: voxel.World.FarOcean 0)"));
}

void FVoxelFarOceanOwner::Destroy()
{
	if (IsValid(Component))
	{
		Component->DestroyComponent();
	}
	Component = nullptr;
	bUndersideShown = false;
}

void FVoxelFarOceanOwner::UpdateUnderside(const FVector& ViewerPosition)
{
	FVoxelSeaUnderside::Update(ViewerPosition.Z < Settings.WaterZ, bUndersideShown, [this](bool bBelow)
	{
		if (IsValid(Component) && Component->GetNumSections() > 2)
		{
			Component->SetMeshSectionVisible(2, bBelow);
		}
	});
}

void FVoxelFarOceanOwner::SetVisible(bool bVisible)
{
	if (IsValid(Component))
	{
		Component->SetVisibility(bVisible);
		UE_LOG(LogVoxelStreaming, Log, TEXT("Far ocean %s"), bVisible ? TEXT("shown") : TEXT("hidden"));
	}
}

FString FVoxelFarOceanOwner::Describe() const
{
	if (!Component)
	{
		return TEXT("no far ocean (not an island, bIslandFarOcean off, or not a game world)");
	}
	FIntPoint GridMin, GridMax;
	FVoxelFarOcean::GetGridColumns(Settings, GridMin, GridMax);
	return FString::Printf(TEXT("visible=%d box %dx%d column(s) water=%s seabed material=%d reaches %.0f uu past the edge"),
		Component->IsVisible() ? 1 : 0, GridMax.X - GridMin.X + 1, GridMax.Y - GridMin.Y + 1,
		Component->GetNumSections() > 1 ? TEXT("yes") : TEXT("no"), Settings.SeabedMaterialID, Settings.OuterDistance);
}

// ==================== Planet Ocean (spherical SP-E2) ====================

void FVoxelPlanetOceanOwner::Build(AActor* Owner, const FVoxelPlanetOceanSettings& InSettings, UMaterialInterface* WaterMaterial,
	bool bVisible, const FVector& ViewerPosition)
{
	Destroy();
	const double StartTime = FPlatformTime::Seconds();
	Settings = InSettings;

	FVoxelPlanetOceanMesh Top;
	FVoxelPlanetOceanMesh Underside;
	int32 Segments = 0;
	const int32 Quads = FVoxelPlanetOcean::Build(Settings, Top, &Underside, Segments);
	if (Quads == 0)
	{
		return;
	}

	Component = VoxelFarOceanManagerPrivate::MakeComponent(Owner, TEXT("VoxelPlanetOcean"), Settings.Center);
	auto MakeTangents = [](const TArray<FVector>& In)
	{
		TArray<FProcMeshTangent> Out;
		Out.Reserve(In.Num());
		for (const FVector& T : In)
		{
			Out.Add(FProcMeshTangent(T, false));
		}
		return Out;
	};
	const TArray<FVector2D> NoUV;
	Component->CreateMeshSection(0, Top.Positions, Top.Triangles, Top.Normals, Top.UV0, Top.UV1, NoUV, NoUV, Top.Colors,
		MakeTangents(Top.Tangents), /*bCreateCollision=*/false);
	Component->CreateMeshSection(1, Underside.Positions, Underside.Triangles, Underside.Normals, Underside.UV0, Underside.UV1,
		NoUV, NoUV, Underside.Colors, MakeTangents(Underside.Tangents), /*bCreateCollision=*/false);
	Component->SetMaterial(0, WaterMaterial);
	Component->SetMaterial(1, WaterMaterial);
	Component->SetVisibility(bVisible);
	bUndersideShown = true; // force the first update to decide
	UpdateUnderside(ViewerPosition);

	UE_LOG(LogVoxelStreaming, Log,
		TEXT("Planet ocean: radius %.0f, %d quads a face edge (%d quads, sag at most %.1f uu), built in %.2f ms%s"),
		Settings.Radius, Segments, Quads, FVoxelPlanetOcean::MeasureMaxSag(Settings.Radius, Segments),
		(FPlatformTime::Seconds() - StartTime) * 1000.0,
		bVisible ? TEXT("") : TEXT(" (hidden: voxel.World.PlanetOcean 0)"));
}

void FVoxelPlanetOceanOwner::Destroy()
{
	if (IsValid(Component))
	{
		Component->DestroyComponent();
	}
	Component = nullptr;
	bUndersideShown = false;
}

void FVoxelPlanetOceanOwner::UpdateUnderside(const FVector& ViewerPosition)
{
	if (!Component)
	{
		return;
	}
	const bool bInside = FVector::DistSquared(ViewerPosition, Settings.Center) < Settings.Radius * Settings.Radius;
	FVoxelSeaUnderside::Update(bInside, bUndersideShown, [this](bool bBelow)
	{
		if (Component->GetNumSections() > 1)
		{
			Component->SetMeshSectionVisible(1, bBelow);
		}
	});
}

void FVoxelPlanetOceanOwner::SetVisible(bool bVisible)
{
	if (IsValid(Component))
	{
		Component->SetVisibility(bVisible);
		UE_LOG(LogVoxelStreaming, Log, TEXT("Planet ocean %s"), bVisible ? TEXT("shown") : TEXT("hidden"));
	}
}
