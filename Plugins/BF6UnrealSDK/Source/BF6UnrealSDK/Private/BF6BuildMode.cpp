#include "BF6BuildMode.h"
#include "BF6Theme.h"
#include "BF6MapManifest.h"
#include "SBF6PreviewViewport.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/IMenu.h"
#include "InputCoreTypes.h"
#include "Widgets/SWindow.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Containers/Ticker.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Misc/MessageDialog.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateColor.h"
#include "Styling/SlateTypes.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"

#include "LevelEditor.h"
#include "SLevelViewport.h"

// ============================================================================
// Build Mode implementation. All UI here consumes BF6Api (engine/session logic
// in BF6UnrealSDK.cpp) so this file stays free of engine internals.
// ============================================================================

namespace
{
	// --- shared font helpers ---
	FSlateFontInfo FontBold(int32 Size)   { return FCoreStyle::GetDefaultFontStyle("Bold", Size); }
	FSlateFontInfo FontReg(int32 Size)    { return FCoreStyle::GetDefaultFontStyle("Regular", Size); }

	// --- solid-colour brushes (Slate wants a brush, not a colour, for fills) ---
	const FSlateBrush* PanelBrush()      { static FSlateColorBrush B(BF6Theme::Panel);      return &B; }
	const FSlateBrush* InkBrush()        { static FSlateColorBrush B(BF6Theme::Ink);        return &B; }
	const FSlateBrush* PanelLightBrush() { static FSlateColorBrush B(BF6Theme::PanelLight); return &B; }
	const FSlateBrush* AccentBrush()     { static FSlateColorBrush B(BF6Theme::Accent);     return &B; }
	const FSlateBrush* DimBrush()        { static FSlateColorBrush B(FLinearColor(0.f,0.f,0.f,0.35f)); return &B; }

	// ==== the Portal kit: styles matching the real portal.battlefield.com ====
	// Ghost button: near-transparent fill, hairline border; hover = white border
	// + brightened fill (the site's secondary button).
	const FButtonStyle& GhostButtonStyle()
	{
		static FButtonStyle S = FButtonStyle()
			.SetNormal (FSlateRoundedBoxBrush(BF6Theme::PanelLight, 0.f, BF6Theme::Line,        1.f))
			.SetHovered(FSlateRoundedBoxBrush(FLinearColor(FColor(0x24,0x28,0x2B)), 0.f, FLinearColor::White,   1.f))
			.SetPressed(FSlateRoundedBoxBrush(FLinearColor(FColor(0x2E,0x33,0x37)), 0.f, FLinearColor::White,   1.f))
			.SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0, 1, 0, -1));
		return S;
	}
	// Primary button: solid white fill, dark text (the site's main CTA).
	const FButtonStyle& PrimaryButtonStyle()
	{
		static FButtonStyle S = FButtonStyle()
			.SetNormal (FSlateRoundedBoxBrush(FLinearColor(FColor(0xE8,0xEC,0xEF)), 0.f))
			.SetHovered(FSlateRoundedBoxBrush(FLinearColor::White, 0.f))
			.SetPressed(FSlateRoundedBoxBrush(FLinearColor(FColor(0xC0,0xC8,0xCE)), 0.f))
			.SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0, 1, 0, -1));
		return S;
	}
	// Map tile: hairline frame normally, bright white frame on hover (the site's
	// choose-maps tile hover).
	const FButtonStyle& CardButtonStyle()
	{
		static FButtonStyle S = FButtonStyle()
			.SetNormal (FSlateRoundedBoxBrush(BF6Theme::Panel, 0.f, BF6Theme::Line,      1.f))
			.SetHovered(FSlateRoundedBoxBrush(BF6Theme::Panel, 0.f, FLinearColor::White, 2.f))
			.SetPressed(FSlateRoundedBoxBrush(BF6Theme::Panel, 0.f, BF6Theme::Accent,    2.f))
			.SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0));
		return S;
	}

	// Ghost button with an uppercase label - the standard control everywhere.
	TSharedRef<SWidget> MakeToolButton(const FString& Label, TFunction<void()> OnClick)
	{
		return SNew(SButton).ButtonStyle(&GhostButtonStyle()).ContentPadding(FMargin(16, 8))
			.OnClicked_Lambda([OnClick]{ if (OnClick) OnClick(); return FReply::Handled(); })
			[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(Label.ToUpper())) ];
	}
	// Primary (white) button with dark uppercase label - the main action.
	TSharedRef<SWidget> MakePrimaryButton(const FString& Label, TFunction<void()> OnClick)
	{
		return SNew(SButton).ButtonStyle(&PrimaryButtonStyle()).ContentPadding(FMargin(18, 8))
			.OnClicked_Lambda([OnClick]{ if (OnClick) OnClick(); return FReply::Handled(); })
			[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity(FSlateColor(BF6Theme::Ink)).Text(FText::FromString(Label.ToUpper())) ];
	}

	// Leaving an active session wipes the world (open/import) - never silently.
	bool ConfirmLeaveSession()
	{
		if (!BF6Api::IsEditing()) return true;
		const EAppReturnType::Type R = FMessageDialog::Open(EAppMsgType::YesNoCancel, FText::FromString(FString::Printf(
			TEXT("Save changes to '%s' before leaving?\n\nYes = save and continue. No = discard changes. Cancel = stay."),
			*BF6Api::CurrentSave())));
		if (R == EAppReturnType::Cancel) return false;
		if (R == EAppReturnType::Yes) BF6Api::SaveCurrent();
		return true;
	}

	const FBF6MapCard* FindMapCard(const FString& Level)
	{
		for (int i = 0; i < GBF6MapCardCount; i++)
			if (Level == GBF6MapCards[i].Code) return &GBF6MapCards[i];
		return nullptr;
	}

	const FSlateBrush* LineBrush() { static FSlateColorBrush B(BF6Theme::Line); return &B; }

	// Pie menu brushes: oval capsule pills with white outlines (corner radius =
	// half the pill height), plus a circular hub.
	const FSlateBrush* PiePill()    { static FSlateRoundedBoxBrush B(FLinearColor(BF6Theme::Panel.R, BF6Theme::Panel.G, BF6Theme::Panel.B, 0.94f), 23.f, FLinearColor::White, 1.f); return &B; }
	const FSlateBrush* PiePillHot() { static FSlateRoundedBoxBrush B(BF6Theme::Accent, 23.f, FLinearColor::White, 2.f); return &B; }
	const FSlateBrush* PieHub()     { static FSlateRoundedBoxBrush B(FLinearColor(BF6Theme::Ink.R, BF6Theme::Ink.G, BF6Theme::Ink.B, 0.94f), 60.f, FLinearColor::White, 1.f); return &B; }
	const FSlateBrush* PieHubHot()  { static FSlateRoundedBoxBrush B(BF6Theme::Accent, 60.f, FLinearColor::White, 2.f); return &B; }

	// Section header like the site's "AVAILABLE MAPS": uppercase label + hairline.
	TSharedRef<SWidget> MakeSectionHeader(const FString& Label)
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[ SNew(STextBlock).Font(FontBold(12)).ColorAndOpacity(FSlateColor(BF6Theme::TextBlue)).Text(FText::FromString(Label.ToUpper())) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 5, 0, 0)
			[ SNew(SBox).HeightOverride(1.f)[ SNew(SBorder).BorderImage(LineBrush()).Padding(0) ] ];
	}

	// The site's tile badges: an orange square with a black glyph. Size letter
	// for every map; a price tag on maps that need the full game.
	TSharedRef<SWidget> MakeBadge(const FString& Glyph)
	{
		return SNew(SBox).WidthOverride(22.f).HeightOverride(22.f)
			[
				SNew(SBorder).BorderImage(AccentBrush()).HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(0)
				[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(FLinearColor::Black)).Text(FText::FromString(Glyph)) ]
			];
	}
}

// ---------------------------------------------------------------------------
// Category popup: a small searchable box of the placeables in one category.
// Double-click places the object at the remembered right-click world position.
// ---------------------------------------------------------------------------
class SBF6CategoryPopup : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6CategoryPopup) {}
		SLATE_ARGUMENT(FString, Category)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Category = InArgs._Category;
		Rebuild();

		ChildSlot
		[
			SNew(SBox).WidthOverride(560.f).HeightOverride(400.f)
			[
				SNew(SBorder).BorderImage(PanelBrush()).Padding(10.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontBold(13)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(Category.ToUpper())) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("hover=preview  dbl-click=place"))) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)
					[
						SNew(SSearchBox)
						.HintText(FText::FromString(TEXT("Search this category...")))
						.OnTextChanged(this, &SBF6CategoryPopup::OnSearch)
					]
					+ SVerticalBox::Slot().FillHeight(1)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(0.55f).Padding(0,0,8,0)
						[
							SAssignNew(List, SListView<TSharedPtr<BF6Api::FPlaceableInfo>>)
							.ListItemsSource(&Items).SelectionMode(ESelectionMode::Single)
							.OnGenerateRow(this, &SBF6CategoryPopup::OnGenerateRow)
							.OnSelectionChanged(this, &SBF6CategoryPopup::OnSelChanged)
							.OnMouseButtonDoubleClick(this, &SBF6CategoryPopup::OnActivate)
						]
						// hover preview: the SDK's low-poly model, live 3D
						+ SHorizontalBox::Slot().FillWidth(0.45f)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().FillHeight(1)
							[ SNew(SBorder).BorderImage(InkBrush()).Padding(2.f)[ SAssignNew(Preview, SBF6PreviewViewport) ] ]
							+ SVerticalBox::Slot().AutoHeight().Padding(0,4,0,0)
							[ SNew(STextBlock).Font(FontReg(10)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text_Lambda([this]{ return FText::FromString(PreviewName); }) ]
						]
					]
				]
			]
		];
	}

private:
	FString Category, Query, PreviewName;
	TArray<TSharedPtr<BF6Api::FPlaceableInfo>> Items;
	TSharedPtr<SListView<TSharedPtr<BF6Api::FPlaceableInfo>>> List;
	TSharedPtr<SBF6PreviewViewport> Preview;

	void ShowPreviewFor(TSharedPtr<BF6Api::FPlaceableInfo> Item)
	{
		if (!Item.IsValid() || !Preview.IsValid()) return;
		if (PreviewName == Item->Type) return;   // already showing it
		PreviewName = Item->Type;
		Preview->ShowModel(Item->Mesh.IsEmpty() ? Item->Type : Item->Mesh);
	}

	void OnSelChanged(TSharedPtr<BF6Api::FPlaceableInfo> Item, ESelectInfo::Type) { ShowPreviewFor(Item); }

	void Rebuild()
	{
		Items.Reset();
		for (const BF6Api::FPlaceableInfo& P : BF6Api::PlaceablesIn(Category, Query))
			Items.Add(MakeShared<BF6Api::FPlaceableInfo>(P));
	}

	void OnSearch(const FText& T)
	{
		Query = T.ToString();
		Rebuild();
		if (List.IsValid()) List->RequestListRefresh();
	}

	TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<BF6Api::FPlaceableInfo> Item, const TSharedRef<STableViewBase>& Owner)
	{
		TSharedRef<STableRow<TSharedPtr<BF6Api::FPlaceableInfo>>> Row =
			SNew(STableRow<TSharedPtr<BF6Api::FPlaceableInfo>>, Owner).Padding(2.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
				[ SNew(STextBlock).Font(FontReg(11)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(Item->Type)) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6,0,0,0)
				[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(Item->PhysicsCost > 0 ? BF6Theme::AccentDim : BF6Theme::TextDim)).Text(FText::FromString(FString::Printf(TEXT("%d"), Item->PhysicsCost))) ]
			];
		// hovering a row shows its SDK low-poly model in the preview pane
		Row->SetOnMouseEnter(FNoReplyPointerEventHandler::CreateLambda(
			[this, Item](const FGeometry&, const FPointerEvent&){ ShowPreviewFor(Item); }));
		return Row;
	}

	void OnActivate(TSharedPtr<BF6Api::FPlaceableInfo> Item)
	{
		if (!Item.IsValid()) return;
		// Place at the remembered right-click spot (set when the radial opened).
		extern FVector GBF6PendingWorld;
		BF6Api::PlaceType(Item->Type, GBF6PendingWorld);
		BF6Api::HideTransientMenus();
	}
};

// ---------------------------------------------------------------------------
// Directional pie menu (Blender-style). Fills the viewport (hit-test invisible -
// the input handler drives it). Wedges ring the centre; the highlighted wedge is
// whichever the mouse points toward. Centre (deadzone) = no selection = cancel.
// ---------------------------------------------------------------------------
class SBF6PieMenu : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6PieMenu) {}
		SLATE_ARGUMENT(TArray<FString>, Items)   // custom labels; empty = place categories
		SLATE_ARGUMENT(TArray<FString>, Subs)    // small sublabels, aligned with Items
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		SetVisibility(EVisibility::HitTestInvisible);   // the input handler drives it
		Cats = InArgs._Items;
		Subs = InArgs._Subs;
		if (Cats.Num() == 0)
		{
			Cats = BF6Api::Categories();
			Subs.Reset();
			for (const FString& C : Cats) Subs.Add(FString::Printf(TEXT("%d"), BF6Api::CategoryCount(C)));
		}
		Subs.SetNum(Cats.Num());
		const int32 N = FMath::Max(1, Cats.Num());
		// Oval pills: size the ring so even horizontally-adjacent neighbours near
		// the top/bottom of the wheel can never overlap (worst case needs the arc
		// chord to exceed the pill WIDTH plus a gap).
		const float PillW = 128.f, PillH = 46.f;
		const float Need = PillW + 14.f;
		const float HalfStep = PI / (float)N;
		const float R = FMath::Max(250.f, Need / (2.f * FMath::Sin(HalfStep)));

		TSharedRef<SConstraintCanvas> Canvas = SNew(SConstraintCanvas);

		// dim the viewport behind the pie
		Canvas->AddSlot().Anchors(FAnchors(0.f, 0.f, 1.f, 1.f)).Offset(FMargin(0))
			[ SNew(SBorder).BorderImage(DimBrush()) ];

		// centre hub - a white-outlined circle showing the pick, or CANCEL
		Canvas->AddSlot().Anchors(FAnchors(0.5f, 0.5f)).Alignment(FVector2D(0.5f, 0.5f)).AutoSize(true)
			[
				SNew(SBox).WidthOverride(120.f).HeightOverride(120.f)
				[
					SNew(SBorder).BorderImage_Lambda([this]{ return Highlighted < 0 ? PieHub() : PieHubHot(); })
					.HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(10.f)
					[ SNew(STextBlock).Font(FontBold(11)).Justification(ETextJustify::Center).AutoWrapText(true)
						.ColorAndOpacity_Lambda([this]{ return FSlateColor(Highlighted < 0 ? BF6Theme::TextDim : BF6Theme::Ink); })
						.Text_Lambda([this]{ return FText::FromString(Highlighted < 0 ? FString(TEXT("CANCEL")) : (Cats.IsValidIndex(Highlighted) ? Cats[Highlighted].ToUpper() : FString())); }) ]
				]
			];

		for (int32 i = 0; i < Cats.Num(); i++)
		{
			const float Ang = (-90.f + (360.f / N) * i) * PI / 180.f;
			const float X = R * FMath::Cos(Ang);
			const float Y = R * FMath::Sin(Ang);
			const FString Cat = Cats[i];
			const FString Sub = Subs[i];
			const int32 Idx = i;

			Canvas->AddSlot()
				.Anchors(FAnchors(0.5f, 0.5f)).Offset(FMargin(X, Y, 0.f, 0.f)).Alignment(FVector2D(0.5f, 0.5f)).AutoSize(true)
				[
					SNew(SBox).WidthOverride(PillW).HeightOverride(PillH)
					[
						SNew(SBorder).BorderImage_Lambda([this, Idx]{ return Highlighted == Idx ? PiePillHot() : PiePill(); })
						.HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(FMargin(12, 4))
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[ SNew(STextBlock).Font(FontBold(9))
								.ColorAndOpacity_Lambda([this, Idx]{ return FSlateColor(Highlighted == Idx ? BF6Theme::Ink : BF6Theme::Text); })
								.Text(FText::FromString(Cat.ToUpper())) ]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
							[ SNew(STextBlock).Font(FontReg(8))
								.Visibility(Sub.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
								.ColorAndOpacity_Lambda([this, Idx]{ return FSlateColor(Highlighted == Idx ? BF6Theme::Ink : BF6Theme::TextDim); })
								.Text(FText::FromString(Sub)) ]
						]
					]
				];
		}

		ChildSlot [ Canvas ];
	}

	void SetHighlighted(int32 i)          { Highlighted = i; }
	int32 GetHighlighted() const          { return Highlighted; }
	const TArray<FString>& Categories() const { return Cats; }

private:
	TArray<FString> Cats;
	TArray<FString> Subs;
	int32 Highlighted = -1;
};

// ---------------------------------------------------------------------------
// Attribute editor popup: opened from the context radial when an object's
// property is picked. Bools get TRUE/FALSE buttons; everything else a text box.
// ---------------------------------------------------------------------------
class SBF6PropEditPopup : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6PropEditPopup) : _TargetActor(nullptr) {}
		SLATE_ARGUMENT(AActor*, TargetActor)
		SLATE_ARGUMENT(BF6Api::FPropDef, Def)
		SLATE_ARGUMENT(FString, TypeName)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Target = InArgs._TargetActor;
		Def = InArgs._Def;
		const FString Current = BF6Api::GetActorProp(InArgs._TargetActor, Def.Name, Def.Default);
		const bool bBool = Def.Type == TEXT("bool");

		TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
		Box->AddSlot().AutoHeight().Padding(0, 0, 0, 2)
		[ SNew(STextBlock).Font(FontBold(12)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(FString::Printf(TEXT("%s  -  %s"), *InArgs._TypeName.ToUpper(), *Def.Name.ToUpper()))) ];
		Box->AddSlot().AutoHeight().Padding(0, 0, 0, 10)
		[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(FString::Printf(TEXT("%s   default: %s"), *Def.Type, Def.Default.IsEmpty() ? TEXT("(none)") : *Def.Default))) ];

		if (bBool)
		{
			const bool bCur = Current.Equals(TEXT("true"), ESearchCase::IgnoreCase);
			Box->AddSlot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[ bCur ? MakePrimaryButton(TEXT("True"),  [this]{ Apply(TEXT("true"));  }) : MakeToolButton(TEXT("True"),  [this]{ Apply(TEXT("true"));  }) ]
				+ SHorizontalBox::Slot().AutoWidth()
				[ !bCur ? MakePrimaryButton(TEXT("False"), [this]{ Apply(TEXT("false")); }) : MakeToolButton(TEXT("False"), [this]{ Apply(TEXT("false")); }) ]
			];
		}
		else if (Def.Type == TEXT("selection") && Def.Options.Num() > 0)
		{
			// the same dropdown the Godot SDK shows for this field
			OptSource = MakeShared<TArray<TSharedPtr<FString>>>();
			for (const FString& O : Def.Options) OptSource->Add(MakeShared<FString>(O));
			Box->AddSlot().AutoHeight()
			[
				SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(OptSource.Get())
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> In){ return SNew(STextBlock).Text(FText::FromString(In.IsValid() ? *In : FString())); })
				.OnSelectionChanged_Lambda([this](TSharedPtr<FString> In, ESelectInfo::Type){ if (In.IsValid()) Apply(*In); })
				[ SNew(STextBlock).Font(FontReg(11)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(Current.IsEmpty() ? FString(TEXT("choose...")) : Current)) ]
			];
		}
		else
		{
			Box->AddSlot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SAssignNew(ValueBox, SEditableTextBox).Text(FText::FromString(Current))
				.OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type C){ if (C == ETextCommit::OnEnter) Apply(T.ToString()); })
			];
			Box->AddSlot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[ MakePrimaryButton(TEXT("Apply"), [this]{ if (ValueBox.IsValid()) Apply(ValueBox->GetText().ToString()); }) ]
				+ SHorizontalBox::Slot().AutoWidth()
				[ MakeToolButton(TEXT("Cancel"), []{ BF6Api::HideTransientMenus(); }) ]
			];
		}

		ChildSlot
		[
			SNew(SBox).WidthOverride(380.f)
			[ SNew(SBorder).BorderImage(PanelBrush()).Padding(14.f)[ Box ] ]
		];
	}

private:
	TWeakObjectPtr<AActor> Target;
	BF6Api::FPropDef Def;
	TSharedPtr<SEditableTextBox> ValueBox;
	TSharedPtr<TArray<TSharedPtr<FString>>> OptSource;

	void Apply(const FString& Value)
	{
		if (AActor* A = Target.Get()) BF6Api::SetActorProp(A, Def.Name, Value.TrimStartAndEnd());
		BF6Api::HideTransientMenus();
	}
};

// ---------------------------------------------------------------------------
// Build overlay: the always-on viewport chrome. Budget bar (top, hideable) and
// import/export (bottom-right). Ticks to keep the budget live as you build.
// ---------------------------------------------------------------------------
class SBF6BuildOverlay : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6BuildOverlay) {}
		SLATE_EVENT(FSimpleDelegate, OnChooseMap)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		OnChooseMap = InArgs._OnChooseMap;
		SetCanTick(true);
		// The HUD spans the whole viewport: empty space must pass clicks through
		// to the level viewport below, so only the actual bars/buttons hit-test.
		SetVisibility(EVisibility::SelfHitTestInvisible);
		ChildSlot
		[
			SNew(SConstraintCanvas).Visibility(EVisibility::SelfHitTestInvisible)

			// --- budget bar (top, full width) ---
			+ SConstraintCanvas::Slot()
				.Anchors(FAnchors(0.f, 0.f, 1.f, 0.f)).Offset(FMargin(0.f, 0.f, 0.f, 40.f)).Alignment(FVector2D(0.f, 0.f))
				[
					SNew(SBox).Visibility_Lambda([this]{ return bBarHidden ? EVisibility::Collapsed : EVisibility::Visible; })
					[
						SNew(SBorder).BorderImage(InkBrush()).Padding(FMargin(10.f, 5.f))
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,10,0)
							[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("PORTAL BUDGET"))) ]
							+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
							[
								SNew(SOverlay)
								+ SOverlay::Slot()
								[ SNew(SProgressBar).Percent_Lambda([this]{ return BF6Api::BudgetFrac(); }).FillColorAndOpacity_Lambda([this]{ return FSlateColor(BF6Api::BudgetColor()); }) ]
								+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
								[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(FLinearColor::White)).Text_Lambda([this]{ return BF6Api::BudgetText(); }) ]
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10,0,0,0)
							[
								SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(6,2))
								.OnClicked_Lambda([this]{ bBarHidden = true; return FReply::Handled(); })
								[ SNew(STextBlock).Font(FontReg(10)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("hide"))) ]
							]
						]
					]
				]

			// --- collapsed budget pill (top-left) when hidden ---
			+ SConstraintCanvas::Slot()
				.Anchors(FAnchors(0.f, 0.f)).Offset(FMargin(10.f, 8.f, 0.f, 0.f)).Alignment(FVector2D(0.f, 0.f)).AutoSize(true)
				[
					SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(0)
					.Visibility_Lambda([this]{ return bBarHidden ? EVisibility::Visible : EVisibility::Collapsed; })
					.OnClicked_Lambda([this]{ bBarHidden = false; return FReply::Handled(); })
					[
						SNew(SBorder).BorderImage(InkBrush()).Padding(FMargin(8,4))
						[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity_Lambda([this]{ return FSlateColor(BF6Api::BudgetColor()); }).Text_Lambda([this]{ return FText::FromString(TEXT("show budget")); }) ]
					]
				]

			// --- bottom-right: name+create (base) or save (editing), plus export ---
			+ SConstraintCanvas::Slot()
				.Anchors(FAnchors(1.f, 1.f)).Offset(FMargin(0.f, 0.f, 14.f, 14.f)).Alignment(FVector2D(1.f, 1.f)).AutoSize(true)
				[
					SNew(SBorder).BorderImage(PanelBrush()).Padding(8.f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,6,0)
						[
							SNew(SBox).WidthOverride(190.f)
							.Visibility_Lambda([]{ return BF6Api::IsEditing() ? EVisibility::Collapsed : EVisibility::Visible; })
							[ SAssignNew(NameBox, SEditableTextBox).HintText(FText::FromString(TEXT("custom map name..."))) ]
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)
						[
							SNew(SBox).Visibility_Lambda([]{ return BF6Api::IsEditing() ? EVisibility::Collapsed : EVisibility::Visible; })
							[ MakePrimaryButton(TEXT("Create"), [this]{ BF6Api::CreateCustom(NameBox.IsValid() ? NameBox->GetText().ToString() : FString()); }) ]
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)
						[
							SNew(SBox).Visibility_Lambda([]{ return BF6Api::IsEditing() ? EVisibility::Visible : EVisibility::Collapsed; })
							[ MakePrimaryButton(TEXT("Save"), []{ BF6Api::SaveCurrent(); }) ]
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[ MakeToolButton(TEXT("Export"), []{ BF6Api::ExportSpatial(); }) ]
					]
				]

			// --- bottom-left: back to the map selector + current map label ---
			+ SConstraintCanvas::Slot()
				.Anchors(FAnchors(0.f, 1.f)).Offset(FMargin(14.f, 0.f, 0.f, 14.f)).Alignment(FVector2D(0.f, 1.f)).AutoSize(true)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)
					[ MakeToolButton(TEXT("< Maps"), [this]{ OnChooseMap.ExecuteIfBound(); }) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SBorder).BorderImage(InkBrush()).Padding(FMargin(8,5))
						[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text_Lambda([]{ return FText::FromString(BF6Api::CurrentSave().IsEmpty() ? BF6Api::DisplayName(BF6Api::CurrentLevel()) + TEXT("  (base)") : BF6Api::DisplayName(BF6Api::CurrentLevel()) + TEXT("  /  ") + BF6Api::CurrentSave()); }) ]
					]
				]

		];
	}

	virtual void Tick(const FGeometry& G, const double T, const float D) override
	{
		SCompoundWidget::Tick(G, T, D);
		BF6Api::TickZoneAutoEdit();   // selecting a zone starts point editing, like Godot
		BF6Api::TickVolumeEdit();     // live zone-wall rebuild while handles are dragged
		if (T - LastCalc > 0.25) { LastCalc = T; BF6Api::RecomputeBudget(); }
		// the tool's own autosave: closing the editor can never cost more than
		// a minute of work (session files are tiny)
		if (BF6Api::IsEditing() && T - LastAutoSave > 60.0)
		{
			LastAutoSave = T;
			BF6Api::SaveCurrent(true);
		}
	}

private:
	bool bBarHidden = false;
	double LastCalc = 0.0;
	double LastAutoSave = 0.0;
	TSharedPtr<SEditableTextBox> NameBox;
	FSimpleDelegate OnChooseMap;
};

// ---------------------------------------------------------------------------
// Map selector - the tool's first screen. Embedded in the dockable panel, so it
// fills the editor and resizes with it. Reports the chosen map via OnOpen.
// ---------------------------------------------------------------------------
class SBF6MapSelector : public SCompoundWidget
{
public:
	DECLARE_DELEGATE_TwoParams(FOnOpen, FString /*Level*/, FString /*Save*/);

	SLATE_BEGIN_ARGS(SBF6MapSelector) {}
		SLATE_EVENT(FOnOpen, OnOpen)
		SLATE_EVENT(FSimpleDelegate, OnImport)
		SLATE_EVENT(FSimpleDelegate, OnSdkSetup)
		SLATE_EVENT(FSimpleDelegate, OnReturn)   // back to the active build session
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		OnOpen = InArgs._OnOpen;
		OnImport = InArgs._OnImport;
		OnSdkSetup = InArgs._OnSdkSetup;
		OnReturn = InArgs._OnReturn;

		// Two sections instead of a price badge: full-game maps, then the free
		// RedSec maps (a $ on the tile read like the PLUGIN costs money).
		TSharedRef<SWrapBox> GridPaid = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(12, 12));
		TSharedRef<SWrapBox> GridFree = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(12, 12));
		int32 MapCount = 0;
		for (const FString& Level : BF6Api::AllLevels())
		{
			const FBF6MapCard* Info = FindMapCard(Level);
			const bool bPaid = !Info || Info->bPaid;   // unknown new maps: assume full game
			(bPaid ? GridPaid : GridFree)->AddSlot()[ MakeCard(Level) ];
			MapCount++;
		}

		// Styled after the real Portal choose-maps page: near-black ground, small
		// accent eyebrow, big title, widescreen key-art tiles.
		ChildSlot
		[
			SNew(SBorder).BorderImage(InkBrush()).Padding(0)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(32, 24, 32, 0))
				[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(TEXT("B F 6   U N R E A L   S D K"))) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(32, 2, 32, 4))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
					[ SNew(STextBlock).Font(FontBold(30)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(TEXT("CHOOSE MAPS"))) ]
					+ SHorizontalBox::Slot().FillWidth(1)[ SNew(SSpacer) ]
					// back to the active build session (ESC does the same)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(0,0,8,0)
					[
						SNew(SBox).Visibility(BF6Api::CurrentLevel().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
						[ MakePrimaryButton(TEXT("Return to build"), [this]{ OnReturn.ExecuteIfBound(); }) ]
					]
					// Import lives on this screen: it detects the map from the file
					// and opens straight into build mode named after the file.
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(0,0,8,0)
					[ MakeToolButton(TEXT("SDK Setup"), [this]{ OnSdkSetup.ExecuteIfBound(); }) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(0,0,8,0)
					[ MakeToolButton(TEXT("Import .spatial.json"), [this]{ OnImport.ExecuteIfBound(); }) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
					[ MakeToolButton(FString::Printf(TEXT("v%s - Check for updates"), *BF6Api::PluginVersion()), []{ BF6Api::CheckForUpdates(true); }) ]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(32, 0, 32, 18))
				[ SNew(STextBlock).Font(FontReg(11)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(FString::Printf(TEXT("%d maps  -  click a map to open its base, or resume a saved project below it."), MapCount))) ]
				+ SVerticalBox::Slot().FillHeight(1).Padding(FMargin(28, 0, 28, 24))
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot().Padding(FMargin(2, 0, 2, 10))[ MakeSectionHeader(TEXT("Battlefield 6 maps")) ]
					+ SScrollBox::Slot()[ GridPaid ]
					+ SScrollBox::Slot().Padding(FMargin(2, 22, 2, 10))[ MakeSectionHeader(TEXT("RedSec maps - free for everyone")) ]
					+ SScrollBox::Slot()[ GridFree ]
				]
			]
		];
	}

private:
	FOnOpen OnOpen;
	FSimpleDelegate OnImport;
	FSimpleDelegate OnSdkSetup;
	FSimpleDelegate OnReturn;

	void Open(const FString& Level, const FString& Save)
	{
		OnOpen.ExecuteIfBound(Level, Save);
	}

	TSharedRef<SWidget> MakeCard(const FString& Level)
	{
		const TArray<FString> Saves = BF6Api::SavesFor(Level);
		const FSlateBrush* Thumb = BF6Api::MapThumbnail(Level);
		const FBF6MapCard* Info = FindMapCard(Level);

		// saves dropdown source
		TSharedPtr<TArray<TSharedPtr<FString>>> Src = MakeShared<TArray<TSharedPtr<FString>>>();
		for (const FString& S : Saves) Src->Add(MakeShared<FString>(S));
		CardSaveSources.Add(Src);

		// Portal tile: the whole card is the button (key art + name bar); the
		// frame brightens to white on hover like the site's choose-maps tiles.
		TSharedRef<SVerticalBox> Card = SNew(SVerticalBox);
		Card->AddSlot().AutoHeight()
		[
			SNew(SButton).ButtonStyle(&CardButtonStyle()).ContentPadding(FMargin(1.f))
			.OnClicked_Lambda([this, Level]{ Open(Level, FString()); return FReply::Handled(); })
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBox).WidthOverride(300.f).HeightOverride(169.f)
					[
						SNew(SOverlay)
						+ SOverlay::Slot()
						[
							Thumb
							? StaticCastSharedRef<SWidget>(SNew(SImage).Image(Thumb))
							: StaticCastSharedRef<SWidget>(SNew(SBorder).BorderImage(PanelLightBrush()).HAlign(HAlign_Center).VAlign(VAlign_Center)[ SNew(STextBlock).Font(FontBold(12)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("NO PREVIEW"))) ])
						]
						// the site's top-left badge: map size class
						+ SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(8.f)
						[ MakeBadge(Info ? FString::Chr(Info->Size) : TEXT("M")) ]
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(10, 8))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight()
							[ SNew(STextBlock).Font(FontBold(13)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(BF6Api::DisplayName(Level).ToUpper())) ]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
							[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(FString::Printf(TEXT("%d OBJECTS"), BF6Api::PlaceableTotal(Level)))) ]
						]
						// the resume dropdown lives right in the title bar
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 6, 0)
						[
							Saves.Num() == 0
							? StaticCastSharedRef<SWidget>(SNullWidget::NullWidget)
							: StaticCastSharedRef<SWidget>(
								SNew(SComboBox<TSharedPtr<FString>>)
								.OptionsSource(Src.Get())
								.OnGenerateWidget_Lambda([](TSharedPtr<FString> In){ return SNew(STextBlock).Text(FText::FromString(In.IsValid() ? *In : FString())); })
								.OnSelectionChanged_Lambda([this, Level](TSharedPtr<FString> In, ESelectInfo::Type){ if (In.IsValid()) Open(Level, *In); })
								[ SNew(STextBlock).Font(FontBold(9)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(FString::Printf(TEXT("RESUME (%d)"), Saves.Num()))) ]
							)
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontBold(18)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("+"))) ]
					]
				]
			]
		];
		return SNew(SBox).WidthOverride(302.f)[ Card ];
	}

	TArray<TSharedPtr<TArray<TSharedPtr<FString>>>> CardSaveSources;
};

// ---------------------------------------------------------------------------
// The whole tool as one embedded, dockable panel. Screen 0 = map selector,
// screen 1 = build controls (map label + import/export + choose-another-map).
// The thin budget bar rides on the viewport during the build screen.
// ---------------------------------------------------------------------------
class SBF6ToolPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6ToolPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		ChildSlot
		[
			SNew(SBorder).BorderImage(InkBrush()).Padding(0)
			[
				SAssignNew(Switcher, SWidgetSwitcher)
				+ SWidgetSwitcher::Slot()
				[
					SNew(SBF6MapSelector).OnOpen_Lambda([this](FString Level, FString Save)
					{
						BF6Api::EnterBuild(Level, Save);
						if (Switcher.IsValid()) Switcher->SetActiveWidgetIndex(1);
					})
				]
				+ SWidgetSwitcher::Slot()
				[ BuildControls() ]
			]
		];
	}

private:
	TSharedPtr<SWidgetSwitcher> Switcher;

	TSharedRef<SWidget> BuildControls()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(20, 18, 20, 6))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
				[ SNew(STextBlock).Font(FontBold(22)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(TEXT("BUILDING"))) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(10,0,0,3)
				[ SNew(STextBlock).Font(FontBold(14)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text_Lambda([]{ return FText::FromString(BF6Api::DisplayName(BF6Api::CurrentLevel())); }) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(20, 0, 20, 14))
			[ SNew(STextBlock).Font(FontReg(11)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text_Lambda([]{ return FText::FromString(BF6Api::CurrentSave().IsEmpty() ? TEXT("Read-only base. Create a custom map to place objects.") : FString::Printf(TEXT("Editing '%s'  -  aim in the viewport and press SPACE to place objects."), *BF6Api::CurrentSave())); }) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(20, 0, 20, 0))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)
				[ MakeToolButton(TEXT("Import .spatial.json"), []{ BF6Api::ImportSpatial(); }) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)
				[ MakeToolButton(TEXT("Export .spatial.json"), []{ BF6Api::ExportSpatial(); }) ]
			]
			+ SVerticalBox::Slot().FillHeight(1)[ SNew(SSpacer) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(20, 8, 20, 18))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[ MakeToolButton(TEXT("< Choose another map"), [this]{ BF6Api::HideBuildOverlay(); if (Switcher.IsValid()) Switcher->SetActiveWidgetIndex(0); }) ]
			];
	}
};

// ---------------------------------------------------------------------------
// First-time setup: point the tool at an unzipped Portal SDK download and it
// generates all of its data from it (catalogue, base setups, and every model,
// converted by driving the SDK's own bundled Godot headlessly). Also reachable
// later from the selector for re-syncing after an SDK update.
// ---------------------------------------------------------------------------
class SBF6SetupScreen : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6SetupScreen) {}
		SLATE_EVENT(FSimpleDelegate, OnDone)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		OnDone = InArgs._OnDone;
		ChildSlot
		[
			SNew(SBorder).BorderImage(InkBrush()).Padding(0).HAlign(HAlign_Center).VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(640.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(TEXT("B F 6   U N R E A L   S D K"))) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 10)
					[ SNew(STextBlock).Font(FontBold(26)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(TEXT("SDK SETUP"))) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 16)
					[ SNew(STextBlock).AutoWrapText(true).Font(FontReg(12)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("This tool builds its map and model data from the official Battlefield 6 Portal SDK. Download the SDK from the Portal site, unzip it anywhere, then point the tool at that folder. An SDK you already mod in works fine - only the official content is read. Close the SDK's Godot editor while the import runs. The import takes a while (about 9,700 models); re-running it after an SDK update only converts what changed."))) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[ SAssignNew(PathBox, SEditableTextBox).Text(FText::FromString(BF6Api::StoredSdkRoot())).HintText(FText::FromString(TEXT("C:\\...\\PortalSDK  (the unzipped SDK folder)"))) ]
						+ SHorizontalBox::Slot().AutoWidth()
						[ MakeToolButton(TEXT("Browse..."), [this]{ Browse(); }) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 16)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SBox).Visibility_Lambda([]{ return BF6Api::IsImporting() ? EVisibility::Collapsed : EVisibility::Visible; })
							[ MakePrimaryButton(TEXT("Import SDK data"), [this]{ if (PathBox.IsValid()) BF6Api::StartSdkImport(PathBox->GetText().ToString()); }) ]
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
						[
							SNew(SBox).Visibility_Lambda([]{ return BF6Api::ImportDone() ? EVisibility::Visible : EVisibility::Collapsed; })
							[ MakePrimaryButton(TEXT("Continue"), [this]{ OnDone.ExecuteIfBound(); }) ]
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
						[
							// wipe + reconvert: needed when an SDK update CHANGES existing
							// content (the normal import only adds what's new)
							SNew(SBox).Visibility_Lambda([]{ return (!BF6Api::IsImporting() && BF6Api::IsDataInstalled()) ? EVisibility::Visible : EVisibility::Collapsed; })
							[ MakeToolButton(TEXT("Full re-sync"), [this]
							{
								if (!PathBox.IsValid()) return;
								if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(
									TEXT("Delete all converted models and map meshes, then reconvert everything from the SDK?\n\nThis takes as long as the first import. Use it after a big SDK update, when existing content may have changed."))) == EAppReturnType::Yes)
									BF6Api::StartSdkImport(PathBox->GetText().ToString(), true);
							}) ]
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
						[
							// data already there (re-sync visit): let the user back out
							SNew(SBox).Visibility_Lambda([]{ return (!BF6Api::IsImporting() && BF6Api::IsDataInstalled()) ? EVisibility::Visible : EVisibility::Collapsed; })
							[ MakeToolButton(TEXT("Back"), [this]{ OnDone.ExecuteIfBound(); }) ]
						]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SVerticalBox)
						.Visibility_Lambda([]{ return (BF6Api::IsImporting() || BF6Api::ImportDone()) ? EVisibility::Visible : EVisibility::Collapsed; })
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
						[ SNew(SProgressBar).Percent_Lambda([]{ return BF6Api::ImportFrac(); }).FillColorAndOpacity(FSlateColor(BF6Theme::Accent)) ]
						+ SVerticalBox::Slot().AutoHeight()
						[ SNew(STextBlock).Font(FontReg(11)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text_Lambda([]{ return BF6Api::ImportStatus(); }) ]
					]
				]
			]
		];
	}

private:
	FSimpleDelegate OnDone;
	TSharedPtr<SEditableTextBox> PathBox;

	void Browse()
	{
		IDesktopPlatform* DP = FDesktopPlatformModule::Get();
		if (!DP || !PathBox.IsValid()) return;
		FString Picked;
		const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(SharedThis(this));
		if (DP->OpenDirectoryDialog(Parent, TEXT("Select your unzipped Portal SDK folder"), PathBox->GetText().ToString(), Picked))
			PathBox->SetText(FText::FromString(Picked));
	}
};

// ---------------------------------------------------------------------------
// Viewport root: the entire tool UI, attached to the REAL level viewport (which
// is the editor's whole centre and resizes with it - no window, no docked tab).
// Screen 0 = the map selector, opaque and filling the viewport. Screen 1 = the
// build HUD (budget bar + save/export), hit-test-invisible in empty space.
// ---------------------------------------------------------------------------
class SBF6ViewportRoot : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6ViewportRoot) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		SetVisibility(EVisibility::SelfHitTestInvisible);
		ChildSlot
		[
			SAssignNew(Switcher, SWidgetSwitcher).Visibility(EVisibility::SelfHitTestInvisible)
			+ SWidgetSwitcher::Slot()
			[ SAssignNew(SelectorHost, SBox) ]
			+ SWidgetSwitcher::Slot()
			[
				SNew(SBF6BuildOverlay).OnChooseMap_Lambda([this]{ ShowSelector(); })
			]
		];
		RebuildSelector();
	}

	void ShowBuild()    { if (Switcher.IsValid()) Switcher->SetActiveWidgetIndex(1); }
	// Rebuild the selector every time it's shown so the per-map saves dropdowns
	// pick up sessions created/saved since (they were snapshotted at construct).
	void ShowSelector() { RebuildSelector(); if (Switcher.IsValid()) Switcher->SetActiveWidgetIndex(0); }
	bool IsBuildScreen() const { return Switcher.IsValid() && Switcher->GetActiveWidgetIndex() == 1; }

	void ShowSetup()
	{
		if (!SelectorHost.IsValid()) return;
		SelectorHost->SetContent(SNew(SBF6SetupScreen).OnDone_Lambda([this]{ RebuildSelector(); }));
		if (Switcher.IsValid()) Switcher->SetActiveWidgetIndex(0);
	}

private:
	TSharedPtr<SWidgetSwitcher> Switcher;
	TSharedPtr<SBox> SelectorHost;

	void RebuildSelector()
	{
		if (!SelectorHost.IsValid()) return;
		// no data yet = first run: the setup screen takes over until the SDK
		// import has produced the model packs
		if (!BF6Api::IsDataInstalled()) { ShowSetup(); return; }
		SelectorHost->SetContent(
			SNew(SBF6MapSelector)
			.OnOpen_Lambda([](FString Level, FString Save){ if (ConfirmLeaveSession()) BF6Api::EnterBuild(Level, Save); })
			.OnImport_Lambda([this]{ if (ConfirmLeaveSession() && BF6Api::ImportSpatial()) ShowBuild(); })
			.OnSdkSetup_Lambda([this]{ ShowSetup(); })
			.OnReturn_Lambda([this]{ ShowBuild(); })
		);
	}
};

// ============================================================================
// Build Mode module state + the directional pie.
// ============================================================================
class FBF6InputProcessor;   // fwd (for the GInput pointer below)

FVector GBF6PendingWorld = FVector::ZeroVector;   // world spot captured when the pie opened

namespace
{
	TSharedPtr<SBF6ViewportRoot>   GRoot;            // the whole tool UI, on the viewport
	TSharedPtr<SLevelViewport>     GRootViewport;
	TSharedPtr<FBF6InputProcessor> GInput;
	TSharedPtr<IMenu>              GTransientMenu;   // the category object popup
	TSharedPtr<SBF6PieMenu>        GPie;             // the directional pie
	TSharedPtr<SLevelViewport>     GPieViewport;
	FVector2D                      GPieCenter = FVector2D::ZeroVector;

	// context the pie opened with: placing objects, editing an object's
	// attributes, or editing a zone's points
	enum class EBF6PieMode { Place, Props, VolEdit };
	EBF6PieMode                   GPieMode = EBF6PieMode::Place;
	TWeakObjectPtr<AActor>        GPieTarget;
	FString                       GPieTargetType;
	TArray<BF6Api::FPropDef>      GPieProps;
	int32                         GPiePage = 0;
	static const int32            kPiePropsPerPage = 10;
}

static bool BF6Pie_Active() { return GPie.IsValid(); }

// Push a popup and ALWAYS clear our pointer when the menu stack dismisses it
// (clicking away, focus loss). Holding a stale IMenu kept GTransientMenu
// "valid" forever, which permanently gated the space-bar radial.
static void BF6_PushTransient(TSharedRef<SWidget> Content, const FVector2D& Center)
{
	if (!GRoot.IsValid()) return;
	GTransientMenu = FSlateApplication::Get().PushMenu(
		GRoot.ToSharedRef(), FWidgetPath(), Content, Center,
		FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
	if (GTransientMenu.IsValid())
		GTransientMenu->GetOnMenuDismissed().AddLambda([](TSharedRef<IMenu>){ GTransientMenu.Reset(); });
}

static void BF6Pie_Close()
{
	if (GPie.IsValid() && GPieViewport.IsValid()) GPieViewport->RemoveOverlayWidget(GPie.ToSharedRef());
	GPie.Reset(); GPieViewport.Reset();
}

// Build the pie's items for the current mode and attach it at GPieCenter.
static void BF6Pie_Attach()
{
	FLevelEditorModule& LE = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	TSharedPtr<SLevelViewport> VP = LE.GetFirstActiveLevelViewport();
	if (!VP.IsValid()) return;

	TArray<FString> Items, Subs;
	switch (GPieMode)
	{
	case EBF6PieMode::VolEdit:
		Items = { TEXT("ADD POINT"), TEXT("DELETE POINT"), TEXT("FINISH EDITING") };
		Subs  = { TEXT("after selected"), TEXT("selected"), TEXT("bake the zone") };
		break;
	case EBF6PieMode::Props:
	{
		GPieProps = BF6Api::PropsForType(GPieTargetType);
		if (BF6Api::IsVolumeActor(GPieTarget.Get())) { Items.Add(TEXT("EDIT POINTS")); Subs.Add(TEXT("zone shape")); }
		int32 Start = GPiePage * kPiePropsPerPage;
		if (Start >= GPieProps.Num()) { GPiePage = 0; Start = 0; }
		for (int32 i = Start; i < GPieProps.Num() && i < Start + kPiePropsPerPage; i++)
		{
			Items.Add(GPieProps[i].Name);
			Subs.Add(GPieProps[i].Type);
		}
		if (GPieProps.Num() > kPiePropsPerPage)
		{
			Items.Add(TEXT("MORE..."));
			Subs.Add(FString::Printf(TEXT("%d total"), GPieProps.Num()));
		}
		break;
	}
	default: break;   // Place: empty items = the object categories
	}

	GPie = SNew(SBF6PieMenu).Items(Items).Subs(Subs);
	VP->AddOverlayWidget(GPie.ToSharedRef(), 100);
	GPieViewport = VP;
	GPie->SetHighlighted(-1);
	FSlateApplication::Get().SetCursorPos(GPieCenter);
}

// Space over the viewport: pick the pie's mode from context (selection / zone
// edit in progress), remember the world spot under the crosshair, and open the
// pie centred on the viewport with the cursor warped to neutral.
static void BF6Pie_Open()
{
	FVector W; if (BF6Api::WorldFromViewportCursor(W)) GBF6PendingWorld = W;

	if (BF6Api::IsVolumeEditing())
	{
		GPieMode = EBF6PieMode::VolEdit;
	}
	else
	{
		FString SelType;
		AActor* Sel = BF6Api::SelectedGameplayActor(SelType);
		if (Sel)
		{
			GPieMode = EBF6PieMode::Props;
			GPieTarget = Sel;
			GPieTargetType = SelType;
			GPiePage = 0;
		}
		else GPieMode = EBF6PieMode::Place;
	}

	FLevelEditorModule& LE = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	TSharedPtr<SLevelViewport> VP = LE.GetFirstActiveLevelViewport();
	if (!VP.IsValid()) return;
	const FGeometry& Geo = VP->GetCachedGeometry();
	GPieCenter = Geo.LocalToAbsolute(Geo.GetLocalSize() * 0.5f);
	BF6Pie_Attach();
}

// Mouse direction from the centre picks the wedge; inside the deadzone = cancel.
static void BF6Pie_Update(const FVector2D& CursorScreen)
{
	if (!GPie.IsValid()) return;
	const FVector2D v = CursorScreen - GPieCenter;
	const int32 N = GPie->Categories().Num();
	if (N == 0 || v.Size() < 66.f) { GPie->SetHighlighted(-1); return; }   // inside the hub circle = cancel
	const float Ang = FMath::RadiansToDegrees(FMath::Atan2(v.Y, v.X));
	const float Step = 360.f / N;
	int32 Idx = FMath::RoundToInt((Ang - (-90.f)) / Step);
	Idx = ((Idx % N) + N) % N;
	GPie->SetHighlighted(Idx);
}

// Confirm: dispatch the highlighted wedge by the mode the pie opened in.
// Deadzone (no highlight) just closes.
static void BF6Pie_Confirm()
{
	if (!GPie.IsValid()) return;
	const int32 Idx = GPie->GetHighlighted();
	const TArray<FString> Items = GPie->Categories();
	const FVector2D Center = GPieCenter;
	const EBF6PieMode Mode = GPieMode;
	BF6Pie_Close();
	if (Idx < 0 || !Items.IsValidIndex(Idx)) return;
	const FString Pick = Items[Idx];

	if (Mode == EBF6PieMode::VolEdit)
	{
		if (Pick == TEXT("ADD POINT"))         BF6Api::VolumeAddPoint();
		else if (Pick == TEXT("DELETE POINT")) BF6Api::VolumeDeletePoint();
		else                                   BF6Api::FinishVolumeEdit();
		return;
	}

	if (Mode == EBF6PieMode::Props)
	{
		AActor* Target = GPieTarget.Get();
		if (!Target) return;
		if (Pick == TEXT("EDIT POINTS")) { BF6Api::BeginVolumeEdit(Target); return; }
		if (Pick == TEXT("MORE...")) { GPiePage++; BF6Pie_Attach(); return; }
		const BF6Api::FPropDef* Def = GPieProps.FindByPredicate([&](const BF6Api::FPropDef& D){ return D.Name == Pick; });
		if (!Def) return;
		const bool bLink = Def->Type.Contains(TEXT("Volume")) || Def->Type.Contains(TEXT("Array[")) || Def->Type.Contains(TEXT("Path")) || Def->Type.Contains(TEXT("SpawnPoint"));
		if (bLink)
		{
			BF6Api::BeginLinkPick(Target, Def->Name, Def->Type.Contains(TEXT("Array[")));
			return;
		}
		BF6_PushTransient(SNew(SBF6PropEditPopup).TargetActor(Target).Def(*Def).TypeName(GPieTargetType), Center);
		return;
	}

	// Place mode: open the category's object list
	BF6_PushTransient(SNew(SBF6CategoryPopup).Category(Pick), Center);
}

static void BF6Pie_Cancel() { BF6Pie_Close(); }

// ---------------------------------------------------------------------------
// Input handler: SPACE over the viewport opens the pie; mouse direction picks a
// wedge; SPACE or LEFT-CLICK confirms; centre + confirm, or ESC, cancels.
// ---------------------------------------------------------------------------
class FBF6InputProcessor : public IInputProcessor
{
public:
	virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}

	virtual bool HandleKeyDownEvent(FSlateApplication& App, const FKeyEvent& E) override
	{
		const FKey K = E.GetKey();
		if (K == EKeys::SpaceBar)
		{
			if (BF6Pie_Active()) { if (!E.IsRepeat()) BF6Pie_Confirm(); return true; }
			if (E.IsRepeat()) return false;
			// assigning a link: SPACE commits the current selection as the target
			if (BF6Api::IsLinkPicking()) { BF6Api::ConfirmLinkPick(); return true; }
			if (!BF6Api::IsBuildOverlayActive() || !BF6Api::IsEditing() || GTransientMenu.IsValid()) return false;
			FVector W; if (!BF6Api::WorldFromViewportCursor(W)) return false;   // only over a viewport
			BF6Pie_Open();
			return true;
		}
		if (K == EKeys::Escape)
		{
			if (BF6Pie_Active()) { BF6Pie_Cancel(); return true; }
			if (BF6Api::IsLinkPicking()) { BF6Api::CancelLinkPick(); return true; }
			if (BF6Api::IsVolumeEditing()) { BF6Api::FinishVolumeEdit(); return true; }
			// on the map screen with a session running: ESC returns to the build
			if (GRoot.IsValid() && !GRoot->IsBuildScreen() && !BF6Api::CurrentLevel().IsEmpty() && !GTransientMenu.IsValid())
			{
				GRoot->ShowBuild();
				return true;
			}
		}
		// ENTER also bakes and ends a zone-point edit (when no popup is up)
		if (K == EKeys::Enter && !BF6Pie_Active() && !GTransientMenu.IsValid() && BF6Api::IsVolumeEditing())
		{
			BF6Api::FinishVolumeEdit();
			return true;
		}
		return false;
	}

	virtual bool HandleMouseMoveEvent(FSlateApplication& App, const FPointerEvent& E) override
	{
		if (BF6Pie_Active()) BF6Pie_Update(E.GetScreenSpacePosition());
		if (bRightDown && FVector2D::Distance(E.GetScreenSpacePosition(), RightDownPos) > 6.f) bRightDragged = true;
		return false;
	}

	virtual bool HandleMouseButtonDownEvent(FSlateApplication& App, const FPointerEvent& E) override
	{
		if (BF6Pie_Active() && E.GetEffectingButton() == EKeys::LeftMouseButton) { BF6Pie_Confirm(); return true; }
		if (E.GetEffectingButton() == EKeys::RightMouseButton)
		{
			bRightDown = true;
			bRightDragged = false;
			RightDownPos = E.GetScreenSpacePosition();
		}
		return false;   // never consume the down - camera look must still work
	}

	virtual bool HandleMouseButtonUpEvent(FSlateApplication& App, const FPointerEvent& E) override
	{
		if (E.GetEffectingButton() != EKeys::RightMouseButton) return false;
		const bool bWasClick = bRightDown && !bRightDragged;
		bRightDown = false;
		// Godot-style: while a zone's points are up, right-clicking near an edge
		// inserts a point there. Right-drag (camera) passes through untouched.
		if (bWasClick && BF6Api::IsVolumeEditing())
		{
			FVector W;
			if (BF6Api::WorldFromViewportCursor(W)) { BF6Api::VolumeAddPointAt(W); return true; }
		}
		return false;
	}

private:
	bool bRightDown = false, bRightDragged = false;
	FVector2D RightDownPos = FVector2D::ZeroVector;
};

// ---- BF6Api entry points (module state + the pie live above) ----
TSharedRef<SWidget> BF6Api::MakeToolPanel()
{
	return SNew(SBF6ToolPanel);
}

void BF6Api::EnterBuild(const FString& Level, const FString& SaveName)
{
	BF6Api::OpenMapWorld(Level, SaveName);
	BF6Api::ShowBuildOverlay();
}

// Attach the tool UI to the level viewport. The viewport may not exist yet at
// module startup, so retry on a ticker until it does.
void BF6Api::ShowStartupUI()
{
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
	{
		if (GRoot.IsValid()) return false;                                        // already attached
		if (!FModuleManager::Get().IsModuleLoaded("LevelEditor")) return true;    // keep waiting
		FLevelEditorModule& LE = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
		TSharedPtr<SLevelViewport> VP = LE.GetFirstActiveLevelViewport();
		if (!VP.IsValid()) return true;                                           // keep waiting
		GRoot = SNew(SBF6ViewportRoot);
		VP->AddOverlayWidget(GRoot.ToSharedRef(), 50);
		GRootViewport = VP;
		return false;                                                             // done
	}), 0.5f);
}

// Park a shared pointer on the heap forever: releases OUR reference without
// ever running the widget's destructor. Used only during engine teardown,
// where destructing Slate widgets (whose destructors touch preview scenes and
// viewports that are already dead) crashes. The memory goes with the process.
template <typename T>
static void BF6_LeakForExit(TSharedPtr<T>& P)
{
	if (P.IsValid()) { new TSharedPtr<T>(P); P.Reset(); }
}

void BF6Api::DetachUI()
{
	// Too late in teardown to touch OR destroy widgets: RemoveOverlayWidget
	// crashed first, and even Reset() crashes in the destructor chain (preview
	// scenes reference dead worlds). Leak instead; the process is exiting anyway.
	if (!FSlateApplication::IsInitialized() || IsEngineExitRequested())
	{
		BF6_LeakForExit(GTransientMenu);
		BF6_LeakForExit(GPie); BF6_LeakForExit(GPieViewport);
		BF6_LeakForExit(GRoot); BF6_LeakForExit(GRootViewport);
		return;
	}
	BF6Pie_Close();
	HideTransientMenus();
	if (GRoot.IsValid() && GRootViewport.IsValid())
		GRootViewport->RemoveOverlayWidget(GRoot.ToSharedRef());
	GRoot.Reset();
	GRootViewport.Reset();
}

void BF6Api::ShowBuildOverlay() { if (GRoot.IsValid()) GRoot->ShowBuild(); }
void BF6Api::HideBuildOverlay() { if (GRoot.IsValid()) GRoot->ShowSelector(); }
void BF6Api::ShowSdkSetup()     { if (GRoot.IsValid()) GRoot->ShowSetup(); }

bool BF6Api::IsBuildOverlayActive() { return GRoot.IsValid() && GRoot->IsBuildScreen(); }

void BF6Api::HideTransientMenus()
{
	if (GTransientMenu.IsValid()) { GTransientMenu->Dismiss(); GTransientMenu.Reset(); }
}

void BF6Api::InstallInputHandler()
{
	if (GInput.IsValid()) return;
	GInput = MakeShared<FBF6InputProcessor>();
	FSlateApplication::Get().RegisterInputPreProcessor(GInput);
}

void BF6Api::RemoveInputHandler()
{
	if (GInput.IsValid() && FSlateApplication::IsInitialized())
		FSlateApplication::Get().UnregisterInputPreProcessor(GInput);
	GInput.Reset();
}
