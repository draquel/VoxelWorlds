// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelChunkCodec.h"
#include "ChunkDescriptor.h"
#include "Tasks/Task.h"
#include "Templates/Function.h"

#if WITH_DEV_AUTOMATION_TESTS

DEFINE_LOG_CATEGORY_STATIC(LogVoxelCodecTest, Log, All);

namespace VoxelChunkCodecTestUtils
{
	/** Terrain-like chunk: air above a gently varying surface, a thin surface band, solid below.
	 *  Mirrors the compressible far-chunk shape (near-constant material/biome/metadata, density gradient). */
	static TArray<FVoxelData> MakeTerrainChunk(int32 CS)
	{
		TArray<FVoxelData> Data;
		Data.SetNumUninitialized(CS * CS * CS);
		const int32 SurfaceZ = CS / 2;
		for (int32 z = 0; z < CS; ++z)
		{
			for (int32 y = 0; y < CS; ++y)
			{
				for (int32 x = 0; x < CS; ++x)
				{
					const int32 i = x + y * CS + z * CS * CS;
					const int32 H = SurfaceZ + FMath::RoundToInt(3.0f * FMath::Sin(x * 0.2f) * FMath::Cos(y * 0.2f));
					if (z > H)         Data[i] = FVoxelData::Air();
					else if (z == H)   Data[i] = FVoxelData(2, 127, 1, 0);
					else               Data[i] = FVoxelData::Solid(3, 1);
				}
			}
		}
		return Data;
	}

	/** Terrain chunk with water/underground metadata flags set near the surface (lossless stress). */
	static TArray<FVoxelData> MakeFlaggedChunk(int32 CS)
	{
		TArray<FVoxelData> Data = MakeTerrainChunk(CS);
		for (int32 i = 0; i < Data.Num(); ++i)
		{
			if (Data[i].IsAir() && (i % 7 == 0)) { Data[i].SetWaterFlag(true); }
			if (Data[i].IsSolid() && (i % 11 == 0)) { Data[i].SetUndergroundFlag(true); }
		}
		return Data;
	}

	/** Incompressible noise: deterministic pseudo-random bytes (worst case → Raw fallback). */
	static TArray<FVoxelData> MakeNoiseChunk(int32 CS)
	{
		TArray<FVoxelData> Data;
		Data.SetNumUninitialized(CS * CS * CS);
		uint32 State = 0x9E3779B9u;
		for (int32 i = 0; i < Data.Num(); ++i)
		{
			State ^= State << 13; State ^= State >> 17; State ^= State << 5; // xorshift
			Data[i] = FVoxelData::Unpack(State);
		}
		return Data;
	}

	static bool RoundTripsEqual(const TArray<FVoxelData>& In, EVoxelChunkCodec Codec, int32 CS, FString& OutInfo)
	{
		TArray<uint8> Buffer;
		const double T0 = FPlatformTime::Seconds();
		const bool bCompressed = FVoxelChunkCodec::Compress(In, Codec, CS, Buffer);
		const double T1 = FPlatformTime::Seconds();
		TArray<FVoxelData> Out;
		const bool bDecompressed = FVoxelChunkCodec::Decompress(Buffer, Out);
		const double T2 = FPlatformTime::Seconds();

		const int32 RawBytes = In.Num() * sizeof(FVoxelData);
		const double Ratio = Buffer.Num() > 0 ? (double)RawBytes / (double)Buffer.Num() : 0.0;
		OutInfo = FString::Printf(TEXT("raw=%dKB comp=%dKB ratio=%.1fx enc=%.2fms dec=%.2fms"),
			RawBytes / 1024, Buffer.Num() / 1024, Ratio, (T1 - T0) * 1000.0, (T2 - T1) * 1000.0);

		return bCompressed && bDecompressed && (Out == In);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelChunkCodecRoundTripTest,
	"VoxelWorlds.Compression.Codec.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelChunkCodecRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace VoxelChunkCodecTestUtils;
	const int32 CS = 32; // 32768 voxels — fast, still exercises the format

	const EVoxelChunkCodec Codecs[] = {
		EVoxelChunkCodec::Raw, EVoxelChunkCodec::LZ4, EVoxelChunkCodec::Oodle,
		EVoxelChunkCodec::LZ4Planar, EVoxelChunkCodec::OodlePlanar };

	struct FCase { const TCHAR* Name; TArray<FVoxelData> Data; };
	TArray<FCase> Cases;
	Cases.Add({ TEXT("terrain"), MakeTerrainChunk(CS) });
	Cases.Add({ TEXT("flagged"), MakeFlaggedChunk(CS) });
	Cases.Add({ TEXT("noise"),   MakeNoiseChunk(CS) });

	for (const FCase& C : Cases)
	{
		for (EVoxelChunkCodec Codec : Codecs)
		{
			FString Info;
			const bool bOk = RoundTripsEqual(C.Data, Codec, CS, Info);
			TestTrue(FString::Printf(TEXT("round-trip %s codec=%d (%s)"), C.Name, (int32)Codec, *Info), bOk);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelChunkCodecBakeOffTest,
	"VoxelWorlds.Compression.Codec.BakeOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelChunkCodecBakeOffTest::RunTest(const FString& Parameters)
{
	using namespace VoxelChunkCodecTestUtils;
	const int32 CS = 64; // production chunk size = 262144 voxels = 1 MB

	const TArray<FVoxelData> Terrain = MakeTerrainChunk(CS);

	struct FEntry { const TCHAR* Name; EVoxelChunkCodec Codec; };
	const FEntry Entries[] = {
		{ TEXT("LZ4       "), EVoxelChunkCodec::LZ4 },
		{ TEXT("Oodle     "), EVoxelChunkCodec::Oodle },
		{ TEXT("LZ4Planar "), EVoxelChunkCodec::LZ4Planar },
		{ TEXT("OodlePlanar"), EVoxelChunkCodec::OodlePlanar },
	};

	UE_LOG(LogVoxelCodecTest, Warning, TEXT("=== Codec bake-off (64^3 terrain-like chunk, 1MB raw) ==="));
	for (const FEntry& E : Entries)
	{
		FString Info;
		const bool bOk = RoundTripsEqual(Terrain, E.Codec, CS, Info);
		UE_LOG(LogVoxelCodecTest, Warning, TEXT("  %s : %s"), E.Name, *Info);
		AddInfo(FString::Printf(TEXT("%s : %s"), E.Name, *Info));
		TestTrue(FString::Printf(TEXT("bake-off round-trip %s"), E.Name), bOk);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelChunkCodecDescriptorTest,
	"VoxelWorlds.Compression.Codec.DescriptorIntegration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelChunkCodecDescriptorTest::RunTest(const FString& Parameters)
{
	using namespace VoxelChunkCodecTestUtils;
	const int32 CS = 32;

	FChunkDescriptor D(FIntVector::ZeroValue, CS);
	D.AllocateVoxelData();
	const TArray<FVoxelData> Original = MakeTerrainChunk(CS);
	D.VoxelData = Original;
	D.Residency = EVoxelDataResidency::Resident;

	// Non-uniform terrain chunk compresses to the Compressed tier (OodlePlanar).
	TestTrue(TEXT("compress non-uniform chunk"), D.TryCompress(EVoxelChunkCodec::OodlePlanar));
	TestEqual(TEXT("residency is Compressed"), (int32)D.Residency, (int32)EVoxelDataResidency::Compressed);
	TestEqual(TEXT("raw array dropped"), D.VoxelData.Num(), 0);
	TestTrue(TEXT("compressed buffer present"), D.CompressedVoxelData.Num() > 0);
	TestTrue(TEXT("compressed smaller than raw"), D.CompressedVoxelData.Num() < Original.Num() * (int32)sizeof(FVoxelData));

	// Access decompresses transparently and losslessly.
	const TArray<FVoxelData>& Restored = D.EnsureResident();
	TestEqual(TEXT("residency Resident after access"), (int32)D.Residency, (int32)EVoxelDataResidency::Resident);
	TestTrue(TEXT("lossless round-trip through the descriptor"), Restored == Original);

	// Buffer retained → free re-compress (no re-encode).
	TestTrue(TEXT("buffer retained after expand"), D.CompressedVoxelData.Num() > 0);
	TestTrue(TEXT("free re-compress"), D.TryCompress(EVoxelChunkCodec::OodlePlanar));
	TestEqual(TEXT("re-compressed to Compressed"), (int32)D.Residency, (int32)EVoxelDataResidency::Compressed);

	// Mutation invalidates the compressed buffer.
	TArray<FVoxelData>& Mut = D.GetVoxelDataMutable();
	Mut[0] = FVoxelData::Solid(9, 4);
	TestEqual(TEXT("compressed buffer dropped on mutation"), D.CompressedVoxelData.Num(), 0);
	TestTrue(TEXT("mutation preserved"), D.GetVoxel(FIntVector(0, 0, 0)) == FVoxelData::Solid(9, 4));

	return true;
}

// ---------------------------------------------------------------------------
// Async far compression (WORLD_MODES_REVIEW_PLAN.md G6). The chunk manager encodes a copy of a far
// chunk's array on a background worker (FChunkDescriptor::ComputeCompactForm) and installs the result
// on the game thread (InstallCompactForm) only while the descriptor still carries the job's ticket.
// These tests pin the two halves: the split path gives exactly what TryCompress gives, and every
// write or residency change voids an in-flight ticket.
// ---------------------------------------------------------------------------

namespace VoxelChunkCodecTestUtils
{
	static void MakeResident(FChunkDescriptor& D, const TArray<FVoxelData>& Data)
	{
		TArray<FVoxelData> Copy = Data;
		D.SetResidentVoxelData(MoveTemp(Copy));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelChunkAsyncCompactMatchesSyncTest,
	"VoxelWorlds.Compression.Async.MatchesSyncCompress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelChunkAsyncCompactMatchesSyncTest::RunTest(const FString& Parameters)
{
	using namespace VoxelChunkCodecTestUtils;
	const int32 CS = 32;

	TArray<FVoxelData> Air;
	Air.Init(FVoxelData::Air(), CS * CS * CS);

	struct FCase { const TCHAR* Name; TArray<FVoxelData> Data; EVoxelDataResidency Expected; };
	TArray<FCase> Cases;
	Cases.Add({ TEXT("terrain"), MakeTerrainChunk(CS), EVoxelDataResidency::Compressed });
	Cases.Add({ TEXT("flagged"), MakeFlaggedChunk(CS), EVoxelDataResidency::Compressed });
	Cases.Add({ TEXT("noise"),   MakeNoiseChunk(CS),   EVoxelDataResidency::Resident });
	Cases.Add({ TEXT("air"),     Air,                  EVoxelDataResidency::Uniform });

	for (const FCase& C : Cases)
	{
		// Sync: the encode inside TryCompress.
		FChunkDescriptor Sync(FIntVector::ZeroValue, CS);
		MakeResident(Sync, C.Data);
		Sync.TryCompress(EVoxelChunkCodec::Oodle);

		// Async: encode a copy on a worker, install on this thread.
		FChunkDescriptor Async(FIntVector::ZeroValue, CS);
		MakeResident(Async, C.Data);
		const TArray<FVoxelData> Copy = Async.VoxelData;
		FVoxelChunkCompactForm Form;
		UE::Tasks::Launch(UE_SOURCE_LOCATION, [&Form, &Copy, CS]()
		{
			Form = FChunkDescriptor::ComputeCompactForm(Copy, EVoxelChunkCodec::Oodle, CS);
		}).Wait();
		Async.InstallCompactForm(MoveTemp(Form));

		TestEqual(FString::Printf(TEXT("%s: residency as expected"), C.Name), (int32)Async.Residency, (int32)C.Expected);
		TestEqual(FString::Printf(TEXT("%s: residency matches sync"), C.Name), (int32)Async.Residency, (int32)Sync.Residency);
		TestTrue(FString::Printf(TEXT("%s: buffer matches sync"), C.Name), Async.CompressedVoxelData == Sync.CompressedVoxelData);
		TestTrue(FString::Printf(TEXT("%s: uniform flag matches sync"), C.Name), Async.bUniformValueValid == Sync.bUniformValueValid);
		TestTrue(FString::Printf(TEXT("%s: evaluated"), C.Name), Async.bCompressionEvaluated);
		TestTrue(FString::Printf(TEXT("%s: lossless"), C.Name), Async.GetVoxelDataForRead() == C.Data);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelChunkAsyncCompactTicketTest,
	"VoxelWorlds.Compression.Async.TicketVoidedByWrites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelChunkAsyncCompactTicketTest::RunTest(const FString& Parameters)
{
	using namespace VoxelChunkCodecTestUtils;
	const int32 CS = 32;
	const TArray<FVoxelData> Terrain = MakeTerrainChunk(CS);
	const uint32 Ticket = 7;

	struct FCase { const TCHAR* Name; TFunction<void(FChunkDescriptor&)> Op; bool bVoids; };
	const FCase Cases[] = {
		// Writes and residency changes void the ticket...
		{ TEXT("SetVoxel"),            [](FChunkDescriptor& D) { D.SetVoxel(FIntVector(1, 2, 3), FVoxelData::Solid(9, 1)); }, true },
		{ TEXT("SetVoxelByIndex"),     [](FChunkDescriptor& D) { D.SetVoxelByIndex(5, FVoxelData::Solid(9, 1)); }, true },
		{ TEXT("GetVoxelDataMutable"), [](FChunkDescriptor& D) { D.GetVoxelDataMutable(); }, true },
		{ TEXT("SetResidentVoxelData"), [&Terrain](FChunkDescriptor& D) { TArray<FVoxelData> Copy = Terrain; D.SetResidentVoxelData(MoveTemp(Copy)); }, true },
		{ TEXT("AllocateVoxelData"),   [](FChunkDescriptor& D) { D.AllocateVoxelData(); }, true },
		{ TEXT("ClearVoxelData"),      [](FChunkDescriptor& D) { D.ClearVoxelData(); }, true },
		{ TEXT("TryCompress"),         [](FChunkDescriptor& D) { D.TryCompress(EVoxelChunkCodec::Oodle); }, true },
		{ TEXT("TryCollapseUniform"),  [](FChunkDescriptor& D) { D.VoxelData.Init(FVoxelData::Air(), D.GetTotalVoxels()); D.TryCollapseUniform(); }, true },
		// ...reads and edit-layer version bumps do not (the procedural array is unchanged).
		{ TEXT("GetVoxelDataForRead"), [](FChunkDescriptor& D) { D.GetVoxelDataForRead(); }, false },
		{ TEXT("GetVoxelResident"),    [](FChunkDescriptor& D) { D.GetVoxelResident(FIntVector(1, 2, 3)); }, false },
		{ TEXT("BumpContentVersion"),  [](FChunkDescriptor& D) { D.BumpContentVersion(); }, false },
	};

	for (const FCase& C : Cases)
	{
		FChunkDescriptor D(FIntVector::ZeroValue, CS);
		MakeResident(D, Terrain);
		D.AsyncCompactTicket = Ticket;
		C.Op(D);
		if (C.bVoids)
		{
			TestTrue(FString::Printf(TEXT("%s voids the ticket"), C.Name), D.AsyncCompactTicket == 0);
		}
		else
		{
			TestTrue(FString::Printf(TEXT("%s keeps the ticket"), C.Name), D.AsyncCompactTicket == Ticket);
		}
	}

	// A write while the worker runs: the result must not be installed (the manager compares tickets).
	FChunkDescriptor D(FIntVector::ZeroValue, CS);
	MakeResident(D, Terrain);
	D.AsyncCompactTicket = Ticket;
	const TArray<FVoxelData> Copy = D.VoxelData;
	UE::Tasks::FTask Encode = UE::Tasks::Launch(UE_SOURCE_LOCATION, [&Copy, CS]()
	{
		(void)FChunkDescriptor::ComputeCompactForm(Copy, EVoxelChunkCodec::Oodle, CS);
	});
	D.SetVoxel(FIntVector(4, 4, 4), FVoxelData::Solid(9, 1));
	Encode.Wait();
	TestTrue(TEXT("a write during the encode voids its result"), D.AsyncCompactTicket != Ticket);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelChunkAsyncCompactCacheOnlyTest,
	"VoxelWorlds.Compression.Async.CacheOnlyInstall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelChunkAsyncCompactCacheOnlyTest::RunTest(const FString& Parameters)
{
	using namespace VoxelChunkCodecTestUtils;
	const int32 CS = 32;
	const TArray<FVoxelData> Terrain = MakeTerrainChunk(CS);

	// A chunk that stopped qualifying while its encode ran keeps the raw array and caches the form.
	FChunkDescriptor D(FIntVector::ZeroValue, CS);
	MakeResident(D, Terrain);
	D.AsyncCompactTicket = 3;
	const bool bDropped = D.InstallCompactForm(FChunkDescriptor::ComputeCompactForm(Terrain, EVoxelChunkCodec::Oodle, CS), /*bDropResident*/ false);
	TestFalse(TEXT("cache-only install keeps the raw array"), bDropped);
	TestEqual(TEXT("still Resident"), (int32)D.Residency, (int32)EVoxelDataResidency::Resident);
	TestTrue(TEXT("raw array intact"), D.VoxelData == Terrain);
	TestTrue(TEXT("buffer cached"), D.CompressedVoxelData.Num() > 0);
	TestTrue(TEXT("ticket consumed"), D.AsyncCompactTicket == 0);

	// Re-qualifying later re-applies the cached form for free (no encode), losslessly.
	const TArray<uint8> Cached = D.CompressedVoxelData;
	TestTrue(TEXT("free re-apply"), D.TryCompress(EVoxelChunkCodec::Oodle));
	TestEqual(TEXT("now Compressed"), (int32)D.Residency, (int32)EVoxelDataResidency::Compressed);
	TestTrue(TEXT("same buffer"), D.CompressedVoxelData == Cached);
	TestTrue(TEXT("lossless"), D.GetVoxelDataForRead() == Terrain);

	// A write drops the cached form.
	FChunkDescriptor W(FIntVector::ZeroValue, CS);
	MakeResident(W, Terrain);
	W.InstallCompactForm(FChunkDescriptor::ComputeCompactForm(Terrain, EVoxelChunkCodec::Oodle, CS), false);
	W.SetVoxel(FIntVector(0, 0, 0), FVoxelData::Solid(9, 1));
	TestEqual(TEXT("write drops the cached buffer"), W.CompressedVoxelData.Num(), 0);
	TestFalse(TEXT("write clears evaluated"), W.bCompressionEvaluated);

	// Incompressible: stays resident, marked evaluated so the sweep skips it.
	FChunkDescriptor N(FIntVector::ZeroValue, CS);
	const TArray<FVoxelData> Noise = MakeNoiseChunk(CS);
	MakeResident(N, Noise);
	TestFalse(TEXT("noise does not compact"), N.InstallCompactForm(FChunkDescriptor::ComputeCompactForm(Noise, EVoxelChunkCodec::Oodle, CS)));
	TestEqual(TEXT("noise stays Resident"), (int32)N.Residency, (int32)EVoxelDataResidency::Resident);
	TestTrue(TEXT("noise marked evaluated"), N.bCompressionEvaluated);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelChunkCompactSourceTest,
	"VoxelWorlds.Compression.Codec.CompactSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * World modes review G65: a compact form copied off a descriptor (FVoxelCompactSource) expands to the chunk's exact array
 * on any thread while the descriptor stays compact; a resident chunk has no compact source; and the descriptor's own
 * in-place expansion is what the residency statistics count.
 */
bool FVoxelChunkCompactSourceTest::RunTest(const FString& Parameters)
{
	using namespace VoxelChunkCodecTestUtils;
	const int32 CS = 32;

	// Compressed (Oodle, the default codec): the copy expands to the original, the descriptor stays compressed.
	FChunkDescriptor D(FIntVector(3, -2, 5), CS);
	D.AllocateVoxelData();
	const TArray<FVoxelData> Original = MakeTerrainChunk(CS);
	D.VoxelData = Original;
	D.Residency = EVoxelDataResidency::Resident;

	FVoxelCompactSource Resident;
	TestFalse(TEXT("a resident chunk has no compact source"), D.CopyCompactSource(Resident));
	TestFalse(TEXT("the empty source is not valid"), Resident.IsValid());

	TestTrue(TEXT("compress the terrain chunk"), D.TryCompress(EVoxelChunkCodec::Oodle));
	TestTrue(TEXT("compact once compressed"), D.IsCompact());
	FVoxelCompactSource Source;
	TestTrue(TEXT("copy the compressed form"), D.CopyCompactSource(Source));
	TestEqual(TEXT("the descriptor stays compressed"), (int32)D.Residency, (int32)EVoxelDataResidency::Compressed);
	TestEqual(TEXT("no raw array after the copy"), D.VoxelData.Num(), 0);

	int32 Count = 0;
	float Ms = 0.0f;
	VoxelResidencyStats::ConsumeGameThread(Count, Ms);
	TArray<FVoxelData> Expanded;
	TestTrue(TEXT("the copy expands"), Source.Expand(Expanded));
	TestTrue(TEXT("to the exact array"), Expanded == Original);
	VoxelResidencyStats::ConsumeGameThread(Count, Ms);
	TestEqual(TEXT("expanding a copy is not an in-place expansion"), Count, 0);

	// Expanding it on a worker gives the same array.
	TArray<FVoxelData> WorkerExpanded;
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [&Source, &WorkerExpanded]() { Source.Expand(WorkerExpanded); }).Wait();
	TestTrue(TEXT("a worker expands the same array"), WorkerExpanded == Original);

	// The descriptor's own expansion is counted (game thread here).
	D.EnsureResident();
	VoxelResidencyStats::ConsumeGameThread(Count, Ms);
	TestEqual(TEXT("an in-place expansion on the game thread is counted"), Count, 1);
	TestTrue(TEXT("and the chunk is resident again"), D.IsVoxelDataResident());

	// Uniform: the value is copied, the expansion fills the chunk.
	FChunkDescriptor U(FIntVector::ZeroValue, CS);
	U.AllocateVoxelData();
	const FVoxelData Rock = FVoxelData::Solid(4, 1);
	for (FVoxelData& V : U.VoxelData)
	{
		V = Rock;
	}
	TestTrue(TEXT("collapse the uniform chunk"), U.TryCollapseUniform());
	FVoxelCompactSource UniformSource;
	TestTrue(TEXT("copy the uniform form"), U.CopyCompactSource(UniformSource));
	TestEqual(TEXT("the descriptor stays uniform"), (int32)U.Residency, (int32)EVoxelDataResidency::Uniform);
	TArray<FVoxelData> Filled;
	TestTrue(TEXT("the uniform copy expands"), UniformSource.Expand(Filled));
	bool bAllRock = Filled.Num() == CS * CS * CS;
	for (const FVoxelData& V : Filled)
	{
		bAllRock &= (V == Rock);
	}
	TestTrue(TEXT("to the chunk's value everywhere"), bAllRock);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
