#include "BF6PhaseTimer.h"
#include "BF6Internal.h"
#include "HAL/PlatformTime.h"

namespace BF6Ext
{

FPhaseTimer::FPhaseTimer(const FString& InJobName)
	: JobName(InJobName)
	, Started(FPlatformTime::Seconds())
{
}

void FPhaseTimer::CloseCurrent()
{
	if (Current.IsEmpty()) { return; }
	const double Spent = FPlatformTime::Seconds() - PhaseStarted;
	// MERGED BY NAME, because the phase that matters most is entered once per
	// batch. One row per batch would be hundreds of lines saying nothing; the
	// total for "objects" against the total for "ground" is the comparison
	// anybody reading this actually wants.
	for (TPair<FString, double>& R : Rows)
	{
		if (R.Key == Current) { R.Value += Spent; Current.Reset(); return; }
	}
	Rows.Emplace(Current, Spent);
	Current.Reset();
}

void FPhaseTimer::Enter(const FString& PhaseName)
{
	CloseCurrent();
	Current = PhaseName;
	PhaseStarted = FPlatformTime::Seconds();
}

void FPhaseTimer::Stop()
{
	CloseCurrent();
}

double FPhaseTimer::Total() const
{
	return FPlatformTime::Seconds() - Started;
}

FString FPhaseTimer::ToString() const
{
	if (Rows.Num() == 0) { return FString(); }

	double Sum = 0.0;
	int32 Widest = 0;
	for (const TPair<FString, double>& R : Rows)
	{
		Sum += R.Value;
		Widest = FMath::Max(Widest, R.Key.Len());
	}
	const double Whole = FMath::Max(Sum, KINDA_SMALL_NUMBER);

	// Slowest first: the top line is the one worth working on, and a table
	// ordered by when a phase happened buries it in the middle.
	TArray<TPair<FString, double>> Sorted = Rows;
	Sorted.Sort([](const TPair<FString, double>& A, const TPair<FString, double>& B)
	{
		return A.Value > B.Value;
	});

	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("%s: %.2fs over %d phase(s)"), *JobName, Sum, Rows.Num()));
	for (const TPair<FString, double>& R : Sorted)
	{
		const double Pct = R.Value / Whole * 100.0;
		// A bar, so the shape of the job is readable without doing arithmetic.
		const int32 Bars = FMath::Clamp(FMath::RoundToInt(Pct / 2.5), 0, 40);
		Lines.Add(FString::Printf(TEXT("   %-*s %8.2fs  %5.1f%%  %s"),
			Widest, *R.Key, R.Value, Pct, *FString::ChrN(Bars, TEXT('#'))));
	}
	return FString::Join(Lines, TEXT("\n"));
}

void FPhaseTimer::Report() const
{
	const FString Text = ToString();
	if (Text.IsEmpty()) { return; }
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	for (const FString& L : Lines)
	{
		UE_LOG(LogBF6, Display, TEXT("%s"), *L);
	}
}

} // namespace BF6Ext
