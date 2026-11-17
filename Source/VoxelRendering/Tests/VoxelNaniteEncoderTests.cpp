// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelNaniteEncoder.h"
#include "VoxelLocalVertexFactory.h"
#include "Rendering/NaniteResources.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FVoxelLocalVertex MakeVertex(const FVector3f& Position, const FVector3f& Normal, FRandomStream& Random)
	{
		FVoxelVertex Voxel;
		Voxel.Position = Position;
		Voxel.UV = FVector2f(Position.X / 100.0f, Position.Y / 100.0f);
		Voxel.SetNormal(Normal);
		FVoxelLocalVertex Vertex = FVoxelLocalVertex::FromVoxelVertex(Voxel);
		// Material ID and face type as the mesher writes them; biome and AO in the colour
		Vertex.TexCoord1 = FVector2f(float(Random.RandRange(0, 40)), float(Random.RandRange(0, 2)));
		Vertex.Color = FColor(0, uint8(Random.RandRange(0, 12)), uint8(Random.RandRange(0, 3) << 6), 255);
		return Vertex;
	}

	/** A heightfield grid: shared vertices, two triangles a cell (the shape of a terrain chunk's surface) */
	void MakeGrid(int32 Cells, float CellSize, float Amplitude, const FVector3f& Offset, TArray<FVoxelLocalVertex>& OutVertices, TArray<uint32>& OutIndices, FRandomStream& Random)
	{
		const int32 Base = OutVertices.Num();
		for (int32 Y = 0; Y <= Cells; ++Y)
		{
			for (int32 X = 0; X <= Cells; ++X)
			{
				const float Height = Amplitude * FMath::Sin(X * 0.37f) * FMath::Cos(Y * 0.23f);
				const FVector3f Normal = FVector3f(-Amplitude * 0.37f * FMath::Cos(X * 0.37f) * FMath::Cos(Y * 0.23f) / CellSize,
					Amplitude * 0.23f * FMath::Sin(X * 0.37f) * FMath::Sin(Y * 0.23f) / CellSize, 1.0f).GetSafeNormal();
				OutVertices.Add(MakeVertex(Offset + FVector3f(X * CellSize, Y * CellSize, Height), Normal, Random));
			}
		}
		for (int32 Y = 0; Y < Cells; ++Y)
		{
			for (int32 X = 0; X < Cells; ++X)
			{
				const uint32 A = Base + Y * (Cells + 1) + X;
				const uint32 B = A + 1;
				const uint32 C = A + (Cells + 1);
				const uint32 D = C + 1;
				OutIndices.Append({ A, C, B, B, C, D });
			}
		}
	}

	/** A sphere: normals in every direction, triangles whose vertices are far apart in index order (the reference window) */
	void MakeSphere(int32 Rings, int32 Segments, float Radius, TArray<FVoxelLocalVertex>& OutVertices, TArray<uint32>& OutIndices, FRandomStream& Random)
	{
		const int32 Base = OutVertices.Num();
		for (int32 Ring = 0; Ring <= Rings; ++Ring)
		{
			const float Theta = PI * Ring / Rings;
			for (int32 Segment = 0; Segment <= Segments; ++Segment)
			{
				const float Phi = 2.0f * PI * Segment / Segments;
				const FVector3f Normal(FMath::Sin(Theta) * FMath::Cos(Phi), FMath::Sin(Theta) * FMath::Sin(Phi), FMath::Cos(Theta));
				OutVertices.Add(MakeVertex(Normal * Radius + FVector3f(1600.0f), Normal, Random));
			}
		}
		for (int32 Ring = 0; Ring < Rings; ++Ring)
		{
			for (int32 Segment = 0; Segment < Segments; ++Segment)
			{
				const uint32 A = Base + Ring * (Segments + 1) + Segment;
				const uint32 B = A + 1;
				const uint32 C = A + (Segments + 1);
				const uint32 D = C + 1;
				OutIndices.Append({ A, B, C, B, D, C });
			}
		}
	}

	bool RunCase(FAutomationTestBase& Test, const TCHAR* Name, const TArray<FVoxelLocalVertex>& Vertices, const TArray<uint32>& Indices,
		bool bVertexReuse, FVoxelNaniteEncodeStats& OutStats)
	{
		FVoxelNaniteEncodeSettings Settings;
		Settings.bVertexReuse = bVertexReuse;
		TUniquePtr<Nanite::FResources> Resources = EncodeVoxelNaniteMesh(Vertices, Indices, Settings, &OutStats);
		if (!Test.TestTrue(FString::Printf(TEXT("%s: encoded"), Name), Resources.IsValid()))
		{
			return false;
		}
		const FVoxelNaniteValidation Validation = ValidateVoxelNaniteMesh(*Resources, Vertices, Indices, Settings);
		Test.AddInfo(FString::Printf(TEXT("%s (reuse %d): %d triangles, %d clusters, %d pages, %d nodes, %d stored vertices, %lld GPU bytes, ")
			TEXT("%lld stored bytes, precision %d, %.3f ms; position error %.4f, normal dot %.4f, tangent dot %.4f, UV0 error %g"),
			Name, bVertexReuse ? 1 : 0, OutStats.NumTriangles, OutStats.NumClusters, OutStats.NumPages, OutStats.NumHierarchyNodes,
			OutStats.NumStoredVertices, OutStats.GpuBytes, OutStats.RootDataBytes, OutStats.PositionPrecision, OutStats.EncodeSeconds * 1000.0,
			Validation.MaxPositionError, Validation.MinNormalDot, Validation.MinTangentDot, Validation.MaxUV0Error));
		Test.TestTrue(FString::Printf(TEXT("%s: decodes to the input (%s)"), Name, *Validation.Error), Validation.bValid);
		Test.TestEqual(FString::Printf(TEXT("%s: every triangle decoded"), Name), Validation.NumDecodedTriangles, OutStats.NumTriangles);
		Test.TestEqual(FString::Printf(TEXT("%s: every page a root page"), Name), int32(Resources->NumRootPages), Resources->PageStreamingStates.Num());
		return Validation.bValid;
	}
}

/**
 * Runtime Nanite spike, N1 (Documentation/RUNTIME_NANITE_SPIKE_PLAN.md): a chunk mesh encoded as a Nanite resource of
 * root pages decodes on the CPU, as the GPU transcode reads it, to the same triangles in the same order and winding,
 * with positions within half a quantization step, normals and tangents within their bits, colours, tangent signs and
 * UV1 (material and face type) exact. Cases: a terrain-like grid (shared vertices), a sphere (normals everywhere,
 * vertices far apart in index order), a far chunk's span (the position precision lowered to fit 21 bits), degenerate
 * triangles dropped; each with and without vertex reuse. Reuse stores fewer vertices than three a triangle.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNaniteEncodeRoundTripTest,
	"VoxelWorlds.Rendering.Nanite.EncodeRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelNaniteEncodeRoundTripTest::RunTest(const FString& Parameters)
{
	FRandomStream Random(1234);

	TArray<FVoxelLocalVertex> GridVertices;
	TArray<uint32> GridIndices;
	MakeGrid(48, 100.0f, 250.0f, FVector3f::ZeroVector, GridVertices, GridIndices, Random);

	TArray<FVoxelLocalVertex> SphereVertices;
	TArray<uint32> SphereIndices;
	MakeSphere(40, 64, 900.0f, SphereVertices, SphereIndices, Random);

	// A very far chunk: 32 cells of 6,400 uu with 20 km of relief; a cluster's row spans more than 2^21 sixteenths of a uu
	TArray<FVoxelLocalVertex> FarVertices;
	TArray<uint32> FarIndices;
	MakeGrid(32, 6400.0f, 20000.0f, FVector3f(0.0f, 0.0f, -5000.0f), FarVertices, FarIndices, Random);

	// Degenerate triangles among real ones
	TArray<FVoxelLocalVertex> DegenerateVertices = GridVertices;
	TArray<uint32> DegenerateIndices = GridIndices;
	DegenerateIndices.Append({ 0, 0, 1, 5, 6, 5, 7, 7, 7 });

	for (const bool bReuse : { true, false })
	{
		FVoxelNaniteEncodeStats GridStats, SphereStats, FarStats, DegenerateStats;
		RunCase(*this, TEXT("Grid"), GridVertices, GridIndices, bReuse, GridStats);
		RunCase(*this, TEXT("Sphere"), SphereVertices, SphereIndices, bReuse, SphereStats);
		RunCase(*this, TEXT("Far"), FarVertices, FarIndices, bReuse, FarStats);
		RunCase(*this, TEXT("Degenerate"), DegenerateVertices, DegenerateIndices, bReuse, DegenerateStats);

		TestEqual(TEXT("Degenerate triangles dropped"), DegenerateStats.NumTriangles, GridStats.NumTriangles);
		TestTrue(TEXT("The far chunk lowered the position precision"), FarStats.PositionPrecision < 4);
		TestTrue(TEXT("Several pages for the sphere"), SphereStats.NumPages > 1);
		if (bReuse)
		{
			TestTrue(TEXT("Reuse stores under two vertices a triangle on the grid"), GridStats.NumStoredVertices < 2 * GridStats.NumTriangles);
		}
		else
		{
			TestEqual(TEXT("Without reuse every triangle stores three vertices"), GridStats.NumStoredVertices, 3 * GridStats.NumTriangles);
		}
	}

	// Nothing to encode
	TArray<FVoxelLocalVertex> NoVertices;
	TArray<uint32> NoIndices;
	TestFalse(TEXT("No triangles, no resource"), EncodeVoxelNaniteMesh(NoVertices, NoIndices).IsValid());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
