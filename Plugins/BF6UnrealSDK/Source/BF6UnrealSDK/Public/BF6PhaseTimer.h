#pragma once

#include "CoreMinimal.h"

namespace BF6Ext
{

// WHERE A LONG JOB SPENDS ITS TIME.
//
// Every job in this tool that shows a progress bar reported one number at the
// end: how long the whole thing took. That is enough to know a build is slow
// and useless for knowing WHY, so every attempt to speed one up started by
// guessing which phase to look at.
//
// This records each phase as it runs and prints one aligned table when the job
// ends, with each phase's share of the total. It is deliberately cheap - one
// FPlatformTime::Seconds() per phase boundary, nothing per item - so it can
// stay switched on in a shipping build rather than being something you add when
// you already suspect a problem.
//
// Game thread only, like the jobs it measures.
class BF6UNREALSDK_API FPhaseTimer
{
public:
	explicit FPhaseTimer(const FString& InJobName);

	// End the phase that was running and start a new one. The first call just
	// starts the first phase.
	void Enter(const FString& PhaseName);

	// End the phase that was running. Safe to call twice.
	void Stop();

	// Seconds since the timer was made.
	double Total() const;

	// The phases in the order they ran. Repeated names are merged, so a phase
	// entered once per batch reports its total rather than one row per batch.
	const TArray<TPair<FString, double>>& Phases() const { return Rows; }

	// One table into the log. Nothing is printed for a job with no phases.
	void Report() const;

	// The same table as text, for a load test that wants to aggregate rather
	// than print.
	FString ToString() const;

private:
	FString JobName;
	double  Started = 0.0;
	double  PhaseStarted = 0.0;
	FString Current;
	TArray<TPair<FString, double>> Rows;

	void CloseCurrent();
};

} // namespace BF6Ext
