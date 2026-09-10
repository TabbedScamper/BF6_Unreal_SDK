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
#include "BF6Internal.h"   // LogBF6
#include "BF6UiSound.h"

#include "Algo/BinarySearch.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "Widgets/SWindow.h"
#include "Widgets/Layout/SBorder.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/CoreStyle.h"
#include "Misc/ConfigCacheIni.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/AutomationTest.h"
#include "ScopedTransaction.h"

namespace
{
	// Sorted by Order at registration, so the ring never has to sort.
	TArray<BF6Ext::FPieEntry> GEntries;
	TMap<FName,TFunction<bool()>> GObjectEditors;
	TMap<FName,BF6Ext::FEquipmentPreviewProvider> GEquipmentPreviewProviders;

	BF6Ext::FBF6MapOpened  GMapOpened;
	BF6Ext::FBF6MapClosing GMapClosing;

	const FName GAddonTag("BF6Addon");

	// Thumbnail providers, in registration order; the first brush wins.
	struct FThumbProvider
	{
		FName Id;
		TFunction<const FSlateBrush*(const FString&)> Fn;
	};
	TArray<FThumbProvider> GThumbProviders;

	// What each provider calls the picture it drew, keyed by the same Id.
	struct FThumbDetail
	{
		FName Id;
		TFunction<FString(const FString&)> Fn;
	};
	TArray<FThumbDetail> GThumbDetails;

	FString OwnerTag(const FString& AddonName)
	{
		return FString(TEXT("addon:")) + AddonName;
	}
}

namespace BF6Ext
{
	int32 ApiVersion() { return 11; }  // 11: asynchronous equipment artwork
	void RegisterEquipmentPreviewProvider(FName Id, FEquipmentPreviewProvider Provider)
	{
		check(IsInGameThread());
		if (!Id.IsNone() && Provider) GEquipmentPreviewProviders.Add(Id, MoveTemp(Provider));
	}
	void UnregisterEquipmentPreviewProvider(FName Id) { check(IsInGameThread()); GEquipmentPreviewProviders.Remove(Id); }
	void RequestEquipmentPreview(const FString& RequestJson, FEquipmentPreviewReply Reply)
	{
		check(IsInGameThread());
		if (!Reply) return;
		for (auto& Pair : GEquipmentPreviewProviders) { Pair.Value(RequestJson, MoveTemp(Reply)); return; }
		Reply(TEXT("{\"error\":\"Install and enable the High Poly add-on to preview game equipment artwork.\"}"));
	}
	void RegisterObjectEditor(FName Id, TFunction<bool()> Open) { if(!Id.IsNone() && Open) GObjectEditors.Add(Id,MoveTemp(Open)); }
	void UnregisterObjectEditor(FName Id) { GObjectEditors.Remove(Id); }
	bool OpenSelectedObjectEditor()
	{
		if(!IsEditing()) return false;
		for(const auto& Entry:GObjectEditors) if(Entry.Value()) return true;
		return false;
	}

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

	void RegisterThumbnailProvider(FName Id, TFunction<const FSlateBrush*(const FString&)> Provider)
	{
		if (Id.IsNone() || !Provider) return;
		UnregisterThumbnailProvider(Id);   // a reloaded add-on replaces itself
		GThumbProviders.Add({ Id, MoveTemp(Provider) });
	}

	void UnregisterThumbnailProvider(FName Id)
	{
		GThumbProviders.RemoveAll([Id](const FThumbProvider& P){ return P.Id == Id; });
		UnregisterThumbnailDetail(Id);   // the label belongs to the picture
	}

	void RegisterThumbnailDetail(FName Id, TFunction<FString(const FString&)> Detail)
	{
		if (Id.IsNone() || !Detail) return;
		UnregisterThumbnailDetail(Id);
		GThumbDetails.Add({ Id, MoveTemp(Detail) });
	}

	void UnregisterThumbnailDetail(FName Id)
	{
		GThumbDetails.RemoveAll([Id](const FThumbDetail& D){ return D.Id == Id; });
	}

	void PlaceableTypes(TArray<FString>& Out)
	{
		Out.Reset();
		// The open map's shelf when there is one, else everything: the same
		// rows the library panel shows, uncapped.
		const bool bAllLevels = BF6Api::CurrentLevel().IsEmpty();
		for (const BF6Api::FPlaceableInfo& P : BF6Api::LibraryPlaceables(FString(), FString(), bAllLevels, 1000000))
			if (!P.Type.IsEmpty()) Out.Add(P.Type);
	}

	FBF6MapOpened&  OnMapOpened()  { return GMapOpened; }
	FBF6MapClosing& OnMapClosing() { return GMapClosing; }

	FString CurrentLevel() { return BF6Api::CurrentLevel(); }
	FString CurrentSave()  { return BF6Api::CurrentSave(); }
	bool    IsEditing()    { return BF6Api::IsEditing(); }
	bool    IsWalking()    { return BF6Api::IsWalking(); }
	void    CreateCustomMap(const FString& Name)
	{
		BF6Api::CreateCustom(Name);
	}

	void    OpenMap(const FString& Level, const FString& SaveName)
	{
		// EnterBuild is the SDK's complete non-interactive open path: it reads
		// the base/save and switches the viewport root from CHOOSE MAPS to the
		// build overlay.  Calling OpenMapWorld alone left a correctly built
		// level hidden behind the startup selector, which made extension-driven
		// benches look as though the map had never opened.
		BF6Api::EnterBuild(Level, SaveName);
	}

	void    ShowBuildOverlay()
	{
		BF6Api::ShowBuildOverlay();
	}

	int32   MapImageState()
	{
		return BF6Api::MapDecalState();
	}

	void    SetMapImageVisible(bool bVisible)
	{
		const int32 State = BF6Api::MapDecalState();
		if ((bVisible && (State == 0 || State == 1)) ||
			(!bVisible && State == 3))
		{
			BF6Api::ToggleMapDecal();
		}
	}

	int32 FixSafeValidationIssues(bool bSave)
	{
		if (!BF6Api::IsEditing()) return 0;
		const TArray<BF6Api::FLintItem> Items = BF6Api::RunLint();
		TSet<AActor*> FixedActors;
		for (const BF6Api::FLintItem& Item : Items)
		{
			AActor* Actor = Item.Actor.Get();
			if (!Item.bWindingFix || !Actor || FixedActors.Contains(Actor)) continue;
			if (BF6Api::ReverseVolumeWinding(Actor)) FixedActors.Add(Actor);
		}
		if (FixedActors.Num() > 0)
		{
			BF6Api::RunLint();
			if (bSave) BF6Api::SaveCurrent(true);
		}
		return FixedActors.Num();
	}

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

// ---------------------------------------------------------------------------
// ADD-ON WINDOWS: floating, always on top, and dockable to a screen edge.
//
// The add-on panel used to be a popup. A popup is the wrong container for a
// panel somebody works in - it cannot be moved, it does not stay above the
// viewport, and it closes on the first click outside it, which is why controls
// below the fold were effectively unreachable.
//
// Docking here means SNAPPING TO A SCREEN EDGE, not joining the editor's tab
// layout. Dragged within kSnapPx of an edge the window takes that edge, filling
// it and taking a fixed width or height; dragged away it becomes free again.
// That is what "drag it to the edge to dock it" means to somebody using it, and
// it does not require the panel to be a tab spawner.
// ---------------------------------------------------------------------------
namespace
{
	// How close to an edge counts as docking there.
	constexpr float kSnapPx = 24.f;
	// The docked bar's thickness. Wide enough for the panel's own rows.
	constexpr float kDockThickness = 420.f;

	enum class EDockEdge : uint8 { None, Left, Right, Top, Bottom };

	struct FAddonWindow
	{
		TWeakPtr<SWindow>   Window;
		TSharedPtr<SBorder> Host;      // contents are swapped in here
		EDockEdge           Edge = EDockEdge::None;
		FString             Key;
	};
	TMap<FString, FAddonWindow> GAddonWindows;
	// ReshapeWindow emits OnWindowMoved before its cached size is updated.
	// Keep the guard outside the map: a nested Slate callback can mutate it.
	TSet<const SWindow*> GDockingWindows;

	FString WindowIni(const FString& Key, const TCHAR* Field)
	{
		return FString::Printf(TEXT("AddonWindow_%s_%s"), *Key, Field);
	}

	// The work area of the display the point sits on, so docking follows the
	// monitor the window was dragged to rather than always the primary one.
	FSlateRect WorkAreaFor(const FVector2D& Pos)
	{
		FDisplayMetrics M;
		FSlateApplication::Get().GetCachedDisplayMetrics(M);
		for (const FMonitorInfo& Mon : M.MonitorInfo)
		{
			const FSlateRect R(Mon.WorkArea.Left, Mon.WorkArea.Top,
				Mon.WorkArea.Right, Mon.WorkArea.Bottom);
			if (Pos.X >= R.Left && Pos.X <= R.Right && Pos.Y >= R.Top && Pos.Y <= R.Bottom)
			{
				return R;
			}
		}
		return FSlateRect(M.PrimaryDisplayWorkAreaRect.Left, M.PrimaryDisplayWorkAreaRect.Top,
			M.PrimaryDisplayWorkAreaRect.Right, M.PrimaryDisplayWorkAreaRect.Bottom);
	}

	EDockEdge EdgeForPosition(const TSharedRef<SWindow>& W)
	{
		const FVector2D Pos = W->GetPositionInScreen();
		const FVector2D Size = W->GetSizeInScreen();
		const FSlateRect Area = WorkAreaFor(Pos + Size * 0.5f);
		if (FMath::Abs(Pos.X - Area.Left) <= kSnapPx)                 return EDockEdge::Left;
		if (FMath::Abs((Pos.X + Size.X) - Area.Right) <= kSnapPx)     return EDockEdge::Right;
		if (FMath::Abs(Pos.Y - Area.Top) <= kSnapPx)                  return EDockEdge::Top;
		if (FMath::Abs((Pos.Y + Size.Y) - Area.Bottom) <= kSnapPx)    return EDockEdge::Bottom;
		return EDockEdge::None;
	}

	void ApplyDock(const TSharedRef<SWindow>& W, EDockEdge Edge)
	{
		if (Edge == EDockEdge::None || GDockingWindows.Contains(&W.Get())) { return; }
		GDockingWindows.Add(&W.Get());
		ON_SCOPE_EXIT { GDockingWindows.Remove(&W.Get()); };
		const FSlateRect Area = WorkAreaFor(W->GetPositionInScreen() + W->GetSizeInScreen() * 0.5f);
		const float AW = Area.Right - Area.Left;
		const float AH = Area.Bottom - Area.Top;
		switch (Edge)
		{
		case EDockEdge::Left:
			W->ReshapeWindow(FVector2D(Area.Left, Area.Top), FVector2D(kDockThickness, AH));
			break;
		case EDockEdge::Right:
			W->ReshapeWindow(FVector2D(Area.Right - kDockThickness, Area.Top),
				FVector2D(kDockThickness, AH));
			break;
		case EDockEdge::Top:
			W->ReshapeWindow(FVector2D(Area.Left, Area.Top), FVector2D(AW, kDockThickness));
			break;
		case EDockEdge::Bottom:
			W->ReshapeWindow(FVector2D(Area.Left, Area.Bottom - kDockThickness),
				FVector2D(AW, kDockThickness));
			break;
		default: break;
		}
	}

	void SaveWindowPlacement(const FString& Key, const TSharedRef<SWindow>& W, EDockEdge Edge)
	{
		if (!GConfig) { return; }
		const FVector2D P = W->GetPositionInScreen();
		const FVector2D S = W->GetSizeInScreen();
		GConfig->SetInt(TEXT("BF6UnrealSDK"), *WindowIni(Key, TEXT("Edge")), (int32)Edge, GEditorPerProjectIni);
		GConfig->SetFloat(TEXT("BF6UnrealSDK"), *WindowIni(Key, TEXT("X")), P.X, GEditorPerProjectIni);
		GConfig->SetFloat(TEXT("BF6UnrealSDK"), *WindowIni(Key, TEXT("Y")), P.Y, GEditorPerProjectIni);
		GConfig->SetFloat(TEXT("BF6UnrealSDK"), *WindowIni(Key, TEXT("W")), S.X, GEditorPerProjectIni);
		GConfig->SetFloat(TEXT("BF6UnrealSDK"), *WindowIni(Key, TEXT("H")), S.Y, GEditorPerProjectIni);
	}
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBF6AddonDockTest, "BF6.Editor.AddonDockReentrancy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBF6AddonDockTest::RunTest(const FString& Parameters)
{
	// A real, hidden native window exercises the synchronous moved callback
	// inside Slate's ReshapeWindow, including its not-yet-updated cached size.
	TSharedRef<SWindow> W = SNew(SWindow).Title(FText::FromString(TEXT("Dock regression")))
		.ClientSize(FVector2D(640, 480)).SupportsMaximize(false).SupportsMinimize(false);
	FSlateApplication::Get().AddWindow(W, false);
	ON_SCOPE_EXIT
	{
		W->SetOnWindowMoved(FOnWindowMoved());
		FSlateApplication::Get().RequestDestroyWindow(W);
	};
	int32 Depth = 0, MaxDepth = 0, Calls = 0;
	EDockEdge Edge = EDockEdge::Top;
	W->SetOnWindowMoved(FOnWindowMoved::CreateLambda([&](const TSharedRef<SWindow>& Moved)
	{
		++Calls; ++Depth; MaxDepth = FMath::Max(MaxDepth, Depth);
		// Bound the regression itself so a future missing guard fails the test
		// rather than exhausting the editor's stack.
		if (Depth < 5) ApplyDock(Moved, Edge);
		--Depth;
	}));
	for (EDockEdge Target : { EDockEdge::Top, EDockEdge::Bottom, EDockEdge::Left, EDockEdge::Right })
	{
		Edge = Target;
		ApplyDock(W, Edge);
		TestFalse(TEXT("Dock guard is released after each reshape"), GDockingWindows.Contains(&W.Get()));
	}
	TestTrue(TEXT("Native reshape emitted move callbacks"), Calls > 0);
	TestEqual(TEXT("Programmatic docking never recursively reshapes"), MaxDepth, 1);
	return true;
}
#endif

void ShowAddonWindow(const FString& Key, const FString& Title,
	TSharedRef<SWidget> Content, FVector2D DefaultSize)
{
	// Already up: bring it forward with the new contents rather than opening a
	// second copy, which is what repeated presses of a toolbar button do.
	if (FAddonWindow* Existing = GAddonWindows.Find(Key))
	{
		if (TSharedPtr<SWindow> W = Existing->Window.Pin())
		{
			if (Existing->Host.IsValid()) { Existing->Host->SetContent(Content); }
			W->BringToFront();
			return;
		}
		GAddonWindows.Remove(Key);
	}

	FVector2D Size = DefaultSize;
	FVector2D Pos(-1.f, -1.f);
	int32 EdgeInt = 0;
	if (GConfig)
	{
		float V = 0.f;
		if (GConfig->GetFloat(TEXT("BF6UnrealSDK"), *WindowIni(Key, TEXT("W")), V, GEditorPerProjectIni) && V > 64.f) Size.X = V;
		if (GConfig->GetFloat(TEXT("BF6UnrealSDK"), *WindowIni(Key, TEXT("H")), V, GEditorPerProjectIni) && V > 64.f) Size.Y = V;
		float X = 0.f, Y = 0.f;
		if (GConfig->GetFloat(TEXT("BF6UnrealSDK"), *WindowIni(Key, TEXT("X")), X, GEditorPerProjectIni)
			&& GConfig->GetFloat(TEXT("BF6UnrealSDK"), *WindowIni(Key, TEXT("Y")), Y, GEditorPerProjectIni))
		{
			// A NEGATIVE X IS A REAL PLACE. A monitor to the left of the
			// primary one has negative coordinates, so treating "< 0" as
			// invalid threw away a perfectly good position and dragged the
			// panel back onto the main screen every launch.
			//
			// What actually has to be checked is whether the saved place still
			// EXISTS: a monitor that has been unplugged leaves a window
			// somewhere nobody can reach. So it is kept when it still lands on
			// a display, and dropped when it does not.
			const FVector2D Want(X, Y);
			FDisplayMetrics M;
			FSlateApplication::Get().GetCachedDisplayMetrics(M);
			bool bOnScreen = false;
			for (const FMonitorInfo& Mon : M.MonitorInfo)
			{
				// A generous margin, so a window whose title bar is on the
				// display still counts as reachable.
				if (Want.X >= Mon.WorkArea.Left - 32 && Want.X <= Mon.WorkArea.Right - 64
					&& Want.Y >= Mon.WorkArea.Top - 8 && Want.Y <= Mon.WorkArea.Bottom - 32)
				{
					bOnScreen = true;
					break;
				}
			}
			if (bOnScreen) { Pos = Want; }
			else
			{
				UE_LOG(LogBF6, Display,
					TEXT("The %s panel was last at (%.0f, %.0f), which is not on any display now, ")
					TEXT("so it opens on the main one."), *Key, X, Y);
			}
		}
		GConfig->GetInt(TEXT("BF6UnrealSDK"), *WindowIni(Key, TEXT("Edge")), EdgeInt, GEditorPerProjectIni);
	}

	TSharedPtr<SBorder> Host;
	TSharedRef<SWindow> W = SNew(SWindow)
		.Title(FText::FromString(Title))
		.ClientSize(Size)
		.SizingRule(ESizingRule::UserSized)
		// ON TOP, which is the whole point: the panel drives the viewport, so
		// it has to stay visible while the viewport has focus.
		.IsTopmostWindow(true)
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		.CreateTitleBar(true)
		.AdjustInitialSizeAndPositionForDPIScale(true)
		[
			SAssignNew(Host, SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("NoBorder"))
			.Padding(FMargin(0.f))
			[
				Content
			]
		];

	if (Pos.X >= 0.f) { W->MoveWindowTo(Pos); }

	FAddonWindow Rec;
	Rec.Host = Host;
	Rec.Key = Key;
	Rec.Edge = (EDockEdge)FMath::Clamp(EdgeInt, 0, 4);

	// EDGE SNAPPING. Slate tells us when the user has finished moving the
	// window; that is the moment to decide whether it landed on an edge. Doing
	// it while the drag is live would fight the pointer.
	W->SetOnWindowMoved(FOnWindowMoved::CreateLambda(
		[Key](const TSharedRef<SWindow>& Moved)
		{
			if (GDockingWindows.Contains(&Moved.Get())) { return; }
			FAddonWindow* Found = GAddonWindows.Find(Key);
			if (!Found) { return; }
			const EDockEdge Now = EdgeForPosition(Moved);
			if (Now != EDockEdge::None)
			{
				Found->Edge = Now;
				ApplyDock(Moved, Now);
			}
			else
			{
				// Dragged off the edge: it is a free window again.
				Found->Edge = EDockEdge::None;
			}
			SaveWindowPlacement(Key, Moved, Found->Edge);
		}));

	W->GetOnWindowClosedEvent().AddLambda([Key](const TSharedRef<SWindow>& Closed)
	{
		if (FAddonWindow* Found = GAddonWindows.Find(Key))
		{
			SaveWindowPlacement(Key, Closed, Found->Edge);
		}
		GAddonWindows.Remove(Key);
	});

	FSlateApplication::Get().AddWindow(W);
	Rec.Window = W;
	GAddonWindows.Add(Key, Rec);

	// A window that was docked when it was last closed comes back docked.
	if (Rec.Edge != EDockEdge::None) { ApplyDock(W, Rec.Edge); }
}

// Every add-on window, closed. Called from module shutdown: a floating window
// outlives the tab it came from, and one still holding an add-on's widgets
// while that add-on is torn down is a crash waiting for the next repaint.
void CloseAllAddonWindows()
{
	TArray<FString> Keys;
	GAddonWindows.GetKeys(Keys);
	for (const FString& K : Keys)
	{
		if (FAddonWindow* Found = GAddonWindows.Find(K))
		{
			if (TSharedPtr<SWindow> W = Found->Window.Pin())
			{
				SaveWindowPlacement(K, W.ToSharedRef(), Found->Edge);
				W->RequestDestroyWindow();
			}
		}
	}
	GAddonWindows.Empty();
}

void CloseAddonWindow(const FString& Key)
{
	if (FAddonWindow* Found = GAddonWindows.Find(Key))
	{
		if (TSharedPtr<SWindow> W = Found->Window.Pin()) { W->RequestDestroyWindow(); }
		GAddonWindows.Remove(Key);
	}
}

bool IsAddonWindowOpen(const FString& Key)
{
	const FAddonWindow* Found = GAddonWindows.Find(Key);
	return Found && Found->Window.IsValid();
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

	// ---- placing the user's content -----------------------------------------

	AActor* PlaceObject(const FString& Type, const FTransform& WorldXf)
	{
		if (!BF6Api::IsEditing()) return nullptr;
		AActor* A = BF6Api::PlaceType(Type, WorldXf.GetLocation());
		if (A && !BF6Api::IsVolumeActor(A)) A->SetActorTransform(WorldXf);
		return A;
	}

	AActor* PlaceNode(const FString& Label, const FVector& WorldPos)
	{
		if (!BF6Api::IsEditing()) return nullptr;
		AActor* A = BF6Api::PlaceType(TEXT("Node3D"), WorldPos);
		if (!A || Label.IsEmpty()) return A;
		BF6Api::SetActorPrettyLabel(A, Label);
		// PlaceType keyed the node on the label it was born with; re-key it on
		// the one it has now so its children's tree tags point at a real name.
		FString Key = A->GetActorLabel(); Key.RemoveFromStart(TEXT("BF6_"));
		for (int32 i = A->Tags.Num() - 1; i >= 0; i--)
			if (A->Tags[i].ToString().StartsWith(TEXT("gpath:"))) A->Tags.RemoveAt(i);
		A->Tags.Add(FName(*(FString(TEXT("gpath:")) + Key)));
		return A;
	}

	bool HasPlaceable(const FString& Type)
	{
		if (Type.IsEmpty()) return false;
		if (BF6Api::PropsForType(Type).Num() > 0) return true;
		for (const BF6Api::FPlaceableInfo& P : BF6Api::LibraryPlaceables(FString(), Type, true, 200))
			if (P.Type.Equals(Type, ESearchCase::IgnoreCase)) return true;
		return false;
	}

	void ObjectPropDefs(const FString& Type, TArray<TPair<FString, FString>>& Out)
	{
		Out.Reset();
		for (const BF6Api::FPropDef& D : BF6Api::PropsForType(Type)) Out.Emplace(D.Name, D.Type);
	}

	void SetObjectProp(AActor* A, const FString& Key, const FString& Value)
	{
		if (A && BF6Api::IsEditing()) BF6Api::SetActorProp(A, Key, Value);
	}

	FString ObjectProp(AActor* A, const FString& Key) { return BF6Api::GetActorProp(A, Key); }

	FObjectPreview SelectedObjectPreview()
	{
		FObjectPreview Out;
		FString Type;
		AActor* A = BF6Api::SelectedGameplayActor(Type);
		if (!A || !IsEditing() || !A->Tags.Contains(FName(TEXT("BF6Placed")))) return Out;
		Out.Id = A->GetPathName(); Out.Type = Type; Out.Label = A->GetActorLabel();
		for (const FName& Tag : A->Tags)
		{
			const FString S = Tag.ToString();
			FString Key, Value;
			if (S.StartsWith(TEXT("preview:highpoly.")) && S.Mid(17).Split(TEXT("="), &Key, &Value))
				Out.Values.Add(Key, Value);
		}
		return Out;
	}

	bool SetObjectPreview(const FString& Id, const FString& Key, const FString& Value)
	{
		return SetObjectPreviews(Id, {{Key, Value}});
	}
	bool SetObjectPreviews(const FString& Id, const TMap<FString, FString>& Values)
	{
		if (!IsEditing() || !GEditor || Id.IsEmpty() || Values.IsEmpty()) return false;
		for (const auto& Pair : Values)
		{
			if (Pair.Key.IsEmpty() || Pair.Key.Len() > 48 || Pair.Value.Len() > 700) return false;
			for (TCHAR C : Pair.Key) if (!FChar::IsAlnum(C) && C != TEXT('_')) return false;
			if (Pair.Value.Contains(TEXT("\n")) || Pair.Value.Contains(TEXT("\r"))) return false;
		}
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return false;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			AActor* A = *It;
			if (A->GetPathName() != Id || !A->Tags.Contains(FName(TEXT("BF6Placed")))) continue;
			FScopedTransaction Tx(FText::FromString(TEXT("Change loadout preview")));
			A->Modify();
			for (const auto& Pair : Values)
			{
				const FString Prefix = TEXT("preview:highpoly.") + Pair.Key + TEXT("=");
				A->Tags.RemoveAll([&](const FName& T){ return T.ToString().StartsWith(Prefix); });
				if (!Pair.Value.IsEmpty()) A->Tags.Add(FName(*(Prefix + Pair.Value)));
			}
			return true;
		}
		return false;
	}

	void SetObjectLabel(AActor* A, const FString& Label)
	{
		if (A && !Label.IsEmpty()) BF6Api::SetActorPrettyLabel(A, Label);
	}

	FString ObjectLinkName(AActor* A)
	{
		if (!A) return FString();
		FString Nm = A->GetActorLabel();
		Nm.RemoveFromStart(TEXT("BF6_"));
		return Nm;
	}

	bool SetVolumeLoop(AActor* Volume, const TArray<FVector>& WorldPoints)
	{
		if (!BF6Api::IsEditing()) return false;
		return BF6Api::SetVolumeLoop(Volume, WorldPoints);
	}

	void ParentUnder(AActor* Child, AActor* Parent)
	{
		if (BF6Api::IsEditing()) BF6Api::ParentUnder(Child, Parent);
	}

	void SelectNone()
	{
		if (GEditor) GEditor->SelectNone(false, true, false);
	}

	void RefreshSceneTree() { BF6Api::RefreshSceneTree(); }

	// ---- UI sounds ----------------------------------------------------------
	//
	// The seam's types are the ones an add-on compiles against and are not the
	// tool's internal ones, so the sound module can change shape without an
	// add-on rebuild. These two adapters are the whole cost of that.

	static_assert(int32(EUiSound::Count) == int32(EBF6UiSound::Count),
		"BF6Ext::EUiSound and EBF6UiSound have drifted apart");

	namespace
	{
		class FUiSoundBridge : public BF6UiSound::IBF6UiSoundProvider
		{
		public:
			explicit FUiSoundBridge(TSharedPtr<IUiSoundProvider> In) : Inner(In) {}
			virtual USoundWave* SoundFor(EBF6UiSound Event) override
			{
				return Inner.IsValid() ? Inner->SoundFor(EUiSound(Event)) : nullptr;
			}
			virtual FString Describe() const override
			{
				return Inner.IsValid() ? Inner->Describe() : FString();
			}
		private:
			TSharedPtr<IUiSoundProvider> Inner;
		};

		class FPreviewBridge : public BF6UiSound::IBF6PlaceableSoundPreview
		{
		public:
			explicit FPreviewBridge(TSharedPtr<IPlaceableSoundPreview> In) : Inner(In) {}
			virtual bool CanPreview(const FString& Type) const override
			{
				return Inner.IsValid() && Inner->CanPreview(Type);
			}
			virtual void Preview(const FString& Type) override
			{
				if (Inner.IsValid()) Inner->Preview(Type);
			}
			virtual void Stop() override { if (Inner.IsValid()) Inner->Stop(); }
			virtual FString Playing() const override
			{
				return Inner.IsValid() ? Inner->Playing() : FString();
			}
		private:
			TSharedPtr<IPlaceableSoundPreview> Inner;
		};
	}

	void RegisterUiSoundProvider(TSharedPtr<IUiSoundProvider> Provider)
	{
		TSharedPtr<BF6UiSound::IBF6UiSoundProvider> Bridge;
		if (Provider.IsValid()) Bridge = MakeShared<FUiSoundBridge>(Provider);
		BF6UiSound::SetProvider(Bridge);
	}

	void RegisterPlaceableSoundPreview(TSharedPtr<IPlaceableSoundPreview> Preview)
	{
		TSharedPtr<BF6UiSound::IBF6PlaceableSoundPreview> Bridge;
		if (Preview.IsValid()) Bridge = MakeShared<FPreviewBridge>(Preview);
		BF6UiSound::SetPlaceablePreview(Bridge);
	}

	bool  UiSoundEnabled() { return BF6UiSound::IsEnabled(); }
	float UiSoundVolume()  { return BF6UiSound::Volume(); }
}

// ---- what the tool calls, on its side of the seam ---------------------------

namespace BF6ExtInternal
{
	const TArray<BF6Ext::FPieEntry>& PieEntries() { return GEntries; }

	const TArray<BF6Ext::FPieSubEntry>& AddonSubEntries() { return BF6Ext::GSubEntries; }

	const FSlateBrush* AddonThumb(const FString& Type)
	{
		if (Type.IsEmpty()) return nullptr;
		for (const FThumbProvider& P : GThumbProviders)
			if (const FSlateBrush* B = P.Fn(Type)) return B;
		return nullptr;
	}

	FString AddonThumbDetail(const FString& Type)
	{
		if (Type.IsEmpty()) return FString();
		// First non-empty answer wins, matching the brush rule, so the label a
		// card shows always comes from the same add-on that could have drawn it.
		for (const FThumbDetail& D : GThumbDetails)
		{
			const FString S = D.Fn(Type);
			if (!S.IsEmpty()) return S;
		}
		return FString();
	}

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
	static bool bMapClosing = false;
	bool IsMapClosing() { return bMapClosing; }
	void BroadcastMapClosing(const FString& Level)
	{
		TGuardValue<bool> Closing(bMapClosing, true);
		GMapClosing.Broadcast(Level);
	}
}
