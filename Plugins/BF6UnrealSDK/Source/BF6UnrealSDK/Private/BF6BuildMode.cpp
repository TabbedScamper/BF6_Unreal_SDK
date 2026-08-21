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
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SCheckBox.h"
#include "Containers/Ticker.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Misc/MessageDialog.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/STileView.h"
#include "Input/DragAndDrop.h"
#include "Fonts/FontMeasure.h"
#include "Widgets/SToolTip.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateColor.h"
#include "Styling/SlateTypes.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"

#include "LevelEditor.h"
#include "SLevelViewport.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "UnrealWidgetFwd.h"
#include "Settings/LevelEditorViewportSettings.h"

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
	// Same ghost button, but the label follows a value - used where one control
	// flips between two jobs (the outliner tree/folders toggle).
	TSharedRef<SWidget> MakeToolButton_Dynamic(TAttribute<FText> Label, TFunction<void()> OnClick)
	{
		return SNew(SButton).ButtonStyle(&GhostButtonStyle()).ContentPadding(FMargin(16, 8))
			.OnClicked_Lambda([OnClick]{ if (OnClick) OnClick(); return FReply::Handled(); })
			[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity(FSlateColor(BF6Theme::Text))
				.Text_Lambda([Label]{ return FText::FromString(Label.Get(FText::GetEmpty()).ToString().ToUpper()); }) ];
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
// fwd: reopen the pie exactly where it was (the pop-out menus' BACK button)
static void BF6Pie_Reopen();

// ---------------------------------------------------------------------------
// SDK hints: pretty hover explanations of what things do, sourced from the
// SDK's own docs. Toggled from the CONTROLS panel; the custom tooltip class
// reports itself empty when hints are off, so every hint obeys one switch.
// ---------------------------------------------------------------------------
static bool GSdkHints = true;
static bool GSdkHintsLoaded = false;

static bool BF6_HintsOn()
{
	if (!GSdkHintsLoaded)
	{
		GSdkHintsLoaded = true;
		GConfig->GetBool(TEXT("BF6UnrealSDK"), TEXT("SdkHintsOn"), GSdkHints, GEditorPerProjectIni);
	}
	return GSdkHints;
}

static void BF6_SetHintsOn(bool bOn)
{
	GSdkHints = bOn; GSdkHintsLoaded = true;
	GConfig->SetBool(TEXT("BF6UnrealSDK"), TEXT("SdkHintsOn"), bOn, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

class SBF6HintTip : public SToolTip
{
public:
	using FArguments = SToolTip::FArguments;
	void Construct(const FArguments& InArgs) { SToolTip::Construct(InArgs); }
	virtual bool IsEmpty() const override { return !BF6_HintsOn() || SToolTip::IsEmpty(); }
};

static TSharedRef<IToolTip> BF6_MakeHint(const FString& Title, const FString& Body)
{
	static FSlateColorBrush LineB(FLinearColor(FColor(0xAE, 0xC0, 0xCC)) * FLinearColor(1.f, 1.f, 1.f, 0.5f));
	return SNew(SBF6HintTip)
	.TextMargin(FMargin(0))
	.Content()
	[
		SNew(SBorder).BorderImage(&LineB).Padding(1.f)
		[
			SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(12, 9))
			[
				SNew(SBox).MaxDesiredWidth(300.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(Title.ToUpper())) ]
					+ SVerticalBox::Slot().AutoHeight()
					[ SNew(STextBlock).Font(FontReg(10)).AutoWrapText(true).ColorAndOpacity(FSlateColor(BF6Theme::TextBlue)).Text(FText::FromString(Body)) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
					[ SNew(STextBlock).Font(FontBold(7)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("S D K   H I N T"))) ]
				]
			]
		]
	];
}

// what each editable field means, distilled from the SDK's shipped docs
static FString BF6_AttributeHint(const BF6Api::FPropDef& Def)
{
	const bool bLink = Def.Type.Contains(TEXT("Volume")) || Def.Type.Contains(TEXT("Array[")) || Def.Type.Contains(TEXT("Path")) || Def.Type.Contains(TEXT("SpawnPoint"));
	if (Def.Name.Equals(TEXT("ObjId"), ESearchCase::IgnoreCase))
		return TEXT("A unique number scripts use to grab this object - GetHQ(objId), GetSpawnPoint(objId) and friends. If your mode has no scripting you can leave it alone.");
	if (Def.Name.Contains(TEXT("Team"), ESearchCase::IgnoreCase))
		return TEXT("Which team this belongs to. An HQ spawner deploys its team's players from here; team-less spawners are driven from script instead.");
	if (Def.Name.Equals(TEXT("height"), ESearchCase::IgnoreCase))
		return TEXT("The zone's height in metres. 0 means infinite - the editor draws infinite zones at 5 m, just like Godot.");
	if (Def.Name.Equals(TEXT("size"), ESearchCase::IgnoreCase))
		return TEXT("The box volume's size in metres (x, height, z). Drag the face handles to shape it, or type exact numbers here.");
	if (Def.Name.Contains(TEXT("Vehicle"), ESearchCase::IgnoreCase))
		return TEXT("Which vehicle this spawner produces. Trigger the spawn from script via GetVehicleSpawner(objId).");
	if (bLink)
		return TEXT("A link to other placed objects. Use Pick in world to click the target - for example, spawn points linked to an HQ decide where its players appear.");
	if (Def.Type == TEXT("selection"))
		return TEXT("Pick one of the SDK's preset options - the same dropdown the Godot editor shows for this field.");
	if (Def.Type == TEXT("bool"))
		return TEXT("An on/off switch the game mode reads from this object.");
	return FString::Printf(TEXT("A %s value the SDK reads from this object at runtime."), *Def.Type);
}

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
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[
							SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(6, 2))
							.OnClicked_Lambda([]{ BF6Pie_Reopen(); return FReply::Handled(); })
							[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("< BACK"))) ]
						]
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

// fwd (defined with the pie further down; the library's context menu uses it)
static void BF6_PushTransient(TSharedRef<SWidget> Content, const FVector2D& Center);

// ---------------------------------------------------------------------------
// Drag payload for library tiles: drop anywhere on the map to place the object
// there. bActive gates the full-viewport drop catcher's hit-testing so it only
// exists while one of OUR drags is in flight.
// ---------------------------------------------------------------------------
class FBF6LibDragOp : public FDragDropOperation
{
public:
	DRAG_DROP_OPERATOR_TYPE(FBF6LibDragOp, FDragDropOperation)
	FString PlaceableType;
	static bool bActive;

	static TSharedRef<FBF6LibDragOp> New(const FString& InType)
	{
		TSharedRef<FBF6LibDragOp> Op = MakeShared<FBF6LibDragOp>();
		Op->PlaceableType = InType;
		Op->Construct();
		bActive = true;
		return Op;
	}
	// last-resort ghost cleanup: runs however the drag ends (drop, cancel,
	// released outside every viewport)
	virtual ~FBF6LibDragOp() override { bActive = false; bWarnedReadOnly = false; BF6Api::DestroyDragGhost(); }
	static bool bWarnedReadOnly;   // one read-only warning per drag

	virtual TSharedPtr<SWidget> GetDefaultDecorator() const override
	{
		return SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(10, 5))
			[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(PlaceableType)) ];
	}
};
bool FBF6LibDragOp::bActive = false;
bool FBF6LibDragOp::bWarnedReadOnly = false;

static bool BF6_GodotCameraOn();   // defined with the input handler below

// ---------------------------------------------------------------------------
// The CONTROLS hint panel (top-left): persistent and Portal-styled, it slides
// in from the left like the Object Library slides up. Pinned by default;
// unpinned it collapses to a slim strip on the left edge and peeks on hover.
// Its rows follow what the user is doing (zone points, box faces, groups...).
// ---------------------------------------------------------------------------
class SBF6HintPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6HintPanel) {}
	SLATE_END_ARGS()

	static constexpr float kPanelW = 292.f;

	void Construct(const FArguments&)
	{
		SetCanTick(true);
		SetVisibility(EVisibility::SelfHitTestInvisible);
		bool bPin = true;
		GConfig->GetBool(TEXT("BF6UnrealSDK"), TEXT("HintsPinned"), bPin, GEditorPerProjectIni);
		bPinned = bPin;
		if (bPinned) SlideTarget = 1.f;

		ChildSlot
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox).Clipping(EWidgetClipping::ClipToBounds)
				.WidthOverride_Lambda([this]{ return FMath::Max(1.f, kPanelW * Slide); })
				.Visibility_Lambda([this]{ return Slide > 0.01f ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(10, 8))
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
							[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(BF6Theme::Accent))
								.Text_Lambda([this]{ return FText::FromString(TitleForMode(BuiltMode)); }) ]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[
								SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(6, 1))
								.ToolTipText(FText::FromString(TEXT("SDK hints: explain what fields and objects do when you hover them")))
								.OnClicked_Lambda([]{ BF6_SetHintsOn(!BF6_HintsOn()); return FReply::Handled(); })
								[ SNew(STextBlock).Font(FontBold(10))
									.ColorAndOpacity_Lambda([]{ return FSlateColor(BF6_HintsOn() ? BF6Theme::Accent : BF6Theme::TextDim); })
									.Text(FText::FromString(TEXT("?"))) ]
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[
								SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(6, 1))
								.OnClicked_Lambda([this]{ SetPinned(!bPinned); return FReply::Handled(); })
								[ SNew(STextBlock).Font(FontBold(9))
									.ColorAndOpacity_Lambda([this]{ return FSlateColor(bPinned ? BF6Theme::Accent : BF6Theme::TextDim); })
									.Text_Lambda([this]{ return FText::FromString(bPinned ? TEXT("PINNED") : TEXT("PIN")); }) ]
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[
								SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(6, 1))
								.OnClicked_Lambda([this]{ SetPinned(false); SlideTarget = 0.f; return FReply::Handled(); })
								[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("<"))) ]
							]
						]
						+ SVerticalBox::Slot().AutoHeight()
						[ SAssignNew(Rows, SVerticalBox) ]
					]
				]
			]
			// the peek strip when hidden
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)
			[
				SNew(SBox).Visibility_Lambda([this]{ return Slide < 0.05f ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(4, 14))
					.OnMouseButtonDown_Lambda([this](const FGeometry&, const FPointerEvent&){ SlideTarget = 1.f; return FReply::Handled(); })
					[ SNew(STextBlock).Font(FontBold(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT(">"))) ]
				]
			]
		];
		Rebuild(0);
	}

	virtual void Tick(const FGeometry& G, const double T, const float D) override
	{
		SCompoundWidget::Tick(G, T, D);
		const float Prev = Slide;
		const float Step = D * 7.f;
		Slide = SlideTarget > Slide ? FMath::Min(Slide + Step, SlideTarget) : FMath::Max(Slide - Step, SlideTarget);
		if (!FMath::IsNearlyEqual(Prev, Slide)) Invalidate(EInvalidateWidgetReason::Layout);
		const int32 M = CurrentMode();
		// context key: mode + whatever inside the mode changes the rows, so the
		// panel follows the selection, not just the editing mode
		int32 Key = M * 1000;
		if (M == 5) Key += BF6Api::ModeWizardStep();
		else if (M == 9) Key += (BF6Api::IsScatterDrawing() ? 1 : 0) + (BF6Api::GetScatterShape() == 3 ? 2 : 0);
		else if (M == 6 || M == 8)
		{
			const BF6Api::FSelInfo I = BF6Api::SelectionInfo();
			Key += (I.bBlock ? 1 : 0) + (I.bMesh ? 2 : 0) + (I.Fields > 0 ? 4 : 0);
		}
		if (Key != BuiltKey) Rebuild(M, Key);
	}

	virtual void OnMouseEnter(const FGeometry& G, const FPointerEvent& E) override
	{
		SCompoundWidget::OnMouseEnter(G, E);
		if (SlideTarget < 0.5f) SlideTarget = 1.f;   // peek
	}
	virtual void OnMouseLeave(const FPointerEvent& E) override
	{
		SCompoundWidget::OnMouseLeave(E);
		if (!bPinned) SlideTarget = 0.f;
	}

private:
	bool bPinned = true;
	float Slide = 0.f, SlideTarget = 0.f;
	int32 BuiltMode = -1;
	int32 BuiltKey = -1;
	TSharedPtr<SVerticalBox> Rows;

	void SetPinned(bool bIn)
	{
		bPinned = bIn;
		GConfig->SetBool(TEXT("BF6UnrealSDK"), TEXT("HintsPinned"), bPinned, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	static int32 CurrentMode()
	{
		if (BF6Api::IsPickPlacing()) return 10;
		if (BF6Api::IsScatterLive()) return 9;
		if (BF6Api::IsModeWizardActive()) return 5;
		if (BF6Api::IsLinkPicking()) return 4;
		if (BF6Api::IsVolumeEditing()) return 1;
		if (BF6Api::IsObbEditing()) return 2;
		if (BF6Api::IsGroupEditing()) return 3;
		// no editing mode active: the SELECTION decides what help to show
		const BF6Api::FSelInfo I = BF6Api::SelectionInfo();
		if (I.Count == 0) return 0;
		if (I.bOneGroup) return 8;   // one group/block acting as a unit
		return I.Count == 1 ? 6 : 7;
	}

	static const TCHAR* TitleForMode(int32 M)
	{
		switch (M)
		{
		case 1: return TEXT("ZONE POINTS");
		case 2: return TEXT("BOX FACES");
		case 3: return BF6Api::GroupEditIsBlock() ? TEXT("BLOCK EDIT") : TEXT("GROUP EDIT");
		case 4: return TEXT("ASSIGN LINK");
		case 5: return TEXT("MODE SETUP");
		case 6: return TEXT("THIS OBJECT");
		case 7: return TEXT("SELECTION");
		case 8: return BF6Api::SelectionInfo().bBlock ? TEXT("BLOCK") : TEXT("GROUP");
		case 9: return TEXT("SCATTER");
		case 10: return TEXT("PICK PLACE");
		default: return TEXT("CONTROLS");
		}
	}

	void Rebuild(int32 M, int32 Key = 0)
	{
		BuiltMode = M;
		BuiltKey = Key;
		if (!Rows.IsValid()) return;
		Rows->ClearChildren();
		TArray<TPair<FString, FString>> B;
		switch (M)
		{
		case 1:
			B = { { TEXT("Drag dot"), TEXT("move the point") },
				  { TEXT("Drag TOP dot"), TEXT("set the height (to the floor = infinite)") },
				  { TEXT("Ctrl+LMB"), TEXT("add a point on the edge") },
				  { TEXT("Del / Ctrl+RMB"), TEXT("delete the point") },
				  { TEXT("Enter / Esc"), TEXT("finish editing") } };
			break;
		case 2:
			B = { { TEXT("Drag face"), TEXT("resize that side") },
				  { TEXT("Alt + drag"), TEXT("symmetric resize") },
				  { TEXT("Enter / Esc"), TEXT("finish editing") } };
			break;
		case 3:
			B = { { TEXT("Click member"), TEXT("only members are editable") },
				  { TEXT("Enter"), TEXT("keep edits (block: updates ALL copies)") },
				  { TEXT("Esc"), TEXT("revert everything from this edit") } };
			break;
		case 4:
			B = { { TEXT("Click a glowing target"), TEXT("cyan = free, green = assigned, orange = picked") },
				  { TEXT("Space / Enter"), TEXT("confirm the picked targets, back to attributes") },
				  { TEXT("Esc"), TEXT("cancel, back to attributes") } };
			break;
		case 5:
			B = { { FString::Printf(TEXT("Step %d of %d"), BF6Api::ModeWizardStep(), BF6Api::ModeWizardTotal()), BF6Api::ModeWizardTitle() },
				  { FString(), BF6Api::ModeWizardBody() },
				  { TEXT("Click"), TEXT("place it right where you aim") },
				  { TEXT("Esc"), TEXT("stop the setup") } };
			break;
		case 6:   // one object selected
		{
			const BF6Api::FSelInfo I = BF6Api::SelectionInfo();
			B = { { TEXT("Space"), I.Fields > 0 ? TEXT("attributes, multiply, grouping") : TEXT("multiply, grouping, select similar") } };
			B.Add({ TEXT("Drag it"), TEXT("move it (Ctrl = snap to grid)") });
			if (I.bMesh) B.Add({ TEXT("Alt + Arrows"), TEXT("duplicate flush (Ctrl+Alt = stack)") });
			B.Add({ TEXT("Del"), TEXT("delete it") });
			break;
		}
		case 7:   // several loose objects
			B = { { TEXT("Space"), TEXT("group them, or save as a block") },
				  { TEXT("Drag one"), TEXT("move them all (Ctrl = snap)") },
				  { TEXT("Del"), TEXT("delete the selection") } };
			break;
		case 8:   // one group / block as a unit
		{
			const BF6Api::FSelInfo I = BF6Api::SelectionInfo();
			B = { { TEXT("Double-click"), I.bBlock ? TEXT("edit inside the block - Enter updates every copy") : TEXT("edit inside the group") },
				  { TEXT("Drag it"), TEXT("move the whole thing (Ctrl = snap)") },
				  { TEXT("Space"), TEXT("grouping, attributes, multiply") },
				  { TEXT("Ctrl + D"), TEXT("duplicate the whole thing") },
				  { TEXT("Del"), TEXT("delete it") } };
			break;
		}
		case 9:   // live scatter editor
			if (BF6Api::IsScatterDrawing())
				B = { { TEXT("Click"), TEXT("add a corner to the outline") },
					  { TEXT("Drag dot"), TEXT("move a corner") },
					  { TEXT("Ctrl + LMB"), TEXT("insert on an edge") },
					  { TEXT("Del / Ctrl+RMB"), TEXT("remove the corner") },
					  { TEXT("Ctrl + Z"), TEXT("undo an outline edit") },
					  { TEXT("Enter"), TEXT("finish the outline") },
					  { TEXT("Esc"), TEXT("throw the outline away") } };
			else
			{
				B = { { TEXT("Sliders"), TEXT("the scatter re-forms as you drag") } };
				if (BF6Api::GetScatterShape() == 3)
				{
					B.Add({ TEXT("Drag dot"), TEXT("move a corner") });
					B.Add({ TEXT("Ctrl + LMB"), TEXT("insert a corner on an edge") });
					B.Add({ TEXT("Del / Ctrl+RMB"), TEXT("remove the corner") });
					B.Add({ TEXT("Ctrl + Z"), TEXT("undo an outline edit") });
				}
				B.Add({ TEXT("New pattern"), TEXT("re-roll the layout") });
				B.Add({ TEXT("Enter"), TEXT("keep it (one undo removes all)") });
				B.Add({ TEXT("Esc"), TEXT("remove the scatter") });
			}
			break;
		case 10:  // carrying a selection with the cursor
			B = { { TEXT("Move mouse"), TEXT("it rides the cursor on the terrain") },
				  { TEXT("Click / Enter"), TEXT("place it here (one undo reverts)") },
				  { TEXT("Hold Ctrl"), TEXT("snap to the metre grid") },
				  { TEXT("Esc"), TEXT("put it back where it was") } };
			break;
		default:  // nothing selected: placement
			B = { { TEXT("Space"), TEXT("place objects") },
				  { TEXT("Hold Ctrl"), TEXT("snap while dragging") } };
			if (BF6_GodotCameraOn())
			{
				B.Add({ TEXT("LMB drag"), TEXT("box select on empty ground") });
				B.Add({ TEXT("MMB"), TEXT("orbit   (Shift = pan)") });
			}
			B.Add({ TEXT("F1"), TEXT("all controls") });
			break;
		}
		for (const TPair<FString, FString>& P : B)
		{
			Rows->AddSlot().AutoHeight().Padding(0, 2)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(104.f).HAlign(HAlign_Left)
					[
						SNew(SBorder).BorderImage(PanelLightBrush()).Padding(FMargin(7, 1))
						[ SNew(STextBlock).Font(FontBold(9)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(P.Key)) ]
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(7, 0, 0, 0)
				[ SNew(STextBlock).Font(FontReg(10)).AutoWrapText(true).ColorAndOpacity(FSlateColor(BF6Theme::TextBlue)).Text(FText::FromString(P.Value)) ]
			];
		}
	}
};

// A wrapper that makes any content a drag source for placing one object
// (the pop-out preview's DRAG INTO WORLD grip).
class SBF6DragSource : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6DragSource) {}
		SLATE_ARGUMENT(FString, PlaceableType)
		SLATE_DEFAULT_SLOT(FArguments, Content)
	SLATE_END_ARGS()
	void Construct(const FArguments& InArgs)
	{
		Type = InArgs._PlaceableType;
		ChildSlot[ InArgs._Content.Widget ];
	}
	virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& E) override
	{
		return E.GetEffectingButton() == EKeys::LeftMouseButton
			? FReply::Handled().DetectDrag(SharedThis(this), EKeys::LeftMouseButton)
			: FReply::Unhandled();
	}
	virtual FReply OnDragDetected(const FGeometry&, const FPointerEvent&) override
	{
		return FReply::Handled().BeginDragDrop(FBF6LibDragOp::New(Type));
	}
private:
	FString Type;
};

// ---------------------------------------------------------------------------
// SCATTER live editor (right side, Proton-Scatter feel). Docks while a
// scatter session is live: the scatter re-forms in the viewport as the
// sliders move, and every copy draws its OWN random rotation, elevation,
// and size inside the limits set here. New pattern re-rolls the layout;
// Apply keeps it as one undoable action; Cancel (or Esc) removes it all.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Fixed-camera live preview: docks whenever a camera object (DeployCam and
// friends) is selected, showing exactly what that camera sees - the picture
// updates in real time while the camera is moved. "Set from view" hands the
// camera the editor's current view; "Look through" jumps the editor to the
// camera's.
// ---------------------------------------------------------------------------
class SBF6CameraPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6CameraPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		Brush = MakeShared<FSlateBrush>();
		Brush->ImageSize = FVector2D(320.f, 180.f);
		ChildSlot
		[
			SNew(SBox)
			.Visibility_Lambda([]{ return BF6Api::CameraPreviewTarget() ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				SNew(SBorder).BorderImage(PanelBrush()).Padding(10.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[ SNew(STextBlock).Font(FontBold(12)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(TEXT("CAMERA VIEW"))) ]
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
							.Text_Lambda([]
							{
								AActor* A = BF6Api::CameraPreviewTarget();
								return FText::FromString(A ? A->GetActorLabel() : FString());
							}) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[
						SNew(SBox).WidthOverride(320.f).HeightOverride(180.f)
						[ SNew(SImage).Image_Lambda([this]
							{
								UTexture* T = BF6Api::CameraPreviewTexture();
								if (Brush->GetResourceObject() != T) Brush->SetResourceObject(T);
								return Brush.Get();
							}) ]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
						[
							SNew(SBox).ToolTip(BF6_MakeHint(TEXT("Set from view"), TEXT("The camera takes the editor's CURRENT view - frame the shot by flying the viewport, then click this. One Ctrl+Z reverts.")))
							[ MakePrimaryButton(TEXT("Set from view"), []{ BF6Api::SetCameraFromView(); }) ]
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SBox).ToolTip(BF6_MakeHint(TEXT("Look through"), TEXT("Jumps the editor viewport to the camera's own view, so you can judge the framing full-screen.")))
							[ MakeToolButton(TEXT("Look through"), []{ BF6Api::LookThroughCamera(); }) ]
						]
						+ SHorizontalBox::Slot().FillWidth(1)[ SNullWidget::NullWidget ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("live"))) ]
					]
				]
			]
		];
	}

private:
	TSharedPtr<FSlateBrush> Brush;
};

// The slider's number readout, clickable: type an exact value, and typing
// past the slider's current top STRETCHES that slider's range (up to a sane
// hard cap per row). Esc while typing keeps the old value.
class SBF6ScatterValue : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6ScatterValue)
		: _Value(nullptr), _MaxPtr(nullptr), _Min(0.f), _HardMax(1e9f), _DisplayScale(1.f), _Dirty(nullptr) {}
		SLATE_ARGUMENT(float*, Value)
		SLATE_ARGUMENT(float*, MaxPtr)
		SLATE_ARGUMENT(float, Min)
		SLATE_ARGUMENT(float, HardMax)
		// the readout's unit vs the internal value (wobble/lean show the
		// +/- HALF arc): typed numbers are read in READOUT units
		SLATE_ARGUMENT(float, DisplayScale)
		SLATE_ARGUMENT(TFunction<FString(float)>, Fmt)
		SLATE_ARGUMENT(bool*, Dirty)
	SLATE_END_ARGS()

	void Construct(const FArguments& A)
	{
		Value = A._Value; MaxPtr = A._MaxPtr; Min = A._Min; HardMax = A._HardMax;
		Scale = A._DisplayScale > 0.f ? A._DisplayScale : 1.f;
		Fmt = A._Fmt; Dirty = A._Dirty;
		ChildSlot
		[
			SAssignNew(Switcher, SWidgetSwitcher)
			+ SWidgetSwitcher::Slot()
			[
				SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(2, 0))
				.ToolTip(BF6_MakeHint(TEXT("Type a value"), TEXT("Click to type an exact number. Typing past the slider's top stretches the slider's range.")))
				.OnClicked_Lambda([this]
				{
					if (Box.IsValid() && Switcher.IsValid() && Value)
					{
						Box->SetText(FText::FromString(FString::Printf(TEXT("%g"), *Value * Scale)));
						Switcher->SetActiveWidgetIndex(1);
						FSlateApplication::Get().SetKeyboardFocus(Box, EFocusCause::SetDirectly);
					}
					return FReply::Handled();
				})
				[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(FLinearColor::White))
					.Text_Lambda([this]{ return FText::FromString(Fmt && Value ? Fmt(*Value) : FString()); }) ]
			]
			+ SWidgetSwitcher::Slot()
			[
				SAssignNew(Box, SEditableTextBox).Font(FontReg(9)).SelectAllTextWhenFocused(true)
				.OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type How)
				{
					if (How != ETextCommit::OnCleared)   // Esc keeps the old value
					{
						const FString S = T.ToString().TrimStartAndEnd();
						if (!S.IsEmpty() && S.IsNumeric() && Value)
						{
							// typed in READOUT units -> internal, then clamp
							float V = FMath::Clamp(FCString::Atof(*S) / Scale, Min, HardMax);
							if (MaxPtr && V > *MaxPtr) *MaxPtr = V;
							*Value = V;
							if (Dirty) *Dirty = true;
						}
					}
					if (Switcher.IsValid()) Switcher->SetActiveWidgetIndex(0);
				})
			]
		];
	}

private:
	float* Value = nullptr;
	float* MaxPtr = nullptr;
	float Min = 0.f, HardMax = 1e9f, Scale = 1.f;
	TFunction<FString(float)> Fmt;
	bool* Dirty = nullptr;
	TSharedPtr<SWidgetSwitcher> Switcher;
	TSharedPtr<SEditableTextBox> Box;
};

class SBF6ScatterPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6ScatterPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		// MaxP is per-row state: typing a bigger number into the readout
		// stretches that slider's range (never past HardMax). DisplayScale
		// maps internal units to the readout's (the +/- rows show HALF the
		// internal arc, and typed numbers are read in readout units).
		auto Row = [this](const TCHAR* Label, float Min, float* MaxP, float HardMax, float DisplayScale, float* Val, TFunction<FString(float)> Fmt, const TCHAR* HintTitle, const TCHAR* HintBody)
		{
			return SNew(SBox).ToolTip(BF6_MakeHint(HintTitle, HintBody))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[ SNew(SBox).WidthOverride(74.f)
					[ SNew(STextBlock).Font(FontBold(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(Label)) ] ]
				+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
				[ SNew(SSlider)
					.Value_Lambda([Val, Min, MaxP]{ return (*Val - Min) / FMath::Max(*MaxP - Min, 0.001f); })
					.OnValueChanged_Lambda([this, Val, Min, MaxP](float v){ *Val = Min + v * (*MaxP - Min); bDirty = true; })
					.SliderBarColor(BF6Theme::AccentDim)
					.SliderHandleColor(BF6Theme::Accent) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
				[ SNew(SBox).WidthOverride(58.f).HAlign(HAlign_Right)
					[ SNew(SBF6ScatterValue).Value(Val).MaxPtr(MaxP).Min(Min).HardMax(HardMax).DisplayScale(DisplayScale).Fmt(Fmt).Dirty(&bDirty) ] ]
			];
		};

		auto ShapeBtn = [](const TCHAR* Label, int32 Shape) -> TSharedRef<SWidget>
		{
			return SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(6, 2))
				.OnClicked_Lambda([Shape]{ BF6Api::SetScatterShape(Shape); return FReply::Handled(); })
				[ SNew(STextBlock).Font(FontBold(9))
					.ColorAndOpacity_Lambda([Shape]{ return FSlateColor(BF6Api::GetScatterShape() == Shape ? BF6Theme::Accent : BF6Theme::TextDim); })
					.Text(FText::FromString(Label)) ];
		};

		ChildSlot
		[
			SNew(SBox).WidthOverride(320.f)
			.Visibility_Lambda([]{ return BF6Api::IsScatterLive() ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				SNew(SBorder).BorderImage(PanelBrush()).Padding(12.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 2)
					[ SNew(STextBlock).Font(FontBold(13)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(TEXT("SCATTER"))) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
					[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).AutoWrapText(true)
						.Text(FText::FromString(TEXT("Live - the scatter re-forms as you drag. Each copy rolls its own rotation, elevation, and size inside these limits."))) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[ Row(TEXT("COUNT"), 1.f, &MaxCount, 1000.f, 1.f, &CountF, [](float v){ return FString::Printf(TEXT("%d"), FMath::RoundToInt(v)); },
						TEXT("Count"), TEXT("How many copies to scatter inside the circle. Spacing adapts so the count always fits.")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[ Row(TEXT("RADIUS"), 2.f, &MaxRadius, 1000.f, 1.f, &RadiusM, [](float v){ return FString::Printf(TEXT("%.0f m"), v); },
						TEXT("Radius"), TEXT("The size of the circle around the selection the copies land in.")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[ Row(TEXT("ROTATION"), 0.f, &MaxRot, 360.f, 1.f, &RotDeg, [](float v){ return FString::Printf(TEXT("%.0f deg"), v); },
						TEXT("Rotation limit"), TEXT("Each copy turns a random amount inside this arc. 360 = any direction, 0 = every copy faces the same way as the original.")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[
						SNew(SBox).Visibility_Lambda([this]{ return bFineWob ? EVisibility::Collapsed : EVisibility::Visible; })
						[ Row(TEXT("WOBBLE"), 0.f, &MaxWob, 180.f, 0.5f, &WobbleF, [](float v){ return FString::Printf(TEXT("+/- %.0f deg"), v * 0.5f); },
							TEXT("Wobble limit"), TEXT("Each copy leans over a random amount inside this arc, in a random direction - trees and rocks stop looking machine-planted. Fine tune splits it into separate X and Y lean limits.")) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[
						SNew(SBox).Visibility_Lambda([this]{ return bFineWob ? EVisibility::Visible : EVisibility::Collapsed; })
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
							[ Row(TEXT("LEAN X"), 0.f, &MaxTiltX, 360.f, 0.5f, &TiltXF, [](float v){ return FString::Printf(TEXT("+/- %.0f deg"), v * 0.5f); },
								TEXT("Lean X limit"), TEXT("Each copy rolls a random amount inside this arc on the X axis.")) ]
							+ SVerticalBox::Slot().AutoHeight()
							[ Row(TEXT("LEAN Y"), 0.f, &MaxTiltY, 360.f, 0.5f, &TiltYF, [](float v){ return FString::Printf(TEXT("+/- %.0f deg"), v * 0.5f); },
								TEXT("Lean Y limit"), TEXT("Each copy pitches a random amount inside this arc on the Y axis.")) ]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([this]{ return bFineWob ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this](ECheckBoxState S)
						{
							bFineWob = S == ECheckBoxState::Checked;
							// carry the current wobble into the split sliders (and back)
							if (bFineWob) { TiltXF = WobbleF; TiltYF = WobbleF; }
							else WobbleF = FMath::Max(TiltXF, TiltYF);
							bDirty = true;
						})
						[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("fine tune wobble (separate X / Y)"))) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[ Row(TEXT("ELEVATION"), 0.f, &MaxElev, 100.f, 1.f, &ElevM, [](float v){ return FString::Printf(TEXT("+/- %.1f m"), v); },
						TEXT("Elevation limit"), TEXT("Each copy shifts up or down a random amount within this range AFTER landing on the ground. Handy for sinking rocks and varying tree bases.")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[ Row(TEXT("SIZE"), 0.f, &MaxVary, 100.f, 1.f, &VaryPct, [](float v){ return FString::Printf(TEXT("+/- %.0f%%"), v); },
						TEXT("Size limit"), TEXT("Each copy grows or shrinks a random amount within this percentage.")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[ SNew(SBox).WidthOverride(74.f)
							[ SNew(STextBlock).Font(FontBold(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("SHAPE"))) ] ]
						+ SHorizontalBox::Slot().AutoWidth()[ ShapeBtn(TEXT("CIRCLE"), 0) ]
						+ SHorizontalBox::Slot().AutoWidth()[ ShapeBtn(TEXT("SQUARE"), 1) ]
						+ SHorizontalBox::Slot().AutoWidth()[ ShapeBtn(TEXT("RING"), 2) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0)
						[
							SNew(SBox).ToolTip(BF6_MakeHint(TEXT("Draw an area"), TEXT("Outline any area yourself: click corners on the map and the scatter fills the outline live from the third corner on. Enter finishes the outline, Esc throws it away.")))
							[
								SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(6, 2))
								.OnClicked_Lambda([]{ BF6Api::BeginScatterDraw(); return FReply::Handled(); })
								[ SNew(STextBlock).Font(FontBold(9))
									.ColorAndOpacity_Lambda([]{ return FSlateColor(BF6Api::GetScatterShape() == 3 ? BF6Theme::Accent : BF6Theme::TextDim); })
									.Text(FText::FromString(TEXT("DRAW..."))) ]
							]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[
						SNew(SBox).ToolTip(BF6_MakeHint(TEXT("Follow terrain"), TEXT("On: every copy drops onto whatever is under it. Off: every copy stays at the original object's height - for scattering across a flat build or in the air.")))
						[
							SNew(SCheckBox)
							.IsChecked_Lambda([]{ return BF6Api::GetScatterFollowTerrain() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
							.OnCheckStateChanged_Lambda([](ECheckBoxState S){ BF6Api::SetScatterFollowTerrain(S == ECheckBoxState::Checked); })
							[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("follow terrain"))) ]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[
						SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).AutoWrapText(true)
						.Visibility_Lambda([]{ return BF6Api::IsScatterDrawing() ? EVisibility::Visible : EVisibility::Collapsed; })
						.Text_Lambda([]
						{
							const int32 n = BF6Api::ScatterDrawPointCount();
							return FText::FromString(n < 3
								? FString::Printf(TEXT("Click corners on the map (%d so far - needs 3+)."), n)
								: FString::Printf(TEXT("%d corners - keep clicking, Enter when done."), n));
						})
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth()
						[ MakeToolButton(TEXT("New pattern"), [this]{ Seed++; bDirty = true; }) ]
						+ SHorizontalBox::Slot().FillWidth(1)[ SNullWidget::NullWidget ]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
						[ MakePrimaryButton(TEXT("Apply"), []{ BF6Api::ApplyScatterLive(); }) ]
						+ SHorizontalBox::Slot().AutoWidth()
						[ MakeToolButton(TEXT("Cancel"), []{ BF6Api::CancelScatterLive(); }) ]
						+ SHorizontalBox::Slot().FillWidth(1)[ SNullWidget::NullWidget ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("Enter / Esc"))) ]
					]
				]
			]
		];
	}

	virtual void Tick(const FGeometry& Geo, const double CurTime, const float Dt) override
	{
		SCompoundWidget::Tick(Geo, CurTime, Dt);
		const bool bLive = BF6Api::IsScatterLive();
		// session counter, not a rising-edge flag: the panel is collapsed
		// (and never ticks) between sessions, so an edge would be missed
		if (bLive && BF6Api::ScatterSession() != LastSession)
		{
			LastSession = BF6Api::ScatterSession();
			// fresh session: sliders start on the engine's opening recipe
			CountF = 24.f; RadiusM = 20.f; RotDeg = 360.f; ElevM = 0.f; VaryPct = 15.f;
			WobbleF = 0.f; TiltXF = 0.f; TiltYF = 0.f; bFineWob = false;
			MaxCount = 200.f; MaxRadius = 100.f; MaxRot = 360.f; MaxWob = 45.f;
			MaxTiltX = 90.f; MaxTiltY = 90.f; MaxElev = 10.f; MaxVary = 100.f;
			Seed = 1;
			bDirty = false;
		}
		// throttle the rebuilds so dragging stays smooth
		if (bLive && bDirty && CurTime - LastPush > 0.12)
		{
			LastPush = CurTime;
			bDirty = false;
			BF6Api::UpdateScatterLive(FMath::RoundToInt(CountF), RadiusM, RotDeg,
				bFineWob ? TiltXF : WobbleF, bFineWob ? TiltYF : WobbleF,
				ElevM, VaryPct / 100.f, Seed);
		}
	}

private:
	float CountF = 24.f, RadiusM = 20.f, RotDeg = 360.f, ElevM = 0.f, VaryPct = 15.f;
	float WobbleF = 0.f, TiltXF = 0.f, TiltYF = 0.f;
	// per-slider tops: typing a bigger number into a readout stretches these
	float MaxCount = 200.f, MaxRadius = 100.f, MaxRot = 360.f, MaxWob = 45.f;
	float MaxTiltX = 90.f, MaxTiltY = 90.f, MaxElev = 10.f, MaxVary = 100.f;
	int32 Seed = 1, LastSession = -1;
	bool bDirty = false, bFineWob = false;
	double LastPush = 0.0;
};

// ---------------------------------------------------------------------------
// The zone's point handles, Godot-style: little orange screen-space dots at a
// constant pixel size, painted over the viewport. Dragging happens directly in
// the input processor (no gizmo); this widget only draws.
// ---------------------------------------------------------------------------
class SBF6ZoneDots : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6ZoneDots) {}
	SLATE_END_ARGS()
	void Construct(const FArguments&)
	{
		SetVisibility(EVisibility::HitTestInvisible);
		ChildSlot[ SNullWidget::NullWidget ];
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
	{
		// ---- special-mode chrome, Revit style: the whole viewport gets a
		// coloured frame plus a top banner naming the mode and its exits, so
		// nobody is ever silently inside assign / group / block / zone editing
		{
			FString Title, Hint;
			FLinearColor Col = BF6Theme::Accent;
			bool bMode = true;
			if (BF6Api::IsLinkPicking())
			{
				Col = FLinearColor(0.13f, 1.f, 0.27f);
				Title = BF6Api::LinkPickLabel();
				Hint = TEXT("Click the glowing targets  -  Space or Enter confirms  -  Esc backs out");
			}
			else if (BF6Api::IsGroupEditing() && BF6Api::GroupEditIsBlock())
			{
				Col = FLinearColor(0.25f, 0.55f, 1.f);
				Title = TEXT("EDITING BLOCK");
				Hint = TEXT("Enter keeps and updates every copy  -  Esc reverts");
			}
			else if (BF6Api::IsGroupEditing())
			{
				Title = TEXT("EDITING GROUP");
				Hint = TEXT("Enter keeps  -  Esc reverts");
			}
			else if (BF6Api::IsPickPlacing())
			{
				Title = TEXT("CARRYING OBJECT");
				Hint = TEXT("Click to set it down  -  Esc puts it back");
			}
			else if (BF6Api::IsScatterLive())
			{
				Col = FLinearColor(0.25f, 0.55f, 1.f);
				Title = TEXT("SCATTER");
				Hint = TEXT("Enter keeps the scatter  -  Esc removes it");
			}
			else if (BF6Api::IsVolumeEditing() || BF6Api::IsObbEditing())
			{
				Title = TEXT("EDITING ZONE SHAPE");
				Hint = TEXT("Enter or Esc finishes");
			}
			else if (BF6Api::IsModeWizardActive())
			{
				Title = TEXT("MODE SETUP");
				Hint = TEXT("Click to place each step  -  Esc stops the setup");
			}
			// no custom level open: the whole build screen is a read-only view
			// of the stock map - say so permanently, or users place and move
			// things that silently never save
			else if (!BF6Api::IsEditing())
			{
				Col = FLinearColor(1.f, 0.72f, 0.f);
				Title = TEXT("BASE MAP  -  READ ONLY");
				Hint = TEXT("Nothing you place or move here is kept. Name your map and hit Create in the bottom right, or go back to < Maps and resume a custom level.");
			}
			else bMode = false;
			if (bMode)
			{
				// paint FAR above the sibling widgets (library panel, budget bar,
				// pie hints all draw later with higher layers) - the frame must
				// outline the whole screen ON TOP of every panel, Revit style
				const int32 CL = LayerId + 5000;
				const FVector2D L = AllottedGeometry.GetLocalSize();
				const FSlateBrush* WB = FCoreStyle::Get().GetBrush("GenericWhiteBox");
				const FLinearColor Frame = Col.CopyWithNewOpacity(0.85f);
				const float T = 3.f;
				auto Box = [&](const FVector2D& Pos, const FVector2D& Size, const FLinearColor& C, const FSlateBrush* B, int32 Layer)
				{
					FSlateDrawElement::MakeBox(OutDrawElements, Layer,
						AllottedGeometry.ToPaintGeometry(FVector2f(Size), FSlateLayoutTransform(FVector2f(Pos))), B,
						ESlateDrawEffect::None, C);
				};
				Box(FVector2D(0, 0), FVector2D(L.X, T), Frame, WB, CL);
				Box(FVector2D(0, L.Y - T), FVector2D(L.X, T), Frame, WB, CL);
				Box(FVector2D(0, T), FVector2D(T, L.Y - 2 * T), Frame, WB, CL);
				Box(FVector2D(L.X - T, T), FVector2D(T, L.Y - 2 * T), Frame, WB, CL);

				// the banner: dark pill, mode-coloured title, dim exit hints
				const FSlateFontInfo TF = FontBold(12), HF = FontReg(9);
				const TSharedRef<FSlateFontMeasure> FM = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
				const FVector2D TS = FM->Measure(Title, TF), HS = FM->Measure(Hint, HF);
				const float PW = FMath::Max(TS.X, HS.X) + 36.f, PH = TS.Y + HS.Y + 18.f;
				const FVector2D PP((L.X - PW) * 0.5f, 10.f);
				static const FSlateRoundedBoxBrush Pill(FLinearColor::White, 9.f);
				Box(PP, FVector2D(PW, PH), FLinearColor(0.05f, 0.06f, 0.07f, 0.92f), &Pill, CL + 1);
				Box(FVector2D(PP.X, PP.Y + PH - 2.f), FVector2D(PW, 2.f), Frame, WB, CL + 2);
				FSlateDrawElement::MakeText(OutDrawElements, CL + 2,
					AllottedGeometry.ToPaintGeometry(FVector2f(PW, TS.Y),
						FSlateLayoutTransform(FVector2f(PP.X + (PW - TS.X) * 0.5f, PP.Y + 7.f))),
					Title, TF, ESlateDrawEffect::None, Col);
				FSlateDrawElement::MakeText(OutDrawElements, CL + 2,
					AllottedGeometry.ToPaintGeometry(FVector2f(PW, HS.Y),
						FSlateLayoutTransform(FVector2f(PP.X + (PW - HS.X) * 0.5f, PP.Y + 9.f + TS.Y))),
					Hint, HF, ESlateDrawEffect::None, BF6Theme::TextDim);
			}
		}

		// Godot-style marquee: translucent accent box while LMB-dragging on
		// empty ground
		{
			FVector2D RA, RB;
			if (BF6Api::GetBoxSelectRect(RA, RB))
			{
				const float BS = AllottedGeometry.Scale;
				const FVector2D LA = RA / BS, LB = RB / BS;
				const FVector2D Size = LB - LA;
				if (Size.X > 1.f && Size.Y > 1.f)
				{
					FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
						AllottedGeometry.ToPaintGeometry(FVector2f(Size), FSlateLayoutTransform(FVector2f(LA))),
						FCoreStyle::Get().GetBrush("GenericWhiteBox"),
						ESlateDrawEffect::None, BF6Theme::Accent.CopyWithNewOpacity(0.07f));
					TArray<FVector2f> Border = { FVector2f(LA), FVector2f(LB.X, LA.Y), FVector2f(LB), FVector2f(LA.X, LB.Y), FVector2f(LA) };
					FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 1, AllottedGeometry.ToPaintGeometry(),
						Border, ESlateDrawEffect::None, BF6Theme::Accent, true, 1.4f);
				}
			}
		}

		// assign mode: lines from the owner + colour-coded candidate markers
		{
			TArray<FVector2D> LPx; TArray<uint8> LState; FVector2D OwnerPx; bool bOwner = false;
			if (BF6Api::GetLinkOverlay(LPx, LState, OwnerPx, bOwner))
			{
				// the same neon trio the meshes glow in (see kLinkNeon engine-side):
				// free cyan / assigned green / pending orange - the line to the
				// owner carries the target's colour so links read at a glance
				const float LS = AllottedGeometry.Scale;
				static const FSlateRoundedBoxBrush FreeDot(FLinearColor(0.f, 0.9f, 1.f), 8.f, FLinearColor::White, 1.5f);
				static const FSlateRoundedBoxBrush AssignedDot(FLinearColor(0.22f, 1.f, 0.08f), 9.f, FLinearColor::White, 2.f);
				static const FSlateRoundedBoxBrush PendingDot(FLinearColor(1.f, 0.63f, 0.f), 9.f, FLinearColor::White, 2.f);
				const FLinearColor NeonCyan(0.f, 0.9f, 1.f), NeonGreen(0.22f, 1.f, 0.08f), NeonOrange(1.f, 0.63f, 0.f);
				for (int32 i = 0; i < LPx.Num(); i++)
				{
					if (LPx[i].X < -999.f) continue;
					const uint8 St = LState.IsValidIndex(i) ? LState[i] : 0;
					if (bOwner && St != 0)
					{
						TArray<FVector2f> Line = { FVector2f(OwnerPx / LS), FVector2f(LPx[i] / LS) };
						FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 1, AllottedGeometry.ToPaintGeometry(),
							Line, ESlateDrawEffect::None,
							(St == 2 ? NeonOrange : NeonGreen).CopyWithNewOpacity(0.95f), true, 2.4f);
					}
					const FSlateRoundedBoxBrush* B = St == 2 ? &PendingDot : St == 1 ? &AssignedDot : &FreeDot;
					const float Sz = St == 0 ? 16.f : 18.f;
					const FVector2D Local = LPx[i] / LS - FVector2D(Sz, Sz) * 0.5f;
					FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 2,
						AllottedGeometry.ToPaintGeometry(FVector2f(Sz, Sz), FSlateLayoutTransform(FVector2f(Local))),
						B, ESlateDrawEffect::None,
						St == 2 ? NeonOrange : St == 1 ? NeonGreen : NeonCyan);
				}
			}
		}

		// scatter outline corners: blue dots + the outline itself, painted the
		// same way as zone points (the translucent region mesh is 3D-side)
		{
			TArray<FVector2D> SPx; int32 SDrag = -1;
			if (BF6Api::GetScatterDots(SPx, SDrag))
			{
				const float SS = AllottedGeometry.Scale;
				const FLinearColor Blue(0.25f, 0.55f, 1.f);
				// the outline between corners (closed once there are 3+)
				TArray<FVector2f> Line;
				for (int32 i = 0; i <= SPx.Num(); i++)
				{
					const FVector2D& P = SPx[i % SPx.Num()];
					if (P.X < -999.f) { Line.Reset(); break; }
					if (i == SPx.Num() && SPx.Num() < 3) break;   // open while still short
					Line.Add(FVector2f(P / SS));
				}
				if (Line.Num() >= 2)
					FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 1,
						AllottedGeometry.ToPaintGeometry(), Line, ESlateDrawEffect::None, Blue, true, 2.f);
				static const FSlateRoundedBoxBrush BlueDot16(FLinearColor(0.25f, 0.55f, 1.f), 8.f, FLinearColor::White, 2.f);
				static const FSlateRoundedBoxBrush BlueDot20(FLinearColor(0.25f, 0.55f, 1.f), 10.f, FLinearColor::White, 2.5f);
				const FSlateBrush* SQ = FCoreStyle::Get().GetBrush("GenericWhiteBox");
				for (int32 i = 0; i < SPx.Num(); i++)
				{
					if (SPx[i].X < -999.f) continue;
					const bool bBig = i == SDrag;
					const float Sz = bBig ? 20.f : 16.f;
					const FVector2D Local = SPx[i] / SS - FVector2D(Sz, Sz) * 0.5f;
					FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
						AllottedGeometry.ToPaintGeometry(FVector2f(bBig ? 12.f : 10.f, bBig ? 12.f : 10.f),
							FSlateLayoutTransform(FVector2f(SPx[i] / SS - FVector2D(bBig ? 6.f : 5.f, bBig ? 6.f : 5.f)))),
						SQ, ESlateDrawEffect::None, Blue);   // fail-safe core
					FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 2,
						AllottedGeometry.ToPaintGeometry(FVector2f(Sz, Sz), FSlateLayoutTransform(FVector2f(Local))),
						bBig ? &BlueDot20 : &BlueDot16, ESlateDrawEffect::None, Blue);
				}
				// Ctrl edge preview: white insert dot with a blue ring
				FVector2D EPx;
				if (BF6Api::GetScatterEdgePreview(EPx))
				{
					static const FSlateRoundedBoxBrush BlueEdge14(FLinearColor::White, 7.f, FLinearColor(0.25f, 0.55f, 1.f), 2.f);
					FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 2,
						AllottedGeometry.ToPaintGeometry(FVector2f(14.f, 14.f),
							FSlateLayoutTransform(FVector2f(EPx / SS - FVector2D(7.f, 7.f)))),
						&BlueEdge14, ESlateDrawEffect::None, FLinearColor::White);
				}
			}
		}

		TArray<FVector2D> Px; int32 PointCount = 0, Active = -1, Drag = -1; FVector2D EdgePx; bool bEdge = false;
		const bool bHave = BF6Api::GetZoneDots(Px, PointCount, Active, Drag, EdgePx, bEdge);
		if (!bHave) return LayerId;

		// Godot-handle orange circles with white rings. The corner radius MUST be
		// exactly half the box size (an oversized radius can cut the whole box
		// away); a plain tinted square is drawn underneath as a fail-safe so the
		// handles are visible even if the rounded-box shader misbehaves.
		static const FSlateRoundedBoxBrush Dot16(FLinearColor(FColor(0xFF, 0x8A, 0x00)), 8.f, FLinearColor::White, 2.f);
		static const FSlateRoundedBoxBrush Dot20(FLinearColor(FColor(0xFF, 0x8A, 0x00)), 10.f, FLinearColor::White, 2.5f);
		static const FSlateRoundedBoxBrush Edge14(FLinearColor::White, 7.f, FLinearColor(FColor(0xFF, 0x8A, 0x00)), 2.f);
		const FSlateBrush* Square = FCoreStyle::Get().GetBrush("GenericWhiteBox");
		const FLinearColor OrangeTint(FColor(0xFF, 0x8A, 0x00));

		const float S = AllottedGeometry.Scale;   // viewport pixels -> local units
		auto DrawAt = [&](const FVector2D& P, float Size, const FSlateBrush* Brush, const FLinearColor& Tint)
		{
			const FVector2D Local = P / S - FVector2D(Size, Size) * 0.5f;
			FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
				AllottedGeometry.ToPaintGeometry(FVector2f(Size, Size), FSlateLayoutTransform(FVector2f(Local))),
				Brush, ESlateDrawEffect::None, Tint);
		};
		for (int32 i = 0; i < Px.Num(); i++)
		{
			if (Px[i].X < -999.f) continue;   // behind the camera
			const bool bBig = (i == Drag) || (PointCount > 0 && (i % PointCount) == Active);
			DrawAt(Px[i], bBig ? 12.f : 10.f, Square, OrangeTint);                    // fail-safe core
			// the rounded-box shader takes the ELEMENT tint as its fill color
			DrawAt(Px[i], bBig ? 20.f : 16.f, bBig ? &Dot20 : &Dot16, OrangeTint);
		}
		if (bEdge)
		{
			DrawAt(EdgePx, 9.f, Square, FLinearColor::White);
			DrawAt(EdgePx, 14.f, &Edge14, FLinearColor::White);
		}
		return LayerId + 1;
	}
};

static void BF6_MiniToast(const FString& Msg);   // defined with the toast helpers below

// Invisible full-viewport widget that only hit-tests while a library drag is in
// flight, so the drop lands on the map instead of dying on the level viewport.
class SBF6DropCatcher : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6DropCatcher) {}
	SLATE_END_ARGS()
	void Construct(const FArguments&)
	{
		SetVisibility(TAttribute<EVisibility>::CreateLambda([]
			{ return FBF6LibDragOp::bActive ? EVisibility::Visible : EVisibility::SelfHitTestInvisible; }));
		ChildSlot[ SNullWidget::NullWidget ];
	}
	virtual FReply OnDragOver(const FGeometry& G, const FDragDropEvent& E) override
	{
		TSharedPtr<FBF6LibDragOp> Op = E.GetOperationAs<FBF6LibDragOp>();
		if (!Op.IsValid()) return FReply::Unhandled();
		// read-only base: the drop WILL refuse, so say it now, loudly, once -
		// a fresh user's first drag otherwise just "disappears"
		if (!BF6Api::IsEditing())
		{
			if (!FBF6LibDragOp::bWarnedReadOnly)
			{
				FBF6LibDragOp::bWarnedReadOnly = true;
				BF6_MiniToast(TEXT("This base map is read-only. Type a name and click Create (bottom right) to start your custom map - then objects can be placed."));
			}
			return FReply::Handled();
		}
		// live ghost: the actual model rides the cursor across the terrain
		const FVector2D Px = G.AbsoluteToLocal(E.GetScreenSpacePosition()) * G.Scale;
		FVector W;
		if (BF6Api::WorldFromViewportPoint(Px, W)) BF6Api::UpdateDragGhost(Op->PlaceableType, W);
		return FReply::Handled();
	}
	virtual void OnDragLeave(const FDragDropEvent& E) override
	{
		// dragging back onto the library strip: hide the ghost until re-entry
		if (E.GetOperationAs<FBF6LibDragOp>().IsValid()) BF6Api::DestroyDragGhost();
	}
	virtual FReply OnDrop(const FGeometry& G, const FDragDropEvent& E) override
	{
		TSharedPtr<FBF6LibDragOp> Op = E.GetOperationAs<FBF6LibDragOp>();
		if (!Op.IsValid()) return FReply::Unhandled();
		BF6Api::DestroyDragGhost();
		// the DROP EVENT's position, never the cached mouse pos - that froze
		// while the drag was in flight, so drops stopped landing under the
		// cursor. The catcher fills the viewport, so local units * scale =
		// render pixels.
		const FVector2D Px = G.AbsoluteToLocal(E.GetScreenSpacePosition()) * G.Scale;
		FVector W;
		if (BF6Api::WorldFromViewportPoint(Px, W) || BF6Api::WorldFromViewportCursor(W))
		{
			// "block::<name>" payloads place a whole prefab
			if (Op->PlaceableType.StartsWith(TEXT("block::"))) BF6Api::PlaceBlock(Op->PlaceableType.Mid(7), W);
			else BF6Api::PlaceType(Op->PlaceableType, W);
		}
		return FReply::Handled();
	}
};

// ---------------------------------------------------------------------------
// The Object Library: a panel that slides up from the bottom edge of the
// viewport like an auto-hidden taskbar. Hover the bottom edge to peek, PIN to
// keep it up. Two scopes: THIS MAP (only what the open level allows) and FULL
// LIBRARY (everything in the SDK, reachable from the pie). Category tabs mirror
// the pie's categories; right-click a tile to move it to another category.
// ---------------------------------------------------------------------------
class SBF6LibraryPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6LibraryPanel) {}
	SLATE_END_ARGS()

	static constexpr float kBodyH = 300.f;

	void Construct(const FArguments&)
	{
		SetCanTick(true);
		// only the strip / panel borders hit-test; empty space stays clickable
		SetVisibility(EVisibility::SelfHitTestInvisible);
		bool bPinSaved = false;
		GConfig->GetBool(TEXT("BF6UnrealSDK"), TEXT("LibraryPinned"), bPinSaved, GEditorPerProjectIni);
		GConfig->GetFloat(TEXT("BF6UnrealSDK"), TEXT("LibraryTileSize"), TileSize, GEditorPerProjectIni);
		TileSize = FMath::Clamp(TileSize, 72.f, 200.f);
		bPinned = bPinSaved;
		if (bPinned) SlideTarget = 1.f;

		ChildSlot
		[
			SNew(SVerticalBox)

			// the peek strip: a slim always-there tab at the very bottom edge
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				SNew(SBox).Visibility_Lambda([this]{ return Slide < 0.05f ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(26, 3))
					.OnMouseButtonDown_Lambda([this](const FGeometry&, const FPointerEvent&){ OpenPeek(); return FReply::Handled(); })
					[ SNew(STextBlock).Font(FontBold(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("OBJECT LIBRARY"))) ]
				]
			]

			// the sliding body
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).Clipping(EWidgetClipping::ClipToBounds)
				.HeightOverride_Lambda([this]{ return FMath::Max(1.f, kBodyH * Slide); })
				.Visibility_Lambda([this]{ return Slide > 0.01f ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(12, 8))
					[
						SNew(SVerticalBox)

						// header: title / scope / search / count / pin / close
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 14, 0)
							[ SNew(STextBlock).Font(FontBold(12)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(TEXT("OBJECT LIBRARY"))) ]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[ MakeScopeButton(TEXT("THIS MAP"), false) ]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 14, 0)
							[ MakeScopeButton(TEXT("FULL LIBRARY"), true) ]
							+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
							[
								SAssignNew(SearchBox, SSearchBox)
								.HintText(FText::FromString(TEXT("Search objects...")))
								.OnTextChanged_Lambda([this](const FText& T){ Query = T.ToString(); RebuildItems(); })
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10, 0)
							[ SNew(STextBlock).Font(FontReg(10)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
								.Text_Lambda([this]{ return FText::FromString(FString::Printf(TEXT("%d shown"), Items.Num())); }) ]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
							[
								SNew(SBox).WidthOverride(110.f)
								[
									SNew(SSlider).Value_Lambda([this]{ return (TileSize - 72.f) / 128.f; })
									.OnValueChanged_Lambda([this](float V)
									{
										TileSize = 72.f + V * 128.f;
										GConfig->SetFloat(TEXT("BF6UnrealSDK"), TEXT("LibraryTileSize"), TileSize, GEditorPerProjectIni);
									})
								]
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
							[
								SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(8, 2))
								.OnClicked_Lambda([this]{ SetPinned(!bPinned); return FReply::Handled(); })
								[ SNew(STextBlock).Font(FontBold(10))
									.ColorAndOpacity_Lambda([this]{ return FSlateColor(bPinned ? BF6Theme::Accent : BF6Theme::TextDim); })
									.Text_Lambda([this]{ return FText::FromString(bPinned ? TEXT("PINNED") : TEXT("PIN")); }) ]
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[
								SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(8, 2))
								.OnClicked_Lambda([this]{ ForceClose(); return FReply::Handled(); })
								[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("CLOSE"))) ]
							]
						]

						// category tabs
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
						[ SNew(SScrollBox).Orientation(Orient_Horizontal) + SScrollBox::Slot()[ SAssignNew(CatRow, SHorizontalBox) ] ]

						// the card grid: iso thumbnail + name caption per object
						+ SVerticalBox::Slot().FillHeight(1)
						[
							SAssignNew(Tiles, STileView<TSharedPtr<BF6Api::FPlaceableInfo>>)
							.ListItemsSource(&Items).SelectionMode(ESelectionMode::Single)
							.ItemWidth(TAttribute<float>::CreateLambda([this]{ return TileSize; }))
							.ItemHeight(TAttribute<float>::CreateLambda([this]{ return TileSize + 20.f; }))
							.OnGenerateTile(this, &SBF6LibraryPanel::OnGenerateTile)
							.OnMouseButtonClick(this, &SBF6LibraryPanel::OnCardClick)
							.OnMouseButtonDoubleClick(this, &SBF6LibraryPanel::OnActivate)
						]
					]
				]
			]
		];
	}

	float CurrentHeight() const { return kBodyH * Slide; }
	bool  IsOpen() const { return SlideTarget > 0.5f; }

	void OpenPeek()
	{
		RefreshIfStale();
		SlideTarget = 1.f;
	}

	// the pie's FULL LIBRARY pill: pin it up in the everything scope
	void OpenFull()
	{
		bFull = true;
		SetPinned(true);
		RefreshIfStale(true);
		SlideTarget = 1.f;
		if (SearchBox.IsValid()) FSlateApplication::Get().SetKeyboardFocus(SearchBox);
	}

	void ForceClose()
	{
		SetPinned(false);
		SlideTarget = 0.f;
	}

	virtual void Tick(const FGeometry& G, const double T, const float D) override
	{
		SCompoundWidget::Tick(G, T, D);
		const float Prev = Slide;
		const float Step = D * 7.f;
		Slide = SlideTarget > Slide ? FMath::Min(Slide + Step, SlideTarget) : FMath::Max(Slide - Step, SlideTarget);
		// the panel lives in flow: its height change must re-layout the HUD above
		if (!FMath::IsNearlyEqual(Prev, Slide)) Invalidate(EInvalidateWidgetReason::Layout);

		// drag-out: once the cursor carries a tile OFF the strip, the panel
		// slides away so the whole viewport is free for placing; it comes
		// back when the drag ends (up if pinned, hidden if auto-hide)
		const bool bDragOut = FBF6LibDragOp::bActive
			&& !G.IsUnderLocation(FSlateApplication::Get().GetCursorPos());
		if (bDragOut)
		{
			bDragHidden = true;
			SlideTarget = 0.f;
		}
		else if (bDragHidden && !FBF6LibDragOp::bActive)
		{
			bDragHidden = false;
			SlideTarget = bPinned ? 1.f : 0.f;
		}
	}

	virtual void OnMouseEnter(const FGeometry& G, const FPointerEvent& E) override
	{
		SCompoundWidget::OnMouseEnter(G, E);
		// taskbar behavior: touching the bottom-edge strip slides the panel up
		if (!IsOpen()) OpenPeek();
	}

	virtual void OnMouseLeave(const FPointerEvent& E) override
	{
		SCompoundWidget::OnMouseLeave(E);
		// taskbar behavior: slide away unless pinned (or mid drag-out)
		if (!bPinned && !FBF6LibDragOp::bActive && !GTransientMenuUp()) SlideTarget = 0.f;
	}

private:
	static inline const FString kBlocksTab = TEXT("::blocks");
	bool bPinned = false, bFull = false, bDragHidden = false;
	float Slide = 0.f; float SlideTarget = 0.f;
	float TileSize = 108.f;
	FString ActiveCat, Query, CachedLevel;
	bool bCachedFull = false;
	TArray<TSharedPtr<BF6Api::FPlaceableInfo>> Items;
	TSharedPtr<STileView<TSharedPtr<BF6Api::FPlaceableInfo>>> Tiles;
	TSharedPtr<SHorizontalBox> CatRow;
	TSharedPtr<SSearchBox> SearchBox;

	static bool GTransientMenuUp();   // defined after GTransientMenu below

	void SetPinned(bool bIn)
	{
		bPinned = bIn;
		GConfig->SetBool(TEXT("BF6UnrealSDK"), TEXT("LibraryPinned"), bPinned, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	void RefreshIfStale(bool bForce = false)
	{
		if (!bForce && CachedLevel == BF6Api::CurrentLevel() && bCachedFull == bFull) return;
		CachedLevel = BF6Api::CurrentLevel(); bCachedFull = bFull;
		ActiveCat.Reset();
		RebuildCats();
		RebuildItems();
	}

	TSharedRef<SWidget> MakeScopeButton(const FString& Label, bool bIsFull)
	{
		return SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(8, 2))
			.OnClicked_Lambda([this, bIsFull]{ bFull = bIsFull; RefreshIfStale(true); return FReply::Handled(); })
			[ SNew(STextBlock).Font(FontBold(10))
				.ColorAndOpacity_Lambda([this, bIsFull]{ return FSlateColor(bFull == bIsFull ? BF6Theme::Accent : BF6Theme::TextDim); })
				.Text(FText::FromString(Label)) ];
	}

	void RebuildCats()
	{
		if (!CatRow.IsValid()) return;
		CatRow->ClearChildren();
		auto AddTab = [this](const FString& Label, const FString& CatKey)
		{
			CatRow->AddSlot().AutoWidth().Padding(0, 0, 4, 0)
			[
				SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(9, 2))
				.OnClicked_Lambda([this, CatKey]{ ActiveCat = CatKey; RebuildItems(); return FReply::Handled(); })
				[ SNew(STextBlock).Font(FontBold(10))
					.ColorAndOpacity_Lambda([this, CatKey]{ return FSlateColor(ActiveCat == CatKey ? BF6Theme::Accent : BF6Theme::TextBlue); })
					.Text(FText::FromString(Label)) ]
			];
		};
		AddTab(TEXT("ALL"), FString());
		for (const FString& C : BF6Api::LibraryCategories(bFull))
			AddTab(C.ToUpper(), C);
		AddTab(TEXT("BLOCKS"), kBlocksTab);   // the user's saved prefabs
	}

	static bool IsBlockItem(const TSharedPtr<BF6Api::FPlaceableInfo>& I)
	{
		return I.IsValid() && I->Mesh.StartsWith(TEXT("block::"));
	}

	void RebuildItems()
	{
		Items.Reset();
		if (ActiveCat == kBlocksTab)
		{
			// blocks as cards: Mesh carries the thumb key, cost shows the count
			for (const BF6Api::FBlockInfo& B : BF6Api::ListBlocks())
			{
				if (!Query.IsEmpty() && !B.Name.Contains(Query)) continue;
				TSharedPtr<BF6Api::FPlaceableInfo> I = MakeShared<BF6Api::FPlaceableInfo>();
				I->Type = B.Name;
				I->Mesh = TEXT("block::") + B.Name;
				I->Directory = B.Level;
				I->Category = TEXT("Blocks");
				I->PhysicsCost = B.Count;
				Items.Add(I);
			}
		}
		else
			for (const BF6Api::FPlaceableInfo& P : BF6Api::LibraryPlaceables(ActiveCat, Query, bFull))
				Items.Add(MakeShared<BF6Api::FPlaceableInfo>(P));
		if (Tiles.IsValid()) Tiles->RequestListRefresh();
	}

	// double-click = quick place in front of the camera (re-placing before the
	// first was moved swaps it, so users can audition objects in one spot).
	// The first click of the pair opened the pop-out - close it again.
	void OnActivate(TSharedPtr<BF6Api::FPlaceableInfo> Item)
	{
		if (!Item.IsValid()) return;
		BF6Api::HideTransientMenus();
		if (IsBlockItem(Item))
		{
			FVector W;
			if (BF6Api::WorldFromViewportCenter(W)) BF6Api::PlaceBlock(Item->Type, W);
			return;
		}
		BF6Api::QuickPlace(Item->Type);
	}

	// single click = the pop-out: a live preview to spin around, with place/drag
	void OnCardClick(TSharedPtr<BF6Api::FPlaceableInfo> Item)
	{
		if (!Item.IsValid()) return;
		if (IsBlockItem(Item))
		{
			// blocks: big composite thumb + place/drag (no single-mesh spin view)
			const FString Name = Item->Type;
			BF6_PushTransient(
				SNew(SBorder).BorderImage(PanelBrush()).Padding(10.f)
				[
					SNew(SBox).WidthOverride(340.f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
						[ SNew(STextBlock).Font(FontBold(12)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(Name)) ]
						+ SVerticalBox::Slot().AutoHeight()
						[ SNew(SBox).HeightOverride(280.f)
							[ SNew(SBorder).BorderImage(InkBrush()).Padding(1.f)
								[ SNew(SImage).Image_Lambda([Name]{ return BF6Api::GetBlockThumb(Name); }) ] ] ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 8)
						[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
							.Text(FText::FromString(FString::Printf(TEXT("%s  -  %d objects"), *BF6Api::DisplayName(Item->Directory), Item->PhysicsCost))) ]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
							[ MakePrimaryButton(TEXT("Place in scene"), [Name]
								{ FVector W; if (BF6Api::WorldFromViewportCenter(W)) BF6Api::PlaceBlock(Name, W); BF6Api::HideTransientMenus(); }) ]
							+ SHorizontalBox::Slot().AutoWidth()
							[
								SNew(SBF6DragSource).PlaceableType(TEXT("block::") + Name)
								[
									SNew(SBorder).BorderImage(PanelLightBrush()).Padding(FMargin(12, 6))
									[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity(FSlateColor(BF6Theme::Text))
										.Text(FText::FromString(TEXT("DRAG INTO WORLD"))) ]
								]
							]
						]
					]
				],
				FSlateApplication::Get().GetCursorPos() - FVector2D(160.f, 460.f));
			return;
		}
		const FString Mesh = Item->Mesh.IsEmpty() ? Item->Type : Item->Mesh;
		TSharedRef<SBF6PreviewViewport> PV = SNew(SBF6PreviewViewport);
		PV->ShowModel(Mesh);
		// open clear of the cursor so a double-click's second click still lands
		// on the card underneath (double-click = quick place)
		const FVector2D At = FSlateApplication::Get().GetCursorPos() - FVector2D(190.f, 510.f);
		BF6_PushTransient(
			SNew(SBorder).BorderImage(PanelBrush()).Padding(10.f)
			[
				SNew(SBox).WidthOverride(400.f).HeightOverride(470.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontBold(12)).ColorAndOpacity(FSlateColor(BF6Theme::Accent))
							.OverflowPolicy(ETextOverflowPolicy::Ellipsis).Text(FText::FromString(Item->Type)) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontReg(10)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
							.Text(FText::FromString(FString::Printf(TEXT("%s   cost %d"), *Item->Category, Item->PhysicsCost))) ]
					]
					+ SVerticalBox::Slot().FillHeight(1)
					[ SNew(SBorder).BorderImage(InkBrush()).Padding(2.f)[ PV ] ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 6)
					[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
						.Text(FText::FromString(TEXT("drag in the preview to spin the model"))) ]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
						[ MakePrimaryButton(TEXT("Place in scene"), [Item]
							{ BF6Api::QuickPlace(Item->Type); BF6Api::HideTransientMenus(); }) ]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SBF6DragSource).PlaceableType(Item->Type)
							[
								SNew(SBorder).BorderImage(PanelLightBrush()).Padding(FMargin(12, 6))
								[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity(FSlateColor(BF6Theme::Text))
									.Text(FText::FromString(TEXT("DRAG INTO WORLD"))) ]
							]
						]
					]
				]
			],
			At);
	}

	void OpenMoveMenu(TSharedPtr<BF6Api::FPlaceableInfo> Item, const FVector2D& ScreenPos)
	{
		TSharedRef<SVerticalBox> Box = SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
				.Text(FText::FromString(FString::Printf(TEXT("MOVE  %s  TO..."), *Item->Type))) ];
		auto AddChoice = [this, Item, Box](const FString& Label, const FString& NewCat)
		{
			Box->AddSlot().AutoHeight()
			[
				SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(8, 3)).HAlign(HAlign_Left)
				.OnClicked_Lambda([this, Item, NewCat]
				{
					BF6Api::SetTypeCategory(Item->Type, NewCat);
					BF6Api::HideTransientMenus();
					RebuildCats(); RebuildItems();
					return FReply::Handled();
				})
				[ SNew(STextBlock).Font(FontReg(10)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(Label)) ]
			];
		};
		for (const FString& C : BF6Api::LibraryCategories(bFull))
			if (C != Item->Category) AddChoice(C, C);
		AddChoice(TEXT("(reset to default)"), FString());
		// free-typed new category
		Box->AddSlot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(SBox).WidthOverride(180.f)
			[
				SNew(SEditableTextBox).HintText(FText::FromString(TEXT("new category...")))
				.OnTextCommitted_Lambda([this, Item](const FText& T, ETextCommit::Type C)
				{
					if (C != ETextCommit::OnEnter || T.IsEmpty()) return;
					BF6Api::SetTypeCategory(Item->Type, T.ToString().TrimStartAndEnd());
					BF6Api::HideTransientMenus();
					RebuildCats(); RebuildItems();
				})
			]
		];
		BF6_PushTransient(
			SNew(SBorder).BorderImage(PanelBrush()).Padding(8.f)
			[ SNew(SBox).MaxDesiredHeight(320.f)[ SNew(SScrollBox) + SScrollBox::Slot()[ Box ] ] ],
			ScreenPos);
	}

	TSharedRef<ITableRow> OnGenerateTile(TSharedPtr<BF6Api::FPlaceableInfo> Item, const TSharedRef<STableViewBase>& Owner)
	{
		const FString Mesh = Item->Mesh.IsEmpty() ? Item->Type : Item->Mesh;
		return SNew(STableRow<TSharedPtr<BF6Api::FPlaceableInfo>>, Owner).Padding(FMargin(3, 3))
			.OnDragDetected_Lambda([Item](const FGeometry&, const FPointerEvent&)
			{
				if (!Item.IsValid()) return FReply::Unhandled();
				// blocks travel as "block::<name>" so the drop places the prefab
				return FReply::Handled().BeginDragDrop(FBF6LibDragOp::New(IsBlockItem(Item) ? Item->Mesh : Item->Type));
			})
			[
				// a map-selector-style card: iso render on top, caption bar below
				SNew(SBorder).BorderImage(InkBrush()).Padding(0.f)
				.ToolTip(BF6_MakeHint(Item->Type, IsBlockItem(Item)
					? FString::Printf(TEXT("Your saved prefab: %d objects, built on %s. Drag it into the world or double-click to place; re-saving a block under the same name updates every placed copy."), Item->PhysicsCost, *BF6Api::DisplayName(Item->Directory))
					: FString::Printf(TEXT("%s object, physics cost %d (counts toward the Portal budget bar). Drag it into the world, double-click to place ahead of the camera, or click once for a 3D preview."), *Item->Category, Item->PhysicsCost)))
				.OnMouseButtonDown_Lambda([this, Item](const FGeometry&, const FPointerEvent& E)
				{
					if (E.GetEffectingButton() == EKeys::RightMouseButton && Item.IsValid())
					{
						if (IsBlockItem(Item))
						{
							if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(FString::Printf(
								TEXT("Delete block '%s'? The file is removed permanently."), *Item->Type))) == EAppReturnType::Yes)
							{ BF6Api::DeleteBlock(Item->Type); RebuildItems(); }
						}
						else OpenMoveMenu(Item, E.GetScreenSpacePosition());
						return FReply::Handled();
					}
					return FReply::Unhandled();
				})
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().FillHeight(1)
					[
						SNew(SOverlay)
						+ SOverlay::Slot()
						[ SNew(SImage).Image_Lambda([Mesh]{ return BF6Api::GetModelThumb(Mesh); }) ]
						+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
							.Visibility_Lambda([Mesh]{ return BF6Api::GetModelThumb(Mesh) ? EVisibility::Collapsed : EVisibility::HitTestInvisible; })
							.Text(FText::FromString(TEXT("rendering..."))) ]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SBorder).BorderImage(PanelLightBrush()).Padding(FMargin(5, 2))
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
							[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::Text))
								.OverflowPolicy(ETextOverflowPolicy::Ellipsis).Text(FText::FromString(Item->Type)) ]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 0, 0)
							[ SNew(STextBlock).Font(FontReg(8)).ColorAndOpacity(FSlateColor(Item->PhysicsCost > 0 ? BF6Theme::AccentDim : BF6Theme::TextDim))
								.Text(FText::FromString(FString::Printf(TEXT("%d"), Item->PhysicsCost))) ]
						]
					]
				]
			];
	}
};

// The one live library panel (the build overlay owns the widget).
static TWeakPtr<SBF6LibraryPanel> GLibraryPanel;

static void BF6_MiniToast(const FString& Msg);   // defined with the input handler

// ---------------------------------------------------------------------------
// "SAVE BLOCK": name the current selection and store it as a reusable prefab.
// ---------------------------------------------------------------------------
class SBF6SaveBlockPopup : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6SaveBlockPopup) {}
	SLATE_END_ARGS()
	void Construct(const FArguments&)
	{
		ChildSlot
		[
			SNew(SBorder).BorderImage(PanelBrush()).Padding(12.f)
			[
				SNew(SBox).WidthOverride(280.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(TEXT("SAVE SELECTION AS BLOCK"))) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[
						SNew(SEditableTextBox).HintText(FText::FromString(TEXT("block name... (Enter saves)")))
						.OnTextCommitted_Lambda([](const FText& T, ETextCommit::Type C)
						{
							if (C != ETextCommit::OnEnter || T.IsEmpty()) return;
							const int32 N = BF6Api::SaveBlockFromSelection(T.ToString());
							if (N > 0) BF6_MiniToast(FString::Printf(TEXT("Block '%s' saved (%d objects, %s)."),
								*T.ToString(), N, *BF6Api::DisplayName(BF6Api::CurrentLevel())));
							BF6Api::HideTransientMenus();
						})
					]
					+ SVerticalBox::Slot().AutoHeight()
					[ SNew(STextBlock).Font(FontReg(9)).AutoWrapText(true).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
						.Text(FText::FromString(TEXT("Place it later from the radial's BLOCKS pill. It arrives grouped; UNGROUP splits it."))) ]
				]
			]
		];
	}
};

// ---------------------------------------------------------------------------
// "BLOCKS": browse saved prefabs - composite preview, source map, object count.
// Double-click places at the spot the radial was aimed at.
// ---------------------------------------------------------------------------
class SBF6BlocksPopup : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6BlocksPopup) {}
	SLATE_END_ARGS()
	void Construct(const FArguments&)
	{
		Refresh();
		ChildSlot
		[
			SNew(SBorder).BorderImage(PanelBrush()).Padding(10.f)
			[
				SNew(SBox).WidthOverride(480.f).HeightOverride(420.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[
							SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(6, 2))
							.OnClicked_Lambda([]{ BF6Pie_Reopen(); return FReply::Handled(); })
							[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("< BACK"))) ]
						]
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontBold(13)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(TEXT("BLOCKS"))) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("double-click = place at the aimed spot"))) ]
					]
					+ SVerticalBox::Slot().FillHeight(1)
					[
						SAssignNew(List, SListView<TSharedPtr<BF6Api::FBlockInfo>>)
						.ListItemsSource(&Blocks).SelectionMode(ESelectionMode::Single)
						.OnGenerateRow(this, &SBF6BlocksPopup::OnGenerateRow)
						.OnMouseButtonDoubleClick(this, &SBF6BlocksPopup::OnPlace)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
						[ MakeToolButton(TEXT("Open blocks folder"), []{ BF6Api::OpenBlocksFolder(); }) ]
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontReg(9)).AutoWrapText(true).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
							.Text(FText::FromString(TEXT("Share a block by sending its .json from this folder; drop received ones in and reopen."))) ]
					]
				]
			]
		];
	}

private:
	TArray<TSharedPtr<BF6Api::FBlockInfo>> Blocks;
	TSharedPtr<SListView<TSharedPtr<BF6Api::FBlockInfo>>> List;

	void Refresh()
	{
		Blocks.Reset();
		for (const BF6Api::FBlockInfo& B : BF6Api::ListBlocks())
			Blocks.Add(MakeShared<BF6Api::FBlockInfo>(B));
		if (List.IsValid()) List->RequestListRefresh();
	}

	void OnPlace(TSharedPtr<BF6Api::FBlockInfo> Item)
	{
		if (!Item.IsValid()) return;
		extern FVector GBF6PendingWorld;
		BF6Api::PlaceBlock(Item->Name, GBF6PendingWorld);
		BF6Api::HideTransientMenus();
	}

	TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<BF6Api::FBlockInfo> Item, const TSharedRef<STableViewBase>& Owner)
	{
		const bool bOtherMap = !Item->Level.IsEmpty() && Item->Level != BF6Api::CurrentLevel();
		return SNew(STableRow<TSharedPtr<BF6Api::FBlockInfo>>, Owner).Padding(FMargin(2, 3))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
				[
					SNew(SBox).WidthOverride(56.f).HeightOverride(56.f)
					[
						SNew(SBorder).BorderImage(InkBrush()).Padding(1.f)
						[ SNew(SImage).Image_Lambda([Name = Item->Name]{ return BF6Api::GetBlockThumb(Name); }) ]
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(Item->Name)) ]
					+ SVerticalBox::Slot().AutoHeight()
					[ SNew(STextBlock).Font(FontReg(9))
						.ColorAndOpacity(FSlateColor(bOtherMap ? BF6Theme::AccentDim : BF6Theme::TextDim))
						.Text(FText::FromString(FString::Printf(TEXT("%s  -  %d objects%s"),
							*BF6Api::DisplayName(Item->Level), Item->Count, bOtherMap ? TEXT("  (other map)") : TEXT("")))) ]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(6, 2))
					.OnClicked_Lambda([this, Item]
					{
						if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(FString::Printf(
							TEXT("Delete block '%s'? The file is removed permanently."), *Item->Name))) == EAppReturnType::Yes)
						{ BF6Api::DeleteBlock(Item->Name); Refresh(); }
						return FReply::Handled();
					})
					[ SNew(STextBlock).Font(FontReg(10)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("delete"))) ]
				]
			];
	}
};

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// "COLLISION": the overlay ported from the Godot high-poly tool. It draws each
// object's own shape in translucent red at the scale the GAME collides at,
// which is uniform from the X axis - so a stretched object shows a smaller red
// shell than the model you see, and that gap is exactly where players will
// walk through it. An approximation, never the real physics data, never saved.
// ---------------------------------------------------------------------------
class SBF6CollisionPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6CollisionPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		const int32 Stretched = BF6Api::CountStretched();
		ChildSlot
		[
			SNew(SBorder).BorderImage(InkBrush()).Padding(FMargin(18, 16))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
				[ SNew(STextBlock).Font(FontBold(13)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(TEXT("COLLISION"))) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
				[ SNew(SBox).WidthOverride(430.f)
					[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).AutoWrapText(true)
						.Text(FText::FromString(TEXT("The game scales collision evenly from the X axis, so a stretched object still bumps as though it were square. Red shows what you actually hit. It is a guide, not the game's real collision data."))) ] ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
				[ SNew(STextBlock).Font(FontReg(9))
					.ColorAndOpacity(FSlateColor(Stretched > 0 ? BF6Theme::Accent : BF6Theme::TextDim))
					.Text(FText::FromString(Stretched > 0
						? FString::Printf(TEXT("%d object%s on this map %s stretched, so collision differs there."), Stretched, Stretched == 1 ? TEXT("") : TEXT("s"), Stretched == 1 ? TEXT("is") : TEXT("are"))
						: FString(TEXT("Nothing on this map is stretched, so collision matches what you see.")))) ]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
					[
						SNew(SBox).ToolTip(BF6_MakeHint(TEXT("Stretched objects"), TEXT("Shows the overlay only where it differs from the model - every object you scaled unevenly. This is the one you want most of the time, and it stays cheap on a big map.")))
						[ MakePrimaryButton(TEXT("Show stretched"), []
							{
								const int32 n = BF6Api::ShowCollisionOverlay(1);
								BF6_MiniToast(n > 0
									? FString::Printf(TEXT("Collision shown on %d stretched object%s."), n, n == 1 ? TEXT("") : TEXT("s"))
									: FString(TEXT("Nothing is stretched - collision matches what you see.")));
							}) ]
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
					[
						SNew(SBox).ToolTip(BF6_MakeHint(TEXT("Every object"), TEXT("Shows the overlay on everything. On a full map that is a second copy of every model, so it costs memory and frame rate - use it for a look, not to work in.")))
						[ MakeToolButton(TEXT("Everything"), []
							{
								const int32 n = BF6Api::ShowCollisionOverlay(2);
								BF6_MiniToast(FString::Printf(TEXT("Collision shown on %d object%s."), n, n == 1 ? TEXT("") : TEXT("s")));
							}) ]
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SBox).ToolTip(BF6_MakeHint(TEXT("Turn it off"), TEXT("Removes every overlay and frees the memory they used.")))
						[ MakeToolButton(TEXT("Hide"), []
							{
								const int32 n = BF6Api::HideCollisionOverlay();
								BF6_MiniToast(n > 0
									? FString::Printf(TEXT("Collision hidden on %d object%s."), n, n == 1 ? TEXT("") : TEXT("s"))
									: FString(TEXT("Collision is not showing.")));
							}) ]
					]
				]
			]
		];
	}
};

// ---------------------------------------------------------------------------
// "COLORIZE": the recolorizer, ported from the Godot plugin of the same idea.
// A pure VIEW aid - it paints meshes in the editor so a blockout reads at a
// glance and duplicates stand out, and nothing it does reaches the export.
// ---------------------------------------------------------------------------
class SBF6ColorizePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6ColorizePanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		// the Godot plugin's quick palette, which reads well on grey blockouts
		const TArray<FLinearColor> Swatches = {
			FLinearColor(FColor(231, 76, 60)),  FLinearColor(FColor(230, 126, 34)),
			FLinearColor(FColor(241, 196, 15)), FLinearColor(FColor(46, 204, 113)),
			FLinearColor(FColor(26, 188, 156)), FLinearColor(FColor(52, 152, 219)),
			FLinearColor(FColor(142, 68, 173)), FLinearColor(FColor(236, 112, 99)),
			FLinearColor(FColor(165, 105, 189)), FLinearColor(FColor(127, 140, 141)) };

		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
		for (const FLinearColor& C : Swatches)
		{
			Row->AddSlot().AutoWidth().Padding(0, 0, 6, 0)
			[
				SNew(SBox).WidthOverride(30.f).HeightOverride(30.f)
				[
					SNew(SButton).ButtonStyle(&GhostButtonStyle()).ContentPadding(FMargin(2))
					.OnClicked_Lambda([C]
					{
						const int32 n = BF6Api::RecolorSelection(C);
						BF6_MiniToast(n > 0
							? FString::Printf(TEXT("Painted %d object%s."), n, n == 1 ? TEXT("") : TEXT("s"))
							: FString(TEXT("Select objects first, then pick a colour.")));
						return FReply::Handled();
					})
					[ SNew(SImage).Image(FCoreStyle::Get().GetBrush("GenericWhiteBox")).ColorAndOpacity(FSlateColor(C)) ]
				]
			];
		}

		ChildSlot
		[
			SNew(SBorder).BorderImage(InkBrush()).Padding(FMargin(18, 16))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
				[ SNew(STextBlock).Font(FontBold(13)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(TEXT("COLORIZE"))) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 14)
				[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).AutoWrapText(true)
					.Text(FText::FromString(TEXT("A view aid only - colours save with your map and never reach your export."))) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[ SNew(STextBlock).Font(FontBold(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("PAINT THE SELECTION"))) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 16)[ Row ]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
					[
						SNew(SBox).ToolTip(BF6_MakeHint(TEXT("Colour by type"), TEXT("Gives every distinct object type its own colour across the whole map, so repeated props, one-offs and mistakes stand out instantly. The same map always paints the same way.")))
						[ MakePrimaryButton(TEXT("Colour by type"), []
							{
								const int32 n = BF6Api::RecolorByType();
								BF6_MiniToast(n > 0
									? FString::Printf(TEXT("Painted %d objects by type."), n)
									: FString(TEXT("Nothing to paint yet.")));
							}) ]
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
					[
						SNew(SBox).ToolTip(BF6_MakeHint(TEXT("Clear the selection"), TEXT("Puts just the selected objects back to the material they really had.")))
						[ MakeToolButton(TEXT("Clear selection"), []
							{
								const int32 n = BF6Api::ClearRecolorSelection();
								BF6_MiniToast(n > 0
									? FString::Printf(TEXT("Restored %d object%s."), n, n == 1 ? TEXT("") : TEXT("s"))
									: FString(TEXT("Nothing painted in the selection.")));
							}) ]
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SBox).ToolTip(BF6_MakeHint(TEXT("Clear every colour"), TEXT("Puts the whole map back to its real materials. Colours save with your map, so this is how you remove them for good.")))
						[ MakeToolButton(TEXT("Clear all"), []
							{
								const int32 n = BF6Api::ClearRecolor();
								BF6_MiniToast(n > 0
									? FString::Printf(TEXT("Restored %d object%s."), n, n == 1 ? TEXT("") : TEXT("s"))
									: FString(TEXT("Nothing is painted.")));
							}) ]
					]
				]
			]
		];
	}
};

// "OBJECT IDS": the ObjId registry. Scripts address gameplay objects by these
// ids; duplicates or unset ids quietly break modes, so the registry lists
// every id, flags the problems, and auto-numbers a selection in a
// left-to-right spatial sweep.
// ---------------------------------------------------------------------------
static void BF6_MiniToast(const FString& Msg);

class SBF6ObjIdPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6ObjIdPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		ChildSlot
		[
			SNew(SBox).WidthOverride(480.f)
			[
				SNew(SBorder).BorderImage(PanelBrush()).Padding(10.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[
							SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(6, 2))
							.OnClicked_Lambda([]{ BF6Pie_Reopen(); return FReply::Handled(); })
							[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("< BACK"))) ]
						]
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontBold(13)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(TEXT("OBJECT IDS"))) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("click a row to select it"))) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[ SAssignNew(Summary, STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
						[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("start at"))) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[ SNew(SBox).WidthOverride(64.f)[ SAssignNew(StartBox, SEditableTextBox) ] ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[ MakePrimaryButton(TEXT("Assign to selection"), [this]
							{
								const int32 Start = FMath::Max(0, FCString::Atoi(*StartBox->GetText().ToString()));
								// fills blanks only - ids already set are what scripts address
								BF6Api::FObjIdAssign R = BF6Api::AutoAssignObjIds(Start, false);
								if (R.Considered == 0) { BF6_MiniToast(TEXT("Select gameplay objects first.")); Refresh(); return; }
								if (R.Assigned == 0 && R.Kept > 0)
								{
									// nothing blank left: renumbering is destructive, so ask plainly
									const EAppReturnType::Type Pick = FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(FString::Printf(TEXT(
										"All %d selected objects already have an ObjId.\n\n"
										"Scripts address objects by these ids, so renumbering can break a mod that is already written.\n\n"
										"Renumber them anyway, starting at %d?"), R.Kept, Start)));
									if (Pick != EAppReturnType::Yes) { BF6_MiniToast(TEXT("Left every id as it was.")); return; }
									R = BF6Api::AutoAssignObjIds(Start, true);
									BF6_MiniToast(FString::Printf(TEXT("Renumbered %d ids starting at %d."), R.Assigned, Start));
								}
								else
								{
									BF6_MiniToast(R.Kept > 0
										? FString::Printf(TEXT("Assigned %d id%s. Left %d that already had one."), R.Assigned, R.Assigned == 1 ? TEXT("") : TEXT("s"), R.Kept)
										: FString::Printf(TEXT("Assigned %d id%s."), R.Assigned, R.Assigned == 1 ? TEXT("") : TEXT("s")));
								}
								Refresh();
							}) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ MakeToolButton(TEXT("Select duplicates"), [this]
							{
								const int32 n = BF6Api::SelectDuplicateObjIds();
								BF6_MiniToast(n > 0
									? FString::Printf(TEXT("Selected %d actors with duplicate ids."), n)
									: FString(TEXT("No duplicate ids - clean.")));
							}) ]
						+ SHorizontalBox::Slot().FillWidth(1) [ SNullWidget::NullWidget ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ MakeToolButton(TEXT("Refresh"), [this]{ Refresh(); }) ]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[ SNew(SBox).MaxDesiredHeight(340.f)[ SNew(SScrollBox) + SScrollBox::Slot()[ SAssignNew(Rows, SVerticalBox) ] ] ]
				]
			]
		];
		Refresh();
	}

private:
	TSharedPtr<SVerticalBox> Rows;
	TSharedPtr<STextBlock> Summary;
	TSharedPtr<SEditableTextBox> StartBox;

	void Refresh()
	{
		if (!Rows.IsValid()) return;
		TArray<BF6Api::FObjIdRow> All = BF6Api::GatherObjIds();
		TMap<int32, int32> Count;
		for (const BF6Api::FObjIdRow& R : All) if (R.Id >= 0) Count.FindOrAdd(R.Id)++;
		int32 nDup = 0, nUnset = 0, MaxId = -1;
		for (const BF6Api::FObjIdRow& R : All)
		{
			if (R.Id < 0) { nUnset++; continue; }
			if (Count[R.Id] > 1) nDup++;
			MaxId = FMath::Max(MaxId, R.Id);
		}
		if (Summary.IsValid())
			Summary->SetText(FText::FromString(FString::Printf(
				TEXT("%d objects with ids     %d duplicate%s     %d unset"),
				All.Num(), nDup, nDup == 1 ? TEXT("") : TEXT("s"), nUnset)));
		// suggest the next free id, but never stomp what the user typed
		if (StartBox.IsValid() && StartBox->GetText().IsEmpty())
			StartBox->SetText(FText::FromString(FString::FromInt(MaxId + 1)));

		All.Sort([](const BF6Api::FObjIdRow& A, const BF6Api::FObjIdRow& B)
		{
			if ((A.Id < 0) != (B.Id < 0)) return B.Id < 0;   // unset sink to the bottom
			if (A.Id != B.Id) return A.Id < B.Id;
			return A.Name < B.Name;
		});

		Rows->ClearChildren();
		for (const BF6Api::FObjIdRow& R : All)
		{
			const bool bDup = R.Id >= 0 && Count[R.Id] > 1;
			const bool bUnset = R.Id < 0;
			TWeakObjectPtr<AActor> Wk = R.Actor;
			Rows->AddSlot().AutoHeight().Padding(0, 1)
			[
				SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(6, 3)).HAlign(HAlign_Fill)
				.OnClicked_Lambda([Wk]{ BF6Api::SelectOnly(Wk.Get()); return FReply::Handled(); })
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[ SNew(SBox).WidthOverride(64.f)
						[ SNew(STextBlock).Font(FontBold(10))
							.ColorAndOpacity(FSlateColor(bDup ? BF6Theme::Accent : (bUnset ? BF6Theme::TextDim : BF6Theme::Text)))
							.Text(FText::FromString(bUnset ? FString(TEXT("unset")) : FString::FromInt(R.Id))) ] ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0)
					[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(R.Type)) ]
					+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(8, 0, 0, 0)
					[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(R.Name)) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[ SNew(STextBlock).Font(FontBold(8)).ColorAndOpacity(FSlateColor(BF6Theme::Accent))
						.Visibility(bDup ? EVisibility::Visible : EVisibility::Collapsed)
						.Text(FText::FromString(TEXT("DUPLICATE"))) ]
				]
			];
		}
	}
};

// ---------------------------------------------------------------------------
// "CHECKS": the lint panel. Runs every offline validation rule and lists what
// it found - problems first. Rows select the offending actor; winding rows
// carry a one-click FIX.
// ---------------------------------------------------------------------------
class SBF6LintPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6LintPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		ChildSlot
		[
			SNew(SBox).WidthOverride(520.f)
			[
				SNew(SBorder).BorderImage(PanelBrush()).Padding(10.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[
							SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(6, 2))
							.OnClicked_Lambda([]{ BF6Pie_Reopen(); return FReply::Handled(); })
							[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("< BACK"))) ]
						]
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontBold(13)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(TEXT("VALIDATE"))) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("click a row to select it"))) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
							[
								// ids are a correctness question, so they live in Validate now
								SNew(SBox).ToolTip(BF6_MakeHint(TEXT("Object IDs"), TEXT("The registry of the numbers scripts use to address objects. Shows every id, flags duplicates, and fills in blanks without touching ids you already set.")))
								[ MakeToolButton(TEXT("Object IDs"), []{ BF6_PushTransient(SNew(SBF6ObjIdPanel), FSlateApplication::Get().GetCursorPos()); }) ]
							]
							+ SHorizontalBox::Slot().AutoWidth()
							[ MakeToolButton(TEXT("Re-check"), [this]{ Refresh(); }) ]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[ SAssignNew(Summary, STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)) ]
					+ SVerticalBox::Slot().AutoHeight()
					[ SNew(SBox).MaxDesiredHeight(380.f)[ SNew(SScrollBox) + SScrollBox::Slot()[ SAssignNew(Rows, SVerticalBox) ] ] ]
				]
			]
		];
		Refresh();
	}

private:
	TSharedPtr<SVerticalBox> Rows;
	TSharedPtr<STextBlock> Summary;

	void Refresh()
	{
		if (!Rows.IsValid()) return;
		TArray<BF6Api::FLintItem> Items = BF6Api::RunLint();
		int32 nProb = 0, nWarn = 0, nAdv = 0;
		for (const BF6Api::FLintItem& I : Items)
			(I.Severity == 0 ? nProb : I.Severity == 1 ? nWarn : nAdv)++;
		if (Summary.IsValid())
			Summary->SetText(FText::FromString(Items.Num() == 0
				? FString(TEXT("All clear - nothing to report."))
				: FString::Printf(TEXT("%d problem%s     %d warning%s     %d advisor%s"),
					nProb, nProb == 1 ? TEXT("") : TEXT("s"),
					nWarn, nWarn == 1 ? TEXT("") : TEXT("s"),
					nAdv, nAdv == 1 ? TEXT("y") : TEXT("ies"))));
		Rows->ClearChildren();
		for (const BF6Api::FLintItem& I : Items)
		{
			const TCHAR* Chip = I.Severity == 0 ? TEXT("PROBLEM") : I.Severity == 1 ? TEXT("WARNING") : TEXT("ADVICE");
			const FLinearColor ChipCol = I.Severity == 0 ? BF6Theme::Accent : I.Severity == 1 ? BF6Theme::Text : BF6Theme::TextDim;
			TWeakObjectPtr<AActor> Wk = I.Actor;
			TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0, 2, 8, 0)
				[ SNew(SBox).WidthOverride(64.f)
					[ SNew(STextBlock).Font(FontBold(8)).ColorAndOpacity(FSlateColor(ChipCol)).Text(FText::FromString(Chip)) ] ]
				+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
				[ SNew(STextBlock).Font(FontReg(9)).AutoWrapText(true).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(I.Message)) ];
			if (I.bWindingFix)
				Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
				[ MakeToolButton(TEXT("Fix"), [this, Wk]
					{
						if (BF6Api::ReverseVolumeWinding(Wk.Get())) BF6_MiniToast(TEXT("Point order reversed."));
						Refresh();
					}) ];
			Rows->AddSlot().AutoHeight().Padding(0, 2)
			[
				SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(6, 3)).HAlign(HAlign_Fill)
				.OnClicked_Lambda([Wk]{ BF6Api::SelectOnly(Wk.Get()); return FReply::Handled(); })
				[ Row ]
			];
		}
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
		SLATE_ARGUMENT(bool, ObjectRing)         // true = the object catalogue step
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		SetVisibility(EVisibility::HitTestInvisible);   // the input handler drives it
		Cats = InArgs._Items;
		Subs = InArgs._Subs;
		const bool bObjectRing = InArgs._ObjectRing;
		if (Cats.Num() == 0)
		{
			// The top ring stays SHORT. Twelve SDK categories plus every tool made
			// seventeen pills nobody could scan, so objects live one step in and
			// the top level is just the four things you actually choose between.
			if (bObjectRing)
			{
				// stepping in must always be reversible: BACK leads the ring so it
				// keeps one predictable wedge no matter how many categories load
				Cats = BF6Api::Categories();
				Subs.Reset();
				for (const FString& C : Cats) Subs.Add(FString::Printf(TEXT("%d"), BF6Api::CategoryCount(C)));
				Cats.Insert(TEXT("< BACK"), 0);
				Subs.Insert(TEXT("the main menu"), 0);
				Cats.Add(TEXT("FULL LIBRARY"));
				Subs.Add(TEXT("search everything"));
				Cats.Add(TEXT("BLOCKS"));
				Subs.Add(FString::Printf(TEXT("%d saved"), BF6Api::ListBlocks().Num()));
			}
			else
			{
				Cats.Reset(); Subs.Reset();
				Cats.Add(TEXT("OBJECTS"));
				Subs.Add(TEXT("place, library, blocks"));
				Cats.Add(TEXT("MODE SETUP"));
				Subs.Add(TEXT("conquest, breakthrough"));
				Cats.Add(TEXT("VALIDATE"));
				Subs.Add(TEXT("checks and object ids"));
				Cats.Add(TEXT("COLORIZE"));
				Subs.Add(BF6Api::AnyRecolored() ? TEXT("painted - clear inside") : TEXT("see your blockout"));
				Cats.Add(TEXT("COLLISION"));
				Subs.Add(BF6Api::AnyCollisionOverlay() ? TEXT("showing - hide inside") : TEXT("what you really hit"));
			}
		}
		Subs.SetNum(Cats.Num());
		const int32 N = FMath::Max(1, Cats.Num());
		// each pill sizes to ITS text so nothing ever clips
		const TSharedRef<FSlateFontMeasure> FM = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
		TArray<float> PillWs; PillWs.Reserve(Cats.Num());
		for (int32 i = 0; i < Cats.Num(); i++)
		{
			const float LabelW = (float)FM->Measure(Cats[i].ToUpper(), FontBold(9)).X;
			const float SubW = Subs.IsValidIndex(i) && !Subs[i].IsEmpty() ? (float)FM->Measure(Subs[i], FontReg(8)).X + 6.f : 0.f;
			PillWs.Add(FMath::Clamp(LabelW + SubW + 32.f, 96.f, 300.f));
		}
		const float PillH = 46.f;
		// Each pill anchors its INNER edge on the ring and grows away from the
		// centre (Blender-style), so side pills stack vertically and top/bottom
		// pills extend up/down - pill width no longer forces the ring outward.
		// The radius starts just outside the hub and grows only until an actual
		// AABB test says no two pills touch: as close as geometry allows.
		TArray<FVector2D> Dirs;   Dirs.Reserve(N);
		TArray<FVector2D> Aligns; Aligns.Reserve(N);
		for (int32 i = 0; i < Cats.Num(); i++)
		{
			const float Ang = (-90.f + (360.f / N) * i) * PI / 180.f;
			const FVector2D Dir(FMath::Cos(Ang), FMath::Sin(Ang));
			Dirs.Add(Dir);
			// right side (cos=1) -> left edge on the ring; left side -> right edge;
			// top (sin=-1) -> bottom edge; smooth blend in between
			Aligns.Add(FVector2D(0.5f * (1.f - Dir.X), 0.5f * (1.f - Dir.Y)));
		}
		float R = 78.f;                 // hub half (48) + a little air
		const float Gap = 10.f;         // minimum breathing room between pills
		for (; R < 600.f; R += 6.f)
		{
			bool bClash = false;
			for (int32 i = 0; i < Cats.Num() && !bClash; i++)
				for (int32 j = i + 1; j < Cats.Num() && !bClash; j++)
				{
					const float Li = Dirs[i].X * R - Aligns[i].X * PillWs[i];
					const float Ti = Dirs[i].Y * R - Aligns[i].Y * PillH;
					const float Lj = Dirs[j].X * R - Aligns[j].X * PillWs[j];
					const float Tj = Dirs[j].Y * R - Aligns[j].Y * PillH;
					bClash = Li < Lj + PillWs[j] + Gap && Lj < Li + PillWs[i] + Gap
					      && Ti < Tj + PillH     + Gap && Tj < Ti + PillH     + Gap;
				}
			if (!bClash) break;
		}

		TSharedRef<SConstraintCanvas> Canvas = SNew(SConstraintCanvas);

		// dim the viewport behind the pie
		Canvas->AddSlot().Anchors(FAnchors(0.f, 0.f, 1.f, 1.f)).Offset(FMargin(0))
			[ SNew(SBorder).BorderImage(DimBrush()) ];

		// centre hub - the text NEVER mirrors the hovered pill (labels don't fit
		// in a 96px circle); it stays CANCEL, only lighting up when the hovered
		// pill is itself a CANCEL/BACK action.
		Canvas->AddSlot().Anchors(FAnchors(0.5f, 0.5f)).Alignment(FVector2D(0.5f, 0.5f)).AutoSize(true)
			[
				SNew(SBox).WidthOverride(96.f).HeightOverride(96.f)
				[
					SNew(SBorder).BorderImage_Lambda([this]{ return HubIsAction() ? PieHubHot() : PieHub(); })
					.HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(10.f)
					[ SNew(STextBlock).Font(FontBold(11)).Justification(ETextJustify::Center)
						.ColorAndOpacity_Lambda([this]{ return FSlateColor(HubIsAction() ? BF6Theme::Ink : BF6Theme::TextDim); })
						.Text_Lambda([this]{ return FText::FromString(HubIsAction() ? Cats[Highlighted].ToUpper() : FString(TEXT("CANCEL"))); }) ]
				]
			];

		for (int32 i = 0; i < Cats.Num(); i++)
		{
			const float X = R * Dirs[i].X;
			const float Y = R * Dirs[i].Y;
			const FString Cat = Cats[i];
			const FString Sub = Subs[i];
			const int32 Idx = i;

			Canvas->AddSlot()
				.Anchors(FAnchors(0.5f, 0.5f)).Offset(FMargin(X, Y, 0.f, 0.f)).Alignment(Aligns[i]).AutoSize(true)
				[
					SNew(SBox).WidthOverride(PillWs[i]).HeightOverride(PillH)
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

	// only these labels are short enough (and semantically the hub's own job)
	// to echo in the centre circle
	bool HubIsAction() const
	{
		if (!Cats.IsValidIndex(Highlighted)) return false;
		const FString U = Cats[Highlighted].ToUpper();
		return U == TEXT("BACK") || U == TEXT("CANCEL");
	}
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
		// The library panel sits IN FLOW below the HUD canvas: when it slides up
		// its desired height grows, the canvas shrinks, and every bottom-anchored
		// panel rides up with it through plain layout (slot-offset attribute
		// bindings proved unreliable for per-frame animation).
		ChildSlot
		[
			SNew(SVerticalBox).Visibility(EVisibility::SelfHitTestInvisible)

			+ SVerticalBox::Slot().FillHeight(1.f)
			[
			SAssignNew(RootCanvas, SConstraintCanvas).Visibility(EVisibility::SelfHitTestInvisible)

			// --- drop catcher: hit-tests ONLY while a library tile drag is live,
			// so the drop lands on the map through all the other overlay layers ---
			+ SConstraintCanvas::Slot()
				.Anchors(FAnchors(0.f, 0.f, 1.f, 1.f)).Offset(FMargin(0.f))
				[ SNew(SBF6DropCatcher) ]

			// --- the zone's orange point dots (Godot-style handles) ---
			+ SConstraintCanvas::Slot()
				.Anchors(FAnchors(0.f, 0.f, 1.f, 1.f)).Offset(FMargin(0.f))
				[ SNew(SBF6ZoneDots) ]

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

			// --- the context CONTROLS hints (top-left, slides in from the left) ---
			+ SConstraintCanvas::Slot()
				.Anchors(FAnchors(0.f, 0.f)).Offset(FMargin(0.f, 48.f, 0.f, 0.f)).Alignment(FVector2D(0.f, 0.f)).AutoSize(true)
				[ SNew(SBF6HintPanel) ]

			// --- SCATTER live editor (right side, only while a scatter is live) ---
			+ SConstraintCanvas::Slot()
				.Anchors(FAnchors(1.f, 0.5f)).Offset(FMargin(0.f, 0.f, 14.f, 0.f)).Alignment(FVector2D(1.f, 0.5f)).AutoSize(true)
				[ SNew(SBF6ScatterPanel) ]

			// --- fixed-camera live preview (bottom-centre while one is selected) ---
			+ SConstraintCanvas::Slot()
				.Anchors(FAnchors(0.5f, 1.f)).Offset(FMargin(0.f, -64.f, 0.f, 0.f)).Alignment(FVector2D(0.5f, 1.f)).AutoSize(true)
				[ SNew(SBF6CameraPanel) ]

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
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
						[
							SNew(SBox).Visibility_Lambda([]{ return BF6Api::IsEditing() ? EVisibility::Visible : EVisibility::Collapsed; })
							// One button, two directions: with an imported Godot tree present it
							// flips between that tree and automatic folders, and the choice sticks.
							.ToolTip_Lambda([]
							{
								return BF6Api::AnyGodotTree() && BF6Api::KeepingGodotTree()
									? BF6_MakeHint(TEXT("Sort into folders"), TEXT("Files every object by what it is - HQs, spawns, zones, props by category, each block in its own folder. Your Godot tree is remembered, so you can put it back at any time."))
									: BF6Api::AnyGodotTree()
									? BF6_MakeHint(TEXT("Back to your Godot tree"), TEXT("Rebuilds the level list exactly as you had it in Godot, folder for folder."))
									: BF6_MakeHint(TEXT("Tidy up the list"), TEXT("Sorts every object in the level list into folders by what it is - HQs, spawns, zones, props by category, and each block in its own folder. New objects sort themselves; use this on a level built earlier."));
							})
							[ MakeToolButton_Dynamic(
								TAttribute<FText>::CreateLambda([]
								{
									return FText::FromString(!BF6Api::AnyGodotTree() ? TEXT("Tidy up")
										: BF6Api::KeepingGodotTree() ? TEXT("Sort into folders") : TEXT("Godot tree"));
								}),
								[]
								{
									if (BF6Api::AnyGodotTree())
									{
										const bool bKeep = !BF6Api::KeepingGodotTree();
										const int32 n = BF6Api::SetOutlinerMode(bKeep);
										BF6_MiniToast(bKeep
											? FString::Printf(TEXT("Your Godot tree is back - %d objects."), n)
											: FString::Printf(TEXT("Sorted %d objects into folders."), n));
									}
									else
									{
										const int32 n = BF6Api::OrganizeOutliner();
										BF6_MiniToast(FString::Printf(TEXT("Sorted %d objects into folders."), n));
									}
								}) ]
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
						[ MakeToolButton(TEXT("Export"), []{ BF6Api::ExportSpatial(); }) ]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SBox)
							.ToolTip(BF6_MakeHint(TEXT("Open exports"), TEXT("Opens the folder every exported .spatial.json lands in - the files you upload on the Portal site's Map Rotation page. Session saves live elsewhere and are not uploadable.")))
							[ MakeToolButton(TEXT("Open exports"), []{ BF6Api::OpenExportsFolder(); }) ]
						]
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

			]   // (end of the HUD canvas slot)

			// --- the Object Library: in flow at the very bottom ---
			+ SVerticalBox::Slot().AutoHeight()
			[ SAssignNew(Library, SBF6LibraryPanel) ]

		];
		GLibraryPanel = Library;
	}

	virtual void Tick(const FGeometry& G, const double T, const float D) override
	{
		SCompoundWidget::Tick(G, T, D);
		BF6Api::TickZoneAutoEdit();   // selecting a zone starts point editing, like Godot
		BF6Api::TickVolumeEdit();     // live zone-wall rebuild while handles are dragged
		BF6Api::TickObbEdit();        // live box resize while face handles are dragged
		BF6Api::TickGroupEdit();      // group edit mode: members-only selection
		BF6Api::TickLinkPick();       // assign mode: project the candidate markers
		BF6Api::TickScatter();        // scatter outline: project the corner dots
		BF6Api::TickCameraPreview();  // camera selected: live picture-in-picture
		BF6Api::TickCollisionOverlay();   // overlays follow objects as they move
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
	TSharedPtr<SBF6LibraryPanel> Library;
	TSharedPtr<SConstraintCanvas> RootCanvas;
	FSimpleDelegate OnChooseMap;
};

// ---------------------------------------------------------------------------
// Map selector - the tool's first screen. Embedded in the dockable panel, so it
// fills the editor and resizes with it. Reports the chosen map via OnOpen.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// VERSION HISTORY: the tool's changelog and every Portal SDK release in one
// scrollable viewer. SDK entries come from the baked archive history in the
// plugin plus locally generated diffs for updates installed on this machine.
// ---------------------------------------------------------------------------
class SBF6HistoryPopup : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6HistoryPopup) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);
		TArray<FString> Lines;
		BF6Api::VersionHistoryText().ParseIntoArrayLines(Lines, false);
		for (const FString& L : Lines)
		{
			if (L.StartsWith(TEXT("# ")))
				Body->AddSlot().AutoHeight().Padding(0, 14, 0, 4)
				[ SNew(STextBlock).Font(FontBold(14)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(L.Mid(2).ToUpper())) ];
			else if (L.StartsWith(TEXT("## ")))
				Body->AddSlot().AutoHeight().Padding(0, 10, 0, 2)
				[ SNew(STextBlock).Font(FontBold(12)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(L.Mid(3))) ];
			else if (L.StartsWith(TEXT("- ")))
				Body->AddSlot().AutoHeight().Padding(10, 1, 0, 1)
				[ SNew(STextBlock).AutoWrapText(true).Font(FontReg(10)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(L)) ];
			else if (!L.TrimStartAndEnd().IsEmpty())
				Body->AddSlot().AutoHeight().Padding(0, 1)
				[ SNew(STextBlock).AutoWrapText(true).Font(FontReg(10)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(L)) ];
		}

		ChildSlot
		[
			SNew(SBox).WidthOverride(760.f).HeightOverride(560.f)
			[
				SNew(SBorder).BorderImage(PanelBrush()).Padding(14.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
						[ SNew(STextBlock).Font(FontBold(14)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(TEXT("VERSION HISTORY"))) ]
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontReg(10)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("the tool, and every Portal SDK release - click away to close"))) ]
					]
					+ SVerticalBox::Slot().FillHeight(1)
					[ SNew(SScrollBox) + SScrollBox::Slot()[ Body ] ]
				]
			]
		];
	}
};

class SBF6MapSelector : public SCompoundWidget
{
public:
	DECLARE_DELEGATE_TwoParams(FOnOpen, FString /*Level*/, FString /*Save*/);

	SLATE_BEGIN_ARGS(SBF6MapSelector) {}
		SLATE_EVENT(FOnOpen, OnOpen)
		SLATE_EVENT(FSimpleDelegate, OnImport)
		SLATE_EVENT(FSimpleDelegate, OnSdkSetup)
		SLATE_EVENT(FSimpleDelegate, OnReturn)   // back to the active build session
		SLATE_EVENT(FSimpleDelegate, OnChanged)  // a save was deleted: rebuild me
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		OnOpen = InArgs._OnOpen;
		OnImport = InArgs._OnImport;
		OnSdkSetup = InArgs._OnSdkSetup;
		OnReturn = InArgs._OnReturn;
		OnChanged = InArgs._OnChanged;

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
					// Import lives on this screen: ONE button, both formats
					// (.spatial.json or a Godot .tscn). It detects the map from
					// the file and opens straight into build mode.
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(0,0,8,0)
					[ MakeToolButton(TEXT("SDK Setup"), [this]{ OnSdkSetup.ExecuteIfBound(); }) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(0,0,8,0)
					[ MakeToolButton(TEXT("Import map..."), [this]{ OnImport.ExecuteIfBound(); }) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(0,0,8,0)
					[
						SNew(SBox).ToolTip(BF6_MakeHint(TEXT("Open saves"), TEXT("Opens the session-save folder: one folder per custom map with its level file inside. That folder is what you back up or share. Uploadable exports live in the EXPORT folder instead.")))
						[ MakeToolButton(TEXT("Open saves"), []{ BF6Api::OpenSavesFolder(); }) ]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(0,0,8,0)
					[
						SNew(SBox).ToolTip(BF6_MakeHint(TEXT("Version history"), TEXT("What changed in every version of this tool, and in every Portal SDK release EA has shipped - including what each SDK update added or removed on this machine.")))
						[ MakeToolButton(TEXT("History"), []{ BF6_PushTransient(SNew(SBF6HistoryPopup), FSlateApplication::Get().GetCursorPos()); }) ]
					]
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
	FSimpleDelegate OnChanged;

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
								.OnGenerateWidget_Lambda([this, Level](TSharedPtr<FString> In)
								{
									// name opens; the x deletes (with a confirm)
									return SNew(SHorizontalBox)
									+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(0, 0, 10, 0)
									[ SNew(STextBlock).Text(FText::FromString(In.IsValid() ? *In : FString())) ]
									+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
									[
										SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(4, 0))
										.ToolTip(BF6_MakeHint(TEXT("Delete this save"), TEXT("Removes the saved project's file for this map (asks first). Exports already made from it are untouched.")))
										.OnClicked_Lambda([this, Level, In]
										{
											if (!In.IsValid()) return FReply::Handled();
											if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(FString::Printf(
												TEXT("Delete the save '%s' for %s?\n\nThis removes its file for good. Exports already made from it stay."),
												**In, *BF6Api::DisplayName(Level)))) == EAppReturnType::Yes)
											{
												if (BF6Api::DeleteSave(Level, *In))
												{
													BF6_MiniToast(FString::Printf(TEXT("Deleted '%s'."), **In));
													OnChanged.ExecuteIfBound();
												}
											}
											return FReply::Handled();
										})
										[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.35f, 0.30f))).Text(FText::FromString(TEXT("x"))) ]
									];
								})
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
				[ MakeToolButton(TEXT("Import map..."), []{ BF6Api::ImportSpatial(); }) ]
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
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
					[ SNew(STextBlock).AutoWrapText(true).Font(FontReg(12)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("This tool downloads the official Battlefield 6 Portal SDK into this Unreal project and builds everything from it - maps, models, and gameplay data. The import that follows takes a while (about 9,700 models); after SDK updates only changed content reconverts."))) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SBox).Visibility_Lambda([]{ return (BF6Api::IsImporting() || BF6Api::IsSdkFetching()) ? EVisibility::Collapsed : EVisibility::Visible; })
							[ MakePrimaryButton(TEXT("Download the SDK for me"), []{ BF6Api::StartSdkDownload(); }) ]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[ SNew(STextBlock).AutoWrapText(true).Font(FontReg(10)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("About 3 GB, straight from EA's official Portal download service (with the community archive as backup). The download resumes if interrupted, and when EA releases a new SDK the tool offers the update - your maps and blocks always carry over."))) ]
					// nobody installs blind: the exact folder everything lands in
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
						[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("Install location:"))) ]
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[ SNew(STextBlock).Font(FontReg(10)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(BF6Api::ManagedSdkDir())) ]
						+ SHorizontalBox::Slot().AutoWidth()
						[ MakeToolButton(TEXT("Open"), []{ BF6Api::OpenManagedSdkDir(); }) ]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SVerticalBox)
						.Visibility_Lambda([]{ return (BF6Api::IsSdkFetching() || BF6Api::SdkFetchFailed()) ? EVisibility::Visible : EVisibility::Collapsed; })
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
						[ SNew(SProgressBar).Percent_Lambda([]{ return BF6Api::SdkFetchFrac(); })
							.FillColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([]{ return FSlateColor(BF6Api::SdkFetchFailed() ? FLinearColor(0.80f, 0.25f, 0.20f) : BF6Theme::Accent); })) ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
						[ SNew(STextBlock).Font(FontReg(11)).AutoWrapText(true)
							.ColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([]{ return FSlateColor(BF6Api::SdkFetchFailed() ? FLinearColor(0.90f, 0.45f, 0.40f) : BF6Theme::TextDim); }))
							.Text_Lambda([]{ return BF6Api::SdkFetchStatus(); }) ]
					]
					// download failed: the manual road, with a real check before proceeding
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SVerticalBox)
						.Visibility_Lambda([]{ return BF6Api::SdkFetchFailed() ? EVisibility::Visible : EVisibility::Collapsed; })
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
						[ SNew(STextBlock).AutoWrapText(true).Font(FontReg(10)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(FString::Printf(
							TEXT("Download not working? Get the SDK yourself: download PortalSDK.zip from portal.battlefield.com, unzip it, and put the unzipped folder here:\n%s\nThen click the button - the tool checks the folder is a complete SDK before it proceeds."), *BF6Api::ManagedSdkDir()))) ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth()
							[ MakeToolButton(TEXT("I placed it - check the folder"), []
							{
								FString Msg;
								BF6Api::CheckManualSdkDrop(Msg);
								BF6_MiniToast(Msg);
							}) ]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 16)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SBox).Visibility_Lambda([]{ return BF6Api::ImportDone() ? EVisibility::Visible : EVisibility::Collapsed; })
							[ MakePrimaryButton(TEXT("Continue"), [this]{ OnDone.ExecuteIfBound(); }) ]
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
						[
							// wipe + reconvert: needed when an SDK update CHANGES existing
							// content (the normal import only adds what's new)
							SNew(SBox).Visibility_Lambda([]{ return (!BF6Api::IsImporting() && BF6Api::IsDataInstalled() && !BF6Api::StoredSdkRoot().IsEmpty()) ? EVisibility::Visible : EVisibility::Collapsed; })
							[ MakeToolButton(TEXT("Full re-sync"), []
							{
								if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(
									TEXT("Delete all converted models and map meshes, then reconvert everything from the SDK?\n\nThis takes as long as the first import. Use it after a big SDK update, when existing content may have changed."))) == EAppReturnType::Yes)
									BF6Api::StartSdkImport(BF6Api::StoredSdkRoot(), true);
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
						.Visibility_Lambda([]{ return (BF6Api::IsImporting() || BF6Api::ImportDone() || BF6Api::ImportFailed()) ? EVisibility::Visible : EVisibility::Collapsed; })
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
						[ SNew(SProgressBar).Percent_Lambda([]{ return BF6Api::ImportFrac(); })
							.FillColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([]{ return FSlateColor(BF6Api::ImportFailed() ? FLinearColor(0.80f, 0.25f, 0.20f) : BF6Theme::Accent); })) ]
						+ SVerticalBox::Slot().AutoHeight()
						[ SNew(STextBlock).Font(FontReg(11)).AutoWrapText(true)
							.ColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([]{ return FSlateColor(BF6Api::ImportFailed() ? FLinearColor(0.90f, 0.45f, 0.40f) : BF6Theme::TextDim); }))
							.Text_Lambda([]{ return BF6Api::ImportStatus(); }) ]
					]
				]
			]
		];
	}

private:
	FSimpleDelegate OnDone;
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
			.OnChanged_Lambda([this]{ RebuildSelector(); })
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
	enum class EBF6PieMode { Place, Objects, Props, VolEdit };
	EBF6PieMode                   GPieMode = EBF6PieMode::Place;
	TWeakObjectPtr<AActor>        GPieTarget;
	FString                       GPieTargetType;
	TArray<BF6Api::FPropDef>      GPieProps;
	int32                         GPiePage = 0;
	static const int32            kPiePropsPerPage = 10;
}

// the library's auto-hide must not fire while its context menu is up
bool SBF6LibraryPanel::GTransientMenuUp() { return GTransientMenu.IsValid(); }

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
		Items = { TEXT("ADD POINT"), TEXT("DELETE POINT"), TEXT("RESET CENTER"), TEXT("FINISH EDITING") };
		Subs  = { TEXT("after selected"), TEXT("selected"), TEXT("origin to the middle"), TEXT("bake the zone") };
		break;
	case EBF6PieMode::Props:
	{
		// a FEW pills; the detail lives in pop-out menus (way too many pills to
		// remember otherwise - attribute lists open as a menu instead)
		GPieProps = BF6Api::PropsForType(GPieTargetType);
		if (BF6Api::IsVolumeActor(GPieTarget.Get())) { Items.Add(TEXT("EDIT POINTS")); Subs.Add(TEXT("zone shape")); }
		if (BF6Api::CameraPreviewTarget() == GPieTarget.Get() && GPieTarget.IsValid())
		{ Items.Add(TEXT("SET CAMERA")); Subs.Add(TEXT("take the editor view")); }
		if (GPieProps.Num() > 0)
		{ Items.Add(TEXT("ATTRIBUTES")); Subs.Add(FString::Printf(TEXT("%d fields"), GPieProps.Num())); }
		// collision is a per-object question, and the top ring is unreachable
		// while anything is selected - so the selection scope lives here
		if (BF6Api::AnyCollisionOverlay())
		{ Items.Add(TEXT("HIDE COLLISION")); Subs.Add(TEXT("clear the red")); }
		else
		{ Items.Add(TEXT("COLLISION")); Subs.Add(TEXT("what you really hit")); }
		Items.Add(TEXT("PICK PLACE"));     Subs.Add(TEXT("carry with the cursor"));
		Items.Add(TEXT("SELECT SIMILAR")); Subs.Add(TEXT("every copy"));
		Items.Add(TEXT("MULTIPLY"));       Subs.Add(TEXT("rows, grids, circles"));
		Items.Add(TEXT("SCATTER"));        Subs.Add(TEXT("live editor"));
		Items.Add(TEXT("GROUPING"));       Subs.Add(TEXT("group, block, edit"));
		break;
	}
	default:
		// Place: empty items = the object categories. If the catalogue never
		// loaded (bf6_core failed, or the data import never ran), say so
		// instead of presenting an empty ring with nothing but Cancel.
		if (GPieMode == EBF6PieMode::Objects && BF6Api::Categories().Num() == 0)
		{
			FNotificationInfo Info(FText::FromString(TEXT(
				"No object catalogue is loaded. Check Saved/Logs/BF6_High_Poly.log for bf6_core errors, or run SDK Setup from the map screen.")));
			Info.ExpireDuration = 8.0f;
			FSlateNotificationManager::Get().AddNotification(Info);
			return;
		}
		break;
	}

	GPie = SNew(SBF6PieMenu).Items(Items).Subs(Subs).ObjectRing(GPieMode == EBF6PieMode::Objects);
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
	if (N == 0 || v.Size() < 52.f) { GPie->SetHighlighted(-1); return; }   // inside the hub circle = cancel
	const float Ang = FMath::RadiansToDegrees(FMath::Atan2(v.Y, v.X));
	const float Step = 360.f / N;
	int32 Idx = FMath::RoundToInt((Ang - (-90.f)) / Step);
	Idx = ((Idx % N) + N) % N;
	GPie->SetHighlighted(Idx);
}

// Confirm: dispatch the highlighted wedge by the mode the pie opened in.
// Deadzone (no highlight) just closes.
// reopen the pie exactly as it was (mode/target/centre persist in the globals)
static void BF6Pie_Reopen()
{
	BF6Api::HideTransientMenus();
	BF6Pie_Attach();
}

// ---------------------------------------------------------------------------
// ATTRIBUTES pop-out: every editable field of the selected object as a menu
// (replaces the old one-pill-per-attribute wheel). Click a row to edit it;
// link-type rows start the pick flow. BACK returns to the wheel.
// ---------------------------------------------------------------------------
class SBF6AttributesMenu : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6AttributesMenu) : _TargetActor(nullptr) {}
		SLATE_ARGUMENT(AActor*, TargetActor)
		SLATE_ARGUMENT(FString, TypeName)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Target = InArgs._TargetActor;
		TypeName = InArgs._TypeName;
		const TArray<BF6Api::FPropDef> Defs = BF6Api::PropsForType(TypeName);

		// size the name column to the LONGEST attribute name (and note the widest
		// type tag) so no label ever clips - the panel widens to fit instead
		const TSharedRef<FSlateFontMeasure> FM = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
		float NameW = 96.f, TypeTagW = 0.f;
		for (const BF6Api::FPropDef& D : Defs)
		{
			NameW    = FMath::Max(NameW,    (float)FM->Measure(D.Name, FontBold(10)).X);
			TypeTagW = FMath::Max(TypeTagW, (float)FM->Measure(D.Type, FontReg(8)).X);
		}
		NameW = FMath::Min(NameW + 8.f, 260.f);
		// name column + editor (min ~190) + type tag + row/panel paddings
		const float PanelW = FMath::Clamp(NameW + TypeTagW + 230.f, 420.f, 680.f);

		TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
		for (const BF6Api::FPropDef& D : Defs)
		{
			const BF6Api::FPropDef Def = D;
			const bool bLink = Def.Type.Contains(TEXT("Volume")) || Def.Type.Contains(TEXT("Array[")) || Def.Type.Contains(TEXT("Path")) || Def.Type.Contains(TEXT("SpawnPoint"));
			AActor* Now = Target.Get();
			const FString Current = Now ? BF6Api::GetActorProp(Now, Def.Name, Def.Default) : Def.Default;

			// every field edits IN PLACE - no pop-out, the menu stays open
			TSharedRef<SWidget> Editor = SNullWidget::NullWidget;
			if (bLink)
			{
				Editor = MakeToolButton(TEXT("Pick in world..."), [this, Def]
				{
					AActor* A = Target.Get(); if (!A) return;
					BF6Api::HideTransientMenus();
					BF6Api::BeginLinkPick(A, Def.Name, Def.Type.Contains(TEXT("Array[")));
				});
			}
			else if (Def.Type == TEXT("bool"))
			{
				auto BoolBtn = [this, Def](const TCHAR* Label, const TCHAR* Value) -> TSharedRef<SWidget>
				{
					const FString Val(Value);
					return SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(8, 2))
						.OnClicked_Lambda([this, Def, Val]{ if (AActor* A = Target.Get()) BF6Api::SetActorProp(A, Def.Name, Val); return FReply::Handled(); })
						[ SNew(STextBlock).Font(FontBold(10))
							.ColorAndOpacity_Lambda([this, Def, Val]
							{
								AActor* A = Target.Get();
								const bool bOn = A && BF6Api::GetActorProp(A, Def.Name, Def.Default).Equals(Val, ESearchCase::IgnoreCase);
								return FSlateColor(bOn ? BF6Theme::Accent : BF6Theme::TextDim);
							})
							.Text(FText::FromString(Label)) ];
				};
				Editor = SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()[ BoolBtn(TEXT("True"), TEXT("true")) ]
					+ SHorizontalBox::Slot().AutoWidth()[ BoolBtn(TEXT("False"), TEXT("false")) ];
			}
			else if (Def.Type == TEXT("selection") && Def.Options.Num() > 0)
			{
				TSharedPtr<TArray<TSharedPtr<FString>>> Src = MakeShared<TArray<TSharedPtr<FString>>>();
				for (const FString& O : Def.Options) Src->Add(MakeShared<FString>(O));
				OptSources.Add(Src);
				Editor = SNew(SComboBox<TSharedPtr<FString>>)
					.OptionsSource(Src.Get())
					.OnGenerateWidget_Lambda([](TSharedPtr<FString> In){ return SNew(STextBlock).Text(FText::FromString(In.IsValid() ? *In : FString())); })
					.OnSelectionChanged_Lambda([this, Def](TSharedPtr<FString> In, ESelectInfo::Type)
					{ if (In.IsValid()) if (AActor* A = Target.Get()) BF6Api::SetActorProp(A, Def.Name, *In); })
					[ SNew(STextBlock).Font(FontReg(10)).ColorAndOpacity(FSlateColor(BF6Theme::Text))
						.Text_Lambda([this, Def]{ AActor* A = Target.Get(); const FString V = A ? BF6Api::GetActorProp(A, Def.Name, Def.Default) : FString(); return FText::FromString(V.IsEmpty() ? TEXT("choose...") : *V); }) ];
			}
			else
			{
				Editor = SNew(SEditableTextBox).Text(FText::FromString(Current))
					.OnTextCommitted_Lambda([this, Def](const FText& T, ETextCommit::Type C)
					{
						if (C != ETextCommit::OnEnter && C != ETextCommit::OnUserMovedFocus) return;
						if (AActor* A = Target.Get()) BF6Api::SetActorProp(A, Def.Name, T.ToString());
					});
			}

			List->AddSlot().AutoHeight().Padding(0, 2)
			[
				SNew(SHorizontalBox)
				.ToolTip(BF6_MakeHint(Def.Name, BF6_AttributeHint(Def)))
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ SNew(SBox).WidthOverride(NameW)
					[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(Def.Name)) ] ]
				+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(6, 0)
				[ Editor ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ SNew(STextBlock).Font(FontReg(8)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(Def.Type)) ]
			];
		}

		ChildSlot
		[
			SNew(SBox).WidthOverride(PanelW)
			[
				SNew(SBorder).BorderImage(PanelBrush()).Padding(10.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[
							SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(6, 2))
							.OnClicked_Lambda([]{ BF6Pie_Reopen(); return FReply::Handled(); })
							[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("< BACK"))) ]
						]
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontBold(13)).ColorAndOpacity(FSlateColor(BF6Theme::Accent))
							.Text(FText::FromString(FString::Printf(TEXT("%s - ATTRIBUTES"), *TypeName.ToUpper()))) ]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[ SNew(SBox).MaxDesiredHeight(380.f)[ SNew(SScrollBox) + SScrollBox::Slot()[ List ] ] ]
				]
			]
		];
	}

private:
	TWeakObjectPtr<AActor> Target;
	FString TypeName;
	TArray<TSharedPtr<TArray<TSharedPtr<FString>>>> OptSources;   // keeps combo sources alive
};

// ---------------------------------------------------------------------------
// GROUPING pop-out: group / ungroup / edit group / save block as a small menu.
// ---------------------------------------------------------------------------
class SBF6GroupingMenu : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6GroupingMenu) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
		auto AddRow = [&Box](const FString& Label, const FString& Hint, TFunction<void()> Fn, bool bEnabled = true)
		{
			Box->AddSlot().AutoHeight().Padding(0, 1)
			[
				SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(8, 4)).HAlign(HAlign_Fill)
				.IsEnabled(bEnabled)
				.OnClicked_Lambda([Fn]{ Fn(); return FReply::Handled(); })
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
					[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity(FSlateColor(bEnabled ? BF6Theme::Text : BF6Theme::TextDim)).Text(FText::FromString(Label)) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(Hint)) ]
				]
			];
		};
		AddRow(TEXT("Group"), TEXT("move as one"), []{ BF6Api::HideTransientMenus(); BF6Api::GroupSelection(); });
		AddRow(TEXT("Ungroup"), TEXT("split group or block"), []{ BF6Api::HideTransientMenus(); BF6Api::UngroupSelection(); });
		AddRow(TEXT("Edit group / block"), TEXT("tab in (double-click works too)"), []{ BF6Api::HideTransientMenus(); BF6Api::BeginGroupEditFromSelection(); },
			BF6Api::SelectionGrouped() && !BF6Api::IsGroupEditing());
		AddRow(TEXT("Save as block"), TEXT("reusable prefab"), []
		{
			BF6Api::HideTransientMenus();
			BF6_PushTransient(SNew(SBF6SaveBlockPopup), GPieCenter);
		});

		ChildSlot
		[
			SNew(SBox).WidthOverride(300.f)
			[
				SNew(SBorder).BorderImage(PanelBrush()).Padding(10.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[
							SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(6, 2))
							.OnClicked_Lambda([]{ BF6Pie_Reopen(); return FReply::Handled(); })
							[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("< BACK"))) ]
						]
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontBold(13)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(TEXT("GROUPING"))) ]
					]
					+ SVerticalBox::Slot().AutoHeight()[ Box ]
				]
			]
		];
	}
};

// ---------------------------------------------------------------------------
// MULTIPLY pop-out: dead-simple duplication from the selected object. Rows
// and grids tile flush using the object's own footprint (gap adds air);
// circles ring the object, every copy facing the centre. That's it - no
// splines, no presets, nothing to learn.
// ---------------------------------------------------------------------------
class SBF6MultiplyMenu : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6MultiplyMenu) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		auto NumBox = [](TSharedPtr<SEditableTextBox>& Out, const TCHAR* Default)
		{
			return SNew(SBox).WidthOverride(52.f)[ SAssignNew(Out, SEditableTextBox).Text(FText::FromString(Default)) ];
		};
		auto Dim = [](const TCHAR* T)
		{
			return SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(T));
		};

		ChildSlot
		[
			SNew(SBox).WidthOverride(380.f)
			[
				SNew(SBorder).BorderImage(PanelBrush()).Padding(10.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[
							SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(6, 2))
							.OnClicked_Lambda([]{ BF6Pie_Reopen(); return FReply::Handled(); })
							[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("< BACK"))) ]
						]
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontBold(13)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(TEXT("MULTIPLY"))) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)[ Dim(TEXT("Row / grid:")) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ NumBox(CountBox, TEXT("5")) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0)[ Dim(TEXT("x")) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ NumBox(RowsBox, TEXT("1")) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 4, 0)[ Dim(TEXT("gap (m)")) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ NumBox(GapBox, TEXT("0")) ]
						+ SHorizontalBox::Slot().FillWidth(1) [ SNullWidget::NullWidget ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ MakePrimaryButton(TEXT("Create"), [this]
							{
								const int32 n = BF6Api::MultiplyGrid(
									FCString::Atoi(*CountBox->GetText().ToString()),
									FCString::Atoi(*RowsBox->GetText().ToString()),
									FCString::Atod(*GapBox->GetText().ToString()));
								if (n > 0) { BF6Api::HideTransientMenus(); BF6_MiniToast(FString::Printf(TEXT("Added %d flush cop%s."), n, n == 1 ? TEXT("y") : TEXT("ies"))); }
							}) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[ Dim(TEXT("Copies tile edge to edge along the object's facing. Gap adds space.")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)[ Dim(TEXT("Circle:")) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ NumBox(CircleBox, TEXT("8")) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 4, 0)[ Dim(TEXT("radius (m)")) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ NumBox(RadiusBox, TEXT("5")) ]
						+ SHorizontalBox::Slot().FillWidth(1) [ SNullWidget::NullWidget ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ MakePrimaryButton(TEXT("Create"), [this]
							{
								const int32 n = BF6Api::MultiplyCircle(
									FCString::Atoi(*CircleBox->GetText().ToString()),
									FCString::Atod(*RadiusBox->GetText().ToString()));
								if (n > 0) { BF6Api::HideTransientMenus(); BF6_MiniToast(FString::Printf(TEXT("Ringed %d copies around it."), n)); }
							}) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[ Dim(TEXT("Copies ring the selected object, each facing the centre.")) ]
					+ SVerticalBox::Slot().AutoHeight()
					[ Dim(TEXT("Looking for random natural placement? That is the SCATTER pill now - a live editor with sliders.")) ]
				]
			]
		];
	}

private:
	TSharedPtr<SEditableTextBox> CountBox, RowsBox, GapBox, CircleBox, RadiusBox;
};

// ---------------------------------------------------------------------------
// MODE SETUP pop-out: pick Conquest or Breakthrough and a count, then the
// wizard walks you through placement click by click ("Step N of M" lives in
// the top-left panel while it runs).
// ---------------------------------------------------------------------------
class SBF6ModeWizardMenu : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6ModeWizardMenu) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		auto NumBox = [](TSharedPtr<SEditableTextBox>& Out, const TCHAR* Default)
		{
			return SNew(SBox).WidthOverride(44.f)[ SAssignNew(Out, SEditableTextBox).Text(FText::FromString(Default)) ];
		};
		auto Dim = [](const TCHAR* T)
		{
			return SNew(STextBlock).Font(FontReg(9)).AutoWrapText(true).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(T));
		};

		ChildSlot
		[
			SNew(SBox).WidthOverride(380.f)
			[
				SNew(SBorder).BorderImage(PanelBrush()).Padding(10.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[
							SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(6, 2))
							.OnClicked_Lambda([]{ BF6Pie_Reopen(); return FReply::Handled(); })
							[ SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("< BACK"))) ]
						]
						+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
						[ SNew(STextBlock).Font(FontBold(13)).ColorAndOpacity(FSlateColor(BF6Theme::Accent)).Text(FText::FromString(TEXT("MODE SETUP"))) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ MakePrimaryButton(TEXT("Conquest"), [this]
							{
								BF6Api::HideTransientMenus();
								BF6Api::StartModeWizard(TEXT("Conquest"), FCString::Atoi(*FlagsBox->GetText().ToString()));
							}) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 4, 0)
						[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("flags"))) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ NumBox(FlagsBox, TEXT("3")) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[ Dim(TEXT("Both HQs, every flag with its capture area and linked spawns, and the sector that gives flags their letters. Placed step by step where you click.")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ MakePrimaryButton(TEXT("Breakthrough"), [this]
							{
								BF6Api::HideTransientMenus();
								BF6Api::StartModeWizard(TEXT("Breakthrough"), FCString::Atoi(*SectorsBox->GetText().ToString()));
							}) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 4, 0)
						[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(TEXT("sectors"))) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ NumBox(SectorsBox, TEXT("3")) ]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[ Dim(TEXT("Per sector: attacker and defender HQs plus objectives A and B, wired into their sector as you go.")) ]
				]
			]
		];
	}

private:
	TSharedPtr<SEditableTextBox> FlagsBox, SectorsBox;
};

// Assign mode is always entered FROM the attributes menu, so leaving it
// (confirm or Esc) drops the user straight back into that menu instead of
// making them re-navigate the wheel. The freshly built menu re-reads the
// actor's props, so a confirmed assign shows up immediately.
static void BF6_ReopenAttributesAfterLink()
{
	AActor* Target = GPieTarget.Get();
	if (!Target || GPieTargetType.IsEmpty()) return;
	BF6_PushTransient(SNew(SBF6AttributesMenu).TargetActor(Target).TypeName(GPieTargetType), GPieCenter);
}

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
		else if (Pick == TEXT("RESET CENTER"))
		{
			BF6_MiniToast(BF6Api::ResetVolumeCenter()
				? TEXT("Zone origin moved to the middle - the shape didn't move.")
				: TEXT("Already centred."));
		}
		else                                   BF6Api::FinishVolumeEdit();
		return;
	}

	if (Mode == EBF6PieMode::Props)
	{
		AActor* Target = GPieTarget.Get();
		if (!Target) return;
		if (Pick == TEXT("EDIT POINTS")) { BF6Api::BeginVolumeEdit(Target); return; }
		if (Pick == TEXT("SET CAMERA"))
		{
			BF6Api::SetCameraFromView();
			BF6_MiniToast(TEXT("Camera set to the editor's view - one Ctrl+Z reverts."));
			return;
		}
		if (Pick == TEXT("PICK PLACE"))
		{
			if (BF6Api::BeginPickPlace())
				BF6_MiniToast(TEXT("Riding the cursor - click to place, Esc puts it back."));
			return;
		}
		if (Pick == TEXT("COLLISION"))
		{
			const int32 n = BF6Api::ShowCollisionOverlay(0);
			BF6_MiniToast(n > 0
				? FString::Printf(TEXT("Collision shown on %d object%s - red is what you hit."), n, n == 1 ? TEXT("") : TEXT("s"))
				: FString(TEXT("No collision shape for this one.")));
			return;
		}
		if (Pick == TEXT("HIDE COLLISION"))
		{
			const int32 n = BF6Api::HideCollisionOverlay();
			BF6_MiniToast(FString::Printf(TEXT("Collision hidden on %d object%s."), n, n == 1 ? TEXT("") : TEXT("s")));
			return;
		}
		if (Pick == TEXT("SELECT SIMILAR")) { BF6Api::SelectSimilar(); return; }
		if (Pick == TEXT("ATTRIBUTES"))
		{
			BF6_PushTransient(SNew(SBF6AttributesMenu).TargetActor(Target).TypeName(GPieTargetType), Center);
			return;
		}
		if (Pick == TEXT("MULTIPLY"))
		{
			BF6_PushTransient(SNew(SBF6MultiplyMenu), Center);
			return;
		}
		if (Pick == TEXT("SCATTER"))
		{
			// no pop-out: the live panel docks on the right and the scatter
			// appears immediately, re-forming as the sliders move
			BF6Api::BeginScatterLive();
			return;
		}
		if (Pick == TEXT("GROUPING"))
		{
			BF6_PushTransient(SNew(SBF6GroupingMenu), Center);
			return;
		}
		return;
	}

	// Top ring: OBJECTS steps into the catalogue, the rest open their panel.
	if (Pick == TEXT("< BACK"))
	{
		GPieMode = EBF6PieMode::Place;
		GPieCenter = Center;
		BF6Pie_Attach();
		return;
	}
	if (Pick == TEXT("OBJECTS"))
	{
		GPieMode = EBF6PieMode::Objects;
		GPieCenter = Center;
		BF6Pie_Attach();   // same wheel, one step in
		return;
	}
	if (Pick == TEXT("COLLISION"))
	{
		BF6_PushTransient(SNew(SBF6CollisionPanel), Center);
		return;
	}
	if (Pick == TEXT("COLORIZE"))
	{
		BF6_PushTransient(SNew(SBF6ColorizePanel), Center);
		return;
	}
	if (Pick == TEXT("VALIDATE"))
	{
		BF6_PushTransient(SNew(SBF6LintPanel), Center);
		return;
	}
	// Objects step: FULL LIBRARY pins the object library up; BLOCKS browses the
	// user's prefabs; a category opens its object list
	if (Pick == TEXT("FULL LIBRARY"))
	{
		if (TSharedPtr<SBF6LibraryPanel> L = GLibraryPanel.Pin()) L->OpenFull();
		return;
	}
	if (Pick == TEXT("BLOCKS"))
	{
		BF6_PushTransient(SNew(SBF6BlocksPopup), Center);
		return;
	}
	if (Pick == TEXT("OBJECT IDS"))
	{
		BF6_PushTransient(SNew(SBF6ObjIdPanel), Center);
		return;
	}
	if (Pick == TEXT("MODE SETUP"))
	{
		BF6_PushTransient(SNew(SBF6ModeWizardMenu), Center);
		return;
	}
	BF6_PushTransient(SNew(SBF6CategoryPopup).Category(Pick), Center);
}

static void BF6Pie_Cancel() { BF6Pie_Close(); }

// ---------------------------------------------------------------------------
// Input handler: SPACE over the viewport opens the pie; mouse direction picks a
// wedge; SPACE or LEFT-CLICK confirms; centre + confirm, or ESC, cancels.
// ---------------------------------------------------------------------------
// True while the user is typing in any text field - our single-key binds must
// never fire then (T, Space, F1 are all normal characters).
static bool BF6_TextFieldFocused()
{
	TSharedPtr<SWidget> W = FSlateApplication::Get().GetKeyboardFocusedWidget();
	return W.IsValid() && W->GetTypeAsString().Contains(TEXT("EditableText"));
}

static void BF6_MiniToast(const FString& Msg)
{
	FNotificationInfo Info(FText::FromString(Msg));
	Info.ExpireDuration = 1.6f;
	FSlateNotificationManager::Get().AddNotification(Info);
}

// ---- Godot-style camera navigation (MMB orbit / Shift+MMB pan) ----
// On by default for Godot muscle memory; toggleable from the F1 sheet for
// people who want Unreal's native MMB pan back. Persisted per user.
static bool GGodotCamera = true;
static bool GGodotCameraLoaded = false;

static bool BF6_GodotCameraOn()
{
	if (!GGodotCameraLoaded)
	{
		GGodotCameraLoaded = true;
		GConfig->GetBool(TEXT("BF6UnrealSDK"), TEXT("GodotCamera"), GGodotCamera, GEditorPerProjectIni);
	}
	return GGodotCamera;
}

static void BF6_SetGodotCamera(bool bOn)
{
	GGodotCamera = bOn; GGodotCameraLoaded = true;
	GConfig->SetBool(TEXT("BF6UnrealSDK"), TEXT("GodotCamera"), bOn, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

// ---- Godot-style hold-Ctrl snapping ----
// While Ctrl is down, grid + angle snap turn on at Godot's default increments
// (1 m / 15 degrees); releasing restores the user's own toolbar toggles.
static bool  GSnapHeld = false;
static bool  GSnapPrevGrid = false, GSnapPrevRot = false;
static int32 GSnapPrevPosIdx = 0, GSnapPrevRotIdx = 0;

static void BF6_SnapHold(bool bDown)
{
	ULevelEditorViewportSettings* S = GetMutableDefault<ULevelEditorViewportSettings>();
	if (!S) return;
	if (bDown && !GSnapHeld)
	{
		GSnapHeld = true;
		GSnapPrevGrid = S->GridEnabled; GSnapPrevRot = S->RotGridEnabled;
		GSnapPrevPosIdx = S->CurrentPosGridSize; GSnapPrevRotIdx = S->CurrentRotGridSize;
		S->GridEnabled = true; S->RotGridEnabled = true;
		for (int32 i = 0; i < S->DecimalGridSizes.Num(); i++)
			if (FMath::IsNearlyEqual(S->DecimalGridSizes[i], 100.f)) { S->CurrentPosGridSize = i; break; }
		for (int32 i = 0; i < S->CommonRotGridSizes.Num(); i++)
			if (FMath::IsNearlyEqual(S->CommonRotGridSizes[i], 15.f)) { S->CurrentRotGridSize = i; break; }
	}
	else if (!bDown && GSnapHeld)
	{
		GSnapHeld = false;
		S->GridEnabled = GSnapPrevGrid; S->RotGridEnabled = GSnapPrevRot;
		S->CurrentPosGridSize = GSnapPrevPosIdx; S->CurrentRotGridSize = GSnapPrevRotIdx;
	}
}

// ---- the F1 controls overlay (Portal-styled cheat sheet) ----
static TSharedPtr<SWidget>      GControls;
static TWeakPtr<SLevelViewport> GControlsVP;
static void BF6_ControlsClose();
static void BF6_ControlsToggle();

namespace
{
	struct FBF6Bind { const TCHAR* Keys; const TCHAR* What; bool bGodot; };

	TSharedRef<SWidget> MakeKeyChip(const FString& Keys)
	{
		static FSlateColorBrush ChipBg(FLinearColor::White);
		return SNew(SBorder)
			.BorderImage(&ChipBg).BorderBackgroundColor(BF6Theme::PanelLight)
			.Padding(FMargin(8, 2))
			[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity(BF6Theme::Text).Text(FText::FromString(Keys)) ];
	}

	TSharedRef<SWidget> MakeBindRow(const FBF6Bind& B)
	{
		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ SNew(SBox).WidthOverride(190).HAlign(HAlign_Left) [ MakeKeyChip(B.Keys) ] ]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(10, 0, 0, 0)
			[ SNew(STextBlock).Font(FontReg(11)).ColorAndOpacity(BF6Theme::TextBlue).Text(FText::FromString(B.What)) ];
		if (B.bGodot)
			Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
			[ SNew(STextBlock).Font(FontBold(8)).ColorAndOpacity(BF6Theme::TextDim).Text(FText::FromString(TEXT("SAME AS GODOT"))) ];
		return Row;
	}

	TSharedRef<SWidget> MakeBindSection(const FString& Title, const TArray<FBF6Bind>& Binds)
	{
		TSharedRef<SVerticalBox> Box = SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 14, 0, 6)
			[ SNew(STextBlock).Font(FontBold(10)).ColorAndOpacity(BF6Theme::TextDim).Text(FText::FromString(Title)) ];
		for (const FBF6Bind& B : Binds)
			Box->AddSlot().AutoHeight().Padding(0, 3) [ MakeBindRow(B) ];
		return Box;
	}

	TSharedRef<SWidget> BF6_ControlsPanel()
	{
		static FSlateColorBrush ScrimBrush(FLinearColor(0.f, 0.f, 0.f, 0.65f));
		static FSlateColorBrush LineBrush(FLinearColor::White);
		static FSlateColorBrush PanelBrush(FLinearColor::White);

		TArray<FBF6Bind> Camera = {
			{ TEXT("RMB + WASD"),        TEXT("Fly the camera (scroll wheel changes speed)"), true },
			{ TEXT("F"),                 TEXT("Frame the selected object"), true } };
		if (BF6_GodotCameraOn())
		{
			Camera.Add({ TEXT("MMB"),         TEXT("Orbit (selection, or the point under the cursor)"), true });
			Camera.Add({ TEXT("Shift + MMB"), TEXT("Pan"), true });
		}
		else
		{
			Camera.Add({ TEXT("Alt + LMB"),   TEXT("Orbit around the selection"), false });
			Camera.Add({ TEXT("MMB"),         TEXT("Pan (Unreal default)"), false });
		}
		const TArray<FBF6Bind> Transform = {
			{ TEXT("Q / W / E / R"),     TEXT("Select / Move / Rotate / Scale"), true },
			{ TEXT("Hold Ctrl"),         TEXT("Snap while dragging (1 m, 15 degrees)"), true },
			{ TEXT("T"),                 TEXT("Toggle local / world space"), true },
			{ TEXT("Ctrl + D"),          TEXT("Duplicate the selection"), true },
			{ TEXT("Del"),               TEXT("Delete the selection"), true },
			{ TEXT("Ctrl + Z / Ctrl + Shift + Z"), TEXT("Undo / Redo"), true } };
		const TArray<FBF6Bind> Build = {
			{ TEXT("Space"),             TEXT("Object radial at the crosshair; with a gameplay object selected, its attributes"), false },
			{ TEXT("Double-click"),      TEXT("Place the highlighted library object"), false },
			{ TEXT("Esc"),               TEXT("Cancel / close / back"), false } };
		const TArray<FBF6Bind> Zones = {
			{ TEXT("Click a zone"),      TEXT("Its points appear for editing"), true },
			{ TEXT("Ctrl + LMB"),        TEXT("Add a point on the nearest edge"), true },
			{ TEXT("Ctrl + RMB"),        TEXT("Delete the point under the cursor"), true },
			{ TEXT("Enter or Esc"),      TEXT("Finish editing the zone"), false } };

		return SNew(SBorder)
			.BorderImage(&ScrimBrush)
			.HAlign(HAlign_Center).VAlign(VAlign_Center)
			.OnMouseButtonDown_Lambda([](const FGeometry&, const FPointerEvent&) { BF6_ControlsClose(); return FReply::Handled(); })
			[
				SNew(SBox).WidthOverride(680)
				[
					SNew(SBorder).BorderImage(&LineBrush).BorderBackgroundColor(BF6Theme::Line).Padding(1)
					[
						SNew(SBorder).BorderImage(&PanelBrush).BorderBackgroundColor(BF6Theme::Panel).Padding(FMargin(28, 20, 28, 22))
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight()
							[ SNew(STextBlock).Font(FontBold(16)).ColorAndOpacity(BF6Theme::Text).Text(FText::FromString(TEXT("CONTROLS"))) ]
							+ SVerticalBox::Slot().AutoHeight() [ MakeBindSection(TEXT("CAMERA"), Camera) ]
							+ SVerticalBox::Slot().AutoHeight() [ MakeBindSection(TEXT("TRANSFORM"), Transform) ]
							+ SVerticalBox::Slot().AutoHeight() [ MakeBindSection(TEXT("BUILDING"), Build) ]
							+ SVerticalBox::Slot().AutoHeight() [ MakeBindSection(TEXT("ZONES"), Zones) ]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 16, 0, 0)
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
								[
									SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(8, 3))
									// stop the click from reaching the scrim (which closes the sheet)
									.OnClicked_Lambda([]
									{
										BF6_SetGodotCamera(!BF6_GodotCameraOn());
										// rebuild the sheet so the camera rows update
										BF6_ControlsClose(); BF6_ControlsToggle();
										return FReply::Handled();
									})
									[ SNew(STextBlock).Font(FontBold(10))
										.ColorAndOpacity_Lambda([]{ return FSlateColor(BF6_GodotCameraOn() ? BF6Theme::Accent : BF6Theme::TextDim); })
										.Text_Lambda([]{ return FText::FromString(BF6_GodotCameraOn()
											? TEXT("GODOT CAMERA: ON  (click for Unreal's default MMB pan)")
											: TEXT("GODOT CAMERA: OFF  (click for MMB orbit like Godot)")); }) ]
								]
							]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
							[ SNew(STextBlock).Font(FontReg(10)).ColorAndOpacity(BF6Theme::TextDim).Text(FText::FromString(TEXT("F1, Esc, or click anywhere to close. Sessions autosave every 60 seconds."))) ]
						]
					]
				]
			];
	}
}

static void BF6_ControlsClose()
{
	if (GControls.IsValid())
		if (TSharedPtr<SLevelViewport> VP = GControlsVP.Pin())
			VP->RemoveOverlayWidget(GControls.ToSharedRef());
	GControls.Reset(); GControlsVP.Reset();
}

static void BF6_ControlsToggle()
{
	if (GControls.IsValid()) { BF6_ControlsClose(); return; }
	FLevelEditorModule& LE = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	TSharedPtr<SLevelViewport> VP = LE.GetFirstActiveLevelViewport();
	if (!VP.IsValid()) return;
	GControls = BF6_ControlsPanel();
	VP->AddOverlayWidget(GControls.ToSharedRef(), 200);
	GControlsVP = VP;
}

class FBF6InputProcessor : public IInputProcessor
{
public:
	virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}

	virtual bool HandleKeyDownEvent(FSlateApplication& App, const FKeyEvent& E) override
	{
		const FKey K = E.GetKey();
		// typing in a text box: none of our binds may fire
		if (BF6_TextFieldFocused()) return false;

		// Godot-style hold-Ctrl snapping (never consumed - Ctrl still combines)
		if ((K == EKeys::LeftControl || K == EKeys::RightControl) && !E.IsRepeat() && BF6Api::IsBuildOverlayActive())
			BF6_SnapHold(true);

		// Godot's redo chord alongside Unreal's Ctrl+Y
		if (K == EKeys::Z && E.IsControlDown() && E.IsShiftDown() && BF6Api::IsBuildOverlayActive())
		{
			if (GEditor) GEditor->RedoTransaction();
			return true;
		}
		// Godot's local/world toggle
		if (K == EKeys::T && !E.IsControlDown() && !E.IsShiftDown() && !E.IsAltDown()
			&& BF6Api::IsBuildOverlayActive() && !GTransientMenu.IsValid() && !BF6Pie_Active())
		{
			FEditorModeTools& MT = GLevelEditorModeTools();
			const bool bWorld = MT.GetCoordSystem() == COORD_World;
			MT.SetCoordSystem(bWorld ? COORD_Local : COORD_World);
			BF6_MiniToast(bWorld ? TEXT("Local space") : TEXT("World space"));
			return true;
		}
		// the controls cheat sheet
		if (K == EKeys::F1 && BF6Api::IsBuildOverlayActive() && !E.IsRepeat())
		{
			BF6_ControlsToggle();
			return true;
		}
		if (K == EKeys::SpaceBar)
		{
			if (BF6Pie_Active()) { if (!E.IsRepeat()) BF6Pie_Confirm(); return true; }
			if (E.IsRepeat()) return false;
			// assigning a link: SPACE commits the current selection as the target,
			// then returns to the attributes menu the pick started from
			if (BF6Api::IsLinkPicking()) { BF6Api::ConfirmLinkPick(); BF6_ReopenAttributesAfterLink(); return true; }
			if (!BF6Api::IsBuildOverlayActive() || !BF6Api::IsEditing() || GTransientMenu.IsValid()) return false;
			FVector W; if (!BF6Api::WorldFromViewportCursor(W)) return false;   // only over a viewport
			BF6Pie_Open();
			return true;
		}
		if (K == EKeys::Escape)
		{
			if (BF6Api::IsPickPlacing())
			{
				BF6Api::CancelPickPlace();
				BF6_MiniToast(TEXT("Put back where it was."));
				return true;
			}
			if (bMovePend) { bMovePend = false; bMoveDragging = false; BF6Api::CancelDragMove(); return true; }
			if (bBoxDragging) { bBoxDown = false; bBoxDragging = false; BF6Api::CancelBoxSelect(); return true; }
			if (GControls.IsValid()) { BF6_ControlsClose(); return true; }
			if (BF6Pie_Active()) { BF6Pie_Cancel(); return true; }
			// a popup menu (attributes, grouping, ...) is up: Esc closes it. The
			// menu can't rely on its own key handling - after a viewport click
			// the keyboard focus is anywhere but the menu. Typing in one of its
			// boxes keeps Esc for the box itself.
			if (GTransientMenu.IsValid() && !BF6_TextFieldFocused())
			{
				BF6Api::HideTransientMenus();
				return true;
			}
			if (TSharedPtr<SBF6LibraryPanel> L = GLibraryPanel.Pin())
				if (L->IsOpen() && !GTransientMenu.IsValid()) { L->ForceClose(); return true; }
			// Esc backs out of assign mode INTO the attributes menu it came from
			if (BF6Api::IsLinkPicking()) { BF6Api::CancelLinkPick(); BF6_ReopenAttributesAfterLink(); return true; }
			// Esc while outlining a scatter area throws just the outline away;
			// a second Esc ends the whole scatter session. Typing in the
			// panel's value box keeps Esc for the box itself.
			if (BF6Api::IsScatterDrawing() && !BF6_TextFieldFocused()) { BF6Api::CancelScatterDraw(); return true; }
			if (BF6Api::IsScatterLive() && !BF6_TextFieldFocused())
			{
				BF6Api::CancelScatterLive();
				BF6_MiniToast(TEXT("Scatter removed."));
				return true;
			}
			// Esc stops the mode wizard; placed steps stay (Ctrl+Z removes them)
			if (BF6Api::IsModeWizardActive())
			{
				BF6Api::CancelModeWizard();
				BF6_MiniToast(TEXT("Mode setup stopped. Placed steps stay - Ctrl+Z removes them."));
				return true;
			}
			// Esc also DESELECTS the volume, or the auto-edit restarts next tick
			if (BF6Api::IsVolumeEditing()) { BF6Api::FinishVolumeEdit(); BF6Api::ClearSelection(); return true; }
			if (BF6Api::IsObbEditing()) { BF6Api::FinishObbEdit(); BF6Api::ClearSelection(); return true; }
			if (BF6Api::IsGroupEditing())
			{
				// Esc = throw the edits away and put everything back
				const bool bBlk = BF6Api::GroupEditIsBlock();
				BF6Api::FinishGroupEdit(false);
				BF6_MiniToast(bBlk ? TEXT("Block edits reverted.") : TEXT("Group edits reverted."));
				return true;
			}
			// nothing modal left: Esc simply deselects, like Godot. This is the
			// missing last step of backing out of an assign - the first Esc
			// returns to the attributes menu, the second closes it, and this
			// one clears the selection the pick (or confirm) left behind.
			if (BF6Api::IsBuildOverlayActive() && BF6Api::IsEditing()
				&& !BF6_TextFieldFocused() && !BF6Api::IsViewportPiloting()
				&& BF6Api::HasSelection())
			{
				BF6Api::ClearSelection();
				return true;
			}
			// on the map screen with a session running: ESC returns to the build
			if (GRoot.IsValid() && !GRoot->IsBuildScreen() && !BF6Api::CurrentLevel().IsEmpty() && !GTransientMenu.IsValid())
			{
				GRoot->ShowBuild();
				return true;
			}
		}
		// ENTER confirms an assign in progress, same as Space, and returns to
		// the attributes menu the pick started from
		if (K == EKeys::Enter && BF6Api::IsLinkPicking())
		{
			BF6Api::ConfirmLinkPick();
			BF6_ReopenAttributesAfterLink();
			return true;
		}
		// Ctrl+Z / Ctrl+Y while a scatter is live: undo / redo the OUTLINE
		// edits (corner add, insert, delete, drag) - they live outside the
		// editor's transaction system. Falls through to the editor's own undo
		// when there is no outline history.
		if (E.IsControlDown() && !BF6_TextFieldFocused() && BF6Api::IsScatterLive())
		{
			if (K == EKeys::Z && !E.IsShiftDown() && BF6Api::ScatterOutlineUndo()) return true;
			if ((K == EKeys::Y || (K == EKeys::Z && E.IsShiftDown())) && BF6Api::ScatterOutlineRedo()) return true;
		}
		// DEL on a scatter outline corner removes just that corner - checked
		// first so hovering a dot never deletes the selection instead
		if (K == EKeys::Delete && !BF6Pie_Active() && !GTransientMenu.IsValid() && BF6Api::IsScatterLive())
		{
			const int32 Dot = BF6Api::ScatterDotUnderMouse();
			if (Dot != INDEX_NONE) { BF6Api::ScatterDeletePointByIndex(Dot); return true; }
		}
		// DEL removes the zone point under the cursor (or the last-clicked one).
		// Consuming it also protects the zone itself: the volume actor is the
		// selection during point editing, so the editor's own Delete would
		// otherwise remove the whole zone.
		if (K == EKeys::Delete && !BF6Pie_Active() && !GTransientMenu.IsValid() && BF6Api::IsVolumeEditing())
		{
			const int32 Dot = BF6Api::ZoneDotUnderMouse();
			if (Dot != INDEX_NONE) BF6Api::VolumeDeletePointByIndex(Dot);
			else BF6Api::VolumeDeletePoint();
			return true;
		}
		// DEL on our objects: the fast path empties proc-mesh payloads before
		// the transaction, so deleting a scattered forest is instant instead
		// of serializing megabytes of vertex data into the undo buffer
		if (K == EKeys::Delete && !BF6Pie_Active() && !GTransientMenu.IsValid() && !BF6_TextFieldFocused()
			&& BF6Api::IsBuildOverlayActive() && BF6Api::IsEditing() && !BF6Api::IsVolumeEditing())
		{
			if (BF6Api::DeleteSelectionFast()) return true;
		}
		// ENTER also bakes and ends a zone-point edit (when no popup is up);
		// deselecting stops the auto-edit from restarting it next tick
		if (K == EKeys::Enter && !BF6Pie_Active() && !GTransientMenu.IsValid() && BF6Api::IsVolumeEditing())
		{
			BF6Api::FinishVolumeEdit();
			BF6Api::ClearSelection();
			return true;
		}
		// ...and a box-face edit
		if (K == EKeys::Enter && !BF6Pie_Active() && !GTransientMenu.IsValid() && BF6Api::IsObbEditing())
		{
			BF6Api::FinishObbEdit();
			BF6Api::ClearSelection();
			return true;
		}
		// ENTER while carrying sets the selection down, same as the click
		if (K == EKeys::Enter && !BF6Pie_Active() && !GTransientMenu.IsValid() && BF6Api::IsPickPlacing())
		{
			BF6Api::FinishPickPlace();
			BF6_MiniToast(TEXT("Placed - one Ctrl+Z puts it back."));
			return true;
		}
		// ENTER while outlining a scatter area finishes the outline; while a
		// scatter is live it applies the whole scatter as one undoable action
		if (K == EKeys::Enter && !BF6Pie_Active() && !GTransientMenu.IsValid() && !BF6_TextFieldFocused() && BF6Api::IsScatterDrawing())
		{
			BF6Api::FinishScatterDraw();
			return true;
		}
		if (K == EKeys::Enter && !BF6Pie_Active() && !GTransientMenu.IsValid() && !BF6_TextFieldFocused() && BF6Api::IsScatterLive())
		{
			BF6Api::ApplyScatterLive();
			return true;
		}
		// ...and ENTER keeps a group/block edit (a block re-saves, which also
		// refreshes every other placed copy)
		if (K == EKeys::Enter && !BF6Pie_Active() && !GTransientMenu.IsValid() && BF6Api::IsGroupEditing())
		{
			const bool bBlk = BF6Api::GroupEditIsBlock();
			BF6Api::FinishGroupEdit(true);
			BF6_MiniToast(bBlk ? TEXT("Block saved - every placed copy updated.") : TEXT("Group locked."));
			return true;
		}
		// snap-build: Alt+Arrows duplicates the selection FLUSH in the pressed
		// direction (Ctrl+Alt+Up/Down stacks vertically). Key repeat is on
		// purpose - holding the key extends the run.
		if (E.IsAltDown() && !BF6Pie_Active() && !GTransientMenu.IsValid() && !BF6_TextFieldFocused()
			&& BF6Api::IsBuildOverlayActive() && BF6Api::IsEditing())
		{
			int32 Dir = -1;
			if (K == EKeys::Right)     Dir = 0;
			else if (K == EKeys::Left) Dir = 1;
			else if (K == EKeys::Up)   Dir = E.IsControlDown() ? 4 : 2;
			else if (K == EKeys::Down) Dir = E.IsControlDown() ? 5 : 3;
			if (Dir >= 0 && BF6Api::SnapBuildDuplicate(Dir)) return true;
		}
		return false;
	}

	virtual bool HandleKeyUpEvent(FSlateApplication& App, const FKeyEvent& E) override
	{
		// always restore snapping on Ctrl release, even if focus moved mid-hold
		if (E.GetKey() == EKeys::LeftControl || E.GetKey() == EKeys::RightControl)
			BF6_SnapHold(false);
		return false;
	}

	virtual bool HandleMouseMoveEvent(FSlateApplication& App, const FPointerEvent& E) override
	{
		if (BF6Pie_Active()) BF6Pie_Update(E.GetScreenSpacePosition());
		// pick place: the selection rides the cursor (never consumed, so the
		// camera and everything else keep working while carrying)
		if (BF6Api::IsPickPlacing() && !BF6Pie_Active()) BF6Api::TickPickPlace(E.IsControlDown());
		if (bRightDown && FVector2D::Distance(E.GetScreenSpacePosition(), RightDownPos) > 6.f) bRightDragged = true;
		// drag-move: a few pixels of LMB drag on a selected object starts it
		if (bMovePend)
		{
			if (!bMoveDragging && FVector2D::Distance(E.GetScreenSpacePosition(), MoveDownPos) > 4.f)
				bMoveDragging = true;
			if (bMoveDragging) BF6Api::UpdateDragMove(E.IsControlDown());
			return true;
		}
		// box select: a few pixels of LMB drag on empty ground starts the marquee
		if (bBoxDown)
		{
			if (!bBoxDragging && FVector2D::Distance(E.GetScreenSpacePosition(), BoxDownPos) > 4.f)
			{
				bBoxDragging = true;
				BF6Api::BeginBoxSelect();
			}
			if (bBoxDragging) BF6Api::UpdateBoxSelect();
			return true;
		}
		if (BF6Api::IsZoneDotDragging()) { BF6Api::DragZoneDotToCursor(); return true; }
		if (BF6Api::IsScatterDotDragging()) { BF6Api::DragScatterDotToCursor(); return true; }
		if (bMMBNav)
		{
			const FVector2D D = E.GetScreenSpacePosition() - NavLast;
			NavLast = E.GetScreenSpacePosition();
			if (bMMBPan) BF6Api::CameraPan(D, NavPivot);
			else         BF6Api::CameraOrbit(D, NavPivot);
			return true;
		}
		return false;
	}

	// The 3D hit-proxy test looks straight THROUGH Slate overlays, so any
	// branch that claims clicks "on the map" must first make sure the cursor
	// is not on our own UI (panel borders and controls float over sky a lot -
	// the scatter panel's sliders were unusable without this).
	static bool BF6_CursorOverSlateUI(FSlateApplication& App, const FVector2D& ScreenPos)
	{
		FWidgetPath Path = App.LocateWindowUnderMouse(ScreenPos, App.GetInteractiveTopLevelWindows());
		if (!Path.IsValid()) return false;
		for (int32 i = Path.Widgets.Num() - 1; i >= 0; i--)
		{
			const FName Ty = Path.Widgets[i].Widget->GetType();
			if (Ty == "SViewport") return false;   // reached the 3D view first: not UI
			if (Ty == "SButton" || Ty == "SSlider" || Ty == "SCheckBox"
				|| Ty == "SEditableTextBox" || Ty == "SBorder" || Ty == "SProgressBar")
				return true;                       // a panel or control is under the mouse
		}
		return false;
	}

	virtual bool HandleMouseButtonDownEvent(FSlateApplication& App, const FPointerEvent& E) override
	{
		if (GControls.IsValid()) { BF6_ControlsClose(); return true; }
		if (BF6Pie_Active() && E.GetEffectingButton() == EKeys::LeftMouseButton) { BF6Pie_Confirm(); return true; }

		// pick place: the click sets the carried selection down right here
		if (E.GetEffectingButton() == EKeys::LeftMouseButton && BF6Api::IsPickPlacing()
			&& !GTransientMenu.IsValid()
			&& !BF6_CursorOverSlateUI(App, E.GetScreenSpacePosition()))
		{
			BF6Api::FinishPickPlace();
			BF6_MiniToast(TEXT("Placed - one Ctrl+Z puts it back."));
			return true;
		}

		// read-only base: any click on the map earns a reminder (throttled, and
		// NOT consumed - browsing and orbiting the base stays free)
		if (E.GetEffectingButton() == EKeys::LeftMouseButton
			&& BF6Api::IsBuildOverlayActive() && !BF6Api::IsEditing()
			&& !GTransientMenu.IsValid() && !GControls.IsValid() && !BF6Pie_Active()
			&& !BF6_CursorOverSlateUI(App, E.GetScreenSpacePosition()))
		{
			static double LastWarn = 0.0;
			const double Now = FPlatformTime::Seconds();
			if (Now - LastWarn > 5.0)
			{
				LastWarn = Now;
				BF6_MiniToast(TEXT("Read-only base - hit Create in the bottom right, or resume a custom level from < Maps."));
			}
		}

		// assign mode: clicking a candidate marker (de)selects that target
		if (E.GetEffectingButton() == EKeys::LeftMouseButton && BF6Api::IsLinkPicking())
		{
			const int32 Dot = BF6Api::LinkDotUnderMouse();
			if (Dot != INDEX_NONE) { BF6Api::ToggleLinkCandidate(Dot); return true; }
		}

		// mode setup wizard: every click places the current step's bundle
		if (E.GetEffectingButton() == EKeys::LeftMouseButton && BF6Api::IsModeWizardActive()
			&& !GTransientMenu.IsValid() && !GControls.IsValid()
			&& !BF6_CursorOverSlateUI(App, E.GetScreenSpacePosition()))
		{
			FVector W;
			if (BF6Api::WorldFromViewportCursor(W)) { BF6Api::ModeWizardPlaceAt(W); return true; }
		}

		// scatter outline corners: grabbing a dot starts a direct drag (the
		// region follows live, the copies refill on release) - checked BEFORE
		// add-corner so a click near an existing dot moves it instead. Ctrl
		// stays free for the insert/delete gestures below.
		if (E.GetEffectingButton() == EKeys::LeftMouseButton && !E.IsControlDown() && BF6Api::IsScatterLive()
			&& !GTransientMenu.IsValid() && !GControls.IsValid() && !BF6Pie_Active())
		{
			const int32 Dot = BF6Api::ScatterDotUnderMouse();
			if (Dot != INDEX_NONE) { BF6Api::BeginScatterDotDrag(Dot); return true; }
		}

		// scatter outline: every click on the map adds a corner (the fill
		// regenerates live from the third corner on). Clicks on the scatter
		// panel itself stay the panel's; Ctrl+LMB means INSERT at the edge
		// preview, never append - it falls through to the gesture below.
		if (E.GetEffectingButton() == EKeys::LeftMouseButton && !E.IsControlDown() && BF6Api::IsScatterDrawing()
			&& !GTransientMenu.IsValid() && !GControls.IsValid() && !BF6Pie_Active()
			&& !BF6_CursorOverSlateUI(App, E.GetScreenSpacePosition()))
		{
			FVector W;
			if (BF6Api::WorldFromViewportCursor(W)) { BF6Api::ScatterDrawAddPoint(W); return true; }
		}

		// Godot-style dots: grabbing one starts a direct drag (consumed, so the
		// click never reaches the viewport and the zone stays selected)
		if (E.GetEffectingButton() == EKeys::LeftMouseButton && !E.IsControlDown() && BF6Api::IsVolumeEditing())
		{
			const int32 Dot = BF6Api::ZoneDotUnderMouse();
			if (Dot != INDEX_NONE) { BF6Api::BeginZoneDotDrag(Dot); return true; }
		}

		// scatter outline, volume-style: Ctrl+LMB inserts a corner at the edge
		// preview dot; Ctrl+RMB on a corner dot deletes it. Claimed only when
		// a preview or dot is actually there, so Ctrl+click stays native
		if (E.IsControlDown() && BF6Api::IsScatterLive() && !BF6Pie_Active() && !GTransientMenu.IsValid())
		{
			if (E.GetEffectingButton() == EKeys::LeftMouseButton)
			{
				FVector2D EPx;
				if (BF6Api::GetScatterEdgePreview(EPx)) { BF6Api::ScatterAddPointAtPreview(); return true; }
			}
			if (E.GetEffectingButton() == EKeys::RightMouseButton)
			{
				const int32 Dot = BF6Api::ScatterDotUnderMouse();
				if (Dot != INDEX_NONE) { BF6Api::ScatterDeletePointByIndex(Dot); return true; }
			}
		}

		// Godot's zone gestures: Ctrl+LMB adds a point exactly at the edge
		// preview dot; Ctrl+RMB on a dot deletes that point
		if (E.IsControlDown() && BF6Api::IsVolumeEditing() && !BF6Pie_Active())
		{
			if (E.GetEffectingButton() == EKeys::LeftMouseButton)
			{ BF6Api::VolumeAddPointAtPreview(); return true; }
			if (E.GetEffectingButton() == EKeys::RightMouseButton)
			{
				const int32 Dot = BF6Api::ZoneDotUnderMouse();
				if (Dot != INDEX_NONE) { BF6Api::VolumeDeletePointByIndex(Dot); return true; }
				FVector W;
				if (BF6Api::WorldFromViewportCursor(W)) { BF6Api::VolumeDeletePointAt(W); return true; }
			}
		}

		// Godot navigation: MMB orbits, Shift+MMB pans (claimed only over the
		// viewport, with no pie/menu up, and only while the toggle is on)
		if (E.GetEffectingButton() == EKeys::MiddleMouseButton && BF6_GodotCameraOn()
			&& BF6Api::IsBuildOverlayActive() && !BF6Pie_Active() && !GTransientMenu.IsValid() && !GControls.IsValid())
		{
			FVector Under;
			if (BF6Api::WorldFromViewportCursor(Under))
			{
				bMMBNav = true;
				bMMBPan = E.IsShiftDown();
				NavLast = E.GetScreenSpacePosition();
				if (bMMBPan) NavPivot = Under;                    // pan speed reference
				else if (!BF6Api::ComputeOrbitPivot(NavPivot)) NavPivot = Under;
				return true;   // keep Unreal's own MMB pan out of the way
			}
		}

		// Godot defaults, decided WITHOUT hit proxies (they proved stale at
		// mouse-down): press an object = select it and drag moves it; press
		// empty ground = click deselects, drag rubber-bands; the gizmo's
		// screen zone is never claimed so axis drags stay native. Shift-click
		// on an object stays native additive select.
		if (E.GetEffectingButton() == EKeys::LeftMouseButton && BF6_GodotCameraOn()
			&& !E.IsControlDown() && !E.IsAltDown()
			&& BF6Api::IsBuildOverlayActive() && BF6Api::IsEditing()
			&& !BF6Pie_Active() && !GTransientMenu.IsValid() && !GControls.IsValid()
			&& !BF6Api::IsLinkPicking() && !BF6Api::IsModeWizardActive()
			&& !BF6Api::IsVolumeEditing() && !BF6Api::IsObbEditing()
			&& !BF6_CursorOverSlateUI(App, E.GetScreenSpacePosition()))
		{
			AActor* Under = nullptr;
			const int32 Cls = BF6Api::ClassifyCursorForGodotClick(Under);
			if (Cls == 1 && !E.IsShiftDown() && BF6Api::BeginDragMoveOn(Under))
			{
				bMovePend = true;
				bMoveDragging = false;
				MoveDownPos = E.GetScreenSpacePosition();
				return true;
			}
			if (Cls == 0)
			{
				bBoxDown = true;
				bBoxDragging = false;
				BoxDownPos = E.GetScreenSpacePosition();
				return true;
			}
			// Cls == 2 (gizmo zone) or Shift on an actor: Unreal's click
		}

		if (E.GetEffectingButton() == EKeys::RightMouseButton)
		{
			bRightDown = true;
			bRightDragged = false;
			RightDownPos = E.GetScreenSpacePosition();
		}
		return false;   // never consume the down - camera look must still work
	}

	// double-click a grouped object or a placed block = tab into it (focus
	// edit): the rest of the world ghosts, Enter keeps / Esc reverts
	virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& App, const FPointerEvent& E) override
	{
		if (E.GetEffectingButton() != EKeys::LeftMouseButton) return false;
		if (!BF6Api::IsBuildOverlayActive() || !BF6Api::IsEditing()) return false;
		if (BF6Pie_Active() || GTransientMenu.IsValid() || GControls.IsValid()) return false;
		if (BF6_CursorOverSlateUI(App, E.GetScreenSpacePosition())) return false;
		if (BF6Api::IsGroupEditing() || BF6Api::IsLinkPicking() || BF6Api::IsVolumeEditing() || BF6Api::IsObbEditing()) return false;
		// bounds-based pick (hit proxies proved stale); the classification's
		// gizmo zone doesn't matter here - OutActor fills either way, and a
		// double-click on a selected group lands right where its gizmo sits
		AActor* A = BF6Api::ActorUnderCursor();
		if (!A) BF6Api::ClassifyCursorForGodotClick(A);
		return A && BF6Api::BeginGroupEditFromActor(A);
	}

	virtual bool HandleMouseButtonUpEvent(FSlateApplication& App, const FPointerEvent& E) override
	{
		if (E.GetEffectingButton() == EKeys::LeftMouseButton && bMovePend)
		{
			bMovePend = false;
			bMoveDragging = false;
			BF6Api::EndDragMove();   // plain click on a selected object: nothing changes
			return true;
		}
		if (E.GetEffectingButton() == EKeys::LeftMouseButton && bBoxDown)
		{
			bBoxDown = false;
			if (bBoxDragging)
			{
				bBoxDragging = false;
				BF6Api::EndBoxSelect(E.IsShiftDown());   // Shift adds, like Godot
				return true;
			}
			// plain click on empty ground: Godot deselects (Shift-click keeps)
			BF6Api::CancelBoxSelect();
			AActor* Under = nullptr;
			if (BF6Api::ClassifyCursorForGodotClick(Under) == 1 && Under) BF6Api::SelectClicked(Under);
			else if (!E.IsShiftDown()) BF6Api::ClearSelection();
			return true;
		}
		if (E.GetEffectingButton() == EKeys::LeftMouseButton && BF6Api::IsZoneDotDragging())
		{
			BF6Api::EndZoneDotDrag();
			return true;
		}
		if (E.GetEffectingButton() == EKeys::LeftMouseButton && BF6Api::IsScatterDotDragging())
		{
			BF6Api::EndScatterDotDrag();
			return true;
		}
		if (E.GetEffectingButton() == EKeys::MiddleMouseButton && bMMBNav)
		{
			bMMBNav = false;
			return true;
		}
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
	// Godot MMB navigation state
	bool bMMBNav = false, bMMBPan = false;
	FVector2D NavLast = FVector2D::ZeroVector;
	FVector NavPivot = FVector::ZeroVector;
	// Godot box-select state: LMB went down on empty ground
	bool bBoxDown = false, bBoxDragging = false;
	FVector2D BoxDownPos = FVector2D::ZeroVector;
	// Godot drag-move state: LMB went down on a selected movable actor
	bool bMovePend = false, bMoveDragging = false;
	FVector2D MoveDownPos = FVector2D::ZeroVector;
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
		BF6_LeakForExit(GControls);
		BF6_LeakForExit(GRoot); BF6_LeakForExit(GRootViewport);
		return;
	}
	BF6Pie_Close();
	BF6_ControlsClose();
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
