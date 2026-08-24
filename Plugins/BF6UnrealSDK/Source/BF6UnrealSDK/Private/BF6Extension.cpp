// ============================================================================
// The add-on seam, implemented.
//
// Deliberately thin: a registry, two delegates, and forwarding to the API the
// tool already has. Everything an add-on can reach is listed in
// Public/BF6SDKExtension.h and nothing else is reachable, which is what makes
// "an add-on cannot break the tool" true rather than a promise.
// ============================================================================

#include "BF6SDKExtension.h"
#include "BF6BuildMode.h"
#include "BF6ExtensionInternal.h"

#include "Algo/BinarySearch.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

namespace
{
	// Sorted by Order at registration, so the ring never has to sort.
	TArray<BF6Ext::FPieEntry> GEntries;

	BF6Ext::FBF6MapOpened  GMapOpened;
	BF6Ext::FBF6MapClosing GMapClosing;

	const FName GAddonTag("BF6Addon");

	FString OwnerTag(const FString& AddonName)
	{
		return FString(TEXT("addon:")) + AddonName;
	}
}

namespace BF6Ext
{
	int32 ApiVersion() { return 2; }   // 2: OpenPieSubRing

	void RegisterPieEntry(const FPieEntry& Entry)
	{
		if (Entry.Id.IsNone() || Entry.Label.IsEmpty()) return;
		// Re-registering the same id replaces it: a hot-reloaded add-on module
		// would otherwise stack a second pill every time it loads.
		UnregisterPieEntry(Entry.Id);
		const int32 At = Algo::UpperBoundBy(GEntries, Entry.Order, [](const FPieEntry& E){ return E.Order; });
		GEntries.Insert(Entry, At);
	}

	void UnregisterPieEntry(FName Id)
	{
		GEntries.RemoveAll([Id](const FPieEntry& E){ return E.Id == Id; });
	}

	FBF6MapOpened&  OnMapOpened()  { return GMapOpened; }
	FBF6MapClosing& OnMapClosing() { return GMapClosing; }

	FString CurrentLevel() { return BF6Api::CurrentLevel(); }
	FString CurrentSave()  { return BF6Api::CurrentSave(); }
	bool    IsEditing()    { return BF6Api::IsEditing(); }
	bool    IsWalking()    { return BF6Api::IsWalking(); }

	FString GameInstallDir() { return BF6Api::GameInstallDir(); }
	FString SdkRoot()        { return BF6Api::StoredSdkRoot(); }

	FString ToolPluginDir()
	{
		TSharedPtr<IPlugin> P = IPluginManager::Get().FindPlugin(TEXT("BF6UnrealSDK"));
		return P.IsValid() ? P->GetBaseDir() : FString();
	}

	FString ToolSavedDir()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"));
	}

	FName AddonTag() { return GAddonTag; }

	void MarkAddonActor(AActor* A, const FString& AddonName)
	{
		if (!A) return;
		if (!A->Tags.Contains(GAddonTag)) A->Tags.Add(GAddonTag);
		const FName Owner(*OwnerTag(AddonName));
		if (!A->Tags.Contains(Owner)) A->Tags.Add(Owner);
	}

	int32 ClearAddonActors(const FString& AddonName)
	{
		if (!GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return 0;
		const FName Owner(*OwnerTag(AddonName));
		TArray<AActor*> Doomed;
		for (TActorIterator<AActor> It(W); It; ++It)
			if (It->Tags.Contains(Owner)) Doomed.Add(*It);
		for (AActor* A : Doomed) A->Destroy();
		return Doomed.Num();
	}

	void ShowPopup(TSharedRef<SWidget> Content, FVector2D ScreenPos)
	{
		BF6Api::PushAddonPopup(Content, ScreenPos);
	}

	// The one live sub-ring. One is enough: a wheel shows one page, and the
	// entries are copied so the add-on's array can be a temporary.
	static TArray<FPieSubEntry> GSubEntries;

	void OpenPieSubRing(const TArray<FPieSubEntry>& Entries, FVector2D ScreenCenter)
	{
		GSubEntries = Entries;
		BF6Api::OpenAddonSubRing(ScreenCenter);
	}

	void Notify(const FString& Message) { BF6Api::Toast(Message); }

	int32 SetLowPolyMapHidden(bool bHidden)
	{
		return BF6Api::SetContextHidden(bHidden);
	}

	bool IsLowPolyMapHidden()
	{
		return BF6Api::IsContextHidden();
	}

	void Selection(TArray<AActor*>& Out)
	{
		BF6Api::SelectionTargets(Out);
		Out.RemoveAll([](AActor* A){ return !A || A->Tags.Contains(GAddonTag); });
	}

	bool WorldAheadOfCamera(FVector& OutWorld)
	{
		return BF6Api::WorldFromViewportCenter(OutWorld);
	}
}

// ---- what the tool calls, on its side of the seam ---------------------------

namespace BF6ExtInternal
{
	const TArray<BF6Ext::FPieEntry>& PieEntries() { return GEntries; }

	const TArray<BF6Ext::FPieSubEntry>& AddonSubEntries() { return BF6Ext::GSubEntries; }

	// Called from the pie's dispatch after its own cases. Returns true when an
	// add-on owned that label, so the tool stops looking. The comparison ignores
	// case because the ring uppercases what it draws, and an add-on is entitled
	// to register "High Poly" and get the pill back.
	bool DispatchPie(const FString& Label, const FVector2D& Center)
	{
		for (const BF6Ext::FPieEntry& E : GEntries)
			if (E.Label.Equals(Label, ESearchCase::IgnoreCase))
			{
				if (E.OnPick) E.OnPick(Center);
				return true;
			}
		return false;
	}

	void BroadcastMapOpened(const FString& Level, const FString& Save) { GMapOpened.Broadcast(Level, Save); }
	void BroadcastMapClosing(const FString& Level)                     { GMapClosing.Broadcast(Level); }
}
