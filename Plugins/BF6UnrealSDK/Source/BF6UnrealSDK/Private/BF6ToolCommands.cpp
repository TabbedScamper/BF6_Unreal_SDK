// BF6ToolCommands - console entry points for things the tool otherwise offers
// only through its HUD, so a scripted or headless session can drive them.
// None of these read the game, so they belong to the tool, not to an add-on.
#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "BF6SDKExtension.h"
#include "GameFramework/Actor.h"

DEFINE_LOG_CATEGORY_STATIC(LogBF6Tool, Log, All);

static FAutoConsoleCommand GBF6CreateCustomCmd(
	TEXT("BF6.CreateCustom"),
	TEXT("<name>: create a custom map on the open level and start editing it (the HUD's Create)."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogBF6Tool, Display, TEXT("usage: BF6.CreateCustom <name>"));
			return;
		}
		const FString Name = FString::Join(Args, TEXT(" "));
		BF6Ext::CreateCustomMap(Name);
		UE_LOG(LogBF6Tool, Display, TEXT("custom map requested: %s (level %s, editing=%s)"),
			*Name, *BF6Ext::CurrentLevel(), BF6Ext::IsEditing() ? TEXT("yes") : TEXT("no"));
	}));

static FAutoConsoleCommand GBF6OpenSaveCmd(
	TEXT("BF6.OpenSave"),
	TEXT("<level> <save>: open a level on one of its saves through the tool's own route."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogBF6Tool, Display, TEXT("usage: BF6.OpenSave <level> <save>"));
			return;
		}
		// SAVE NAMES HAVE SPACES IN THEM. Almost every real one does - "UNDEAD
		// GROUND ZERO GUNMASTER V1.4" - and the console splits on spaces, so
		// taking Args[1] alone meant this command could open exactly the saves
		// nobody has: it silently asked for "UNDEAD" and opened the level with
		// no save at all. The rest of the line is the name.
		TArray<FString> Rest(Args);
		Rest.RemoveAt(0);
		const FString Save = FString::Join(Rest, TEXT(" ")).TrimQuotes();
		BF6Ext::OpenMap(Args[0], Save);
	}));

static FAutoConsoleCommand GBF6TypesCmd(
	TEXT("BF6.Types"),
	TEXT("[filter]: list the placeable types the library carries (substring filter, case-insensitive)."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		TArray<FString> Types;
		BF6Ext::PlaceableTypes(Types);
		const FString Filter = Args.Num() ? Args[0] : FString();
		int32 Shown = 0;
		for (const FString& T : Types)
		{
			if (!Filter.IsEmpty() && !T.Contains(Filter, ESearchCase::IgnoreCase)) continue;
			UE_LOG(LogBF6Tool, Display, TEXT("type: %s"), *T);
			if (++Shown >= 60) { UE_LOG(LogBF6Tool, Display, TEXT("... (first 60 shown)")); break; }
		}
		UE_LOG(LogBF6Tool, Display, TEXT("%d of %d placeable type(s) match '%s'"), Shown, Types.Num(), *Filter);
	}));

static FAutoConsoleCommand GBF6PlaceCmd(
	TEXT("BF6.Place"),
	TEXT("<type> [x y z in metres]: place one object (needs a custom map open); default is the spot ahead of the camera."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
#if defined(BF6EXT_HAS_PLACEMENT)
		if (Args.Num() < 1)
		{
			UE_LOG(LogBF6Tool, Display, TEXT("usage: BF6.Place <type> [x y z]"));
			return;
		}
		FVector Where = FVector::ZeroVector;
		if (Args.Num() >= 4)
		{
			Where = FVector(FCString::Atod(*Args[1]), FCString::Atod(*Args[2]), FCString::Atod(*Args[3])) * 100.0;
		}
		else if (!BF6Ext::WorldAheadOfCamera(Where))
		{
			UE_LOG(LogBF6Tool, Display, TEXT("no build viewport to aim with; pass x y z"));
			return;
		}
		AActor* A = BF6Ext::PlaceObject(Args[0], FTransform(Where));
		UE_LOG(LogBF6Tool, Display, TEXT("place %s at (%.1f %.1f %.1f) m -> %s"),
			*Args[0], Where.X * 0.01, Where.Y * 0.01, Where.Z * 0.01,
			A ? *A->GetActorLabel() : TEXT("refused (read-only base map, or unknown type)"));
#else
		UE_LOG(LogBF6Tool, Display, TEXT("this tool build has no placement seam"));
#endif
	}));
