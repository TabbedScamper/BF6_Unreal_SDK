#include "BF6Experience.h"
#include "BF6BuildMode.h"
#include "BF6EditorOverlay.h"
#include "BF6Internal.h"
#include "BF6PortalProfile.h"
#include "BF6PortalSettings.h"
#include "BF6PortalWeb.h"
#include "BF6Theme.h"

#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Internationalization/Regex.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"

// ============================================================================
// See BF6Experience.h for what this is. Layout of this file:
//   1. the tool's kit, as this file needs it
//   2. detection: is the open project an experience, and which one
//   3. the screen: a rail and four sections, each one somebody else's widget
//   4. the one screen, console commands and module hooks
// ============================================================================

namespace
{
	// ---- 1. the tool's kit ---------------------------------------------------
	// Mirrors the styles in BF6PortalWeb.cpp, BF6PortalProfile.cpp and
	// BF6BuildMode.cpp. Every file in the tool carries its own copy of these
	// four lines rather than a shared style header nobody owns.

	FSlateFontInfo FontBold(int32 Size) { return FCoreStyle::GetDefaultFontStyle("Bold", Size); }
	FSlateFontInfo FontReg(int32 Size)  { return FCoreStyle::GetDefaultFontStyle("Regular", Size); }
	const FSlateBrush* InkBrush()       { static FSlateColorBrush B(BF6Theme::Ink);        return &B; }
	const FSlateBrush* PanelBrush()     { static FSlateColorBrush B(BF6Theme::Panel);      return &B; }
	const FSlateBrush* PanelLightBrush(){ static FSlateColorBrush B(BF6Theme::PanelLight); return &B; }
	const FSlateBrush* AccentBrush()    { static FSlateColorBrush B(BF6Theme::Accent);     return &B; }
	const FSlateBrush* LineBrush()      { static FSlateColorBrush B(BF6Theme::Line);       return &B; }

	const FButtonStyle& GhostStyle()
	{
		static FButtonStyle S = FButtonStyle()
			.SetNormal (FSlateRoundedBoxBrush(BF6Theme::PanelLight, 0.f, BF6Theme::Line, 1.f))
			.SetHovered(FSlateRoundedBoxBrush(FLinearColor(FColor(0x24,0x28,0x2B)), 0.f, FLinearColor::White, 1.f))
			.SetPressed(FSlateRoundedBoxBrush(FLinearColor(FColor(0x2E,0x33,0x37)), 0.f, FLinearColor::White, 1.f))
			.SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0, 1, 0, -1));
		return S;
	}

	TSharedRef<SWidget> Btn(const FString& Label, TFunction<void()> Fn, const FString& Tip = FString(),
		TFunction<bool()> Enabled = nullptr)
	{
		return SNew(SButton).ButtonStyle(&GhostStyle()).ContentPadding(FMargin(10, 6))
			.ToolTipText(FText::FromString(Tip))
			.IsEnabled_Lambda([Enabled]{ return Enabled ? Enabled() : true; })
			.OnClicked_Lambda([Fn]{ if (Fn) Fn(); return FReply::Handled(); })
			[ SNew(STextBlock).Font(FontBold(9)).ColorAndOpacity(FSlateColor(BF6Theme::Text))
				.Text(FText::FromString(Label.ToUpper())) ];
	}

	TSharedRef<SWidget> Line(const FString& Text, int32 Size, const FLinearColor& C, bool bBold = false)
	{
		return SNew(STextBlock).AutoWrapText(false).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			.Font(bBold ? FontBold(Size) : FontReg(Size)).ColorAndOpacity(FSlateColor(C))
			.Text(FText::FromString(Text));
	}

	TSharedRef<SWidget> Wrapped(const FString& Text, int32 Size, const FLinearColor& C)
	{
		return SNew(STextBlock).AutoWrapText(true)
			.Font(FontReg(Size)).ColorAndOpacity(FSlateColor(C))
			.Text(FText::FromString(Text));
	}

	// A label and its value on one row, the shape every fact on this screen has.
	TSharedRef<SWidget> Fact(const FString& Label, const FString& Value)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ SNew(SBox).WidthOverride(150.f)[ Line(Label, 9, BF6Theme::TextDim) ] ]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[ Line(Value, 10, BF6Theme::Text) ];
	}

	// A hairline, drawn rather than borrowed: SSeparator's own brush belongs to
	// the editor's style, not to this one.
	TSharedRef<SWidget> Rule()
	{
		return SNew(SBox).HeightOverride(1.f)
			[ SNew(SBorder).BorderImage(LineBrush()).Padding(FMargin(0)) ];
	}

	TSharedRef<SWidget> SectionHead(const FString& Title, const FString& Caption)
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[ Line(Title, 15, BF6Theme::Text, true) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 10)
			[ Wrapped(Caption, 9, BF6Theme::TextDim) ];
	}

	FString WhenText(int64 Unix)
	{
		if (Unix <= 0) return FString(TEXT("date unknown"));
		return FDateTime::FromUnixTimestamp(Unix).ToString(TEXT("%Y-%m-%d"));
	}

	FString FindUuid(const FString& In)
	{
		const FRegexPattern P(TEXT("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}"));
		FRegexMatcher M(P, In);
		return M.FindNext() ? M.GetCaptureGroup(0).ToLower() : FString();
	}

	// ---- 2. detection --------------------------------------------------------
	//
	// TWO WAYS TO BE AN EXPERIENCE, and the cheap one is asked first. The save
	// file itself carries the link the moment it was imported or linked by hand
	// ("portalExperience"), and that answer costs a map lookup. Failing that,
	// the profile's own model is walked: an experience whose rotation names this
	// level with this save is this project's experience even if the link was
	// never written, which is what makes a save imported by an older build of
	// the tool light the button up.
	//
	// Recompute when the open save, its link, or the profile revision changes.
	// Toolbar polling must not repeatedly walk the filesystem.

	struct FDetect
	{
		FString Level, Save;
		FString Link;
		uint32  Fp = 0;
		bool    bYes = false;
		FString Id;
		FString From;     // how it was decided, for the status line
		FString Why;      // why not, for the button's tooltip
	};
	FDetect GDetect;

	void EnsureDetect()
	{
		const FString Level = BF6Api::CurrentLevel();
		const FString Save  = BF6Api::CurrentSave();
		// This runs from toolbar attributes during Slate prepass. The card-list
		// fingerprint asks the filesystem about every experience and cannot be
		// used as a per-frame detection key. The profile's revision is memory only.
		const uint32  Fp    = BF6PortalProfile::UiFingerprint();
		const FString Link = BF6PortalWeb::ExperienceForSave(Level, Save);
		if (Level == GDetect.Level && Save == GDetect.Save && Link == GDetect.Link
			&& (Fp == GDetect.Fp || GDetect.From == TEXT("the open save's own link")))
		{
			return;
		}

		GDetect.Level = Level;
		GDetect.Save = Save;
		GDetect.Link = Link;
		GDetect.Fp = Fp;
		GDetect.bYes = false;
		GDetect.Id.Reset();
		GDetect.From = TEXT("not detected");
		GDetect.Why.Reset();

		if (Save.IsEmpty())
		{
			GDetect.Why = TEXT("No custom map is open. Open one of your experiences from the map screen and this opens on it.");
			return;
		}

		const FString FromSave = FindUuid(Link);
		if (!FromSave.IsEmpty())
		{
			GDetect.bYes = true;
			GDetect.Id = FromSave;
			GDetect.From = TEXT("the open save's own link");
			return;
		}

		for (const BF6PortalProfile::FExperienceRow& R : BF6PortalProfile::List())
		{
			for (const BF6PortalProfile::FRotationRow& Slot : BF6PortalProfile::RotationFor(R.Id))
			{
				if (Slot.SaveName == Save && (Slot.Map.IsEmpty() || Slot.Map == Level))
				{
					GDetect.bYes = true;
					GDetect.Id = R.Id;
					GDetect.From = TEXT("the profile's map rotation");
					return;
				}
			}
		}

		GDetect.Why = (BF6PortalProfile::State() == BF6PortalProfile::EState::Linked)
			? FString(TEXT("The open map is not part of one of your Portal experiences. Open one from MY EXPERIENCES on the map screen."))
			: FString(TEXT("Your Portal profile is not linked yet. Press LINK PORTAL PROFILE on the map screen and sign in on the site."));
	}

	BF6PortalProfile::FExperienceRow RowFor(const FString& Id)
	{
		if (!Id.IsEmpty())
		{
			for (const BF6PortalProfile::FExperienceRow& R : BF6PortalProfile::List())
			{
				if (R.Id == Id) return R;
			}
		}
		BF6PortalProfile::FExperienceRow Empty;
		return Empty;
	}
}

// ============================================================================
// 3. the screen
// ============================================================================

// A rail and a frame. The rail names the four sections; the frame holds all
// four at once in a switcher, so moving between them never rebuilds the
// settings column under someone's hands.
class SBF6ExperienceScreen : public SCompoundWidget
{
public:
	enum class ESection : uint8 { Identity = 0, Maps = 1, Settings = 2, Account = 3 };

	SLATE_BEGIN_ARGS(SBF6ExperienceScreen) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		SetCanTick(true);

		ChildSlot
		[
			SNew(SBorder).BorderImage(InkBrush()).Padding(FMargin(0))
			[
				SNew(SHorizontalBox)

				// ---- the rail ------------------------------------------------
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SBox).WidthOverride(232.f)
					[
						SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(14, 16))
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 2)
							[ Line(TEXT("EXPERIENCE"), 11, BF6Theme::Accent, true) ]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 14)
							[
								SNew(STextBlock).AutoWrapText(true).Font(FontReg(9))
								.ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
								.Text_Lambda([]
								{
									const FString Id = BF6Experience::CurrentId();
									if (Id.IsEmpty()) return FText::FromString(TEXT("Nothing open"));
									const FString Name = RowFor(Id).Name;
									return FText::FromString(Name.IsEmpty() ? Id.Left(8) : Name);
								})
							]
							+ SVerticalBox::Slot().AutoHeight()[ RailEntry(ESection::Identity, TEXT("Identity")) ]
							+ SVerticalBox::Slot().AutoHeight()[ RailEntry(ESection::Maps,     TEXT("Maps")) ]
							+ SVerticalBox::Slot().AutoHeight()[ RailEntry(ESection::Settings, TEXT("Settings")) ]
							+ SVerticalBox::Slot().AutoHeight()[ RailEntry(ESection::Account,  TEXT("Account")) ]
							+ SVerticalBox::Slot().FillHeight(1.f)[ SNew(SSpacer) ]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 0)
							[
								SNew(STextBlock).AutoWrapText(true).Font(FontReg(8))
								.ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
								.Text(FText::FromString(TEXT("This is the tool's own screen, not the site. The PORTAL button shows the site itself.")))
							]
						]
					]
				]

				// ---- the frame -----------------------------------------------
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(SBorder).BorderImage(InkBrush()).Padding(FMargin(22, 18))
					[
						SNew(SVerticalBox)
						// ---- BF6PortalProfile: what the site is doing ----
						// The site is off screen while the tool drives it, so
						// this line is the feedback. Collapsed when idle.
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
						[ BF6PortalProfile::MakeWorkStrip() ]
						// ---- end BF6PortalProfile ----
						+ SVerticalBox::Slot().FillHeight(1.f)
						[
						SAssignNew(Switcher, SWidgetSwitcher)

						+ SWidgetSwitcher::Slot()
						[
							SNew(SScrollBox)
							+ SScrollBox::Slot()[ SAssignNew(IdentityBody, SBox) ]
						]

						+ SWidgetSwitcher::Slot()
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight()
							[ SectionHead(TEXT("MAPS"), TEXT("The experience's map rotation. Blocks, script and settings are shared by every map in it; only the map itself differs.")) ]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
							[ BF6PortalProfile::MakeMapSwitcher() ]
							+ SVerticalBox::Slot().FillHeight(1.f)
							[
								SNew(SScrollBox)
								+ SScrollBox::Slot()[ SAssignNew(MapsBody, SBox) ]
							]
						]

						+ SWidgetSwitcher::Slot()
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight()
							[ SectionHead(TEXT("SETTINGS"),
								TEXT("Mode, teams, the modifier pages, the restriction pages and the publish steps. Every change is made through the site's own controls, so keep the experience open on the site: the PORTAL button, or OPEN ON THE SITE under Identity.")) ]
							+ SVerticalBox::Slot().FillHeight(1.f)
							[ BF6PortalSettings::MakeColumn() ]
						]

						+ SWidgetSwitcher::Slot()
						[
							SNew(SScrollBox)
							+ SScrollBox::Slot()[ SAssignNew(AccountBody, SBox) ]
						]
						]
					]
				]
			]
		];

		Rebuild();
	}

	virtual void Tick(const FGeometry& G, const double T, const float D) override
	{
		SCompoundWidget::Tick(G, T, D);

		// The rail can lose the section the user is standing on: the map screen
		// closes the save, the profile is unlinked. Fall back rather than show a
		// frame nothing points at.
		if (!Available(Selected)) Select(ESection::Identity);

		const FString Id = BF6Experience::CurrentId();
		const uint32  Fp = BF6PortalProfile::ListFingerprint();
		const FString Save = BF6Api::CurrentSave();
		if (Id != ShownId || Fp != ShownFp || Save != ShownSave)
		{
			ShownId = Id;
			ShownFp = Fp;
			ShownSave = Save;
			Rebuild();
		}
	}

private:
	ESection Selected = ESection::Identity;
	TSharedPtr<SWidgetSwitcher> Switcher;
	TSharedPtr<SBox> IdentityBody, MapsBody, AccountBody;
	FString ShownId, ShownSave;
	uint32  ShownFp = 0;
	// Read once per rebuild, not once per rail entry per frame: walking the
	// rotation is a parse, and four visibility lambdas ask this every tick.
	int32   SlotCount = 0;

	bool Available(ESection S) const
	{
		switch (S)
		{
		case ESection::Maps:     return SlotCount > 0;
		case ESection::Settings: return BF6PortalProfile::State() == BF6PortalProfile::EState::Linked;
		default:                 return true;
		}
	}

	void Select(ESection S)
	{
		Selected = S;
		if (Switcher.IsValid()) Switcher->SetActiveWidgetIndex((int32)S);
	}

	TSharedRef<SWidget> RailEntry(ESection S, const FString& Label)
	{
		return SNew(SBox)
			.Padding(FMargin(0, 0, 0, 2))
			.Visibility_Lambda([this, S]{ return Available(S) ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				SNew(SButton).ButtonStyle(&FCoreStyle::Get(), "NoBorder").ContentPadding(0)
				.OnClicked_Lambda([this, S]{ Select(S); return FReply::Handled(); })
				[
					SNew(SBorder)
					.BorderImage_Lambda([this, S]{ return Selected == S ? AccentBrush() : PanelLightBrush(); })
					.Padding(FMargin(12, 9))
					[
						SNew(STextBlock).Font(FontBold(10))
						.ColorAndOpacity_Lambda([this, S]{ return FSlateColor(Selected == S ? BF6Theme::Ink : BF6Theme::Text); })
						.Text(FText::FromString(Label.ToUpper()))
					]
				]
			];
	}

	// ---- the sections -------------------------------------------------------

	void Rebuild()
	{
		const FString Id = BF6Experience::CurrentId();
		SlotCount = Id.IsEmpty() ? 0 : BF6PortalProfile::RotationFor(Id).Num();
		if (IdentityBody.IsValid()) IdentityBody->SetContent(MakeIdentity());
		if (MapsBody.IsValid())     MapsBody->SetContent(MakeMaps());
		if (AccountBody.IsValid())  AccountBody->SetContent(MakeAccount());
		if (!Available(Selected))   Select(ESection::Identity);
	}

	// Nothing is open, or nothing is linked: say which, and say the one thing
	// that fixes it. This is the whole screen in that case, not a corner of it.
	TSharedRef<SWidget> MakeEmpty() const
	{
		const bool bLinked = BF6PortalProfile::State() == BF6PortalProfile::EState::Linked;
		const FString Why = BF6Experience::WhyUnavailable();
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[ SectionHead(TEXT("NO EXPERIENCE OPEN"),
				TEXT("This screen shows the Portal experience the open project belongs to: its identity, its map rotation, its settings and the account it lives on.")) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 14)
			[ Wrapped(Why.IsEmpty() ? FString(TEXT("Nothing is open.")) : Why, 11, BF6Theme::Text) ]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SWrapBox).UseAllottedSize(true)
				+ SWrapBox::Slot().Padding(0, 0, 6, 6)
				[ Btn(TEXT("Link Portal profile"), []{ BF6PortalWeb::Open(); BF6PortalProfile::StartLink(); },
					TEXT("Opens the Portal site in the tool so you can sign in there. The tool never sees your password."),
					[bLinked]{ return !bLinked; }) ]
				+ SWrapBox::Slot().Padding(0, 0, 6, 6)
				[ Btn(TEXT("Show the Portal site"), []{ BF6PortalWeb::Open(); },
					TEXT("Show portal.battlefield.com in the editor. The site, and nothing else.")) ]
			];
	}

	TSharedRef<SWidget> MakeIdentity() const
	{
		const FString Id = BF6Experience::CurrentId();
		if (Id.IsEmpty()) return MakeEmpty();

		const BF6PortalProfile::FExperienceRow R = RowFor(Id);
		const int32 Slots = BF6PortalProfile::RotationFor(Id).Num();
		const bool  bImported = BF6PortalProfile::IsImported(Id);
		const FString Name = R.Name.IsEmpty() ? FString(TEXT("Untitled experience")) : R.Name;

		TSharedRef<SVerticalBox> Facts = SNew(SVerticalBox);
		Facts->AddSlot().AutoHeight().Padding(0, 0, 0, 4)[ Fact(TEXT("Short id"), Id.Left(8)) ];
		Facts->AddSlot().AutoHeight().Padding(0, 0, 0, 4)[ Fact(TEXT("Last changed"), WhenText(R.UpdatedUnix)) ];
		Facts->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
			[ Fact(TEXT("Maps in rotation"), Slots > 0 ? FString::FromInt(Slots)
				: FString(R.bNoMapData ? TEXT("none attached yet") : TEXT("not read yet"))) ];
		Facts->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
			[ Fact(TEXT("In the tool"), bImported ? TEXT("imported") : TEXT("not imported yet")) ];
		Facts->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
			[ Fact(TEXT("Open map"), BF6Api::CurrentSave().IsEmpty() ? FString(TEXT("none"))
				: FString::Printf(TEXT("%s  (%s)"), *BF6Api::CurrentSave(), *BF6Api::DisplayName(BF6Api::CurrentLevel()))) ];
		Facts->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
			[ Fact(TEXT("Publish state"), TEXT("the site does not report it to the tool")) ];

		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[ SectionHead(TEXT("IDENTITY"), TEXT("The experience the open project belongs to, as the site described it.")) ]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 14)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
				[
					SNew(SBox).WidthOverride(176.f).HeightOverride(124.f)
					[
						SNew(SBorder).BorderImage(PanelLightBrush()).Padding(FMargin(1))
						[
							SNew(SImage).Image_Lambda([Id]
							{
								const FSlateBrush* B = BF6PortalProfile::ThumbnailFor(Id);
								return B ? B : PanelBrush();
							})
						]
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[ Line(Name, 20, BF6Theme::Text, true) ]
					+ SVerticalBox::Slot().AutoHeight()[ Facts ]
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SWrapBox).UseAllottedSize(true)
				+ SWrapBox::Slot().Padding(0, 0, 6, 6)
				[ Btn(TEXT("Import into the tool"), [Id]{ BF6PortalProfile::ImportOne(Id); },
					TEXT("Download this experience's maps and open them here as custom maps. Already imported maps are left alone."),
					[]{ return !BF6PortalProfile::IsBusy(); }) ]
				+ SWrapBox::Slot().Padding(0, 0, 6, 6)
				[ Btn(TEXT("Open on the site"), [Id]{ BF6PortalProfile::OpenOnSite(Id); },
					TEXT("Point the Portal panel at this experience. It is the site's own page, in the tool.")) ]
				+ SWrapBox::Slot().Padding(0, 0, 6, 6)
				[ Btn(TEXT("Show the Portal site"), []{ BF6PortalWeb::Open(); },
					TEXT("Show the Portal site in the editor, in the placement you last used.")) ]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
			[ Wrapped(TEXT("Publishing is the site's own three steps, under SETTINGS. The tool never presses the publish button for you."), 9, BF6Theme::TextDim) ]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[ Rule() ]

			// SCREENSHOT MAP and UPLOAD IMAGE come with the profile's own
			// thumbnail panel, requirements and status line included. Nothing
			// about the thumbnail is re-implemented here.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
			[ BF6PortalProfile::MakeThumbnailPanel() ];
	}

	TSharedRef<SWidget> MakeMaps() const
	{
		const FString Id = BF6Experience::CurrentId();
		const TArray<BF6PortalProfile::FRotationRow> Rows = BF6PortalProfile::RotationFor(Id);
		if (Rows.Num() == 0)
		{
			return Wrapped(TEXT("This experience has no map rotation the tool can read yet. Open it on the site once and it appears here."), 10, BF6Theme::TextDim);
		}

		const FString OpenSave  = BF6Api::CurrentSave();
		const FString OpenLevel = BF6Api::CurrentLevel();

		TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
		for (const BF6PortalProfile::FRotationRow& Row : Rows)
		{
			const bool bOpen = !OpenSave.IsEmpty() && Row.SaveName == OpenSave && Row.Map == OpenLevel;
			const FString State = Row.SaveName.IsEmpty()
				? FString(Row.bHasSpatial ? TEXT("not imported yet") : TEXT("no map data on the site yet"))
				: FString::Printf(TEXT("imported as %s"), *Row.SaveName);
			const int32   Idx = Row.MapIdx;
			const FString Map = Row.Map;

			List->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(SBorder)
				.BorderImage(bOpen ? PanelLightBrush() : PanelBrush())
				.Padding(FMargin(12, 10))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
					[ SNew(SBox).WidthOverride(24.f)[ Line(FString::FromInt(Idx + 1), 12, BF6Theme::Accent, true) ] ]
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[ Line(BF6Api::DisplayName(Map), 12, BF6Theme::Text, true) ]
						+ SVerticalBox::Slot().AutoHeight()
						[ Line(FString::Printf(TEXT("%s  -  %s"), *Map, *State), 9, BF6Theme::TextDim) ]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SBox)
						.Visibility(bOpen ? EVisibility::Collapsed : EVisibility::Visible)
						[ Btn(TEXT("Open this map"), [Id, Idx]{ BF6PortalProfile::SwitchToMap(Id, Idx); },
							TEXT("Save the open map and open this slot of the rotation instead. It is imported first if it has never been here."),
							[]{ return !BF6PortalProfile::IsBusy(); }) ]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SBox)
						.Visibility(bOpen ? EVisibility::Visible : EVisibility::Collapsed)
						[ Line(TEXT("OPEN NOW"), 9, BF6Theme::Accent, true) ]
					]
				]
			];
		}
		return List;
	}

	TSharedRef<SWidget> MakeAccount() const
	{
		TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);
		Body->AddSlot().AutoHeight()
			[ SectionHead(TEXT("ACCOUNT"), TEXT("The Portal account this tool is reading, and the state of its session. You sign in on the site itself; the tool never reads, stores or types your password.")) ];

		Body->AddSlot().AutoHeight().Padding(0, 0, 0, 4)[ Fact(TEXT("State"), BF6PortalProfile::StateLabel()) ];
		Body->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
			[ Fact(TEXT("Account"), BF6PortalProfile::AccountName().IsEmpty()
				? FString(TEXT("not reported by the page")) : BF6PortalProfile::AccountName()) ];
		Body->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
			[ Fact(TEXT("Experiences"), BF6PortalProfile::HasExperiences()
				? FString::FromInt(BF6PortalProfile::List().Num()) : FString(TEXT("none read yet"))) ];
		Body->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
			[ Fact(TEXT("Session"), BF6PortalProfile::IsSessionLost()
				? FString(TEXT("lost")) : FString(TEXT("live"))) ];
		Body->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
			[ Fact(TEXT("Keep alive"), BF6PortalProfile::KeepAliveEnabled() ? TEXT("on") : TEXT("off")) ];

		const FString Page = BF6PortalProfile::PageStatus();
		if (!Page.IsEmpty()) Body->AddSlot().AutoHeight().Padding(0, 0, 0, 4)[ Fact(TEXT("Page check"), Page) ];
		const FString Work = BF6PortalProfile::WorkStatus();
		if (!Work.IsEmpty()) Body->AddSlot().AutoHeight().Padding(0, 0, 0, 4)[ Fact(TEXT("Working on"), Work) ];
		const FString Loss = BF6PortalProfile::LastLossReason();
		if (!Loss.IsEmpty()) Body->AddSlot().AutoHeight().Padding(0, 0, 0, 4)[ Fact(TEXT("Last loss"), Loss) ];
		const FString Rec = BF6PortalProfile::LastRecoveryResult();
		if (!Rec.IsEmpty())  Body->AddSlot().AutoHeight().Padding(0, 0, 0, 4)[ Fact(TEXT("Last recovery"), Rec) ];

		const FString Banner = BF6PortalProfile::Banner();
		if (!Banner.IsEmpty())
		{
			Body->AddSlot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(12, 9))
				[ Wrapped(Banner, 10, BF6Theme::Accent) ]
			];
		}

		Body->AddSlot().AutoHeight().Padding(0, 14, 0, 0)
		[
			SNew(SWrapBox).UseAllottedSize(true)
			+ SWrapBox::Slot().Padding(0, 0, 6, 6)
			[ Btn(TEXT("Sign in again"), []{ BF6PortalWeb::Open(); BF6PortalProfile::StartLink(); },
				TEXT("Open the Portal site at its login page. Signing in happens there, never here.")) ]
			+ SWrapBox::Slot().Padding(0, 0, 6, 6)
			[ Btn(TEXT("Unlink"), []{ BF6PortalProfile::Unlink(); },
				TEXT("Forget the account and clear the site's saved session. Nothing on your maps or on the site changes.")) ]
			+ SWrapBox::Slot().Padding(0, 0, 6, 6)
			[ Btn(TEXT("Keep alive on or off"), []{ BF6PortalProfile::SetKeepAliveEnabled(!BF6PortalProfile::KeepAliveEnabled()); },
				TEXT("Whether the tool quietly keeps the site's session alive while you work. Remembered per project.")) ]
			+ SWrapBox::Slot().Padding(0, 0, 6, 6)
			[ Btn(TEXT("Log session to output"), []{ BF6PortalProfile::LogSession(); },
				TEXT("Write the session's full state to the Output Log under LogBF6Portal.")) ]
		];

		return Body;
	}
};

// ============================================================================
// 4. the one screen, console commands and module hooks
// ============================================================================

namespace
{
	TSharedPtr<SWidget>     GScreen;
	TArray<IConsoleObject*> GCmds;
}

TSharedRef<SWidget> BF6Experience::Widget()
{
	if (!GScreen.IsValid()) GScreen = SNew(SBF6ExperienceScreen);
	return GScreen.ToSharedRef();
}

void BF6Experience::ReleaseWidget()
{
	// Nothing to hand it back to: this screen has no dock tab of its own. It is
	// kept built, so the settings section and the rail are exactly as they were
	// the next time the button is pressed.
}

void BF6Experience::Open()
{
	BF6EditorOverlay::Show(BF6EditorOverlay::EEditor::Experience);
}

bool BF6Experience::IsExperienceOpen()
{
	EnsureDetect();
	return GDetect.bYes;
}

FString BF6Experience::CurrentId()
{
	EnsureDetect();
	return GDetect.Id;
}

FString BF6Experience::WhyUnavailable()
{
	EnsureDetect();
	return GDetect.bYes ? FString() : GDetect.Why;
}

FString BF6Experience::Status()
{
	EnsureDetect();
	const FString Name = RowFor(GDetect.Id).Name;
	return FString::Printf(TEXT(
		"BF6 experience\n"
		"  open project   : %s\n"
		"  experience     : %s\n"
		"  detected from  : %s\n"
		"  profile state  : %s%s\n"
		"  screen         : %s\n"
		"  why not        : %s"),
		BF6Api::CurrentSave().IsEmpty() ? TEXT("nothing open")
			: *FString::Printf(TEXT("%s / %s"), *BF6Api::CurrentLevel(), *BF6Api::CurrentSave()),
		GDetect.Id.IsEmpty() ? TEXT("none")
			: *FString::Printf(TEXT("%s  %s"), *GDetect.Id, *Name),
		*GDetect.From,
		*BF6PortalProfile::StateLabel(),
		BF6PortalProfile::AccountName().IsEmpty() ? TEXT("")
			: *FString::Printf(TEXT(" (%s)"), *BF6PortalProfile::AccountName()),
		BF6EditorOverlay::IsShowing(BF6EditorOverlay::EEditor::Experience)
			? TEXT("covering the viewport") : TEXT("not shown"),
		GDetect.Why.IsEmpty() ? TEXT("it is available") : *GDetect.Why);
}

void BF6Experience::Register()
{
	IConsoleManager& CM = IConsoleManager::Get();
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Experience.Open"),
		TEXT("Cover the viewport with the experience screen: identity, maps, settings and the account."),
		FConsoleCommandDelegate::CreateLambda([]{ BF6Experience::Open(); })));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Experience.Status"),
		TEXT("Which Portal experience the open project belongs to, how the tool decided, and whether the screen is up."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			UE_LOG(LogBF6, Display, TEXT("%s"), *BF6Experience::Status());
		})));
}

void BF6Experience::Unregister()
{
	for (IConsoleObject* C : GCmds) if (C) IConsoleManager::Get().UnregisterConsoleObject(C);
	GCmds.Reset();
	GScreen.Reset();
}
