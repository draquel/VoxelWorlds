// Copyright Daniel Raquel. All Rights Reserved.

// Test conditioning lines from a cvar (the terrain lines plan's L2): voxel.World.TestLines names lines that a chunk
// manager adds at the end of its Initialize, before it builds a generation request, so a PIE started with the cvar set
// generates every chunk with them. The fixture for the shots, the bench and the look of a line until the hand-placed
// line (the plan's L4) exists: a line here follows the natural terrain along a path, as that component will by default.

#include "Diagnostics/VoxelConsoleHelpers.h"
#include "VoxelChunkManager.h"
#include "VoxelStreaming.h"
#include "VoxelConditioningLine.h"
#include "VoxelCubeSphere.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "IVoxelWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "HAL/IConsoleManager.h"

#if !UE_BUILD_SHIPPING

static TAutoConsoleVariable<FString> CVarVoxelWorldTestLines(
	TEXT("voxel.World.TestLines"),
	TEXT(""),
	TEXT("Test conditioning lines a voxel world adds when it starts (set it before PIE): lines separated by ';', each ")
	TEXT("'<blend>[,key=value...]:<point>:<point>[:<point>...]'. blend: level, lower or raise. Points: 'x,y' on a heightmap world, ")
	TEXT("'lat,lon' in degrees on a planet (joined along great circles). Keys: hw (half width in voxels, 3), falloff (voxels, 4), ")
	TEXT("step (vertex spacing in voxels, 2), offset (uu added to the target, 0), smooth (uu of the natural height averaged each way ")
	TEXT("along the line, 1200), grade (the target's steepest slope as a fraction, 0 = no limit), flat (1: one target all along, the ")
	TEXT("natural height at the first point plus the offset), strength (1). The target follows the natural terrain. Empty: none."),
	ECVF_Default);

namespace VoxelTestLines
{
	/** A path's points resampled every Spacing: along straight legs on a heightmap world, along great circles on a planet. */
	void Resample(const TArray<FVector>& Points, bool bPlanet, double PlanetRadius, double Spacing, TArray<FVector>& Out)
	{
		Out.Reset();
		for (int32 Index = 0; Index + 1 < Points.Num(); ++Index)
		{
			const FVector& A = Points[Index];
			const FVector& B = Points[Index + 1];
			const double Angle = bPlanet ? FMath::Atan2(FVector::CrossProduct(A, B).Size(), FVector::DotProduct(A, B)) : 0.0;
			const double Length = bPlanet ? Angle * PlanetRadius : (B - A).Size();
			const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Length / FMath::Max(Spacing, 1.0)));
			for (int32 Step = (Index == 0 ? 0 : 1); Step <= Steps; ++Step)
			{
				const double T = static_cast<double>(Step) / Steps;
				if (bPlanet && Angle > UE_DOUBLE_SMALL_NUMBER)
				{
					// Along the great circle from A to B.
					Out.Add((A * FMath::Sin((1.0 - T) * Angle) + B * FMath::Sin(T * Angle)).GetSafeNormal());
				}
				else
				{
					Out.Add(FMath::Lerp(A, B, T));
				}
			}
		}
	}

	/** One line of the cvar's text; false with a reason when it does not parse. */
	bool Parse(const FString& Text, bool bPlanet, float VoxelSize, FVoxelConditioningLineParams& OutParams, TArray<FVector>& OutPoints,
		double& OutSpacing, float& OutOffset, double& OutSmooth, double& OutGrade, bool& bOutFlat, FString& OutError)
	{
		TArray<FString> Fields;
		Text.ParseIntoArray(Fields, TEXT(":"), true);
		if (Fields.Num() < 3)
		{
			OutError = TEXT("needs '<blend>[,key=value...]' and two or more points");
			return false;
		}
		TArray<FString> Options;
		Fields[0].ParseIntoArray(Options, TEXT(","), true);
		float HalfWidthVoxels = 3.0f;
		float FalloffVoxels = 4.0f;
		float StepVoxels = 2.0f;
		OutOffset = 0.0f;
		OutSmooth = 1200.0;
		OutGrade = 0.0;
		bOutFlat = false;
		OutParams.Strength = 1.0f;
		for (int32 Index = 0; Index < Options.Num(); ++Index)
		{
			const FString Option = Options[Index].TrimStartAndEnd().ToLower();
			FString Key, Value;
			if (!Option.Split(TEXT("="), &Key, &Value))
			{
				if (Option == TEXT("level")) { OutParams.Blend = EVoxelConditioningLineBlend::Level; }
				else if (Option == TEXT("lower")) { OutParams.Blend = EVoxelConditioningLineBlend::LowerOnly; }
				else if (Option == TEXT("raise")) { OutParams.Blend = EVoxelConditioningLineBlend::RaiseOnly; }
				else
				{
					OutError = FString::Printf(TEXT("unknown blend '%s' (level, lower, raise)"), *Option);
					return false;
				}
				continue;
			}
			const float Number = FCString::Atof(*Value);
			if (Key == TEXT("hw")) { HalfWidthVoxels = Number; }
			else if (Key == TEXT("falloff")) { FalloffVoxels = Number; }
			else if (Key == TEXT("step")) { StepVoxels = Number; }
			else if (Key == TEXT("offset")) { OutOffset = Number; }
			else if (Key == TEXT("smooth")) { OutSmooth = Number; }
			else if (Key == TEXT("grade")) { OutGrade = Number; }
			else if (Key == TEXT("flat")) { bOutFlat = Number != 0.0f; }
			else if (Key == TEXT("strength")) { OutParams.Strength = Number; }
			else
			{
				OutError = FString::Printf(TEXT("unknown key '%s'"), *Key);
				return false;
			}
		}
		OutParams.FalloffWidth = FalloffVoxels * VoxelSize;
		OutParams.bRadial = bPlanet;
		OutSpacing = FMath::Max(StepVoxels, 0.25f) * VoxelSize;
		OutPoints.Reset();
		for (int32 Index = 1; Index < Fields.Num(); ++Index)
		{
			FString First, Second;
			if (!Fields[Index].Split(TEXT(","), &First, &Second))
			{
				OutError = FString::Printf(TEXT("point '%s' is not two numbers"), *Fields[Index]);
				return false;
			}
			const double U = FCString::Atod(*First);
			const double V = FCString::Atod(*Second);
			OutPoints.Add(bPlanet ? FVoxelCubeSphere::LatLonToDirection(U, V) : FVector(U, V, 0.0));
		}
		// The half width rides on the vertices, filled by the caller.
		OutParams.Vertices.Reset();
		OutParams.Vertices.Add(FVoxelConditioningLineVertex(FVector::ZeroVector, 0.0f, HalfWidthVoxels * VoxelSize));
		return true;
	}
}

void VoxelStreamingDiagnostics::AddTestConditioningLines(UVoxelChunkManager& Manager)
{
	const FString Text = CVarVoxelWorldTestLines.GetValueOnGameThread();
	const UVoxelWorldConfiguration* Configuration = Manager.GetConfiguration();
	const IVoxelWorldMode* WorldMode = Manager.GetWorldMode();
	if (Text.IsEmpty() || !Configuration || !WorldMode)
	{
		return;
	}
	const bool bPlanet = Configuration->WorldMode == EWorldMode::SphericalPlanet;
	const double PlanetRadius = bPlanet ? FVoxelWorldModeFactory::MakePlanetParams(*Configuration).PlanetRadius : 0.0;
	const FVoxelNoiseParams NoiseParams = Configuration->GetNoiseParams();
	const auto NaturalHeight = [&](const FVector& Position)
	{
		return bPlanet
			? static_cast<const FSphericalPlanetWorldMode*>(WorldMode)->GetTerrainAltitude(Position, NoiseParams)
			: static_cast<float>(WorldMode->GetSurfaceOnUpLine(FVector(Position.X, Position.Y, 0.0), NoiseParams).Position.Z);
	};

	TArray<FString> LineTexts;
	Text.ParseIntoArray(LineTexts, TEXT(";"), true);
	for (int32 LineIndex = 0; LineIndex < LineTexts.Num(); ++LineIndex)
	{
		FVoxelConditioningLineParams Params;
		TArray<FVector> Points;
		double Spacing = 0.0;
		float Offset = 0.0f;
		double Smooth = 0.0;
		double Grade = 0.0;
		bool bFlat = false;
		FString Error;
		if (!VoxelTestLines::Parse(LineTexts[LineIndex], bPlanet, Configuration->VoxelSize, Params, Points, Spacing, Offset, Smooth, Grade, bFlat, Error))
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.World.TestLines: line %d ('%s'): %s."), LineIndex + 1, *LineTexts[LineIndex], *Error);
			continue;
		}
		const float HalfWidth = Params.Vertices[0].HalfWidth;
		TArray<FVector> Path;
		VoxelTestLines::Resample(Points, bPlanet, PlanetRadius, Spacing, Path);

		// The target: the natural height along the path, averaged over Smooth each way, held to the grade, plus the offset.
		TArray<float> Natural;
		Natural.Reserve(Path.Num());
		for (const FVector& Position : Path)
		{
			Natural.Add(NaturalHeight(Position));
		}
		const int32 Window = FMath::Max(0, FMath::RoundToInt32(Smooth / Spacing));
		TArray<float> Target;
		Target.SetNumUninitialized(Path.Num());
		for (int32 Index = 0; Index < Path.Num(); ++Index)
		{
			double Sum = 0.0;
			int32 Count = 0;
			for (int32 Other = FMath::Max(0, Index - Window); Other <= FMath::Min(Path.Num() - 1, Index + Window); ++Other)
			{
				Sum += Natural[Other];
				++Count;
			}
			Target[Index] = static_cast<float>(Sum / Count);
		}
		if (Grade > 0.0)
		{
			const float MaxStep = static_cast<float>(Grade * Spacing);
			for (int32 Index = 1; Index < Path.Num(); ++Index)
			{
				Target[Index] = FMath::Clamp(Target[Index], Target[Index - 1] - MaxStep, Target[Index - 1] + MaxStep);
			}
			for (int32 Index = Path.Num() - 2; Index >= 0; --Index)
			{
				Target[Index] = FMath::Clamp(Target[Index], Target[Index + 1] - MaxStep, Target[Index + 1] + MaxStep);
			}
		}

		Params.Vertices.Reset(Path.Num());
		for (int32 Index = 0; Index < Path.Num(); ++Index)
		{
			Params.Vertices.Add(FVoxelConditioningLineVertex(Path[Index], (bFlat ? Natural[0] : Target[Index]) + Offset, HalfWidth));
		}
		Params.Id = LineIndex + 1;
		UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.World.TestLines: line %d: %d vertices from %d points, half width %.0f, target %.0f to %.0f."),
			LineIndex + 1, Params.Vertices.Num(), Points.Num(), HalfWidth,
			Params.Vertices.Num() > 0 ? Params.Vertices[0].TargetHeight : 0.0f, Params.Vertices.Num() > 0 ? Params.Vertices.Last().TargetHeight : 0.0f);
		Manager.AddConditioningLine(Params);
	}
}

#endif // !UE_BUILD_SHIPPING
