#include "BF6Capabilities.h"
#include "BF6Internal.h"
#include "BF6SDKExtension.h"     // BF6Ext::ToolSavedDir
#include "BF6BuildMode.h"        // BF6Api::StoredSdkRoot, GameInstallDir, HighPolyIsInstalled
#include "BF6GameLog.h"          // the probe's answers come back through the Portal log
#include "BF6Theme.h"

#include "Algo/Reverse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"

DEFINE_LOG_CATEGORY(LogBF6Caps);

namespace BF6Caps
{

// The collector version is part of a snapshot's identity. Change it whenever
// the shape of what a collector produces changes, or old snapshots will be
// compared against new ones as though the difference were the source moving.
// caps/2: declaration signatures became whole-declaration and overload-aware,
// settings shapes carry option identities, and runtime symbol presence stopped
// being reported as measured behavior. Every one of those changes what a
// collector produces, so old snapshots must not be diffed against new ones.
static const TCHAR* kCollector = TEXT("caps/2");

const TCHAR* EvidenceName(EEvidence E)
{
	switch (E)
	{
	case EEvidence::Observed:        return TEXT("observed");
	case EEvidence::Structure:       return TEXT("structure");
	case EEvidence::Exposed:         return TEXT("exposed");
	case EEvidence::Serialized:      return TEXT("serialized");
	case EEvidence::SiteAccepted:    return TEXT("accepted by the site");
	case EEvidence::RuntimePresent:  return TEXT("present at runtime");
	case EEvidence::RuntimeVerified: return TEXT("verified in game");
	case EEvidence::NotObserved:     return TEXT("not observed");
	}
	return TEXT("?");
}

const TCHAR* SourceName(ESource S)
{
	switch (S)
	{
	case ESource::Sdk:       return TEXT("sdk");
	case ESource::Portal:    return TEXT("portal");
	case ESource::Game:      return TEXT("game");
	case ESource::Watchlist: return TEXT("watchlist");
	}
	return TEXT("?");
}

static ESource SourceFromName(const FString& S)
{
	if (S == TEXT("portal"))    return ESource::Portal;
	if (S == TEXT("game"))      return ESource::Game;
	if (S == TEXT("watchlist")) return ESource::Watchlist;
	return ESource::Sdk;
}

const TCHAR* CoverageName(ECoverage C)
{
	switch (C)
	{
	case ECoverage::Complete:    return TEXT("complete");
	case ECoverage::Partial:     return TEXT("partial");
	case ECoverage::Skipped:     return TEXT("skipped");
	case ECoverage::Unavailable: return TEXT("unavailable");
	case ECoverage::Failed:      return TEXT("failed");
	case ECoverage::Cancelled:   return TEXT("cancelled");
	}
	return TEXT("?");
}

bool CoverageIsComplete(ECoverage C) { return C == ECoverage::Complete; }

const TCHAR* ChangeName(FChange::EKind K)
{
	switch (K)
	{
	case FChange::EKind::Added:          return TEXT("new");
	case FChange::EKind::Removed:        return TEXT("gone");
	case FChange::EKind::Changed:        return TEXT("changed");
	case FChange::EKind::NotObserved:    return TEXT("not observed");
	case FChange::EKind::EvidenceRaised: return TEXT("better evidence");
	case FChange::EKind::EvidenceLowered:return TEXT("weaker evidence");
	}
	return TEXT("?");
}

static EEvidence EvidenceFromName(const FString& S)
{
	if (S == TEXT("structure"))            return EEvidence::Structure;
	if (S == TEXT("exposed"))              return EEvidence::Exposed;
	if (S == TEXT("serialized"))           return EEvidence::Serialized;
	if (S == TEXT("accepted by the site")) return EEvidence::SiteAccepted;
	if (S == TEXT("present at runtime"))   return EEvidence::RuntimePresent;
	if (S == TEXT("verified in game"))     return EEvidence::RuntimeVerified;
	if (S == TEXT("not observed"))         return EEvidence::NotObserved;
	return EEvidence::Observed;
}

static ECoverage CoverageFromName(const FString& S)
{
	if (S == TEXT("partial"))     return ECoverage::Partial;
	if (S == TEXT("skipped"))     return ECoverage::Skipped;
	if (S == TEXT("unavailable")) return ECoverage::Unavailable;
	if (S == TEXT("failed"))      return ECoverage::Failed;
	if (S == TEXT("cancelled"))   return ECoverage::Cancelled;
	return ECoverage::Complete;
}

static FString KeyOf(const FRecord& R) { return R.Kind + TEXT("|") + R.Key; }

const FScope* FSnapshot::ScopeNamed(const FString& Name) const
{
	for (const FScope& S : Scopes) { if (S.Name == Name) { return &S; } }
	return nullptr;
}

bool FSnapshot::ScopeWasComplete(const FString& Name) const
{
	const FScope* S = ScopeNamed(Name);
	return S && CoverageIsComplete(S->State);
}

// ---------------------------------------------------------------------------
// THE STORE
// ---------------------------------------------------------------------------
FString Root()
{
	return FPaths::Combine(BF6Ext::ToolSavedDir(), TEXT("capabilities"));
}

FString SnapshotDir(ESource Source, const FString& Id)
{
	return FPaths::Combine(Root(), TEXT("snapshots"), SourceName(Source), Id);
}

FString ObservationDir(ESource Source)
{
	return FPaths::Combine(Root(), TEXT("observations"), SourceName(Source));
}

// The identity of a snapshot is what is IN it, not what it was called. Two
// collections of the same SDK version produce the same id and the second one
// costs nothing; a repack that changed content under an unchanged version
// number produces a different id and is not mistaken for the same thing.
static FString ContentId(const FSnapshot& S)
{
	TArray<FString> Keys;
	S.Records.GetKeys(Keys);
	Keys.Sort();
	FString Canon = FString(kCollector) + TEXT("\n") + SourceName(S.Source) + TEXT("\n") + S.Build + TEXT("\n");
	for (const FString& K : Keys)
	{
		const FRecord& R = S.Records[K];
		Canon += K + TEXT("\x1f") + R.Shape + TEXT("\x1f") + EvidenceName(R.Evidence) + TEXT("\n");
	}
	// Scopes are part of identity too: the same records gathered under weaker
	// coverage are a different observation and must not share an id.
	TArray<FScope> Sc = S.Scopes;
	Sc.Sort([](const FScope& A, const FScope& B) { return A.Name < B.Name; });
	for (const FScope& Sp : Sc)
	{
		Canon += TEXT("scope\x1f") + Sp.Name + TEXT("\x1f") + CoverageName(Sp.State) + TEXT("\n");
	}
	return FMD5::HashAnsiString(*Canon).Left(16);
}

static TSharedRef<FJsonObject> ToJson(const FSnapshot& S)
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("id"), S.Id);
	O->SetStringField(TEXT("source"), SourceName(S.Source));
	O->SetStringField(TEXT("build"), S.Build);
	O->SetStringField(TEXT("collector"), S.Collector);
	O->SetStringField(TEXT("capturedUtc"), S.CapturedUtc.ToIso8601());
	O->SetStringField(TEXT("context"), S.Context);

	TArray<TSharedPtr<FJsonValue>> Sc;
	for (const FScope& Sp : S.Scopes)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("name"), Sp.Name);
		J->SetStringField(TEXT("state"), CoverageName(Sp.State));
		J->SetStringField(TEXT("why"), Sp.Why);
		J->SetNumberField(TEXT("items"), Sp.Items);
		J->SetNumberField(TEXT("cap"), Sp.Cap);
		Sc.Add(MakeShared<FJsonValueObject>(J));
	}
	O->SetArrayField(TEXT("scopes"), Sc);

	TArray<FString> Keys;
	S.Records.GetKeys(Keys);
	Keys.Sort();
	TArray<TSharedPtr<FJsonValue>> Rs;
	for (const FString& K : Keys)
	{
		const FRecord& R = S.Records[K];
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("key"), R.Key);
		J->SetStringField(TEXT("kind"), R.Kind);
		J->SetStringField(TEXT("scope"), R.Scope);
		J->SetStringField(TEXT("display"), R.Display);
		J->SetStringField(TEXT("shape"), R.Shape);
		J->SetStringField(TEXT("detail"), R.Detail);
		J->SetStringField(TEXT("evidence"), EvidenceName(R.Evidence));
		Rs.Add(MakeShared<FJsonValueObject>(J));
	}
	O->SetArrayField(TEXT("records"), Rs);
	return O;
}

static bool FromJson(const TSharedPtr<FJsonObject>& O, FSnapshot& S)
{
	if (!O.IsValid()) { return false; }
	FString Tmp;
	O->TryGetStringField(TEXT("id"), S.Id);
	if (O->TryGetStringField(TEXT("source"), Tmp)) { S.Source = SourceFromName(Tmp); }
	O->TryGetStringField(TEXT("build"), S.Build);
	O->TryGetStringField(TEXT("collector"), S.Collector);
	O->TryGetStringField(TEXT("context"), S.Context);
	if (O->TryGetStringField(TEXT("capturedUtc"), Tmp)) { FDateTime::ParseIso8601(*Tmp, S.CapturedUtc); }

	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (O->TryGetArrayField(TEXT("scopes"), Arr))
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject> J = V->AsObject();
			if (!J.IsValid()) { continue; }
			FScope Sp;
			J->TryGetStringField(TEXT("name"), Sp.Name);
			if (J->TryGetStringField(TEXT("state"), Tmp)) { Sp.State = CoverageFromName(Tmp); }
			J->TryGetStringField(TEXT("why"), Sp.Why);
			J->TryGetNumberField(TEXT("items"), Sp.Items);
			J->TryGetNumberField(TEXT("cap"), Sp.Cap);
			S.Scopes.Add(MoveTemp(Sp));
		}

	if (O->TryGetArrayField(TEXT("records"), Arr))
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject> J = V->AsObject();
			if (!J.IsValid()) { continue; }
			FRecord R;
			J->TryGetStringField(TEXT("key"), R.Key);
			J->TryGetStringField(TEXT("kind"), R.Kind);
			J->TryGetStringField(TEXT("scope"), R.Scope);
			J->TryGetStringField(TEXT("display"), R.Display);
			J->TryGetStringField(TEXT("shape"), R.Shape);
			J->TryGetStringField(TEXT("detail"), R.Detail);
			if (J->TryGetStringField(TEXT("evidence"), Tmp)) { R.Evidence = EvidenceFromName(Tmp); }
			if (!R.Key.IsEmpty()) { S.Records.Add(KeyOf(R), MoveTemp(R)); }
		}
	return true;
}

bool Publish(const FSnapshot& InSnap, FString& OutError)
{
	FSnapshot Snap = InSnap;
	Snap.Collector = kCollector;
	if (Snap.CapturedUtc == FDateTime()) { Snap.CapturedUtc = FDateTime::UtcNow(); }
	Snap.Id = ContentId(Snap);

	const FString Final = SnapshotDir(Snap.Source, Snap.Id);
	if (FPaths::DirectoryExists(Final))
	{
		// This content is already stored, so there is nothing to write. This is
		// DEDUPLICATION, not "nothing happened": the fact that we looked again
		// is recorded by Record() in the observation log. Treating this early
		// return as the whole answer is what lost the A/B/A case, because the
		// return to A wrote nothing and left B looking like the newest state.
		return true;
	}

	// Write beside the destination and rename. A snapshot directory that exists
	// is therefore always finished: the old history code treated the presence of
	// one file as proof a snapshot was complete, so an interrupted write became
	// a permanent, silently wrong baseline.
	const FString Staging = Final + TEXT(".writing");
	IFileManager::Get().DeleteDirectory(*Staging, false, true);
	if (!IFileManager::Get().MakeDirectory(*Staging, true))
	{
		OutError = FString::Printf(TEXT("could not create %s"), *Staging);
		return false;
	}

	FString Text;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(ToJson(Snap), W);
	if (!FFileHelper::SaveStringToFile(Text, *(Staging / TEXT("snapshot.json")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		IFileManager::Get().DeleteDirectory(*Staging, false, true);
		OutError = TEXT("could not write the snapshot");
		return false;
	}
	if (!IFileManager::Get().Move(*Final, *Staging, true, true))
	{
		IFileManager::Get().DeleteDirectory(*Staging, false, true);
		OutError = TEXT("could not publish the snapshot");
		return false;
	}
	UE_LOG(LogBF6Caps, Display, TEXT("%s snapshot %s: %d capability record(s)"),
		SourceName(Snap.Source), *Snap.Id, Snap.Records.Num());
	return true;
}

bool Load(ESource Source, const FString& Id, FSnapshot& Out)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *(SnapshotDir(Source, Id) / TEXT("snapshot.json")))) { return false; }
	TSharedPtr<FJsonObject> O;
	const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(R, O) || !O.IsValid()) { return false; }
	return FromJson(O, Out);
}

TArray<FString> List(ESource Source)
{
	TArray<FString> Dirs;
	IFileManager::Get().FindFiles(Dirs, *(FPaths::Combine(Root(), TEXT("snapshots"), SourceName(Source)) / TEXT("*")), false, true);
	// A ".writing" directory is an interrupted publish, never a snapshot.
	Dirs.RemoveAll([](const FString& D) { return D.EndsWith(TEXT(".writing")); });

	// Newest first, by what the snapshot says rather than by file times, which
	// a copy or a sync would rewrite.
	TMap<FString, FDateTime> When;
	for (const FString& D : Dirs)
	{
		FSnapshot S;
		When.Add(D, Load(Source, D, S) ? S.CapturedUtc : FDateTime());
	}
	Dirs.Sort([&When](const FString& A, const FString& B) { return When[A] > When[B]; });
	return Dirs;
}

// ---------------------------------------------------------------------------
// THE OBSERVATION LOG
//
// One numbered file per scan, per source. The head is simply the highest number
// present, which survives a restart for free and needs no separate pointer that
// could disagree with the files beside it. Each file is written to a staging
// name and renamed, so a number either exists whole or does not exist: a
// half-written observation would become a permanent, silently wrong head.
//
// An observation names its content blob rather than repeating it. Two scans that
// saw the same thing point at the same blob and are still two observations.
// ---------------------------------------------------------------------------
static FString ObservationFile(ESource Source, int32 Seq)
{
	return ObservationDir(Source) / FString::Printf(TEXT("obs_%08d.json"), Seq);
}

TArray<int32> ObservationSeqs(ESource Source)
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(ObservationDir(Source) / TEXT("obs_*.json")), true, false);
	TArray<int32> Seqs;
	for (const FString& F : Files)
	{
		// obs_00000012.json -> 12. Anything that does not parse is not ours.
		const FString Digits = FPaths::GetBaseFilename(F).RightChop(4);
		if (Digits.IsEmpty() || !Digits.IsNumeric()) { continue; }
		const int32 N = FCString::Atoi(*Digits);
		if (N > 0) { Seqs.Add(N); }
	}
	Seqs.Sort([](int32 A, int32 B) { return A > B; });   // newest first
	return Seqs;
}

static bool LoadObservation(ESource Source, int32 Seq, FSnapshot& Out)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *ObservationFile(Source, Seq))) { return false; }
	TSharedPtr<FJsonObject> O;
	const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(R, O) || !O.IsValid()) { return false; }

	FString ContentId;
	if (!O->TryGetStringField(TEXT("contentId"), ContentId) || ContentId.IsEmpty()) { return false; }
	if (!Load(Source, ContentId, Out)) { return false; }

	// The blob's own CapturedUtc is when this CONTENT was first stored, which is
	// not when this observation happened. Reporting the first-seen time as the
	// current one is how a re-scan looked stale.
	Out.Seq = Seq;
	FString When;
	Out.ObservedUtc = Out.CapturedUtc;
	if (O->TryGetStringField(TEXT("observedUtc"), When)) { FDateTime::ParseIso8601(*When, Out.ObservedUtc); }
	FString Ctx;
	if (O->TryGetStringField(TEXT("context"), Ctx)) { Out.Context = Ctx; }
	return true;
}

bool ObservationBack(ESource Source, int32 Back, FSnapshot& Out)
{
	const TArray<int32> Seqs = ObservationSeqs(Source);
	if (Back < 0 || Back >= Seqs.Num()) { return false; }
	return LoadObservation(Source, Seqs[Back], Out);
}

bool Latest(ESource Source, FSnapshot& Out)
{
	return ObservationBack(Source, 0, Out);
}

static bool WriteObservation(const FSnapshot& Snap, int32 Seq, FString& OutError)
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("seq"), Seq);
	O->SetStringField(TEXT("source"), SourceName(Snap.Source));
	O->SetStringField(TEXT("contentId"), Snap.Id);
	O->SetStringField(TEXT("build"), Snap.Build);
	O->SetStringField(TEXT("collector"), Snap.Collector);
	O->SetStringField(TEXT("observedUtc"), Snap.ObservedUtc.ToIso8601());
	O->SetStringField(TEXT("context"), Snap.Context);
	O->SetNumberField(TEXT("records"), Snap.Records.Num());
	// Coverage is kept on the observation as well as inside the blob, so what a
	// given scan managed to look at can be read back without opening the blob,
	// and so a scan is never summarised as one verdict.
	TArray<TSharedPtr<FJsonValue>> Sc;
	for (const FScope& Sp : Snap.Scopes)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("name"), Sp.Name);
		J->SetStringField(TEXT("state"), CoverageName(Sp.State));
		J->SetStringField(TEXT("why"), Sp.Why);
		J->SetNumberField(TEXT("items"), Sp.Items);
		J->SetNumberField(TEXT("cap"), Sp.Cap);
		Sc.Add(MakeShared<FJsonValueObject>(J));
	}
	O->SetArrayField(TEXT("scopes"), Sc);

	FString Text;
	const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(O, W);

	const FString Final = ObservationFile(Snap.Source, Seq);
	const FString Staging = Final + TEXT(".writing");
	IFileManager::Get().MakeDirectory(*ObservationDir(Snap.Source), true);
	if (!FFileHelper::SaveStringToFile(Text, *Staging, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = TEXT("could not write the observation");
		return false;
	}
	if (!IFileManager::Get().Move(*Final, *Staging, true, true))
	{
		IFileManager::Get().Delete(*Staging, false, true);
		OutError = TEXT("could not append the observation");
		return false;
	}
	return true;
}

// A store written before the log existed has blobs and no observations. Seed the
// log from them in capture order once, so upgrading does not throw away the
// history that is already there and does not make the next scan look like a
// first ever look at the source.
static void SeedObservationsFromBlobs(ESource Source)
{
	if (ObservationSeqs(Source).Num() > 0) { return; }
	TArray<FString> Ids = List(Source);          // newest capture first
	Algo::Reverse(Ids);
	int32 Seq = 0;
	for (const FString& Id : Ids)
	{
		FSnapshot S;
		if (!Load(Source, Id, S)) { continue; }
		S.Id = Id;
		S.ObservedUtc = S.CapturedUtc;
		FString Err;
		if (WriteObservation(S, ++Seq, Err))
		{
			UE_LOG(LogBF6Caps, Display, TEXT("seeded %s observation %d from existing snapshot %s"),
				SourceName(Source), Seq, *Id);
		}
		else { --Seq; }
	}
}

bool Record(const FSnapshot& InSnap, FSnapshot& OutStored, FString& OutError)
{
	FSnapshot Snap = InSnap;
	if (Snap.ObservedUtc == FDateTime()) { Snap.ObservedUtc = FDateTime::UtcNow(); }
	if (!Publish(Snap, OutError)) { return false; }
	Snap.Collector = kCollector;
	Snap.Id = ContentId(Snap);

	SeedObservationsFromBlobs(Snap.Source);

	const TArray<int32> Seqs = ObservationSeqs(Snap.Source);
	int32 Next = (Seqs.Num() ? Seqs[0] : 0) + 1;
	// Move() overwrites, so a number that is somehow already taken would be
	// silently replaced and one scan would vanish. Step past anything present.
	while (FPaths::FileExists(ObservationFile(Snap.Source, Next))) { ++Next; }
	if (!WriteObservation(Snap, Next, OutError)) { return false; }

	Snap.Seq = Next;
	OutStored = Snap;
	UE_LOG(LogBF6Caps, Display, TEXT("%s observation %d: content %s, %d capability record(s)"),
		SourceName(Snap.Source), Next, *Snap.Id, Snap.Records.Num());
	return true;
}

// ---------------------------------------------------------------------------
// COMPARING
//
// The whole value of this is in being trustworthy about removals, so the rule
// is applied here and nowhere else: a key present in Old and absent from New is
// only "gone" when BOTH snapshots completed the scope it lived in. Otherwise it
// is "not observed", which reads as a gap in what we looked at rather than a
// claim about the source.
// ---------------------------------------------------------------------------
TArray<FChange> Compare(const FSnapshot& Old, const FSnapshot& New)
{
	TArray<FChange> Out;
	if (Old.Source != New.Source)
	{
		// Sources are separate namespaces. Comparing a site key against an SDK
		// key by name is how a rename becomes an invented capability.
		UE_LOG(LogBF6Caps, Warning, TEXT("refusing to compare a %s snapshot with a %s one"),
			SourceName(Old.Source), SourceName(New.Source));
		return Out;
	}
	if (!Old.Collector.IsEmpty() && !New.Collector.IsEmpty() && Old.Collector != New.Collector)
	{
		// The collector changed what a shape looks like, so every record would
		// compare as "changed" and the report would describe this tool being
		// edited as the SDK moving. A difference in the instrument is never a
		// finding about the thing being measured.
		UE_LOG(LogBF6Caps, Warning,
			TEXT("refusing to compare snapshots from collector %s and collector %s"),
			*Old.Collector, *New.Collector);
		return Out;
	}

	for (const TPair<FString, FRecord>& It : New.Records)
	{
		const FRecord* Was = Old.Records.Find(It.Key);
		if (!Was)
		{
			FChange C; C.What = FChange::EKind::Added; C.After = It.Value;
			// New in a scope the previous snapshot never completed may just be
			// the first proper look at it, and saying so costs nothing.
			if (!Old.ScopeWasComplete(It.Value.Scope))
				C.Why = FString::Printf(TEXT("the previous scan did not complete %s, so this may not be new"), *It.Value.Scope);
			Out.Add(MoveTemp(C));
			continue;
		}
		if (Was->Shape != It.Value.Shape)
		{
			FChange C; C.What = FChange::EKind::Changed; C.Before = *Was; C.After = It.Value;
			Out.Add(MoveTemp(C));
		}
		else if (It.Value.Evidence != Was->Evidence)
		{
			// The capability did not change; what we know about it did, and that
			// moves in BOTH directions. Only raises were reported once, so a
			// capability that stopped being found at runtime produced silence,
			// which is the one outcome a user most needs to see.
			//
			// NotObserved is the largest enum value and is NOT the top of the
			// ladder, so it can never be compared with the ordinal operators.
			// It is handled first, by name, for exactly that reason.
			FChange C; C.Before = *Was; C.After = It.Value;
			if (It.Value.Evidence == EEvidence::NotObserved)
			{
				C.What = FChange::EKind::EvidenceLowered;
				C.Why = FString::Printf(
					TEXT("it was %s before and was looked for and not seen this time. The record is kept so the change is visible."),
					EvidenceName(Was->Evidence));
			}
			else if (Was->Evidence == EEvidence::NotObserved)
			{
				C.What = FChange::EKind::EvidenceRaised;
				C.Why = TEXT("it was recorded as not observed and has been seen again.");
			}
			else
			{
				C.What = It.Value.Evidence > Was->Evidence
					? FChange::EKind::EvidenceRaised : FChange::EKind::EvidenceLowered;
			}
			Out.Add(MoveTemp(C));
		}
	}

	for (const TPair<FString, FRecord>& It : Old.Records)
	{
		if (New.Records.Contains(It.Key)) { continue; }
		const bool bBothCompleted = Old.ScopeWasComplete(It.Value.Scope) && New.ScopeWasComplete(It.Value.Scope);
		FChange C;
		C.Before = It.Value;
		if (bBothCompleted)
		{
			C.What = FChange::EKind::Removed;
			C.Why = FString::Printf(TEXT("%s was collected fully both times"), *It.Value.Scope);
		}
		else
		{
			const FScope* NowScope = New.ScopeNamed(It.Value.Scope);
			C.What = FChange::EKind::NotObserved;
			C.Why = NowScope
				? FString::Printf(TEXT("%s was %s this time (%s), so this is a gap in the scan and not a removal"),
					*It.Value.Scope, CoverageName(NowScope->State), *NowScope->Why)
				: FString::Printf(TEXT("%s was not visited this time, so this is a gap in the scan and not a removal"),
					*It.Value.Scope);
		}
		Out.Add(MoveTemp(C));
	}

	Out.Sort([](const FChange& A, const FChange& B)
	{
		if (A.What != B.What) { return (uint8)A.What < (uint8)B.What; }
		const FString& KA = A.After.Key.IsEmpty() ? A.Before.Key : A.After.Key;
		const FString& KB = B.After.Key.IsEmpty() ? B.Before.Key : B.After.Key;
		return KA < KB;
	});
	return Out;
}

FString ReportMarkdown(const FSnapshot& Old, const FSnapshot& New, const TArray<FChange>& Changes)
{
	FString Md;
	Md += FString::Printf(TEXT("# What changed in the %s\n\n"), SourceName(New.Source));
	// Both times are the times these OBSERVATIONS were taken. Using the content's
	// first-seen time here made a re-scan of familiar content look stale.
	Md += FString::Printf(TEXT("%s (observation %d, %s) compared with %s (observation %d, %s).\n\n"),
		*New.Build, New.Seq, *New.ObservedUtc.ToString(),
		*Old.Build, Old.Seq, *Old.ObservedUtc.ToString());

	// Coverage first. A reader needs to know what was looked at before they read
	// anything into what was found.
	Md += TEXT("## What was looked at\n\n");
	for (const FScope& S : New.Scopes)
	{
		Md += FString::Printf(TEXT("- %s: %s"), *S.Name, CoverageName(S.State));
		if (S.Items)      { Md += FString::Printf(TEXT(", %d item(s)"), S.Items); }
		if (S.Cap)        { Md += FString::Printf(TEXT(", stopped at a limit of %d"), S.Cap); }
		if (!S.Why.IsEmpty()) { Md += FString::Printf(TEXT(" (%s)"), *S.Why); }
		Md += TEXT("\n");
	}
	Md += TEXT("\nComplete means complete for what this scan set out to cover. It never means the whole of Portal is known.\n\n");

	if (Changes.Num() == 0)
	{
		Md += TEXT("## Nothing changed\n\nNo differences in any scope that was collected.\n");
		return Md;
	}

	auto Section = [&Md, &Changes](FChange::EKind K, const TCHAR* Title, const TCHAR* Blurb)
	{
		TArray<const FChange*> Mine;
		for (const FChange& C : Changes) { if (C.What == K) { Mine.Add(&C); } }
		if (Mine.Num() == 0) { return; }
		Md += FString::Printf(TEXT("## %s (%d)\n\n%s\n\n"), Title, Mine.Num(), Blurb);
		for (const FChange* C : Mine)
		{
			const FRecord& R = C->After.Key.IsEmpty() ? C->Before : C->After;
			Md += FString::Printf(TEXT("- **%s** `%s`"), *R.Kind, *R.Key);
			if (!R.Display.IsEmpty() && R.Display != R.Key) { Md += FString::Printf(TEXT(" - %s"), *R.Display); }
			if (C->What == FChange::EKind::Changed)
			{
				Md += FString::Printf(TEXT("\n    - was: `%s`\n    - now: `%s`"), *C->Before.Shape, *C->After.Shape);
			}
			else if (!R.Shape.IsEmpty() && C->What == FChange::EKind::Added)
			{
				Md += FString::Printf(TEXT("\n    - `%s`"), *R.Shape);
			}
			if (C->What == FChange::EKind::EvidenceRaised || C->What == FChange::EKind::EvidenceLowered)
			{
				Md += FString::Printf(TEXT("\n    - %s, was %s"),
					EvidenceName(C->After.Evidence), EvidenceName(C->Before.Evidence));
			}
			else
			{
				Md += FString::Printf(TEXT("\n    - evidence: %s"), EvidenceName(R.Evidence));
			}
			if (!C->Why.IsEmpty()) { Md += FString::Printf(TEXT("\n    - %s"), *C->Why); }
			Md += TEXT("\n");
		}
		Md += TEXT("\n");
	};

	Section(FChange::EKind::Added, TEXT("New"),
		TEXT("Present now and not in the previous snapshot."));
	Section(FChange::EKind::Changed, TEXT("Changed"),
		TEXT("The same capability with a different shape. A widened range or a new option belongs here, and so does a changed signature."));
	Section(FChange::EKind::EvidenceRaised, TEXT("Better evidence"),
		TEXT("Unchanged, but now known more firmly than it was."));
	Section(FChange::EKind::EvidenceLowered, TEXT("Weaker evidence"),
		TEXT("Still recorded, but the evidence behind it dropped. A capability that was found at runtime and is not found now appears here, not under Gone: the record still exists, what fell away is the proof."));
	Section(FChange::EKind::Removed, TEXT("Gone"),
		TEXT("Absent from a scope that was collected completely both times. This is the only section that claims something was taken away."));
	Section(FChange::EKind::NotObserved, TEXT("Not observed"),
		TEXT("Seen before, not seen now, in a scope this scan did not finish. Almost always a gap in the scan rather than a removal. Re-run the scan before reading anything into these."));
	return Md;
}

// ---------------------------------------------------------------------------
// THE SDK COLLECTOR
// ---------------------------------------------------------------------------

// Whitespace-normalized, comments removed. Comparing raw text would report a
// reflow or a reworded doc comment as a capability change, which trains people
// to ignore the report.
static FString NormalizeSignature(const FString& In)
{
	FString Out;
	bool bSpace = false;
	for (const TCHAR C : In)
	{
		if (FChar::IsWhitespace(C)) { bSpace = true; continue; }
		if (bSpace && !Out.IsEmpty()) { Out.AppendChar(TEXT(' ')); }
		bSpace = false;
		Out.AppendChar(C);
	}
	return Out.TrimStartAndEnd();
}

// A record's Shape ends up in JSON and in a report, so a 400-member enum cannot
// be pasted in whole. Long signatures keep a readable head and a hash of the
// whole thing, so a change anywhere in the body still changes the shape.
static FString BoundedShape(const FString& Full)
{
	const int32 kMaxInline = 240;
	if (Full.Len() <= kMaxInline) { return Full; }
	return Full.Left(200) + FString::Printf(TEXT(" ... %d chars, %s"),
		Full.Len(), *FMD5::HashAnsiString(*Full).Left(12));
}

// Strip // and /* */ comments from one line, and say whether a block comment is
// still open at the end of it. Brace counting has to ignore commented-out braces
// or a doc comment ends the declaration early.
static FString StripComments(const FString& Line, bool& bInBlock)
{
	FString Out;
	TCHAR Quote = 0;
	for (int32 i = 0; i < Line.Len(); ++i)
	{
		const TCHAR C = Line[i];
		const TCHAR N = (i + 1 < Line.Len()) ? Line[i + 1] : TEXT('\0');
		if (bInBlock)
		{
			if (C == TEXT('*') && N == TEXT('/')) { bInBlock = false; ++i; }
			continue;
		}
		if (Quote)
		{
			Out.AppendChar(C);
			if (C == TEXT('\\') && N) { Out.AppendChar(N); ++i; continue; }
			if (C == Quote) { Quote = 0; }
			continue;
		}
		if (C == TEXT('"') || C == TEXT('\'') || C == TEXT('`')) { Quote = C; Out.AppendChar(C); continue; }
		if (C == TEXT('/') && N == TEXT('/')) { break; }
		if (C == TEXT('/') && N == TEXT('*')) { bInBlock = true; ++i; continue; }
		Out.AppendChar(C);
	}
	return Out;
}

// How far the brackets are still open after this text, so a declaration split
// over several lines can be read as one thing.
static int32 BracketDelta(const FString& Text)
{
	int32 Depth = 0;
	TCHAR Quote = 0;
	for (int32 i = 0; i < Text.Len(); ++i)
	{
		const TCHAR C = Text[i];
		if (Quote)
		{
			if (C == TEXT('\\')) { ++i; continue; }
			if (C == Quote) { Quote = 0; }
			continue;
		}
		if (C == TEXT('"') || C == TEXT('\'') || C == TEXT('`')) { Quote = C; continue; }
		if (C == TEXT('{') || C == TEXT('(') || C == TEXT('[')) { ++Depth; }
		else if (C == TEXT('}') || C == TEXT(')') || C == TEXT(']')) { --Depth; }
	}
	return Depth;
}

// WHAT THIS READS, AND WHAT IT CANNOT.
//
// It reads top-level `export` declarations out of a .d.ts, taking each one as
// far as its brackets close, and it keeps every overload under one key rather
// than letting the last one win. Three real changes used to be invisible here:
// a member added to a multiline enum (the old signature stopped at the opening
// brace), a changed parameter on a declaration whose parameter list was wrapped
// (the old signature was the first line, which is just the name and a paren),
// and any edit to a non-final overload (each one overwrote the one before it,
// so only the last survived).
//
// It is still not a TypeScript parser. It does not follow `export {` re-export
// lists, `export *`, or declarations nested inside another declaration's body.
// bOutComplete says whether the file contained anything of that sort, because a
// scope may only be called complete when the collector could actually cover it.
static void CollectDeclarations(const FString& DtsPath, const FString& Scope, FSnapshot& Snap,
								int32& OutCount, bool& bOutComplete, FString& OutWhy)
{
	bOutComplete = false;
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *DtsPath))
	{
		OutWhy = TEXT("the typings file could not be read");
		return;
	}
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, true);

	// Overloads under one key, in the order the file declares them. A set that
	// gained, lost or reordered a member is a different shape.
	struct FDecl { FString Kind; FString Name; TArray<FString> Sigs; };
	TArray<FString> Order;
	TMap<FString, FDecl> Decls;

	const int32 kMaxDeclLines = 4000;   // an unterminated bracket must not eat the file
	int32 Unreadable = 0;
	bool bInBlock = false;

	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		FString Code = StripComments(Lines[i], bInBlock);
		FString L = Code;
		L.TrimStartInline();
		if (!L.StartsWith(TEXT("export "))) { continue; }
		FString Head = L.RightChop(7);
		if (Head.StartsWith(TEXT("declare "))) { Head.RightChopInline(8); }

		FString Kind;
		for (const TCHAR* K : { TEXT("function "), TEXT("enum "), TEXT("const "),
								TEXT("class "), TEXT("interface "), TEXT("type "), TEXT("namespace ") })
		{
			if (Head.StartsWith(K)) { Kind = FString(K).TrimEnd(); Head.RightChopInline(FCString::Strlen(K)); break; }
		}
		if (Kind.IsEmpty())
		{
			// `export { A, B }`, `export * from ...`, `export default ...`: real
			// exports this cannot resolve to a capability. Counting them is what
			// keeps the scope from claiming a completeness it does not have.
			Unreadable++;
			continue;
		}

		FString Name;
		for (const TCHAR C : Head)
		{
			if (FChar::IsAlnum(C) || C == TEXT('_')) { Name.AppendChar(C); } else { break; }
		}
		if (Name.IsEmpty()) { Unreadable++; continue; }

		// Take the declaration as far as its brackets close, so a wrapped
		// parameter list or an enum body is part of the signature.
		FString Full = L;
		int32 Depth = BracketDelta(L);
		int32 Consumed = 1;
		while ((Depth > 0 || (!Full.TrimEnd().EndsWith(TEXT(";")) && !Full.TrimEnd().EndsWith(TEXT("}"))))
			   && i + 1 < Lines.Num() && Consumed < kMaxDeclLines)
		{
			++i;
			const FString More = StripComments(Lines[i], bInBlock);
			Full += TEXT(" ") + More;
			Depth += BracketDelta(More);
			++Consumed;
		}
		if (Depth > 0) { Unreadable++; }   // ran out of file with brackets still open

		const FString Sig = NormalizeSignature(Full);
		if (Sig.IsEmpty()) { continue; }
		const FString Ident = Kind + TEXT("|") + Name;
		FDecl* D = Decls.Find(Ident);
		if (!D)
		{
			Order.Add(Ident);
			D = &Decls.Add(Ident, FDecl{ Kind, Name, {} });
		}
		D->Sigs.Add(Sig);
	}

	for (const FString& Ident : Order)
	{
		const FDecl& D = Decls[Ident];
		FRecord R;
		R.Key = D.Name;
		R.Kind = D.Kind;
		R.Scope = Scope;
		R.Display = D.Name;
		R.Shape = BoundedShape(FString::Join(D.Sigs, TEXT(" | ")));
		if (D.Sigs.Num() > 1) { R.Detail = FString::Printf(TEXT("%d overloads"), D.Sigs.Num()); }
		R.Evidence = EEvidence::Structure;   // a parsed declaration, nothing more
		Snap.Records.Add(KeyOf(R), MoveTemp(R));
		OutCount++;
	}

	bOutComplete = (Unreadable == 0) && OutCount > 0;
	if (OutCount == 0)
	{
		OutWhy = TEXT("the typings file held no exported declarations");
	}
	else if (Unreadable > 0)
	{
		OutWhy = FString::Printf(
			TEXT("%d export(s) are re-export lists, defaults or unterminated declarations that this collector does not resolve, so absences here are gaps and not removals"),
			Unreadable);
	}
	else
	{
		OutWhy = TEXT("every top-level exported declaration in the typings, with its body and all its overloads");
	}
}

static void CollectBasenames(const FString& Dir, const TCHAR* Pattern, const TCHAR* Kind,
							 const FString& Scope, FSnapshot& Snap, int32& OutCount)
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Dir / Pattern), true, false);
	for (const FString& F : Files)
	{
		FRecord R;
		R.Key = FPaths::GetBaseFilename(F);
		R.Kind = Kind;
		R.Scope = Scope;
		R.Display = R.Key;
		// The name only. A map whose contents changed under the same name is a
		// real change this cannot see, and the report says so rather than
		// implying the map was checked.
		R.Shape = R.Key;
		R.Evidence = EEvidence::Observed;
		Snap.Records.Add(KeyOf(R), MoveTemp(R));
		OutCount++;
	}
}

bool CollectSdk(FSnapshot& Out, FString& OutError)
{
	Out = FSnapshot();
	Out.Source = ESource::Sdk;
	Out.CapturedUtc = FDateTime::UtcNow();

	const FString SdkRoot = BF6Api::StoredSdkRoot();
	if (SdkRoot.IsEmpty() || !FPaths::DirectoryExists(SdkRoot))
	{
		// Not an error. "No SDK is set up" is a true and useful answer, and it
		// must not look like an SDK with nothing in it.
		FScope S; S.Name = TEXT("sdk"); S.State = ECoverage::Unavailable;
		S.Why = TEXT("no Portal SDK folder is set up in this project");
		Out.Scopes.Add(MoveTemp(S));
		Out.Context = TEXT("no SDK");
		return true;
	}

	Out.Build = BF6_ReadSdkVersion(SdkRoot / TEXT("sdk.version.json"));
	Out.Context = SdkRoot;

	{
		const FString Dts = SdkRoot / TEXT("code/types/mod/index.d.ts");
		FScope S; S.Name = TEXT("api");
		if (FPaths::FileExists(Dts))
		{
			bool bComplete = false;
			CollectDeclarations(Dts, S.Name, Out, S.Items, bComplete, S.Why);
			// Completeness is the collector's answer about its own reach, not a
			// guess from the item count. A file with re-export lists in it has
			// keys this cannot see, and calling that scope complete would let a
			// later scan report them as removals.
			S.State = bComplete ? ECoverage::Complete
				: (S.Items > 0 ? ECoverage::Partial : ECoverage::Failed);
		}
		else
		{
			S.State = ECoverage::Unavailable;
			S.Why = FString::Printf(TEXT("%s is not there"), *Dts);
		}
		Out.Scopes.Add(MoveTemp(S));
	}

	{
		FScope S; S.Name = TEXT("placeables");
		FString Text;
		const FString Path = SdkRoot / TEXT("FbExportData/asset_types.json");
		if (FFileHelper::LoadFileToString(Text, *Path))
		{
			TSharedPtr<FJsonObject> J;
			const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
			const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
			if (FJsonSerializer::Deserialize(R, J) && J.IsValid() && J->TryGetArrayField(TEXT("AssetTypes"), Rows))
			{
				for (const TSharedPtr<FJsonValue>& V : *Rows)
				{
					const TSharedPtr<FJsonObject> O = V->AsObject();
					FString T, Dir;
					if (!O.IsValid() || !O->TryGetStringField(TEXT("type"), T) || T.IsEmpty()) { continue; }
					O->TryGetStringField(TEXT("directory"), Dir);
					FRecord Rec;
					Rec.Key = T;
					Rec.Kind = TEXT("placeable");
					Rec.Scope = S.Name;
					Rec.Display = T;
					Rec.Shape = Dir;      // its category, which is what moves when one is reclassified
					Rec.Evidence = EEvidence::Structure;
					Out.Records.Add(KeyOf(Rec), MoveTemp(Rec));
					S.Items++;
				}
				S.State = ECoverage::Complete;
			}
			else
			{
				S.State = ECoverage::Failed;
				S.Why = TEXT("asset_types.json could not be read as JSON");
			}
		}
		else
		{
			S.State = ECoverage::Unavailable;
			S.Why = FString::Printf(TEXT("%s is not there"), *Path);
		}
		Out.Scopes.Add(MoveTemp(S));
	}

	{
		FScope S; S.Name = TEXT("maps");
		const FString Dir = SdkRoot / TEXT("GodotProject/levels");
		if (FPaths::DirectoryExists(Dir))
		{
			CollectBasenames(Dir, TEXT("MP_*.tscn"), TEXT("map"), S.Name, Out, S.Items);
			S.State = ECoverage::Partial;
			S.Why = TEXT("names only: a map whose contents changed under the same name is not detected");
		}
		else { S.State = ECoverage::Unavailable; S.Why = TEXT("no levels folder"); }
		Out.Scopes.Add(MoveTemp(S));
	}

	{
		FScope S; S.Name = TEXT("models");
		const FString Dir = SdkRoot / TEXT("GodotProject/raw/models");
		if (FPaths::DirectoryExists(Dir))
		{
			CollectBasenames(Dir, TEXT("*.glb"), TEXT("model"), S.Name, Out, S.Items);
			S.State = ECoverage::Partial;
			S.Why = TEXT("names only: a model whose contents changed under the same name is not detected");
		}
		else { S.State = ECoverage::Unavailable; S.Why = TEXT("no models folder"); }
		Out.Scopes.Add(MoveTemp(S));
	}
	return true;
}

// ---------------------------------------------------------------------------
// THE PORTAL COLLECTOR
//
// Reads what the site has already told us, from the captures the Blocks editor
// and the settings panel keep. It does not browse: a sweep that navigates the
// user's editor is a separate, opt-in thing, and this needs to be safe to run
// at any moment.
// ---------------------------------------------------------------------------
static FString PortalCacheDir()
{
	return FPaths::Combine(BF6Ext::ToolSavedDir(), TEXT("portal"), TEXT("blockly"));
}

bool CollectPortal(FSnapshot& Out, FString& OutError)
{
	Out = FSnapshot();
	Out.Source = ESource::Portal;
	Out.CapturedUtc = FDateTime::UtcNow();
	Out.Context = TEXT("from the captures already on disk");

	{
		FScope S; S.Name = TEXT("blocks");
		FString Text;
		const FString Path = PortalCacheDir() / TEXT("definitions_synth.json");
		if (FFileHelper::LoadFileToString(Text, *Path) && !Text.IsEmpty())
		{
			TSharedPtr<FJsonObject> J;
			const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
			if (FJsonSerializer::Deserialize(R, J) && J.IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& It : J->Values)
				{
					FRecord Rec;
					Rec.Key = It.Key;
					Rec.Kind = TEXT("block");
					Rec.Scope = S.Name;
					Rec.Display = It.Key;
					// The definition itself is the shape: a block that gained a
					// socket or changed a dropdown is a change worth reporting.
					FString One;
					const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&One);
					if (It.Value.IsValid() && It.Value->Type == EJson::Object)
					{
						FJsonSerializer::Serialize(It.Value->AsObject().ToSharedRef(), W);
					}
					Rec.Shape = FMD5::HashAnsiString(*One).Left(12);
					Rec.Detail = FString::Printf(TEXT("%d bytes of definition"), One.Len());
					Rec.Evidence = EEvidence::Exposed;   // registered on the site's own editor
					Out.Records.Add(KeyOf(Rec), MoveTemp(Rec));
					S.Items++;
				}
				S.State = ECoverage::Complete;
				S.Why = TEXT("the block set the last Portal capture brought back");
			}
			else { S.State = ECoverage::Failed; S.Why = TEXT("the captured definitions are not readable JSON"); }
		}
		else
		{
			S.State = ECoverage::Unavailable;
			S.Why = TEXT("no Portal capture yet: sign in to Portal once and open Blocks");
		}
		Out.Scopes.Add(MoveTemp(S));
	}

	{
		// Settings come from the shipped catalogue plus whatever the live
		// overlay has learned since. The catalogue's own mined-at date is the
		// build, because that is what these records actually describe.
		FScope S; S.Name = TEXT("settings");
		FString Text;
		const FString Shipped = g_pluginDir / TEXT("Resources/portal/settings_catalog.json");
		if (FFileHelper::LoadFileToString(Text, *Shipped))
		{
			TSharedPtr<FJsonObject> J;
			const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
			const TArray<TSharedPtr<FJsonValue>>* Pages = nullptr;
			if (FJsonSerializer::Deserialize(R, J) && J.IsValid())
			{
				J->TryGetStringField(TEXT("appVersion"), Out.Build);
				FString MinedAt;
				J->TryGetStringField(TEXT("minedAt"), MinedAt);
				if (J->TryGetArrayField(TEXT("pages"), Pages))
				{
					for (const TSharedPtr<FJsonValue>& PV : *Pages)
					{
						const TSharedPtr<FJsonObject> P = PV->AsObject();
						if (!P.IsValid()) { continue; }
						FString PageName;
						P->TryGetStringField(TEXT("page"), PageName);
						const TArray<TSharedPtr<FJsonValue>>* Ss = nullptr;
						if (!P->TryGetArrayField(TEXT("settings"), Ss)) { continue; }
						for (const TSharedPtr<FJsonValue>& SV : *Ss)
						{
							const TSharedPtr<FJsonObject> O = SV->AsObject();
							FString Id;
							if (!O.IsValid() || !O->TryGetStringField(TEXT("testId"), Id) || Id.IsEmpty()) { continue; }
							FRecord Rec;
							Rec.Key = Id;
							Rec.Kind = TEXT("setting");
							Rec.Scope = S.Name;
							O->TryGetStringField(TEXT("title"), Rec.Display);
							if (Rec.Display.IsEmpty()) { Rec.Display = Id; }
							// What a user is allowed to enter: the range, and the
							// options THEMSELVES.
							//
							// This used to be a count. Two dropdowns with three
							// options each produced the same shape whether or not
							// the three were the same three, so a relabelled or
							// revalued option, or the whole list being replaced,
							// compared as identical. The values matter most: a
							// label is what a person reads and a value is what a
							// mod sends, and either moving is a real change.
							double Min = 0, Max = 0, Step = 0, Default = 0;
							const bool bMin = O->TryGetNumberField(TEXT("min"), Min);
							const bool bMax = O->TryGetNumberField(TEXT("max"), Max);
							O->TryGetNumberField(TEXT("step"), Step);
							const bool bDefault = O->TryGetNumberField(TEXT("default"), Default);
							bool bPerTeam = false;
							O->TryGetBoolField(TEXT("perTeam"), bPerTeam);

							const TArray<TSharedPtr<FJsonValue>>* Opts = nullptr;
							FString OptText;
							if (O->TryGetArrayField(TEXT("options"), Opts))
							{
								TArray<FString> Each;
								for (const TSharedPtr<FJsonValue>& OV : *Opts)
								{
									const TSharedPtr<FJsonObject> OO = OV.IsValid() ? OV->AsObject() : nullptr;
									if (!OO.IsValid()) { Each.Add(OV.IsValid() ? OV->AsString() : FString()); continue; }
									double Val = 0;
									FString Label;
									const bool bVal = OO->TryGetNumberField(TEXT("value"), Val);
									OO->TryGetStringField(TEXT("label"), Label);
									Each.Add(FString::Printf(TEXT("%s=%s"),
										bVal ? *FString::SanitizeFloat(Val) : TEXT("-"), *Label));
								}
								// Source order is kept: a reordered dropdown is a
								// change to what the user sees, so sorting these
								// would hide it.
								OptText = FString::Printf(TEXT(" options=[%s]"), *FString::Join(Each, TEXT(",")));
							}

							FString Kind2;
							O->TryGetStringField(TEXT("kind"), Kind2);
							Rec.Shape = BoundedShape(FString::Printf(
								TEXT("%s min=%s max=%s step=%g default=%s perTeam=%s%s"),
								*Kind2,
								bMin ? *FString::SanitizeFloat(Min) : TEXT("-"),
								bMax ? *FString::SanitizeFloat(Max) : TEXT("-"),
								Step,
								bDefault ? *FString::SanitizeFloat(Default) : TEXT("-"),
								bPerTeam ? TEXT("yes") : TEXT("no"),
								*OptText));
							Rec.Detail = PageName;
							Rec.Evidence = EEvidence::Exposed;
							Out.Records.Add(KeyOf(Rec), MoveTemp(Rec));
							S.Items++;
						}
					}
					S.State = ECoverage::Partial;
					S.Why = FString::Printf(
						TEXT("the catalogue mined on %s, not a fresh read of the live site"),
						MinedAt.IsEmpty() ? TEXT("an unrecorded date") : *MinedAt);
				}
				else { S.State = ECoverage::Failed; S.Why = TEXT("the catalogue has no pages"); }
			}
			else { S.State = ECoverage::Failed; S.Why = TEXT("the catalogue is not readable JSON"); }
		}
		else { S.State = ECoverage::Unavailable; S.Why = TEXT("no shipped settings catalogue"); }
		Out.Scopes.Add(MoveTemp(S));
	}
	return true;
}

// ---------------------------------------------------------------------------
// THE GAME COLLECTOR
//
// Only an identity check in this release: which install is being read, and
// whether a reader is there at all. Decoding maps and water structures is
// worth doing and is not worth guessing at, so this records what it can stand
// behind and marks the rest unavailable rather than implying it looked.
// ---------------------------------------------------------------------------
bool CollectGame(FSnapshot& Out, FString& OutError)
{
	Out = FSnapshot();
	Out.Source = ESource::Game;
	Out.CapturedUtc = FDateTime::UtcNow();

	FScope S; S.Name = TEXT("install");
	const bool bAddOn = BF6Api::HighPolyIsInstalled();
	const FString Install = BF6Api::GameInstallDir();
	if (Install.IsEmpty())
	{
		S.State = ECoverage::Unavailable;
		S.Why = TEXT("no Battlefield install is being read");
	}
	else
	{
		FRecord R;
		R.Key = TEXT("install");
		R.Kind = TEXT("game");
		R.Scope = S.Name;
		R.Display = TEXT("Battlefield install");
		R.Shape = Install;
		R.Detail = bAddOn ? TEXT("the High Poly reader is present") : TEXT("no High Poly reader");
		R.Evidence = EEvidence::Observed;
		Out.Records.Add(KeyOf(R), MoveTemp(R));
		Out.Context = Install;
		S.Items = 1;
		S.State = ECoverage::Partial;
		S.Why = TEXT("identity only: map, placement and water contents are not decoded by this scan");
	}
	Out.Scopes.Add(MoveTemp(S));
	return true;
}

// ---------------------------------------------------------------------------
// THE WATCHLIST
//
// Named candidates whose status is worth keeping across builds, seeded with
// what has actually been measured rather than with guesses.
//
// The tool cannot call into a running game, so the top of the evidence ladder
// is reached by handing the user a probe to paste into their own mod. Its
// output goes to the Portal log, which this tool already reads, so the answer
// comes back without anybody transcribing anything.
// ---------------------------------------------------------------------------
namespace
{
	struct FCandidate
	{
		const TCHAR* Name;
		const TCHAR* Kind;
		const TCHAR* Note;
		EEvidence    Known;
	};

	// Everything here is a recorded observation, with where it came from. The
	// runtime results are from a probe run in Night of the Undead that used four
	// positive controls, which is what makes its negatives worth anything.
	static const FCandidate kWatch[] =
	{
		{ TEXT("SetTickRate"), TEXT("native"),
		  TEXT("Real in the engine and rejected by name at upload. Takes a TickRates enum member, not a number: SetTickRate(1) is refused as a Number where TickRates was wanted. Rate_60Hz applies only on a local host, where the sim runs at client framerate; a measured online host reported exactly 30 Hz."),
		  EEvidence::RuntimeVerified },
		// Present at runtime, not verified in a running game: the probe asked
		// whether the name exists and called nothing, so nothing here is
		// measured behavior. SetTickRate above keeps the higher rung because a
		// host was actually measured at 30 Hz.
		{ TEXT("TickRates"), TEXT("enum"),
		  TEXT("Observed present at runtime alongside SetTickRate. Carries Rate_60Hz. Rejected by name at upload."),
		  EEvidence::RuntimePresent },
		{ TEXT("AutoPlayers_SetPlayerCount"), TEXT("native"),
		  TEXT("Part of the bot API, all of which was found present at runtime while undeclared in the typings. Nothing has been called."),
		  EEvidence::RuntimePresent },
		{ TEXT("AISetAwareness"), TEXT("native"),
		  TEXT("Probed at runtime and absent."), EEvidence::NotObserved },
		{ TEXT("EnableSpatialObject"), TEXT("native"),
		  TEXT("Probed at runtime and absent."), EEvidence::NotObserved },
		{ TEXT("UIDumpTree"), TEXT("native"),
		  TEXT("Probed at runtime and absent."), EEvidence::NotObserved },
		// THE FOUR WATER GETTERS. These exact spellings come from the archived
		// GlacierPortal/ModBuilder registries, not from memory: three sound
		// plausible that do not exist (GetWaterDepth, GetWaterNormal,
		// GetWaterVelocity return nothing anywhere), so the names are worth
		// being literal about.
		//
		// They are candidates, NOT absences. The runtime probe whose notes say
		// "all water functions" were missing drew its names from a Blockly block
		// catalogue, a different corpus from the registry these came out of, and
		// the probe's own list has since been deleted. So nothing on disk ties
		// that negative to these identifiers, and recording them as absent would
		// be inventing evidence.
		{ TEXT("GetWaterHeight"), TEXT("native"),
		  TEXT("In the archived ModBuilder registries, derived signature returns Number. An archived expression graph exists and needs host state. Not probed at runtime under this name, and absent from the shipped typings."),
		  EEvidence::Observed },
		{ TEXT("GetWaterIsEnabled"), TEXT("native"),
		  TEXT("In the archived ModBuilder registries, derived signature returns Boolean. No expression graph recorded. Not probed at runtime under this name, and absent from the shipped typings."),
		  EEvidence::Observed },
		{ TEXT("GetWaterBeaufortScale"), TEXT("native"),
		  TEXT("In the archived ModBuilder registries, derived signature returns Number. An archived expression graph exists and needs host state. Not probed at runtime under this name, and absent from the shipped typings."),
		  EEvidence::Observed },
		{ TEXT("GetWaterWaveAmplitude"), TEXT("native"),
		  TEXT("In the archived ModBuilder registries, derived signature returns Number. An archived expression graph exists and needs host state. Not probed at runtime under this name, and absent from the shipped typings."),
		  EEvidence::Observed },
	};
}

bool CollectWatchlist(FSnapshot& Out, FString& OutError)
{
	Out = FSnapshot();
	Out.Source = ESource::Watchlist;
	Out.CapturedUtc = FDateTime::UtcNow();
	Out.Build = TEXT("recorded");
	Out.Context = TEXT("named candidates, with the evidence behind each");

	FScope S; S.Name = TEXT("candidates");
	for (const FCandidate& C : kWatch)
	{
		FRecord R;
		R.Key = C.Name;
		R.Kind = C.Kind;
		R.Scope = S.Name;
		R.Display = C.Name;
		R.Shape = C.Kind;
		R.Detail = C.Note;
		R.Evidence = C.Known;
		Out.Records.Add(KeyOf(R), MoveTemp(R));
		S.Items++;
	}
	S.State = ECoverage::Complete;
	S.Why = TEXT("the list as recorded; running the probe in a game updates it");
	Out.Scopes.Add(MoveTemp(S));
	return true;
}

namespace
{
	// The four positive controls. If any of them reads false the probe is broken
	// and its negatives mean nothing.
	static const TCHAR* kControls[] =
	{
		TEXT("SpawnObject"), TEXT("GetObjectPosition"), TEXT("DealDamage"), TEXT("CreateVector")
	};
	static const int32 kControlCount = UE_ARRAY_COUNT(kControls);
	// Bumping this invalidates old logs on purpose: results whose lines were
	// produced by a different probe format cannot be read by this parser.
	static const TCHAR* kProbeFormat = TEXT("probe/2");
}

// Identifies the candidate list and the line format a run was built from. A run
// whose hash does not match this build was asking different questions, and
// pairing its answers with today's watchlist would attach a result to a name it
// was never asked about.
FString ProbeHash()
{
	FString Canon = FString(kProbeFormat) + TEXT("\n");
	for (const TCHAR* C : kControls) { Canon += FString(TEXT("control ")) + C + TEXT("\n"); }
	for (const FCandidate& C : kWatch) { Canon += FString(TEXT("candidate ")) + C.Name + TEXT("\n"); }
	return FMD5::HashAnsiString(*Canon).Left(12);
}

FString ProbeScript()
{
	// WHY THIS SHAPE. typeof on a string index never calls anything, so there
	// are no arguments to guess and nothing can crash. The controls are what
	// make a negative result mean anything: without them, a probe that was
	// broken and a capability that was absent look the same.
	//
	// WHY EVERY LINE CARRIES A RUN ID. The log is a rolling file that keeps
	// older runs. Reading it as one flat list let a failed run's answers be
	// certified by a later healthy run's controls, which is the opposite of what
	// the controls are for. The id is generated inside the run, so two runs can
	// never collide, and begin and end brackets are what tell a truncated log
	// from a complete one.
	const FString Hash = ProbeHash();
	FString Controls;
	for (int32 i = 0; i < kControlCount; ++i)
	{
		Controls += FString::Printf(TEXT("%s\"%s\""), i ? TEXT(", ") : TEXT(""), kControls[i]);
	}

	FString S;
	S += TEXT("// Paste into your mod, host it locally, and read the LOG section.\n");
	S += TEXT("// This only ASKS whether a name exists. It calls none of them, so a\n");
	S += TEXT("// \"present\" answer means the name is there and nothing more.\n");
	S += TEXT("{\n");
	S += TEXT("  const M = mod as unknown as Record<string, unknown>;\n");
	S += TEXT("  const has = (n: string) => typeof M[n] === \"function\" || (M[n] !== undefined && typeof M[n] === \"object\");\n");
	S += TEXT("  // One id for this run. Every line below carries it so results from\n");
	S += TEXT("  // two runs in one log can never be read as one run.\n");
	S += TEXT("  const run = \"r\" + Date.now().toString(36) + \"_\" + Math.floor(Math.random() * 1679616).toString(36);\n");
	S += TEXT("  const say = (s: string) => console.log(\"BF6PROBE \" + run + \" \" + s);\n");
	S += FString::Printf(TEXT("  console.log(\"BF6PROBE begin \" + run + \" probe=%s\");\n"), *Hash);
	S += FString::Printf(TEXT("  const controls = [%s];\n"), *Controls);
	S += TEXT("  const ok = controls.filter(has).length;\n");
	S += TEXT("  say(\"controls \" + ok + \"/\" + controls.length);\n");
	S += TEXT("  const names = [\n");
	for (const FCandidate& C : kWatch)
	{
		S += FString::Printf(TEXT("    \"%s\",\n"), C.Name);
	}
	S += TEXT("  ];\n");
	S += TEXT("  for (const n of names) say(n + \" \" + (has(n) ? \"present\" : \"absent\"));\n");
	S += TEXT("  console.log(\"BF6PROBE end \" + run + \" \" + names.length);\n");
	S += TEXT("}\n");
	return S;
}

namespace
{
	// One probe run as the log describes it. Nothing here is read until the run
	// is known to have started, ended, passed its controls and produced every
	// answer it promised.
	struct FProbeRun
	{
		FString Id;
		FString Hash;
		bool bBegan = false;
		bool bEnded = false;
		int32 Promised = -1;     // the count the end marker claims
		int32 Controls = -1;
		int32 ControlsOf = -1;
		TMap<FString, bool> Results;
		int32 FirstLine = 0;
	};
}

int32 IngestProbeLines(const TArray<FString>& Lines, FString& OutSummary)
{
	// Group by run id FIRST. The old parser accumulated every BF6PROBE line into
	// one map and kept whichever controls line came last, so a stale run's
	// absences were accepted on the strength of a later run's controls. Results
	// and the controls that certify them must come from the same run or they
	// certify nothing.
	TArray<FProbeRun> Runs;
	TMap<FString, int32> ById;
	int32 Untagged = 0;

	auto RunFor = [&Runs, &ById](const FString& Id, int32 LineNo) -> FProbeRun&
	{
		if (int32* Found = ById.Find(Id)) { return Runs[*Found]; }
		FProbeRun R; R.Id = Id; R.FirstLine = LineNo;
		ById.Add(Id, Runs.Add(MoveTemp(R)));
		return Runs.Last();
	};

	for (int32 LineNo = 0; LineNo < Lines.Num(); ++LineNo)
	{
		const FString& L = Lines[LineNo];
		const int32 At = L.Find(TEXT("BF6PROBE "));
		if (At == INDEX_NONE) { continue; }
		FString Rest = L.RightChop(At + 9).TrimStartAndEnd();

		if (Rest.StartsWith(TEXT("begin ")))
		{
			FString Body = Rest.RightChop(6).TrimStart();
			FString Id, Tail;
			if (!Body.Split(TEXT(" "), &Id, &Tail)) { Id = Body; }
			if (Id.IsEmpty()) { Untagged++; continue; }
			FProbeRun& R = RunFor(Id, LineNo);
			R.bBegan = true;
			const int32 P = Tail.Find(TEXT("probe="));
			if (P != INDEX_NONE)
			{
				R.Hash = Tail.RightChop(P + 6);
				int32 Sp = INDEX_NONE;
				if (R.Hash.FindChar(TEXT(' '), Sp)) { R.Hash.LeftInline(Sp); }
				R.Hash.TrimStartAndEndInline();
			}
			continue;
		}
		if (Rest.StartsWith(TEXT("end ")))
		{
			FString Body = Rest.RightChop(4).TrimStart();
			FString Id, Tail;
			if (!Body.Split(TEXT(" "), &Id, &Tail)) { Id = Body; }
			if (Id.IsEmpty()) { Untagged++; continue; }
			FProbeRun& R = RunFor(Id, LineNo);
			R.bEnded = true;
			R.Promised = FCString::Atoi(*Tail.TrimStartAndEnd());
			continue;
		}

		// Everything else is "<runId> <what>". A line with no run id is from a
		// probe older than this parser and cannot be tied to a run, so it is
		// counted and ignored rather than merged into one.
		FString Id, What;
		if (!Rest.Split(TEXT(" "), &Id, &What)) { Untagged++; continue; }
		What.TrimStartAndEndInline();
		if (Id.IsEmpty() || What.IsEmpty()) { Untagged++; continue; }

		// A line from a probe older than this format has a capability name where
		// the run id belongs, so "BF6PROBE SetTickRate present" would otherwise
		// invent a run called SetTickRate and then refuse it for having no start
		// marker, which reads as a broken run rather than as an old log. Ids the
		// script generates are "r<base36>_<base36>"; anything else is either an
		// old line or a line for a run whose start has already been seen.
		const bool bLooksLikeRunId = Id.StartsWith(TEXT("r")) && Id.Contains(TEXT("_"));
		if (!bLooksLikeRunId && !ById.Contains(Id)) { Untagged++; continue; }

		FProbeRun& R = RunFor(Id, LineNo);
		if (What.StartsWith(TEXT("controls ")))
		{
			const FString N = What.RightChop(9);
			int32 Slash = INDEX_NONE;
			if (N.FindChar(TEXT('/'), Slash))
			{
				R.Controls = FCString::Atoi(*N.Left(Slash));
				R.ControlsOf = FCString::Atoi(*N.RightChop(Slash + 1));
			}
			continue;
		}
		FString Name, State;
		if (What.Split(TEXT(" "), &Name, &State))
		{
			R.Results.Add(Name, State.TrimStartAndEnd() == TEXT("present"));
		}
	}

	const FString Want = ProbeHash();

	// Newest usable run wins, and the reason the others were refused is what the
	// user needs to read when none of them is usable.
	const FProbeRun* Use = nullptr;
	FString Refused;
	for (int32 i = Runs.Num() - 1; i >= 0; --i)
	{
		const FProbeRun& R = Runs[i];
		const TCHAR* Why = nullptr;
		if (!R.bBegan)                            { Why = TEXT("its start marker is not in the log, so it may be cut off at the top"); }
		else if (!R.bEnded)                       { Why = TEXT("it has no end marker, so it did not finish or the log is cut off"); }
		else if (R.Hash != Want)                  { Why = TEXT("it was built from a different candidate list than this build has"); }
		else if (R.ControlsOf != kControlCount)   { Why = TEXT("it reported a different number of controls than this build expects"); }
		else if (R.Controls != R.ControlsOf)      { Why = TEXT("its controls did not all pass, so the probe itself was not working"); }
		else if (R.Results.Num() != R.Promised)   { Why = TEXT("it produced fewer answers than it promised, so its output is partial"); }
		if (!Why) { Use = &R; break; }
		if (Refused.IsEmpty())
		{
			Refused = FString::Printf(TEXT("the newest run (%s) was not used because %s."), *R.Id, Why);
		}
	}

	if (!Use)
	{
		if (Runs.Num() == 0)
		{
			OutSummary = Untagged > 0
				? FString::Printf(
					TEXT("found %d probe line(s) with no run id. They are from an older probe and cannot be tied to a single run, ")
					TEXT("so nothing was recorded. Copy the probe again and re-run it."), Untagged)
				: FString(TEXT("no probe output found in the log yet. Host a match locally with the probe in your mod."));
			return 0;
		}
		OutSummary = FString::Printf(
			TEXT("found %d probe run(s) and used none of them. %s Nothing was recorded."),
			Runs.Num(), *Refused);
		return 0;
	}

	FSnapshot Snap;
	FString Err;
	CollectWatchlist(Snap, Err);
	// Run identity goes in the context, which is deliberately NOT part of the
	// content id: two runs that found the same thing share one stored blob and
	// are still two separate observations of it.
	//
	// The game build is not recorded here because a mod cannot read it without
	// calling something, and the probe's whole safety property is that it calls
	// nothing. What ties these results to a known set of questions is the probe
	// hash, and that is what is claimed.
	Snap.Context = FString::Printf(TEXT("probe run %s, probe %s, %d of %d controls passed"),
		*Use->Id, *Use->Hash, Use->Controls, Use->ControlsOf);

	int32 Updated = 0;
	for (TPair<FString, FRecord>& It : Snap.Records)
	{
		const bool* Found = Use->Results.Find(It.Value.Key);
		if (!Found) { continue; }   // not asked about in this run: leave what was recorded
		// Present means the name exists. The probe called nothing, so this is
		// never RuntimeVerified: reporting a symbol lookup as measured behavior
		// is the most misleading thing this could claim.
		It.Value.Evidence = *Found ? EEvidence::RuntimePresent : EEvidence::NotObserved;
		It.Value.Detail = *Found
			? FString::Printf(TEXT("the name exists in a running game (run %s, all %d controls passed). Nothing was called, so this is not proof it works."),
				*Use->Id, Use->ControlsOf)
			: FString::Printf(TEXT("the name does not exist in a running game (run %s, all %d controls passed)."),
				*Use->Id, Use->ControlsOf);
		Updated++;
	}

	FSnapshot Stored;
	if (Updated && !Record(Snap, Stored, Err))
	{
		OutSummary = FString::Printf(TEXT("read %d result(s) but could not record them: %s"), Updated, *Err);
		return Updated;
	}
	OutSummary = FString::Printf(
		TEXT("recorded %d result(s) from run %s, whose %d controls all passed.%s%s"),
		Updated, *Use->Id, Use->ControlsOf,
		Runs.Num() > 1 ? TEXT(" Other runs in the log were left alone.") : TEXT(""),
		Untagged > 0 ? TEXT(" Lines with no run id were ignored.") : TEXT(""));
	return Updated;
}

// ---------------------------------------------------------------------------
// SCANNING, AS ONE CALL
// ---------------------------------------------------------------------------
namespace
{
	TArray<IConsoleObject*> GCommands;
	TSharedPtr<SMultiLineEditableTextBox> GOut;
	TSharedPtr<STextBlock> GStatus;
	bool GWantSdk = true, GWantPortal = true, GWantGame = true, GWantWatch = true;

	void Say(const FString& Text)
	{
		if (GOut.IsValid()) { GOut->SetText(FText::FromString(Text)); }
	}
	void Status(const FString& Text)
	{
		if (GStatus.IsValid()) { GStatus->SetText(FText::FromString(Text)); }
	}
}

// Collect the chosen sources, publish each, and report each against whatever
// came before it. Every source is independent: one being unavailable never
// stops the others, because "the SDK moved but the site did not" is exactly the
// kind of answer this exists to give.
FString ScanNow(bool bSdk, bool bPortal, bool bGame, bool bWatch)
{
	FString Report;
	auto One = [&Report](ESource Src, bool (*Collect)(FSnapshot&, FString&))
	{
		FSnapshot Fresh;
		FString Err;
		if (!Collect(Fresh, Err))
		{
			Report += FString::Printf(TEXT("## %s\n\ncould not be collected: %s\n\n"), SourceName(Src), *Err);
			return;
		}
		// The comparison is against the previous OBSERVATION, not against
		// whatever content blob happens to sort newest. Reading the store back
		// after recording used to be how "current" was decided, and on a return
		// to content that was already stored it handed back the wrong snapshot
		// with the timestamp of the first time that content was ever seen.
		FSnapshot Before;
		const bool bHadBefore = Latest(Src, Before);

		FSnapshot Stored;
		if (!Record(Fresh, Stored, Err))
		{
			Report += FString::Printf(TEXT("## %s\n\ncollected but not recorded: %s\n\n"), SourceName(Src), *Err);
			return;
		}

		if (!bHadBefore)
		{
			// The first look at a source cannot be a comparison, and calling it
			// "no changes" would be a lie by omission.
			Report += FString::Printf(
				TEXT("## %s\n\nBaseline recorded: %d capability record(s). ")
				TEXT("There is nothing earlier to compare against, so nothing here is new or missing yet.\n\n"),
				SourceName(Src), Stored.Records.Num());
			for (const FScope& S : Stored.Scopes)
			{
				Report += FString::Printf(TEXT("- %s: %s%s\n"), *S.Name, CoverageName(S.State),
					S.Why.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *S.Why));
			}
			Report += TEXT("\n");
			return;
		}
		if (!Before.Collector.IsEmpty() && Before.Collector != Stored.Collector)
		{
			// Say this instead of showing a diff. Every shape would read as
			// changed, and a reader has no way to tell a rewritten collector
			// from a rewritten SDK by looking at the list.
			Report += FString::Printf(
				TEXT("## %s\n\nRe-baselined. The previous scan was taken by collector %s and this one by %s, ")
				TEXT("so their shapes are not comparable and no changes are being claimed. ")
				TEXT("This scan is observation %d, and the next scan will compare against it.\n\n"),
				SourceName(Src), *Before.Collector, *Stored.Collector, Stored.Seq);
			return;
		}
		if (Before.Id == Stored.Id)
		{
			// The observation was still appended: "we looked again and it was the
			// same" is a fact worth having, and it is what makes a later return
			// to earlier content visible as a change rather than as silence.
			Report += FString::Printf(
				TEXT("## %s\n\nNothing changed since the scan at %s. This scan was recorded as observation %d.\n\n"),
				SourceName(Src), *Before.ObservedUtc.ToString(), Stored.Seq);
			return;
		}
		const TArray<FChange> Changes = Compare(Before, Stored);
		Report += ReportMarkdown(Before, Stored, Changes);
		Report += TEXT("\n");
	};

	if (bSdk)    { One(ESource::Sdk, &CollectSdk); }
	if (bPortal) { One(ESource::Portal, &CollectPortal); }
	if (bGame)   { One(ESource::Game, &CollectGame); }
	if (bWatch)  { One(ESource::Watchlist, &CollectWatchlist); }

	if (Report.IsEmpty()) { Report = TEXT("No sources were selected.\n"); }

	// Keep the report beside the snapshots so it can be read again, and by the
	// MCP tools, without re-running the scan.
	const FString Dir = FPaths::Combine(Root(), TEXT("reports"));
	IFileManager::Get().MakeDirectory(*Dir, true);
	FFileHelper::SaveStringToFile(Report,
		*(Dir / FString::Printf(TEXT("scan_%s.md"), *FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")))),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	return Report;
}

// ---------------------------------------------------------------------------
// THE SECTION ON THE BUILD SCREEN
// ---------------------------------------------------------------------------
TSharedRef<SWidget> Widget()
{
	auto Toggle = [](const TCHAR* Label, const TCHAR* Tip, bool* Flag)
	{
		return SNew(SCheckBox)
			.IsChecked_Lambda([Flag] { return *Flag ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([Flag](ECheckBoxState S) { *Flag = (S == ECheckBoxState::Checked); })
			.ToolTipText(FText::FromString(Tip))
			[
				SNew(STextBlock).Text(FText::FromString(Label))
			];
	};

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 8, 8, 2)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(FText::FromString(TEXT(
				"What each source says this build can do, and what moved since the last scan. "
				"The installed SDK, the Portal website and the running game disagree with each other, "
				"and knowing which one you are looking at is the point.")))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 6, 8, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
			[ Toggle(TEXT("Installed SDK"), TEXT("The Portal SDK folder set up in this project. Works offline."), &GWantSdk) ]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
			[ Toggle(TEXT("Portal website"), TEXT("What the last Portal capture brought back. Reads what is already on disk; it does not browse."), &GWantPortal) ]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
			[ Toggle(TEXT("Installed game"), TEXT("Which install is being read. Needs a game folder; no game files leave this machine."), &GWantGame) ]
			+ SHorizontalBox::Slot().AutoWidth()
			[ Toggle(TEXT("Watchlist"), TEXT("Named candidates such as SetTickRate, with the evidence recorded for each."), &GWantWatch) ]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 4, 8, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Scan now")))
				.ToolTipText(FText::FromString(TEXT("Collect the chosen sources and report what moved since last time.")))
				.OnClicked_Lambda([]
				{
					Status(TEXT("scanning..."));
					const FString R = ScanNow(GWantSdk, GWantPortal, GWantGame, GWantWatch);
					Say(R);
					Status(FString::Printf(TEXT("scanned at %s UTC"), *FDateTime::UtcNow().ToString()));
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Copy the runtime probe")))
				.ToolTipText(FText::FromString(TEXT(
					"Copies a snippet that asks the engine which of the watchlist names exist. "
					"Paste it into your mod, host locally, then press READ PROBE RESULTS. "
					"It calls none of them.")))
				.OnClicked_Lambda([]
				{
					FPlatformApplicationMisc::ClipboardCopy(*ProbeScript());
					Say(ProbeScript());
					Status(TEXT("probe copied. Paste it into your mod, host a local match, then read the results."));
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Read probe results")))
				.ToolTipText(FText::FromString(TEXT("Read the probe's output out of the Portal log and record what it found.")))
				.OnClicked_Lambda([]
				{
					const BF6GameLog::FResult L = BF6GameLog::ReadLocal(4000);
					if (!L.bFound) { Status(L.Why); return FReply::Handled(); }
					TArray<FString> Lines;
					for (const BF6GameLog::FEntry& E : L.Entries) { Lines.Add(E.Text); }
					FString Summary;
					IngestProbeLines(Lines, Summary);
					Status(Summary);
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SAssignNew(GStatus, STextBlock)
				.AutoWrapText(true)
				.Text(FText::FromString(TEXT("Nothing scanned yet in this session.")))
			]
		]
		+ SVerticalBox::Slot().FillHeight(1.f).Padding(8, 0, 8, 8)
		[
			SAssignNew(GOut, SMultiLineEditableTextBox)
			.IsReadOnly(true)
			.AlwaysShowScrollbars(true)
			.Text(FText::FromString(TEXT(
				"Press SCAN NOW.\n\n"
				"The first scan of a source records a baseline and reports no changes, because there is "
				"nothing earlier to compare it against. The one after it is where this starts being useful.")))
		];
}

void ReleaseWidget()
{
	GOut.Reset();
	GStatus.Reset();
}

void Register()
{
	GCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BF6.Caps.Scan"),
		TEXT("Collect every capability source and report what changed since the last scan."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			const FString R = ScanNow(true, true, true, true);
			TArray<FString> Lines;
			R.ParseIntoArrayLines(Lines, false);
			for (const FString& L : Lines) { UE_LOG(LogBF6Caps, Display, TEXT("%s"), *L); }
		}), ECVF_Default));

	GCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BF6.Caps.Status"),
		TEXT("Say what snapshots exist for each source and when they were taken."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			UE_LOG(LogBF6Caps, Display, TEXT("capability snapshots under %s"), *Root());
			for (const ESource S : { ESource::Sdk, ESource::Portal, ESource::Game, ESource::Watchlist })
			{
				// Scans and stored contents are counted separately on purpose:
				// five scans of an unchanged SDK is one blob and five
				// observations, and collapsing those two numbers into one is
				// what made a repeat scan look like it had not happened.
				const TArray<int32> Seqs = ObservationSeqs(S);
				const TArray<FString> Ids = List(S);
				FSnapshot Snap;
				if (Seqs.Num() == 0 || !ObservationBack(S, 0, Snap))
				{
					UE_LOG(LogBF6Caps, Display, TEXT("  %-9s : none yet"), SourceName(S));
					continue;
				}
				UE_LOG(LogBF6Caps, Display,
					TEXT("  %-9s : %d scan(s) of %d distinct content(s), newest observation %d at %s, content %s with %d record(s)"),
					SourceName(S), Seqs.Num(), Ids.Num(), Snap.Seq,
					*Snap.ObservedUtc.ToString(), *Snap.Id, Snap.Records.Num());
				for (const FScope& Sc : Snap.Scopes)
				{
					UE_LOG(LogBF6Caps, Display, TEXT("      %-12s %-12s %s"),
						*Sc.Name, CoverageName(Sc.State), *Sc.Why);
				}
			}
		}), ECVF_Default));

	GCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BF6.Caps.Probe"),
		TEXT("Print the runtime probe to paste into a mod, and copy it to the clipboard."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			FPlatformApplicationMisc::ClipboardCopy(*ProbeScript());
			UE_LOG(LogBF6Caps, Display, TEXT("%s"), *ProbeScript());
			UE_LOG(LogBF6Caps, Display,
				TEXT("Copied. Paste into your mod, host a local match, then run BF6.Caps.ReadProbe."));
		}), ECVF_Default));

	GCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BF6.Caps.ReadProbe"),
		TEXT("Read the runtime probe's output out of the Portal log and record what it found."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			const BF6GameLog::FResult L = BF6GameLog::ReadLocal(4000);
			if (!L.bFound) { UE_LOG(LogBF6Caps, Warning, TEXT("%s"), *L.Why); return; }
			TArray<FString> Lines;
			for (const BF6GameLog::FEntry& E : L.Entries) { Lines.Add(E.Text); }
			FString Summary;
			IngestProbeLines(Lines, Summary);
			UE_LOG(LogBF6Caps, Display, TEXT("%s"), *Summary);
		}), ECVF_Default));

	UE_LOG(LogBF6Caps, Display,
		TEXT("BF6 Capabilities ready. CHANGES on the build screen, or BF6.Caps.Scan."));
}

void Unregister()
{
	for (IConsoleObject* C : GCommands) { IConsoleManager::Get().UnregisterConsoleObject(C); }
	GCommands.Reset();
	ReleaseWidget();
}

} // namespace BF6Caps
