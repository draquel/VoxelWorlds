// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelChunkCodec.h"
#include "ChunkDescriptor.h"

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

#endif // WITH_DEV_AUTOMATION_TESTS
