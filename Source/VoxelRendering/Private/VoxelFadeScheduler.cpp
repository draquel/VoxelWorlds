// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelFadeScheduler.h"

bool FVoxelFadeScheduler::Start(const FIntVector4& Key, double NowSeconds, bool bHold)
{
	FEntry* Entry = Entries.Find(Key);
	if (!Entry)
	{
		if (Entries.Num() >= MaxConcurrent)
		{
			return false;
		}
		Entry = &Entries.Add(Key);
	}
	Entry->StartSeconds = NowSeconds;
	Entry->bHeld = bHold;
	Entry->HeldSinceSeconds = bHold ? NowSeconds : 0.0;
	// A swap now has a previous mesh to fade from, whatever this entry was doing before.
	Entry->bAppear = false;
	return true;
}

bool FVoxelFadeScheduler::StartAppear(const FIntVector4& Key, double NowSeconds, bool bHold, double DistanceToViewer)
{
	// Near geometry appears at once: a see-through patch in front of the player reads as a glitch,
	// while at the frontier a fade only replaces a pop.
	if (!PassesAppearGate(DistanceToViewer) || Entries.Contains(Key) || Entries.Num() >= MaxConcurrent)
	{
		return false;
	}
	FEntry& Entry = Entries.Add(Key);
	Entry.StartSeconds = NowSeconds;
	Entry.bHeld = bHold;
	Entry.HeldSinceSeconds = bHold ? NowSeconds : 0.0;
	Entry.bAppear = true;
	return true;
}

float FVoxelFadeScheduler::GetRemainingSeconds(const FIntVector4& Key, double NowSeconds, float DurationSeconds) const
{
	const FEntry* Entry = Entries.Find(Key);
	if (!Entry || Entry->bHeld)
	{
		return 0.0f;
	}
	const double Duration = FMath::Max(0.02, static_cast<double>(DurationSeconds));
	return static_cast<float>(FMath::Max(0.0, Entry->StartSeconds + Duration - NowSeconds));
}

bool FVoxelFadeScheduler::Release(const FIntVector4& Key, double NowSeconds)
{
	FEntry* Entry = Entries.Find(Key);
	if (!Entry || !Entry->bHeld)
	{
		return false;
	}
	Entry->bHeld = false;
	Entry->StartSeconds = NowSeconds;
	return true;
}

bool FVoxelFadeScheduler::Cancel(const FIntVector4& Key)
{
	return Entries.Remove(Key) > 0;
}

void FVoxelFadeScheduler::CancelAll(TArray<FIntVector4>& OutCancelled)
{
	Entries.GetKeys(OutCancelled);
	Entries.Reset();
}

void FVoxelFadeScheduler::Advance(double NowSeconds, float DurationSeconds, double HoldTimeoutSeconds, FAdvanceResult& Out)
{
	Out.Alphas.Reset();
	Out.Completed.Reset();
	Out.TimedOutHolds.Reset();

	const double Duration = FMath::Max(0.02, static_cast<double>(DurationSeconds));
	Out.Alphas.Reserve(Entries.Num());

	for (auto It = Entries.CreateIterator(); It; ++It)
	{
		FEntry& Entry = It.Value();
		if (Entry.bHeld)
		{
			// Failsafe: a hold nobody releases (a lost seam, a caller bug) must not freeze the
			// swap forever. The chunk manager's own cap is far shorter; this only catches leaks.
			if (HoldTimeoutSeconds > 0.0 && NowSeconds - Entry.HeldSinceSeconds >= HoldTimeoutSeconds)
			{
				Entry.bHeld = false;
				Entry.StartSeconds = NowSeconds;
				Out.TimedOutHolds.Add(It.Key());
			}
			else
			{
				Out.Alphas.Emplace(It.Key(), 0.0f);
				continue;
			}
		}

		const float Alpha = static_cast<float>((NowSeconds - Entry.StartSeconds) / Duration);
		if (Alpha >= 1.0f)
		{
			Out.Completed.Add(It.Key());
			It.RemoveCurrent();
			continue;
		}
		Out.Alphas.Emplace(It.Key(), FMath::Max(0.0f, Alpha));
	}
}
