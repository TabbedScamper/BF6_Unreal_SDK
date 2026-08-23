#pragma once

// ---------------------------------------------------------------------------
// Shared internals of the BF6UnrealSDK module.
//
// BF6UnrealSDK.cpp began as one translation unit, which meant every helper
// could be `static` and every ordering problem was solved by moving code up
// the file. Splitting it means the pieces that genuinely ARE shared have to be
// declared somewhere, and this is that somewhere.
//
// This is NOT the tool's public surface. Widgets talk to the engine through
// BF6BuildMode.h and nothing else; add-ons come in through
// Public/BF6SDKExtension.h. Anything declared here is module-internal
// plumbing, and it should stay short. If it grows, that is a sign a seam is
// in the wrong place.
// ---------------------------------------------------------------------------

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

DECLARE_LOG_CATEGORY_EXTERN(LogBF6, Log, All);

// Where the plugin is installed. Set once at startup.
extern FString g_pluginDir;

// A brief on-screen toast, top-right of the editor.
void Notify(const FString& Msg);

// ---- SDK data import (BF6SdkImport.cpp) -----------------------------------
//
// The tool's data packs are generated from the user's own unzipped Portal SDK
// download. Split out because it is a one-time setup pipeline rather than part
// of the running tool, and because it touched almost nothing else: only
// g_pluginDir, Notify and the log category cross the seam.

// Where the generated packs live.
FString BF6_DataDir();
int32   BF6_CountFiles(const FString& Dir, const TCHAR* Pattern);
FString BF6_ReadSdkVersion(const FString& JsonPath);
FString BF6_FindSdkGodot(const FString& SdkRoot);

// Parse levels/MP_*.tscn into <map>.base.json, and regenerate them when the
// format the tool expects has moved on.
void BF6_ExtractBaseSetups(const FString& SdkRoot);
void BF6_EnsureBaseSetupFormat();

// The headless-Godot conversion run: generated scripts, sharded workers, and
// the ticker that watches them.
struct FBF6Import
{
	enum class EPhase { Idle, Objects, Maps, Done, Failed };
	EPhase  Phase = EPhase::Idle;
	FString SdkRoot, GodotExe;
	// PARALLEL workers: each converts a deterministic shard of the work
	// (name.hash() % shards), so N headless Godots run at once - the
	// conversion is CPU-bound per process and one worker took ~4x as long
	TArray<FProcHandle> Procs;
	TArray<void*> PipeReads;
	TArray<void*> PipeWrites;
	int32   ObjTotal = 0, MapTotal = 0, LastCount = 0, Stagnant = 0;
	// StartCount = converted files when the phase began, so a phase that adds
	// NOTHING can be told apart from one that legitimately skipped existing work
	int32   StartCount = 0, ObjDone = 0;
	FString Status;
	float   Frac = 0.f;
	FTSTicker::FDelegateHandle Tick;
};
// worker counts: objects are many small loads (4 wide); map meshes are huge
// whole-map scenes, so 2 wide keeps peak memory sane
constexpr int32 kBF6ObjShards = 4;
constexpr int32 kBF6MapShards = 2;
extern FBF6Import g_imp;

FString BF6_ObjectScript(const FString& OutDir, int32 Shard, int32 Shards);
FString BF6_MapScript(const FString& OutDir, int32 Shard, int32 Shards);
FString BF6_ImportLogPath();
void    BF6_ImportLog(const FString& Line);
void    BF6_ImportFail(const FString& Why);
bool    BF6_LaunchGodotWorkers(bool bObjects);
void    BF6_ImportTickPhase();

namespace BF6Api
{
	// Records the SDK's own file list so the NEXT update can be diffed against
	// it. Lives with the version-history code, called from the import's tail.
	void BF6_SnapshotSdkHistory(const FString& SdkRoot);
}
