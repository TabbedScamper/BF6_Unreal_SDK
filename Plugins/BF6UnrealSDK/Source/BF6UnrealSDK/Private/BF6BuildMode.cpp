#include "BF6BuildMode.h"
#include "BF6Theme.h"
#include "SBF6PreviewViewport.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/IMenu.h"
#include "InputCoreTypes.h"
#include "Widgets/SWindow.h"
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
			.SetNormal (FSlateRoundedBoxBrush(FLinearColor(1,1,1,0.03f), 2.f, BF6Theme::Line,        1.f))
			.SetHovered(FSlateRoundedBoxBrush(FLinearColor(1,1,1,0.10f), 2.f, FLinearColor::White,   1.f))
			.SetPressed(FSlateRoundedBoxBrush(FLinearColor(1,1,1,0.16f), 2.f, FLinearColor::White,   1.f))
			.SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0, 1, 0, -1));
		return S;
	}
	// Primary button: solid white fill, dark text (the site's main CTA).
	const FButtonStyle& PrimaryButtonStyle()
	{
		static FButtonStyle S = FButtonStyle()
			.SetNormal (FSlateRoundedBoxBrush(FLinearColor(0.92f,0.94f,0.96f,1.f), 2.f))
			.SetHovered(FSlateRoundedBoxBrush(FLinearColor(1.00f,1.00f,1.00f,1.f), 2.f))
			.SetPressed(FSlateRoundedBoxBrush(FLinearColor(0.78f,0.81f,0.85f,1.f), 2.f))
			.SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0, 1, 0, -1));
		return S;
	}
	// Map tile: hairline frame normally, bright white frame on hover (the site's
	// choose-maps tile hover).
	const FButtonStyle& CardButtonStyle()
	{
		static FButtonStyle S = FButtonStyle()
			.SetNormal (FSlateRoundedBoxBrush(BF6Theme::Panel, 2.f, BF6Theme::Line,      1.f))
			.SetHovered(FSlateRoundedBoxBrush(BF6Theme::Panel, 2.f, FLinearColor::White, 2.f))
			.SetPressed(FSlateRoundedBoxBrush(BF6Theme::Panel, 2.f, BF6Theme::Accent,    2.f))
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
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		SetVisibility(EVisibility::HitTestInvisible);   // the input handler drives it
		Cats = BF6Api::Categories();
		const int32 N = FMath::Max(1, Cats.Num());
		const float R = 168.f;   // ring radius (px)

		TSharedRef<SConstraintCanvas> Canvas = SNew(SConstraintCanvas);

		// dim the viewport behind the pie
		Canvas->AddSlot().Anchors(FAnchors(0.f, 0.f, 1.f, 1.f)).Offset(FMargin(0))
			[ SNew(SBorder).BorderImage(DimBrush()) ];

		// centre hub - shows the highlighted category, or PLACE / CANCEL
		Canvas->AddSlot().Anchors(FAnchors(0.5f, 0.5f)).Alignment(FVector2D(0.5f, 0.5f)).AutoSize(true)
			[
				SNew(SBox).WidthOverride(96.f).HeightOverride(96.f)
				[
					SNew(SBorder).BorderImage_Lambda([this]{ return Highlighted < 0 ? InkBrush() : AccentBrush(); }).HAlign(HAlign_Center).VAlign(VAlign_Center)
					[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity_Lambda([this]{ return FSlateColor(Highlighted < 0 ? BF6Theme::TextDim : BF6Theme::Ink); })
						.Text_Lambda([this]{ return FText::FromString(Highlighted < 0 ? TEXT("cancel") : (Cats.IsValidIndex(Highlighted) ? Cats[Highlighted] : FString())); }) ]
				]
			];

		for (int32 i = 0; i < Cats.Num(); i++)
		{
			const float Ang = (-90.f + (360.f / N) * i) * PI / 180.f;
			const float X = R * FMath::Cos(Ang);
			const float Y = R * FMath::Sin(Ang);
			const FString Cat = Cats[i];
			const int32 Cnt = BF6Api::CategoryCount(Cat);
			const int32 Idx = i;

			Canvas->AddSlot()
				.Anchors(FAnchors(0.5f, 0.5f)).Offset(FMargin(X, Y, 0.f, 0.f)).Alignment(FVector2D(0.5f, 0.5f)).AutoSize(true)
				[
					SNew(SBox).WidthOverride(104.f).HeightOverride(60.f)
					[
						SNew(SBorder).BorderImage_Lambda([this, Idx]{ return Highlighted == Idx ? AccentBrush() : PanelBrush(); }).HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(4.f)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
							[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity_Lambda([this, Idx]{ return FSlateColor(Highlighted == Idx ? BF6Theme::Ink : BF6Theme::Text); }).Text(FText::FromString(Cat)) ]
							+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
							[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity_Lambda([this, Idx]{ return FSlateColor(Highlighted == Idx ? BF6Theme::Ink : BF6Theme::TextDim); }).Text(FText::FromString(FString::Printf(TEXT("%d"), Cnt))) ]
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
	int32 Highlighted = -1;
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
		if (T - LastCalc > 0.25) { LastCalc = T; BF6Api::RecomputeBudget(); }
	}

private:
	bool bBarHidden = false;
	double LastCalc = 0.0;
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
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		OnOpen = InArgs._OnOpen;
		OnImport = InArgs._OnImport;
		OnSdkSetup = InArgs._OnSdkSetup;

		TSharedRef<SWrapBox> Grid = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(12, 12));
		int32 MapCount = 0;
		for (const FString& Level : BF6Api::AllLevels())
		{
			Grid->AddSlot()[ MakeCard(Level) ];
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
				[ SNew(SScrollBox) + SScrollBox::Slot()[ Grid ] ]
			]
		];
	}

private:
	FOnOpen OnOpen;
	FSimpleDelegate OnImport;
	FSimpleDelegate OnSdkSetup;

	void Open(const FString& Level, const FString& Save)
	{
		OnOpen.ExecuteIfBound(Level, Save);
	}

	TSharedRef<SWidget> MakeCard(const FString& Level)
	{
		const TArray<FString> Saves = BF6Api::SavesFor(Level);
		const FSlateBrush* Thumb = BF6Api::MapThumbnail(Level);

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
						Thumb
						? StaticCastSharedRef<SWidget>(SNew(SImage).Image(Thumb))
						: StaticCastSharedRef<SWidget>(SNew(SBorder).BorderImage(PanelLightBrush()).HAlign(HAlign_Center).VAlign(VAlign_Center)[ SNew(STextBlock).Font(FontBold(12)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("NO PREVIEW"))) ])
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(10, 8))
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[ SNew(STextBlock).Font(FontBold(13)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(BF6Api::DisplayName(Level).ToUpper())) ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
						[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(FString::Printf(TEXT("%s  -  %d OBJECTS"), *Level.ToUpper(), BF6Api::PlaceableTotal(Level)))) ]
					]
				]
			]
		];
		// resume strip - only when this map has saved projects
		if (Saves.Num() > 0)
		{
			Card->AddSlot().AutoHeight().Padding(FMargin(0, 4, 0, 0))
			[
				SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(10, 4))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
					[ SNew(STextBlock).Font(FontBold(9)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(TEXT("RESUME"))) ]
					+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
					[
						SNew(SComboBox<TSharedPtr<FString>>)
						.OptionsSource(Src.Get())
						.OnGenerateWidget_Lambda([](TSharedPtr<FString> In){ return SNew(STextBlock).Text(FText::FromString(In.IsValid() ? *In : FString())); })
						.OnSelectionChanged_Lambda([this, Level](TSharedPtr<FString> In, ESelectInfo::Type){ if (In.IsValid()) Open(Level, *In); })
						[ SNew(STextBlock).Font(FontReg(10)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(FString::Printf(TEXT("%d saved project%s..."), Saves.Num(), Saves.Num() == 1 ? TEXT("") : TEXT("s")))) ]
					]
				]
			];
		}
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
					[ SNew(STextBlock).AutoWrapText(true).Font(FontReg(12)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("This tool builds its map and model data from the official Battlefield 6 Portal SDK. Download the SDK from the Portal site, unzip it anywhere, then point the tool at that folder. The import runs once and takes a while (about 9,700 models); re-running it after an SDK update only converts what changed."))) ]
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
			.OnOpen_Lambda([](FString Level, FString Save){ BF6Api::EnterBuild(Level, Save); })
			.OnImport_Lambda([this]{ if (BF6Api::ImportSpatial()) ShowBuild(); })
			.OnSdkSetup_Lambda([this]{ ShowSetup(); })
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
}

static bool BF6Pie_Active() { return GPie.IsValid(); }

static void BF6Pie_Close()
{
	if (GPie.IsValid() && GPieViewport.IsValid()) GPieViewport->RemoveOverlayWidget(GPie.ToSharedRef());
	GPie.Reset(); GPieViewport.Reset();
}

// Space over the viewport: remember the world spot under the crosshair, drop the
// pie centred on the viewport, and warp the cursor to the centre so the gesture
// starts neutral (centre = cancel).
static void BF6Pie_Open()
{
	FVector W; if (BF6Api::WorldFromViewportCursor(W)) GBF6PendingWorld = W;
	FLevelEditorModule& LE = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	TSharedPtr<SLevelViewport> VP = LE.GetFirstActiveLevelViewport();
	if (!VP.IsValid()) return;
	const FGeometry& Geo = VP->GetCachedGeometry();
	GPieCenter = Geo.LocalToAbsolute(Geo.GetLocalSize() * 0.5f);
	GPie = SNew(SBF6PieMenu);
	VP->AddOverlayWidget(GPie.ToSharedRef(), 100);
	GPieViewport = VP;
	GPie->SetHighlighted(-1);
	FSlateApplication::Get().SetCursorPos(GPieCenter);
}

// Mouse direction from the centre picks the wedge; inside the deadzone = cancel.
static void BF6Pie_Update(const FVector2D& CursorScreen)
{
	if (!GPie.IsValid()) return;
	const FVector2D v = CursorScreen - GPieCenter;
	const int32 N = GPie->Categories().Num();
	if (N == 0 || v.Size() < 44.f) { GPie->SetHighlighted(-1); return; }
	const float Ang = FMath::RadiansToDegrees(FMath::Atan2(v.Y, v.X));
	const float Step = 360.f / N;
	int32 Idx = FMath::RoundToInt((Ang - (-90.f)) / Step);
	Idx = ((Idx % N) + N) % N;
	GPie->SetHighlighted(Idx);
}

// Confirm: a highlighted wedge opens its object list at the centre; the deadzone
// (no highlight) just closes.
static void BF6Pie_Confirm()
{
	if (!GPie.IsValid()) return;
	const int32 Idx = GPie->GetHighlighted();
	const TArray<FString> Cats = GPie->Categories();
	const FVector2D Center = GPieCenter;
	BF6Pie_Close();
	if (Idx >= 0 && Cats.IsValidIndex(Idx) && GRoot.IsValid())
	{
		TSharedRef<SBF6CategoryPopup> Popup = SNew(SBF6CategoryPopup).Category(Cats[Idx]);
		GTransientMenu = FSlateApplication::Get().PushMenu(
			GRoot.ToSharedRef(), FWidgetPath(), Popup, Center,
			FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
	}
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
			if (!BF6Api::IsBuildOverlayActive() || !BF6Api::IsEditing() || GTransientMenu.IsValid()) return false;
			FVector W; if (!BF6Api::WorldFromViewportCursor(W)) return false;   // only over a viewport
			BF6Pie_Open();
			return true;
		}
		if (K == EKeys::Escape && BF6Pie_Active()) { BF6Pie_Cancel(); return true; }
		return false;
	}

	virtual bool HandleMouseMoveEvent(FSlateApplication& App, const FPointerEvent& E) override
	{
		if (BF6Pie_Active()) BF6Pie_Update(E.GetScreenSpacePosition());
		return false;
	}

	virtual bool HandleMouseButtonDownEvent(FSlateApplication& App, const FPointerEvent& E) override
	{
		if (BF6Pie_Active() && E.GetEffectingButton() == EKeys::LeftMouseButton) { BF6Pie_Confirm(); return true; }
		return false;
	}
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

void BF6Api::DetachUI()
{
	if (!FSlateApplication::IsInitialized()) { GRoot.Reset(); GRootViewport.Reset(); return; }
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
