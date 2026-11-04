// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/MeshComponent.h"
#include "DynamicMeshBuilder.h"
#include "VoxelFarFieldPatchComponent.generated.h"

struct FVoxelFarFieldSection;

/** One section of a far-field patch component: what it draws, whether it shows, whether it may be ray traced. */
struct FVoxelFarFieldPatchSection
{
	/** The vertices as the proxy uploads them (four texture coordinates: UV0, UV1 and two zero channels) */
	TArray<FDynamicMeshVertex> Vertices;
	TArray<uint32> Indices;
	FBox LocalBox = FBox(ForceInit);
	bool bVisible = true;
	/** Whether a proxy builds this section's ray tracing geometry when the component can be traced (the terrain, not the sea) */
	bool bRayTracing = false;

	bool IsEmpty() const { return Vertices.Num() == 0 || Indices.Num() == 0; }
};

/**
 * One far-field patch (FVoxelFarFieldPresenter): sections drawn through FLocalVertexFactory like a procedural mesh
 * component's, section 0 the terrain, 1 the sea's top and 2 its underside. Unlike UProceduralMeshComponent, whose proxy
 * builds a ray tracing acceleration structure for every section whenever ray tracing is enabled (G121), the proxy builds
 * one only for the sections marked for ray tracing, and only while something can trace the component: it is in the
 * ray tracing scene (bVisibleInRayTracing) and, in the engine's far-field layer (bRayTracingFarField), Lumen's far field
 * is on (r.LumenScene.FarField, whose change recreates every proxy).
 */
UCLASS(ClassGroup = Rendering, hidecategories = (Object, LOD, Collision, Physics))
class VOXELSTREAMING_API UVoxelFarFieldPatchComponent : public UMeshComponent
{
	GENERATED_BODY()

public:
	UVoxelFarFieldPatchComponent(const FObjectInitializer& ObjectInitializer);

	/**
	 * Fill a section from a far-field mesh (an empty mesh clears it); the proxy is recreated.
	 * @param SectionIndex  The section (0 terrain, 1 sea, 2 the sea's underside)
	 * @param Mesh          The section's vertices and triangles, in the component's space
	 * @param bReverse      Reverse every triangle's winding (an underside)
	 * @param bRayTracing   Whether the section is traced when the component can be (FVoxelFarFieldPatchSection::bRayTracing)
	 */
	void SetSection(int32 SectionIndex, const FVoxelFarFieldSection& Mesh, bool bReverse, bool bRayTracing);

	/**
	 * Empty a section (its slot stays); the proxy is recreated.
	 * @param SectionIndex  The section
	 */
	void ClearSection(int32 SectionIndex);

	/** Remove every section; the proxy is recreated. */
	void ClearAllSections();

	/**
	 * Show or hide a section, for drawing and ray tracing, without recreating the proxy.
	 * @param SectionIndex  The section
	 * @param bNewVisible   Whether it shows
	 */
	void SetSectionVisible(int32 SectionIndex, bool bNewVisible);

	/**
	 * @param SectionIndex  The section
	 * @return Whether the section exists and shows
	 */
	bool IsSectionVisible(int32 SectionIndex) const;

	/** @return How many section slots the component has (empty ones included) */
	int32 GetNumSections() const { return Sections.Num(); }

	/**
	 * @param SectionIndex  The section
	 * @return The section, or null past the last slot
	 */
	const FVoxelFarFieldPatchSection* GetSection(int32 SectionIndex) const;

	/** @return Whether the component's ray tracing layer is traced: in the ray tracing scene, and in the far-field layer only with Lumen's far field on */
	bool IsInTracedLayer() const;

	/** @return Whether a proxy created now builds ray tracing geometry: ray tracing enabled and IsInTracedLayer */
	bool CanBeRayTraced() const;

	/** @return The sections a proxy builds ray tracing geometry for where ray tracing is enabled (non-empty, marked, IsInTracedLayer) */
	int32 GetRayTracingSectionCount() const;

	/** @return The ray tracing geometries the current proxy holds (render thread state; flush rendering commands first; tests) */
	int32 GetProxyRayTracingGeometryCount() const;

	//~ UPrimitiveComponent
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	//~ UMeshComponent
	virtual int32 GetNumMaterials() const override;

private:
	/** Recompute the local bounds from the sections' boxes and send them to the render thread. */
	void UpdateLocalBounds();

	TArray<FVoxelFarFieldPatchSection> Sections;

	/** The sections' boxes together, in the component's space */
	FBoxSphereBounds LocalBounds = FBoxSphereBounds(FVector::ZeroVector, FVector::ZeroVector, 0.0);
};
