#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

class SWidget;

DECLARE_LOG_CATEGORY_EXTERN(LogBF6GameLog, Log, All);

// ============================================================================
// BF6 game log: what the mod printed while it was actually running.
//
// Battlefield writes a Portal mod's console.log output to a plain text file on
// PC. It is the only view of a running mod that is not the mod's own on-screen
// UI, so after a localhost playtest it is the thing you want, and it is
// currently four folders deep in a temp directory nobody would guess.
//
// FINDING IT IS THE WHOLE TRICK, AND IT IS NOT WHAT IT LOOKS LIKE.
//
// The folder is "Battlefield(tm) 6" under %TEMP%, except that the name on disk
// is literally the bytes  Battlefield  +  C3 A2 C2 84 C2 A2  +  ` 6`. That is
// UTF-8 for the trademark sign, written by something that then had its output
// read back as Windows-1252 - so the real directory name contains the mojibake
// permanently. Constructing the path from a U+2122 character finds nothing.
//
// So the folder is located by pattern, never by literal, and the pattern is
// deliberately loose: EA can spell the name differently in another build, or
// fix the encoding, and this should keep working when they do.
// ============================================================================
namespace BF6GameLog
{
	struct FEntry
	{
		FString TimestampUtc;   // "2026-09-04 07:01:26", empty when unparsed
		FString Kind;           // "console.log", "error", "system"
		FString Text;
	};

	struct FResult
	{
		bool bFound = false;
		FString Path;
		FString Why;            // when not found, what was looked for
		FDateTime Written;
		int64 Bytes = 0;
		TArray<FEntry> Entries;
	};

	// The live localhost log, wherever Battlefield put it this time.
	FResult ReadLocal(int32 MaxEntries = 2000);

	// A log the user exported from an online host, or any file they point at.
	FResult ReadFile(const FString& Path, int32 MaxEntries = 2000);

	// Where it looked, for a status line. Empty when the folder is not there.
	FString LocateFolder();

	// ---- watching it live -------------------------------------------------
	//
	// Polling, not a filesystem notification, and reading only the bytes added
	// since last time. Two reasons. The game holds the file open while it runs,
	// so the reader must not take an exclusive handle or it will either fail or
	// interfere with the writer. And a change notification would still leave us
	// re-reading a file that grows to megabytes over a long match.
	//
	// A file that suddenly gets SHORTER is a new session, not corruption:
	// Battlefield truncates it when a mod starts. That is reported rather than
	// treated as an error, because "your last run ended, this is a fresh one"
	// is exactly what somebody watching wants to be told.

	// Called on the game thread with whatever arrived since the last tick.
	// bNewSession is true on the first batch after the file was truncated.
	DECLARE_DELEGATE_TwoParams(FOnLines, const TArray<FEntry>&, bool /*bNewSession*/);

	// Several views can watch at once: the LOG section, the Blocks editor and
	// Script all offer a Watch button, and a user who turns two of them on
	// means both, not the most recent one. Each caller keeps its own handle and
	// removes only itself; the single underlying poll stops when the last one
	// goes. Every listener receives one copy of every line.
	FGuid AddWatcher(FOnLines OnLines, float IntervalSeconds = 1.0f);
	void RemoveWatcher(const FGuid& Handle);

	// Ends the poll for everyone. For editor shutdown, not for a panel closing.
	void StopWatch();
	bool IsWatching();
	int32 WatcherCount();

	void Register();
	void Unregister();

	// ---- the LOG section on the build screen's top bar ---------------------
	//
	// Its own sheet, between SCRIPT and UI, because after a playtest "what did
	// my mod print" is a question in its own right rather than a corner of the
	// block editor. The same pull and watch are still available inside Blocks
	// and Script, where you are when you think of it.
	//
	// Deliberately small: view and pull. It is a reading surface, not a second
	// place to author anything.
	TSharedRef<SWidget> Widget();
	void ReleaseWidget();
}
