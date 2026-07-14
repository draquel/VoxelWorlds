// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "ChunkDescriptor.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Far-chunk compression — uniform tier (PR B).
// Exercises FChunkDescriptor's uniform-collapse codec: detection, the
// collapse/expand round-trip, the free re-collapse fast path, mutation
// invalidation, the residency predicates, and the memory drop.
// ---------------------------------------------------------------------------

namespace ChunkCompressionTestUtils
{
	/** A resident descriptor of ChunkSize^3 voxels, every voxel set to Fill. */
	static FChunkDescriptor MakeUniform(int32 ChunkSize, const FVoxelData& Fill)
	{
		FChunkDescriptor D(FIntVector::ZeroValue, ChunkSize);
		D.AllocateVoxelData(); // zeroed = air, Resident
		for (FVoxelData& V : D.VoxelData)
		{
			V = Fill;
		}
		return D;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChunkCompressionUniformDetectionTest,
	"VoxelWorlds.Compression.Uniform.Detection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FChunkCompressionUniformDetectionTest::RunTest(const FString& Parameters)
{
	using namespace ChunkCompressionTestUtils;

	// All-air (allocate zeroes) is uniform.
	{
		FChunkDescriptor D(FIntVector::ZeroValue, 4);
		D.AllocateVoxelData();
		FVoxelData Out(255, 255, 255, 255);
		TestTrue(TEXT("all-air is uniform"), FChunkDescriptor::ComputeUniformValue(D.VoxelData, Out));
		TestTrue(TEXT("uniform value is air"), Out == FVoxelData::Air());
	}

	// All-solid is uniform.
	{
		FChunkDescriptor D = MakeUniform(4, FVoxelData::Solid(3, 1));
		FVoxelData Out;
		TestTrue(TEXT("all-solid is uniform"), FChunkDescriptor::ComputeUniformValue(D.VoxelData, Out));
		TestTrue(TEXT("uniform value is the solid voxel"), Out == FVoxelData::Solid(3, 1));
	}

	// A single differing voxel breaks uniformity.
	{
		FChunkDescriptor D = MakeUniform(4, FVoxelData::Air());
		D.VoxelData[17] = FVoxelData::Solid(1);
		FVoxelData Out;
		TestFalse(TEXT("mixed is not uniform"), FChunkDescriptor::ComputeUniformValue(D.VoxelData, Out));
	}

	// Empty array is never uniform.
	{
		TArray<FVoxelData> Empty;
		FVoxelData Out;
		TestFalse(TEXT("empty is not uniform"), FChunkDescriptor::ComputeUniformValue(Empty, Out));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChunkCompressionRoundTripTest,
	"VoxelWorlds.Compression.Uniform.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FChunkCompressionRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace ChunkCompressionTestUtils;

	const FVoxelData Fill = FVoxelData::Solid(7, 2);
	FChunkDescriptor D = MakeUniform(8, Fill);
	const int32 Total = D.GetTotalVoxels();

	// Collapse: raw array dropped, residency flips to Uniform, data still "available".
	TestTrue(TEXT("resident before collapse"), D.IsVoxelDataResident());
	TestTrue(TEXT("collapse succeeds"), D.TryCollapseUniform());
	TestEqual(TEXT("residency is Uniform"), (int32)D.Residency, (int32)EVoxelDataResidency::Uniform);
	TestEqual(TEXT("raw array emptied"), D.VoxelData.Num(), 0);
	TestFalse(TEXT("not resident when Uniform"), D.IsVoxelDataResident());
	TestTrue(TEXT("still available when Uniform"), D.HasVoxelDataAvailable());

	// Expand: full array restored, every voxel identical to the original fill.
	const TArray<FVoxelData>& Restored = D.EnsureResident();
	TestEqual(TEXT("expanded to full size"), Restored.Num(), Total);
	TestEqual(TEXT("residency is Resident after expand"), (int32)D.Residency, (int32)EVoxelDataResidency::Resident);
	bool bAllMatch = true;
	for (const FVoxelData& V : Restored)
	{
		bAllMatch &= (V == Fill);
	}
	TestTrue(TEXT("round-trip preserves every voxel"), bAllMatch);

	// A point read on a compressed chunk decompresses transparently.
	D.TryCollapseUniform(); // re-collapse (free)
	TestEqual(TEXT("re-collapsed to Uniform"), (int32)D.Residency, (int32)EVoxelDataResidency::Uniform);
	const FVoxelData Q = D.GetVoxelResident(FIntVector(1, 2, 3));
	TestTrue(TEXT("point query on Uniform returns the value"), Q == Fill);
	TestEqual(TEXT("point query materialized the array"), (int32)D.Residency, (int32)EVoxelDataResidency::Resident);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChunkCompressionNonUniformTest,
	"VoxelWorlds.Compression.Uniform.NonUniformStaysResident",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FChunkCompressionNonUniformTest::RunTest(const FString& Parameters)
{
	using namespace ChunkCompressionTestUtils;

	FChunkDescriptor D = MakeUniform(4, FVoxelData::Air());
	D.VoxelData[10] = FVoxelData::Solid(1); // break uniformity

	TestFalse(TEXT("non-uniform collapse fails"), D.TryCollapseUniform());
	TestTrue(TEXT("stays resident"), D.IsVoxelDataResident());
	TestTrue(TEXT("marked evaluated so the sweep won't re-scan"), D.bCompressionEvaluated);
	TestFalse(TEXT("no valid uniform value cached"), D.bUniformValueValid);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChunkCompressionMutationTest,
	"VoxelWorlds.Compression.Uniform.MutationInvalidatesCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FChunkCompressionMutationTest::RunTest(const FString& Parameters)
{
	using namespace ChunkCompressionTestUtils;

	FChunkDescriptor D = MakeUniform(4, FVoxelData::Air());
	TestTrue(TEXT("collapse air chunk"), D.TryCollapseUniform());

	// Mutating through the write accessor expands then invalidates the uniform cache.
	TArray<FVoxelData>& Mut = D.GetVoxelDataMutable();
	TestEqual(TEXT("mutable access expands"), (int32)D.Residency, (int32)EVoxelDataResidency::Resident);
	TestFalse(TEXT("uniform cache invalidated by mutable access"), D.bUniformValueValid);
	Mut[5] = FVoxelData::Solid(2); // make it genuinely non-uniform

	// The now-non-uniform chunk must NOT free-collapse to the stale air value.
	TestFalse(TEXT("mutated non-uniform chunk does not collapse"), D.TryCollapseUniform());
	TestTrue(TEXT("stays resident after failed collapse"), D.IsVoxelDataResident());
	TestTrue(TEXT("mutated voxel survived (no stale re-collapse)"), D.GetVoxel(FIntVector(1, 1, 0)) == FVoxelData::Solid(2));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChunkCompressionMemoryTest,
	"VoxelWorlds.Compression.Uniform.MemoryDrops",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FChunkCompressionMemoryTest::RunTest(const FString& Parameters)
{
	using namespace ChunkCompressionTestUtils;

	FChunkDescriptor D = MakeUniform(16, FVoxelData::Solid(1)); // 4096 voxels = 16 KB
	const SIZE_T Before = D.GetMemoryUsage();
	TestTrue(TEXT("collapse"), D.TryCollapseUniform());
	const SIZE_T After = D.GetMemoryUsage();

	TestTrue(TEXT("collapsed footprint is much smaller"), After < Before);
	TestEqual(TEXT("no raw array bytes when Uniform"), (int32)D.VoxelData.GetAllocatedSize(), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
