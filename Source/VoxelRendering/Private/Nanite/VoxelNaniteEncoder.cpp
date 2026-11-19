// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelNaniteEncoder.h"
#include "VoxelRendering.h"
#include "Nanite/VoxelNaniteFormat.h"
#include "Rendering/NaniteResources.h"
#include "Math/Float16.h"
#include "HAL/PlatformTime.h"

// The runtime Nanite spike's encoder (N1, Documentation/RUNTIME_NANITE_SPIKE_PLAN.md, D152). The value encodings below
// (UV floats, zigzag deltas, octahedral normals, tangent angles, the byte streams, the strip bitmasks) are adapted from
// UE 5.8.4's NaniteBuilder Encode folder, which the GPU transcode decodes; the clustering (index order), the flat
// hierarchy, the root-only fixups and the page filling are this file's own.

namespace
{
	using namespace VoxelNanite;

	constexpr uint32 NumTexCoords = 2;
	constexpr uint32 UVMantissaBits = NANITE_UV_FLOAT_NUM_MANTISSA_BITS;
	/** A corner refers back to a vertex at most this many vertices behind the cluster's newest; so a triangle's indices span
	 *  at most 31, which the GPU index format (a base and two 5-bit offsets) needs */
	constexpr uint32 MaxReferenceDistance = 29;
	/** Every cluster is a leaf of a root group that nothing streams: always drawn, whatever its error */
	constexpr uint32 LeafClusterFlags = NANITE_CLUSTER_FLAG_ROOT_LEAF | NANITE_CLUSTER_FLAG_STREAMING_LEAF
		| NANITE_CLUSTER_FLAG_FULL_LEAF | NANITE_CLUSTER_FLAG_ROOT_GROUP;

	// ==================== Value encodings (NaniteEncodeGeometryData.cpp) ====================

	uint32 EncodeUVFloat(float Value, uint32 NumMantissaBits)
	{
		// [0,1] is denormal (uniform precision); positive values have the sign bit set and negative ones their exponent
		// and mantissa inverted, so the encoded values keep the floats' order as unsigned integers
		if (!FMath::IsFinite(Value))
		{
			Value = 0.0f;
		}
		const uint32 SignBitPosition = NANITE_UV_FLOAT_NUM_EXPONENT_BITS + NumMantissaBits;
		const uint32 FloatUInt = reinterpret_cast<const uint32&>(Value);
		const uint32 AbsFloatUInt = FloatUInt & 0x7FFFFFFFu;
		uint32 Result;
		if (AbsFloatUInt < 0x3F800000u)
		{
			const float AbsFloat = reinterpret_cast<const float&>(AbsFloatUInt);
			Result = uint32(double(AbsFloat * float(1u << NumMantissaBits)) + 0.5);
		}
		else
		{
			const uint32 Shift = 23 - NumMantissaBits;
			const uint32 Tmp = (AbsFloatUInt - 0x3F000000u) + (1u << (Shift - 1));
			Result = FMath::Min(Tmp >> Shift, (1u << SignBitPosition) - 1u);
		}
		const uint32 SignMask = (1u << SignBitPosition) - (FloatUInt >> 31u);
		return Result ^ SignMask;
	}

	float DecodeUVFloat(uint32 EncodedValue, uint32 NumMantissaBits)
	{
		const uint32 ExponentAndMantissaMask = (1u << (NANITE_UV_FLOAT_NUM_EXPONENT_BITS + NumMantissaBits)) - 1u;
		const bool bNeg = EncodedValue <= ExponentAndMantissaMask;
		const uint32 ExponentAndMantissa = (bNeg ? ~EncodedValue : EncodedValue) & ExponentAndMantissaMask;
		const uint32 FloatBits = 0x3F000000u + (ExponentAndMantissa << (23 - NumMantissaBits));
		float Result = reinterpret_cast<const float&>(FloatBits);
		Result = FMath::Min(Result * 2.0f - 1.0f, Result);
		return bNeg ? -Result : Result;
	}

	/** The delta wrapped into NumBits (two's complement), as the decoder adds it modulo 2^NumBits */
	int32 ShortestWrap(int32 Value, uint32 NumBits)
	{
		if (NumBits == 0)
		{
			return 0;
		}
		const int32 Shift = 32 - NumBits;
		return (Value << Shift) >> Shift;
	}

	uint32 EncodeZigZag(int32 X) { return uint32((X << 1) ^ (X >> 31)); }
	int32 DecodeZigZag(uint32 X) { return int32(X >> 1) ^ -int32(X & 1); }

	FVector2f OctahedronEncode(FVector3f N)
	{
		FVector3f AbsN = N.GetAbs();
		N /= (AbsN.X + AbsN.Y + AbsN.Z);
		if (N.Z < 0.0f)
		{
			AbsN = N.GetAbs();
			N.X = (N.X >= 0.0f) ? (1.0f - AbsN.Y) : (AbsN.Y - 1.0f);
			N.Y = (N.Y >= 0.0f) ? (1.0f - AbsN.X) : (AbsN.X - 1.0f);
		}
		return FVector2f(N.X, N.Y);
	}

	FVector3f OctahedronDecode(int32 X, int32 Y, int32 QuantizationBits)
	{
		const int32 QuantizationMaxValue = (1 << QuantizationBits) - 1;
		float FX = float(X) * (2.0f / float(QuantizationMaxValue)) - 1.0f;
		float FY = float(Y) * (2.0f / float(QuantizationMaxValue)) - 1.0f;
		const float FZ = 1.0f - FMath::Abs(FX) - FMath::Abs(FY);
		const float T = FMath::Clamp(-FZ, 0.0f, 1.0f);
		FX += (FX >= 0.0f ? -T : T);
		FY += (FY >= 0.0f ? -T : T);
		return FVector3f(FX, FY, FZ).GetUnsafeNormal();
	}

	/** The quantized octahedral point nearest the normal among the four around it (OctahedronEncodePrecise) */
	uint32 PackNormal(const FVector3f& Normal, uint32 QuantizationBits)
	{
		const int32 QuantizationMaxValue = (1 << QuantizationBits) - 1;
		const FVector2f Coord = OctahedronEncode(Normal);
		const float Scale = 0.5f * float(QuantizationMaxValue);
		const float Bias = 0.5f * float(QuantizationMaxValue);
		const int32 NX = FMath::Clamp(int32(Coord.X * Scale + Bias), 0, QuantizationMaxValue);
		const int32 NY = FMath::Clamp(int32(Coord.Y * Scale + Bias), 0, QuantizationMaxValue);
		float MinError = 1.0f;
		int32 BestNX = NX;
		int32 BestNY = NY;
		for (int32 OffsetY = 0; OffsetY < 2; OffsetY++)
		{
			for (int32 OffsetX = 0; OffsetX < 2; OffsetX++)
			{
				const int32 TX = NX + OffsetX;
				const int32 TY = NY + OffsetY;
				if (TX <= QuantizationMaxValue && TY <= QuantizationMaxValue)
				{
					const FVector3f RN = OctahedronDecode(TX, TY, QuantizationBits);
					const float Error = FMath::Abs(1.0f - (RN | Normal));
					if (Error < MinError)
					{
						MinError = Error;
						BestNX = TX;
						BestNY = TY;
					}
				}
			}
		}
		return (uint32(BestNY) << QuantizationBits) | uint32(BestNX);
	}

	FVector3f UnpackNormal(uint32 PackedNormal, uint32 QuantizationBits)
	{
		const uint32 QuantizationMaxValue = (1u << QuantizationBits) - 1u;
		return OctahedronDecode(int32(PackedNormal & QuantizationMaxValue), int32(PackedNormal >> QuantizationBits), int32(QuantizationBits));
	}

	/** The tangent as an angle around the normal against a reference frame built from the normal alone */
	bool PackTangent(uint32& OutAngle, FVector3f TangentX, FVector3f TangentZ, uint32 NumTangentBits)
	{
		const bool bSwapXZ = FMath::Abs(TangentZ.Z) > FMath::Abs(TangentZ.X);
		if (bSwapXZ)
		{
			Swap(TangentZ.X, TangentZ.Z);
			Swap(TangentX.X, TangentX.Z);
		}
		const FVector3f RefX = FVector3f(-TangentZ.Y, TangentZ.X, 0.0f).GetSafeNormal();
		const FVector3f RefY = TangentZ ^ RefX;
		const float X = TangentX | RefX;
		const float Y = TangentX | RefY;
		if (X * X + Y * Y < 0.0001f)
		{
			return false;
		}
		float Angle = FMath::Atan2(Y, X);
		if (Angle < PI)
		{
			Angle += 2.0f * PI;
		}
		const int32 IntAngle = FMath::FloorToInt(Angle / (2.0f * PI) * float(1 << NumTangentBits) + 0.5f);
		OutAngle = uint32(IntAngle & ((1 << NumTangentBits) - 1));
		return true;
	}

	FVector3f UnpackTangent(uint32 Angle, FVector3f TangentZ, uint32 NumTangentBits)
	{
		const bool bSwapXZ = FMath::Abs(TangentZ.Z) > FMath::Abs(TangentZ.X);
		if (bSwapXZ)
		{
			Swap(TangentZ.X, TangentZ.Z);
		}
		const FVector3f RefX = FVector3f(-TangentZ.Y, TangentZ.X, 0.0f).GetSafeNormal();
		const FVector3f RefY = TangentZ ^ RefX;
		const float UnpackedAngle = float(Angle) / float(1 << NumTangentBits) * 2.0f * PI;
		FVector3f Result = (RefX * FMath::Cos(UnpackedAngle) + RefY * FMath::Sin(UnpackedAngle)).GetUnsafeNormal();
		if (bSwapXZ)
		{
			Swap(Result.X, Result.Z);
		}
		return Result;
	}

	/** Bits appended least significant first (NaniteEncodeShared.h FBitWriter) */
	struct FBitWriter
	{
		TArray<uint8>& Buffer;
		uint64 PendingBits = 0;
		int32 NumPendingBits = 0;

		explicit FBitWriter(TArray<uint8>& InBuffer) : Buffer(InBuffer) {}

		void PutBits(uint32 Bits, uint32 NumBits)
		{
			check(uint64(Bits) < (1ull << NumBits));
			PendingBits |= uint64(Bits) << NumPendingBits;
			NumPendingBits += NumBits;
			while (NumPendingBits >= 8)
			{
				Buffer.Add(uint8(PendingBits));
				PendingBits >>= 8;
				NumPendingBits -= 8;
			}
		}

		void Flush(uint32 Alignment)
		{
			if (NumPendingBits > 0)
			{
				Buffer.Add(uint8(PendingBits));
			}
			while (Buffer.Num() % Alignment != 0)
			{
				Buffer.Add(0);
			}
			PendingBits = 0;
			NumPendingBits = 0;
		}
	};

	/** A vertex as Nanite stores it */
	struct FSourceVertex
	{
		FVector3f Position;
		FVector3f Normal;
		FVector3f TangentX;
		bool bTangentYNegative = false;
		FColor Color;
		FVector2f UVs[NumTexCoords];
	};

	FSourceVertex ToSourceVertex(const FVoxelLocalVertex& In)
	{
		FSourceVertex Out;
		Out.Position = In.Position;
		Out.Normal = In.TangentZ.ToFVector3f().GetSafeNormal();
		if (Out.Normal.IsNearlyZero())
		{
			Out.Normal = FVector3f::UpVector;
		}
		Out.TangentX = In.TangentX.ToFVector3f();
		Out.bTangentYNegative = In.TangentZ.ToFVector4f().W < 0.0f;
		Out.Color = In.Color;
		Out.UVs[0] = In.TexCoord;
		Out.UVs[1] = In.TexCoord1;
		return Out;
	}

	// ==================== Clusters ====================

	struct FCluster
	{
		/** The input vertex of each cluster vertex, in the order the strip decode numbers them */
		TArray<uint32, TInlineAllocator<NANITE_MAX_CLUSTER_VERTICES>> SourceVertices;
		/** Three cluster vertex indices a triangle, in the order the decode returns them */
		TArray<uint8, TInlineAllocator<NANITE_MAX_CLUSTER_INDICES>> Indexes;
		uint32 NumTris = 0;

		/** Strip description: [dword of 32 triangles][IsStart, IsLeft, IsRef]; refs and new vertices per dword */
		uint32 StripBitmasks[4][3] = {};
		uint32 NewInDword[4] = {};
		uint32 RefInDword[4] = {};
		TArray<uint8> StripIndexData;
		uint64 PendingRefBits = 0;
		int32 NumPendingRefBits = 0;

		// Quantization
		TArray<FIntVector, TInlineAllocator<NANITE_MAX_CLUSTER_VERTICES>> QuantizedPositions;
		FIntVector QuantizedPosStart = FIntVector::ZeroValue;
		FIntVector QuantizedPosBits = FIntVector::ZeroValue;
		FBox3f Bounds = FBox3f(ForceInit);
		float EdgeLength = 0.0f;

		// Encoding
		uint32 BitsPerIndex = 1;
		uint32 BitsPerAttribute = 0;
		uint32 ColorMode = NANITE_VERTEX_COLOR_MODE_CONSTANT;
		FIntVector4 ColorMin = FIntVector4(255, 255, 255, 255);
		FIntVector4 ColorBits = FIntVector4(0, 0, 0, 0);
		uint32 UVMin[NumTexCoords][2] = {};
		uint32 UVBits[NumTexCoords][2] = {};
		FPageSections GpuSizes;
		TArray<uint32> VertReuseBatchInfo;

		void PutRefBits(uint32 Bits)
		{
			PendingRefBits |= uint64(Bits) << NumPendingRefBits;
			NumPendingRefBits += 5;
			while (NumPendingRefBits >= 8)
			{
				StripIndexData.Add(uint8(PendingRefBits));
				PendingRefBits >>= 8;
				NumPendingRefBits -= 8;
			}
		}

		void FinishStrips()
		{
			if (NumPendingRefBits > 0)
			{
				StripIndexData.Add(uint8(PendingRefBits));
			}
			while (StripIndexData.Num() % 4 != 0)
			{
				StripIndexData.Add(0);
			}
			PendingRefBits = 0;
			NumPendingRefBits = 0;
		}

		uint32 GetNumPrevNewVerticesBeforeDwords() const
		{
			const uint32 N1 = NewInDword[0];
			const uint32 N2 = N1 + NewInDword[1];
			const uint32 N3 = N2 + NewInDword[2];
			return (N3 << 20) | (N2 << 10) | N1;
		}

		uint32 GetNumPrevRefVerticesBeforeDwords() const
		{
			const uint32 N1 = RefInDword[0];
			const uint32 N2 = N1 + RefInDword[1];
			const uint32 N3 = N2 + RefInDword[2];
			return (N3 << 20) | (N2 << 10) | N1;
		}
	};

	/**
	 * Cut the triangle list into clusters in index order. Each triangle is a strip start whose first corners are 5-bit
	 * references back from the cluster's newest vertex and whose others are new vertices (rotated, winding kept, so the
	 * references come first, as the decode reads them).
	 */
	TArray<FCluster> BuildClusters(TConstArrayView<uint32> Indices, int32 NumVertices, bool bVertexReuse, int32& OutNumTriangles)
	{
		TArray<FCluster> Clusters;
		TArray<int32> LocalIndex;
		TArray<int32> LocalCluster;
		LocalIndex.Init(-1, NumVertices);
		LocalCluster.Init(-1, NumVertices);
		OutNumTriangles = 0;

		const int32 NumTriangles = Indices.Num() / 3;
		for (int32 TriangleIndex = 0; TriangleIndex < NumTriangles; ++TriangleIndex)
		{
			uint32 Corners[3] = { Indices[3 * TriangleIndex + 0], Indices[3 * TriangleIndex + 1], Indices[3 * TriangleIndex + 2] };
			if (Corners[0] == Corners[1] || Corners[1] == Corners[2] || Corners[0] == Corners[2])
			{
				continue;
			}
			++OutNumTriangles;

			const auto IsReference = [&](uint32 Vertex) -> bool
			{
				if (!bVertexReuse || Clusters.Num() == 0 || LocalCluster[Vertex] != Clusters.Num() - 1)
				{
					return false;
				}
				const FCluster& Current = Clusters.Last();
				return uint32(Current.SourceVertices.Num() - 1 - LocalIndex[Vertex]) <= MaxReferenceDistance;
			};

			bool bRef[3] = { IsReference(Corners[0]), IsReference(Corners[1]), IsReference(Corners[2]) };
			int32 NumRefs = int32(bRef[0]) + int32(bRef[1]) + int32(bRef[2]);
			if (Clusters.Num() == 0 || Clusters.Last().NumTris == NANITE_MAX_CLUSTER_TRIANGLES
				|| Clusters.Last().SourceVertices.Num() + (3 - NumRefs) > NANITE_MAX_CLUSTER_VERTICES)
			{
				if (Clusters.Num() > 0)
				{
					Clusters.Last().FinishStrips();
				}
				Clusters.AddDefaulted();
				bRef[0] = bRef[1] = bRef[2] = false;
				NumRefs = 0;
			}
			FCluster& Cluster = Clusters.Last();
			const int32 ClusterIndex = Clusters.Num() - 1;

			// Rotate (keeping the winding) so the referenced corners come first
			int32 Rotation = 0;
			if (NumRefs == 1)
			{
				Rotation = bRef[0] ? 0 : (bRef[1] ? 1 : 2);
			}
			else if (NumRefs == 2)
			{
				Rotation = !bRef[2] ? 0 : (!bRef[0] ? 1 : 2);
			}
			const uint32 Rotated[3] = { Corners[Rotation], Corners[(Rotation + 1) % 3], Corners[(Rotation + 2) % 3] };

			const uint32 BaseVertex = uint32(Cluster.SourceVertices.Num()) - 1u;
			uint8 Local[3];
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 Vertex = Rotated[Corner];
				if (Corner < NumRefs)
				{
					Local[Corner] = uint8(LocalIndex[Vertex]);
					Cluster.PutRefBits(BaseVertex - uint32(LocalIndex[Vertex]));
				}
				else
				{
					Local[Corner] = uint8(Cluster.SourceVertices.Num());
					LocalIndex[Vertex] = Cluster.SourceVertices.Num();
					LocalCluster[Vertex] = ClusterIndex;
					Cluster.SourceVertices.Add(Vertex);
				}
			}
			Cluster.Indexes.Append(Local, 3);

			const uint32 ClusterTriangle = Cluster.NumTris++;
			const uint32 Dword = ClusterTriangle >> 5;
			const uint32 Bit = ClusterTriangle & 31u;
			Cluster.StripBitmasks[Dword][0] |= 1u << Bit;
			Cluster.StripBitmasks[Dword][1] |= uint32((NumRefs >> 1) & 1) << Bit;
			Cluster.StripBitmasks[Dword][2] |= uint32(NumRefs & 1) << Bit;
			Cluster.NewInDword[Dword] += 3 - NumRefs;
			Cluster.RefInDword[Dword] += NumRefs;
		}
		if (Clusters.Num() > 0)
		{
			Clusters.Last().FinishStrips();
		}
		return Clusters;
	}

	/** One position precision for the mesh, lowered until every cluster fits 21 bits an axis (the engine's rule) */
	int32 QuantizeClusters(TArray<FCluster>& Clusters, const TArray<FSourceVertex>& Vertices, int32 RequestedPrecision)
	{
		const int32 MaxQuantizedValue = (1 << NANITE_MAX_POSITION_QUANTIZATION_BITS) - 1;
		int32 Precision = FMath::Clamp(RequestedPrecision, NANITE_MIN_POSITION_PRECISION, NANITE_MAX_POSITION_PRECISION);
		float Scale = FMath::Exp2(float(Precision));

		for (const FCluster& Cluster : Clusters)
		{
			FBox3f Box(ForceInit);
			for (const uint32 Vertex : Cluster.SourceVertices)
			{
				Box += Vertices[Vertex].Position;
			}
			while (Precision > NANITE_MIN_POSITION_PRECISION)
			{
				const FVector3f Min = Box.Min * Scale;
				const FVector3f Max = Box.Max * Scale;
				const bool bFits = Min.GetMin() >= -2147483648.0f && Max.GetMax() <= 2147483520.0f
					&& int64(FMath::RoundToFloat(Max.X)) - int64(FMath::RoundToFloat(Min.X)) <= MaxQuantizedValue
					&& int64(FMath::RoundToFloat(Max.Y)) - int64(FMath::RoundToFloat(Min.Y)) <= MaxQuantizedValue
					&& int64(FMath::RoundToFloat(Max.Z)) - int64(FMath::RoundToFloat(Min.Z)) <= MaxQuantizedValue;
				if (bFits)
				{
					break;
				}
				Scale *= 0.5f;
				--Precision;
			}
		}

		const float RcpScale = 1.0f / Scale;
		for (FCluster& Cluster : Clusters)
		{
			const int32 NumVerts = Cluster.SourceVertices.Num();
			Cluster.QuantizedPositions.SetNumUninitialized(NumVerts);
			FIntVector Min(MAX_int32, MAX_int32, MAX_int32);
			FIntVector Max(MIN_int32, MIN_int32, MIN_int32);
			for (int32 i = 0; i < NumVerts; ++i)
			{
				const FVector3f& Position = Vertices[Cluster.SourceVertices[i]].Position;
				const FIntVector Q(int32(FMath::RoundToFloat(Position.X * Scale)), int32(FMath::RoundToFloat(Position.Y * Scale)),
					int32(FMath::RoundToFloat(Position.Z * Scale)));
				Cluster.QuantizedPositions[i] = Q;
				Min = FIntVector(FMath::Min(Min.X, Q.X), FMath::Min(Min.Y, Q.Y), FMath::Min(Min.Z, Q.Z));
				Max = FIntVector(FMath::Max(Max.X, Q.X), FMath::Max(Max.Y, Q.Y), FMath::Max(Max.Z, Q.Z));
			}
			Cluster.QuantizedPosBits = FIntVector(FMath::CeilLogTwo(uint32(Max.X - Min.X + 1)), FMath::CeilLogTwo(uint32(Max.Y - Min.Y + 1)),
				FMath::CeilLogTwo(uint32(Max.Z - Min.Z + 1)));
			Cluster.QuantizedPosStart = Min;
			for (FIntVector& Q : Cluster.QuantizedPositions)
			{
				Q -= Min;
			}
			Cluster.Bounds = FBox3f(FVector3f(Min) * RcpScale, FVector3f(Max) * RcpScale);

			// The longest edge, which picks hardware or software raster per cluster
			float MaxEdgeSquared = 0.0f;
			for (uint32 Triangle = 0; Triangle < Cluster.NumTris; ++Triangle)
			{
				for (int32 Edge = 0; Edge < 3; ++Edge)
				{
					const FIntVector A = Cluster.QuantizedPositions[Cluster.Indexes[3 * Triangle + Edge]];
					const FIntVector B = Cluster.QuantizedPositions[Cluster.Indexes[3 * Triangle + (Edge + 1) % 3]];
					MaxEdgeSquared = FMath::Max(MaxEdgeSquared, FVector3f(A - B).SizeSquared());
				}
			}
			Cluster.EdgeLength = FMath::Sqrt(MaxEdgeSquared) * RcpScale;
		}
		return Precision;
	}

	/** The bit widths and GPU sizes of a cluster (CalculateEncodingInfo, with tangents, colours and two UV sets) */
	void CalculateEncoding(FCluster& Cluster, const TArray<FSourceVertex>& Vertices, const FVoxelNaniteEncodeSettings& Settings)
	{
		const uint32 NumVerts = Cluster.SourceVertices.Num();
		const uint32 NumTris = Cluster.NumTris;
		Cluster.BitsPerIndex = (NumVerts > 1 && NumTris > 1) ? (FMath::FloorLog2(NumVerts - 1) + 1) : 1;
		const uint32 BitsPerTriangle = Cluster.BitsPerIndex + 2 * 5;

		Cluster.BitsPerAttribute = 2 * Settings.NormalPrecision + 1 + Settings.TangentPrecision;

		FIntVector4 ColorMin(255, 255, 255, 255);
		FIntVector4 ColorMax(0, 0, 0, 0);
		for (const uint32 Vertex : Cluster.SourceVertices)
		{
			const FColor& Color = Vertices[Vertex].Color;
			ColorMin = FIntVector4(FMath::Min(ColorMin.X, int32(Color.R)), FMath::Min(ColorMin.Y, int32(Color.G)), FMath::Min(ColorMin.Z, int32(Color.B)), FMath::Min(ColorMin.W, int32(Color.A)));
			ColorMax = FIntVector4(FMath::Max(ColorMax.X, int32(Color.R)), FMath::Max(ColorMax.Y, int32(Color.G)), FMath::Max(ColorMax.Z, int32(Color.B)), FMath::Max(ColorMax.W, int32(Color.A)));
		}
		Cluster.ColorMin = ColorMin;
		Cluster.ColorBits = FIntVector4(FMath::CeilLogTwo(uint32(ColorMax.X - ColorMin.X + 1)), FMath::CeilLogTwo(uint32(ColorMax.Y - ColorMin.Y + 1)),
			FMath::CeilLogTwo(uint32(ColorMax.Z - ColorMin.Z + 1)), FMath::CeilLogTwo(uint32(ColorMax.W - ColorMin.W + 1)));
		const uint32 NumColorBits = Cluster.ColorBits.X + Cluster.ColorBits.Y + Cluster.ColorBits.Z + Cluster.ColorBits.W;
		Cluster.BitsPerAttribute += NumColorBits;
		Cluster.ColorMode = NumColorBits > 0 ? NANITE_VERTEX_COLOR_MODE_VARIABLE : NANITE_VERTEX_COLOR_MODE_CONSTANT;

		for (uint32 UVIndex = 0; UVIndex < NumTexCoords; ++UVIndex)
		{
			uint32 Min[2] = { MAX_uint32, MAX_uint32 };
			uint32 Max[2] = { 0, 0 };
			for (const uint32 Vertex : Cluster.SourceVertices)
			{
				const FVector2f& UV = Vertices[Vertex].UVs[UVIndex];
				const uint32 U = EncodeUVFloat(UV.X, UVMantissaBits);
				const uint32 V = EncodeUVFloat(UV.Y, UVMantissaBits);
				Min[0] = FMath::Min(Min[0], U);
				Min[1] = FMath::Min(Min[1], V);
				Max[0] = FMath::Max(Max[0], U);
				Max[1] = FMath::Max(Max[1], V);
			}
			for (int32 Axis = 0; Axis < 2; ++Axis)
			{
				Cluster.UVMin[UVIndex][Axis] = Min[Axis];
				Cluster.UVBits[UVIndex][Axis] = FMath::CeilLogTwo(Max[Axis] - Min[Axis] + 1u);
				Cluster.BitsPerAttribute += Cluster.UVBits[UVIndex][Axis];
			}
		}

		FPageSections& Sizes = Cluster.GpuSizes;
		Sizes = FPageSections();
		Sizes.Cluster = sizeof(Nanite::FPackedCluster);
		Sizes.DecodeInfo = NumTexCoords * sizeof(FPackedUVHeader);
		Sizes.Index = (NumTris * BitsPerTriangle + 31) / 32 * 4;
		const uint32 PositionBitsPerVertex = Cluster.QuantizedPosBits.X + Cluster.QuantizedPosBits.Y + Cluster.QuantizedPosBits.Z;
		Sizes.Position = (NumVerts * PositionBitsPerVertex + 31) / 32 * 4;
		Sizes.Attribute = (NumVerts * Cluster.BitsPerAttribute + 31) / 32 * 4;

		// Vertex reuse batches for the hardware rasterizer: at most 32 unique vertices and 32 triangles each
		// (NaniteEncodeVertReuseBatch.cpp), one material range, packed inline: 4 bits of batch count, then 5 bits a batch
		TArray<uint32, TInlineAllocator<16>> BatchTriCounts;
		{
			TBitArray<> Used(false, NANITE_MAX_CLUSTER_VERTICES);
			uint32 NumUnique = 0;
			uint32 NumBatchTris = 0;
			for (uint32 Triangle = 0; Triangle < NumTris; ++Triangle)
			{
				const uint32 V[3] = { Cluster.Indexes[3 * Triangle], Cluster.Indexes[3 * Triangle + 1], Cluster.Indexes[3 * Triangle + 2] };
				const uint32 NumNew = uint32(!Used[V[0]]) + uint32(!Used[V[1]]) + uint32(!Used[V[2]]);
				if (NumUnique + NumNew > 32)
				{
					BatchTriCounts.Add(NumBatchTris);
					NumUnique = 0;
					NumBatchTris = 0;
					Used.Init(false, NANITE_MAX_CLUSTER_VERTICES);
					--Triangle;
					continue;
				}
				Used[V[0]] = true;
				Used[V[1]] = true;
				Used[V[2]] = true;
				NumUnique += NumNew;
				if (++NumBatchTris == 32)
				{
					BatchTriCounts.Add(NumBatchTris);
					NumUnique = 0;
					NumBatchTris = 0;
					Used.Init(false, NANITE_MAX_CLUSTER_VERTICES);
				}
			}
			if (NumBatchTris > 0)
			{
				BatchTriCounts.Add(NumBatchTris);
			}
		}
		check(BatchTriCounts.Num() > 0 && BatchTriCounts.Num() < 16);
		const uint32 NumBitsNeeded = 4 + BatchTriCounts.Num() * 5 + 4 * 2;
		Cluster.VertReuseBatchInfo.Init(0u, FMath::DivideAndRoundUp(NumBitsNeeded, 32u));
		check(Cluster.VertReuseBatchInfo.Num() <= 4);
		const auto SetBitsAt = [&Cluster](uint32 BitOffset, uint32 Bits, uint32 NumBits)
		{
			for (uint32 i = 0; i < NumBits; ++i)
			{
				const uint32 Bit = BitOffset + i;
				Cluster.VertReuseBatchInfo[Bit >> 5] |= ((Bits >> i) & 1u) << (Bit & 31u);
			}
		};
		SetBitsAt(0, BatchTriCounts.Num(), 4);
		for (int32 Batch = 0; Batch < BatchTriCounts.Num(); ++Batch)
		{
			SetBitsAt(3 * 4 + Batch * 5, BatchTriCounts[Batch] - 1, 5);
		}
	}

	/** A cluster's vertex data into the page's low, mid and high byte streams (EncodeGeometryData, no vertex refs) */
	void EncodeGeometry(const FCluster& Cluster, const TArray<FSourceVertex>& Vertices, const FVoxelNaniteEncodeSettings& Settings,
		TArray<uint8>& Low, TArray<uint8>& Mid, TArray<uint8>& High)
	{
		const auto WriteZigZagDelta = [&](int32 Delta, uint32 NumBytes)
		{
			const uint32 Value = EncodeZigZag(Delta);
			check(NumBytes == 0 ? Value == 0 : (NumBytes >= 4 || Value < (1u << (NumBytes * 8))));
			if (NumBytes >= 3)
			{
				High.Add((Value >> 16) & 0xFFu);
			}
			if (NumBytes >= 2)
			{
				Mid.Add((Value >> 8) & 0xFFu);
			}
			if (NumBytes >= 1)
			{
				Low.Add(Value & 0xFFu);
			}
		};
		const uint32 NumVerts = Cluster.SourceVertices.Num();
		const FIntVector& PosBits = Cluster.QuantizedPosBits;
		const uint32 BytesPerPosition = (FMath::Max3(PosBits.X, PosBits.Y, PosBits.Z) + 7) / 8;
		const uint32 BytesPerNormal = (Settings.NormalPrecision + 7) / 8;
		const uint32 BytesPerTangent = (Settings.TangentPrecision + 1 + 7) / 8;

		FIntVector PrevPosition((1 << PosBits.X) >> 1, (1 << PosBits.Y) >> 1, (1 << PosBits.Z) >> 1);
		for (uint32 i = 0; i < NumVerts; ++i)
		{
			const FIntVector& Position = Cluster.QuantizedPositions[i];
			WriteZigZagDelta(ShortestWrap(Position.X - PrevPosition.X, PosBits.X), BytesPerPosition);
			WriteZigZagDelta(ShortestWrap(Position.Y - PrevPosition.Y, PosBits.Y), BytesPerPosition);
			WriteZigZagDelta(ShortestWrap(Position.Z - PrevPosition.Z, PosBits.Z), BytesPerPosition);
			PrevPosition = Position;
		}

		TArray<uint32, TInlineAllocator<NANITE_MAX_CLUSTER_VERTICES>> PackedNormals;
		PackedNormals.SetNumUninitialized(NumVerts);
		FIntPoint PrevNormal = FIntPoint::ZeroValue;
		const uint32 NormalMask = (1u << Settings.NormalPrecision) - 1u;
		for (uint32 i = 0; i < NumVerts; ++i)
		{
			const uint32 Packed = PackNormal(Vertices[Cluster.SourceVertices[i]].Normal, Settings.NormalPrecision);
			PackedNormals[i] = Packed;
			const FIntPoint Normal(int32(Packed & NormalMask), int32(Packed >> Settings.NormalPrecision));
			WriteZigZagDelta(ShortestWrap(Normal.X - PrevNormal.X, Settings.NormalPrecision), BytesPerNormal);
			WriteZigZagDelta(ShortestWrap(Normal.Y - PrevNormal.Y, Settings.NormalPrecision), BytesPerNormal);
			PrevNormal = Normal;
		}

		uint32 PrevTangentBits = 0;
		for (uint32 i = 0; i < NumVerts; ++i)
		{
			const FSourceVertex& Vertex = Vertices[Cluster.SourceVertices[i]];
			const FVector3f TangentZ = UnpackNormal(PackedNormals[i], Settings.NormalPrecision);
			// A collapsed tangent repeats the previous vertex's (the engine's rule)
			uint32 TangentBits = PrevTangentBits;
			if (Vertex.TangentX.SizeSquared() > 1e-8f)
			{
				uint32 Angle;
				if (PackTangent(Angle, Vertex.TangentX.GetUnsafeNormal(), TangentZ, Settings.TangentPrecision))
				{
					TangentBits = (Vertex.bTangentYNegative ? (1u << Settings.TangentPrecision) : 0u) | Angle;
				}
			}
			WriteZigZagDelta(ShortestWrap(int32(TangentBits) - int32(PrevTangentBits), Settings.TangentPrecision + 1), BytesPerTangent);
			PrevTangentBits = TangentBits;
		}

		if (Cluster.ColorMode == NANITE_VERTEX_COLOR_MODE_VARIABLE)
		{
			FIntVector4 PrevColor(0, 0, 0, 0);
			for (uint32 i = 0; i < NumVerts; ++i)
			{
				const FColor& Color = Vertices[Cluster.SourceVertices[i]].Color;
				const FIntVector4 Value = FIntVector4(Color.R, Color.G, Color.B, Color.A) - Cluster.ColorMin;
				WriteZigZagDelta(ShortestWrap(Value.X - PrevColor.X, Cluster.ColorBits.X), 1);
				WriteZigZagDelta(ShortestWrap(Value.Y - PrevColor.Y, Cluster.ColorBits.Y), 1);
				WriteZigZagDelta(ShortestWrap(Value.Z - PrevColor.Z, Cluster.ColorBits.Z), 1);
				WriteZigZagDelta(ShortestWrap(Value.W - PrevColor.W, Cluster.ColorBits.W), 1);
				PrevColor = Value;
			}
		}

		for (uint32 UVIndex = 0; UVIndex < NumTexCoords; ++UVIndex)
		{
			const uint32 BitsU = Cluster.UVBits[UVIndex][0];
			const uint32 BitsV = Cluster.UVBits[UVIndex][1];
			const uint32 BytesPerUV = (FMath::Max(BitsU, BitsV) + 7) / 8;
			FIntPoint PrevUV = FIntPoint::ZeroValue;
			for (uint32 i = 0; i < NumVerts; ++i)
			{
				const FVector2f& UV = Vertices[Cluster.SourceVertices[i]].UVs[UVIndex];
				const FIntPoint Value(int32(EncodeUVFloat(UV.X, UVMantissaBits) - Cluster.UVMin[UVIndex][0]),
					int32(EncodeUVFloat(UV.Y, UVMantissaBits) - Cluster.UVMin[UVIndex][1]));
				WriteZigZagDelta(ShortestWrap(Value.X - PrevUV.X, BitsU), BytesPerUV);
				WriteZigZagDelta(ShortestWrap(Value.Y - PrevUV.Y, BitsV), BytesPerUV);
				PrevUV = Value;
			}
		}
	}

	/** The engine's packed cluster (PackCluster), the offsets the page gives it set by the caller */
	Nanite::FPackedCluster PackCluster(const FCluster& Cluster, int32 Precision, const FVoxelNaniteEncodeSettings& Settings, uint32 ClusterIndex)
	{
		Nanite::FPackedCluster Out;
		FMemory::Memzero(Out);
		Out.SetNumVerts(Cluster.SourceVertices.Num());
		Out.SetNumTris(Cluster.NumTris);
		Out.ColorMin = uint32(Cluster.ColorMin.X) | (uint32(Cluster.ColorMin.Y) << 8) | (uint32(Cluster.ColorMin.Z) << 16) | (uint32(Cluster.ColorMin.W) << 24);
		Out.SetColorBitsR(Cluster.ColorBits.X);
		Out.SetColorBitsG(Cluster.ColorBits.Y);
		Out.SetColorBitsB(Cluster.ColorBits.Z);
		Out.SetColorBitsA(Cluster.ColorBits.W);
		Out.SetGroupIndex(ClusterIndex);

		Out.PosStart = Cluster.QuantizedPosStart;
		Out.SetBitsPerIndex(Cluster.BitsPerIndex);
		Out.SetPosPrecision(Precision);
		Out.SetPosBitsX(Cluster.QuantizedPosBits.X);
		Out.SetPosBitsY(Cluster.QuantizedPosBits.Y);
		Out.SetPosBitsZ(Cluster.QuantizedPosBits.Z);

		const FVector3f Center = Cluster.Bounds.GetCenter();
		const float Radius = Cluster.Bounds.GetExtent().Size();
		Out.LODBounds = FSphere3f(Center, Radius);
		Out.BoxBoundsCenter = Center;
		// A leaf cluster's own error is 0: it is the finest there is
		Out.LODErrorAndEdgeLength = uint32(FFloat16(0.0f).Encoded) | (uint32(FFloat16(Cluster.EdgeLength).Encoded) << 16);
		Out.BoxBoundsExtent = Cluster.Bounds.GetExtent();
		Out.SetFlags(LeafClusterFlags);

		uint32 UVBitOffsets = 0;
		uint32 BitOffset = 0;
		for (uint32 UVIndex = 0; UVIndex < NumTexCoords; ++UVIndex)
		{
			UVBitOffsets |= BitOffset << (UVIndex * 8);
			BitOffset += Cluster.UVBits[UVIndex][0] + Cluster.UVBits[UVIndex][1];
		}
		Out.SetBitsPerAttribute(Cluster.BitsPerAttribute);
		Out.SetNormalPrecision(Settings.NormalPrecision);
		Out.SetTangentPrecision(Settings.TangentPrecision);
		Out.SetHasTangents(true);
		Out.SetNumUVs(NumTexCoords);
		Out.SetColorMode(Cluster.ColorMode);
		Out.UVBitOffsets = UVBitOffsets;
		// One material: index 0 over all the triangles (the fast path: Material0Length - 1 in bits 18-24)
		Out.PackedMaterialInfo = (Cluster.NumTris - 1u) << 18;
		Out.SetParentLODError(1e10f);
		Out.SetParentSphereBoundRadius(Radius);
		return Out;
	}

	/** The error pair every slot carries: always descend (MaxParentLODError, low half), never cull a leaf (MinLODError, high) */
	uint32 GetAlwaysDrawnLODErrors()
	{
		return uint32(FFloat16(1e10f).Encoded) | (uint32(FFloat16(-1.0f).Encoded) << 16);
	}

	void SetEmptySlot(Nanite::FPackedHierarchyNode& Node, int32 Slot)
	{
		Node.LODBounds[Slot] = FVector4f(0, 0, 0, 0);
		Node.Misc0[Slot].BoxBoundsCenter = FVector3f::ZeroVector;
		Node.Misc0[Slot].MinLODError_MaxParentLODError = GetAlwaysDrawnLODErrors();
		Node.Misc1[Slot].BoxBoundsExtent = FVector3f::ZeroVector;
		Node.Misc1[Slot].ChildStartReference = 0xFFFFFFFFu;
		Node.Misc2[Slot].ResourcePageRangeKey = NANITE_PAGE_RANGE_KEY_EMPTY_RANGE;
		Node.Misc2[Slot].GroupPartSize_AssemblyPartIndex = NANITE_HIERARCHY_MAX_ASSEMBLY_TRANSFORMS;
		Node.Misc2[Slot].NumBones_BoneIndicesOffset = 0;
	}

	void SetSlotBounds(Nanite::FPackedHierarchyNode& Node, int32 Slot, const FBox3f& Box)
	{
		Node.LODBounds[Slot] = FVector4f(Box.GetCenter(), Box.GetExtent().Size());
		Node.Misc0[Slot].BoxBoundsCenter = Box.GetCenter();
		Node.Misc0[Slot].MinLODError_MaxParentLODError = GetAlwaysDrawnLODErrors();
		Node.Misc1[Slot].BoxBoundsExtent = Box.GetExtent();
		Node.Misc2[Slot].NumBones_BoneIndicesOffset = 0;
	}

	template<typename T>
	void AppendBytes(TArray<uint8>& Out, const T& Value)
	{
		Out.Append(reinterpret_cast<const uint8*>(&Value), sizeof(T));
	}

	void AlignTo(TArray<uint8>& Out, int32 Alignment, int32 RelativeTo = 0)
	{
		while ((Out.Num() - RelativeTo) % Alignment != 0)
		{
			Out.Add(0);
		}
	}
}

TUniquePtr<Nanite::FResources> EncodeVoxelNaniteMesh(TConstArrayView<FVoxelLocalVertex> InVertices, TConstArrayView<uint32> Indices,
	const FVoxelNaniteEncodeSettings& Settings, FVoxelNaniteEncodeStats* OutStats)
{
	const double StartTime = FPlatformTime::Seconds();
	for (const uint32 Index : Indices)
	{
		if (Index >= uint32(InVertices.Num()))
		{
			UE_LOG(LogVoxelRendering, Error, TEXT("EncodeVoxelNaniteMesh: index %u out of %d vertices"), Index, InVertices.Num());
			return nullptr;
		}
	}

	TArray<FSourceVertex> Vertices;
	Vertices.SetNumUninitialized(InVertices.Num());
	for (int32 i = 0; i < InVertices.Num(); ++i)
	{
		Vertices[i] = ToSourceVertex(InVertices[i]);
	}

	int32 NumTriangles = 0;
	TArray<FCluster> Clusters = BuildClusters(Indices, Vertices.Num(), Settings.bVertexReuse, NumTriangles);
	if (Clusters.Num() == 0)
	{
		return nullptr;
	}
	const int32 Precision = QuantizeClusters(Clusters, Vertices, Settings.PositionPrecision);
	for (FCluster& Cluster : Clusters)
	{
		CalculateEncoding(Cluster, Vertices, Settings);
	}

	// Pages filled in cluster order, each a root page within the GPU page size and cluster count
	struct FPage
	{
		int32 FirstCluster = 0;
		int32 NumClusters = 0;
		FPageSections Sizes;
	};
	TArray<FPage> Pages;
	for (int32 ClusterIndex = 0; ClusterIndex < Clusters.Num(); ++ClusterIndex)
	{
		bool bNewPage = Pages.Num() == 0;
		if (!bNewPage)
		{
			FPageSections Candidate = Pages.Last().Sizes;
			Candidate += Clusters[ClusterIndex].GpuSizes;
			bNewPage = Candidate.GetTotal() > NANITE_ROOT_PAGE_GPU_SIZE || uint32(Pages.Last().NumClusters) >= NANITE_ROOT_PAGE_MAX_CLUSTERS;
		}
		if (bNewPage)
		{
			FPage& Page = Pages.AddDefaulted_GetRef();
			Page.FirstCluster = ClusterIndex;
		}
		FPage& Page = Pages.Last();
		Page.NumClusters++;
		Page.Sizes += Clusters[ClusterIndex].GpuSizes;
	}
	TArray<int32> ClusterPage;
	ClusterPage.SetNumUninitialized(Clusters.Num());
	for (int32 PageIndex = 0; PageIndex < Pages.Num(); ++PageIndex)
	{
		for (int32 i = 0; i < Pages[PageIndex].NumClusters; ++i)
		{
			ClusterPage[Pages[PageIndex].FirstCluster + i] = PageIndex;
		}
	}

	TUniquePtr<Nanite::FResources> Resources = MakeUnique<Nanite::FResources>();

	// The flat hierarchy: a slot per cluster in the leaf nodes, four children a node above them, the root first
	TArray<int32> LevelCounts;
	LevelCounts.Add(FMath::DivideAndRoundUp(Clusters.Num(), int32(NANITE_MAX_BVH_NODE_FANOUT)));
	while (LevelCounts.Last() > 1)
	{
		LevelCounts.Add(FMath::DivideAndRoundUp(LevelCounts.Last(), int32(NANITE_MAX_BVH_NODE_FANOUT)));
	}
	const int32 NumLevels = LevelCounts.Num();
	TArray<int32> LevelStart;
	LevelStart.SetNumZeroed(NumLevels);
	int32 NumNodes = 0;
	for (int32 Level = NumLevels - 1; Level >= 0; --Level)
	{
		LevelStart[Level] = NumNodes;
		NumNodes += LevelCounts[Level];
	}
	Resources->HierarchyNodes.SetNumZeroed(NumNodes);
	TArray<FBox3f> NodeBoxes;
	NodeBoxes.Init(FBox3f(ForceInit), NumNodes);
	TArray<uint32> ClusterHierarchyLocation;
	ClusterHierarchyLocation.SetNumUninitialized(Clusters.Num());
	for (int32 Level = 0; Level < NumLevels; ++Level)
	{
		const int32 NumChildren = Level == 0 ? Clusters.Num() : LevelCounts[Level - 1];
		for (int32 NodeInLevel = 0; NodeInLevel < LevelCounts[Level]; ++NodeInLevel)
		{
			const int32 NodeIndex = LevelStart[Level] + NodeInLevel;
			Nanite::FPackedHierarchyNode& Node = Resources->HierarchyNodes[NodeIndex];
			for (int32 Slot = 0; Slot < int32(NANITE_MAX_BVH_NODE_FANOUT); ++Slot)
			{
				const int32 Child = NodeInLevel * NANITE_MAX_BVH_NODE_FANOUT + Slot;
				if (Child >= NumChildren)
				{
					SetEmptySlot(Node, Slot);
					continue;
				}
				if (Level == 0)
				{
					// A leaf slot: one cluster, reached through the page's fixup, which writes ChildStartReference on install
					const FBox3f& Box = Clusters[Child].Bounds;
					SetSlotBounds(Node, Slot, Box);
					Node.Misc1[Slot].ChildStartReference = 0xFFFFFFFFu;
					Node.Misc2[Slot].ResourcePageRangeKey = Nanite::FPageRangeKey(ClusterPage[Child], 1, false, false).Value;
					Node.Misc2[Slot].GroupPartSize_AssemblyPartIndex = NANITE_HIERARCHY_MAX_ASSEMBLY_TRANSFORMS | (1u << NANITE_HIERARCHY_ASSEMBLY_TRANSFORM_INDEX_BITS);
					ClusterHierarchyLocation[Child] = PackHierarchyLocation(NodeIndex, Slot);
					NodeBoxes[NodeIndex] += Box;
				}
				else
				{
					const int32 ChildNode = LevelStart[Level - 1] + Child;
					const FBox3f& Box = NodeBoxes[ChildNode];
					SetSlotBounds(Node, Slot, Box);
					Node.Misc1[Slot].ChildStartReference = uint32(ChildNode);
					Node.Misc2[Slot].ResourcePageRangeKey = 0xFFFFFFFFu;
					Node.Misc2[Slot].GroupPartSize_AssemblyPartIndex = NANITE_HIERARCHY_MAX_ASSEMBLY_TRANSFORMS;
					NodeBoxes[NodeIndex] += Box;
				}
			}
		}
	}
	// The leaves' depth: the levels the traversal must reach (the root is depth 0)
	const uint32 LeafDepth = uint32(NumLevels - 1);

	int64 GpuBytes = 0;
	for (int32 PageIndex = 0; PageIndex < Pages.Num(); ++PageIndex)
	{
		const FPage& Page = Pages[PageIndex];
		const uint32 NumPageClusters = Page.NumClusters;

		// Fixup: one group (this page), a part and a hierarchy location per cluster
		TArray<uint8> Fixup;
		{
			FFixupHeader Header;
			Header.Magic = NANITE_FIXUP_MAGIC;
			Header.NumGroupFixups = 1;
			Header.NumPartFixups = uint16(NumPageClusters);
			Header.NumClusters = uint16(NumPageClusters);
			Header.NumHierarchyLocations = NumPageClusters;
			AppendBytes(Fixup, Header);
			FFixupGroup Group;
			Group.PageDependencies = Nanite::FPageRangeKey(PageIndex, 1, false, false).Value;
			Group.FirstPartFixup = 0;
			Group.NumPartFixups = uint16(NumPageClusters);
			AppendBytes(Fixup, Group);
			for (uint32 i = 0; i < NumPageClusters; ++i)
			{
				FFixupPart Part;
				Part.PageIndex = uint16(PageIndex);
				Part.StartClusterIndex = uint8(i);
				Part.FirstHierarchyLocation = i;
				Part.NumHierarchyLocations = 1;
				AppendBytes(Fixup, Part);
			}
			for (uint32 i = 0; i < NumPageClusters; ++i)
			{
				AppendBytes(Fixup, ClusterHierarchyLocation[Page.FirstCluster + i]);
			}
			check(uint32(Fixup.Num()) == GetFixupChunkSize(1, NumPageClusters, 0, NumPageClusters, 0, 0));
		}

		// Clusters packed at the offsets the page gives them; their byte streams
		const FPageSections Offsets = Page.Sizes.GetOffsets();
		FPageSections Running = Offsets;
		TArray<Nanite::FPackedCluster> Packed;
		Packed.SetNumUninitialized(NumPageClusters);
		TArray<uint8> Low, Mid, High;
		TArray<uint32> LowCounts, MidCounts, HighCounts;
		for (uint32 i = 0; i < NumPageClusters; ++i)
		{
			const int32 ClusterIndex = Page.FirstCluster + i;
			const FCluster& Cluster = Clusters[ClusterIndex];
			Nanite::FPackedCluster& Out = Packed[i];
			Out = PackCluster(Cluster, Precision, Settings, ClusterIndex);
			Out.SetIndexOffset(Running.Index);
			Out.SetPositionOffset(Running.Position);
			Out.SetAttributeOffset(Running.Attribute);
			Out.SetDecodeInfoOffset(Running.DecodeInfo);
			Out.SetHasSkinning(false);
			TArray<uint32> BatchInfo(Cluster.VertReuseBatchInfo);
			Out.SetVertResourceBatchInfo(BatchInfo, Running.VertReuseBatchInfo, 1);
			Running += Cluster.GpuSizes;

			const int32 PrevLow = Low.Num(), PrevMid = Mid.Num(), PrevHigh = High.Num();
			EncodeGeometry(Cluster, Vertices, Settings, Low, Mid, High);
			LowCounts.Add(Low.Num() - PrevLow);
			MidCounts.Add(Mid.Num() - PrevMid);
			HighCounts.Add(High.Num() - PrevHigh);
		}
		check(Running.Attribute == Page.Sizes.GetTotal());

		TArray<uint8> Data;
		Data.AddZeroed(sizeof(FPageDiskHeader) + NumPageClusters * sizeof(FClusterDiskHeader));
		TArray<FClusterDiskHeader> ClusterHeaders;
		ClusterHeaders.SetNum(NumPageClusters);

		// The section copied to the GPU as is: the GPU header, the clusters as columns of float4s, the UV headers
		const int32 RawStart = Data.Num();
		{
			FPageGPUHeader GPUHeader;
			GPUHeader.NumClusters_MaxClusterBoneInfluences_MaxVoxelBoneInfluences = NumPageClusters;
			AppendBytes(Data, GPUHeader);
			for (uint32 Float4 = 0; Float4 < NANITE_NUM_PACKED_CLUSTER_FLOAT4S; ++Float4)
			{
				for (uint32 i = 0; i < NumPageClusters; ++i)
				{
					Data.Append(reinterpret_cast<const uint8*>(&Packed[i]) + Float4 * 16, 16);
				}
			}
			// No bone influences, material table (one material each), vertex reuse table (inline), bricks or extended data
			const int32 DecodeInfoStart = Data.Num();
			for (uint32 i = 0; i < NumPageClusters; ++i)
			{
				const FCluster& Cluster = Clusters[Page.FirstCluster + i];
				ClusterHeaders[i].DecodeInfoOffset = Data.Num();
				for (uint32 UVIndex = 0; UVIndex < NumTexCoords; ++UVIndex)
				{
					FPackedUVHeader UVHeader;
					UVHeader.Data[0] = (Cluster.UVMin[UVIndex][0] << 5) | Cluster.UVBits[UVIndex][0];
					UVHeader.Data[1] = (Cluster.UVMin[UVIndex][1] << 5) | Cluster.UVBits[UVIndex][1];
					AppendBytes(Data, UVHeader);
				}
			}
			AlignTo(Data, 16, DecodeInfoStart);
		}
		const int32 RawEnd = Data.Num();
		check(uint32(RawEnd - RawStart) == Offsets.Index);

		// Strip index data (5-bit references), then the strip bitmasks
		for (uint32 i = 0; i < NumPageClusters; ++i)
		{
			const FCluster& Cluster = Clusters[Page.FirstCluster + i];
			ClusterHeaders[i].IndexDataOffset = Data.Num();
			ClusterHeaders[i].NumPrevNewVerticesBeforeDwords = Cluster.GetNumPrevNewVerticesBeforeDwords();
			ClusterHeaders[i].NumPrevRefVerticesBeforeDwords = Cluster.GetNumPrevRefVerticesBeforeDwords();
			Data.Append(Cluster.StripIndexData);
		}
		AlignTo(Data, 4);
		const uint32 StripBitmaskOffset = Data.Num();
		for (uint32 i = 0; i < NumPageClusters; ++i)
		{
			const FCluster& Cluster = Clusters[Page.FirstCluster + i];
			for (int32 Dword = 0; Dword < 4; ++Dword)
			{
				for (int32 Mask = 0; Mask < 3; ++Mask)
				{
					AppendBytes(Data, Cluster.StripBitmasks[Dword][Mask]);
				}
			}
		}

		// No page-cluster pairs or vertex references; the vertex reference bitmask is still written, all zero
		for (uint32 i = 0; i < NumPageClusters; ++i)
		{
			ClusterHeaders[i].PageClusterMapOffset = Data.Num();
		}
		const uint32 VertexRefBitmaskOffset = Data.Num();
		Data.AddZeroed(NumPageClusters * (NANITE_MAX_CLUSTER_VERTICES / 8));
		for (uint32 i = 0; i < NumPageClusters; ++i)
		{
			ClusterHeaders[i].VertexRefDataOffset = Data.Num();
			ClusterHeaders[i].NumVertexRefs = 0;
		}
		AlignTo(Data, 4);

		// The low, mid and high byte streams
		{
			uint32 NextLow = Data.Num();
			uint32 NextMid = NextLow + Low.Num();
			uint32 NextHigh = NextMid + Mid.Num();
			for (uint32 i = 0; i < NumPageClusters; ++i)
			{
				ClusterHeaders[i].LowBytesOffset = NextLow;
				ClusterHeaders[i].MidBytesOffset = NextMid;
				ClusterHeaders[i].HighBytesOffset = NextHigh;
				NextLow += LowCounts[i];
				NextMid += MidCounts[i];
				NextHigh += HighCounts[i];
			}
			Data.Append(Low);
			Data.Append(Mid);
			Data.Append(High);
		}

		FPageDiskHeader DiskHeader;
		DiskHeader.NumClusters = NumPageClusters;
		DiskHeader.NumRawFloat4s = uint32(RawEnd - RawStart) / 16u;
		DiskHeader.NumVertexRefs = 0;
		DiskHeader.StripBitmaskOffset = StripBitmaskOffset;
		DiskHeader.VertexRefBitmaskOffset = VertexRefBitmaskOffset;
		FMemory::Memcpy(Data.GetData(), &DiskHeader, sizeof(DiskHeader));
		FMemory::Memcpy(Data.GetData() + sizeof(DiskHeader), ClusterHeaders.GetData(), ClusterHeaders.Num() * sizeof(FClusterDiskHeader));
		AlignTo(Data, 4);

		Nanite::FPageStreamingState& State = Resources->PageStreamingStates.AddZeroed_GetRef();
		State.BulkOffset = Resources->RootData.Num();
		Resources->RootData.Append(Fixup);
		Resources->RootData.Append(Data);
		State.BulkSize = Resources->RootData.Num() - State.BulkOffset;
		State.PageSize = Data.Num();
		State.DependenciesStart = 0;
		State.DependenciesNum = 0;
		State.MaxHierarchyDepth = uint8(LeafDepth);
		State.Flags = 0;
		GpuBytes += Page.Sizes.GetTotal();
	}

	FBox3f MeshBox(ForceInit);
	for (const FCluster& Cluster : Clusters)
	{
		MeshBox += Cluster.Bounds;
	}
	Resources->HierarchyRootOffsets.Add(0);
	Resources->NumRootPages = Pages.Num();
	Resources->PositionPrecision = Precision;
	Resources->NormalPrecision = Settings.NormalPrecision;
	Resources->TangentPrecision = Settings.TangentPrecision;
	Resources->NumInputTriangles = NumTriangles;
	Resources->NumInputVertices = InVertices.Num();
	Resources->NumClusters = Clusters.Num();
	Resources->MeshBounds = FBoxSphereBounds3f(MeshBox);
	Resources->ResourceFlags = 0;

	if (OutStats)
	{
		OutStats->NumTriangles = NumTriangles;
		OutStats->NumClusters = Clusters.Num();
		OutStats->NumPages = Pages.Num();
		OutStats->NumHierarchyNodes = NumNodes;
		OutStats->NumStoredVertices = 0;
		for (const FCluster& Cluster : Clusters)
		{
			OutStats->NumStoredVertices += Cluster.SourceVertices.Num();
		}
		OutStats->GpuBytes = GpuBytes;
		OutStats->RootDataBytes = Resources->RootData.Num();
		OutStats->PositionPrecision = Precision;
		OutStats->EncodeSeconds = FPlatformTime::Seconds() - StartTime;
	}
	return Resources;
}

// ==================== Validation (a CPU decode as the GPU transcode reads the pages) ====================

namespace
{
	uint32 BitFieldExtractU32(uint32 Data, int32 NumBits, int32 StartBit)
	{
		return NumBits == 0 ? 0u : (Data << (32 - StartBit - NumBits)) >> (32 - NumBits);
	}

	int32 BitFieldExtractI32(int32 Data, int32 NumBits, int32 StartBit)
	{
		return (Data << (32 - StartBit - NumBits)) >> (32 - NumBits);
	}

	/** 25 valid bits from a bit offset (the decode reads a dword past the data's end; the caller pads) */
	uint32 ReadUnalignedDword(const uint8* Src, int32 BitOffset)
	{
		if (BitOffset < 0)
		{
			uint32 Value;
			FMemory::Memcpy(&Value, Src, 4);
			return Value << (8 - (BitOffset & 7));
		}
		uint32 Value;
		FMemory::Memcpy(&Value, Src + (BitOffset >> 3), 4);
		return Value >> (BitOffset & 7);
	}

	/** NaniteEncodeTriStrip.cpp UnpackTriangleIndices, the decode the GPU transcode runs */
	void UnpackTriangleIndices(const uint32 (&Bitmasks)[4][3], uint32 NumPrevRefBeforeDwords, uint32 NumPrevNewBeforeDwords,
		const uint8* StripIndexData, uint32 TriIndex, uint32* OutIndices)
	{
		const uint32 DwordIndex = TriIndex >> 5;
		const uint32 BitIndex = TriIndex & 31u;
		const uint32 SMask = Bitmasks[DwordIndex][0];
		const uint32 LMask = Bitmasks[DwordIndex][1];
		const uint32 WMask = Bitmasks[DwordIndex][2];
		const uint32 SLMask = SMask & LMask;
		const uint32 HeadRefVertexMask = (SLMask | ~SMask) & WMask;
		const uint32 PrevBitsMask = (1u << BitIndex) - 1u;
		const uint32 NumPrevRefVerticesBeforeDword = DwordIndex ? BitFieldExtractU32(NumPrevRefBeforeDwords, 10, DwordIndex * 10 - 10) : 0u;
		const uint32 NumPrevNewVerticesBeforeDword = DwordIndex ? BitFieldExtractU32(NumPrevNewBeforeDwords, 10, DwordIndex * 10 - 10) : 0u;

		const int32 CurrentDwordNumPrevRefVertices = int32((FMath::CountBits(SLMask & PrevBitsMask) << 1) + FMath::CountBits(WMask & PrevBitsMask));
		const int32 CurrentDwordNumPrevNewVertices = int32(FMath::CountBits(SMask & PrevBitsMask) << 1) + int32(BitIndex) - CurrentDwordNumPrevRefVertices;
		const int32 NumPrevRefVertices = NumPrevRefVerticesBeforeDword + CurrentDwordNumPrevRefVertices;
		const int32 NumPrevNewVertices = NumPrevNewVerticesBeforeDword + CurrentDwordNumPrevNewVertices;

		const int32 IsStart = BitFieldExtractI32(SMask, 1, BitIndex);
		const int32 IsLeft = BitFieldExtractI32(LMask, 1, BitIndex);
		const int32 IsRef = BitFieldExtractI32(WMask, 1, BitIndex);
		const uint32 BaseVertex = NumPrevNewVertices - 1u;
		uint32 IndexData = ReadUnalignedDword(StripIndexData, (NumPrevRefVertices + ~IsStart) * 5);

		if (IsStart)
		{
			const int32 MinusNumRefVertices = (IsLeft << 1) + IsRef;
			uint32 NextVertex = NumPrevNewVertices;
			if (MinusNumRefVertices <= -1) { OutIndices[0] = BaseVertex - (IndexData & 31u); IndexData >>= 5; } else { OutIndices[0] = NextVertex++; }
			if (MinusNumRefVertices <= -2) { OutIndices[1] = BaseVertex - (IndexData & 31u); IndexData >>= 5; } else { OutIndices[1] = NextVertex++; }
			if (MinusNumRefVertices <= -3) { OutIndices[2] = BaseVertex - (IndexData & 31u); } else { OutIndices[2] = NextVertex++; }
			return;
		}

		const uint32 PrevBitIndex = BitIndex - 1u;
		const int32 IsPrevStart = BitFieldExtractI32(SMask, 1, PrevBitIndex);
		const int32 IsPrevHeadRef = BitFieldExtractI32(HeadRefVertexMask, 1, PrevBitIndex);
		const int32 NumPrevNewVerticesInTriangle = IsPrevStart & (3u - ((BitFieldExtractU32(LMask, 1, PrevBitIndex) << 1) | BitFieldExtractU32(WMask, 1, PrevBitIndex)));
		OutIndices[1] = BaseVertex + (IsPrevHeadRef & (NumPrevNewVerticesInTriangle - (IndexData & 31u)));
		OutIndices[2] = NumPrevNewVertices + (IsRef & (-1 - BitFieldExtractU32(IndexData, 5, 5)));

		const uint32 SearchMask = SMask | (LMask ^ IsLeft);
		const uint32 FoundBitIndex = FMath::FloorLog2(SearchMask & PrevBitsMask);
		const int32 IsFoundCaseS = BitFieldExtractI32(SMask, 1, FoundBitIndex);
		const uint32 FoundPrevBitsMask = (1u << FoundBitIndex) - 1u;
		const int32 FoundCurrentDwordNumPrevRefVertices = int32((FMath::CountBits(SLMask & FoundPrevBitsMask) << 1) + FMath::CountBits(WMask & FoundPrevBitsMask));
		const int32 FoundCurrentDwordNumPrevNewVertices = int32(FMath::CountBits(SMask & FoundPrevBitsMask) << 1) + int32(FoundBitIndex) - FoundCurrentDwordNumPrevRefVertices;
		const int32 FoundNumPrevNewVertices = NumPrevNewVerticesBeforeDword + FoundCurrentDwordNumPrevNewVertices;
		const int32 FoundNumPrevRefVertices = NumPrevRefVerticesBeforeDword + FoundCurrentDwordNumPrevRefVertices;
		const uint32 FoundNumRefVertices = (BitFieldExtractU32(LMask, 1, FoundBitIndex) << 1) + BitFieldExtractU32(WMask, 1, FoundBitIndex);
		const uint32 IsBeforeFoundRefVertex = BitFieldExtractU32(HeadRefVertexMask, 1, FoundBitIndex - 1);
		const int32 ReadOffset = IsFoundCaseS ? IsLeft : 1;
		const uint32 FoundIndexData = ReadUnalignedDword(StripIndexData, (FoundNumPrevRefVertices - ReadOffset) * 5);
		const uint32 FoundIndex = (FoundNumPrevNewVertices - 1u) - BitFieldExtractU32(FoundIndexData, 5, 0);
		const bool bCondition = IsFoundCaseS ? (int32(FoundNumRefVertices) >= 1 - IsLeft) : (IsBeforeFoundRefVertex != 0u);
		const int32 FoundNewVertex = FoundNumPrevNewVertices + (IsFoundCaseS ? (IsLeft & (FoundNumRefVertices == 0)) : -1);
		OutIndices[0] = bCondition ? FoundIndex : FoundNewVertex;
		if (IsLeft)
		{
			Swap(OutIndices[1], OutIndices[2]);
		}
	}

	/** Reads a cluster's byte streams in the order the encoder wrote them */
	struct FByteStreamReader
	{
		const uint8* Low = nullptr;
		const uint8* Mid = nullptr;
		const uint8* High = nullptr;

		int32 ReadZigZag(uint32 NumBytes)
		{
			uint32 Value = 0;
			if (NumBytes >= 3) { Value |= uint32(*High++) << 16; }
			if (NumBytes >= 2) { Value |= uint32(*Mid++) << 8; }
			if (NumBytes >= 1) { Value |= uint32(*Low++); }
			return DecodeZigZag(Value);
		}
	};

	uint32 WrapAdd(uint32 Prev, int32 Delta, uint32 NumBits)
	{
		return NumBits == 0 ? 0u : (uint32(int32(Prev) + Delta) & ((1u << NumBits) - 1u));
	}
}

FVoxelNaniteValidation ValidateVoxelNaniteMesh(const Nanite::FResources& Resources, TConstArrayView<FVoxelLocalVertex> InVertices,
	TConstArrayView<uint32> Indices, const FVoxelNaniteEncodeSettings& Settings)
{
	FVoxelNaniteValidation Result;
	const auto Fail = [&Result](const FString& Error) -> FVoxelNaniteValidation
	{
		Result.bValid = false;
		Result.Error = Error;
		return Result;
	};

	// The input triangles the encoder kept, in order
	TArray<uint32> Triangles;
	for (int32 t = 0; t + 2 < Indices.Num(); t += 3)
	{
		if (Indices[t] != Indices[t + 1] && Indices[t + 1] != Indices[t + 2] && Indices[t] != Indices[t + 2])
		{
			Triangles.Add(Indices[t]);
			Triangles.Add(Indices[t + 1]);
			Triangles.Add(Indices[t + 2]);
		}
	}

	if (Resources.NumRootPages != uint32(Resources.PageStreamingStates.Num()) || Resources.HasStreamingData())
	{
		return Fail(TEXT("not every page is a root page"));
	}
	if (Resources.HierarchyRootOffsets.Num() != 1 || Resources.HierarchyRootOffsets[0] != 0)
	{
		return Fail(TEXT("one hierarchy root at node 0 expected"));
	}

	// The hierarchy's leaf slots, reached from the root
	const TArray<Nanite::FPackedHierarchyNode>& Nodes = Resources.HierarchyNodes;
	TSet<uint32> ReachedLeafSlots;
	{
		TArray<uint32> Stack = { 0u };
		TSet<uint32> Visited;
		while (Stack.Num() > 0)
		{
			const uint32 NodeIndex = Stack.Pop();
			if (NodeIndex >= uint32(Nodes.Num()) || Visited.Contains(NodeIndex))
			{
				return Fail(FString::Printf(TEXT("hierarchy node %u out of range or reached twice"), NodeIndex));
			}
			Visited.Add(NodeIndex);
			for (uint32 Slot = 0; Slot < NANITE_MAX_BVH_NODE_FANOUT; ++Slot)
			{
				const bool bLeaf = Nodes[NodeIndex].Misc2[Slot].ResourcePageRangeKey != 0xFFFFFFFFu;
				if (!bLeaf)
				{
					Stack.Add(Nodes[NodeIndex].Misc1[Slot].ChildStartReference);
				}
				else if (Nodes[NodeIndex].Misc2[Slot].ResourcePageRangeKey != NANITE_PAGE_RANGE_KEY_EMPTY_RANGE)
				{
					ReachedLeafSlots.Add(PackHierarchyLocation(NodeIndex, Slot));
				}
			}
		}
	}

	const float Step = FMath::Exp2(-float(Resources.PositionPrecision));
	TSet<uint32> FixedUpSlots;
	int32 TriangleCursor = 0;
	for (int32 PageIndex = 0; PageIndex < Resources.PageStreamingStates.Num(); ++PageIndex)
	{
		const Nanite::FPageStreamingState& State = Resources.PageStreamingStates[PageIndex];
		if (State.BulkOffset + State.BulkSize > uint32(Resources.RootData.Num()))
		{
			return Fail(FString::Printf(TEXT("page %d outside the root data"), PageIndex));
		}
		const uint8* Bulk = Resources.RootData.GetData() + State.BulkOffset;
		FFixupHeader Fixup;
		FMemory::Memcpy(&Fixup, Bulk, sizeof(Fixup));
		const uint32 FixupSize = GetFixupChunkSize(Fixup.NumGroupFixups, Fixup.NumPartFixups, Fixup.NumParentFixups, Fixup.NumHierarchyLocations,
			Fixup.NumReconsiderPages, Fixup.NumClusterIndices);
		if (Fixup.Magic != NANITE_FIXUP_MAGIC || FixupSize + State.PageSize != State.BulkSize)
		{
			return Fail(FString::Printf(TEXT("page %d: bad fixup chunk"), PageIndex));
		}
		// The fixup's hierarchy locations: each a leaf slot the traversal reaches, of this page, reached once
		const uint8* Parts = Bulk + sizeof(FFixupHeader) + Fixup.NumGroupFixups * sizeof(FFixupGroup);
		const uint8* Locations = Parts + Fixup.NumPartFixups * sizeof(FFixupPart) + Fixup.NumParentFixups * sizeof(FFixupParent);
		for (uint32 PartIndex = 0; PartIndex < Fixup.NumPartFixups; ++PartIndex)
		{
			FFixupPart Part;
			FMemory::Memcpy(&Part, Parts + PartIndex * sizeof(FFixupPart), sizeof(Part));
			for (uint32 l = 0; l < Part.NumHierarchyLocations; ++l)
			{
				uint32 Location;
				FMemory::Memcpy(&Location, Locations + (Part.FirstHierarchyLocation + l) * 4, 4);
				const uint32 Node = Location >> NANITE_MAX_BVH_NODE_FANOUT_BITS;
				const uint32 Slot = Location & NANITE_MAX_BVH_NODE_FANOUT_MASK;
				if (!ReachedLeafSlots.Contains(Location) || FixedUpSlots.Contains(Location) || Part.PageIndex != PageIndex
					|| Nanite::FPageRangeKey(Nodes[Node].Misc2[Slot].ResourcePageRangeKey).GetStartIndex() != uint32(PageIndex))
				{
					return Fail(FString::Printf(TEXT("page %d: hierarchy location %u/%u not a leaf slot of this page, or fixed up twice"), PageIndex, Node, Slot));
				}
				FixedUpSlots.Add(Location);
			}
		}

		// The page itself, padded so the strip decode's dword reads stay inside
		TArray<uint8> Page;
		Page.Append(Bulk + FixupSize, State.PageSize);
		Page.AddZeroed(16);
		FPageDiskHeader DiskHeader;
		FMemory::Memcpy(&DiskHeader, Page.GetData(), sizeof(DiskHeader));
		const uint32 NumClusters = DiskHeader.NumClusters;
		if (NumClusters == 0 || NumClusters > NANITE_ROOT_PAGE_MAX_CLUSTERS || NumClusters != Fixup.NumClusters || DiskHeader.NumVertexRefs != 0)
		{
			return Fail(FString::Printf(TEXT("page %d: %u clusters, %u vertex refs"), PageIndex, NumClusters, DiskHeader.NumVertexRefs));
		}
		TArray<FClusterDiskHeader> ClusterHeaders;
		ClusterHeaders.SetNumUninitialized(NumClusters);
		FMemory::Memcpy(ClusterHeaders.GetData(), Page.GetData() + sizeof(FPageDiskHeader), NumClusters * sizeof(FClusterDiskHeader));
		const uint32 RawStart = sizeof(FPageDiskHeader) + NumClusters * sizeof(FClusterDiskHeader);
		FPageGPUHeader GPUHeader;
		FMemory::Memcpy(&GPUHeader, Page.GetData() + RawStart, sizeof(GPUHeader));
		if ((GPUHeader.NumClusters_MaxClusterBoneInfluences_MaxVoxelBoneInfluences & 0xFFFFu) != NumClusters)
		{
			return Fail(FString::Printf(TEXT("page %d: GPU header cluster count"), PageIndex));
		}

		uint32 GpuSize = DiskHeader.NumRawFloat4s * 16;
		for (uint32 c = 0; c < NumClusters; ++c)
		{
			Nanite::FPackedCluster Packed;
			for (uint32 Float4 = 0; Float4 < NANITE_NUM_PACKED_CLUSTER_FLOAT4S; ++Float4)
			{
				FMemory::Memcpy(reinterpret_cast<uint8*>(&Packed) + Float4 * 16, Page.GetData() + RawStart + sizeof(FPageGPUHeader) + (Float4 * NumClusters + c) * 16, 16);
			}
			const FClusterDiskHeader& Header = ClusterHeaders[c];
			const uint32 NumVerts = Packed.GetNumVerts();
			const uint32 NumTris = Packed.GetNumTris();
			const uint32 PosBits[3] = { Packed.GetPosBitsX(), Packed.GetPosBitsY(), Packed.GetPosBitsZ() };
			const uint32 NormalPrecision = Packed.GetNormalPrecision();
			const uint32 TangentPrecision = Packed.GetTangentPrecision();
			const uint32 NumUVs = Nanite::GetBits(Packed.DecodeInfoOffset_HasTangents_Skinning_NumUVs_ColorMode, 3, 24);
			const uint32 ColorMode = Nanite::GetBits(Packed.DecodeInfoOffset_HasTangents_Skinning_NumUVs_ColorMode, 1, 27);
			const bool bHasTangents = Nanite::GetBits(Packed.DecodeInfoOffset_HasTangents_Skinning_NumUVs_ColorMode, 1, 22) != 0;
			if (NumUVs != NumTexCoords || !bHasTangents || Packed.GetPosPrecision() != Resources.PositionPrecision || NumTris == 0 || NumVerts == 0)
			{
				return Fail(FString::Printf(TEXT("page %d cluster %u: unexpected format"), PageIndex, c));
			}
			const uint32 BitsPerTriangle = Packed.GetBitsPerIndex() + 10;
			GpuSize += (NumTris * BitsPerTriangle + 31) / 32 * 4 + (NumVerts * (PosBits[0] + PosBits[1] + PosBits[2]) + 31) / 32 * 4
				+ (NumVerts * Packed.GetBitsPerAttribute() + 31) / 32 * 4;

			// Vertices
			FByteStreamReader Reader{ Page.GetData() + Header.LowBytesOffset, Page.GetData() + Header.MidBytesOffset, Page.GetData() + Header.HighBytesOffset };
			const uint32 BytesPerPosition = (FMath::Max3(PosBits[0], PosBits[1], PosBits[2]) + 7) / 8;
			TArray<FVector3f, TInlineAllocator<NANITE_MAX_CLUSTER_VERTICES>> Positions;
			uint32 Prev[3] = { (1u << PosBits[0]) >> 1, (1u << PosBits[1]) >> 1, (1u << PosBits[2]) >> 1 };
			for (uint32 v = 0; v < NumVerts; ++v)
			{
				FVector3f Position;
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					Prev[Axis] = WrapAdd(Prev[Axis], Reader.ReadZigZag(BytesPerPosition), PosBits[Axis]);
					Position[Axis] = float(Packed.PosStart[Axis] + int32(Prev[Axis])) * Step;
				}
				Positions.Add(Position);
			}
			TArray<FVector3f, TInlineAllocator<NANITE_MAX_CLUSTER_VERTICES>> Normals;
			{
				const uint32 BytesPerNormal = (NormalPrecision + 7) / 8;
				uint32 PrevX = 0, PrevY = 0;
				for (uint32 v = 0; v < NumVerts; ++v)
				{
					PrevX = WrapAdd(PrevX, Reader.ReadZigZag(BytesPerNormal), NormalPrecision);
					PrevY = WrapAdd(PrevY, Reader.ReadZigZag(BytesPerNormal), NormalPrecision);
					Normals.Add(UnpackNormal((PrevY << NormalPrecision) | PrevX, NormalPrecision));
				}
			}
			TArray<FVector3f, TInlineAllocator<NANITE_MAX_CLUSTER_VERTICES>> Tangents;
			TArray<bool, TInlineAllocator<NANITE_MAX_CLUSTER_VERTICES>> TangentSigns;
			{
				const uint32 BytesPerTangent = (TangentPrecision + 1 + 7) / 8;
				uint32 PrevTangent = 0;
				for (uint32 v = 0; v < NumVerts; ++v)
				{
					PrevTangent = WrapAdd(PrevTangent, Reader.ReadZigZag(BytesPerTangent), TangentPrecision + 1);
					Tangents.Add(UnpackTangent(PrevTangent & ((1u << TangentPrecision) - 1u), Normals[v], TangentPrecision));
					TangentSigns.Add((PrevTangent >> TangentPrecision) != 0);
				}
			}
			TArray<FColor, TInlineAllocator<NANITE_MAX_CLUSTER_VERTICES>> Colors;
			{
				const uint32 ColorMin[4] = { Packed.ColorMin & 0xFF, (Packed.ColorMin >> 8) & 0xFF, (Packed.ColorMin >> 16) & 0xFF, Packed.ColorMin >> 24 };
				const uint32 ColorBits[4] = { Nanite::GetBits(Packed.ColorBits_GroupIndex, 4, 0), Nanite::GetBits(Packed.ColorBits_GroupIndex, 4, 4),
					Nanite::GetBits(Packed.ColorBits_GroupIndex, 4, 8), Nanite::GetBits(Packed.ColorBits_GroupIndex, 4, 12) };
				uint32 PrevColor[4] = { 0, 0, 0, 0 };
				for (uint32 v = 0; v < NumVerts; ++v)
				{
					if (ColorMode == NANITE_VERTEX_COLOR_MODE_VARIABLE)
					{
						for (int32 Channel = 0; Channel < 4; ++Channel)
						{
							PrevColor[Channel] = WrapAdd(PrevColor[Channel], Reader.ReadZigZag(1), ColorBits[Channel]);
						}
					}
					Colors.Add(FColor(uint8(ColorMin[0] + PrevColor[0]), uint8(ColorMin[1] + PrevColor[1]), uint8(ColorMin[2] + PrevColor[2]), uint8(ColorMin[3] + PrevColor[3])));
				}
			}
			TArray<FVector2f, TInlineAllocator<NANITE_MAX_CLUSTER_VERTICES>> UVs[NumTexCoords];
			for (uint32 UVIndex = 0; UVIndex < NumTexCoords; ++UVIndex)
			{
				FPackedUVHeader UVHeader;
				FMemory::Memcpy(&UVHeader, Page.GetData() + Header.DecodeInfoOffset + UVIndex * sizeof(FPackedUVHeader), sizeof(UVHeader));
				const uint32 Min[2] = { UVHeader.Data[0] >> 5, UVHeader.Data[1] >> 5 };
				const uint32 Bits[2] = { UVHeader.Data[0] & 31u, UVHeader.Data[1] & 31u };
				const uint32 BytesPerUV = (FMath::Max(Bits[0], Bits[1]) + 7) / 8;
				uint32 PrevU = 0, PrevV = 0;
				for (uint32 v = 0; v < NumVerts; ++v)
				{
					PrevU = WrapAdd(PrevU, Reader.ReadZigZag(BytesPerUV), Bits[0]);
					PrevV = WrapAdd(PrevV, Reader.ReadZigZag(BytesPerUV), Bits[1]);
					UVs[UVIndex].Add(FVector2f(DecodeUVFloat(Min[0] + PrevU, UVMantissaBits), DecodeUVFloat(Min[1] + PrevV, UVMantissaBits)));
				}
			}

			// Triangles
			uint32 Bitmasks[4][3];
			FMemory::Memcpy(Bitmasks, Page.GetData() + DiskHeader.StripBitmaskOffset + c * sizeof(Bitmasks), sizeof(Bitmasks));
			for (uint32 t = 0; t < NumTris; ++t)
			{
				uint32 Decoded[3];
				UnpackTriangleIndices(Bitmasks, Header.NumPrevRefVerticesBeforeDwords, Header.NumPrevNewVerticesBeforeDwords,
					Page.GetData() + Header.IndexDataOffset, t, Decoded);
				const uint32 MaxIndex = FMath::Max3(Decoded[0], Decoded[1], Decoded[2]);
				const uint32 MinIndex = FMath::Min3(Decoded[0], Decoded[1], Decoded[2]);
				if (MaxIndex >= NumVerts || MaxIndex - MinIndex > 31)
				{
					return Fail(FString::Printf(TEXT("page %d cluster %u triangle %u: indices %u %u %u (of %u)"), PageIndex, c, t, Decoded[0], Decoded[1], Decoded[2], NumVerts));
				}
				if (TriangleCursor + 2 >= Triangles.Num())
				{
					return Fail(TEXT("more triangles decoded than encoded"));
				}
				// The same triangle, in some rotation that keeps the winding
				const FVoxelLocalVertex* Source[3] = { &InVertices[Triangles[TriangleCursor]], &InVertices[Triangles[TriangleCursor + 1]], &InVertices[Triangles[TriangleCursor + 2]] };
				int32 BestRotation = 0;
				float BestError = MAX_flt;
				for (int32 Rotation = 0; Rotation < 3; ++Rotation)
				{
					float Error = 0.0f;
					for (int32 Corner = 0; Corner < 3; ++Corner)
					{
						Error += (Positions[Decoded[Corner]] - Source[(Rotation + Corner) % 3]->Position).SizeSquared();
					}
					if (Error < BestError)
					{
						BestError = Error;
						BestRotation = Rotation;
					}
				}
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const uint32 D = Decoded[Corner];
					const FSourceVertex S = ToSourceVertex(*Source[(BestRotation + Corner) % 3]);
					Result.MaxPositionError = FMath::Max(Result.MaxPositionError, (Positions[D] - S.Position).Size());
					Result.MinNormalDot = FMath::Min(Result.MinNormalDot, Normals[D] | S.Normal);
					if (S.TangentX.SizeSquared() > 1e-4f)
					{
						// The input tangent against the decoded normal, as the encoder measured it
						const FVector3f Expected = (S.TangentX - Normals[D] * (S.TangentX | Normals[D])).GetSafeNormal();
						if (!Expected.IsNearlyZero())
						{
							Result.MinTangentDot = FMath::Min(Result.MinTangentDot, Tangents[D] | Expected);
						}
					}
					if (Colors[D] != S.Color || TangentSigns[D] != S.bTangentYNegative)
					{
						Result.ColorMismatches++;
					}
					Result.MaxUV0Error = FMath::Max(Result.MaxUV0Error, (UVs[0][D] - S.UVs[0]).GetAbsMax());
					Result.MaxUV1Error = FMath::Max(Result.MaxUV1Error, (UVs[1][D] - S.UVs[1]).GetAbsMax());
				}
				TriangleCursor += 3;
				Result.NumDecodedTriangles++;
			}
		}
		if (GpuSize > NANITE_ROOT_PAGE_GPU_SIZE)
		{
			return Fail(FString::Printf(TEXT("page %d: %u bytes on the GPU, over a root page"), PageIndex, GpuSize));
		}
	}

	if (TriangleCursor != Triangles.Num())
	{
		return Fail(FString::Printf(TEXT("%d triangles decoded, %d encoded"), TriangleCursor / 3, Triangles.Num() / 3));
	}
	if (FixedUpSlots.Num() != ReachedLeafSlots.Num() || FixedUpSlots.Num() != int32(Resources.NumClusters))
	{
		return Fail(FString::Printf(TEXT("%d leaf slots reached, %d fixed up, %u clusters"), ReachedLeafSlots.Num(), FixedUpSlots.Num(), Resources.NumClusters));
	}

	// Within the encodings' precision: half a step an axis; 8-bit octahedral normals; 7-bit tangent angles; UV0 at 14
	// mantissa bits; colours, tangent signs and UV1 (material and face type, whole numbers) exact
	const float MaxPositionError = 0.5f * Step * UE_SQRT_3 * 1.01f + 1e-3f;
	if (Result.MaxPositionError > MaxPositionError)
	{
		return Fail(FString::Printf(TEXT("position error %.4f uu over %.4f"), Result.MaxPositionError, MaxPositionError));
	}
	if (Result.MinNormalDot < 0.995f || Result.MinTangentDot < 0.99f)
	{
		return Fail(FString::Printf(TEXT("normal dot %.4f, tangent dot %.4f"), Result.MinNormalDot, Result.MinTangentDot));
	}
	if (Result.ColorMismatches > 0 || Result.MaxUV1Error > 0.0f)
	{
		return Fail(FString::Printf(TEXT("%d colour or tangent sign mismatches, UV1 error %f"), Result.ColorMismatches, Result.MaxUV1Error));
	}
	Result.bValid = true;
	return Result;
}
