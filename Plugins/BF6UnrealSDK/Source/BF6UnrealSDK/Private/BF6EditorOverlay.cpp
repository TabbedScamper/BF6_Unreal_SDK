#include "BF6EditorOverlay.h"
#include "BF6GameLog.h"
#include "BF6Capabilities.h"   // the CHANGES sheet
#include "BF6BuildMode.h"    // BF6Api::IsBuildOverlayActive: which screen is up
#include "BF6Blocks.h"
#include "BF6Experience.h"   // ---- BF6Experience ---- the fourth entry
#include "BF6Script.h"
#include "BF6UiBuilder.h"
#include "BF6Internal.h"
#include "BF6Theme.h"

#include "Brushes/SlateColorBrush.h"
#include "Containers/Ticker.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/IConsoleManager.h"
#include "Layout/WidgetPath.h"
#include "LevelEditor.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "SLevelViewport.h"
#include "Styling/SlateBrush.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"

// ============================================================================
// See BF6EditorOverlay.h for what this is. Layout of this file:
//   1. state
//   2. the sheet: one widget over the viewport, with the toolbar row left clear
//   3. show, hide, and handing the page back to its dock tab
//   4. what the user left up last time
//   5. console commands and the input-handler seam
// ============================================================================

namespace
{
	// ---- 1. state ----------------------------------------------------------

	using EEditor = BF6EditorOverlay::EEditor;

	const TCHAR* kIniSection = TEXT("BF6UnrealSDK");
	const TCHAR* kIniKey     = TEXT("EditorOverlay");

	// Above the build HUD (50), below the Portal column (90), the radial (100)
	// and the controls sheet (200). The HUD's toolbar row is drawn under this
	// sheet and shows through the strip the sheet deliberately leaves clear.
	const int32 kZOrder = 70;

	EEditor GActive     = EEditor::None;
	EEditor GRemembered = EEditor::None;
	bool    GRestored   = false;

	// The watchdog that makes "never on the map screen" a fact rather than an
	// agreement between callers. See the header.
	FTSTicker::FDelegateHandle GGuard;

	TSharedPtr<SWidget>      GRoot;    // the whole viewport; the top strip lets clicks through
	TSharedPtr<SWidget>      GSheet;   // the opaque part, and what the hit test asks about
	TSharedPtr<SBox>         GHost;    // where the editor's own widget sits
	TSharedPtr<SWidget>      GPage;    // that widget, so focus can be handed to it
	TWeakPtr<SLevelViewport> GVP;

	TArray<IConsoleObject*>  GCmds;

	const FSlateBrush* SheetBrush() { static FSlateColorBrush B(BF6Theme::Ink); return &B; }

	const TCHAR* NameOf(EEditor E)
	{
		switch (E)
		{
		case EEditor::Blocks:     return TEXT("blocks");
		case EEditor::Log:        return TEXT("log");
		case EEditor::Script:     return TEXT("script");
		case EEditor::Ui:         return TEXT("ui");
		case EEditor::Experience: return TEXT("experience");   // ---- BF6Experience ----
		default:                  return TEXT("none");
		}
	}

	EEditor FromName(const FString& S)
	{
		if (S.Equals(TEXT("blocks"), ESearchCase::IgnoreCase)) return EEditor::Blocks;
		if (S.Equals(TEXT("script"), ESearchCase::IgnoreCase)) return EEditor::Script;
		if (S.Equals(TEXT("log"), ESearchCase::IgnoreCase))    return EEditor::Log;
		if (S.Equals(TEXT("changes"), ESearchCase::IgnoreCase)) return EEditor::Caps;
		if (S.Equals(TEXT("ui"), ESearchCase::IgnoreCase))     return EEditor::Ui;
		// ---- BF6Experience ----
		if (S.Equals(TEXT("experience"), ESearchCase::IgnoreCase)) return EEditor::Experience;
		// ---- end BF6Experience ----
		return EEditor::None;
	}

	// THE ONE PLACE THAT KNOWS THE FOUR. Everything else here is placement.
	TSharedRef<SWidget> WidgetFor(EEditor E)
	{
		switch (E)
		{
		case EEditor::Blocks:     return BF6Blocks::Widget();
		case EEditor::Log:        return BF6GameLog::Widget();
		case EEditor::Script:     return BF6Script::Widget();
		case EEditor::Caps:       return BF6Caps::Widget();
		case EEditor::Ui:         return BF6UiBuilder::Widget();
		case EEditor::Experience: return BF6Experience::Widget();   // a Slate screen, not a page
		default:                  return SNullWidget::NullWidget;
		}
	}

	void ReleaseWidget(EEditor E)
	{
		switch (E)
		{
		case EEditor::Blocks:     BF6Blocks::ReleaseWidget();     break;
		case EEditor::Log:        BF6GameLog::ReleaseWidget();    break;
		case EEditor::Script:     BF6Script::ReleaseWidget();     break;
		case EEditor::Caps:       BF6Caps::ReleaseWidget();     break;
		case EEditor::Ui:         BF6UiBuilder::ReleaseWidget();  break;
		case EEditor::Experience: BF6Experience::ReleaseWidget(); break;
		default: break;
		}
	}

	void Remember(EEditor E)
	{
		// In memory only. Persisting this is what made a fresh session open a
		// map straight into the block editor; see RestoreRemembered.
		GRemembered = E;
	}

	// ---- 2. the sheet -------------------------------------------------------

	TSharedRef<SWidget> MakeRoot()
	{
		// THE HIT-TEST LAW, the same one the Portal overlay keeps: the root
		// spans the whole viewport, so it must let clicks through everywhere it
		// is not drawing. Only the sheet below the toolbar row is real, and the
		// strip above it belongs to the row's buttons.
		TSharedRef<SVerticalBox> Root = SNew(SVerticalBox).Visibility(EVisibility::SelfHitTestInvisible);

		Root->AddSlot().AutoHeight()
		[
			SNew(SBox)
			.HeightOverride(BF6EditorOverlay::TopBarHeight)
			.Visibility(EVisibility::SelfHitTestInvisible)
			[ SNew(SSpacer).Visibility(EVisibility::SelfHitTestInvisible) ]
		];

		TSharedRef<SBorder> Sheet =
			SNew(SBorder)
			.BorderImage(SheetBrush())
			.Padding(0.f)
			.Visibility(EVisibility::Visible)
			[ SAssignNew(GHost, SBox) ];

		Root->AddSlot().FillHeight(1.f)[ Sheet ];
		GSheet = Sheet;
		return Root;
	}

	TSharedPtr<SLevelViewport> ActiveViewport()
	{
		if (!FModuleManager::Get().IsModuleLoaded("LevelEditor")) return nullptr;
		FLevelEditorModule& LE = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
		return LE.GetFirstActiveLevelViewport();
	}

	// After a move, hand the keyboard to the page. Next tick, once the parent
	// chain exists: focusing a widget that is not in a window yet does nothing.
	void FocusPage()
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			if (GActive != EEditor::None && GPage.IsValid() && FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().SetKeyboardFocus(GPage, EFocusCause::SetDirectly);
			}
			return false;
		}), 0.f);
	}

	// Hiding gives the keyboard back to the world, so WASD flies again without
	// anyone having to click the viewport first.
	void FocusViewport()
	{
		if (!FSlateApplication::IsInitialized()) return;
		if (TSharedPtr<SLevelViewport> VP = ActiveViewport())
		{
			FSlateApplication::Get().SetKeyboardFocus(VP, EFocusCause::SetDirectly);
		}
	}

	// Take the sheet down and give the page back to its dock tab. Says which
	// editor was up, because the two callers want different things remembered:
	// the user pressing the button means "none", the build screen going away
	// means "this one, when you come back".
	EEditor TakeDownSheet()
	{
		const EEditor Was = GActive;
		if (Was == EEditor::None) return Was;
		GActive = EEditor::None;
		GPage.Reset();

		if (GHost.IsValid()) GHost->SetContent(SNullWidget::NullWidget);
		// The page is not destroyed, only unparented: it goes back into its
		// dock tab if that tab is open, and otherwise waits, still loaded, for
		// the next time a button asks for it.
		ReleaseWidget(Was);

		if (GRoot.IsValid())
		{
			if (TSharedPtr<SLevelViewport> VP = GVP.Pin()) VP->RemoveOverlayWidget(GRoot.ToSharedRef());
		}
		GVP.Reset();
		FocusViewport();
		return Was;
	}

// ---- the scene tree, out of the way while an editor is up --------------------
//
// Blocks, script and the UI builder cover the viewport, but the Scene Outliner
// is its own dock tab somewhere else in the layout and stayed on screen behind
// them: a list of level actors next to a script editor, belonging to neither.
//
// Closed rather than hidden, because a dock tab has no hidden state, and only
// the ones that were actually open are put back. Unreal names three of them and
// a layout can hold any of them, so all three are handled.
const FName kOutlinerTabs[] = {
		FName(TEXT("LevelEditorSceneOutliner")),
		FName(TEXT("LevelEditorSceneOutliner2")),
		FName(TEXT("LevelEditorSceneOutliner3"))
	};
	TArray<FName> GClosedOutliners;
	bool GSceneTreeStoodDown = false;

	TSharedPtr<FTabManager> LevelEditorTabs()
	{
		if (!FModuleManager::Get().IsModuleLoaded("LevelEditor")) { return nullptr; }
		FLevelEditorModule& LE = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
		return LE.GetLevelEditorTabManager();
	}

	void StandDownTheSceneTree(bool bAway)
	{
		if (bAway == GSceneTreeStoodDown) { return; }
		TSharedPtr<FTabManager> TM = LevelEditorTabs();
		if (!TM.IsValid()) { return; }
		GSceneTreeStoodDown = bAway;

		if (bAway)
		{
			GClosedOutliners.Reset();
			for (const FName& Id : kOutlinerTabs)
			{
				if (TSharedPtr<SDockTab> T = TM->FindExistingLiveTab(FTabId(Id)))
				{
					GClosedOutliners.Add(Id);
					T->RequestCloseTab();
				}
			}
			if (GClosedOutliners.Num())
			{
				UE_LOG(LogBF6, Display, TEXT("BF6 editors: the scene tree was put away (%d tab(s))."),
					GClosedOutliners.Num());
			}
		}
		else
		{
			for (const FName& Id : GClosedOutliners) { TM->TryInvokeTab(FTabId(Id)); }
			if (GClosedOutliners.Num())
			{
				UE_LOG(LogBF6, Display, TEXT("BF6 editors: the scene tree is back (%d tab(s))."),
					GClosedOutliners.Num());
			}
			GClosedOutliners.Reset();
		}
	}
}

// ---- 3. show and hide --------------------------------------------------------

void BF6EditorOverlay::Show(EEditor Which)
{
	if (Which == EEditor::None) { Hide(); return; }
	if (GActive == Which) return;
	if (!FSlateApplication::IsInitialized()) return;

	// THE MAP SCREEN IS NOT A VIEWPORT TO COVER. Refused here rather than
	// trusted to every caller: the console command, the toolbar row, the
	// EXPERIENCE entry point and the restore all come through this one door,
	// and before the tool UI exists at all IsBuildOverlayActive is false, which
	// is what makes the first frames after launch safe too.
	//
	// Refusing is not forgetting: the editor is remembered, so the button the
	// user pressed is still the one that comes back when the build screen does.
	if (!BF6Api::IsBuildOverlayActive())
	{
		GRemembered = Which;
		GRestored = false;
		UE_LOG(LogBF6, Display,
			TEXT("BF6 editors: not showing '%s' - the build screen is not up, so there is nothing to cover. It is remembered for when it is."),
			NameOf(Which));
		return;
	}

	TSharedPtr<SLevelViewport> VP = ActiveViewport();
	if (!VP.IsValid())
	{
		// SAID ONCE, NOT EVERY QUARTER SECOND. This bail was silent, so an
		// editor that failed to come up looked exactly like one nobody had
		// asked for. The watchdog retries on a tick, so it must not fill the
		// log while it waits for a viewport that is still being built.
		static bool bSaid = false;
		if (!bSaid)
		{
			bSaid = true;
			UE_LOG(LogBF6, Warning,
				TEXT("BF6 editors: '%s' cannot go up yet, the build screen has no level viewport. Retrying."),
				NameOf(Which));
		}
		return;
	}

	// The widget FIRST. A build with no embedded browser hands back its own
	// message page rather than nothing, and the host shows that instead of a
	// blank sheet with no way to tell what went wrong.
	TSharedRef<SWidget> W = WidgetFor(Which);

	const EEditor Was = GActive;
	if (Was != EEditor::None && Was != Which) ReleaseWidget(Was);

	if (!GRoot.IsValid()) GRoot = MakeRoot();
	GPage = W;
	GHost->SetContent(W);

	if (GVP.Pin() != VP)
	{
		if (TSharedPtr<SLevelViewport> Old = GVP.Pin()) Old->RemoveOverlayWidget(GRoot.ToSharedRef());
		VP->AddOverlayWidget(GRoot.ToSharedRef(), kZOrder);
		GVP = VP;
	}

	GActive = Which;
	StandDownTheSceneTree(true);
	Remember(Which);
	FocusPage();
	UE_LOG(LogBF6, Display, TEXT("BF6 editors: '%s' now covers the build screen."), NameOf(Which));
}

void BF6EditorOverlay::Hide()
{
	const EEditor Was = TakeDownSheet();
	if (Was == EEditor::None) return;
	StandDownTheSceneTree(false);
	Remember(EEditor::None);
	UE_LOG(LogBF6, Display, TEXT("BF6 editors: '%s' closed. Nothing is remembered."), NameOf(Was));
}

void BF6EditorOverlay::Suspend()
{
	const EEditor Was = TakeDownSheet();
	if (Was == EEditor::None) return;
	StandDownTheSceneTree(false);
	// The user did not press the button, so nothing is forgotten: the same
	// editor is put back the next time the build screen comes up.
	GRemembered = Was;
	GRestored = false;
	UE_LOG(LogBF6, Display,
		TEXT("BF6 editors: '%s' taken down, the build screen is not up. It comes back with it."), NameOf(Was));
}

void BF6EditorOverlay::Toggle(EEditor Which)
{
	if (Which == EEditor::None || GActive == Which) Hide();
	else                                            Show(Which);
}

void BF6EditorOverlay::HideIfShowing(EEditor Which)
{
	if (Which != EEditor::None && GActive == Which) Hide();
}

BF6EditorOverlay::EEditor BF6EditorOverlay::Active() { return GActive; }
bool BF6EditorOverlay::IsShown() { return GActive != EEditor::None; }
bool BF6EditorOverlay::IsShowing(EEditor Which) { return GActive == Which; }

// ---- 4. what the user left up last time --------------------------------------

void BF6EditorOverlay::RestoreRemembered()
{
	// OPENING A MAP SHOWS THE MAP.
	//
	// This used to put back whichever editor was up when you last left the
	// build screen, persisted across sessions. The intent was to save you
	// reopening it; the effect was that opening a map covered the map with the
	// block editor, every time, and the only way to stop it was to notice that
	// pressing an editor's own close button is what clears the memory while
	// leaving the screen does not.
	//
	// A map is opened to look at the map. The editors are one keystroke away
	// on the top bar, and what is worth remembering about a map is where the
	// camera was, which BF6BuildMode now does.
	//
	// Suspend still keeps GRemembered in memory, so toggling an editor off and
	// back on within one session behaves as before. Nothing is restored on its
	// own and nothing is written to the ini any more.
	return;
}

FString BF6EditorOverlay::Status()
{
	return FString::Printf(TEXT(
		"BF6 editors\n"
		"  covering the screen : %s\n"
		"  screen showing      : %s\n"
		"  remembered          : %s\n"
		"  sheet               : %s\n"
		"  viewport            : %s\n"
		"  page has keyboard   : %s"),
		NameOf(GActive),
		BF6Api::IsBuildOverlayActive() ? TEXT("build") : TEXT("map selection (no sheet allowed)"),
		NameOf(GRemembered),
		GRoot.IsValid() ? TEXT("built") : TEXT("not built yet"),
		GVP.IsValid() ? TEXT("attached") : TEXT("none"),
		WantsKeyboard() ? TEXT("yes") : TEXT("no"));
}

// ---- 5. module hooks and the input-handler seam -------------------------------

void BF6EditorOverlay::Register()
{
	// A previously saved editor is deliberately NOT read back, and the stale
	// key is cleared so an existing install stops doing this too.
	if (GConfig->DoesSectionExist(kIniSection, GEditorPerProjectIni))
	{
		GConfig->RemoveKey(kIniSection, kIniKey, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	// THE WATCHDOG. Four lines that make the rule absolute instead of agreed:
	// whatever route the tool takes back to the map selection screen - the
	// CHOOSE ANOTHER MAP button, the SDK setup screen, an extension, a world
	// that closed under us, or one nobody has written yet - the sheet is down
	// within a quarter of a second, and it is Suspend rather than Hide, so the
	// editor the user had open comes back with the build screen.
	// BOTH DIRECTIONS. The comment above promised the editor "comes back with
	// the build screen" and only the taking-down half was ever wired:
	// RestoreRemembered was written, declared in the header, and called from
	// nowhere in the entire plugin. So opening a map took the sheet down,
	// remembered blocks, and then left the screen empty for ever - which is
	// what put a bare Portal panel in front of the user instead of their
	// blocks. Confirmed on MP_Battery: remembered 'blocks', covering 'none',
	// sheet 'not built yet'.
	//
	// RestoreRemembered is safe to call on a tick: it refuses unless the build
	// screen is up, it refuses once it has succeeded, and Hide clears what is
	// remembered so an editor the user closed stays closed.
	GGuard = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
	{
		const bool bBuildScreen = BF6Api::IsBuildOverlayActive();
		if (GActive != EEditor::None && !bBuildScreen)      BF6EditorOverlay::Suspend();
		else if (GActive == EEditor::None && bBuildScreen)  BF6EditorOverlay::RestoreRemembered();
		return true;
	}), 0.25f);

	IConsoleManager& CM = IConsoleManager::Get();
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Editors.Show"),
		TEXT("BF6.Editors.Show blocks|script|log|ui|experience|none  Cover the viewport with one editor, or show the world again."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
		{
			BF6EditorOverlay::Show(A.Num() ? FromName(A[0]) : EEditor::None);
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Editors.Status"),
		TEXT("Which editor is covering the screen, which one is remembered, and where the keyboard is."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			UE_LOG(LogBF6, Display, TEXT("%s"), *BF6EditorOverlay::Status());
		})));
}

void BF6EditorOverlay::Unregister()
{
	for (IConsoleObject* C : GCmds) if (C) IConsoleManager::Get().UnregisterConsoleObject(C);
	GCmds.Reset();
	if (GGuard.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(GGuard); GGuard.Reset(); }

	// Teardown order is not ours to trust: touching a viewport that is already
	// going crashes, so late in the exit the sheet is simply dropped. The
	// process takes the memory with it.
	if (FSlateApplication::IsInitialized() && !IsEngineExitRequested())
	{
		if (GRoot.IsValid())
		{
			if (TSharedPtr<SLevelViewport> VP = GVP.Pin()) VP->RemoveOverlayWidget(GRoot.ToSharedRef());
		}
		if (GHost.IsValid()) GHost->SetContent(SNullWidget::NullWidget);
	}
	GActive = EEditor::None;
	GVP.Reset();
	GPage.Reset();
	GHost.Reset();
	GSheet.Reset();
	GRoot.Reset();
}

bool BF6EditorOverlay::WantsKeyboard()
{
	if (GActive == EEditor::None || !GPage.IsValid() || !FSlateApplication::IsInitialized()) return false;
	TSharedPtr<SWidget> W = FSlateApplication::Get().GetKeyboardFocusedWidget();
	while (W.IsValid())
	{
		if (W == GPage) return true;
		W = W->GetParentWidget();
	}
	return false;
}

bool BF6EditorOverlay::PathTouchesHost(const FWidgetPath& Path)
{
	if (GActive == EEditor::None || !Path.IsValid()) return false;
	return GSheet.IsValid() && Path.ContainsWidget(GSheet.Get());
}

bool BF6EditorOverlay::CursorOverHost(const FVector2D& ScreenPos)
{
	if (GActive == EEditor::None || !FSlateApplication::IsInitialized()) return false;
	FSlateApplication& App = FSlateApplication::Get();
	const FWidgetPath Path = App.LocateWindowUnderMouse(ScreenPos, App.GetInteractiveTopLevelWindows());
	return PathTouchesHost(Path);
}
