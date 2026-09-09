#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

class SWidget;

DECLARE_LOG_CATEGORY_EXTERN(LogBF6Caps, Log, All);

// ============================================================================
// WHAT THIS BUILD CAN DO, AND WHAT CHANGED SINCE THE LAST ONE.
//
// Three things decide what a Portal mod can actually use, and they disagree
// with each other constantly:
//
//   the installed SDK    what the local type-checker knows about
//   the Portal website   what the site will accept when you upload
//   the running game     what the engine actually has
//
// A function can exist in all three, in one, or in none. SetTickRate is real in
// the engine, rejected by name at the site, and absent from the typings. The
// four GetWater getters are in archived game registries and were not found at
// runtime. Somebody building a mod needs to know which of those they are
// looking at BEFORE they design around it, and today they find out by uploading
// and reading a rejection.
//
// So this collects what each source says, keeps immutable snapshots of it, and
// reports the differences between two snapshots as changes.
//
// ---------------------------------------------------------------------------
// THE RULE THAT MATTERS MOST: ABSENCE IS NOT REMOVAL.
//
// Every collector reports COVERAGE per scope, and a key missing from a scope
// that was not fully collected is "not observed", never "removed". A settings
// page whose dropdowns never mounted, a route the sweep did not reach, an SDK
// file that failed to download: each of those makes things look deleted, and
// reporting them as deletions would be worse than reporting nothing. Removal is
// only ever claimed within a scope both snapshots completed.
//
// ---------------------------------------------------------------------------
// EVIDENCE IS A LADDER, NOT A BOOLEAN.
//
// "Discovered" and "works" are far apart, and the space between them is where
// this is most useful. An identifier in a delivered asset, a parsed type, a
// control on a page, a value the server accepted, an effect measured in game:
// these are different claims and the UI never merges them.
// ============================================================================
namespace BF6Caps
{
	// Weakest to strongest. The ordering is meaningful: a record's evidence may
	// be raised by a later observation, and NotObserved is a transition rather
	// than a rung, so it is kept out of the ordered run.
	//
	// RuntimePresent and RuntimeVerified are deliberately two rungs. The probe
	// asks `typeof mod[name]` and calls nothing, so a "present" answer proves the
	// name exists and proves nothing about whether calling it works. Merging the
	// two would let a symbol lookup be reported as measured behavior, which is
	// the single most misleading thing this tool could say.
	enum class EEvidence : uint8
	{
		Observed = 0,      // an identifier in a delivered asset; meaning unverified
		Structure,         // a parsed schema or type; no proof of reachability
		Exposed,           // a control or registered block, for this route
		Serialized,        // referenced by Portal's own serialization
		SiteAccepted,      // written and read back; the server kept it
		RuntimePresent,    // the name exists in a running game; nothing was called
		RuntimeVerified,   // an effect was measured in a running game
		NotObserved,       // was seen before, not seen in a completed scope now
	};

	enum class ESource : uint8
	{
		Sdk = 0,       // the installed Portal SDK
		Portal,        // the connected website
		Game,          // the installed game, through the High Poly reader
		Watchlist,     // named candidates being tracked deliberately
	};

	// Per scope, never per snapshot: one page can complete while another fails,
	// and a single verdict for the whole sweep would throw that away.
	enum class ECoverage : uint8
	{
		Complete = 0,  // complete FOR ITS DECLARED SCOPE, which is never "everything"
		Partial,       // ran, but a cap or an unmounted control cut it short
		Skipped,       // deliberately not collected
		Unavailable,   // the source was not there to ask
		Failed,        // tried and could not
		Cancelled,     // the user stopped it
	};

	const TCHAR* EvidenceName(EEvidence E);
	const TCHAR* SourceName(ESource S);
	const TCHAR* CoverageName(ECoverage C);
	bool CoverageIsComplete(ECoverage C);

	// One capability, normalized. Key and Kind together identify it; Shape is
	// what a comparison actually looks at.
	struct FRecord
	{
		FString Key;        // source-native, never rewritten to match another source
		FString Kind;       // function, enum, type, block, setting, placeable, map, model
		FString Scope;      // which coverage scope this came from
		FString Display;    // what to call it in the UI
		FString Shape;      // the structural signature: ranges, options, parameters
		FString Detail;     // anything a card should say and nothing compares
		EEvidence Evidence = EEvidence::Observed;
	};

	struct FScope
	{
		FString Name;
		ECoverage State = ECoverage::Complete;
		FString Why;          // required for anything except Complete
		int32 Items = 0;
		int32 Cap = 0;        // a count limit that applied; 0 when none did
	};

	struct FSnapshot
	{
		FString Id;                 // content hash of the records, not a version string
		ESource Source = ESource::Sdk;
		FString Build;              // the source's own version, when it has one
		FString Collector;          // collector + normalization version
		FDateTime CapturedUtc;      // when this CONTENT was first stored, ever
		FDateTime ObservedUtc;      // when THIS observation of it was taken
		int32 Seq = 0;              // observation number for its source; 0 when not from the log
		FString Context;            // route, map, mode: what this was taken OF
		TMap<FString, FRecord> Records;   // "Kind|Key" -> record
		TArray<FScope> Scopes;

		const FScope* ScopeNamed(const FString& Name) const;
		bool ScopeWasComplete(const FString& Name) const;
	};

	struct FChange
	{
		enum class EKind : uint8 { Added, Removed, Changed, NotObserved, EvidenceRaised, EvidenceLowered };
		EKind What = EKind::Added;
		FRecord Before;
		FRecord After;
		FString Why;      // why this is being claimed, especially for NotObserved
	};

	const TCHAR* ChangeName(FChange::EKind K);

	// ---- the store ---------------------------------------------------------
	// TWO LAYERS, AND THEY ARE NOT THE SAME THING.
	//
	// Content blobs are addressed by a hash of what is in them, so scanning the
	// same unchanged SDK twice costs one blob. Publishing writes to a temporary
	// directory and renames, so a blob directory that exists is always a finished
	// one: a half-written snapshot that looked complete would silently become a
	// false baseline for every later diff.
	//
	// Observations are an append-only, numbered log per source: scan 1 saw blob
	// A, scan 2 saw B, scan 3 saw A again. Identity and history were conflated
	// once, and it cost the A/B/A case entirely: the return to A wrote no blob,
	// so the newest thing in the store stayed B and the newest timestamp was the
	// one A was FIRST seen at. Anything that asks "what is current" or "what
	// changed" must read the log, never the blob directory.
	FString Root();
	FString SnapshotDir(ESource Source, const FString& Id);
	FString ObservationDir(ESource Source);
	bool Publish(const FSnapshot& Snap, FString& OutError);   // the blob only
	bool Load(ESource Source, const FString& Id, FSnapshot& Out);
	TArray<FString> List(ESource Source);          // blob ids, newest capture first

	// Store the content and append one observation of it. OutStored comes back
	// with the content id, the sequence number and this observation's time.
	bool Record(const FSnapshot& Snap, FSnapshot& OutStored, FString& OutError);

	TArray<int32> ObservationSeqs(ESource Source);              // newest first
	bool ObservationBack(ESource Source, int32 Back, FSnapshot& Out);  // 0 is the head
	bool Latest(ESource Source, FSnapshot& Out);                // ObservationBack(0)

	// ---- comparing ---------------------------------------------------------
	// Old and New must share a source. Scopes both completed can report
	// removals; every other scope can only report "not observed".
	TArray<FChange> Compare(const FSnapshot& Old, const FSnapshot& New);
	FString ReportMarkdown(const FSnapshot& Old, const FSnapshot& New, const TArray<FChange>& Changes);

	// ---- collecting --------------------------------------------------------
	// Each returns false and fills OutError only when it could not produce a
	// snapshot at all. A source that is simply absent produces a snapshot whose
	// scopes say Unavailable, because "we could not look" is a finding.
	bool CollectSdk(FSnapshot& Out, FString& OutError);
	bool CollectPortal(FSnapshot& Out, FString& OutError);
	bool CollectGame(FSnapshot& Out, FString& OutError);
	bool CollectWatchlist(FSnapshot& Out, FString& OutError);

	// ---- the watchlist -----------------------------------------------------
	// Named candidates: things believed to exist somewhere, tracked across
	// builds so their status is a record rather than a memory. The tool cannot
	// call into a running game, so the runtime rung of the ladder is reached by
	// emitting a probe the user runs in their own mod, whose output the game log
	// reader picks up.
	//
	// Probe output is tied to ONE run. Every line carries a run id the script
	// generates for itself, the run is bracketed by begin and end markers, and
	// the begin marker carries a hash of the candidate list the script was built
	// from. Results are only ever read out of a single run that started, ended,
	// passed all its controls and produced the number of answers it promised:
	// an old failed run followed by a later healthy one used to blend into one
	// answer, so an absence measured by a broken probe could be certified by a
	// different run's controls.
	FString ProbeScript();
	FString ProbeHash();      // identifies the candidate list a run was built from
	int32 IngestProbeLines(const TArray<FString>& Lines, FString& OutSummary);

	// Collect the chosen sources, record each, and report what moved. Sources
	// are independent: one being unavailable never stops the others.
	FString ScanNow(bool bSdk, bool bPortal, bool bGame, bool bWatch);

	// ---- the section on the build screen ------------------------------------
	TSharedRef<SWidget> Widget();
	void ReleaseWidget();

	void Register();
	void Unregister();
}
