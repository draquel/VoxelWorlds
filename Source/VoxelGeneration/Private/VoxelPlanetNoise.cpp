// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelPlanetNoise.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarContinuousSimplex(
	TEXT("voxel.Noise.ContinuousSimplex"),
	1,
	TEXT("The Simplex noise's corner kernel, every world mode (world modes review G43 for planets, MP4 for heightmap worlds): ")
	TEXT("1 = continuous at the simplex boundaries ((0.6 - d^2)^4 less its value and slope at d^2 = 0.5; PlanetGenerationVersion 6, ")
	TEXT("GenerationVersion 3); 0 = the 0.6 kernel as before, which steps where a sample crosses into the next simplex (up to 136 uu ")
	TEXT("along Review_Planet_L's cube face edges, about a quarter voxel on a heightmap world). For A/B only: 0 generates terrain ")
	TEXT("that current saves do not match. Read when chunks generate; CPU and GPU alike. (voxel.Planet.ContinuousSimplex before MP4.)"),
	ECVF_Default);

bool FVoxelPlanetNoise::IsContinuousSimplex()
{
	return CVarContinuousSimplex.GetValueOnAnyThread() != 0;
}

// Spherical Phase 6, WM-BP5 (D22); every world mode's noise since the world modes review's MP4 (D39). Every function here
// has a twin in Shaders/Private/PlanetNoise.ush with the same float operations in the same order; change both together
// (VoxelWorlds.Generation.GPUvsCPU*Parity).

namespace VoxelPlanetNoiseDetail
{
	// Gradient vectors for Simplex noise (FVoxelCPUNoiseGenerator's set)
	constexpr int32 Grad3[12][3] = {
		{1,1,0}, {-1,1,0}, {1,-1,0}, {-1,-1,0},
		{1,0,1}, {-1,0,1}, {1,0,-1}, {-1,0,-1},
		{0,1,1}, {0,-1,1}, {0,1,-1}, {0,-1,-1}
	};

	constexpr float SkewF3 = 1.0f / 3.0f;
	constexpr float UnskewG3 = 1.0f / 6.0f;

	/** The hash as a float in [0, 1): its top 24 bits. */
	FORCEINLINE float Unit(uint32 H)
	{
		return static_cast<float>(H >> 8) * (1.0f / 16777216.0f);
	}

	FORCEINLINE double UnitD(uint32 H)
	{
		return static_cast<double>(H >> 8) * (1.0 / 16777216.0);
	}

	/** A double lattice coordinate's 32 bits (the hash wraps mod 2^32). */
	FORCEINLINE uint32 Wrap(double Floored)
	{
		return static_cast<uint32>(static_cast<int64>(Floored));
	}

	template <typename T>
	FORCEINLINE T Fade(T X)
	{
		return X * X * X * (X * (X * T(6) - T(15)) + T(10));
	}

	template <typename T>
	FORCEINLINE T Lerp(T A, T B, T Alpha)
	{
		return A + Alpha * (B - A);
	}

	/** Ken Perlin's improved-noise gradient from the hash's low 4 bits (the classic table noise's Grad). */
	template <typename T>
	FORCEINLINE T Grad(uint32 Hash, T X, T Y, T Z)
	{
		const uint32 H = Hash & 15u;
		const T U = H < 8u ? X : Y;
		const T V = H < 4u ? Y : (H == 12u || H == 14u ? X : Z);
		return ((H & 1u) == 0u ? U : -U) + ((H & 2u) == 0u ? V : -V);
	}

	/** The simplex corner order from the unskewed position in the cell (the classic table). */
	template <typename T>
	FORCEINLINE void SimplexCorners(T X0, T Y0, T Z0, int32& I1, int32& J1, int32& K1, int32& I2, int32& J2, int32& K2)
	{
		if (X0 >= Y0)
		{
			if (Y0 >= Z0) { I1 = 1; J1 = 0; K1 = 0; I2 = 1; J2 = 1; K2 = 0; }
			else if (X0 >= Z0) { I1 = 1; J1 = 0; K1 = 0; I2 = 1; J2 = 0; K2 = 1; }
			else { I1 = 0; J1 = 0; K1 = 1; I2 = 1; J2 = 0; K2 = 1; }
		}
		else
		{
			if (Y0 < Z0) { I1 = 0; J1 = 0; K1 = 1; I2 = 0; J2 = 1; K2 = 1; }
			else if (X0 < Z0) { I1 = 0; J1 = 1; K1 = 0; I2 = 0; J2 = 1; K2 = 1; }
			else { I1 = 0; J1 = 1; K1 = 0; I2 = 1; J2 = 1; K2 = 0; }
		}
	}

	/**
	 * A simplex corner's contribution. G43: the classic kernel (0.6 - d^2)^4 reaches past the simplex (a point's nearest
	 * corner outside its simplex is sqrt(0.5) away), so a corner's contribution dropped out abruptly where a sample crossed
	 * into the next simplex: steps in the terrain, every octave's at once on the planes x = y, y = z and x = z (the cube
	 * sphere's face edges). The continuous kernel is that one less its value and slope at d^2 = 0.5, so it reaches zero
	 * there with a zero slope: v^2 (v^2 + 0.4 v + 0.06) with v = 0.5 - d^2. It stays within 1.2% rms of the old noise
	 * (SimplexScale keeps its spread), so the landscape is the same.
	 */
	template <typename T>
	FORCEINLINE T SimplexCorner(uint32 Hash, T X, T Y, T Z, bool bContinuous)
	{
		const int32* G = Grad3[Hash % 12u];
		if (bContinuous)
		{
			const T V = T(0.5) - X * X - Y * Y - Z * Z;
			if (V <= T(0))
			{
				return T(0);
			}
			const T W = V * V * ((V + T(0.4)) * V + T(0.06));
			return W * (T(G[0]) * X + T(G[1]) * Y + T(G[2]) * Z);
		}
		T R = T(0.6) - X * X - Y * Y - Z * Z;
		if (R < T(0))
		{
			return T(0);
		}
		R *= R;
		return R * R * (T(G[0]) * X + T(G[1]) * Y + T(G[2]) * Z);
	}

	/** The corner sum's scale: 32 for the classic kernel; 33.25 gives the continuous one the same spread. */
	FORCEINLINE float SimplexScale(bool bContinuous)
	{
		return bContinuous ? 33.25f : 32.0f;
	}
}

FVoxelNoiseSplit FVoxelPlanetNoise::Split(const FVector& NoisePoint, EVoxelNoiseType Type)
{
	FVector Q = NoisePoint;
	if (Type == EVoxelNoiseType::Simplex)
	{
		// Skew in double: the lattice cell is found in the skewed space, as Simplex3D does.
		const double S = (Q.X + Q.Y + Q.Z) / 3.0;
		Q += FVector(S, S, S);
	}
	const double FX = FMath::FloorToDouble(Q.X);
	const double FY = FMath::FloorToDouble(Q.Y);
	const double FZ = FMath::FloorToDouble(Q.Z);
	FVoxelNoiseSplit Out;
	Out.Cell = FIntVector(
		static_cast<int32>(VoxelPlanetNoiseDetail::Wrap(FX)),
		static_cast<int32>(VoxelPlanetNoiseDetail::Wrap(FY)),
		static_cast<int32>(VoxelPlanetNoiseDetail::Wrap(FZ)));
	Out.Frac = FVector3f(static_cast<float>(Q.X - FX), static_cast<float>(Q.Y - FY), static_cast<float>(Q.Z - FZ));
	return Out;
}

float FVoxelPlanetNoise::Simplex(const FVoxelNoiseSplit& Origin, const FVector3f& Offset, int32 Seed)
{
	using namespace VoxelPlanetNoiseDetail;

	// The skewed point within the origin's cell: the origin's skewed fraction plus the skewed offset.
	const float S = (Offset.X + Offset.Y + Offset.Z) * SkewF3;
	const float GX = Origin.Frac.X + (Offset.X + S);
	const float GY = Origin.Frac.Y + (Offset.Y + S);
	const float GZ = Origin.Frac.Z + (Offset.Z + S);
	const int32 CX = FMath::FloorToInt32(GX);
	const int32 CY = FMath::FloorToInt32(GY);
	const int32 CZ = FMath::FloorToInt32(GZ);
	const float FX = GX - static_cast<float>(CX);
	const float FY = GY - static_cast<float>(CY);
	const float FZ = GZ - static_cast<float>(CZ);

	// Unskew the fraction: the sample's offset from the cell's first corner.
	const float T = (FX + FY + FZ) * UnskewG3;
	const float X0 = FX - T;
	const float Y0 = FY - T;
	const float Z0 = FZ - T;

	int32 I1, J1, K1, I2, J2, K2;
	SimplexCorners(X0, Y0, Z0, I1, J1, K1, I2, J2, K2);

	const float X1 = X0 - static_cast<float>(I1) + UnskewG3;
	const float Y1 = Y0 - static_cast<float>(J1) + UnskewG3;
	const float Z1 = Z0 - static_cast<float>(K1) + UnskewG3;
	const float X2 = X0 - static_cast<float>(I2) + 2.0f * UnskewG3;
	const float Y2 = Y0 - static_cast<float>(J2) + 2.0f * UnskewG3;
	const float Z2 = Z0 - static_cast<float>(K2) + 2.0f * UnskewG3;
	const float X3 = X0 - 1.0f + 3.0f * UnskewG3;
	const float Y3 = Y0 - 1.0f + 3.0f * UnskewG3;
	const float Z3 = Z0 - 1.0f + 3.0f * UnskewG3;

	const uint32 BX = static_cast<uint32>(Origin.Cell.X) + static_cast<uint32>(CX);
	const uint32 BY = static_cast<uint32>(Origin.Cell.Y) + static_cast<uint32>(CY);
	const uint32 BZ = static_cast<uint32>(Origin.Cell.Z) + static_cast<uint32>(CZ);

	const bool bContinuous = IsContinuousSimplex();
	const float N0 = SimplexCorner(LatticeHash(BX, BY, BZ, Seed), X0, Y0, Z0, bContinuous);
	const float N1 = SimplexCorner(LatticeHash(BX + I1, BY + J1, BZ + K1, Seed), X1, Y1, Z1, bContinuous);
	const float N2 = SimplexCorner(LatticeHash(BX + I2, BY + J2, BZ + K2, Seed), X2, Y2, Z2, bContinuous);
	const float N3 = SimplexCorner(LatticeHash(BX + 1u, BY + 1u, BZ + 1u, Seed), X3, Y3, Z3, bContinuous);
	return SimplexScale(bContinuous) * (N0 + N1 + N2 + N3);
}

float FVoxelPlanetNoise::Perlin(const FVoxelNoiseSplit& Origin, const FVector3f& Offset, int32 Seed)
{
	using namespace VoxelPlanetNoiseDetail;

	const float GX = Origin.Frac.X + Offset.X;
	const float GY = Origin.Frac.Y + Offset.Y;
	const float GZ = Origin.Frac.Z + Offset.Z;
	const int32 CX = FMath::FloorToInt32(GX);
	const int32 CY = FMath::FloorToInt32(GY);
	const int32 CZ = FMath::FloorToInt32(GZ);
	const float X = GX - static_cast<float>(CX);
	const float Y = GY - static_cast<float>(CY);
	const float Z = GZ - static_cast<float>(CZ);
	const uint32 BX = static_cast<uint32>(Origin.Cell.X) + static_cast<uint32>(CX);
	const uint32 BY = static_cast<uint32>(Origin.Cell.Y) + static_cast<uint32>(CY);
	const uint32 BZ = static_cast<uint32>(Origin.Cell.Z) + static_cast<uint32>(CZ);

	const float U = Fade(X);
	const float V = Fade(Y);
	const float W = Fade(Z);
	auto H = [&](uint32 DX, uint32 DY, uint32 DZ) { return LatticeHash(BX + DX, BY + DY, BZ + DZ, Seed); };

	return Lerp(
		Lerp(
			Lerp(Grad(H(0, 0, 0), X, Y, Z), Grad(H(1, 0, 0), X - 1.0f, Y, Z), U),
			Lerp(Grad(H(0, 1, 0), X, Y - 1.0f, Z), Grad(H(1, 1, 0), X - 1.0f, Y - 1.0f, Z), U), V),
		Lerp(
			Lerp(Grad(H(0, 0, 1), X, Y, Z - 1.0f), Grad(H(1, 0, 1), X - 1.0f, Y, Z - 1.0f), U),
			Lerp(Grad(H(0, 1, 1), X, Y - 1.0f, Z - 1.0f), Grad(H(1, 1, 1), X - 1.0f, Y - 1.0f, Z - 1.0f), U), V), W);
}

void FVoxelPlanetNoise::Cellular(const FVoxelNoiseSplit& Origin, const FVector3f& Offset, int32 Seed, float& OutF1, float& OutF2, float* OutCellID)
{
	using namespace VoxelPlanetNoiseDetail;

	const float GX = Origin.Frac.X + Offset.X;
	const float GY = Origin.Frac.Y + Offset.Y;
	const float GZ = Origin.Frac.Z + Offset.Z;
	const int32 CX = FMath::FloorToInt32(GX);
	const int32 CY = FMath::FloorToInt32(GY);
	const int32 CZ = FMath::FloorToInt32(GZ);
	const float FracX = GX - static_cast<float>(CX);
	const float FracY = GY - static_cast<float>(CY);
	const float FracZ = GZ - static_cast<float>(CZ);
	const uint32 BX = static_cast<uint32>(Origin.Cell.X) + static_cast<uint32>(CX);
	const uint32 BY = static_cast<uint32>(Origin.Cell.Y) + static_cast<uint32>(CY);
	const uint32 BZ = static_cast<uint32>(Origin.Cell.Z) + static_cast<uint32>(CZ);

	OutF1 = 100.0f;
	OutF2 = 100.0f;
	uint32 NearestX = BX, NearestY = BY, NearestZ = BZ;
	for (int32 DZ = -1; DZ <= 1; ++DZ)
	{
		for (int32 DY = -1; DY <= 1; ++DY)
		{
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				const uint32 NX = BX + static_cast<uint32>(DX);
				const uint32 NY = BY + static_cast<uint32>(DY);
				const uint32 NZ = BZ + static_cast<uint32>(DZ);
				// One feature point per cell, its offset from three hashes of the cell
				const float DeltaX = static_cast<float>(DX) + Unit(LatticeHash(NX, NY, NZ, Seed)) - FracX;
				const float DeltaY = static_cast<float>(DY) + Unit(LatticeHash(NX, NY, NZ, Seed + 1)) - FracY;
				const float DeltaZ = static_cast<float>(DZ) + Unit(LatticeHash(NX, NY, NZ, Seed + 2)) - FracZ;
				const float DistSq = DeltaX * DeltaX + DeltaY * DeltaY + DeltaZ * DeltaZ;
				if (DistSq < OutF1)
				{
					OutF2 = OutF1;
					OutF1 = DistSq;
					NearestX = NX;
					NearestY = NY;
					NearestZ = NZ;
				}
				else if (DistSq < OutF2)
				{
					OutF2 = DistSq;
				}
			}
		}
	}
	OutF1 = FMath::Sqrt(OutF1);
	OutF2 = FMath::Sqrt(OutF2);
	if (OutCellID)
	{
		*OutCellID = Unit(LatticeHash(NearestX, NearestY, NearestZ, Seed + 12345));
	}
}

float FVoxelPlanetNoise::Sample(const FVoxelNoiseSplit& Origin, const FVector3f& Offset, EVoxelNoiseType Type, int32 Seed)
{
	switch (Type)
	{
	case EVoxelNoiseType::Perlin:
		return Perlin(Origin, Offset, Seed);
	case EVoxelNoiseType::Cellular:
	{
		float F1, F2;
		Cellular(Origin, Offset, Seed, F1, F2);
		return F1 * 2.0f - 1.0f;
	}
	case EVoxelNoiseType::Voronoi:
	{
		float F1, F2;
		Cellular(Origin, Offset, Seed, F1, F2);
		return (F2 - F1) * 2.0f - 1.0f;
	}
	case EVoxelNoiseType::Simplex:
	default:
		return Simplex(Origin, Offset, Seed);
	}
}

int32 FVoxelPlanetNoise::OctaveFrequencies(const FVoxelNoiseParams& Params, float* OutFrequencies)
{
	const int32 Count = FMath::Clamp(Params.Octaves, 1, MaxOctaves);
	float Frequency = Params.Frequency;
	for (int32 i = 0; i < Count; ++i)
	{
		OutFrequencies[i] = Frequency;
		Frequency *= Params.Lacunarity;
	}
	return Count;
}

float FVoxelPlanetNoise::FBM3D(const FVector& Position, const FVoxelNoiseParams& Params)
{
	float Frequencies[MaxOctaves];
	const int32 Count = OctaveFrequencies(Params, Frequencies);
	float Total = 0.0f;
	float Amplitude = Params.Amplitude;
	float MaxValue = 0.0f;
	for (int32 i = 0; i < Count; ++i)
	{
		const FVoxelNoiseSplit Point = Split(Position * static_cast<double>(Frequencies[i]), Params.NoiseType);
		Total += Sample(Point, FVector3f::ZeroVector, Params.NoiseType, Params.Seed) * Amplitude;
		MaxValue += Amplitude;
		Amplitude *= Params.Persistence;
	}
	return Total / MaxValue;
}

float FVoxelPlanetNoise::FBM3D(const FVector& Origin, const FVector3f& Offset, const FVoxelNoiseParams& Params)
{
	float Frequencies[MaxOctaves];
	FVoxelNoiseSplit Splits[MaxOctaves];
	const int32 Count = SplitOctaves(Origin, Params, Splits);
	OctaveFrequencies(Params, Frequencies);
	float Total = 0.0f;
	float Amplitude = Params.Amplitude;
	float MaxValue = 0.0f;
	for (int32 i = 0; i < Count; ++i)
	{
		Total += Sample(Splits[i], Offset * Frequencies[i], Params.NoiseType, Params.Seed) * Amplitude;
		MaxValue += Amplitude;
		Amplitude *= Params.Persistence;
	}
	return Total / MaxValue;
}

int32 FVoxelPlanetNoise::SplitOctaves(const FVector& Origin, const FVoxelNoiseParams& Params, FVoxelNoiseSplit* OutSplits)
{
	float Frequencies[MaxOctaves];
	const int32 Count = OctaveFrequencies(Params, Frequencies);
	for (int32 i = 0; i < Count; ++i)
	{
		OutSplits[i] = Split(Origin * static_cast<double>(Frequencies[i]), Params.NoiseType);
	}
	return Count;
}

FVoxelNoiseParams FVoxelPlanetNoise::MakeFieldParams(int32 Seed, float Frequency)
{
	FVoxelNoiseParams Params;
	Params.NoiseType = EVoxelNoiseType::Simplex;
	Params.Octaves = 2;
	Params.Persistence = 0.5f;
	Params.Lacunarity = 2.0f;
	Params.Amplitude = 1.0f;
	Params.Seed = Seed;
	Params.Frequency = Frequency;
	return Params;
}

// ==================== Double reference ====================

double FVoxelPlanetNoise::ReferenceSample(const FVector& P, EVoxelNoiseType Type, int32 Seed)
{
	using namespace VoxelPlanetNoiseDetail;

	if (Type == EVoxelNoiseType::Simplex || (Type != EVoxelNoiseType::Perlin && Type != EVoxelNoiseType::Cellular && Type != EVoxelNoiseType::Voronoi))
	{
		const double S = (P.X + P.Y + P.Z) / 3.0;
		const double QX = P.X + S, QY = P.Y + S, QZ = P.Z + S;
		const double CX = FMath::FloorToDouble(QX), CY = FMath::FloorToDouble(QY), CZ = FMath::FloorToDouble(QZ);
		const double FX = QX - CX, FY = QY - CY, FZ = QZ - CZ;
		const double T = (FX + FY + FZ) / 6.0;
		const double X0 = FX - T, Y0 = FY - T, Z0 = FZ - T;
		int32 I1, J1, K1, I2, J2, K2;
		SimplexCorners(X0, Y0, Z0, I1, J1, K1, I2, J2, K2);
		const double G = 1.0 / 6.0;
		const uint32 BX = Wrap(CX), BY = Wrap(CY), BZ = Wrap(CZ);
		const bool bContinuous = IsContinuousSimplex();
		const double N0 = SimplexCorner(LatticeHash(BX, BY, BZ, Seed), X0, Y0, Z0, bContinuous);
		const double N1 = SimplexCorner(LatticeHash(BX + I1, BY + J1, BZ + K1, Seed), X0 - I1 + G, Y0 - J1 + G, Z0 - K1 + G, bContinuous);
		const double N2 = SimplexCorner(LatticeHash(BX + I2, BY + J2, BZ + K2, Seed), X0 - I2 + 2.0 * G, Y0 - J2 + 2.0 * G, Z0 - K2 + 2.0 * G, bContinuous);
		const double N3 = SimplexCorner(LatticeHash(BX + 1u, BY + 1u, BZ + 1u, Seed), X0 - 1.0 + 3.0 * G, Y0 - 1.0 + 3.0 * G, Z0 - 1.0 + 3.0 * G, bContinuous);
		return static_cast<double>(SimplexScale(bContinuous)) * (N0 + N1 + N2 + N3);
	}

	const double CX = FMath::FloorToDouble(P.X), CY = FMath::FloorToDouble(P.Y), CZ = FMath::FloorToDouble(P.Z);
	const double X = P.X - CX, Y = P.Y - CY, Z = P.Z - CZ;
	const uint32 BX = Wrap(CX), BY = Wrap(CY), BZ = Wrap(CZ);
	if (Type == EVoxelNoiseType::Perlin)
	{
		const double U = Fade(X), V = Fade(Y), W = Fade(Z);
		auto H = [&](uint32 DX, uint32 DY, uint32 DZ) { return LatticeHash(BX + DX, BY + DY, BZ + DZ, Seed); };
		return Lerp(
			Lerp(
				Lerp(Grad(H(0, 0, 0), X, Y, Z), Grad(H(1, 0, 0), X - 1.0, Y, Z), U),
				Lerp(Grad(H(0, 1, 0), X, Y - 1.0, Z), Grad(H(1, 1, 0), X - 1.0, Y - 1.0, Z), U), V),
			Lerp(
				Lerp(Grad(H(0, 0, 1), X, Y, Z - 1.0), Grad(H(1, 0, 1), X - 1.0, Y, Z - 1.0), U),
				Lerp(Grad(H(0, 1, 1), X, Y - 1.0, Z - 1.0), Grad(H(1, 1, 1), X - 1.0, Y - 1.0, Z - 1.0), U), V), W);
	}

	double F1 = 100.0, F2 = 100.0;
	for (int32 DZ = -1; DZ <= 1; ++DZ)
	for (int32 DY = -1; DY <= 1; ++DY)
	for (int32 DX = -1; DX <= 1; ++DX)
	{
		const uint32 NX = BX + static_cast<uint32>(DX), NY = BY + static_cast<uint32>(DY), NZ = BZ + static_cast<uint32>(DZ);
		const double DeltaX = DX + UnitD(LatticeHash(NX, NY, NZ, Seed)) - X;
		const double DeltaY = DY + UnitD(LatticeHash(NX, NY, NZ, Seed + 1)) - Y;
		const double DeltaZ = DZ + UnitD(LatticeHash(NX, NY, NZ, Seed + 2)) - Z;
		const double DistSq = DeltaX * DeltaX + DeltaY * DeltaY + DeltaZ * DeltaZ;
		if (DistSq < F1) { F2 = F1; F1 = DistSq; }
		else if (DistSq < F2) { F2 = DistSq; }
	}
	F1 = FMath::Sqrt(F1);
	F2 = FMath::Sqrt(F2);
	return Type == EVoxelNoiseType::Cellular ? F1 * 2.0 - 1.0 : (F2 - F1) * 2.0 - 1.0;
}

double FVoxelPlanetNoise::ReferenceFBM3D(const FVector& Position, const FVoxelNoiseParams& Params)
{
	float Frequencies[MaxOctaves];
	const int32 Count = OctaveFrequencies(Params, Frequencies);
	double Total = 0.0;
	double Amplitude = Params.Amplitude;
	double MaxValue = 0.0;
	for (int32 i = 0; i < Count; ++i)
	{
		Total += ReferenceSample(Position * static_cast<double>(Frequencies[i]), Params.NoiseType, Params.Seed) * Amplitude;
		MaxValue += Amplitude;
		Amplitude *= Params.Persistence;
	}
	return Total / MaxValue;
}
