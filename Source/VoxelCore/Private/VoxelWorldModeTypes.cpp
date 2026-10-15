// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWorldModeTypes.h"

// ==================== FVoxelWorldBounds ====================

bool FVoxelWorldBounds::ContainsXY(double X, double Y) const
{
	const double DX = X - Center.X;
	const double DY = Y - Center.Y;
	switch (Shape)
	{
	case EVoxelWorldBoundsShape::Circle:
	case EVoxelWorldBoundsShape::Shell:
		return DX * DX + DY * DY <= static_cast<double>(OuterRadius) * OuterRadius;
	case EVoxelWorldBoundsShape::Rectangle:
		return FMath::Abs(DX) <= HalfExtent.X && FMath::Abs(DY) <= HalfExtent.Y;
	case EVoxelWorldBoundsShape::Unbounded:
	default:
		return true;
	}
}

bool FVoxelWorldBounds::Contains(const FVector& WorldPos) const
{
	if (Shape == EVoxelWorldBoundsShape::Shell)
	{
		return FVector::DistSquared(WorldPos, Center) <= static_cast<double>(OuterRadius) * OuterRadius;
	}
	return ContainsXY(WorldPos.X, WorldPos.Y);
}

FBox2D FVoxelWorldBounds::GetBox2D() const
{
	if (!IsBounded())
	{
		return FBox2D(ForceInit);
	}
	const FVector2D C(Center.X, Center.Y);
	return FBox2D(C - HalfExtent, C + HalfExtent);
}

// ==================== FVoxelWorldFingerprint ====================

namespace VoxelWorldFingerprintPrivate
{
	FString WorldModeName(EWorldMode Mode)
	{
		const UEnum* Enum = StaticEnum<EWorldMode>();
		return Enum ? Enum->GetNameStringByValue(static_cast<int64>(Mode)) : FString::FromInt(static_cast<int32>(Mode));
	}

	/** "Name=Value;Name=Value" -> [(Name, Value)], in order. */
	TArray<TPair<FString, FString>> SplitShape(const FString& Shape)
	{
		TArray<FString> Parts;
		Shape.ParseIntoArray(Parts, TEXT(";"), /*InCullEmpty=*/true);
		TArray<TPair<FString, FString>> Out;
		for (const FString& Part : Parts)
		{
			FString Name, Value;
			if (!Part.Split(TEXT("="), &Name, &Value))
			{
				Name = Part;
			}
			Out.Emplace(Name, Value);
		}
		return Out;
	}
}

bool FVoxelWorldFingerprint::Matches(const FVoxelWorldFingerprint& Other) const
{
	return WorldMode == Other.WorldMode && GenerationVersion == Other.GenerationVersion && Shape.Equals(Other.Shape, ESearchCase::CaseSensitive);
}

FString FVoxelWorldFingerprint::DescribeDifferences(const FVoxelWorldFingerprint& Other) const
{
	TArray<FString> Clauses;
	if (WorldMode != Other.WorldMode)
	{
		Clauses.Add(FString::Printf(TEXT("world mode %s vs %s"), *VoxelWorldFingerprintPrivate::WorldModeName(WorldMode), *VoxelWorldFingerprintPrivate::WorldModeName(Other.WorldMode)));
	}
	if (GenerationVersion != Other.GenerationVersion)
	{
		Clauses.Add(FString::Printf(TEXT("generation version %d vs %d"), GenerationVersion, Other.GenerationVersion));
	}
	if (!Shape.Equals(Other.Shape, ESearchCase::CaseSensitive))
	{
		// Name the params that differ; a param only one side has shows as missing on the other.
		const TArray<TPair<FString, FString>> Mine = VoxelWorldFingerprintPrivate::SplitShape(Shape);
		const TArray<TPair<FString, FString>> Theirs = VoxelWorldFingerprintPrivate::SplitShape(Other.Shape);
		TSet<FString> Seen;
		for (const TPair<FString, FString>& Param : Mine)
		{
			Seen.Add(Param.Key);
			const TPair<FString, FString>* Match = Theirs.FindByPredicate([&Param](const TPair<FString, FString>& P) { return P.Key == Param.Key; });
			if (!Match)
			{
				Clauses.Add(FString::Printf(TEXT("%s=%s vs missing"), *Param.Key, *Param.Value));
			}
			else if (Match->Value != Param.Value)
			{
				Clauses.Add(FString::Printf(TEXT("%s=%s vs %s"), *Param.Key, *Param.Value, *Match->Value));
			}
		}
		for (const TPair<FString, FString>& Param : Theirs)
		{
			if (!Seen.Contains(Param.Key))
			{
				Clauses.Add(FString::Printf(TEXT("%s missing vs %s"), *Param.Key, *Param.Value));
			}
		}
	}
	return FString::Join(Clauses, TEXT("; "));
}

FString FVoxelWorldFingerprint::ToString() const
{
	return FString::Printf(TEXT("%s [%s] gen %d"), *VoxelWorldFingerprintPrivate::WorldModeName(WorldMode), *Shape, GenerationVersion);
}
