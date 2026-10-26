// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelPlanetFarField.h"
#include "VoxelCubeSphere.h"
#include "VoxelPlanetOcean.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelBlockSurface.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelWorldConfiguration.h"
#include "SphericalPlanetWorldMode.h"
#include "IVoxelWorldMode.h"
#include "Async/Async.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarFarFieldMaterialBlend(
	TEXT("voxel.World.FarFieldMaterialBlend"),
	1,
	TEXT("Planet far field materials (world modes review G45): 1 = each quad carries its corners' two leading materials and a ")
	TEXT("weight per corner, from each vertex's material mix, and the terrain material dithers between them (a gradient across ")
	TEXT("the quad); 0 = one material a quad, the most common of its corners' picks, as before (borders a staircase of quads). ")
	TEXT("Read when patches build; voxel.World.FarField 0 then 1 rebuilds them."),
	ECVF_Default);

bool FVoxelFarFieldSurface::IsMaterialBlendEnabled()
{
	return CVarFarFieldMaterialBlend.GetValueOnAnyThread() != 0;
}

namespace VoxelPlanetFarFieldPrivate
{
	/** The angle between two unit directions, precise for small angles (atan2 of the cross and dot products). */
	double AngleBetween(const FVector& A, const FVector& B)
	{
		return FMath::Atan2(FVector::CrossProduct(A, B).Size(), FVector::DotProduct(A, B));
	}

	/** The most common value of four (the first of the most common on a tie). */
	int32 MajorityOfFour(const uint16 Values[4])
	{
		int32 Best = 0;
		int32 BestCount = 0;
		for (int32 i = 0; i < 4; ++i)
		{
			int32 Count = 0;
			for (int32 j = 0; j < 4; ++j)
			{
				Count += Values[j] == Values[i] ? 1 : 0;
			}
			if (Count > BestCount)
			{
				Best = i;
				BestCount = Count;
			}
		}
		return Best;
	}

	/** The largest distance from a point to the segment's point at the same parameter (a polyline's chord against its vertices). */
	double ChordGap(const FVector& Point, const FVector& A, const FVector& B, double T)
	{
		return FVector::Dist(Point, FMath::Lerp(A, B, T));
	}
}

// ==================== FVoxelFarFieldPatchKey ====================

FVector FVoxelFarFieldPatchKey::GetDirection(double FU, double FV) const
{
	// FVoxelCubeSphere::SurfaceToDirection, with the face's edge exactly at a gnomonic coordinate of +-1 (tan(pi / 4) is
	// 0.9999999999999999): a vertex on a cube edge is then the same direction from both faces, exactly on the plane
	// x = +-y, y = +-z or x = +-z the edge lies in. The terrain steps across those planes (simplex noise, G43), so two
	// faces sampling a shared edge a rounding apart drew it on opposite sides of the step.
	const double Size = GetUVSize();
	const FVector2D Min = GetUVMin();
	auto Gnomonic = [](double U)
	{
		return U == 1.0 ? 1.0 : (U == -1.0 ? -1.0 : FVoxelCubeSphere::GnomonicFromWarp(U));
	};
	FVector N, T, B;
	FVoxelCubeSphere::GetFaceAxes(Face, N, T, B);
	return (N + Gnomonic(Min.X + FU * Size) * T + Gnomonic(Min.Y + FV * Size) * B).GetSafeNormal();
}

// ==================== FVoxelFarFieldSettings ====================

int32 FVoxelFarFieldSettings::ComputeBandLevel(double InRadius, double InChunkWorldSize)
{
	if (InRadius <= 0.0 || InChunkWorldSize <= 0.0)
	{
		return 0;
	}
	const double Ratio = UE_DOUBLE_HALF_PI * InRadius / InChunkWorldSize;
	if (Ratio < 2.0)
	{
		return 0;
	}
	return FMath::Clamp(FMath::FloorToInt32(FMath::Log2(Ratio)), 0, 27);
}

int32 FVoxelFarFieldSettings::ComputeHeightmapBandLevel(int32 InRootChunks)
{
	if (InRootChunks <= 1)
	{
		return 0;
	}
	// Integer: a float log2 of 64 lands under 6.
	return FMath::Clamp(static_cast<int32>(FMath::FloorLog2(static_cast<uint32>(InRootChunks))), 0, 27);
}

int32 FVoxelFarFieldSettings::ComputeHeightmapRootChunks(double Reach, double InChunkWorldSize)
{
	if (InChunkWorldSize <= 0.0 || Reach <= 0.0)
	{
		return 64;
	}
	// A root tile about a quarter of the reach (a few rows of roots about the camera, each kept built as a planet's six
	// are), a power of two from 4 to 4,096 chunks: 64 with 6,400 uu chunks and the 2,000,000 uu default, 256 with 1,600.
	const double Chunks = 0.25 * Reach / InChunkWorldSize;
	const uint32 Rounded = FMath::RoundUpToPowerOfTwo(static_cast<uint32>(FMath::Clamp(Chunks, 4.0, 4096.0)));
	// Round to the nearest power of two rather than up (96 -> 64, not 128).
	const uint32 Below = Rounded / 2;
	return static_cast<int32>((Chunks - Below < Rounded - Chunks && Below >= 4) ? Below : Rounded);
}

// ==================== FVoxelFarFieldSurface ====================

TSharedPtr<const FVoxelFarFieldSurface> FVoxelFarFieldSurface::Make(const UVoxelWorldConfiguration& Config, const TSharedPtr<IVoxelWorldMode>& InWorldMode)
{
	if (!InWorldMode.IsValid())
	{
		return nullptr;
	}
	const EWorldMode Mode = InWorldMode->GetWorldModeType();
	if (Mode != EWorldMode::SphericalPlanet && Mode != EWorldMode::InfinitePlane && Mode != EWorldMode::IslandBowl)
	{
		return nullptr;
	}
	TSharedPtr<FVoxelFarFieldSurface> Surface = MakeShared<FVoxelFarFieldSurface>();
	Surface->WorldMode = InWorldMode;
	Surface->NoiseParams = Config.NoiseParams;
	if (Config.bEnableBiomes)
	{
		Surface->Biomes = FVoxelBiomeSnapshot::FromConfig(Config.BiomeConfiguration);
	}
	Surface->Sea = FVoxelSeaSurface::FromConfig(Config);
	Surface->VoxelSize = Config.VoxelSize;
	// A heightmap world (WM-MP1): the generated height at (X, Y), the climate from the terrain's seed, the block top in a
	// cubic world (WM-C5: the walking surface up to a block over the isosurface).
	Surface->bHeightmap = Mode != EWorldMode::SphericalPlanet;
	Surface->WorldSeed = Config.NoiseParams.Seed;
	Surface->bWater = Config.bEnableWaterLevel;
	Surface->WaterLevel = Config.WaterLevel;
	Surface->bBlockSurface = Surface->bHeightmap && FVoxelBlockSurface::UsesBlockSurface(Config);
	Surface->WorldOrigin = Config.WorldOrigin;
	return Surface;
}

float FVoxelFarFieldSurface::SampleHeight(double WorldX, double WorldY, const TArray<FVoxelConditioningZone>& Zones) const
{
	double X = WorldX;
	double Y = WorldY;
	if (bBlockSurface)
	{
		// The block column's height is sampled at its lower corner (FVoxelBlockSurface): the surface is that column's block top.
		X = FVoxelBlockSurface::ColumnCorner(WorldX, WorldOrigin.X, VoxelSize);
		Y = FVoxelBlockSurface::ColumnCorner(WorldY, WorldOrigin.Y, VoxelSize);
	}
	float Height = WorldMode->GetTerrainHeightAt(static_cast<float>(X), static_cast<float>(Y), NoiseParams);
	if (Zones.Num() > 0)
	{
		Height = FVoxelTerrainConditioning::ApplyToHeight(X, Y, Height, Zones);
	}
	if (bBlockSurface)
	{
		return static_cast<float>(FVoxelBlockSurface::BlockTopForHeight(Height, WorldOrigin.Z, VoxelSize));
	}
	return Height;
}

void FVoxelFarFieldSurface::SampleConditionsXY(double WorldX, double WorldY, float Height, uint8& OutMaterial, uint8& OutBiome) const
{
	if (Biomes.bIsValid)
	{
		FVoxelSurfaceQuery::QuerySurfaceConditions(static_cast<float>(WorldX), static_cast<float>(WorldY), Height, VoxelSize, Biomes, WorldSeed,
			bWater, WaterLevel, OutMaterial, OutBiome);
		return;
	}
	// No biomes: generation's material by depth (the surface's at depth 0).
	OutMaterial = WorldMode->GetMaterialAtDepth(FVector(WorldX, WorldY, Height), Height, 0.0f);
	OutBiome = 0;
}

FVoxelSurfaceMaterialMix FVoxelFarFieldSurface::SampleMaterialMixXY(double WorldX, double WorldY, float Height) const
{
	if (Biomes.bIsValid)
	{
		return FVoxelSurfaceQuery::QuerySurfaceMaterialMix(static_cast<float>(WorldX), static_cast<float>(WorldY), Height, Biomes, WorldSeed,
			bWater, WaterLevel);
	}
	FVoxelSurfaceMaterialMix Mix;
	uint8 Biome = 0;
	SampleConditionsXY(WorldX, WorldY, Height, Mix.Materials[0], Biome);
	Mix.Materials[1] = Mix.Materials[0];
	Mix.BiomeID = Biome;
	return Mix;
}

const FSphericalPlanetWorldMode& FVoxelFarFieldSurface::GetPlanet() const
{
	return static_cast<const FSphericalPlanetWorldMode&>(*WorldMode);
}

float FVoxelFarFieldSurface::SampleAltitude(const FVector& Direction, const TArray<FVoxelConditioningZone>& Zones, float* OutContinentalness) const
{
	// GetConditionedTerrainAltitude, keeping the continentalness for the material's biome blend.
	const FSphericalPlanetWorldMode& Planet = GetPlanet();
	const FSphericalPlanetParams& Params = Planet.GetPlanetParams();
	float Continentalness = 0.0f;
	float Altitude = FSphericalPlanetWorldMode::SampleRadialDisplacement(Direction, Params.PlanetRadius, Planet.GetTerrainParams(),
		NoiseParams, &Planet.GetBiomeSnapshot(), &Continentalness);
	if (Zones.Num() > 0)
	{
		Altitude = FVoxelTerrainConditioning::ApplyToAltitude(Direction, Params.PlanetRadius, Altitude, Zones);
	}
	if (OutContinentalness)
	{
		*OutContinentalness = Continentalness;
	}
	return Altitude;
}

void FVoxelFarFieldSurface::SampleConditions(const FVector& Direction, float Altitude, float Continentalness, uint8& OutMaterial, uint8& OutBiome) const
{
	const FSphericalPlanetWorldMode& Planet = GetPlanet();
	if (Biomes.bIsValid)
	{
		FVoxelSurfaceQuery::QueryPlanetSurfaceConditionsAt(Planet, Direction, Altitude, Continentalness, Altitude, VoxelSize, NoiseParams,
			Biomes, Sea, OutMaterial, OutBiome);
		return;
	}
	// No biomes: generation's material by depth (the surface's at depth 0).
	const FSphericalPlanetParams& Params = Planet.GetPlanetParams();
	OutMaterial = Planet.GetMaterialAtDepth(Params.PlanetCenter + Direction * (Params.PlanetRadius + Altitude), Altitude, 0.0f);
	OutBiome = 0;
}

FVoxelSurfaceMaterialMix FVoxelFarFieldSurface::SampleMaterialMix(const FVector& Direction, float Altitude, float Continentalness) const
{
	const FSphericalPlanetWorldMode& Planet = GetPlanet();
	if (Biomes.bIsValid)
	{
		return FVoxelSurfaceQuery::QueryPlanetSurfaceMaterialMixAt(Planet, Direction, Altitude, Continentalness, NoiseParams, Biomes, Sea);
	}
	FVoxelSurfaceMaterialMix Mix;
	uint8 Biome = 0;
	SampleConditions(Direction, Altitude, Continentalness, Mix.Materials[0], Biome);
	Mix.Materials[1] = Mix.Materials[0];
	Mix.BiomeID = Biome;
	return Mix;
}

// ==================== FVoxelPlanetFarField: statics ====================

void FVoxelPlanetFarField::GetPatchCap(const FVoxelFarFieldPatchKey& Key, FVector& OutDirection, double& OutAngle)
{
	if (Key.IsHeightmap())
	{
		// No cap on a heightmap world: the patch's footprint (FPatchState::Box) is its extent.
		OutDirection = FVector::UpVector;
		OutAngle = 0.0;
		return;
	}
	OutDirection = Key.GetDirection(0.5, 0.5);
	OutAngle = 0.0;
	for (int32 Corner = 0; Corner < 4; ++Corner)
	{
		const FVector CornerDirection = Key.GetDirection(Corner & 1, Corner >> 1);
		OutAngle = FMath::Max(OutAngle, VoxelPlanetFarFieldPrivate::AngleBetween(OutDirection, CornerDirection));
	}
}

bool FVoxelPlanetFarField::IsBelowHorizon(const FVector& Center, double OccluderRadius, const FVector& Camera, const FVector& CapDirection,
	double CapAngle, double TopRadius)
{
	const FVector ToCamera = Camera - Center;
	const double CameraDistance = ToCamera.Size();
	if (OccluderRadius <= 0.0 || CameraDistance <= OccluderRadius)
	{
		return false;
	}
	// The camera sees the occluding sphere out to the angle where its line of sight grazes it; a top at TopRadius stays in
	// view for as much again past that.
	const double CameraAngle = FMath::Acos(FMath::Clamp(OccluderRadius / CameraDistance, -1.0, 1.0));
	const double TopAngle = TopRadius > OccluderRadius ? FMath::Acos(FMath::Clamp(OccluderRadius / TopRadius, -1.0, 1.0)) : 0.0;
	const double Angle = VoxelPlanetFarFieldPrivate::AngleBetween(ToCamera / CameraDistance, CapDirection);
	return Angle - CapAngle > CameraAngle + TopAngle;
}

bool FVoxelPlanetFarField::ShouldDrawSea(double SeaRadius, double MaxSag, int32 MinSegments, int32 MaxSegments)
{
	if (SeaRadius <= 0.0)
	{
		return false;
	}
	const int32 Segments = FVoxelPlanetOcean::ChooseSegments(SeaRadius, MaxSag, MinSegments, MaxSegments);
	return FVoxelPlanetOcean::MeasureMaxSag(SeaRadius, Segments) > MaxSag;
}

void FVoxelPlanetFarField::BuildPatch(const FVoxelFarFieldSettings& Settings, const FVoxelFarFieldSurface& Surface,
	const TArray<FVoxelConditioningZone>& Zones, const FVoxelFarFieldPatchKey& Key, FVoxelFarFieldPatchMesh& Out)
{
	using namespace VoxelPlanetFarFieldPrivate;
	const double StartTime = FPlatformTime::Seconds();

	Out = FVoxelFarFieldPatchMesh();
	Out.Key = Key;
	Out.ZoneCount = Zones.Num();

	const int32 Q = FMath::Max(Settings.PatchQuads, 2);
	const int32 Row = Q + 1;
	const FVector C = Settings.Center;
	const double R = Settings.Radius;
	const double Sink = Settings.Sink;
	const bool bBandLeaf = Key.Level >= Settings.BandLevel;
	// A heightmap world (WM-MP1): the patch is a tile of the XY plane, the surface the generated height, local up +Z, the
	// "altitude" of a sample its world Z. Everything below reads the surface through the lambdas, which branch once.
	const bool bHeightmap = Settings.bHeightmap && Key.IsHeightmap();
	const double RootTileSize = Settings.GetRootTileSize();
	auto PointXY = [&](double FU, double FV) { return Key.GetPoint2D(Settings.GridOrigin, RootTileSize, FU, FV); };

	if (bHeightmap)
	{
		const FVector2D Centre = PointXY(0.5, 0.5);
		Out.Origin = FVector(Centre.X, Centre.Y, Surface.SampleHeight(Centre.X, Centre.Y, Zones));
	}
	else
	{
		Out.Origin = C + Key.GetDirection(0.5, 0.5) * R;
	}
	const FVector Origin = Out.Origin;

	// The grid with one ring of samples past the patch's edges (G x G, index -1 .. Q + 1): the normals' central differences
	// at the edges then match the neighbour's of the same level.
	const int32 G = Q + 3;
	auto GridIndex = [G](int32 I, int32 J) { return (I + 1) + (J + 1) * G; };
	TArray<FVector> Directions; // local up at the sample (the radial direction; +Z on a heightmap world)
	TArray<FVector2D> SamplesXY; // heightmap worlds: the sample's world (X, Y)
	TArray<double> Altitudes;
	TArray<float> Continentalness;
	Directions.SetNumUninitialized(G * G);
	Altitudes.SetNumUninitialized(G * G);
	Continentalness.SetNumUninitialized(G * G);
	if (bHeightmap)
	{
		SamplesXY.SetNumUninitialized(G * G);
	}
	for (int32 J = -1; J <= Q + 1; ++J)
	{
		for (int32 I = -1; I <= Q + 1; ++I)
		{
			const int32 Index = GridIndex(I, J);
			if (bHeightmap)
			{
				const FVector2D XY = PointXY(static_cast<double>(I) / Q, static_cast<double>(J) / Q);
				SamplesXY[Index] = XY;
				Directions[Index] = FVector::UpVector;
				Altitudes[Index] = Surface.SampleHeight(XY.X, XY.Y, Zones);
				Continentalness[Index] = 0.0f;
				continue;
			}
			const FVector Direction = Key.GetDirection(static_cast<double>(I) / Q, static_cast<double>(J) / Q);
			float Cont = 0.0f;
			Directions[Index] = Direction;
			Altitudes[Index] = Surface.SampleAltitude(Direction, Zones, &Cont);
			Continentalness[Index] = Cont;
		}
	}
	// The analytic surface (for normals) and the drawn, sunk one, in double.
	auto SurfacePoint = [&](int32 I, int32 J)
	{
		const int32 Index = GridIndex(I, J);
		return bHeightmap ? FVector(SamplesXY[Index].X, SamplesXY[Index].Y, Altitudes[Index]) : C + Directions[Index] * (R + Altitudes[Index]);
	};
	auto DrawnPoint = [&](int32 I, int32 J)
	{
		const int32 Index = GridIndex(I, J);
		return bHeightmap ? FVector(SamplesXY[Index].X, SamplesXY[Index].Y, Altitudes[Index] - Sink) : C + Directions[Index] * (R + Altitudes[Index] - Sink);
	};
	// The surface's altitude (world Z on a heightmap world) at a fraction of the patch, and the drawn point there.
	auto AltitudeAt = [&](double FU, double FV, FVector* OutDrawn)
	{
		if (bHeightmap)
		{
			const FVector2D XY = PointXY(FU, FV);
			const double Height = Surface.SampleHeight(XY.X, XY.Y, Zones);
			if (OutDrawn)
			{
				*OutDrawn = FVector(XY.X, XY.Y, Height - Sink);
			}
			return Height;
		}
		const FVector Direction = Key.GetDirection(FU, FV);
		const double Altitude = Surface.SampleAltitude(Direction, Zones);
		if (OutDrawn)
		{
			*OutDrawn = C + Direction * (R + Altitude - Sink);
		}
		return Altitude;
	};

	// Normals from the grid; materials and biomes at the patch's vertices.
	TArray<FVector3f> Normals;
	TArray<uint8> Materials;
	TArray<uint8> Biomes;
	TArray<FVoxelSurfaceMaterialMix> Mixes; // G45: each vertex's material mix, when the quads blend
	const bool bBlendMaterials = FVoxelFarFieldSurface::IsMaterialBlendEnabled();
	Normals.SetNumUninitialized(Row * Row);
	Materials.SetNumUninitialized(Row * Row);
	Biomes.SetNumUninitialized(Row * Row);
	if (bBlendMaterials)
	{
		Mixes.SetNum(Row * Row);
	}
	Out.MinAltitude = TNumericLimits<float>::Max();
	Out.MaxAltitude = TNumericLimits<float>::Lowest();
	for (int32 J = 0; J <= Q; ++J)
	{
		for (int32 I = 0; I <= Q; ++I)
		{
			const int32 Index = GridIndex(I, J);
			const FVector DU = SurfacePoint(I + 1, J) - SurfacePoint(I - 1, J);
			const FVector DV = SurfacePoint(I, J + 1) - SurfacePoint(I, J - 1);
			FVector Normal = FVector::CrossProduct(DU, DV).GetSafeNormal();
			if (Normal.IsNearlyZero() || FVector::DotProduct(Normal, Directions[Index]) <= 0.0)
			{
				Normal = Directions[Index];
			}
			Normals[I + J * Row] = FVector3f(Normal);
			if (bBlendMaterials)
			{
				FVoxelSurfaceMaterialMix& Mix = Mixes[I + J * Row];
				Mix = bHeightmap
					? Surface.SampleMaterialMixXY(SamplesXY[Index].X, SamplesXY[Index].Y, static_cast<float>(Altitudes[Index]))
					: Surface.SampleMaterialMix(Directions[Index], static_cast<float>(Altitudes[Index]), Continentalness[Index]);
				Materials[I + J * Row] = Mix.Materials[0];
				Biomes[I + J * Row] = Mix.BiomeID;
			}
			else if (bHeightmap)
			{
				Surface.SampleConditionsXY(SamplesXY[Index].X, SamplesXY[Index].Y, static_cast<float>(Altitudes[Index]),
					Materials[I + J * Row], Biomes[I + J * Row]);
			}
			else
			{
				Surface.SampleConditions(Directions[Index], static_cast<float>(Altitudes[Index]), Continentalness[Index],
					Materials[I + J * Row], Biomes[I + J * Row]);
			}
			Out.MinAltitude = FMath::Min(Out.MinAltitude, static_cast<float>(Altitudes[Index]));
			Out.MaxAltitude = FMath::Max(Out.MaxAltitude, static_cast<float>(Altitudes[Index]));
		}
	}

	// The quads' centres: the drawn triangles' shared diagonal (A-D) against the surface there (the error), the dips the sea
	// test needs, and the surface chunks between the vertices.
	TArray<double> CentreAltitudes;
	CentreAltitudes.SetNumUninitialized(Q * Q);
	TSet<FIntVector> SurfaceChunks;
	auto ChunkOf = [&Settings](const FVector& Point)
	{
		const FVector Local = (Point - Settings.GridOrigin) / FMath::Max(Settings.ChunkWorldSize, 1.0);
		return FIntVector(FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), FMath::FloorToInt32(Local.Z));
	};
	for (int32 J = 0; J < Q; ++J)
	{
		for (int32 I = 0; I < Q; ++I)
		{
			const FVector A = DrawnPoint(I, J);
			const FVector D = DrawnPoint(I + 1, J + 1);
			const FVector Middle = (A + D) * 0.5;
			double Altitude;
			FVector MiddleSurface;
			if (bHeightmap)
			{
				Altitude = Surface.SampleHeight(Middle.X, Middle.Y, Zones);
				Out.Error = FMath::Max(Out.Error, FMath::Abs(Middle.Z - (Altitude - Sink)));
				MiddleSurface = FVector(Middle.X, Middle.Y, Altitude);
			}
			else
			{
				const FVector MiddleDirection = (Middle - C).GetSafeNormal();
				Altitude = Surface.SampleAltitude(MiddleDirection, Zones);
				Out.Error = FMath::Max(Out.Error, FMath::Abs(FVector::Dist(Middle, C) - (R + Altitude - Sink)));
				MiddleSurface = C + MiddleDirection * (R + Altitude);
			}
			CentreAltitudes[I + J * Q] = Altitude;
			Out.MinAltitude = FMath::Min(Out.MinAltitude, static_cast<float>(Altitude));
			Out.MaxAltitude = FMath::Max(Out.MaxAltitude, static_cast<float>(Altitude));
			if (bBandLeaf)
			{
				SurfaceChunks.Add(ChunkOf(MiddleSurface));
			}
		}
	}
	if (bBandLeaf)
	{
		// Every vertex's chunk, and the chunks a vertex spacing away along each axis: a chunk the surface passes through
		// between vertices is caught too, so a patch never hides over a chunk that is not drawn.
		const double Reach = Settings.GetPatchArc(Key.Level) / Q;
		for (int32 J = 0; J <= Q; ++J)
		{
			for (int32 I = 0; I <= Q; ++I)
			{
				const FVector Point = SurfacePoint(I, J);
				SurfaceChunks.Add(ChunkOf(Point));
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					FVector Offset = FVector::ZeroVector;
					Offset[Axis] = Reach;
					SurfaceChunks.Add(ChunkOf(Point + Offset));
					SurfaceChunks.Add(ChunkOf(Point - Offset));
				}
			}
		}
		Out.SurfaceChunks = SurfaceChunks.Array();
	}

	// Skirts: deep enough to close the gap to the same edge drawn by a coarser neighbour (every 2^D-th of this patch's edge
	// vertices joined by chords, D up to SkirtLevels) and to a finer one (the surface between this patch's edge vertices,
	// sampled 2^SkirtLevels times finer).
	const int32 Fine = 1 << FMath::Clamp(Settings.SkirtLevels, 0, 5);
	double SkirtDepth = 0.0;
	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		// Edge vertex k (0..Q) and its grid indices: 0 = J 0, 1 = I Q, 2 = J Q, 3 = I 0.
		auto EdgeIJ = [Edge, Q](int32 K, int32& OutI, int32& OutJ)
		{
			switch (Edge)
			{
			case 0: OutI = K; OutJ = 0; break;
			case 1: OutI = Q; OutJ = K; break;
			case 2: OutI = K; OutJ = Q; break;
			default: OutI = 0; OutJ = K; break;
			}
		};
		TArray<FVector, TInlineAllocator<65>> EdgePoints;
		for (int32 K = 0; K <= Q; ++K)
		{
			int32 I, J;
			EdgeIJ(K, I, J);
			EdgePoints.Add(DrawnPoint(I, J));
		}
		for (int32 Level = 1; Level <= Settings.SkirtLevels; ++Level)
		{
			const int32 Step = 1 << Level;
			if (Step > Q)
			{
				break;
			}
			for (int32 K0 = 0; K0 + Step <= Q; K0 += Step)
			{
				for (int32 K = K0 + 1; K < K0 + Step; ++K)
				{
					SkirtDepth = FMath::Max(SkirtDepth, ChordGap(EdgePoints[K], EdgePoints[K0], EdgePoints[K0 + Step],
						static_cast<double>(K - K0) / Step));
				}
			}
		}
		if (Fine > 1)
		{
			for (int32 K = 0; K < Q; ++K)
			{
				int32 I0, J0, I1, J1;
				EdgeIJ(K, I0, J0);
				EdgeIJ(K + 1, I1, J1);
				for (int32 S = 1; S < Fine; ++S)
				{
					const double T = static_cast<double>(S) / Fine;
					const double FU = (FMath::Lerp(static_cast<double>(I0), static_cast<double>(I1), T)) / Q;
					const double FV = (FMath::Lerp(static_cast<double>(J0), static_cast<double>(J1), T)) / Q;
					FVector Point;
					AltitudeAt(FU, FV, &Point);
					SkirtDepth = FMath::Max(SkirtDepth, ChordGap(Point, EdgePoints[K], EdgePoints[K + 1], T));
				}
			}
		}
	}
	// A little more than the largest gap, and never less than a sliver of the vertex spacing.
	const double Spacing = Settings.GetPatchArc(Key.Level) / Q;
	Out.SkirtDepth = SkirtDepth * 1.25 + FMath::Max(1.0, 0.02 * Spacing);

	// ---- The terrain surface: one material and biome per quad, a grid vertex per (material, biome) it touches ----
	FVector FaceN = FVector::UpVector, FaceT = FVector::ForwardVector, FaceB = FVector::RightVector;
	if (!bHeightmap)
	{
		FVoxelCubeSphere::GetFaceAxes(Key.Face, FaceN, FaceT, FaceB);
	}
	const FVector2D UVMin = bHeightmap ? FVector2D::ZeroVector : Key.GetUVMin();
	const double UVSize = bHeightmap ? 0.0 : Key.GetUVSize();

	FVoxelFarFieldSection& Terrain = Out.Terrain;
	const int32 ExpectedVertices = Row * Row + 4 * Q * 4;
	Terrain.Positions.Reserve(ExpectedVertices);
	Terrain.Normals.Reserve(ExpectedVertices);
	Terrain.Tangents.Reserve(ExpectedVertices);
	Terrain.UV0.Reserve(ExpectedVertices);
	Terrain.UV1.Reserve(ExpectedVertices);
	Terrain.Colors.Reserve(ExpectedVertices);
	Terrain.Triangles.Reserve(Q * Q * 6 + 4 * Q * 12);

	FBox Bounds(ForceInit);
	// Colour: R the quad's second material (the material itself without one), G the biome, A 255 less the second's weight
	// at the vertex (G45; 255 = none, as every voxel chunk vertex).
	auto AddVertex = [&](FVoxelFarFieldSection& Section, const FVector& WorldPoint, const FVector3f& Normal, const FVector& Direction,
		const FVector2f& UV0, uint8 Material, uint8 Biome, uint8 Secondary, float SecondaryWeight)
	{
		const int32 Index = Section.Positions.Num();
		Section.Positions.Add(FVector3f(WorldPoint - Origin));
		Section.Normals.Add(Normal);
		Section.Tangents.Add(FVector3f((FaceT - FVector::DotProduct(FaceT, Direction) * Direction).GetSafeNormal()));
		Section.UV0.Add(UV0);
		Section.UV1.Add(FVector2f(static_cast<float>(Material), 0.0f));
		const uint8 Alpha = static_cast<uint8>(255 - FMath::Clamp(FMath::RoundToInt32(SecondaryWeight * 255.0f), 0, 255));
		Section.Colors.Add(FColor(Secondary, Biome, 0, Alpha));
		Bounds += WorldPoint;
		return Index;
	};
	// A terrain vertex as the voxel vertex factory presents one (FVoxelLocalVertex::FromVoxelVertex with the planet's radial
	// face up, VoxelSceneProxy's ConvertVertex): the tangent cross(Z, N) (X at the poles), and UV1.y the face type against
	// local up (0 top, 1 side, 2 bottom). The terrain material's shading reads both, and the far field must shade as the
	// voxel surface it continues (a different tangent frame drew it visibly darker at the window's edge).
	auto AddTerrainVertex = [&](const FVector& WorldPoint, const FVector3f& Normal, const FVector& Direction, const FVector2f& UV0,
		uint8 Material, uint8 Biome, uint8 Secondary, float SecondaryWeight)
	{
		const int32 Index = AddVertex(Terrain, WorldPoint, Normal, Direction, UV0, Material, Biome, Secondary, SecondaryWeight);
		const FVector3f Reference = FMath::Abs(Normal.Z) < 0.999f ? FVector3f(0.0f, 0.0f, 1.0f) : FVector3f(1.0f, 0.0f, 0.0f);
		Terrain.Tangents[Index] = FVector3f::CrossProduct(Reference, Normal).GetSafeNormal();
		const float Along = FVector3f::DotProduct(Normal, FVector3f(Direction));
		Terrain.UV1[Index].Y = Along > 0.7f ? 0.0f : (Along < -0.7f ? 2.0f : 1.0f);
		return Index;
	};
	// The terrain's UV0 as the voxel meshers make it (FVoxelCPUDualContourMesher::EmitVertex): the position projected on the
	// plane of the normal's dominant axis, in voxels, here relative to the patch's origin so the values stay small (the
	// absolute face coordinate in voxels reaches 1e6 on a 1,000 km planet, where a float keeps 1/16 of a texture tile).
	const double InvVoxel = 1.0 / FMath::Max(Settings.VoxelSize, 1.0);
	auto TerrainUV0 = [&](const FVector& WorldPoint, const FVector3f& Normal)
	{
		const FVector Local = (WorldPoint - Origin) * InvVoxel;
		const float AX = FMath::Abs(Normal.X), AY = FMath::Abs(Normal.Y), AZ = FMath::Abs(Normal.Z);
		if (AZ >= AX && AZ >= AY)
		{
			return FVector2f(static_cast<float>(Local.X), static_cast<float>(Local.Y));
		}
		if (AX >= AY)
		{
			return FVector2f(static_cast<float>(Local.Y), static_cast<float>(Local.Z));
		}
		return FVector2f(static_cast<float>(Local.X), static_cast<float>(Local.Z));
	};

	// Quad keys: the material (bits 0-7), the second material (8-15) and the biome (16-23). Without the blend the most common
	// (material, biome) of the corners, its second material itself. With it (G45) the two largest shares of the corners'
	// mixes, and the most common biome.
	auto PackKey = [](uint8 Material, uint8 Secondary, uint8 Biome)
	{
		return static_cast<uint32>(Material) | (static_cast<uint32>(Secondary) << 8) | (static_cast<uint32>(Biome) << 16);
	};
	auto KeyMaterial = [](uint32 Key) { return static_cast<uint8>(Key & 0xFF); };
	auto KeySecondary = [](uint32 Key) { return static_cast<uint8>((Key >> 8) & 0xFF); };
	auto KeyBiome = [](uint32 Key) { return static_cast<uint8>((Key >> 16) & 0xFF); };
	// A vertex mix's share of a material.
	auto ShareOf = [](const FVoxelSurfaceMaterialMix& Mix, uint8 Material)
	{
		const float Second = Mix.Materials[1] != Mix.Materials[0] ? Mix.Weight : 0.0f;
		return (Mix.Materials[0] == Material ? 1.0f - Second : 0.0f) + (Mix.Materials[1] == Material && Mix.Materials[1] != Mix.Materials[0] ? Second : 0.0f);
	};
	TArray<uint32> QuadKeys;
	QuadKeys.SetNumUninitialized(Q * Q);
	for (int32 J = 0; J < Q; ++J)
	{
		for (int32 I = 0; I < Q; ++I)
		{
			const int32 Corners[4] = { I + J * Row, (I + 1) + J * Row, I + (J + 1) * Row, (I + 1) + (J + 1) * Row };
			if (!bBlendMaterials)
			{
				uint16 Keys[4];
				for (int32 c = 0; c < 4; ++c)
				{
					Keys[c] = static_cast<uint16>(Materials[Corners[c]]) | (static_cast<uint16>(Biomes[Corners[c]]) << 8);
				}
				const uint16 Majority = Keys[MajorityOfFour(Keys)];
				QuadKeys[I + J * Q] = PackKey(static_cast<uint8>(Majority & 0xFF), static_cast<uint8>(Majority & 0xFF), static_cast<uint8>(Majority >> 8));
				continue;
			}
			// The corners' shares per material; the largest two.
			uint8 Candidates[8];
			float Totals[8];
			int32 Num = 0;
			auto Add = [&](uint8 Material, float Share)
			{
				if (Share <= 0.0f)
				{
					return;
				}
				int32 Slot = 0;
				while (Slot < Num && Candidates[Slot] != Material)
				{
					++Slot;
				}
				if (Slot == Num)
				{
					Candidates[Num] = Material;
					Totals[Num] = 0.0f;
					++Num;
				}
				Totals[Slot] += Share;
			};
			uint16 BiomeKeys[4];
			for (int32 c = 0; c < 4; ++c)
			{
				const FVoxelSurfaceMaterialMix& Mix = Mixes[Corners[c]];
				Add(Mix.Materials[0], ShareOf(Mix, Mix.Materials[0]));
				if (Mix.Materials[1] != Mix.Materials[0])
				{
					Add(Mix.Materials[1], ShareOf(Mix, Mix.Materials[1]));
				}
				BiomeKeys[c] = Mix.BiomeID;
			}
			int32 First = 0;
			for (int32 k = 1; k < Num; ++k)
			{
				First = Totals[k] > Totals[First] ? k : First;
			}
			int32 Second = -1;
			for (int32 k = 0; k < Num; ++k)
			{
				if (k != First && (Second < 0 || Totals[k] > Totals[Second]))
				{
					Second = k;
				}
			}
			const uint8 Primary = Num > 0 ? Candidates[First] : Materials[Corners[0]];
			const uint8 Secondary = Second >= 0 ? Candidates[Second] : Primary;
			QuadKeys[I + J * Q] = PackKey(Primary, Secondary, static_cast<uint8>(BiomeKeys[MajorityOfFour(BiomeKeys)]));
		}
	}
	// The second material's weight at a grid vertex for a quad's pair: its share of the pair in the vertex's mix.
	auto CornerWeight = [&](int32 I, int32 J, uint32 QuadKey)
	{
		const uint8 Primary = KeyMaterial(QuadKey);
		const uint8 Secondary = KeySecondary(QuadKey);
		if (!bBlendMaterials || Primary == Secondary)
		{
			return 0.0f;
		}
		const FVoxelSurfaceMaterialMix& Mix = Mixes[I + J * Row];
		const float P = ShareOf(Mix, Primary);
		const float S = ShareOf(Mix, Secondary);
		return (P + S) > 0.0f ? S / (P + S) : 0.0f;
	};
	// Grid vertex variants: quad key -> vertex index.
	struct FVariant
	{
		uint32 Key;
		int32 Index;
	};
	TArray<TArray<FVariant, TInlineAllocator<2>>> Variants;
	Variants.SetNum(Row * Row);
	auto GetVertex = [&](int32 I, int32 J, uint32 QuadKey)
	{
		TArray<FVariant, TInlineAllocator<2>>& List = Variants[I + J * Row];
		for (const FVariant& Variant : List)
		{
			if (Variant.Key == QuadKey)
			{
				return Variant.Index;
			}
		}
		const int32 GridIdx = GridIndex(I, J);
		const FVector Point = DrawnPoint(I, J);
		const int32 Index = AddTerrainVertex(Point, Normals[I + J * Row], Directions[GridIdx], TerrainUV0(Point, Normals[I + J * Row]),
			KeyMaterial(QuadKey), KeyBiome(QuadKey), KeySecondary(QuadKey), CornerWeight(I, J, QuadKey));
		List.Add({ QuadKey, Index });
		return Index;
	};
	for (int32 J = 0; J < Q; ++J)
	{
		for (int32 I = 0; I < Q; ++I)
		{
			const uint32 QuadKey = QuadKeys[I + J * Q];
			// Corners A (i, j), B (i+1, j), D (i+1, j+1), C (i, j+1) run counter-clockwise seen from outside (T x B = N); UE's
			// front faces take them as (A, D, B), (A, C, D), as the ocean shell's quads do.
			const int32 A = GetVertex(I, J, QuadKey);
			const int32 B = GetVertex(I + 1, J, QuadKey);
			const int32 Cc = GetVertex(I, J + 1, QuadKey);
			const int32 D = GetVertex(I + 1, J + 1, QuadKey);
			Terrain.Triangles.Append({ A, D, B, A, Cc, D });
		}
	}
	Out.TerrainSurfaceVertices = Terrain.Positions.Num();

	// Skirts: each edge dropped along local down by SkirtDepth, both windings, in the material of the quad each segment hangs
	// from. The top is the surface's own vertices; a bottom vertex is shared by the segments of one material either side.
	TMap<uint64, int32> SkirtBottoms;
	auto GetBottom = [&](int32 I, int32 J, uint32 QuadKey)
	{
		const uint64 BottomKey = (static_cast<uint64>(I + J * Row) << 32) | QuadKey;
		if (const int32* Found = SkirtBottoms.Find(BottomKey))
		{
			return *Found;
		}
		const int32 GridIdx = GridIndex(I, J);
		const FVector Point = DrawnPoint(I, J) - Directions[GridIdx] * Out.SkirtDepth;
		const int32 Index = AddTerrainVertex(Point, Normals[I + J * Row], Directions[GridIdx], TerrainUV0(Point, Normals[I + J * Row]),
			KeyMaterial(QuadKey), KeyBiome(QuadKey), KeySecondary(QuadKey), CornerWeight(I, J, QuadKey));
		SkirtBottoms.Add(BottomKey, Index);
		return Index;
	};
	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		for (int32 K = 0; K < Q; ++K)
		{
			int32 I0, J0, I1, J1, QI, QJ;
			switch (Edge)
			{
			case 0: I0 = K; J0 = 0; I1 = K + 1; J1 = 0; QI = K; QJ = 0; break;
			case 1: I0 = Q; J0 = K; I1 = Q; J1 = K + 1; QI = Q - 1; QJ = K; break;
			case 2: I0 = K; J0 = Q; I1 = K + 1; J1 = Q; QI = K; QJ = Q - 1; break;
			default: I0 = 0; J0 = K; I1 = 0; J1 = K + 1; QI = 0; QJ = K; break;
			}
			const uint32 QuadKey = QuadKeys[QI + QJ * Q];
			const int32 T0 = GetVertex(I0, J0, QuadKey);
			const int32 T1 = GetVertex(I1, J1, QuadKey);
			const int32 B0 = GetBottom(I0, J0, QuadKey);
			const int32 B1 = GetBottom(I1, J1, QuadKey);
			Terrain.Triangles.Append({ T0, T1, B1, T0, B1, B0, T0, B1, T1, T0, B0, B1 });
		}
	}

	// ---- The sea: at the sea radius, over the cells whose terrain lies near or under it ----
	// The sea is a smooth sphere: a coarser grid than the terrain's (SeaQuads a side, SeaStep terrain quads to a cell) sags
	// under a uu where it is close and keeps the sea's vertices a sixteenth of the terrain's. A cell is drawn when any terrain
	// quad in it reaches near or under the sea. UV0 counts voxels of arc along the face as the ocean shell's does, less a
	// whole number of SeaUVPeriod per patch: the pattern continues across patches and the floats stay small at any radius.
	// On a heightmap world (WM-MP1) the sea is the WaterLevel plane, where the water tiles lie, and UV0 is world X and Y over
	// the voxel size as the tiles' and the far ocean's (one seamless pattern across the three).
	if (Settings.bSea && (bHeightmap || Settings.SeaRadius > 0.0))
	{
		const double SeaAltitude = bHeightmap ? Settings.SeaZ : Settings.SeaRadius - R;
		const double SeaUVScale = UE_DOUBLE_PI / 4.0 * Settings.SeaRadius / FMath::Max(Settings.VoxelSize, 1.0);
		constexpr double SeaUVPeriod = 1024.0;
		const FVector2D SeaUVOffset(
			FMath::FloorToDouble((UVMin.X + 0.5 * UVSize) * SeaUVScale / SeaUVPeriod) * SeaUVPeriod,
			FMath::FloorToDouble((UVMin.Y + 0.5 * UVSize) * SeaUVScale / SeaUVPeriod) * SeaUVPeriod);
		const int32 SeaStep = FMath::Max(1, Q / FMath::Clamp(FVoxelFarFieldSettings::SeaQuads, 1, Q));
		const uint8 SeaMaterial = FVoxelPlanetOcean::WaterMaterialID;
		TArray<int32> SeaVertex;
		SeaVertex.Init(INDEX_NONE, Row * Row);
		FVoxelFarFieldSection& Sea = Out.Sea;
		auto GetSeaVertex = [&](int32 I, int32 J)
		{
			int32& Index = SeaVertex[I + J * Row];
			if (Index == INDEX_NONE)
			{
				if (bHeightmap)
				{
					const FVector2D XY = SamplesXY[GridIndex(I, J)];
					const FVector2f UV0(static_cast<float>(XY.X / FMath::Max(Settings.VoxelSize, 1.0)), static_cast<float>(XY.Y / FMath::Max(Settings.VoxelSize, 1.0)));
					Index = AddVertex(Sea, FVector(XY.X, XY.Y, Settings.SeaZ), FVector3f(0.0f, 0.0f, 1.0f), FVector::UpVector, UV0, SeaMaterial, 0, SeaMaterial, 0.0f);
					return Index;
				}
				const FVector Direction = Directions[GridIndex(I, J)];
				const FVector2f UV0(
					static_cast<float>((UVMin.X + UVSize * I / Q) * SeaUVScale - SeaUVOffset.X),
					static_cast<float>((UVMin.Y + UVSize * J / Q) * SeaUVScale - SeaUVOffset.Y));
				Index = AddVertex(Sea, C + Direction * Settings.SeaRadius, FVector3f(Direction), Direction, UV0, SeaMaterial, 0, SeaMaterial, 0.0f);
			}
			return Index;
		};
		bool bAnyLand = false;
		for (int32 J0 = 0; J0 < Q; J0 += SeaStep)
		{
			for (int32 I0 = 0; I0 < Q; I0 += SeaStep)
			{
				const int32 I1 = FMath::Min(I0 + SeaStep, Q);
				const int32 J1 = FMath::Min(J0 + SeaStep, Q);
				bool bWet = false;
				for (int32 J = J0; J < J1; ++J)
				{
					for (int32 I = I0; I < I1; ++I)
					{
						const double Lowest = FMath::Min(
							FMath::Min(FMath::Min(Altitudes[GridIndex(I, J)], Altitudes[GridIndex(I + 1, J)]),
								FMath::Min(Altitudes[GridIndex(I, J + 1)], Altitudes[GridIndex(I + 1, J + 1)])),
							CentreAltitudes[I + J * Q]);
						const double Highest = FMath::Max(
							FMath::Max(Altitudes[GridIndex(I, J)], Altitudes[GridIndex(I + 1, J)]),
							FMath::Max(Altitudes[GridIndex(I, J + 1)], Altitudes[GridIndex(I + 1, J + 1)]));
						bAnyLand |= Highest - Sink > SeaAltitude;
						bWet |= Lowest - Sink < SeaAltitude + Settings.SeaMargin;
					}
				}
				if (!bWet)
				{
					continue;
				}
				const int32 A = GetSeaVertex(I0, J0);
				const int32 B = GetSeaVertex(I1, J0);
				const int32 Cc = GetSeaVertex(I0, J1);
				const int32 D = GetSeaVertex(I1, J1);
				Sea.Triangles.Append({ A, D, B, A, Cc, D });
				// The sag at the drawn diagonal's middle, its deepest point under the sphere (a plane has none).
				if (!bHeightmap)
				{
					const FVector Middle = (FVector(Sea.Positions[A]) + FVector(Sea.Positions[D])) * 0.5 + Origin;
					Out.SeaSag = FMath::Max(Out.SeaSag, Settings.SeaRadius - FVector::Dist(Middle, C));
				}
			}
		}
		Out.bHasSea = Sea.Triangles.Num() > 0;
		Out.bHasLand = bAnyLand;
	}

	Out.BoundsCenter = Bounds.IsValid ? Bounds.GetCenter() : Origin;
	Out.BoundsRadius = Bounds.IsValid ? Bounds.GetExtent().Size() : 0.0;
	Out.BuildMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
}

// ==================== FVoxelPlanetFarField ====================

FVoxelPlanetFarField::FVoxelPlanetFarField() = default;

FVoxelPlanetFarField::~FVoxelPlanetFarField()
{
	Reset();
}

void FVoxelPlanetFarField::Initialize(const FVoxelFarFieldSettings& InSettings, TSharedPtr<const FVoxelFarFieldSurface> InSurface)
{
	Reset();
	Settings = InSettings;
	Settings.PatchQuads = FMath::Clamp(Settings.PatchQuads, 2, 64);
	Settings.BandLevel = FMath::Clamp(Settings.BandLevel, 0, 27);
	Settings.SkirtLevels = FMath::Clamp(Settings.SkirtLevels, 0, 5);
	Surface = MoveTemp(InSurface);
}

void FVoxelPlanetFarField::Reset()
{
	Patches.Reset();
	HeightmapRoots.Reset();
	// Builds in flight finish into the old sink, which goes when they let it go.
	Sink = MakeShared<FBuildSink, ESPMode::ThreadSafe>();
	Requests.Reset();
	BuiltThisUpdate.Reset();
	PendingBuildCount = 0;
	BuildsInFlight = 0;
	DrawnPatchCount = 0;
	BuiltSinceRead = 0;
	MeshMemoryBytes = 0;
	bHasLastCamera = false;
	Surface.Reset();
}

FVoxelPlanetFarField::FPatchState* FVoxelPlanetFarField::FindPatchMutable(uint64 PackedKey)
{
	TUniquePtr<FPatchState>* Found = Patches.Find(PackedKey);
	return Found ? Found->Get() : nullptr;
}

const FVoxelPlanetFarField::FPatchState* FVoxelPlanetFarField::FindPatch(uint64 PackedKey) const
{
	const TUniquePtr<FPatchState>* Found = Patches.Find(PackedKey);
	return Found ? Found->Get() : nullptr;
}

FVoxelPlanetFarField::FPatchState& FVoxelPlanetFarField::GetOrAddPatch(const FVoxelFarFieldPatchKey& Key)
{
	TUniquePtr<FPatchState>& Slot = Patches.FindOrAdd(Key.Pack());
	if (!Slot)
	{
		Slot = MakeUnique<FPatchState>();
		Slot->Key = Key;
		GetPatchCap(Key, Slot->CapDirection, Slot->CapAngle);
		if (Key.IsHeightmap())
		{
			Slot->Box = Key.GetBox2D(Settings.GridOrigin, Settings.GetRootTileSize());
		}
	}
	return *Slot;
}

void FVoxelPlanetFarField::ForEachPatch(TFunctionRef<void(const FPatchState&)> VisitPatch) const
{
	for (const TPair<uint64, TUniquePtr<FPatchState>>& Pair : Patches)
	{
		VisitPatch(*Pair.Value);
	}
}

const FVoxelPlanetFarField::FPatchState* FVoxelPlanetFarField::FindDrawnPatchAt(const FVector& WorldPoint, bool bTerrainOnly) const
{
	if (!Settings.bHeightmap)
	{
		return FindDrawnPatch(WorldPoint - Settings.Center, bTerrainOnly);
	}
	const double RootTileSize = Settings.GetRootTileSize();
	if (RootTileSize <= 0.0)
	{
		return nullptr;
	}
	const double LocalX = (WorldPoint.X - Settings.GridOrigin.X) / RootTileSize;
	const double LocalY = (WorldPoint.Y - Settings.GridOrigin.Y) / RootTileSize;
	FVoxelFarFieldPatchKey Key(FVoxelFarFieldPatchKey::HeightmapFace, 0, FMath::FloorToInt32(LocalX), FMath::FloorToInt32(LocalY));
	while (true)
	{
		const FPatchState* Patch = FindPatch(Key.Pack());
		if (!Patch)
		{
			return nullptr;
		}
		if (Patch->bDrawTerrain || (!bTerrainOnly && Patch->bDrawSea))
		{
			return Patch;
		}
		if (!Patch->bHasChildren)
		{
			return nullptr;
		}
		// The child holding the point: the cell indices at the next level.
		const int32 ChildLevel = Key.Level + 1;
		const double Cells = static_cast<double>(1ll << ChildLevel);
		Key = FVoxelFarFieldPatchKey(Key.Face, ChildLevel, FMath::FloorToInt32(LocalX * Cells), FMath::FloorToInt32(LocalY * Cells));
	}
}

const FVoxelPlanetFarField::FPatchState* FVoxelPlanetFarField::FindDrawnPatch(const FVector& Direction, bool bTerrainOnly) const
{
	if (Settings.bHeightmap)
	{
		return nullptr;
	}
	const FVoxelSurfaceCoord Coord = FVoxelCubeSphere::DirectionToSurface(Direction.GetSafeNormal());
	FVoxelFarFieldPatchKey Key(Coord.Face, 0, 0, 0);
	while (true)
	{
		const FPatchState* Patch = FindPatch(Key.Pack());
		if (!Patch)
		{
			return nullptr;
		}
		if (Patch->bDrawTerrain || (!bTerrainOnly && Patch->bDrawSea))
		{
			return Patch;
		}
		if (!Patch->bHasChildren)
		{
			return nullptr;
		}
		const int32 ChildLevel = Key.Level + 1;
		const double ChildSize = 2.0 / static_cast<double>(1ll << ChildLevel);
		const int32 Cells = 1 << ChildLevel;
		const int32 X = FMath::Clamp(FMath::FloorToInt32((Coord.UV.X + 1.0) / ChildSize), 0, Cells - 1);
		const int32 Y = FMath::Clamp(FMath::FloorToInt32((Coord.UV.Y + 1.0) / ChildSize), 0, Cells - 1);
		Key = FVoxelFarFieldPatchKey(Key.Face, ChildLevel, X, Y);
	}
}

void FVoxelPlanetFarField::ReleaseMesh(uint64 PackedKey)
{
	if (FPatchState* Patch = FindPatchMutable(PackedKey))
	{
		if (Patch->Mesh.IsValid())
		{
			MeshMemoryBytes -= static_cast<int64>(Patch->Mesh->GetAllocatedSize());
			Patch->Mesh.Reset();
		}
	}
}

void FVoxelPlanetFarField::InvalidateCap(const FVector& Direction, double AngularRadius)
{
	if (Settings.bHeightmap)
	{
		return;
	}
	for (TPair<uint64, TUniquePtr<FPatchState>>& Pair : Patches)
	{
		FPatchState& Patch = *Pair.Value;
		if (Patch.bBuilt && Patch.CapAngle <= Settings.ZoneGatherMaxAngle
			&& VoxelPlanetFarFieldPrivate::AngleBetween(Patch.CapDirection, Direction) <= Patch.CapAngle + AngularRadius)
		{
			Patch.bDirty = true;
		}
	}
}

void FVoxelPlanetFarField::InvalidateBox(const FBox2D& Region)
{
	if (!Settings.bHeightmap)
	{
		return;
	}
	for (TPair<uint64, TUniquePtr<FPatchState>>& Pair : Patches)
	{
		FPatchState& Patch = *Pair.Value;
		if (Patch.bBuilt && Patch.Box.GetSize().X <= Settings.ZoneGatherMaxSize && Patch.Box.Intersect(Region))
		{
			Patch.bDirty = true;
		}
	}
}

namespace VoxelPlanetFarFieldPrivate
{
	/** The distance from a point to a box's nearest point (0 inside) and to its farthest corner. */
	double DistanceToBoxMin(const FVector2D& Point, const FBox2D& Box)
	{
		const double DX = FMath::Max(FMath::Max(Box.Min.X - Point.X, Point.X - Box.Max.X), 0.0);
		const double DY = FMath::Max(FMath::Max(Box.Min.Y - Point.Y, Point.Y - Box.Max.Y), 0.0);
		return FMath::Sqrt(DX * DX + DY * DY);
	}

	double DistanceToBoxMax(const FVector2D& Point, const FBox2D& Box)
	{
		const double DX = FMath::Max(FMath::Abs(Point.X - Box.Min.X), FMath::Abs(Point.X - Box.Max.X));
		const double DY = FMath::Max(FMath::Abs(Point.Y - Box.Min.Y), FMath::Abs(Point.Y - Box.Max.Y));
		return FMath::Sqrt(DX * DX + DY * DY);
	}

	/** The world bounds' footprint box (invalid for an unbounded world). */
	FBox2D BoundsBox(const FVoxelWorldBounds& Bounds)
	{
		if (!Bounds.IsBounded())
		{
			return FBox2D(ForceInit);
		}
		const FVector2D Centre(Bounds.Center.X, Bounds.Center.Y);
		return FBox2D(Centre - Bounds.HalfExtent, Centre + Bounds.HalfExtent);
	}
}

double FVoxelPlanetFarField::GetCameraDistanceMin(const FPatchState& Patch) const
{
	return VoxelPlanetFarFieldPrivate::DistanceToBoxMin(FVector2D(View.Camera.X, View.Camera.Y), Patch.Box);
}

double FVoxelPlanetFarField::GetGroundDistanceMin(const FPatchState& Patch) const
{
	if (Settings.bHeightmap)
	{
		return VoxelPlanetFarFieldPrivate::DistanceToBoxMin(FVector2D(View.Viewer.X, View.Viewer.Y), Patch.Box);
	}
	const double Angle = VoxelPlanetFarFieldPrivate::AngleBetween(GroundDirection, Patch.CapDirection) - Patch.CapAngle;
	return View.GroundRadius * FMath::Max(Angle, 0.0);
}

double FVoxelPlanetFarField::GetGroundDistanceMax(const FPatchState& Patch) const
{
	if (Settings.bHeightmap)
	{
		return VoxelPlanetFarFieldPrivate::DistanceToBoxMax(FVector2D(View.Viewer.X, View.Viewer.Y), Patch.Box);
	}
	return View.GroundRadius * (VoxelPlanetFarFieldPrivate::AngleBetween(GroundDirection, Patch.CapDirection) + Patch.CapAngle);
}

bool FVoxelPlanetFarField::IsInBand(const FPatchState& Patch) const
{
	return View.BandRadius > 0.0 && GetGroundDistanceMin(Patch) < View.BandRadius * (Patch.bHasChildren ? 1.1 : 1.0);
}

FIntVector FVoxelPlanetFarField::GetChunkOf(const FVector& WorldPoint) const
{
	const FVector Local = (WorldPoint - Settings.GridOrigin) / FMath::Max(Settings.ChunkWorldSize, 1.0);
	return FIntVector(FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), FMath::FloorToInt32(Local.Z));
}

bool FVoxelPlanetFarField::UpdateCovered(FPatchState& Patch)
{
	if (View.ChunkStateVersion >= 0 && Patch.CoveredVersion == View.ChunkStateVersion)
	{
		return Patch.bCovered;
	}
	if (!Patch.bBuilt && Patch.SampleChunks.Num() == 0 && Surface.IsValid())
	{
		// The lazy test for a band patch that is not built: the chunks at a 9 x 9 grid of its surface (spacing at most a
		// quarter of a chunk), without zones.
		const TArray<FVoxelConditioningZone> NoZones;
		TSet<FIntVector> Chunks;
		const double RootTileSize = Settings.GetRootTileSize();
		for (int32 J = 0; J <= 8; ++J)
		{
			for (int32 I = 0; I <= 8; ++I)
			{
				if (Settings.bHeightmap)
				{
					const FVector2D XY = Patch.Key.GetPoint2D(Settings.GridOrigin, RootTileSize, I / 8.0, J / 8.0);
					Chunks.Add(GetChunkOf(FVector(XY.X, XY.Y, Surface->SampleHeight(XY.X, XY.Y, NoZones))));
					continue;
				}
				const FVector Direction = Patch.Key.GetDirection(I / 8.0, J / 8.0);
				Chunks.Add(GetChunkOf(Settings.Center + Direction * (Settings.Radius + Surface->SampleAltitude(Direction, NoZones))));
			}
		}
		Patch.SampleChunks = Chunks.Array();
	}
	const TArray<FIntVector>& Chunks = Patch.bBuilt ? Patch.SurfaceChunks : Patch.SampleChunks;
	bool bCovered = Chunks.Num() > 0 && ChunkDrawn != nullptr;
	if (bCovered)
	{
		for (const FIntVector& Chunk : Chunks)
		{
			if (!(*ChunkDrawn)(Chunk))
			{
				bCovered = false;
				break;
			}
		}
	}
	if (bCovered && !Patch.bCovered)
	{
		Patch.CoveredSince = View.Time;
	}
	if (!bCovered)
	{
		Patch.CoveredSince = -1.0;
	}
	Patch.bCovered = bCovered;
	Patch.CoveredVersion = View.ChunkStateVersion;
	return bCovered;
}

bool FVoxelPlanetFarField::IsCulled(const FPatchState& Patch) const
{
	if (Settings.bHeightmap)
	{
		// A plane has no horizon: a patch is culled past the far distance from the camera, and on a bounded world when its
		// footprint lies wholly outside the world's box (the sea in the box's corners past an island's circle stays: the
		// far ocean's open sea starts at the box).
		if (Settings.FarDistance > 0.0 && GetCameraDistanceMin(Patch) > Settings.FarDistance)
		{
			return true;
		}
		const FBox2D Bounds = VoxelPlanetFarFieldPrivate::BoundsBox(Settings.Bounds);
		return Bounds.bIsValid && !Bounds.Intersect(Patch.Box);
	}
	double Top = Settings.Radius + (Patch.bBuilt ? Patch.MaxAltitude : Settings.MaxAltitude);
	if (Settings.bSea)
	{
		Top = FMath::Max(Top, Settings.SeaRadius);
	}
	return IsBelowHorizon(Settings.Center, Settings.Radius + Settings.MinAltitude - Settings.Sink, View.Camera, Patch.CapDirection,
		Patch.CapAngle, Top);
}

bool FVoxelPlanetFarField::WantsSplit(const FPatchState& Patch) const
{
	if (Patch.Key.Level >= Settings.BandLevel)
	{
		return false;
	}
	if (IsInBand(Patch))
	{
		return true;
	}
	const double Distance = FMath::Max(FVector::Dist(View.Camera, Patch.BoundsCenter) - Patch.BoundsRadius, 1.0);
	double Error = Patch.Error;
	if (Settings.bSea && Patch.bHasSea)
	{
		Error = FMath::Max(Error, Patch.SeaSag);
	}
	const double Pixels = Error * View.PixelsPerRadian / Distance;
	return Pixels > View.MaxScreenError * (Patch.bHasChildren ? 0.7 : 1.0);
}

void FVoxelPlanetFarField::RequestBuild(FPatchState& Patch)
{
	if (Patch.bBuilding || (Patch.bResident && !Patch.bDirty))
	{
		return;
	}
	// Nearest first; a root leaves a hole until it is built, so roots go before everything. The root offset must leave the
	// distance its precision: 1e30 + a distance is 1e30 in a double, which sorted every non-root request alike, so with a
	// few builds a tick the patches under the camera could starve behind thousands of far ones (found on a plane, WM-MP1).
	double Distance;
	if (Settings.bHeightmap)
	{
		Distance = GetCameraDistanceMin(Patch);
	}
	else
	{
		const FVector CapPoint = Settings.Center + Patch.CapDirection * Settings.Radius;
		Distance = FMath::Max(FVector::Dist(View.Camera, CapPoint) - Patch.CapAngle * Settings.Radius, 0.0);
	}
	Requests.Add({ Patch.Key.Pack(), (Patch.Key.Level == 0 ? 0.0 : 1.0e12) + Distance });
}

bool FVoxelPlanetFarField::IsLazy(FPatchState& Patch, const FPatchState& Parent)
{
	// A band patch with nothing to draw stands in without a mesh: every chunk under it drawn, no sea, and not near the
	// window's edge (those are built while covered, ready for when their chunks unload).
	if (Patch.Key.Level < Settings.BandLevel || Patch.bResident)
	{
		return false;
	}
	bool bMayHaveSea = Settings.bSea && (Patch.bBuilt ? Patch.bHasSea : (Parent.bBuilt ? Parent.bHasSea : true));
	if (bMayHaveSea && Settings.bHeightmap && WaterTileQuery)
	{
		// A heightmap column's water tile draws its sea: a covered band patch over one has nothing to draw, sea included,
		// so it can be lazy (otherwise, released once hidden, it was built again a few seconds later and its parent stood
		// in over the window for the frames the build took, over and over while the viewer stood still).
		const FVector2D Centre = Patch.Box.GetCenter();
		const FIntPoint Column(FMath::FloorToInt32((Centre.X - Settings.GridOrigin.X) / Settings.ChunkWorldSize),
			FMath::FloorToInt32((Centre.Y - Settings.GridOrigin.Y) / Settings.ChunkWorldSize));
		bMayHaveSea = !WaterTileQuery(Column);
	}
	if (bMayHaveSea)
	{
		return false;
	}
	if (View.bAvoidStandIns)
	{
		// VP4: covered is enough wherever the patch lies; near the window's edge its build is requested meanwhile (Visit),
		// and nothing stands in for it in the meantime. Past the prebuild radius only once its chunks have been drawn
		// HideDelay: a chunk that has just appeared fades in over where the patch's mesh would be.
		return UpdateCovered(Patch)
			&& (GetGroundDistanceMax(Patch) < View.PrebuildRadius || View.Time - Patch.CoveredSince >= View.HideDelay);
	}
	return GetGroundDistanceMax(Patch) < View.PrebuildRadius && UpdateCovered(Patch);
}

bool FVoxelPlanetFarField::IsKeptBuilt(const FPatchState& Patch) const
{
	// Roots (the fallback of last resort) and band patches near the window's edge (shown the moment their chunks unload).
	return Patch.Key.Level == 0
		|| (Patch.Key.Level >= Settings.BandLevel && GetGroundDistanceMin(Patch) < View.BandRadius
			&& GetGroundDistanceMax(Patch) >= View.PrebuildRadius);
}

void FVoxelPlanetFarField::SetDrawn(FPatchState& Patch)
{
	bool bTerrain = true;
	if (Patch.Key.Level >= Settings.BandLevel && UpdateCovered(Patch) && Patch.CoveredSince >= 0.0
		&& View.Time - Patch.CoveredSince >= View.HideDelay)
	{
		bTerrain = false;
	}
	Patch.bDrawTerrain = bTerrain;
	// On a heightmap world the water tiles draw the sea over the streamed columns (a planet has no tiles: the far field's
	// sea is the only one), so a band patch's sea hides where its column's tile is drawn, and without a tile query with its
	// terrain (WM-MP1). The ring between the window's load and unload horizons holds chunks without tiles: its sea stays.
	bool bSea = Settings.bSea && Patch.bHasSea;
	if (bSea && Settings.bHeightmap && Patch.Key.Level >= Settings.BandLevel)
	{
		if (WaterTileQuery)
		{
			const FVector2D Centre = Patch.Box.GetCenter();
			const FIntPoint Column(FMath::FloorToInt32((Centre.X - Settings.GridOrigin.X) / Settings.ChunkWorldSize),
				FMath::FloorToInt32((Centre.Y - Settings.GridOrigin.Y) / Settings.ChunkWorldSize));
			bSea = !WaterTileQuery(Column);
		}
		else
		{
			bSea = bTerrain;
		}
	}
	Patch.bDrawSea = bSea;
	if (Patch.bDrawTerrain || Patch.bDrawSea)
	{
		Patch.LastDrawnTime = View.Time;
	}
}

void FVoxelPlanetFarField::ClearDrawnSubtree(const FPatchState& Patch)
{
	if (!Patch.bHasChildren)
	{
		return;
	}
	for (int32 Index = 0; Index < 4; ++Index)
	{
		if (FPatchState* Child = FindPatchMutable(Patch.Key.GetChild(Index).Pack()))
		{
			Child->bDrawTerrain = false;
			Child->bDrawSea = false;
			ClearDrawnSubtree(*Child);
		}
	}
}

void FVoxelPlanetFarField::ReleasePatch(FPatchState& Patch, FVoxelFarFieldChanges& OutChanges)
{
	if (!Patch.bResident)
	{
		return;
	}
	const uint64 Key = Patch.Key.Pack();
	// Built this update and gone already: the owner never sees it.
	if (BuiltThisUpdate.Remove(Key) > 0)
	{
		OutChanges.Built.Remove(Key);
	}
	else
	{
		OutChanges.Removed.Add(Key);
	}
	if (Patch.Mesh.IsValid())
	{
		MeshMemoryBytes -= static_cast<int64>(Patch.Mesh->GetAllocatedSize());
		Patch.Mesh.Reset();
	}
	Patch.bResident = false;
	Patch.bDrawTerrain = false;
	Patch.bDrawSea = false;
	Patch.bAppliedTerrain = false;
	Patch.bAppliedSea = false;
	++TotalReleased;
}

void FVoxelPlanetFarField::PruneChildren(FPatchState& Patch, FVoxelFarFieldChanges& OutChanges)
{
	if (!Patch.bHasChildren)
	{
		return;
	}
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const uint64 ChildKey = Patch.Key.GetChild(Index).Pack();
		TUniquePtr<FPatchState>* Found = Patches.Find(ChildKey);
		if (!Found)
		{
			continue;
		}
		FPatchState& Child = **Found;
		PruneChildren(Child, OutChanges);
		ReleasePatch(Child, OutChanges);
		Patches.Remove(ChildKey);
	}
	Patch.bHasChildren = false;
}

bool FVoxelPlanetFarField::Visit(uint64 PackedKey, FVoxelFarFieldChanges& OutChanges)
{
	FPatchState* Patch = FindPatchMutable(PackedKey);
	if (!Patch)
	{
		return false;
	}
	Patch->bDrawTerrain = false;
	Patch->bDrawSea = false;
	const bool bCulledBefore = Patch->bCulled;
	Patch->bCulled = IsCulled(*Patch);
	if (Patch->bCulled)
	{
		PruneChildren(*Patch, OutChanges);
		// A root is built while culled too (F4.2), so after a teleport onto a face the camera never saw it stands in at once.
		if (View.bBuildAllRoots && Patch->Key.Level == 0)
		{
			RequestBuild(*Patch);
		}
		return true;
	}
	// Coming up over the horizon as the camera moves (VP4): nothing was drawn for it, so it waits for its build drawing
	// nothing (a stand-in would replace everything drawn under that ancestor, the horizon and the voxel window included).
	// After a cut (a teleport) it may lie in full view: the ancestor stands in as before, or there would be a hole.
	const bool bWaitsUndrawn = View.bAvoidStandIns && bCulledBefore && !bCameraCut;
	// A band patch standing in without a mesh (its parent found it lazy this update: never built, or released while
	// covered) covers its area by drawing nothing; anything else unbuilt is built first.
	if (Patch->bLazy)
	{
		++CoveredBandVisits;
		// Near the window's edge it is wanted built for when its chunks unload (VP4): built meanwhile, still drawing nothing.
		if (View.bAvoidStandIns && IsKeptBuilt(*Patch))
		{
			RequestBuild(*Patch);
		}
		return true;
	}
	if (!Patch->bBuilt)
	{
		NoteUnready(*Patch);
		RequestBuild(*Patch);
		return bWaitsUndrawn;
	}
	if (Patch->bDirty && Patch->bResident)
	{
		RequestBuild(*Patch);
	}

	bool bSplit = WantsSplit(*Patch);
	bool bStandInOverWindow = false;
	if (!bSplit && Patch->bHasChildren && !Patch->bResident)
	{
		// Merging needs this patch's mesh back: its children keep drawing until it is built again.
		RequestBuild(*Patch);
		bSplit = true;
	}
	if (bSplit)
	{
		if (!Patch->bHasChildren)
		{
			for (int32 Index = 0; Index < 4; ++Index)
			{
				GetOrAddPatch(Patch->Key.GetChild(Index));
			}
			Patch->bHasChildren = true;
		}
		bool bCovered = true;
		const int64 CoveredBandVisitsBefore = CoveredBandVisits;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			FPatchState* Child = FindPatchMutable(Patch->Key.GetChild(Index).Pack());
			if (!Child)
			{
				bCovered = false;
				continue;
			}
			Child->bLazy = IsLazy(*Child, *Patch);
			bCovered &= Visit(Child->Key.Pack(), OutChanges);
		}
		if (bCovered)
		{
			return true;
		}
		// The subtree does not cover the area yet (builds in flight): this patch draws it, nothing under it does.
		ClearDrawnSubtree(*Patch);
		// Over band patches the voxel chunks cover, the stand-in shows through the voxel terrain wherever its chords rise
		// over it (VP4).
		bStandInOverWindow = CoveredBandVisits > CoveredBandVisitsBefore;
	}
	else
	{
		PruneChildren(*Patch, OutChanges);
	}
	if (Patch->bResident)
	{
		SetDrawn(*Patch);
		if (Patch->Key.Level >= Settings.BandLevel && Patch->bCovered)
		{
			++CoveredBandVisits;
		}
		if (bStandInOverWindow && Patch->bDrawTerrain)
		{
			++StandInCount;
			StandInMinLevel = StandInMinLevel < 0 ? Patch->Key.Level : FMath::Min(StandInMinLevel, Patch->Key.Level);
		}
		return true;
	}
	NoteUnready(*Patch);
	RequestBuild(*Patch);
	return bWaitsUndrawn;
}

void FVoxelPlanetFarField::UpdateHeightmapRoots(FVoxelFarFieldChanges& OutChanges)
{
	using namespace VoxelPlanetFarFieldPrivate;
	const double RootTileSize = Settings.GetRootTileSize();
	if (RootTileSize <= 0.0)
	{
		return;
	}
	// The root tiles wanted: those overlapping the square of FarDistance about the camera, and on a bounded world only those
	// overlapping the world's box. Without either limit, the tile under the camera alone.
	const FVector2D CameraXY(View.Camera.X, View.Camera.Y);
	FBox2D Region(ForceInit);
	if (Settings.FarDistance > 0.0)
	{
		Region = FBox2D(CameraXY - FVector2D(Settings.FarDistance), CameraXY + FVector2D(Settings.FarDistance));
	}
	const FBox2D Bounds = BoundsBox(Settings.Bounds);
	if (Bounds.bIsValid)
	{
		if (Region.bIsValid)
		{
			Region = FBox2D(FVector2D::Max(Region.Min, Bounds.Min), FVector2D::Min(Region.Max, Bounds.Max));
			Region.bIsValid = Region.Min.X <= Region.Max.X && Region.Min.Y <= Region.Max.Y;
		}
		else
		{
			Region = Bounds;
		}
	}
	if (!Region.bIsValid && Settings.FarDistance <= 0.0 && !Bounds.bIsValid)
	{
		Region = FBox2D(CameraXY, CameraXY);
	}

	TSet<uint64> Roots;
	if (Region.bIsValid)
	{
		const FVector2D GridXY(Settings.GridOrigin.X, Settings.GridOrigin.Y);
		int32 X0 = FMath::FloorToInt32((Region.Min.X - GridXY.X) / RootTileSize);
		int32 Y0 = FMath::FloorToInt32((Region.Min.Y - GridXY.Y) / RootTileSize);
		int32 X1 = FMath::FloorToInt32((Region.Max.X - GridXY.X) / RootTileSize);
		int32 Y1 = FMath::FloorToInt32((Region.Max.Y - GridXY.Y) / RootTileSize);
		// A bound on the tree count (a far distance many times a root tile): the nearest tiles to the camera.
		constexpr int32 MaxRootsPerAxis = 64;
		const int32 CameraX = FMath::FloorToInt32((CameraXY.X - GridXY.X) / RootTileSize);
		const int32 CameraY = FMath::FloorToInt32((CameraXY.Y - GridXY.Y) / RootTileSize);
		if (X1 - X0 + 1 > MaxRootsPerAxis)
		{
			X0 = FMath::Max(X0, CameraX - MaxRootsPerAxis / 2);
			X1 = FMath::Min(X1, X0 + MaxRootsPerAxis - 1);
		}
		if (Y1 - Y0 + 1 > MaxRootsPerAxis)
		{
			Y0 = FMath::Max(Y0, CameraY - MaxRootsPerAxis / 2);
			Y1 = FMath::Min(Y1, Y0 + MaxRootsPerAxis - 1);
		}
		// Nearest tiles first: a root leaves a hole until it is built, and the build requests take their order from here
		// only through their distance (RequestBuild), so the order of the visits does not matter for drawing.
		for (int32 Y = Y0; Y <= Y1; ++Y)
		{
			for (int32 X = X0; X <= X1; ++X)
			{
				const FVoxelFarFieldPatchKey Root(FVoxelFarFieldPatchKey::HeightmapFace, 0, X, Y);
				Roots.Add(Root.Pack());
				GetOrAddPatch(Root).bLazy = false;
				Visit(Root.Pack(), OutChanges);
			}
		}
	}
	// Roots out of reach go with their trees (a planet's six roots are kept for ever).
	for (const uint64 Old : HeightmapRoots)
	{
		if (Roots.Contains(Old))
		{
			continue;
		}
		if (FPatchState* Root = FindPatchMutable(Old))
		{
			PruneChildren(*Root, OutChanges);
			ReleasePatch(*Root, OutChanges);
			Patches.Remove(Old);
		}
	}
	HeightmapRoots = MoveTemp(Roots);
}

void FVoxelPlanetFarField::NoteUnready(FPatchState& Patch)
{
	if (Patch.Key.Level < Settings.BandLevel)
	{
		return;
	}
	if (UpdateCovered(Patch))
	{
		++UnreadyCoveredBand;
		++CoveredBandVisits;
		return;
	}
	++UnreadyUncoveredBand;
	if (bHasUndrawnChunk || !ChunkDrawn)
	{
		return;
	}
	for (const FIntVector& Chunk : Patch.bBuilt ? Patch.SurfaceChunks : Patch.SampleChunks)
	{
		if (!(*ChunkDrawn)(Chunk))
		{
			UndrawnChunk = Chunk;
			bHasUndrawnChunk = true;
			return;
		}
	}
}

void FVoxelPlanetFarField::ApplyBuild(FPatchState& Patch, const TSharedPtr<FVoxelFarFieldPatchMesh>& Mesh, FVoxelFarFieldChanges& OutChanges)
{
	if (Patch.Mesh.IsValid())
	{
		MeshMemoryBytes -= static_cast<int64>(Patch.Mesh->GetAllocatedSize());
	}
	Patch.Mesh = Mesh;
	MeshMemoryBytes += static_cast<int64>(Mesh->GetAllocatedSize());
	Patch.bBuilt = true;
	Patch.bResident = true;
	Patch.bBuilding = false;
	Patch.LastDrawnTime = View.Time;
	Patch.Origin = Mesh->Origin;
	Patch.Error = Mesh->Error;
	Patch.SeaSag = Mesh->SeaSag;
	Patch.SkirtDepth = Mesh->SkirtDepth;
	Patch.MinAltitude = Mesh->MinAltitude;
	Patch.MaxAltitude = Mesh->MaxAltitude;
	Patch.bHasSea = Mesh->bHasSea;
	Patch.bHasLand = Mesh->bHasLand;
	Patch.BoundsCenter = Mesh->BoundsCenter;
	Patch.BoundsRadius = Mesh->BoundsRadius;
	Patch.SurfaceChunks = Mesh->SurfaceChunks;
	Patch.CoveredVersion = -2; // the precise chunk set replaces the samples'
	// A new component starts with its sections as drawn now; the visibility pass reports it either way.
	Patch.bAppliedTerrain = false;
	Patch.bAppliedSea = false;
	OutChanges.Built.AddUnique(Patch.Key.Pack());
	BuiltThisUpdate.Add(Patch.Key.Pack());
	++BuiltSinceRead;
	++TotalBuilt;
	TotalBuildMs += Mesh->BuildMs;
}

void FVoxelPlanetFarField::DrainBuilds(FVoxelFarFieldChanges& OutChanges)
{
	TArray<FBuildResult> Done;
	{
		FScopeLock Lock(&Sink->Lock);
		Done = MoveTemp(Sink->Done);
		Sink->Done.Reset();
	}
	for (FBuildResult& Result : Done)
	{
		BuildsInFlight = FMath::Max(BuildsInFlight - 1, 0);
		FPatchState* Patch = FindPatchMutable(Result.Key);
		if (!Patch || Patch->BuildSerial != Result.Serial || !Result.Mesh.IsValid())
		{
			continue; // pruned, or rebuilt since
		}
		// A zone change while it built leaves bDirty set: the next update builds it again.
		ApplyBuild(*Patch, Result.Mesh, OutChanges);
	}
}

void FVoxelPlanetFarField::LaunchBuilds()
{
	Requests.Sort([](const FBuildRequest& A, const FBuildRequest& B) { return A.Priority < B.Priority; });
	int32 Slots = MaxBuildsInFlight - BuildsInFlight;
	int32 Waiting = 0;
	TSharedPtr<const FVoxelFarFieldSurface> SharedSurface = Surface;
	for (const FBuildRequest& Request : Requests)
	{
		FPatchState* Patch = FindPatchMutable(Request.Key);
		if (!Patch || Patch->bBuilding)
		{
			continue;
		}
		if (Slots <= 0)
		{
			++Waiting;
			continue;
		}
		TArray<FVoxelConditioningZone> Zones;
		if (Settings.bHeightmap)
		{
			if (ZoneGatherer2D && Patch->Box.GetSize().X <= Settings.ZoneGatherMaxSize)
			{
				ZoneGatherer2D(Patch->Box, Zones);
			}
		}
		else if (ZoneGatherer && Patch->CapAngle <= Settings.ZoneGatherMaxAngle)
		{
			ZoneGatherer(Patch->CapDirection, Patch->CapAngle, Zones);
		}
		Patch->bBuilding = true;
		Patch->bDirty = false;
		const uint32 Serial = ++Patch->BuildSerial;
		++BuildsInFlight;
		--Slots;
		const FVoxelFarFieldPatchKey Key = Patch->Key;
		TSharedPtr<FBuildSink, ESPMode::ThreadSafe> TaskSink = Sink;
		auto Build = [TaskSink, BuildSettings = Settings, SharedSurface, Zones = MoveTemp(Zones), Key, Serial]()
		{
			TSharedPtr<FVoxelFarFieldPatchMesh> Mesh = MakeShared<FVoxelFarFieldPatchMesh>();
			BuildPatch(BuildSettings, *SharedSurface, Zones, Key, *Mesh);
			FScopeLock Lock(&TaskSink->Lock);
			TaskSink->Done.Add({ Key.Pack(), Serial, MoveTemp(Mesh) });
		};
		if (bSynchronousBuilds)
		{
			Build();
		}
		else
		{
			Async(EAsyncExecution::ThreadPool, MoveTemp(Build));
		}
	}
	PendingBuildCount = Waiting;
}

void FVoxelPlanetFarField::Update(const FVoxelFarFieldView& InView, TFunctionRef<bool(const FIntVector&)> IsChunkDrawn, FVoxelFarFieldChanges& OutChanges)
{
	OutChanges.Reset();
	if (!Surface.IsValid())
	{
		return;
	}
	View = InView;
	if (View.GroundRadius <= 0.0)
	{
		View.GroundRadius = Settings.Radius;
	}
	GroundDirection = (View.Viewer - Settings.Center).GetSafeNormal();
	if (GroundDirection.IsNearlyZero())
	{
		GroundDirection = FVector::UpVector;
	}
	ChunkDrawn = &IsChunkDrawn;
	BuiltThisUpdate.Reset();
	// More than a chunk since the last update is a cut, not motion (VP4's horizon rule holds for motion only).
	bCameraCut = !bHasLastCamera || FVector::DistSquared(View.Camera, LastCamera) > FMath::Square(Settings.ChunkWorldSize);
	LastCamera = View.Camera;
	bHasLastCamera = true;
	StandInCount = 0;
	StandInMinLevel = -1;
	UnreadyCoveredBand = 0;
	UnreadyUncoveredBand = 0;
	bHasUndrawnChunk = false;

	DrainBuilds(OutChanges);

	Requests.Reset();
	if (Settings.bHeightmap)
	{
		UpdateHeightmapRoots(OutChanges);
	}
	else
	{
		for (int32 Face = 0; Face < 6; ++Face)
		{
			const FVoxelFarFieldPatchKey Root(Face, 0, 0, 0);
			GetOrAddPatch(Root).bLazy = false;
			Visit(Root.Pack(), OutChanges);
		}
	}

	// Release the components of patches hidden a while (a parent under drawn children, a covered band patch inside the
	// window): they build again when they are needed. Roots and band patches near the window's edge stay.
	if (View.ReleaseDelay >= 0.0)
	{
		for (TPair<uint64, TUniquePtr<FPatchState>>& Pair : Patches)
		{
			FPatchState& Patch = *Pair.Value;
			if (Patch.bResident && !Patch.bDrawTerrain && !Patch.bDrawSea && !Patch.bBuilding
				&& View.Time - Patch.LastDrawnTime > View.ReleaseDelay && !IsKeptBuilt(Patch))
			{
				ReleasePatch(Patch, OutChanges);
			}
		}
	}

	// What the owner must show or hide (and every patch built this update, whose component is new).
	DrawnPatchCount = 0;
	for (TPair<uint64, TUniquePtr<FPatchState>>& Pair : Patches)
	{
		FPatchState& Patch = *Pair.Value;
		if (Patch.bDrawTerrain || Patch.bDrawSea)
		{
			++DrawnPatchCount;
		}
		if (Patch.bResident && (Patch.bDrawTerrain != Patch.bAppliedTerrain || Patch.bDrawSea != Patch.bAppliedSea
			|| BuiltThisUpdate.Contains(Pair.Key)))
		{
			OutChanges.Visibility.Add(Pair.Key);
			Patch.bAppliedTerrain = Patch.bDrawTerrain;
			Patch.bAppliedSea = Patch.bDrawSea;
		}
	}

	TotalStandIns += StandInCount;
	StandInUpdates += StandInCount > 0 ? 1 : 0;

	LaunchBuilds();
	ChunkDrawn = nullptr;
}
