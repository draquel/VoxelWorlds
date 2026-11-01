// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelCPUMarchingCubesMesher.h"
#include "VoxelMeshingTypes.h"
#include "ChunkRenderData.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The CPU meshers' digest (code review CR2, row 9): one CRC per meshing path on fixed scenes, logged as
// "[MeshDigest] <case> crc=<hex> v=<vertices> i=<indices>" lines. A refactor of the meshers that changes no
// behaviour leaves every line as it was; the lines of a build are compared against the record in
// Documentation/CodeReview/CR2/. Every path: both smooth meshers' chunk meshes at LOD 0, 1 and 2 in the
// Interior domain and in the Full domain with every neighbour plane, strip and corner (deep, as the extractor
// fills them) and with most missing (the missing-neighbour rule), Marching Cubes' transition ribbon and boundary
// geomorph along +Z and along a tilted local up, and both meshers' face, edge and corner seams at uniform and
// mixed LODs; each under the default cvars and under each of the meshing cvars turned off. Pure CPU; headless.
// ---------------------------------------------------------------------------

namespace VoxelMeshingDigest
{
	constexpr int32 CS = 32;
	constexpr float VoxelSize = 100.0f;

	/** Noise terrain as the generators write it (the mixed seam tests' FNoiseTerrain), with two materials and two biomes. */
	struct FTerrain
	{
		float Base = 16.0f;
		FVector Seed = FVector::ZeroVector;

		float SignedDistance(int32 X, int32 Y, int32 Z) const
		{
			const FVector P = FVector(X, Y, Z) + Seed;
			const float Hills = 6.0f * FMath::PerlinNoise2D(FVector2D(P.X, P.Y) / 23.0f)
				+ 2.5f * FMath::PerlinNoise2D(FVector2D(P.X, P.Y) / 9.0f + FVector2D(17.0, 3.0));
			const float Bumps = 1.6f * FMath::PerlinNoise3D(P / 5.0f + FVector(5.0, 11.0, 2.0));
			return Base + Hills + Bumps - static_cast<float>(Z);
		}

		FVoxelData Voxel(int32 X, int32 Y, int32 Z) const
		{
			const float SD = SignedDistance(X, Y, Z);
			const float Clamped = FMath::Clamp(SD, -1.0f, 1.0f);
			const uint8 Density = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(127.5f + 127.5f * Clamped), 0, 255));
			const uint8 Material = (SD > 2.5f) ? 2 : 1;
			const uint8 Biome = (((X + Y) / 16) & 1) ? 1 : 0;
			FVoxelData Data = (Density > 0) ? FVoxelData::Solid(Material, Biome) : FVoxelData::Air();
			Data.Density = Density;
			return Data;
		}
	};

	static TSharedPtr<const TArray<FVoxelData>> ChunkVoxels(const FTerrain& T, const FIntVector& ChunkCoord)
	{
		TSharedPtr<TArray<FVoxelData>> Data = MakeShared<TArray<FVoxelData>>();
		Data->SetNumUninitialized(CS * CS * CS);
		const FIntVector O = ChunkCoord * CS;
		for (int32 Z = 0; Z < CS; ++Z)
		{
			for (int32 Y = 0; Y < CS; ++Y)
			{
				for (int32 X = 0; X < CS; ++X)
				{
					(*Data)[X + Y * CS + Z * CS * CS] = T.Voxel(X + O.X, Y + O.Y, Z + O.Z);
				}
			}
		}
		return Data;
	}

	static FVoxelMeshingConfig MakeConfig()
	{
		FVoxelMeshingConfig Config;
		Config.IsoLevel = 0.5f;
		Config.bGenerateUVs = true;
		Config.bCalculateAO = false;
		Config.QEFSVDThreshold = 0.1f;
		return Config;
	}

	/** Which neighbours a Full-domain request carries. */
	enum class ENeighbours : uint8 { None, All, PositiveFaces };

	/**
	 * A chunk request at ChunkCoord over the terrain, with the neighbour data the streaming extractor would fill:
	 * face planes NeighborPlaneDepth deep (stride + 1 at LOD > 0, 1 at LOD 0), edge strips with their deep grids and
	 * corners with their deep boxes, each voxel the terrain's at its global position.
	 */
	static FVoxelMeshingRequest MakeRequest(const FTerrain& T, const FIntVector& ChunkCoord, int32 LOD,
		EVoxelMeshCellDomain Domain, ENeighbours Neighbours)
	{
		FVoxelMeshingRequest R;
		R.ChunkCoord = ChunkCoord;
		R.LODLevel = LOD;
		R.ChunkSize = CS;
		R.VoxelSize = VoxelSize;
		R.MeshCellDomain = Domain;
		R.VoxelData = *ChunkVoxels(T, ChunkCoord);
		if (Neighbours == ENeighbours::None)
		{
			return R;
		}
		const FIntVector O = ChunkCoord * CS;
		const int32 Stride = 1 << LOD;
		const int32 Depth = (Stride <= 1) ? 1 : Stride + 1;
		R.NeighborPlaneDepth = Depth;
		auto At = [&](int32 X, int32 Y, int32 Z) { return T.Voxel(X + O.X, Y + O.Y, Z + O.Z); };
		// Depth d past a face: X = CS + d on the positive side, -1 - d on the negative side.
		auto Past = [](bool bPos, int32 D) { return bPos ? CS + D : -1 - D; };

		auto FillPlane = [&](TArray<FVoxelData>& Plane, TArray<FVoxelData>& Deep, int32 Axis, bool bPos)
		{
			Plane.SetNumUninitialized(CS * CS);
			Deep.SetNumUninitialized((Depth - 1) * CS * CS);
			for (int32 D = 0; D < Depth; ++D)
			{
				for (int32 B = 0; B < CS; ++B)
				{
					for (int32 A = 0; A < CS; ++A)
					{
						int32 V[3];
						V[Axis] = Past(bPos, D);
						V[(Axis == 0) ? 1 : 0] = A;
						V[(Axis == 2) ? 1 : 2] = B;
						const FVoxelData Vox = At(V[0], V[1], V[2]);
						if (D == 0) { Plane[A + B * CS] = Vox; }
						else { Deep[(D - 1) * CS * CS + A + B * CS] = Vox; }
					}
				}
			}
		};
		FillPlane(R.NeighborXPos, R.NeighborXPosDeep, 0, true);
		FillPlane(R.NeighborYPos, R.NeighborYPosDeep, 1, true);
		FillPlane(R.NeighborZPos, R.NeighborZPosDeep, 2, true);
		if (Neighbours == ENeighbours::PositiveFaces)
		{
			return R;
		}
		FillPlane(R.NeighborXNeg, R.NeighborXNegDeep, 0, false);
		FillPlane(R.NeighborYNeg, R.NeighborYNegDeep, 1, false);
		FillPlane(R.NeighborZNeg, R.NeighborZNegDeep, 2, false);

		// Edges: AxisA < AxisB pinned, Free the third; base strip at depth (0, 0), deep grid [(a * D + b) * CS + f].
		auto FillEdge = [&](TArray<FVoxelData>& Base, TArray<FVoxelData>& Deep, int32 AxisA, bool bPosA, int32 AxisB, bool bPosB, uint32 Flag)
		{
			const int32 Free = 3 - AxisA - AxisB;
			Base.SetNumUninitialized(CS);
			if (Depth > 1) { Deep.SetNumUninitialized(Depth * Depth * CS); }
			for (int32 A = 0; A < Depth; ++A)
			{
				for (int32 B = 0; B < Depth; ++B)
				{
					for (int32 F = 0; F < CS; ++F)
					{
						int32 V[3];
						V[AxisA] = Past(bPosA, A);
						V[AxisB] = Past(bPosB, B);
						V[Free] = F;
						const FVoxelData Vox = At(V[0], V[1], V[2]);
						if (A == 0 && B == 0) { Base[F] = Vox; }
						if (Depth > 1) { Deep[(A * Depth + B) * CS + F] = Vox; }
					}
				}
			}
			R.EdgeCornerFlags |= Flag;
		};
		using Q = FVoxelMeshingRequest;
		FillEdge(R.EdgeXPosYPos, R.EdgeXPosYPosDeep, 0, true, 1, true, Q::EDGE_XPOS_YPOS);
		FillEdge(R.EdgeXPosYNeg, R.EdgeXPosYNegDeep, 0, true, 1, false, Q::EDGE_XPOS_YNEG);
		FillEdge(R.EdgeXNegYPos, R.EdgeXNegYPosDeep, 0, false, 1, true, Q::EDGE_XNEG_YPOS);
		FillEdge(R.EdgeXNegYNeg, R.EdgeXNegYNegDeep, 0, false, 1, false, Q::EDGE_XNEG_YNEG);
		FillEdge(R.EdgeXPosZPos, R.EdgeXPosZPosDeep, 0, true, 2, true, Q::EDGE_XPOS_ZPOS);
		FillEdge(R.EdgeXPosZNeg, R.EdgeXPosZNegDeep, 0, true, 2, false, Q::EDGE_XPOS_ZNEG);
		FillEdge(R.EdgeXNegZPos, R.EdgeXNegZPosDeep, 0, false, 2, true, Q::EDGE_XNEG_ZPOS);
		FillEdge(R.EdgeXNegZNeg, R.EdgeXNegZNegDeep, 0, false, 2, false, Q::EDGE_XNEG_ZNEG);
		FillEdge(R.EdgeYPosZPos, R.EdgeYPosZPosDeep, 1, true, 2, true, Q::EDGE_YPOS_ZPOS);
		FillEdge(R.EdgeYPosZNeg, R.EdgeYPosZNegDeep, 1, true, 2, false, Q::EDGE_YPOS_ZNEG);
		FillEdge(R.EdgeYNegZPos, R.EdgeYNegZPosDeep, 1, false, 2, true, Q::EDGE_YNEG_ZPOS);
		FillEdge(R.EdgeYNegZNeg, R.EdgeYNegZNegDeep, 1, false, 2, false, Q::EDGE_YNEG_ZNEG);

		// Corners: the voxel at depth (0, 0, 0) and the deep box [(a * D + b) * D + c].
		auto FillCorner = [&](FVoxelData& Base, TArray<FVoxelData>& Deep, bool bPosX, bool bPosY, bool bPosZ, uint32 Flag)
		{
			if (Depth > 1) { Deep.SetNumUninitialized(Depth * Depth * Depth); }
			for (int32 A = 0; A < Depth; ++A)
			{
				for (int32 B = 0; B < Depth; ++B)
				{
					for (int32 C = 0; C < Depth; ++C)
					{
						const FVoxelData Vox = At(Past(bPosX, A), Past(bPosY, B), Past(bPosZ, C));
						if (A == 0 && B == 0 && C == 0) { Base = Vox; }
						if (Depth > 1) { Deep[(A * Depth + B) * Depth + C] = Vox; }
					}
				}
			}
			R.EdgeCornerFlags |= Flag;
		};
		FillCorner(R.CornerXPosYPosZPos, R.CornerXPosYPosZPosDeep, true, true, true, Q::CORNER_XPOS_YPOS_ZPOS);
		FillCorner(R.CornerXPosYPosZNeg, R.CornerXPosYPosZNegDeep, true, true, false, Q::CORNER_XPOS_YPOS_ZNEG);
		FillCorner(R.CornerXPosYNegZPos, R.CornerXPosYNegZPosDeep, true, false, true, Q::CORNER_XPOS_YNEG_ZPOS);
		FillCorner(R.CornerXPosYNegZNeg, R.CornerXPosYNegZNegDeep, true, false, false, Q::CORNER_XPOS_YNEG_ZNEG);
		FillCorner(R.CornerXNegYPosZPos, R.CornerXNegYPosZPosDeep, false, true, true, Q::CORNER_XNEG_YPOS_ZPOS);
		FillCorner(R.CornerXNegYPosZNeg, R.CornerXNegYPosZNegDeep, false, true, false, Q::CORNER_XNEG_YPOS_ZNEG);
		FillCorner(R.CornerXNegYNegZPos, R.CornerXNegYNegZPosDeep, false, false, true, Q::CORNER_XNEG_YNEG_ZPOS);
		FillCorner(R.CornerXNegYNegZNeg, R.CornerXNegYNegZNegDeep, false, false, false, Q::CORNER_XNEG_YNEG_ZNEG);
		return R;
	}

	static uint32 MeshCrc(const FChunkMeshData& M)
	{
		uint32 Crc = 0;
		auto Add = [&Crc](const auto& Arr) { Crc = FCrc::MemCrc32(Arr.GetData(), Arr.Num() * Arr.GetTypeSize(), Crc); };
		Add(M.Positions);
		Add(M.Normals);
		Add(M.UVs);
		Add(M.UV1s);
		Add(M.Colors);
		Add(M.Indices);
		return Crc;
	}

	static bool SameMesh(const FChunkMeshData& A, const FChunkMeshData& B)
	{
		auto Same = [](const auto& X, const auto& Y)
		{
			return X.Num() == Y.Num() && (X.Num() == 0 || FMemory::Memcmp(X.GetData(), Y.GetData(), X.Num() * X.GetTypeSize()) == 0);
		};
		return Same(A.Positions, B.Positions) && Same(A.Normals, B.Normals) && Same(A.UVs, B.UVs)
			&& Same(A.UV1s, B.UV1s) && Same(A.Colors, B.Colors) && Same(A.Indices, B.Indices);
	}

	/** A cvar held at a value for a scope. */
	struct FScopedCVar
	{
		IConsoleVariable* Var = nullptr;
		int32 Saved = 0;
		FScopedCVar(const TCHAR* Name, int32 Value)
		{
			Var = IConsoleManager::Get().FindConsoleVariable(Name);
			if (Var)
			{
				Saved = Var->GetInt();
				Var->Set(Value, ECVF_SetByCode);
			}
		}
		~FScopedCVar()
		{
			if (Var)
			{
				Var->Set(Saved, ECVF_SetByCode);
			}
		}
	};

	/** The digest's recorder: one line per case, and the uniform-against-mixed seam comparison. */
	struct FDigest
	{
		FAutomationTestBase& Test;
		FString Prefix;
		uint32 Total = 0;
		int32 Cases = 0;
		FVoxelCPUDualContourMesher DC;
		FVoxelCPUMarchingCubesMesher MC;

		explicit FDigest(FAutomationTestBase& InTest) : Test(InTest)
		{
			DC.Initialize();
			DC.SetConfig(MakeConfig());
			MC.Initialize();
			MC.SetConfig(MakeConfig());
		}
		~FDigest()
		{
			DC.Shutdown();
			MC.Shutdown();
		}

		void Record(const FString& Case, bool bOk, const FChunkMeshData& M)
		{
			Test.TestTrue(*FString::Printf(TEXT("%s meshed"), *Case), bOk);
			const uint32 Crc = MeshCrc(M);
			Total = FCrc::MemCrc32(&Crc, sizeof(Crc), Total);
			++Cases;
			UE_LOG(LogTemp, Display, TEXT("[MeshDigest] %s%s crc=%08X v=%d i=%d"), *Prefix, *Case, Crc, M.Positions.Num(), M.Indices.Num());
		}

		void Chunk(const TCHAR* Mesher, const FString& Case, const FVoxelMeshingRequest& R)
		{
			FChunkMeshData M;
			const bool bOk = (Mesher[0] == TEXT('D')) ? DC.GenerateMeshCPU(R, M) : MC.GenerateMeshCPU(R, M);
			Record(FString::Printf(TEXT("%s.%s"), Mesher, *Case), bOk, M);
		}

		void FaceSeam(const TCHAR* Mesher, const FString& Case, const FVoxelFaceSeamRequest& S)
		{
			FChunkMeshData M;
			const bool bOk = (Mesher[0] == TEXT('D')) ? DC.GenerateFaceSeamMeshCPU(S, M) : MC.GenerateFaceSeamMeshCPU(S, M);
			Record(FString::Printf(TEXT("%s.%s"), Mesher, *Case), bOk, M);
		}

		void EdgeSeam(const TCHAR* Mesher, const FString& Case, const FVoxelEdgeSeamRequest& S)
		{
			FChunkMeshData M;
			const bool bOk = (Mesher[0] == TEXT('D')) ? DC.GenerateEdgeSeamMeshCPU(S, M) : MC.GenerateEdgeSeamMeshCPU(S, M);
			Record(FString::Printf(TEXT("%s.%s"), Mesher, *Case), bOk, M);
		}

		void CornerSeam(const TCHAR* Mesher, const FString& Case, const FVoxelCornerSeamRequest& S)
		{
			FChunkMeshData M;
			const bool bOk = (Mesher[0] == TEXT('D')) ? DC.GenerateCornerSeamMeshCPU(S, M) : MC.GenerateCornerSeamMeshCPU(S, M);
			Record(FString::Printf(TEXT("%s.%s"), Mesher, *Case), bOk, M);
		}
	};

	static FVoxelFaceSeamRequest MakeFaceSeam(const FTerrain& T, int32 Axis, int32 LODA, int32 LODB)
	{
		FVoxelFaceSeamRequest S;
		S.Axis = static_cast<uint8>(Axis);
		S.LODLevel = LODA;
		S.LODLevelB = (LODB == LODA) ? -1 : LODB;
		S.ChunkSize = CS;
		S.VoxelSize = VoxelSize;
		FIntVector B = FIntVector::ZeroValue;
		B[Axis] = 1;
		S.VoxelDataA = ChunkVoxels(T, FIntVector::ZeroValue);
		S.VoxelDataB = ChunkVoxels(T, B);
		return S;
	}

	static FVoxelEdgeSeamRequest MakeEdgeSeam(const FTerrain& T, int32 EdgeAxis, const int32 LODs[4])
	{
		FVoxelEdgeSeamRequest S;
		S.EdgeAxis = static_cast<uint8>(EdgeAxis);
		S.LODLevel = LODs[0];
		bool bUniform = true;
		for (int32 q = 1; q < 4; ++q) { bUniform &= (LODs[q] == LODs[0]); }
		for (int32 q = 0; q < 4; ++q) { S.LODLevels[q] = bUniform ? -1 : LODs[q]; }
		S.ChunkSize = CS;
		S.VoxelSize = VoxelSize;
		const int32 PerpA = (EdgeAxis == 0) ? 1 : 0;
		const int32 PerpB = (EdgeAxis == 2) ? 1 : 2;
		for (int32 q = 0; q < 4; ++q)
		{
			FIntVector C = FIntVector::ZeroValue;
			C[PerpA] = q & 1;
			C[PerpB] = (q >> 1) & 1;
			S.VoxelData[q] = ChunkVoxels(T, C);
		}
		return S;
	}

	static FVoxelCornerSeamRequest MakeCornerSeam(const FTerrain& T, const int32 LODs[8])
	{
		FVoxelCornerSeamRequest S;
		S.LODLevel = LODs[0];
		bool bUniform = true;
		for (int32 o = 1; o < 8; ++o) { bUniform &= (LODs[o] == LODs[0]); }
		for (int32 o = 0; o < 8; ++o) { S.LODLevels[o] = bUniform ? -1 : LODs[o]; }
		S.ChunkSize = CS;
		S.VoxelSize = VoxelSize;
		for (int32 o = 0; o < 8; ++o)
		{
			S.VoxelData[o] = ChunkVoxels(T, FIntVector(o & 1, (o >> 1) & 1, (o >> 2) & 1));
		}
		return S;
	}

	/** Every case under the cvars as set. */
	static void RunAllCases(FDigest& D)
	{
		const FTerrain Terrains[2] = { { 16.0f, FVector::ZeroVector }, { 14.0f, FVector(37.0, 91.0, 5.0) } };
		for (int32 TI = 0; TI < 2; ++TI)
		{
			const FTerrain& T = Terrains[TI];
			for (const TCHAR* Mesher : { TEXT("DC"), TEXT("MC") })
			{
				for (int32 LOD = 0; LOD <= 2; ++LOD)
				{
					D.Chunk(Mesher, FString::Printf(TEXT("t%d.interior.lod%d"), TI, LOD),
						MakeRequest(T, FIntVector::ZeroValue, LOD, EVoxelMeshCellDomain::Interior, ENeighbours::None));
					D.Chunk(Mesher, FString::Printf(TEXT("t%d.full.lod%d"), TI, LOD),
						MakeRequest(T, FIntVector::ZeroValue, LOD, EVoxelMeshCellDomain::Full, ENeighbours::All));
					D.Chunk(Mesher, FString::Printf(TEXT("t%d.full.posfaces.lod%d"), TI, LOD),
						MakeRequest(T, FIntVector::ZeroValue, LOD, EVoxelMeshCellDomain::Full, ENeighbours::PositiveFaces));
					D.Chunk(Mesher, FString::Printf(TEXT("t%d.full.nonbr.lod%d"), TI, LOD),
						MakeRequest(T, FIntVector::ZeroValue, LOD, EVoxelMeshCellDomain::Full, ENeighbours::None));
				}
			}
			// Marching Cubes' transition ribbon and boundary geomorph: every face against a coarser neighbour, along +Z
			// and along a tilted local up.
			for (int32 LOD = 0; LOD <= 1; ++LOD)
			{
				for (int32 Tilt = 0; Tilt <= 1; ++Tilt)
				{
					FVoxelMeshingRequest R = MakeRequest(T, FIntVector::ZeroValue, LOD, EVoxelMeshCellDomain::Full, ENeighbours::All);
					R.TransitionFaces = 0x3F;
					for (int32 F = 0; F < 6; ++F) { R.NeighborLODLevels[F] = LOD + 1; }
					if (Tilt) { R.LocalUp = FVector(0.2, 0.3, 0.93).GetSafeNormal(); }
					D.Chunk(TEXT("MC"), FString::Printf(TEXT("t%d.transition%s.lod%d"), TI, Tilt ? TEXT(".tilted") : TEXT(""), LOD), R);
				}
			}
			// Seams, uniform and mixed.
			for (const TCHAR* Mesher : { TEXT("DC"), TEXT("MC") })
			{
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					for (int32 LOD = 0; LOD <= 2; ++LOD)
					{
						D.FaceSeam(Mesher, FString::Printf(TEXT("t%d.face%d.lod%d"), TI, Axis, LOD), MakeFaceSeam(T, Axis, LOD, LOD));
					}
					D.FaceSeam(Mesher, FString::Printf(TEXT("t%d.face%d.lod0-1"), TI, Axis), MakeFaceSeam(T, Axis, 0, 1));
					D.FaceSeam(Mesher, FString::Printf(TEXT("t%d.face%d.lod1-0"), TI, Axis), MakeFaceSeam(T, Axis, 1, 0));
					D.FaceSeam(Mesher, FString::Printf(TEXT("t%d.face%d.lod0-2"), TI, Axis), MakeFaceSeam(T, Axis, 0, 2));
				}
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					for (int32 LOD = 0; LOD <= 2; ++LOD)
					{
						const int32 U[4] = { LOD, LOD, LOD, LOD };
						D.EdgeSeam(Mesher, FString::Printf(TEXT("t%d.edge%d.lod%d"), TI, Axis, LOD), MakeEdgeSeam(T, Axis, U));
					}
					const int32 M1[4] = { 0, 1, 0, 1 };
					const int32 M2[4] = { 1, 0, 2, 0 };
					D.EdgeSeam(Mesher, FString::Printf(TEXT("t%d.edge%d.mixed0101"), TI, Axis), MakeEdgeSeam(T, Axis, M1));
					D.EdgeSeam(Mesher, FString::Printf(TEXT("t%d.edge%d.mixed1020"), TI, Axis), MakeEdgeSeam(T, Axis, M2));
				}
				for (int32 LOD = 0; LOD <= 2; ++LOD)
				{
					const int32 U[8] = { LOD, LOD, LOD, LOD, LOD, LOD, LOD, LOD };
					D.CornerSeam(Mesher, FString::Printf(TEXT("t%d.corner.lod%d"), TI, LOD), MakeCornerSeam(T, U));
				}
				const int32 M1[8] = { 0, 1, 0, 1, 1, 0, 1, 0 };
				const int32 M2[8] = { 2, 0, 1, 0, 0, 1, 0, 2 };
				D.CornerSeam(Mesher, FString::Printf(TEXT("t%d.corner.mixedA"), TI), MakeCornerSeam(T, M1));
				D.CornerSeam(Mesher, FString::Printf(TEXT("t%d.corner.mixedB"), TI), MakeCornerSeam(T, M2));
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMeshingDigestTest, "VoxelWorlds.Meshing.Digest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVoxelMeshingDigestTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMeshingDigest;
	FDigest D(*this);

	D.Prefix = TEXT("default.");
	RunAllCases(D);

	// Each meshing cvar off on its own.
	const TCHAR* CVars[] = {
		TEXT("voxel.Mesh.CrossingGradientNormals"), TEXT("voxel.Mesh.VoxelShadingNormals"), TEXT("voxel.Mesh.VoxelLODCrossings"),
		TEXT("voxel.Mesh.QEFSolve"), TEXT("voxel.Mesh.ScaleClampedGradients"), TEXT("voxel.Seam.MCReadInPlace") };
	for (const TCHAR* Name : CVars)
	{
		FScopedCVar Off(Name, 0);
		D.Prefix = FString::Printf(TEXT("%s=0."), Name);
		RunAllCases(D);
	}

	UE_LOG(LogTemp, Display, TEXT("[MeshDigest] total cases=%d crc=%08X"), D.Cases, D.Total);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
