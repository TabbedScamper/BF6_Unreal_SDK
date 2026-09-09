#include "BF6GameLog.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Containers/Ticker.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY(LogBF6GameLog);

namespace BF6GameLog
{
namespace
{
	TArray<IConsoleObject*> GCommands;
	FGuid GConsoleWatch;        // BF6.Log.Watch's own subscription

	// The file Battlefield writes inside that folder.
	static const TCHAR* LOG_NAME = TEXT("PortalLog.txt");

	FString TempRoot()
	{
		FString T = FPlatformMisc::GetEnvironmentVariable(TEXT("TEMP"));
		if (T.IsEmpty()) { T = FPlatformMisc::GetEnvironmentVariable(TEXT("TMP")); }
		return T;
	}

	// One line of the file:
	//   [UTC 2026-09-04 07:01:26] Mod started
	//   [UTC 2026-09-04 07:01:31] QuickJS: console.log: [PITFALL] pads ready
	FEntry ParseLine(const FString& Line)
	{
		FEntry E;
		FString Rest = Line;

		if (Rest.StartsWith(TEXT("[UTC ")))
		{
			int32 Close = INDEX_NONE;
			if (Rest.FindChar(TEXT(']'), Close) && Close > 5)
			{
				E.TimestampUtc = Rest.Mid(5, Close - 5).TrimStartAndEnd();
				Rest = Rest.Mid(Close + 1).TrimStart();
			}
		}

		// QuickJS is the script engine, so anything it prefixes came from the
		// mod rather than from the game around it. That distinction is the
		// difference between "my code said this" and "the engine said this",
		// and it is the first thing anybody reading this file wants.
		const FString QuickJs = TEXT("QuickJS: ");
		if (Rest.StartsWith(QuickJs))
		{
			Rest = Rest.RightChop(QuickJs.Len());
			if (Rest.StartsWith(TEXT("console.log: ")))
			{
				E.Kind = TEXT("console.log");
				Rest = Rest.RightChop(13);
			}
			else if (Rest.StartsWith(TEXT("console.error: ")))
			{
				E.Kind = TEXT("error");
				Rest = Rest.RightChop(15);
			}
			else
			{
				E.Kind = TEXT("script");
			}
		}
		else
		{
			E.Kind = TEXT("system");
			// "Script loading failed" and friends are the ones that matter most
			// and they carry no marker, so they are promoted here.
			if (Rest.Contains(TEXT("fail"), ESearchCase::IgnoreCase) ||
				Rest.Contains(TEXT("error"), ESearchCase::IgnoreCase))
			{
				E.Kind = TEXT("error");
			}
		}

		E.Text = Rest;
		return E;
	}

	FResult ReadFrom(const FString& Path, int32 MaxEntries)
	{
		FResult R;
		R.Path = Path;
		if (Path.IsEmpty() || !FPaths::FileExists(Path))
		{
			R.Why = FString::Printf(TEXT("no log file at %s"), *Path);
			return R;
		}

		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			// The game can hold the file open while it runs; that is not a
			// reason to fail silently.
			R.Why = FString::Printf(TEXT("found %s but could not read it. If the game "
				"is running, the file may be locked; try again after leaving the match."), *Path);
			return R;
		}

		R.bFound = true;
		R.Bytes = IFileManager::Get().FileSize(*Path);
		R.Written = IFileManager::Get().GetTimeStamp(*Path);

		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);

		// The tail is what matters: the last run is at the end, and a long
		// session can be thousands of lines.
		const int32 First = (MaxEntries > 0 && Lines.Num() > MaxEntries)
			? Lines.Num() - MaxEntries : 0;
		for (int32 i = First; i < Lines.Num(); ++i)
		{
			if (Lines[i].TrimStartAndEnd().IsEmpty()) { continue; }
			R.Entries.Add(ParseLine(Lines[i]));
		}
		return R;
	}
}

FString LocateFolder()
{
	const FString Root = TempRoot();
	if (Root.IsEmpty()) { return FString(); }

	// Pattern, never a literal: see the note in the header about the name on
	// disk carrying permanent mojibake for the trademark sign.
	TArray<FString> Found;
	IFileManager::Get().FindFiles(Found, *(Root / TEXT("Battlefield*")), false, true);

	FString Best;
	FDateTime BestTime = FDateTime::MinValue();
	for (const FString& Name : Found)
	{
		const FString Full = Root / Name;
		// Only a folder that actually holds the log counts, so a stray
		// "Battlefield" folder from something else cannot win.
		if (!FPaths::FileExists(Full / LOG_NAME)) { continue; }
		const FDateTime When = IFileManager::Get().GetTimeStamp(*(Full / LOG_NAME));
		if (When > BestTime) { BestTime = When; Best = Full; }
	}
	return Best;
}

FResult ReadLocal(int32 MaxEntries)
{
	const FString Folder = LocateFolder();
	if (Folder.IsEmpty())
	{
		FResult R;
		R.Why = FString::Printf(
			TEXT("no Portal log found. Looked for a folder matching \"Battlefield*\" ")
			TEXT("holding %s under %s. The game writes it on PC only, and only after a ")
			TEXT("mod has run, so play a round on localhost first."),
			LOG_NAME, *TempRoot());
		return R;
	}
	return ReadFrom(Folder / LOG_NAME, MaxEntries);
}

FResult ReadFile(const FString& Path, int32 MaxEntries)
{
	return ReadFrom(Path, MaxEntries);
}

namespace
{
	FTSTicker::FDelegateHandle GWatchTick;
	// One reader, many listeners. This used to be a single FOnLines that
	// StartWatch overwrote, so the LOG panel and the Blocks editor evicted each
	// other: whichever pressed Watch second took the delegate, and whichever
	// pressed Stop first tore down the poller under the other, which carried on
	// claiming it was watching. The file is read once either way, so the only
	// thing that ever needed to be per-consumer was the callback.
	TMap<FGuid, FOnLines> GListeners;
	FString GWatchPath;
	int64 GOffset = 0;
	bool GSawFile = false;

	// Read only what was added, with a handle that lets the game keep writing.
	// FFileHelper would take the whole file and a stricter share mode.
	bool ReadDelta(const FString& Path, int64& InOutOffset, FString& OutText, bool& bOutReset)
	{
		bOutReset = false;
		IFileHandle* H = FPlatformFileManager::Get().GetPlatformFile()
			.OpenRead(*Path, /*bAllowWrite*/ true);
		if (!H) { return false; }

		const int64 Size = H->Size();
		if (Size < InOutOffset)
		{
			// Truncated: a new mod run started. Begin again from the top.
			InOutOffset = 0;
			bOutReset = true;
		}
		if (Size == InOutOffset) { delete H; return true; }

		const int64 Want = Size - InOutOffset;
		TArray<uint8> Buf;
		Buf.SetNumUninitialized(static_cast<int32>(FMath::Min<int64>(Want, 4 * 1024 * 1024)));
		H->Seek(InOutOffset);
		const bool bOk = H->Read(Buf.GetData(), Buf.Num());
		delete H;
		if (!bOk) { return false; }

		InOutOffset += Buf.Num();
		Buf.Add(0);
		OutText = FString(UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(Buf.GetData())));
		return true;
	}
}

FGuid AddWatcher(FOnLines OnLines, float IntervalSeconds)
{
	const FGuid Handle = FGuid::NewGuid();
	GListeners.Add(Handle, OnLines);

	// A second listener joins the poll already running rather than restarting
	// it. Restarting would rewind the offset and replay the whole file at the
	// consumer that was already up to date.
	if (GWatchTick.IsValid())
	{
		UE_LOG(LogBF6GameLog, Display, TEXT("another view is watching the Portal log (%d now)"),
			GListeners.Num());
		return Handle;
	}

	GWatchPath.Reset();
	GOffset = 0;
	GSawFile = false;

	const float Every = FMath::Clamp(IntervalSeconds, 0.25f, 30.0f);
	GWatchTick = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float) -> bool
		{
			// The folder is re-located each tick until found, so the watch can
			// be started before the game is, which is the normal order: press
			// watch, then go and play.
			if (GWatchPath.IsEmpty())
			{
				const FString Folder = LocateFolder();
				if (Folder.IsEmpty()) { return true; }
				GWatchPath = Folder / LOG_NAME;
				GOffset = 0;
			}

			FString Chunk;
			bool bReset = false;
			if (!ReadDelta(GWatchPath, GOffset, Chunk, bReset))
			{
				// Vanished: the folder can be cleared between runs.
				GWatchPath.Reset();
				return true;
			}
			if (!GSawFile)
			{
				GSawFile = true;
				UE_LOG(LogBF6GameLog, Display, TEXT("watching %s"), *GWatchPath);
			}
			if (Chunk.IsEmpty()) { return true; }

			TArray<FString> Lines;
			Chunk.ParseIntoArrayLines(Lines, false);
			TArray<FEntry> Entries;
			for (const FString& L : Lines)
			{
				if (L.TrimStartAndEnd().IsEmpty()) { continue; }
				Entries.Add(ParseLine(L));
			}
			if (Entries.Num())
			{
				// Copied first: a listener is free to close its own panel from
				// inside its callback, which would otherwise mutate the map
				// mid-iteration.
				TArray<FOnLines> Bound;
				for (const TPair<FGuid, FOnLines>& It : GListeners)
				{
					if (It.Value.IsBound()) { Bound.Add(It.Value); }
				}
				for (const FOnLines& D : Bound) { D.Execute(Entries, bReset); }
			}
			return true;
		}), Every);

	UE_LOG(LogBF6GameLog, Display, TEXT("watching the Portal log every %.2fs"), Every);
	return Handle;
}

void RemoveWatcher(const FGuid& Handle)
{
	if (GListeners.Remove(Handle) == 0) { return; }
	// The poller stops only when the last view has gone, so closing one panel
	// leaves the other one still receiving lines.
	if (GListeners.Num() > 0)
	{
		UE_LOG(LogBF6GameLog, Display, TEXT("a view stopped watching (%d still watching)"),
			GListeners.Num());
		return;
	}
	StopWatch();
}

void StopWatch()
{
	if (GWatchTick.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GWatchTick);
		GWatchTick.Reset();
		UE_LOG(LogBF6GameLog, Display, TEXT("stopped watching the Portal log"));
	}
	GListeners.Reset();
	GWatchPath.Reset();
	GOffset = 0;
	GSawFile = false;
}

bool IsWatching() { return GWatchTick.IsValid(); }
int32 WatcherCount() { return GListeners.Num(); }

// ---------------------------------------------------------------------------
// The LOG sheet.
// ---------------------------------------------------------------------------
namespace
{
	TSharedPtr<SMultiLineEditableTextBox> GBox;
	TSharedPtr<STextBlock> GWhere;
	TArray<FString> GShown;
	bool GPanelWatching = false;
	FGuid GPanelWatch;          // this panel's own subscription, not the whole poll

	void Paint()
	{
		if (!GBox.IsValid()) { return; }
		// The tail is what anybody is reading, and an unbounded box in a match
		// that logs every frame would grow without limit.
		while (GShown.Num() > 2000) { GShown.RemoveAt(0, 200, EAllowShrinking::No); }
		GBox->SetText(FText::FromString(FString::Join(GShown, TEXT("\n"))));
		GBox->ScrollTo(ETextLocation::EndOfDocument);
	}

	FString Format(const FEntry& E)
	{
		const FString When = E.TimestampUtc.IsEmpty()
			? FString(TEXT("--:--:--"))
			: (E.TimestampUtc.Len() > 11 ? E.TimestampUtc.RightChop(11) : E.TimestampUtc);
		const FString Tag = (E.Kind == TEXT("console.log")) ? FString() : (E.Kind + TEXT(": "));
		return When + TEXT("  ") + Tag + E.Text;
	}

	void SetWhere(const FString& Text)
	{
		if (GWhere.IsValid()) { GWhere->SetText(FText::FromString(Text)); }
	}

	FReply OnPull()
	{
		const FResult R = ReadLocal(1000);
		GShown.Reset();
		if (!R.bFound) { SetWhere(R.Why); Paint(); return FReply::Handled(); }
		for (const FEntry& E : R.Entries) { GShown.Add(Format(E)); }
		SetWhere(FString::Printf(TEXT("%s  -  %d line(s), written %s"),
			*R.Path, R.Entries.Num(), *R.Written.ToString()));
		Paint();
		return FReply::Handled();
	}

	FReply OnWatch()
	{
		GPanelWatching = !GPanelWatching;
		if (!GPanelWatching)
		{
			// Only this panel's subscription: the Blocks editor may still be
			// watching, and turning off the LOG section must not stop it.
			RemoveWatcher(GPanelWatch);
			GPanelWatch.Invalidate();
			SetWhere(TEXT("not watching"));
			return FReply::Handled();
		}
		GPanelWatch = AddWatcher(FOnLines::CreateLambda([](const TArray<FEntry>& Entries, bool bNewSession)
		{
			if (bNewSession) { GShown.Add(TEXT("---- the mod restarted, this is a new session ----")); }
			for (const FEntry& E : Entries) { GShown.Add(Format(E)); }
			Paint();
		}), 1.0f);
		const FString Folder = LocateFolder();
		SetWhere(Folder.IsEmpty()
			? TEXT("watching. The file appears once a mod runs on Host Locally.")
			: FString::Printf(TEXT("watching %s"), *(Folder / LOG_NAME)));
		return FReply::Handled();
	}
}

TSharedRef<SWidget> Widget()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 8, 8, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Pull the log")))
				.ToolTipText(FText::FromString(TEXT("Read what your mod printed during the last Host Locally session.")))
				.OnClicked_Static(&OnPull)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
			[
				SNew(SButton)
				.Text_Lambda([] { return FText::FromString(GPanelWatching
					? TEXT("Stop watching") : TEXT("Watch live")); })
				.ToolTipText(FText::FromString(TEXT("Follow the log while you play. Start this before you launch.")))
				.OnClicked_Static(&OnWatch)
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SAssignNew(GWhere, STextBlock)
				.Text(FText::FromString(TEXT("Nothing read yet. Press PULL THE LOG after a session, or WATCH LIVE before one.")))
				.AutoWrapText(true)
			]
		]
		+ SVerticalBox::Slot().FillHeight(1.f).Padding(8, 0, 8, 8)
		[
			SAssignNew(GBox, SMultiLineEditableTextBox)
			.IsReadOnly(true)
			.AlwaysShowScrollbars(true)
			.Text(FText::GetEmpty())
		];
}

void ReleaseWidget()
{
	// The watch is deliberately NOT stopped here. Leaving the sheet to go and
	// play is the normal way to use it, and a watcher that stopped the moment
	// you looked away would never see the session it was armed for.
	GBox.Reset();
	GWhere.Reset();
}

void Register()
{
	GCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BF6.Log.Where"),
		TEXT("Say where Battlefield's Portal log is, without reading it."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			const FString Folder = LocateFolder();
			if (Folder.IsEmpty())
			{
				UE_LOG(LogBF6GameLog, Display,
					TEXT("no Portal log yet. Looked under %s for a Battlefield* folder "
						 "holding %s."), *TempRoot(), LOG_NAME);
				return;
			}
			const FString File = Folder / LOG_NAME;
			UE_LOG(LogBF6GameLog, Display, TEXT("Portal log: %s (%lld bytes, written %s)"),
				*File, IFileManager::Get().FileSize(*File),
				*IFileManager::Get().GetTimeStamp(*File).ToString());
		}),
		ECVF_Default));

	GCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BF6.Log.Pull"),
		TEXT("Read the Portal log from the last localhost session. Optional: how many lines."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const int32 Want = Args.Num() ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 20000) : 60;
			const FResult R = ReadLocal(Want);
			if (!R.bFound)
			{
				UE_LOG(LogBF6GameLog, Warning, TEXT("%s"), *R.Why);
				return;
			}
			UE_LOG(LogBF6GameLog, Display, TEXT("Portal log: %s, %lld bytes, written %s"),
				*R.Path, R.Bytes, *R.Written.ToString());
			for (const FEntry& E : R.Entries)
			{
				UE_LOG(LogBF6GameLog, Display, TEXT("  [%s] %s%s"),
					E.TimestampUtc.IsEmpty() ? TEXT("--") : *E.TimestampUtc,
					E.Kind == TEXT("console.log") ? TEXT("") : *(E.Kind + TEXT(": ")),
					*E.Text);
			}
			UE_LOG(LogBF6GameLog, Display, TEXT("  (%d entries)"), R.Entries.Num());
		}),
		ECVF_Default));

	GCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BF6.Log.Watch"),
		TEXT("Follow the Portal log live while you play. Optional: seconds between checks."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			// The console toggles its own subscription. It used to call
			// StopWatch, which silently killed the LOG panel's watch too.
			if (GConsoleWatch.IsValid())
			{
				RemoveWatcher(GConsoleWatch);
				GConsoleWatch.Invalidate();
				UE_LOG(LogBF6GameLog, Display, TEXT("console stopped watching the Portal log"));
				return;
			}
			const float Every = Args.Num() ? FCString::Atof(*Args[0]) : 1.0f;
			GConsoleWatch = AddWatcher(FOnLines::CreateLambda(
				[](const TArray<FEntry>& Entries, bool bNewSession)
				{
					if (bNewSession)
					{
						UE_LOG(LogBF6GameLog, Display,
							TEXT("---- the mod restarted, this is a new session ----"));
					}
					for (const FEntry& E : Entries)
					{
						if (E.Kind == TEXT("error"))
						{
							UE_LOG(LogBF6GameLog, Warning, TEXT("[%s] %s"),
								*E.TimestampUtc, *E.Text);
						}
						else
						{
							UE_LOG(LogBF6GameLog, Display, TEXT("[%s] %s%s"),
								E.TimestampUtc.IsEmpty() ? TEXT("--") : *E.TimestampUtc,
								E.Kind == TEXT("console.log") ? TEXT("") : *(E.Kind + TEXT(": ")),
								*E.Text);
						}
					}
				}), Every);
		}),
		ECVF_Default));
}

void Unregister()
{
	StopWatch();
	for (IConsoleObject* C : GCommands)
	{
		if (C) { IConsoleManager::Get().UnregisterConsoleObject(C); }
	}
	GCommands.Empty();
}

}
