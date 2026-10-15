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

double FVoxelWorldBounds::GetDistanceToEdgeXY(double X, double Y) const
{
	const double DX = X - Center.X;
	const double DY = Y - Center.Y;
	switch (Shape)
	{
	case EVoxelWorldBoundsShape::Circle:
	case EVoxelWorldBoundsShape::Shell:
		return static_cast<double>(OuterRadius) - FMath::Sqrt(DX * DX + DY * DY);
	case EVoxelWorldBoundsShape::Rectangle:
	{
		// Box signed distance, sign flipped so inside is positive.
		const double QX = FMath::Abs(DX) - HalfExtent.X;
		const double QY = FMath::Abs(DY) - HalfExtent.Y;
		const double Outside = FMath::Sqrt(FMath::Square(FMath::Max(QX, 0.0)) + FMath::Square(FMath::Max(QY, 0.0)));
		const double Inside = FMath::Min(FMath::Max(QX, QY), 0.0);
		return -(Outside + Inside);
	}
	case EVoxelWorldBoundsShape::Unbounded:
	default:
		return TNumericLimits<double>::Max();
	}
}

bool FVoxelWorldBounds::ContainsCircleXY(double X, double Y, double Radius) const
{
	if (!IsBounded())
	{
		return true;
	}
	if (Shape == EVoxelWorldBoundsShape::Rectangle)
	{
		return FMath::Abs(X - Center.X) + Radius <= HalfExtent.X && FMath::Abs(Y - Center.Y) + Radius <= HalfExtent.Y;
	}
	return GetDistanceToEdgeXY(X, Y) >= Radius;
}

bool FVoxelWorldBounds::IntersectsBox2D(const FBox2D& Box) const
{
	if (!IsBounded())
	{
		return true;
	}
	if (Shape == EVoxelWorldBoundsShape::Rectangle)
	{
		return Box.Intersect(GetBox2D());
	}
	// Circle (and a shell's disc): the box's nearest point to the centre is within the radius.
	const double NX = FMath::Clamp(static_cast<double>(Center.X), Box.Min.X, Box.Max.X);
	const double NY = FMath::Clamp(static_cast<double>(Center.Y), Box.Min.Y, Box.Max.Y);
	return ContainsXY(NX, NY);
}

FVector2D FVoxelWorldBounds::ClampXY(double X, double Y, double Margin) const
{
	if (!IsBounded())
	{
		return FVector2D(X, Y);
	}
	if (Shape == EVoxelWorldBoundsShape::Rectangle)
	{
		const double MX = FMath::Max(HalfExtent.X - Margin, 0.0);
		const double MY = FMath::Max(HalfExtent.Y - Margin, 0.0);
		return FVector2D(
			FMath::Clamp(X, Center.X - MX, Center.X + MX),
			FMath::Clamp(Y, Center.Y - MY, Center.Y + MY));
	}
	const double Radius = FMath::Max(static_cast<double>(OuterRadius) - Margin, 0.0);
	const FVector2D C(Center.X, Center.Y);
	const FVector2D Offset = FVector2D(X, Y) - C;
	const double Length = Offset.Size();
	return Length <= Radius ? FVector2D(X, Y) : C + Offset * (Radius / Length);
}

FVector2D FVoxelWorldBounds::GetOutwardNormalXY(double X, double Y) const
{
	const double DX = X - Center.X;
	const double DY = Y - Center.Y;
	switch (Shape)
	{
	case EVoxelWorldBoundsShape::Circle:
	case EVoxelWorldBoundsShape::Shell:
	{
		const FVector2D Offset(DX, DY);
		return Offset.IsNearlyZero() ? FVector2D(1.0, 0.0) : Offset.GetSafeNormal();
	}
	case EVoxelWorldBoundsShape::Rectangle:
	{
		const double QX = FMath::Abs(DX) - HalfExtent.X;
		const double QY = FMath::Abs(DY) - HalfExtent.Y;
		if (QX > 0.0 || QY > 0.0)
		{
			// Outside: away from the nearest point of the box (a side, or a corner).
			return FVector2D(FMath::Max(QX, 0.0) * FMath::Sign(DX), FMath::Max(QY, 0.0) * FMath::Sign(DY)).GetSafeNormal();
		}
		// Inside: the normal of the nearest side.
		return QX >= QY ? FVector2D(DX >= 0.0 ? 1.0 : -1.0, 0.0) : FVector2D(0.0, DY >= 0.0 ? 1.0 : -1.0);
	}
	case EVoxelWorldBoundsShape::Unbounded:
	default:
		return FVector2D::ZeroVector;
	}
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
