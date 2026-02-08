// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelBillboardMeshGenerator.h"
#include "VoxelScatter.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "StaticMeshDescription.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

#if WITH_EDITORONLY_DATA
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#endif

TMap<int64, TWeakObjectPtr<UStaticMesh>> FVoxelBillboardMeshGenerator::CachedMeshes;

int64 FVoxelBillboardMeshGenerator::MakeCacheKey(float Width, float Height)
{
	// Pack rounded dimensions into a 64-bit key (mm precision)
	const int32 W = FMath::RoundToInt(Width * 10.0f);
	const int32 H = FMath::RoundToInt(Height * 10.0f);
	return (static_cast<int64>(W) << 32) | static_cast<int64>(H);
}

static uint64 MakeAtlasCacheKey(float Width, float Height, FVector2f UVMin, FVector2f UVMax)
{
	// Combine size + UV bounds into a hash for cache lookup
	const uint32 W = static_cast<uint32>(FMath::RoundToInt(Width * 10.0f));
	const uint32 H = static_cast<uint32>(FMath::RoundToInt(Height * 10.0f));
	const uint32 UMin = static_cast<uint32>(FMath::RoundToInt(UVMin.X * 10000.0f));
	const uint32 VMin = static_cast<uint32>(FMath::RoundToInt(UVMin.Y * 10000.0f));
	const uint32 UMax = static_cast<uint32>(FMath::RoundToInt(UVMax.X * 10000.0f));
	const uint32 VMax = static_cast<uint32>(FMath::RoundToInt(UVMax.Y * 10000.0f));
	uint64 Key = static_cast<uint64>(W) ^ (static_cast<uint64>(H) << 16);
	Key ^= (static_cast<uint64>(UMin) << 32) ^ (static_cast<uint64>(VMin) << 48);
	Key ^= static_cast<uint64>(UMax) * 2654435761u;
	Key ^= static_cast<uint64>(VMax) * 40503u;
	return Key;
}

UStaticMesh* FVoxelBillboardMeshGenerator::GetOrCreateBillboardMesh(float Width, float Height,
	FVector2f UVMin, FVector2f UVMax)
{
	const int64 Key = static_cast<int64>(MakeAtlasCacheKey(Width, Height, UVMin, UVMax));

	// Check cache
	if (TWeakObjectPtr<UStaticMesh>* Existing = CachedMeshes.Find(Key))
	{
		if (Existing->IsValid())
		{
			return Existing->Get();
		}
		CachedMeshes.Remove(Key);
	}

	// Create a new transient static mesh
	UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	if (!Mesh)
	{
		return nullptr;
	}

	// Build mesh description with 2 intersecting quads
	UStaticMeshDescription* MeshDesc = Mesh->CreateStaticMeshDescription();
	if (!MeshDesc)
	{
		return nullptr;
	}

	FMeshDescription& MD = MeshDesc->GetMeshDescription();
	FStaticMeshAttributes Attributes(MD);
	Attributes.Register();

	const float HalfWidth = Width * 0.5f;

	// Polygon group (single material)
	const FPolygonGroupID PolyGroup = MD.CreatePolygonGroup();

	// Quad 1: XZ plane (aligned along X axis, height along Z)
	// Quad 2: YZ plane (aligned along Y axis, height along Z)
	// Both pivot at bottom center (0,0,0)

	struct FQuadVert
	{
		FVector3f Position;
		FVector3f Normal;
		FVector2f UV;
	};

	// Quad 1 (XZ plane): faces +/-Y
	const FQuadVert Quad1[4] = {
		{ FVector3f(-HalfWidth, 0.0f, 0.0f),     FVector3f(0.0f, 1.0f, 0.0f), FVector2f(UVMin.X, UVMax.Y) },
		{ FVector3f( HalfWidth, 0.0f, 0.0f),     FVector3f(0.0f, 1.0f, 0.0f), FVector2f(UVMax.X, UVMax.Y) },
		{ FVector3f( HalfWidth, 0.0f, Height),    FVector3f(0.0f, 1.0f, 0.0f), FVector2f(UVMax.X, UVMin.Y) },
		{ FVector3f(-HalfWidth, 0.0f, Height),    FVector3f(0.0f, 1.0f, 0.0f), FVector2f(UVMin.X, UVMin.Y) },
	};

	// Quad 2 (YZ plane): faces +/-X
	const FQuadVert Quad2[4] = {
		{ FVector3f(0.0f, -HalfWidth, 0.0f),     FVector3f(1.0f, 0.0f, 0.0f), FVector2f(UVMin.X, UVMax.Y) },
		{ FVector3f(0.0f,  HalfWidth, 0.0f),     FVector3f(1.0f, 0.0f, 0.0f), FVector2f(UVMax.X, UVMax.Y) },
		{ FVector3f(0.0f,  HalfWidth, Height),    FVector3f(1.0f, 0.0f, 0.0f), FVector2f(UVMax.X, UVMin.Y) },
		{ FVector3f(0.0f, -HalfWidth, Height),    FVector3f(1.0f, 0.0f, 0.0f), FVector2f(UVMin.X, UVMin.Y) },
	};

	auto AddQuad = [&](const FQuadVert Verts[4])
	{
		TArray<FVertexInstanceID> VertInstances;
		VertInstances.Reserve(4);

		for (int32 i = 0; i < 4; ++i)
		{
			const FVertexID VertID = MD.CreateVertex();
			Attributes.GetVertexPositions()[VertID] = Verts[i].Position;

			const FVertexInstanceID VertInstID = MD.CreateVertexInstance(VertID);
			Attributes.GetVertexInstanceNormals()[VertInstID] = Verts[i].Normal;
			Attributes.GetVertexInstanceUVs()[VertInstID] = Verts[i].UV;

			VertInstances.Add(VertInstID);
		}

		// Triangle 1: 0-1-2
		{
			TArray<FVertexInstanceID> Tri;
			Tri.Add(VertInstances[0]);
			Tri.Add(VertInstances[1]);
			Tri.Add(VertInstances[2]);
			MD.CreatePolygon(PolyGroup, Tri);
		}

		// Triangle 2: 0-2-3
		{
			TArray<FVertexInstanceID> Tri;
			Tri.Add(VertInstances[0]);
			Tri.Add(VertInstances[2]);
			Tri.Add(VertInstances[3]);
			MD.CreatePolygon(PolyGroup, Tri);
		}
	};

	AddQuad(Quad1);
	AddQuad(Quad2);

	// Build the static mesh from mesh description
	TArray<UStaticMeshDescription*> MeshDescriptions;
	MeshDescriptions.Add(MeshDesc);
	Mesh->BuildFromStaticMeshDescriptions(MeshDescriptions, false);

	// Configure for HISM use
	Mesh->NeverStream = true;

	// Cache
	CachedMeshes.Add(Key, Mesh);

	UE_LOG(LogVoxelScatter, Log, TEXT("Created cross-billboard mesh (%.0f x %.0f cm, UV [%.3f,%.3f]-[%.3f,%.3f])"),
		Width, Height, UVMin.X, UVMin.Y, UVMax.X, UVMax.Y);
	return Mesh;
}

// Cached runtime billboard base material (two-sided, masked, with texture parameter)
static TWeakObjectPtr<UMaterial> CachedRuntimeBillboardBaseMaterial;

static UMaterial* GetOrCreateRuntimeBillboardBaseMaterial()
{
	if (CachedRuntimeBillboardBaseMaterial.IsValid())
	{
		return CachedRuntimeBillboardBaseMaterial.Get();
	}

	UMaterial* Mat = NewObject<UMaterial>(GetTransientPackage(), TEXT("M_Billboard_Runtime"), RF_Transient);
	Mat->TwoSided = true;
	Mat->BlendMode = BLEND_Masked;
	Mat->SetShadingModel(MSM_DefaultLit);
	Mat->bUsedWithInstancedStaticMeshes = true;

#if WITH_EDITORONLY_DATA
	// Create texture sample parameter connected to BaseColor and OpacityMask
	UMaterialExpressionTextureSampleParameter2D* TexParam =
		NewObject<UMaterialExpressionTextureSampleParameter2D>(Mat);
	TexParam->ParameterName = TEXT("BaseTexture");
	TexParam->SamplerType = SAMPLERTYPE_Color;
	TexParam->MaterialExpressionEditorX = -300;
	TexParam->MaterialExpressionEditorY = 0;
	Mat->GetExpressionCollection().AddExpression(TexParam);

	// Connect RGB output (index 0) to BaseColor
	UMaterialEditorOnlyData* EditorData = Mat->GetEditorOnlyData();
	if (EditorData)
	{
		EditorData->BaseColor.Expression = TexParam;
		EditorData->BaseColor.OutputIndex = 0;

		// Connect Alpha output (index 4) to OpacityMask
		EditorData->OpacityMask.Expression = TexParam;
		EditorData->OpacityMask.OutputIndex = 4;
	}

	// Trigger material compilation
	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
#endif

	CachedRuntimeBillboardBaseMaterial = Mat;
	UE_LOG(LogVoxelScatter, Log, TEXT("Created runtime billboard base material (TwoSided, Masked)"));
	return Mat;
}

UMaterialInstanceDynamic* FVoxelBillboardMeshGenerator::CreateBillboardMaterial(UTexture2D* Texture, UObject* Outer)
{
	if (!Outer)
	{
		return nullptr;
	}

	// Try to load user-provided billboard master material first
	static const FString BillboardMaterialPath = TEXT("/VoxelWorlds/Materials/M_Billboard_Master");
	UMaterialInterface* BaseMaterial = Cast<UMaterialInterface>(
		StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, *BillboardMaterialPath));

	if (!BaseMaterial)
	{
		// Create runtime billboard material as fallback
		BaseMaterial = GetOrCreateRuntimeBillboardBaseMaterial();
	}

	if (!BaseMaterial)
	{
		UE_LOG(LogVoxelScatter, Warning, TEXT("Failed to create billboard base material"));
		return nullptr;
	}

	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMaterial, Outer);
	if (MID && Texture)
	{
		MID->SetTextureParameterValue(TEXT("BaseTexture"), Texture);
	}

	return MID;
}

void FVoxelBillboardMeshGenerator::ClearCache()
{
	CachedMeshes.Empty();
	CachedRuntimeBillboardBaseMaterial.Reset();
}
