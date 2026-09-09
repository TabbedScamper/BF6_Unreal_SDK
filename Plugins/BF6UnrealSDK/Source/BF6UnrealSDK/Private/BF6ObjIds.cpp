#include "BF6ObjIds.h"
#include "BF6BuildMode.h"        // BF6Api: the registry, actor props, selection
#include "BF6Theme.h"
#include "BF6SDKExtension.h"     // BF6Ext::ToolSavedDir for the reservation file

#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Selection.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "InputCoreTypes.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Text/STextBlock.h"

// ============================================================================
// See BF6ObjIds.h for what this is and where it sits.
//
// NOTICE: Feature set after ObjId Manager for BFPortal by hekaron, MIT.
//
// Layout of this file:
//   1. style kit and small helpers (the BF6PortalWeb pattern, copied not shared)
//   2. categories, bands, reserved ranges
//   3. the assign core
//   4. the panel
//   5. the export window
//   6. public entry points, the dock tab, and the console commands
// ============================================================================

// Declared HERE, at global scope, on purpose: an unseen class name used inside
// an anonymous namespace would be declared as a DIFFERENT, anonymous type, and
// SNew would then fail on an incomplete type.
class SBF6ObjIdManager;
class SBF6ObjIdExport;

namespace
{
	// ---- 1. style kit ------------------------------------------------------
	// The same handful of helpers every panel in this tool draws with. Copied
	// rather than shared: BF6PortalWeb.cpp keeps its own in an anonymous
	// namespace, and one file reaching into another's would be a new coupling
	// for four functions.
	FSlateFontInfo FontBold(int32 Size) { return FCoreStyle::GetDefaultFontStyle("Bold", Size); }
	FSlateFontInfo FontReg(int32 Size)  { return FCoreStyle::GetDefaultFontStyle("Regular", Size); }
	const FSlateBrush* InkBrush()   { static FSlateColorBrush B(BF6Theme::Ink);   return &B; }
	const FSlateBrush* PanelBrush() { static FSlateColorBrush B(BF6Theme::Panel); return &B; }

	const FButtonStyle& GhostStyle()
	{
		static FButtonStyle S = FButtonStyle()
			.SetNormal (FSlateRoundedBoxBrush(BF6Theme::PanelLight, 0.f, BF6Theme::Line, 1.f))
			.SetHovered(FSlateRoundedBoxBrush(FLinearColor(FColor(0x24, 0x28, 0x2B)), 0.f, FLinearColor::White, 1.f))
			.SetPressed(FSlateRoundedBoxBrush(FLinearColor(FColor(0x2E, 0x33, 0x37)), 0.f, FLinearColor::White, 1.f))
			.SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0, 1, 0, -1));
		return S;
	}

	TSharedRef<SWidget> Btn(const FString& Label, TFunction<void()> Fn, const FString& Tip = FString())
	{
		return SNew(SButton).ButtonStyle(&GhostStyle()).ContentPadding(FMargin(8, 4))
			.ToolTipText(FText::FromString(Tip))
			.OnClicked_Lambda([Fn]{ if (Fn) Fn(); return FReply::Handled(); })
			[ SNew(STextBlock).Font(FontBold(9)).ColorAndOpacity(FSlateColor(BF6Theme::Text))
				.Text(FText::FromString(Label.ToUpper())) ];
	}

	// A tiny inline action inside a row: no frame, just the word.
	TSharedRef<SWidget> MiniBtn(const FString& Label, TFunction<void()> Fn, const FString& Tip = FString())
	{
		return SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(5, 1))
			.ToolTipText(FText::FromString(Tip))
			.OnClicked_Lambda([Fn]{ if (Fn) Fn(); return FReply::Handled(); })
			[ SNew(STextBlock).Font(FontBold(8)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
				.Text(FText::FromString(Label.ToUpper())) ];
	}

	TSharedRef<SWidget> Label(const FString& Text, int32 Size = 9, FLinearColor Col = BF6Theme::TextDim)
	{
		return SNew(STextBlock).Font(FontReg(Size)).ColorAndOpacity(FSlateColor(Col))
			.Text(FText::FromString(Text));
	}

	const TCHAR* kIniSection = TEXT("BF6UnrealSDK.ObjIds");
	const FName  kTabId(TEXT("BF6ObjIds"));

	void Toast(const FString& Msg) { BF6Api::Toast(Msg); }

	// ---- 2. categories -----------------------------------------------------
	// The addon's categories, plus the ones our tool places that it never saw.
	// Matched on the SDK type name rather than a script path, because that is
	// what our registry rows carry. Order matters: VehicleSpawner and
	// LootSpawner both contain "Spawn", and AreaTrigger contains "Area".
	FString CategoryOf(const FString& Type)
	{
		auto Has = [&Type](const TCHAR* S){ return Type.Contains(S, ESearchCase::IgnoreCase); };
		if (Type.StartsWith(TEXT("SFX_")))  return TEXT("Sound");
		if (Type.StartsWith(TEXT("FX_")) || Type.StartsWith(TEXT("fx_")) || Type.StartsWith(TEXT("VFX_")))
			return TEXT("Visual FX");
		if (Has(TEXT("CapturePoint")))      return TEXT("Capture points");
		if (Has(TEXT("Sector")))            return TEXT("Sectors");
		if (Has(TEXT("HQ")))                return TEXT("HQs");
		if (Has(TEXT("MCOM")))              return TEXT("MCOMs");
		if (Has(TEXT("VehicleSpawner")) || Type.StartsWith(TEXT("VEH_")))
			return TEXT("Vehicle spawners");
		if (Has(TEXT("LootSpawner")))       return TEXT("Loot spawners");
		if (Has(TEXT("Emplacement")))       return TEXT("Emplacements");
		if (Has(TEXT("WaypointPath")))      return TEXT("AI paths");
		if (Has(TEXT("Spawn")))             return TEXT("Spawn points");
		if (Has(TEXT("Trigger")))           return TEXT("Area triggers");
		if (Has(TEXT("Cam")))               return TEXT("Fixed cameras");
		if (Has(TEXT("CombatArea")) || Has(TEXT("Volume")) || Has(TEXT("Area")))
			return TEXT("Volumes");
		if (Has(TEXT("RingOfFire")) || Has(TEXT("HeatZone")) || Has(TEXT("Cloud")) || Has(TEXT("Bomb")))
			return TEXT("World effects");
		if (Has(TEXT("WorldIcon")) || Has(TEXT("InteractPoint")) || Has(TEXT("Icon")))
			return TEXT("UI/other");
		return TEXT("Spatial objects");
	}

	// The order categories are drawn in: what a mode is built out of first, then
	// the furniture. Anything unlisted follows, alphabetically.
	const TArray<FString>& CategoryOrder()
	{
		static const TArray<FString> Order = {
			TEXT("Capture points"), TEXT("Sectors"), TEXT("HQs"), TEXT("MCOMs"),
			TEXT("Spawn points"), TEXT("Vehicle spawners"), TEXT("Emplacements"),
			TEXT("Area triggers"), TEXT("Fixed cameras"), TEXT("Volumes"),
			TEXT("AI paths"), TEXT("Loot spawners"), TEXT("World effects"),
			TEXT("Sound"), TEXT("Visual FX"), TEXT("Spatial objects"), TEXT("UI/other")
		};
		return Order;
	}

	int32 CategoryRank(const FString& Cat)
	{
		const int32 i = CategoryOrder().IndexOfByKey(Cat);
		return i >= 0 ? i : CategoryOrder().Num();
	}

	// Only the four bands the community's script templates actually assume are
	// hard-coded. Inventing a convention for the rest would be worse than
	// saying "any": a made-up band flags a perfectly good map as wrong. A
	// project that HAS its own convention adds it in DefaultEngine.ini.
	bool BandOf(const FString& Cat, int32& Base, int32& Span)
	{
		Base = 0; Span = 0;
		FString Key = TEXT("ObjIdBand_") + Cat.Replace(TEXT(" "), TEXT("")).Replace(TEXT("/"), TEXT(""));
		int32 Cfg = 0;
		if (GConfig && GConfig->GetInt(TEXT("BF6UnrealSDK"), *Key, Cfg, GEngineIni) && Cfg > 0)
		{
			Base = Cfg; Span = 100;
			int32 CfgSpan = 0;
			if (GConfig->GetInt(TEXT("BF6UnrealSDK"), *(Key + TEXT("Span")), CfgSpan, GEngineIni) && CfgSpan > 0)
				Span = CfgSpan;
			return true;
		}
		if (Cat == TEXT("Sectors"))        { Base = 100; Span = 100; return true; }
		if (Cat == TEXT("Capture points")) { Base = 200; Span = 100; return true; }   // A is 200
		if (Cat == TEXT("HQs"))            { Base = 301; Span = 99;  return true; }
		if (Cat == TEXT("MCOMs"))          { Base = 400; Span = 100; return true; }
		return false;
	}

	// ---- reserved ranges ---------------------------------------------------
	// Numbers the creator has set aside for something not built yet (a second
	// pass of flags, a script that already names ids). Auto-assign steps over
	// them. Kept beside the session's own save, keyed to level and save name, so
	// reopening a map brings its reservations back and another map never sees
	// them.
	struct FReserve { int32 From = 0; int32 To = 0; FString Note; };

	FString ReserveKey()
	{
		FString K = BF6Api::CurrentLevel() + TEXT("__") + BF6Api::CurrentSave();
		if (K == TEXT("__")) K = TEXT("_no_session");
		FString Safe;
		for (TCHAR C : K) Safe.AppendChar(FChar::IsAlnum(C) || C == TEXT('_') ? C : TEXT('_'));
		return Safe;
	}

	FString ReserveFile() { return FPaths::Combine(BF6Ext::ToolSavedDir(), TEXT("objids"), ReserveKey() + TEXT(".txt")); }

	TArray<FReserve>& Reserves()
	{
		static TArray<FReserve> R;
		static FString LoadedFor;
		const FString Key = ReserveKey();
		if (LoadedFor != Key)
		{
			LoadedFor = Key;
			R.Reset();
			FString Text;
			if (FFileHelper::LoadFileToString(Text, *ReserveFile()))
			{
				TArray<FString> Lines;
				Text.ParseIntoArrayLines(Lines);
				for (const FString& L : Lines)
				{
					TArray<FString> P;
					L.ParseIntoArray(P, TEXT(","), false);
					if (P.Num() < 2) continue;
					FReserve E;
					E.From = FCString::Atoi(*P[0]);
					E.To   = FCString::Atoi(*P[1]);
					if (P.Num() > 2) E.Note = P[2].TrimStartAndEnd();
					if (E.To >= E.From) R.Add(E);
				}
			}
		}
		return R;
	}

	void SaveReserves()
	{
		FString Out;
		for (const FReserve& E : Reserves())
			Out += FString::Printf(TEXT("%d,%d,%s\n"), E.From, E.To, *E.Note);
		const FString File = ReserveFile();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
		FFileHelper::SaveStringToFile(Out, *File);
	}

	bool IsReserved(int32 Id)
	{
		for (const FReserve& E : Reserves()) if (Id >= E.From && Id <= E.To) return true;
		return false;
	}

	// ---- the referenced-by provider ---------------------------------------
	TFunction<TArray<int32>()> GRefProvider;

	// ---- 3. the assign core ------------------------------------------------
	// Band-aware, reservation-aware sequential assignment. The plain path (no
	// bands, nothing reserved) is left to BF6Api::AutoAssignObjIds so the two
	// never drift: this runs only when the panel is asking for something that
	// function cannot express.
	struct FAssign
	{
		int32 Assigned = 0; int32 Kept = 0; int32 Considered = 0;
		// Set when a category's band had no room left. Nothing is assigned in
		// that case: see the note on the two-pass allocation below.
		FString Exhausted;
	};

	// Each target arrives WITH its category, because an actor label is not a
	// type and guessing the band from it would put objects in the wrong one.
	// Every caller here reads the category off the registry row it already has.
	FAssign AssignSequentialTyped(const TArray<TPair<TWeakObjectPtr<AActor>, FString>>& Targets,
	                              int32 Start, bool bBands, bool bOverwrite)
	{
		FAssign R;
		TSet<AActor*> TargetSet;
		for (const auto& P : Targets) if (AActor* A = P.Key.Get()) TargetSet.Add(A);
		R.Considered = TargetSet.Num();
		if (R.Considered == 0) return R;

		TSet<int32> Taken;
		for (const BF6Api::FObjIdRow& Row : BF6Api::GatherObjIds())
		{
			if (Row.Id < 0) continue;
			AActor* RA = Row.Actor.Get();
			if (bOverwrite && RA && TargetSet.Contains(RA)) continue;
			Taken.Add(Row.Id);
		}

		TMap<FString, int32> Cursor;
		// A BAND IS A CEILING, NOT A STARTING POINT.
		//
		// This used to walk Id++ with nothing but a 100000 guard, so a category
		// whose band filled up carried straight on past the end of it, into
		// whatever band came next. Respect bands then did the opposite of what
		// it says: capture points started being numbered into the HQ range, and
		// the only sign was a script addressing the wrong object at runtime.
		// Returns -1 when the band has no room, and the caller stops.
		auto NextFree = [&Taken, &Cursor, Start, bBands](const FString& Cat) -> int32
		{
			int32 Base = 0, Span = 0;
			const bool bHasBand = bBands && BandOf(Cat, Base, Span);
			int32 Id;
			if (int32* C = Cursor.Find(Cat)) Id = *C;
			else if (bHasBand)               Id = (Start > Base && Start < Base + Span) ? Start : Base;
			else                             Id = FMath::Max(0, Start);

			if (bHasBand)
			{
				const int32 End = Base + FMath::Max(Span, 1);
				if (Id < Base) { Id = Base; }
				while (Id < End && (Taken.Contains(Id) || IsReserved(Id))) { Id++; }
				if (Id >= End) { return -1; }
			}
			else
			{
				int32 Guard = 0;
				while ((Taken.Contains(Id) || IsReserved(Id)) && Guard++ < 100000) { Id++; }
			}
			Cursor.Add(Cat, Id + 1);
			return Id;
		};

		// TWO PASSES, BECAUSE A HALF-DONE RENUMBER IS WORSE THAN NONE.
		//
		// Working out every id first means an exhausted band is reported with
		// the map exactly as the user left it. Assigning as we went would leave
		// some objects renumbered and some not, which is harder to undo than to
		// avoid, and leaves a map whose ids nobody can reason about.
		TArray<TPair<AActor*, int32>> Planned;
		for (const auto& P : Targets)
		{
			AActor* A = P.Key.Get();
			if (!A) continue;
			const FString Cur = BF6Api::GetActorProp(A, TEXT("ObjId"));
			const bool bHas = !Cur.IsEmpty() && FCString::Atoi(*Cur) >= 0;
			if (bHas && !bOverwrite) { R.Kept++; continue; }
			const int32 Id = NextFree(P.Value);
			if (Id < 0)
			{
				int32 Base = 0, Span = 0;
				BandOf(P.Value, Base, Span);
				R.Exhausted = FString::Printf(
					TEXT("%s has no free id left between %d and %d. Nothing was renumbered. ")
					TEXT("Free an id in that range, widen the band in DefaultEngine.ini, or turn Respect bands off."),
					*P.Value, Base, Base + FMath::Max(Span, 1) - 1);
				R.Assigned = 0;
				return R;
			}
			Taken.Add(Id);
			Planned.Add({ A, Id });
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Assign ObjIds")));
		for (const TPair<AActor*, int32>& P : Planned)
		{
			P.Key->Modify();
			BF6Api::SetActorProp(P.Key, TEXT("ObjId"), FString::FromInt(P.Value));
			R.Assigned++;
		}
		return R;
	}

	// One id, written as its own undoable step. This is what the in-row edit and
	// "move into band" both go through, so a typo is always one Ctrl+Z away.
	void SetOneId(AActor* A, int32 NewId, const FString& What)
	{
		if (!A) return;
		FScopedTransaction Tx(FText::FromString(What));
		A->Modify();
		BF6Api::SetActorProp(A, TEXT("ObjId"), FString::FromInt(NewId));
	}

	int32 FirstFreeInBand(int32 Base, int32 Span, const TSet<int32>& Taken)
	{
		for (int32 i = Base; i < Base + FMath::Max(Span, 1); i++)
			if (!Taken.Contains(i) && !IsReserved(i)) return i;
		return -1;
	}

	// ---- the TypeScript function map ---------------------------------------
	// The addon's map, kept key for key so a creator moving between the two
	// tools gets the same code. Two differences, both deliberate and listed in
	// the report: CapturePoint and HeatZone get their real accessors here
	// instead of falling through to mod.GetSpatialObject.
	FString TsFuncFor(const FString& Type)
	{
		if (Type.StartsWith(TEXT("SFX_"))) return TEXT("mod.GetSFX");
		if (Type.StartsWith(TEXT("FX_")) || Type.StartsWith(TEXT("fx_")) || Type.StartsWith(TEXT("VFX_")))
			return TEXT("mod.GetVFX");
		static const TMap<FString, FString> M = {
			{ TEXT("AI_Spawner"),                  TEXT("mod.GetSpawner") },
			{ TEXT("AI_WaypointPath"),             TEXT("mod.GetWaypointPath") },
			{ TEXT("AreaTrigger"),                 TEXT("mod.GetAreaTrigger") },
			{ TEXT("FixedCamera"),                 TEXT("mod.GetFixedCamera") },
			{ TEXT("HQ_PlayerSpawner"),            TEXT("mod.GetHQ") },
			{ TEXT("InteractPoint"),               TEXT("mod.GetInteractPoint") },
			{ TEXT("PlayerSpawner"),               TEXT("mod.GetSpawner") },
			{ TEXT("Sector"),                      TEXT("mod.GetSector") },
			{ TEXT("SpawnPoint"),                  TEXT("mod.GetSpawnPoint") },
			{ TEXT("StationaryEmplacementSpawner"), TEXT("mod.GetEmplacementSpawner") },
			{ TEXT("VehicleSpawner"),              TEXT("mod.GetVehicleSpawner") },
			{ TEXT("WorldIcon"),                   TEXT("mod.GetWorldIcon") },
			{ TEXT("MCOM"),                        TEXT("mod.GetMCOM") },
			{ TEXT("LootSpawner"),                 TEXT("mod.GetLootSpawner") },
			{ TEXT("RingOfFire"),                  TEXT("mod.GetRingOfFire") },
			{ TEXT("VL7Cloud"),                    TEXT("mod.GetVL7Cloud") },
			{ TEXT("Bomb"),                        TEXT("mod.GetBomb") },
			{ TEXT("CapturePoint"),                TEXT("mod.GetCapturePoint") },
			{ TEXT("HeatZone"),                    TEXT("mod.GetHeatZone") },
		};
		if (const FString* F = M.Find(Type)) return *F;
		return TEXT("mod.GetSpatialObject");
	}

	// Objects the Portal script API cannot address at all. The addon greys them
	// out rather than hiding them, which is right: the creator needs to see that
	// the object exists and that no code can reach it.
	bool IsExcludedType(const FString& Type)
	{
		return Type == TEXT("CombatArea") || Type == TEXT("DeployCam") || Type == TEXT("SurroundingCombatArea");
	}

	// A label turned into a usable identifier for the Blocks snippet.
	FString IdentFor(const FString& Label, int32 Id)
	{
		FString S;
		bool bUpper = false;
		for (TCHAR C : Label)
		{
			if (FChar::IsAlnum(C)) { S.AppendChar(bUpper ? FChar::ToUpper(C) : C); bUpper = false; }
			else bUpper = S.Len() > 0;
		}
		if (S.Len() == 0 || FChar::IsDigit(S[0])) S = TEXT("obj") + S;
		S[0] = FChar::ToLower(S[0]);
		return S + TEXT("_") + FString::FromInt(Id);
	}
}

// ============================================================================
// 4. the panel
// ============================================================================
class SBF6ObjIdManager : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6ObjIdManager) {}
		SLATE_ARGUMENT(TFunction<void()>, OnBack)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		OnBack = InArgs._OnBack;
		ChildSlot
		[
			SNew(SBox).MinDesiredWidth(640.f)
			[
				SNew(SBorder).BorderImage(PanelBrush()).Padding(10.f)
				[
					SNew(SVerticalBox)

					// title row
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[
							SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(6, 2))
							.Visibility_Lambda([this]{ return OnBack ? EVisibility::Visible : EVisibility::Collapsed; })
							.OnClicked_Lambda([this]{ if (OnBack) OnBack(); return FReply::Handled(); })
							[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
								.Text(FText::FromString(TEXT("< BACK"))) ]
						]
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontBold(13)).ColorAndOpacity(FSlateColor(BF6Theme::Accent))
							.Text(FText::FromString(TEXT("OBJECT IDS"))) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
						[ Label(TEXT("click a name to select it, again to fly there")) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ Btn(TEXT("Dock"), []{ BF6ObjIds::OpenTab(); },
							TEXT("Open the same panel as an editor tab, so it can sit beside the viewport while you build.")) ]
					]

					// status line
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[ SAssignNew(Status, STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)) ]

					// filter row
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
						[ SNew(SBox).WidthOverride(180.f)
							[ SAssignNew(FilterBox, SEditableTextBox)
								.HintText(FText::FromString(TEXT("filter by name, type or id")))
								.OnTextChanged_Lambda([this](const FText& T){ Filter = T.ToString(); Rebuild(); })
								.OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type){ Filter = T.ToString(); Rebuild(); }) ] ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[
							SNew(SCheckBox)
							.IsChecked_Lambda([this]{ return bConflictsOnly ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
							.OnCheckStateChanged_Lambda([this](ECheckBoxState S){ bConflictsOnly = (S == ECheckBoxState::Checked); Rebuild(); })
							[ Label(TEXT("Show conflicts only")) ]
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[
							SNew(SCheckBox)
							.ToolTipText(FText::FromString(TEXT("Tick a row whenever its object is selected in the viewport, so a run can be built by clicking objects in the world.")))
							.IsChecked_Lambda([this]{ return bFollowSelection ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
							.OnCheckStateChanged_Lambda([this](ECheckBoxState S){ bFollowSelection = (S == ECheckBoxState::Checked); })
							[ Label(TEXT("Tick what I select")) ]
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
						[ Btn(TEXT("Collapse all"), [this]{ SetAllCollapsed(true); }) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
						[ Btn(TEXT("Expand all"), [this]{ SetAllCollapsed(false); }) ]
						+ SHorizontalBox::Slot().FillWidth(1) [ SNullWidget::NullWidget ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ Btn(TEXT("Refresh"), [this]{ Rebuild(); }) ]
					]

					// assign row
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(SWrapBox).UseAllottedSize(true)
						+ SWrapBox::Slot().Padding(0, 0, 4, 4).VAlign(VAlign_Center)
						[ Label(TEXT("start at")) ]
						+ SWrapBox::Slot().Padding(4, 0, 6, 4).VAlign(VAlign_Center)
						[ SNew(SBox).WidthOverride(60.f)
							[ SAssignNew(StartBox, SEditableTextBox)
								.OnTextCommitted_Lambda([this](const FText&, ETextCommit::Type How)
									{ if (How == ETextCommit::OnEnter) AssignChecked(); }) ] ]
						+ SWrapBox::Slot().Padding(0, 0, 6, 4).VAlign(VAlign_Center)
						[
							SNew(SCheckBox)
							.ToolTipText(FText::FromString(TEXT("Keep each category inside the band the community's script templates expect: sectors from 100, capture points from 200, HQs from 301, MCOMs from 400.")))
							.IsChecked_Lambda([this]{ return bBands ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
							.OnCheckStateChanged_Lambda([this](ECheckBoxState S){ bBands = (S == ECheckBoxState::Checked); Rebuild(); })
							[ Label(TEXT("Respect bands")) ]
						]
						+ SWrapBox::Slot().Padding(0, 0, 4, 4)
						[ Btn(TEXT("Assign"), [this]{ AssignChecked(); },
							TEXT("Number the checked rows in the order they were checked. With nothing checked, the editor's own selection is used.")) ]
						+ SWrapBox::Slot().Padding(0, 0, 4, 4)
						[ Btn(TEXT("Select duplicates"), [this]
							{
								const int32 n = BF6Api::SelectDuplicateObjIds();
								Toast(n > 0 ? FString::Printf(TEXT("Selected %d actors with duplicate ids."), n)
								            : FString(TEXT("No duplicate ids, clean.")));
								Rebuild();
							}) ]
						+ SWrapBox::Slot().Padding(0, 0, 4, 4)
						[ Btn(TEXT("Export"), []{ BF6ObjIds::OpenExportWindow(); },
							TEXT("Turn the objects you tick into the TypeScript your mod pastes in, or into a Blocks snippet.")) ]
					]

					// reserve row
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
						[ Label(TEXT("reserve")) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
						[ SNew(SBox).WidthOverride(52.f)
							[ SAssignNew(ResFrom, SEditableTextBox)
								.OnTextCommitted_Lambda([this](const FText&, ETextCommit::Type How)
									{ if (How == ETextCommit::OnEnter) AddReserve(); }) ] ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
						[ Label(TEXT("to")) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
						[ SNew(SBox).WidthOverride(52.f)
							[ SAssignNew(ResTo, SEditableTextBox)
								.OnTextCommitted_Lambda([this](const FText&, ETextCommit::Type How)
									{ if (How == ETextCommit::OnEnter) AddReserve(); }) ] ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
						[ Btn(TEXT("Reserve"), [this]{ AddReserve(); },
							TEXT("Set these numbers aside. Auto-assign steps over them, and the reservation is remembered with this save.")) ]
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
						[ SAssignNew(ResText, STextBlock).Font(FontReg(8)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ MiniBtn(TEXT("clear"), [this]{ Reserves().Reset(); SaveReserves(); Rebuild(); },
							TEXT("Drop every reserved range on this save.")) ]
					]

					// the tree
					+ SVerticalBox::Slot().FillHeight(1)
					[ SNew(SBox).MinDesiredHeight(200.f).MaxDesiredHeight(460.f)
						[ SNew(SScrollBox) + SScrollBox::Slot()[ SAssignNew(List, SVerticalBox) ] ] ]
				]
			]
		];
		// Selecting an object in the viewport can tick its row, which is how a
		// run of ids is built by clicking things in the world rather than
		// hunting for them in a list. Off by default: with it on, every ordinary
		// click in the viewport joins the run.
		SelectionHandle = USelection::SelectionChangedEvent.AddSP(this, &SBF6ObjIdManager::OnEditorSelectionChanged);
		Rebuild();
	}

	virtual ~SBF6ObjIdManager() override
	{
		USelection::SelectionChangedEvent.Remove(SelectionHandle);
	}

	// Esc clears the tick marks, exactly as the addon does.
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry&, const FKeyEvent& Key) override
	{
		if (Key.GetKey() == EKeys::Escape) { Checked.Reset(); Rebuild(); return FReply::Handled(); }
		return FReply::Unhandled();
	}

private:
	TFunction<void()> OnBack;
	TSharedPtr<SVerticalBox> List;
	TSharedPtr<STextBlock>   Status;
	TSharedPtr<STextBlock>   ResText;
	TSharedPtr<SEditableTextBox> StartBox, FilterBox, ResFrom, ResTo;

	TArray<BF6ObjIds::FRow> All;
	TArray<TWeakObjectPtr<AActor>> Checked;      // in the order they were ticked
	TSet<FString> CollapsedCats;
	TWeakObjectPtr<AActor> LastToggled;
	TWeakObjectPtr<AActor> LastClicked;
	double  LastClickTime = 0.0;
	FString Filter;
	bool bConflictsOnly = false;
	bool bBands = true;
	bool bFollowSelection = false;
	FDelegateHandle SelectionHandle;

	void OnEditorSelectionChanged(UObject*)
	{
		if (!bFollowSelection || !GEditor) return;
		USelection* S = GEditor->GetSelectedActors();
		if (!S) return;
		bool bAny = false;
		for (int32 i = 0; i < S->Num(); i++)
			if (AActor* A = Cast<AActor>(S->GetSelectedObject(i)))
				for (const BF6ObjIds::FRow& R : All)
					if (R.Actor.Get() == A) { SetChecked(A, true); bAny = true; break; }
		if (bAny) Rebuild();
	}

	bool IsChecked(AActor* A) const { return A && Checked.Contains(TWeakObjectPtr<AActor>(A)); }

	bool PassesFilter(const BF6ObjIds::FRow& R) const
	{
		if (bConflictsOnly && R.SameIdCount < 2) return false;
		if (Filter.IsEmpty()) return true;
		return R.Name.Contains(Filter) || R.Type.Contains(Filter) || R.Category.Contains(Filter)
			|| FString::FromInt(R.Id) == Filter;
	}

	void SetAllCollapsed(bool bCollapse)
	{
		CollapsedCats.Reset();
		if (bCollapse) for (const BF6ObjIds::FRow& R : All) CollapsedCats.Add(R.Category);
		Rebuild();
	}

	void ToggleCheck(const BF6ObjIds::FRow& R, bool bWantChecked)
	{
		AActor* A = R.Actor.Get();
		if (!A) return;
		const bool bShift = FSlateApplication::Get().GetModifierKeys().IsShiftDown();
		if (bShift && LastToggled.IsValid())
		{
			// A range, inside one category, like the addon: the tick you just
			// made decides whether the run is ticked or cleared.
			int32 From = INDEX_NONE, To = INDEX_NONE;
			for (int32 i = 0; i < All.Num(); i++)
			{
				if (All[i].Actor.Get() == LastToggled.Get()) From = i;
				if (All[i].Actor.Get() == A) To = i;
			}
			if (From != INDEX_NONE && To != INDEX_NONE)
			{
				const FString Cat = All[From].Category;
				const int32 Step = From <= To ? 1 : -1;
				for (int32 i = From; ; i += Step)
				{
					if (All[i].Category == Cat && All[i].Actor.IsValid())
						SetChecked(All[i].Actor.Get(), bWantChecked);
					if (i == To) break;
				}
				LastToggled = A;
				Rebuild();
				return;
			}
		}
		SetChecked(A, bWantChecked);
		LastToggled = A;
		Rebuild();
	}

	void SetChecked(AActor* A, bool bWant)
	{
		const TWeakObjectPtr<AActor> W(A);
		if (bWant) { if (!Checked.Contains(W)) Checked.Add(W); }
		else Checked.Remove(W);
	}

	void CheckCategory(const FString& Cat, bool bWant)
	{
		for (const BF6ObjIds::FRow& R : All)
			if (R.Category == Cat && R.Actor.IsValid() && PassesFilter(R))
				SetChecked(R.Actor.Get(), bWant);
		Rebuild();
	}

	void ClickRow(const BF6ObjIds::FRow& R)
	{
		AActor* A = R.Actor.Get();
		if (!A) return;
		const double Now = FPlatformTime::Seconds();
		const bool bAgain = (LastClicked.Get() == A) && (Now - LastClickTime < 0.5);
		LastClicked = A;
		LastClickTime = Now;
		BF6Api::SelectOnly(A);
		if (bAgain && GEditor) GEditor->MoveViewportCamerasToActor(*A, false);
	}

	// The start number the boxes agree on: what the creator typed, or the next
	// free id if they typed nothing.
	int32 StartValue() const
	{
		const FString S = StartBox.IsValid() ? StartBox->GetText().ToString() : FString();
		return S.IsEmpty() ? 0 : FMath::Max(0, FCString::Atoi(*S));
	}

	void AssignChecked()
	{
		const int32 Start = StartValue();

		// Nothing ticked: this is the plain "number what I have selected" case,
		// which BF6Api::AutoAssignObjIds already owns, prompt and all. Bands are
		// a panel idea, so they only apply to the panel's own list.
		if (Checked.Num() == 0)
		{
			BF6Api::FObjIdAssign R = BF6Api::AutoAssignObjIds(Start, false);
			if (R.Considered == 0) { Toast(TEXT("Tick some rows, or select gameplay objects in the viewport.")); Rebuild(); return; }
			if (R.Assigned == 0 && R.Kept > 0)
			{
				const EAppReturnType::Type Pick = FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(FString::Printf(TEXT(
					"All %d selected objects already have an ObjId.\n\n"
					"Scripts address objects by these ids, so renumbering can break a mod that is already written.\n\n"
					"Renumber them anyway, starting at %d?"), R.Kept, Start)));
				if (Pick != EAppReturnType::Yes) { Toast(TEXT("Left every id as it was.")); return; }
				R = BF6Api::AutoAssignObjIds(Start, true);
				Toast(FString::Printf(TEXT("Renumbered %d ids starting at %d."), R.Assigned, Start));
			}
			else
			{
				Toast(R.Kept > 0
					? FString::Printf(TEXT("Assigned %d id%s. Left %d that already had one."), R.Assigned, R.Assigned == 1 ? TEXT("") : TEXT("s"), R.Kept)
					: FString::Printf(TEXT("Assigned %d id%s."), R.Assigned, R.Assigned == 1 ? TEXT("") : TEXT("s")));
			}
			Rebuild();
			return;
		}

		TArray<TPair<TWeakObjectPtr<AActor>, FString>> Targets;
		for (const TWeakObjectPtr<AActor>& W : Checked)
			for (const BF6ObjIds::FRow& R : All)
				if (R.Actor.Get() == W.Get()) { Targets.Add({ W, R.Category }); break; }

		FAssign R = AssignSequentialTyped(Targets, Start, bBands, false);
		// Checked before the "nothing to do" branch below: an exhausted band
		// also reports zero assigned, and that branch would then ask whether to
		// renumber objects it had just refused to number, which is nonsense.
		if (!R.Exhausted.IsEmpty()) { Toast(R.Exhausted); Rebuild(); return; }
		if (R.Assigned == 0 && R.Kept > 0)
		{
			const EAppReturnType::Type Pick = FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(FString::Printf(TEXT(
				"All %d ticked objects already have an ObjId.\n\n"
				"Scripts address objects by these ids, so renumbering can break a mod that is already written.\n\n"
				"Renumber them anyway, starting at %d?"), R.Kept, Start)));
			if (Pick != EAppReturnType::Yes) { Toast(TEXT("Left every id as it was.")); return; }
			R = AssignSequentialTyped(Targets, Start, bBands, true);
			if (!R.Exhausted.IsEmpty()) { Toast(R.Exhausted); Rebuild(); return; }
		}
		Toast(FString::Printf(TEXT("Assigned %d id%s, left %d as they were."),
			R.Assigned, R.Assigned == 1 ? TEXT("") : TEXT("s"), R.Kept));
		Rebuild();
	}

	// Every id in one category, renumbered from the band base (or from the
	// start box when bands are off). Destructive on purpose, so it asks.
	void RenumberCategory(const FString& Cat)
	{
		TArray<TPair<TWeakObjectPtr<AActor>, FString>> Targets;
		for (const BF6ObjIds::FRow& R : All)
			if (R.Category == Cat && R.Actor.IsValid()) Targets.Add({ R.Actor, Cat });
		if (Targets.Num() == 0) return;

		int32 Base = 0, Span = 0;
		const int32 Start = (bBands && BandOf(Cat, Base, Span)) ? Base : StartValue();
		const EAppReturnType::Type Pick = FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(FString::Printf(TEXT(
			"Renumber all %d objects in %s from %d?\n\n"
			"Scripts address objects by these ids, so any rule already written against them will point somewhere else."),
			Targets.Num(), *Cat, Start)));
		if (Pick != EAppReturnType::Yes) return;

		const FAssign R = AssignSequentialTyped(Targets, Start, bBands, true);
		if (!R.Exhausted.IsEmpty()) { Toast(R.Exhausted); Rebuild(); return; }
		Toast(FString::Printf(TEXT("Renumbered %d %s from %d."), R.Assigned, *Cat.ToLower(), Start));
		Rebuild();
	}

	// One object, moved to the first free number in its category's band.
	void MoveIntoBand(const BF6ObjIds::FRow& R)
	{
		int32 Base = 0, Span = 0;
		if (!BandOf(R.Category, Base, Span)) { Toast(TEXT("That category has no expected band.")); return; }
		TSet<int32> Taken;
		for (const BF6Api::FObjIdRow& Row : BF6Api::GatherObjIds())
			if (Row.Id >= 0 && Row.Actor.Get() != R.Actor.Get()) Taken.Add(Row.Id);
		const int32 Id = FirstFreeInBand(Base, Span, Taken);
		if (Id < 0) { Toast(FString::Printf(TEXT("The %d band is full."), Base)); return; }
		SetOneId(R.Actor.Get(), Id, TEXT("Move ObjId into band"));
		Rebuild();
	}

	void MoveCategoryIntoBand(const FString& Cat)
	{
		int32 Base = 0, Span = 0;
		if (!BandOf(Cat, Base, Span)) { Toast(TEXT("That category has no expected band.")); return; }
		TArray<TPair<TWeakObjectPtr<AActor>, FString>> Targets;
		for (const BF6ObjIds::FRow& R : All)
			if (R.Category == Cat && R.bOutOfBand && R.Actor.IsValid()) Targets.Add({ R.Actor, Cat });
		if (Targets.Num() == 0) { Toast(TEXT("Every id in that category is already in band.")); return; }
		const FAssign R = AssignSequentialTyped(Targets, Base, true, true);
		if (!R.Exhausted.IsEmpty()) { Toast(R.Exhausted); Rebuild(); return; }
		Toast(FString::Printf(TEXT("Moved %d id%s into the %d band."), R.Assigned, R.Assigned == 1 ? TEXT("") : TEXT("s"), Base));
		Rebuild();
	}

	void AddReserve()
	{
		const int32 From = ResFrom.IsValid() ? FCString::Atoi(*ResFrom->GetText().ToString()) : 0;
		const int32 To   = ResTo.IsValid()   ? FCString::Atoi(*ResTo->GetText().ToString())   : 0;
		if (To < From || (From == 0 && To == 0)) { Toast(TEXT("Type a range, low number first.")); return; }
		FReserve E; E.From = From; E.To = To;
		Reserves().Add(E);
		SaveReserves();
		Toast(FString::Printf(TEXT("Reserved %d to %d. Auto-assign will step over it."), From, To));
		Rebuild();
	}

	void CommitId(const BF6ObjIds::FRow& R, const FString& Text)
	{
		const FString T = Text.TrimStartAndEnd();
		if (T.IsEmpty()) return;
		if (!T.IsNumeric() && !(T.StartsWith(TEXT("-")) && T.RightChop(1).IsNumeric()))
		{
			Toast(TEXT("An ObjId is a whole number."));
			Rebuild();
			return;
		}
		const int32 New = FCString::Atoi(*T);
		if (New == R.Id) return;
		SetOneId(R.Actor.Get(), New, TEXT("Change ObjId"));
		Rebuild();
	}

	void Rebuild()
	{
		if (!List.IsValid()) return;
		All = BF6ObjIds::Registry();

		int32 nDup = 0, nUnset = 0, nOut = 0, MaxId = -1;
		for (const BF6ObjIds::FRow& R : All)
		{
			if (R.Id < 0) { nUnset++; continue; }
			if (R.SameIdCount > 1) nDup++;
			if (R.bOutOfBand) nOut++;
			MaxId = FMath::Max(MaxId, R.Id);
		}
		if (Status.IsValid())
			Status->SetText(FText::FromString(FString::Printf(
				TEXT("%d objects     %d conflict%s     %d out of band     %d unassigned"),
				All.Num(), nDup, nDup == 1 ? TEXT("") : TEXT("s"), nOut, nUnset)));
		if (StartBox.IsValid() && StartBox->GetText().IsEmpty())
			StartBox->SetText(FText::FromString(FString::FromInt(MaxId + 1)));
		if (ResText.IsValid())
		{
			FString S;
			for (const FReserve& E : Reserves())
				S += (S.IsEmpty() ? TEXT("") : TEXT("  ")) + FString::Printf(TEXT("%d-%d"), E.From, E.To);
			ResText->SetText(FText::FromString(S.IsEmpty() ? FString(TEXT("nothing reserved")) : S));
		}

		// Drop ticks whose actor is gone, so the checked order never holds a
		// stale pointer across a delete.
		Checked.RemoveAll([](const TWeakObjectPtr<AActor>& W){ return !W.IsValid(); });

		List->ClearChildren();
		FString Cat;
		bool bDrawing = false;
		for (int32 i = 0; i < All.Num(); i++)
		{
			const BF6ObjIds::FRow& R = All[i];
			if (!PassesFilter(R)) continue;
			if (R.Category != Cat)
			{
				Cat = R.Category;
				bDrawing = !CollapsedCats.Contains(Cat);
				AddCategoryHeader(Cat);
			}
			if (bDrawing) AddRow(R);
		}
		if (List->NumSlots() == 0)
			List->AddSlot().AutoHeight().Padding(4, 8)
			[ Label(bConflictsOnly ? TEXT("No duplicate ids on this map.") : TEXT("No gameplay object on this map carries an ObjId yet.")) ];
	}

	void AddCategoryHeader(const FString& Cat)
	{
		int32 Base = 0, Span = 0;
		const bool bBand = BandOf(Cat, Base, Span);
		int32 Count = 0, Out = 0;
		for (const BF6ObjIds::FRow& R : All)
			if (R.Category == Cat) { Count++; if (R.bOutOfBand) Out++; }
		const FString CatCopy = Cat;

		List->AddSlot().AutoHeight().Padding(0, 6, 0, 2)
		[
			SNew(SBorder).BorderImage(InkBrush()).Padding(FMargin(4, 3))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ MiniBtn(CollapsedCats.Contains(Cat) ? TEXT("+") : TEXT("-"), [this, CatCopy]
					{
						if (CollapsedCats.Contains(CatCopy)) CollapsedCats.Remove(CatCopy);
						else CollapsedCats.Add(CatCopy);
						Rebuild();
					}, TEXT("Collapse or expand this category.")) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 8, 0)
				[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity(FSlateColor(BF6Theme::TextBlue))
					.Text(FText::FromString(Cat.ToUpper())) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ Label(FString::Printf(TEXT("%d"), Count), 8) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
				[ SNew(STextBlock).Font(FontReg(8))
					.ColorAndOpacity(FSlateColor(Out > 0 ? BF6Theme::Accent : BF6Theme::TextDim))
					.Text(FText::FromString(bBand
						? FString::Printf(TEXT("band %d-%d%s"), Base, Base + Span - 1,
							Out > 0 ? *FString::Printf(TEXT(", %d outside"), Out) : TEXT(""))
						: FString(TEXT("any id")))) ]
				+ SHorizontalBox::Slot().FillWidth(1) [ SNullWidget::NullWidget ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ MiniBtn(TEXT("all"), [this, CatCopy]{ CheckCategory(CatCopy, true); }, TEXT("Tick every object in this category.")) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ MiniBtn(TEXT("none"), [this, CatCopy]{ CheckCategory(CatCopy, false); }, TEXT("Clear every tick in this category.")) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ SNew(SBox).Visibility(bBand && Out > 0 ? EVisibility::Visible : EVisibility::Collapsed)
					[ MiniBtn(TEXT("to band"), [this, CatCopy]{ MoveCategoryIntoBand(CatCopy); },
						TEXT("Move every out-of-band id in this category into the band.")) ] ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ MiniBtn(TEXT("renumber"), [this, CatCopy]{ RenumberCategory(CatCopy); },
					TEXT("Renumber the whole category in one undoable step.")) ]
			]
		];
	}

	void AddRow(const BF6ObjIds::FRow& R)
	{
		const bool bDup   = R.SameIdCount > 1;
		const bool bUnset = R.Id < 0;
		const BF6ObjIds::FRow RowCopy = R;

		List->AddSlot().AutoHeight().Padding(0, 1)
		[
			SNew(SBorder).BorderImage(bDup ? PanelBrush() : InkBrush()).Padding(FMargin(4, 2))
			[
				SNew(SHorizontalBox)

				// tick, and its order in the run
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this, RowCopy]{ return IsChecked(RowCopy.Actor.Get()) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this, RowCopy](ECheckBoxState S){ ToggleCheck(RowCopy, S == ECheckBoxState::Checked); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2, 0, 4, 0)
				[ SNew(SBox).WidthOverride(18.f)
					[ SNew(STextBlock).Font(FontReg(8)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
						.Text_Lambda([this, RowCopy]
						{
							const int32 i = Checked.IndexOfByKey(RowCopy.Actor);
							return FText::FromString(i == INDEX_NONE ? FString() : FString::Printf(TEXT("%d"), i + 1));
						}) ] ]

				// the id itself, editable in place
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ SNew(SBox).WidthOverride(58.f)
					[ SNew(SEditableTextBox)
						.Font(FontBold(10))
						.Text(FText::FromString(bUnset ? FString() : FString::FromInt(R.Id)))
						.HintText(FText::FromString(TEXT("unset")))
						.ToolTipText(FText::FromString(TEXT("Type a number and press Enter. Ctrl+Z puts the old one back.")))
						.OnTextCommitted_Lambda([this, RowCopy](const FText& T, ETextCommit::Type How)
							{ if (How == ETextCommit::OnEnter || How == ETextCommit::OnUserMovedFocus) CommitId(RowCopy, T.ToString()); }) ] ]

				// band state
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
				[ SNew(SBox).WidthOverride(58.f)
					[ SNew(STextBlock).Font(FontReg(8))
						.ColorAndOpacity(FSlateColor(R.bOutOfBand ? BF6Theme::Accent : BF6Theme::TextDim))
						.Text(FText::FromString(R.BandSpan == 0 ? FString(TEXT("any")) : (R.bOutOfBand ? FString(TEXT("off band")) : FString(TEXT("in band"))))) ] ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ SNew(SBox).Visibility(R.bOutOfBand ? EVisibility::Visible : EVisibility::Collapsed)
					[ MiniBtn(TEXT("fix"), [this, RowCopy]{ MoveIntoBand(RowCopy); },
						TEXT("Give this object the first free id in its category's band.")) ] ]

				// type and name: the click target
				+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(6, 0, 0, 0)
				[
					SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(2, 1)).HAlign(HAlign_Fill)
					.ToolTipText(FText::FromString(TEXT("Click to select this object. Click again to fly the camera to it.")))
					.OnClicked_Lambda([this, RowCopy]{ ClickRow(RowCopy); return FReply::Handled(); })
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontBold(9)).ColorAndOpacity(FSlateColor(BF6Theme::Text))
							.Text(FText::FromString(R.Type)) ]
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(8, 0, 0, 0)
						[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
							.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
							.Text(FText::FromString(R.Name)) ]
					]
				]

				// referenced by, when anything can answer
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
				[ SNew(STextBlock).Font(FontReg(8)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
					.ToolTipText(FText::FromString(TEXT("How many times a script or the blocks workspace addresses this id.")))
					.Text(FText::FromString(R.RefCount < 0 ? FString(TEXT("refs ?")) : FString::Printf(TEXT("refs %d"), R.RefCount))) ]

				// the conflict badge carries the count, as the addon's does
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
				[ SNew(STextBlock).Font(FontBold(8)).ColorAndOpacity(FSlateColor(BF6Theme::Accent))
					.Visibility(bDup ? EVisibility::Visible : EVisibility::Collapsed)
					.Text(FText::FromString(FString::Printf(TEXT("SHARED BY %d"), R.SameIdCount))) ]
			]
		];
	}
};

// ============================================================================
// 5. the export window
// ============================================================================
class SBF6ObjIdExport : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6ObjIdExport) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		GConfig->GetString(kIniSection, TEXT("ExportVar"), VarName, GEditorPerProjectIni);
		GConfig->GetBool(kIniSection, TEXT("ExportBlocks"), bBlocks, GEditorPerProjectIni);
		FString Saved;
		GConfig->GetString(kIniSection, TEXT("ExportChecked"), Saved, GEditorPerProjectIni);
		{
			TArray<FString> P;
			Saved.ParseIntoArray(P, TEXT(","), true);
			for (const FString& S : P) CheckedIds.Add(FCString::Atoi(*S));
		}

		ChildSlot
		[
			SNew(SBorder).BorderImage(InkBrush()).Padding(12.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[ SNew(STextBlock).Font(FontBold(12)).ColorAndOpacity(FSlateColor(BF6Theme::Accent))
					.Text(FText::FromString(TEXT("EXPORT OBJECT IDS"))) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
				[ SNew(STextBlock).AutoWrapText(true).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::Text))
					.Text(FText::FromString(TEXT("Writes code that declares the objects you tick, in the order you ticked them, under the variable name below."))) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
				[ SNew(STextBlock).AutoWrapText(true).Font(FontReg(8)).ColorAndOpacity(FSlateColor(BF6Theme::AccentDim))
					.Text(FText::FromString(TEXT("Objects with no id are hidden. CombatArea, DeployCam and SurroundingCombatArea are greyed out because no script can declare them."))) ]

				// controls
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SNew(SWrapBox).UseAllottedSize(true)
					+ SWrapBox::Slot().Padding(0, 0, 6, 4).VAlign(VAlign_Center)
					[ SNew(SBox).WidthOverride(180.f)
						[ SAssignNew(VarBox, SEditableTextBox)
							.HintText(FText::FromString(TEXT("variable name")))
							.Text(FText::FromString(VarName))
							.OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type){ VarName = T.ToString(); Remember(); }) ] ]
					+ SWrapBox::Slot().Padding(0, 0, 6, 4)
					[ Btn(TEXT("Check all"), [this]{ CheckAll(true); }) ]
					+ SWrapBox::Slot().Padding(0, 0, 6, 4)
					[ Btn(TEXT("Uncheck all"), [this]{ CheckAll(false); }) ]
					+ SWrapBox::Slot().Padding(0, 0, 6, 4).VAlign(VAlign_Center)
					[
						SNew(SCheckBox)
						.ToolTipText(FText::FromString(TEXT("Emit one named object per line for the BF6 Blocks editor instead of a TypeScript array. Concrete ids either way.")))
						.IsChecked_Lambda([this]{ return bBlocks ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this](ECheckBoxState S){ bBlocks = (S == ECheckBoxState::Checked); Remember(); })
						[ Label(TEXT("Blocks snippet")) ]
					]
					+ SWrapBox::Slot().Padding(0, 0, 6, 4)
					[ Btn(TEXT("Convert"), [this]{ Convert(); }) ]
					+ SWrapBox::Slot().Padding(0, 0, 6, 4)
					[ Btn(TEXT("Copy"), [this]{ Copy(); }) ]
				]

				// the list
				+ SVerticalBox::Slot().FillHeight(1).Padding(0, 0, 0, 8)
				[
					SNew(SBorder).BorderImage(PanelBrush()).Padding(6.f)
					[ SNew(SScrollBox) + SScrollBox::Slot()[ SAssignNew(ListBox, SVerticalBox) ] ]
				]

				// the code
				+ SVerticalBox::Slot().FillHeight(1)
				[
					SNew(SBorder).BorderImage(PanelBrush()).Padding(2.f)
					[
						SAssignNew(CodeBox, SMultiLineEditableTextBox)
						.IsReadOnly(true)
						.AlwaysShowScrollbars(true)
						.Font(FontReg(9))
					]
				]
			]
		];
		Populate();
	}

private:
	TSharedPtr<SVerticalBox> ListBox;
	TSharedPtr<SEditableTextBox> VarBox;
	TSharedPtr<SMultiLineEditableTextBox> CodeBox;
	TArray<BF6ObjIds::FRow> Rows;
	TArray<int32> CheckedIds;     // by id, so a refresh keeps the ticks
	FString VarName;
	bool bBlocks = false;

	void Remember()
	{
		FString S;
		for (int32 Id : CheckedIds) S += (S.IsEmpty() ? TEXT("") : TEXT(",")) + FString::FromInt(Id);
		GConfig->SetString(kIniSection, TEXT("ExportChecked"), *S, GEditorPerProjectIni);
		GConfig->SetString(kIniSection, TEXT("ExportVar"), *VarName, GEditorPerProjectIni);
		GConfig->SetBool(kIniSection, TEXT("ExportBlocks"), bBlocks, GEditorPerProjectIni);
	}

	void CheckAll(bool bWant)
	{
		CheckedIds.Reset();
		if (bWant)
			for (const BF6ObjIds::FRow& R : Rows)
				if (!IsExcludedType(R.Type)) CheckedIds.Add(R.Id);
		Remember();
		Populate();
	}

	void Toggle(int32 Id, bool bWant)
	{
		if (bWant) { if (!CheckedIds.Contains(Id)) CheckedIds.Add(Id); }
		else CheckedIds.Remove(Id);
		Remember();
		Populate();
	}

	void Convert()
	{
		if (VarBox.IsValid()) VarName = VarBox->GetText().ToString().TrimStartAndEnd();
		if (VarName.IsEmpty()) { Toast(TEXT("Type a variable name first.")); return; }
		TArray<BF6ObjIds::FRow> Picked;
		for (int32 Id : CheckedIds)
			for (const BF6ObjIds::FRow& R : Rows)
				if (R.Id == Id && !IsExcludedType(R.Type)) { Picked.Add(R); break; }
		if (Picked.Num() == 0) { Toast(TEXT("Tick one or more objects first.")); return; }
		if (CodeBox.IsValid()) CodeBox->SetText(FText::FromString(BF6ObjIds::GenerateCode(VarName, Picked, bBlocks)));
		Remember();
	}

	void Copy()
	{
		const FString S = CodeBox.IsValid() ? CodeBox->GetText().ToString() : FString();
		if (S.TrimStartAndEnd().IsEmpty()) { Toast(TEXT("Press Convert first, there is nothing to copy.")); return; }
		FPlatformApplicationMisc::ClipboardCopy(*S);
		Toast(TEXT("Code copied to the clipboard."));
	}

	void Populate()
	{
		if (!ListBox.IsValid()) return;
		Rows.Reset();
		for (const BF6ObjIds::FRow& R : BF6ObjIds::Registry())
			if (R.Id >= 0) Rows.Add(R);           // -1 is unused, and stays hidden

		ListBox->ClearChildren();
		FString Cat;
		for (const BF6ObjIds::FRow& R : Rows)
		{
			if (R.Category != Cat)
			{
				Cat = R.Category;
				ListBox->AddSlot().AutoHeight().Padding(0, 6, 0, 2)
				[ SNew(STextBlock).Font(FontBold(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextBlue))
					.Text(FText::FromString(Cat.ToUpper())) ];
			}
			const bool bOff = IsExcludedType(R.Type);
			const int32 Id = R.Id;
			const int32 Order = CheckedIds.IndexOfByKey(Id);
			const FString Text = FString::Printf(TEXT("%s%s (ObjId=%d)"),
				Order == INDEX_NONE ? TEXT("") : *FString::Printf(TEXT("[%d] "), Order + 1), *R.Name, Id);

			ListBox->AddSlot().AutoHeight().Padding(4, 1)
			[
				SNew(SCheckBox)
				.IsEnabled(!bOff)
				.ToolTipText(FText::FromString(bOff
					? FString::Printf(TEXT("%s cannot be declared from a script, so it cannot be exported."), *R.Type)
					: R.Type + TEXT("  ->  ") + TsFuncFor(R.Type)))
				.IsChecked_Lambda([this, Id]{ return CheckedIds.Contains(Id) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this, Id](ECheckBoxState S){ Toggle(Id, S == ECheckBoxState::Checked); })
				[ SNew(STextBlock).Font(FontReg(9))
					.ColorAndOpacity(FSlateColor(bOff ? BF6Theme::TextDim : BF6Theme::Text))
					.Text(FText::FromString(Text)) ]
			];
		}
		if (Rows.Num() == 0)
			ListBox->AddSlot().AutoHeight().Padding(4, 8)
			[ Label(TEXT("Nothing on this map has an ObjId yet.")) ];
	}
};

// ============================================================================
// 6. public entry points
// ============================================================================
namespace
{
	TWeakPtr<SWindow> GExportWindow;
	bool GTabRegistered = false;
}

TArray<BF6ObjIds::FRow> BF6ObjIds::Registry()
{
	TArray<FRow> Out;
	TArray<BF6Api::FObjIdRow> Src = BF6Api::GatherObjIds();

	TMap<int32, int32> Count;
	for (const BF6Api::FObjIdRow& R : Src) if (R.Id >= 0) Count.FindOrAdd(R.Id)++;

	TArray<int32> Refs;
	const bool bHaveRefs = (bool)GRefProvider;
	if (bHaveRefs) Refs = GRefProvider();

	for (const BF6Api::FObjIdRow& S : Src)
	{
		FRow R;
		R.Actor = S.Actor;
		R.Name  = S.Name;
		R.Type  = S.Type;
		R.Category = CategoryOf(S.Type);
		R.Id = S.Id;
		R.SameIdCount = S.Id >= 0 ? Count[S.Id] : 0;
		BandOf(R.Category, R.BandBase, R.BandSpan);
		R.bOutOfBand = R.BandSpan > 0 && R.Id >= 0 && (R.Id < R.BandBase || R.Id >= R.BandBase + R.BandSpan);
		if (bHaveRefs && R.Id >= 0)
		{
			int32 n = 0;
			for (int32 Ref : Refs) if (Ref == R.Id) n++;
			R.RefCount = n;
		}
		Out.Add(R);
	}

	Out.Sort([](const FRow& A, const FRow& B)
	{
		const int32 RA = CategoryRank(A.Category), RB = CategoryRank(B.Category);
		if (RA != RB) return RA < RB;
		if (A.Category != B.Category) return A.Category < B.Category;
		if ((A.Id < 0) != (B.Id < 0)) return B.Id < 0;      // unassigned sink last
		if (A.Id != B.Id) return A.Id < B.Id;
		return A.Name < B.Name;
	});
	return Out;
}

FString BF6ObjIds::CategoryFor(const FString& Type) { return CategoryOf(Type); }

bool BF6ObjIds::BandFor(const FString& Category, int32& OutBase, int32& OutSpan)
{
	return BandOf(Category, OutBase, OutSpan);
}

FString BF6ObjIds::StatusLine()
{
	const TArray<FRow> All = Registry();
	int32 nDup = 0, nUnset = 0, nOut = 0;
	for (const FRow& R : All)
	{
		if (R.Id < 0) { nUnset++; continue; }
		if (R.SameIdCount > 1) nDup++;
		if (R.bOutOfBand) nOut++;
	}
	return FString::Printf(TEXT("%d objects with ids, %d in a conflict, %d out of band, %d unassigned."),
		All.Num(), nDup, nOut, nUnset);
}

TSharedRef<SWidget> BF6ObjIds::MakePanel(TFunction<void()> OnBack)
{
	return SNew(SBF6ObjIdManager).OnBack(OnBack);
}

void BF6ObjIds::OpenTab()
{
	if (!GTabRegistered && !FGlobalTabmanager::Get()->HasTabSpawner(kTabId))
	{
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(kTabId,
			FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
			{
				return SNew(SDockTab).TabRole(ETabRole::NomadTab)
					[ SNew(SBorder).BorderImage(InkBrush()).Padding(0)[ BF6ObjIds::MakePanel(nullptr) ] ];
			}))
			.SetDisplayName(FText::FromString(TEXT("Object IDs")))
			.SetTooltipText(FText::FromString(TEXT("Every gameplay object's ObjId, with its conflicts and bands.")))
			.SetMenuType(ETabSpawnerMenuType::Hidden);
		GTabRegistered = true;
	}
	FGlobalTabmanager::Get()->TryInvokeTab(kTabId);
}

void BF6ObjIds::OpenExportWindow()
{
	// Already open: this is a "bring it back" press, so its ticks, its code and
	// its geometry all survive.
	if (TSharedPtr<SWindow> W = GExportWindow.Pin())
	{
		W->BringToFront();
		W->FlashWindow();
		return;
	}

	FVector2D Pos(120.f, 120.f), Size(720.f, 620.f);
	FString Rect;
	if (GConfig && GConfig->GetString(kIniSection, TEXT("ExportRect"), Rect, GEditorPerProjectIni))
	{
		TArray<FString> P;
		Rect.ParseIntoArray(P, TEXT(","), true);
		if (P.Num() == 4)
		{
			Pos  = FVector2D(FCString::Atof(*P[0]), FCString::Atof(*P[1]));
			Size = FVector2D(FMath::Max(480.f, (float)FCString::Atof(*P[2])), FMath::Max(360.f, (float)FCString::Atof(*P[3])));
		}
	}

	TSharedRef<SWindow> Win = SNew(SWindow)
		.Title(FText::FromString(TEXT("Export object ids")))
		.ClientSize(Size)
		.ScreenPosition(Pos)
		.SupportsMaximize(true)
		.SupportsMinimize(true)
		.SizingRule(ESizingRule::UserSized)
		[ SNew(SBF6ObjIdExport) ];

	Win->SetOnWindowClosed(FOnWindowClosed::CreateLambda([](const TSharedRef<SWindow>& W)
	{
		const FVector2D P = W->GetPositionInScreen();
		const FVector2D S = W->GetClientSizeInScreen();
		if (GConfig)
		{
			GConfig->SetString(kIniSection, TEXT("ExportRect"),
				*FString::Printf(TEXT("%.0f,%.0f,%.0f,%.0f"), P.X, P.Y, S.X, S.Y), GEditorPerProjectIni);
			GConfig->Flush(false, GEditorPerProjectIni);
		}
	}));

	GExportWindow = Win;
	FSlateApplication::Get().AddWindow(Win);
}

FString BF6ObjIds::GenerateCode(const FString& VarName, const TArray<FRow>& Rows, bool bBlocks)
{
	if (Rows.Num() == 0) return FString();
	FString Base = VarName.TrimStartAndEnd();
	if (Base.IsEmpty()) Base = TEXT("objects");

	if (bBlocks)
	{
		// One named object per line, then the list under the chosen name. The
		// Blocks editor takes concrete ids, so nothing here is a placeholder.
		FString Out = TEXT("// Object ids for the BF6 Blocks editor. Paste as one snippet.\n");
		FString Names;
		for (const FRow& R : Rows)
		{
			const FString Ident = IdentFor(R.Name, R.Id);
			Out += FString::Printf(TEXT("const %s = %s(%d);\n"), *Ident, *TsFuncFor(R.Type), R.Id);
			Names += (Names.IsEmpty() ? TEXT("") : TEXT(", ")) + Ident;
		}
		Out += FString::Printf(TEXT("const %s = [%s];\n"), *Base, *Names);
		return Out;
	}

	TArray<FString> Lines;
	for (const FRow& R : Rows)
		Lines.Add(FString::Printf(TEXT("\t%s(%d)"), *TsFuncFor(R.Type), R.Id));

	if (Lines.Num() == 1)
		return FString::Printf(TEXT("const %s = %s;"), *Base, *Lines[0].TrimStartAndEnd());
	return FString::Printf(TEXT("const %s = [\n%s\n];"), *Base, *FString::Join(Lines, TEXT(",\n")));
}

void BF6ObjIds::LintDuplicates(TFunction<void(uint8, AActor*, const FString&)> Add)
{
	if (!Add) return;
	TMap<int32, TArray<const FRow*>> ById;
	const TArray<FRow> All = Registry();
	for (const FRow& R : All) if (R.Id >= 0) ById.FindOrAdd(R.Id).Add(&R);

	for (const auto& P : ById)
	{
		if (P.Value.Num() < 2) continue;
		TSet<FString> Types;
		for (const FRow* R : P.Value) Types.Add(R->Type);
		// The shipped maps reuse an id ACROSS types (a DeployCam 1 beside an HQ
		// 1), so cross-type reuse is advice. The same id twice within one type
		// is the real problem: no script can tell those two objects apart.
		const bool bSameType = Types.Num() < P.Value.Num();
		Add(bSameType ? (uint8)0 : (uint8)2, P.Value[0]->Actor.Get(), bSameType
			? FString::Printf(TEXT("ObjId %d is used by %d objects of the same type - scripts can't tell them apart. Fix it in OBJECT IDS."), P.Key, P.Value.Num())
			: FString::Printf(TEXT("ObjId %d is shared by %d objects of different types. The base maps do this too, but unique ids everywhere are safer for scripts."), P.Key, P.Value.Num()));
	}
}

void BF6ObjIds::SetReferencedIdsProvider(TFunction<TArray<int32>()> Provider) { GRefProvider = Provider; }
bool BF6ObjIds::HaveReferencedIds() { return (bool)GRefProvider; }

// ---- console ---------------------------------------------------------------
// Everything the panel does, without the panel: a creator scripting a check, or
// a bug report that needs one line of truth about a map.
namespace
{
	void Cmd_Status()
	{
		UE_LOG(LogTemp, Display, TEXT("%s"), *BF6ObjIds::StatusLine());
		for (const BF6ObjIds::FRow& R : BF6ObjIds::Registry())
			if (R.SameIdCount > 1 || R.bOutOfBand || R.Id < 0)
				UE_LOG(LogTemp, Display, TEXT("  %-6s %-28s %-18s %s%s%s"),
					R.Id < 0 ? TEXT("unset") : *FString::FromInt(R.Id), *R.Name, *R.Type,
					R.SameIdCount > 1 ? TEXT("[shared] ") : TEXT(""),
					R.bOutOfBand ? TEXT("[off band] ") : TEXT(""),
					R.Id < 0 ? TEXT("[unassigned]") : TEXT(""));
	}

	void Cmd_Conflicts()
	{
		const int32 n = BF6Api::SelectDuplicateObjIds();
		UE_LOG(LogTemp, Display, TEXT("Selected %d actors with duplicate ObjIds."), n);
	}

	void Cmd_Assign(const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogTemp, Warning, TEXT("BF6.ObjIds.Assign <start> [category] - category numbers that group, otherwise the editor selection is used."));
			return;
		}
		const int32 Start = FCString::Atoi(*Args[0]);
		if (Args.Num() >= 2)
		{
			FString Cat = Args[1];
			for (int32 i = 2; i < Args.Num(); i++) Cat += TEXT(" ") + Args[i];
			TArray<TPair<TWeakObjectPtr<AActor>, FString>> Targets;
			for (const BF6ObjIds::FRow& R : BF6ObjIds::Registry())
				if (R.Category.Equals(Cat, ESearchCase::IgnoreCase) && R.Actor.IsValid())
					Targets.Add({ R.Actor, R.Category });
			if (Targets.Num() == 0) { UE_LOG(LogTemp, Warning, TEXT("No objects in category '%s'."), *Cat); return; }
			const FAssign R = AssignSequentialTyped(Targets, Start, true, false);
			if (!R.Exhausted.IsEmpty()) { UE_LOG(LogTemp, Warning, TEXT("%s"), *R.Exhausted); return; }
			UE_LOG(LogTemp, Display, TEXT("Assigned %d, kept %d, of %d in '%s'."), R.Assigned, R.Kept, R.Considered, *Cat);
			return;
		}
		const BF6Api::FObjIdAssign R = BF6Api::AutoAssignObjIds(Start, false);
		UE_LOG(LogTemp, Display, TEXT("Assigned %d, kept %d, of %d selected."), R.Assigned, R.Kept, R.Considered);
	}

	void Cmd_Export(const TArray<FString>& Args)
	{
		const FString Var = Args.Num() > 0 ? Args[0] : FString(TEXT("objects"));
		TArray<BF6ObjIds::FRow> Rows;
		for (const BF6ObjIds::FRow& R : BF6ObjIds::Registry())
			if (R.Id >= 0 && !IsExcludedType(R.Type)) Rows.Add(R);
		const FString Code = BF6ObjIds::GenerateCode(Var, Rows, false);
		UE_LOG(LogTemp, Display, TEXT("\n%s"), *Code);
	}

	FAutoConsoleCommand GCmdStatus(TEXT("BF6.ObjIds"),
		TEXT("Every ObjId on the open map, with conflicts, out-of-band ids and unassigned objects."),
		FConsoleCommandDelegate::CreateStatic(&Cmd_Status));

	FAutoConsoleCommand GCmdConflicts(TEXT("BF6.ObjIds.Conflicts"),
		TEXT("Select every actor whose ObjId is shared with another."),
		FConsoleCommandDelegate::CreateStatic(&Cmd_Conflicts));

	FAutoConsoleCommand GCmdAssign(TEXT("BF6.ObjIds.Assign"),
		TEXT("BF6.ObjIds.Assign <start> [category] - fill blank ObjIds from <start>, respecting bands."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Cmd_Assign));

	FAutoConsoleCommand GCmdExport(TEXT("BF6.ObjIds.Export"),
		TEXT("BF6.ObjIds.Export <var> - print the TypeScript for every object with an id."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Cmd_Export));
}
