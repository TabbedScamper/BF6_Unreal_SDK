// The scene tree, wearing the SDK's clothes.
//
// Creators come to this tool from Godot, where the tree IS the map: nodes
// parented under other nodes, each with the icon of what it is. Unreal's own
// outliner already does the editing half of that - drag one actor onto another
// to reparent, F2 to rename, delete a subtree - now that the import builds real
// attachment rather than folders. What it does not do is LOOK like the SDK, and
// it shows the whole world rather than the creator's own objects.
//
// So this hosts an outliner of our own making: the engine's widget, built
// through FSceneOutlinerModule with our filter and our columns, in our tab. The
// behaviour is Unreal's, proven and already understood by the engine; the
// symbols, the colours and the contents are the SDK's.

#include "BF6BuildMode.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Framework/Docking/TabManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Views/SExpanderArrow.h"
#include "Widgets/Views/STableRow.h"

#include "LevelEditor.h"
#include "ActorTreeItem.h"
#include "ISceneOutliner.h"
#include "ISceneOutlinerColumn.h"
#include "SceneOutlinerFilters.h"
#include "SSceneOutliner.h"
#include "SceneOutlinerModule.h"
#include "SceneOutlinerPublicTypes.h"

#define LOCTEXT_NAMESPACE "BF6Outliner"

namespace
{
	const FName kOutlinerTab("BF6Outliner");
	TWeakPtr<ISceneOutliner> GLiveOutliner;   // the one on screen, for refreshes
	const FName kIconColumn("BF6NodeIcon");

	// ---- the icon set ----
	// Godot's own node icons, shipped in the plugin (see the notice beside
	// them), plus the SDK's volume icons for the two types it draws itself.
	// Slate renders SVG directly, so these are the same files the SDK uses.
	TSharedPtr<FSlateStyleSet> GStyle;

	void EnsureStyle()
	{
		if (GStyle.IsValid()) return;
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("BF6UnrealSDK"));
		if (!Plugin.IsValid()) return;

		GStyle = MakeShared<FSlateStyleSet>(TEXT("BF6OutlinerStyle"));
		GStyle->SetContentRoot(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("GodotIcons")));

		const FVector2D Icon(16.f, 16.f);
		auto Add = [&](const TCHAR* Name)
		{
			GStyle->Set(*(FString(TEXT("BF6.Node.")) + Name),
				new FSlateVectorImageBrush(GStyle->RootToContentDir(Name, TEXT(".svg")), Icon));
		};
		for (const TCHAR* N : { TEXT("Node"), TEXT("Node3D"), TEXT("MeshInstance3D"), TEXT("Camera3D"),
			TEXT("Marker3D"), TEXT("Area3D"), TEXT("Path3D"), TEXT("CollisionShape3D"),
			TEXT("CollisionPolygon3D"), TEXT("PackedScene"), TEXT("Skeleton3D"),
			TEXT("PolygonVolume"), TEXT("OBBVolume"),
			TEXT("NodeWarning"), TEXT("StatusError") })
			Add(N);

		FSlateStyleRegistry::RegisterSlateStyle(*GStyle);
	}

	// Which symbol an object wears. The SDK picks by Godot node class; we pick
	// by what the object IS, which comes to the same thing for everything a
	// creator actually places.
	FName IconFor(const AActor* A)
	{
		auto Tagged = [A](const TCHAR* Prefix) -> FString
		{
			const FString P(Prefix);
			for (const FName& T : A->Tags)
			{
				const FString S = T.ToString();
				if (S.StartsWith(P)) return S.RightChop(P.Len());
			}
			return FString();
		};

		if (A->Tags.Contains(FName("BF6Group"))) return FName("BF6.Node.Node3D");
		if (A->Tags.Contains(FName("BF6Context")))
			return A->GetActorLabel() == TEXT("Static")
				? FName("BF6.Node.Node3D") : FName("BF6.Node.MeshInstance3D");

		FString Ty = Tagged(TEXT("label:"));
		if (Ty.IsEmpty()) Ty = Tagged(TEXT("type:"));

		if (Ty == TEXT("PolygonVolume") || Ty == TEXT("CombatArea")) return FName("BF6.Node.PolygonVolume");
		if (Ty == TEXT("OBBVolume")) return FName("BF6.Node.OBBVolume");
		if (Ty == TEXT("AreaTrigger") || Ty == TEXT("RingOfFire")) return FName("BF6.Node.Area3D");
		if (Ty.Contains(TEXT("Camera"))) return FName("BF6.Node.Camera3D");
		if (Ty == TEXT("WaypointPath") || Ty.StartsWith(TEXT("AI_"))) return FName("BF6.Node.Path3D");
		if (Ty.Contains(TEXT("Spawn")) || Ty == TEXT("CapturePoint") || Ty == TEXT("MCOM")
			|| Ty == TEXT("Sector") || Ty == TEXT("WorldIcon") || Ty == TEXT("InteractPoint"))
			return FName("BF6.Node.Marker3D");
		if (!Tagged(TEXT("blk:")).IsEmpty()) return FName("BF6.Node.PackedScene");
		if (!Tagged(TEXT("mesh:")).IsEmpty()) return FName("BF6.Node.MeshInstance3D");
		return FName("BF6.Node.Node");
	}

	// Godot names the node's class under the cursor; ours names the Portal type,
	// which is the thing a creator is actually looking for.
	FString DescribeFor(const AActor* A)
	{
		auto Tagged = [A](const TCHAR* Prefix) -> FString
		{
			const FString P(Prefix);
			for (const FName& T : A->Tags)
			{
				const FString S = T.ToString();
				if (S.StartsWith(P)) return S.RightChop(P.Len());
			}
			return FString();
		};
		if (A->Tags.Contains(FName("BF6Group"))) return TEXT("Node3D - a parent you built, holds no object of its own");
		if (A->Tags.Contains(FName("BF6Context")))
			return TEXT("the map itself - locked, not exported, hide it with the eye to see inside");
		FString Ty = Tagged(TEXT("label:"));
		if (Ty.IsEmpty()) Ty = Tagged(TEXT("type:"));
		const FString Id = Tagged(TEXT("p:ObjId="));
		if (Ty.IsEmpty()) Ty = TEXT("Object");
		return Id.IsEmpty() ? Ty : FString::Printf(TEXT("%s  -  ObjId %s"), *Ty, *Id);
	}

	// The badge an item is wearing, if any: an error the catalogue is certain
	// about, or a warning worth checking in game.
	bool MarkFor(const FSceneOutlinerTreeItemPtr& Item, uint8& OutSev, FString& OutMsg)
	{
		if (!Item.IsValid()) return false;
		const FActorTreeItem* Actor = Item->CastTo<FActorTreeItem>();
		AActor* A = Actor ? Actor->Actor.Get() : nullptr;
		return A && BF6Api::LintMarkFor(A, OutSev, OutMsg);
	}

	const FSlateBrush* MarkBrushFor(const FSceneOutlinerTreeItemPtr& Item)
	{
		uint8 Sev = 2; FString Msg;
		if (!MarkFor(Item, Sev, Msg) || !GStyle.IsValid()) return nullptr;
		return GStyle->GetBrush(Sev == 0 ? FName("BF6.Node.StatusError") : FName("BF6.Node.NodeWarning"));
	}

	FString MarkTextFor(const FSceneOutlinerTreeItemPtr& Item)
	{
		uint8 Sev = 2; FString Msg;
		return MarkFor(Item, Sev, Msg) ? Msg : FString();
	}

	// ---- the node row ----
	// Godot draws a row as indent guides, then the type symbol, then the name,
	// with relationship lines running from a parent down to each of its children.
	// Unreal's own label column has no wires and a tighter indent, so the row is
	// assembled here instead - the expander does the lines, and the name widget
	// still comes from the tree item, which keeps rename, search highlighting and
	// the engine's own colours working.
	const float kIndent = 16.f;   // Godot's Tree item_margin

	class FBF6IconColumn : public ISceneOutlinerColumn
	{
	public:
		explicit FBF6IconColumn(ISceneOutliner& InOutliner) : Outliner(InOutliner) {}

		virtual FName GetColumnID() override { return kIconColumn; }

		virtual SHeaderRow::FColumn::FArguments ConstructHeaderRowColumn() override
		{
			return SHeaderRow::Column(kIconColumn)
				.FillWidth(1.f)
				.DefaultLabel(FText::GetEmpty());
		}

		// The search box matches against whatever the COLUMNS hand it, and ours
		// replaced the built-in label column - so nothing was contributing and the
		// box found nothing at all. Everything a creator might type goes in here:
		// the name, the name with its separators stripped (so "spawnpoint" finds
		// SpawnPoint_1_1), what the object IS, its ObjId, its model, and the words
		// for the shapes that have no name of their own.
		virtual void PopulateSearchStrings(const ISceneOutlinerTreeItem& Item, TArray<FString>& Out) const override
		{
			const FString Display = Item.GetDisplayString();
			Out.Add(Display);
			FString Squashed = Display;
			Squashed.ReplaceInline(TEXT("_"), TEXT(""));
			Squashed.ReplaceInline(TEXT("-"), TEXT(""));
			Squashed.ReplaceInline(TEXT(" "), TEXT(""));
			if (Squashed != Display) Out.Add(Squashed);

			const FActorTreeItem* Actor = Item.CastTo<FActorTreeItem>();
			const AActor* A = Actor ? Actor->Actor.Get() : nullptr;
			if (!A) return;

			auto Tagged = [A](const TCHAR* Prefix) -> FString
			{
				const FString P(Prefix);
				for (const FName& T : A->Tags)
				{
					const FString S = T.ToString();
					if (S.StartsWith(P)) return S.RightChop(P.Len());
				}
				return FString();
			};
			auto AddIf = [&Out](const FString& S){ if (!S.IsEmpty()) Out.Add(S); };

			AddIf(Tagged(TEXT("label:")));
			AddIf(Tagged(TEXT("type:")));
			AddIf(Tagged(TEXT("mesh:")));
			AddIf(Tagged(TEXT("blk:")));
			const FString Id = Tagged(TEXT("p:ObjId="));
			if (!Id.IsEmpty()) { Out.Add(Id); Out.Add(FString(TEXT("objid ")) + Id); }

			// the words people actually type for things named something else
			if (A->Tags.Contains(FName("BF6Group")))   { Out.Add(TEXT("node")); Out.Add(TEXT("node3d")); Out.Add(TEXT("parent")); }
			if (A->Tags.Contains(FName("BF6Context"))) { Out.Add(TEXT("map")); Out.Add(TEXT("terrain")); Out.Add(TEXT("static")); }
			const FString Ty = Tagged(TEXT("label:")).IsEmpty() ? Tagged(TEXT("type:")) : Tagged(TEXT("label:"));
			if (Ty.Contains(TEXT("Volume")) || Ty == TEXT("CombatArea")) { Out.Add(TEXT("volume")); Out.Add(TEXT("zone")); }
			if (Ty.Contains(TEXT("Camera"))) Out.Add(TEXT("camera"));
			if (Ty.Contains(TEXT("Spawn")))  Out.Add(TEXT("spawn"));
		}

		// Cheap: returns immediately unless the cached result has aged out.
		virtual void Tick(double, float) override { BF6Api::RefreshLintIfStale(4.0); }

		virtual bool SupportsSorting() const override { return true; }

		// Godot lists a node's children in the order the scene file declares them.
		// Sorting by name here would reorder every tree we import, so the authored
		// position recorded at import wins, and anything without one (an object
		// placed since) falls in behind, by name.
		virtual void SortItems(TArray<FSceneOutlinerTreeItemPtr>& Items, const EColumnSortMode::Type Mode) const override
		{
			auto OrderOf = [](const FSceneOutlinerTreeItemPtr& I) -> int32
			{
				if (const FActorTreeItem* Actor = I->CastTo<FActorTreeItem>())
					if (const AActor* A = Actor->Actor.Get())
						for (const FName& T : A->Tags)
						{
							const FString S = T.ToString();
							if (S.StartsWith(TEXT("gord:"))) return FCString::Atoi(*S.RightChop(5));
						}
				return MAX_int32;
			};
			Items.Sort([&OrderOf, Mode](const FSceneOutlinerTreeItemPtr& A, const FSceneOutlinerTreeItemPtr& B)
			{
				const int32 OA = OrderOf(A), OB = OrderOf(B);
				const bool bLess = (OA != OB) ? (OA < OB) : (A->GetDisplayString() < B->GetDisplayString());
				return Mode == EColumnSortMode::Descending ? !bLess : bLess;
			});
		}

	private:
		ISceneOutliner& Outliner;

	public:
		virtual const TSharedRef<SWidget> ConstructRowWidget(FSceneOutlinerTreeItemRef Item,
			const STableRow<FSceneOutlinerTreeItemPtr>& Row) override
		{
			const FSlateBrush* Brush = nullptr;
			FText Tip;
			if (const FActorTreeItem* Actor = Item->CastTo<FActorTreeItem>())
				if (const AActor* A = Actor->Actor.Get())
				{
					if (GStyle.IsValid()) Brush = GStyle->GetBrush(IconFor(A));
					Tip = FText::FromString(DescribeFor(A));
				}

			// AsShared() hands back the SWidget side of the row; ITableRow is the
			// other base, so the cast goes through the row's own type first.
			using FRowType = STableRow<FSceneOutlinerTreeItemPtr>;
			TSharedPtr<ITableRow> RowRef = StaticCastSharedRef<FRowType>(
				const_cast<FRowType&>(Row).AsShared());

			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Fill)
				[
					SNew(SExpanderArrow, RowRef)
						.IndentAmount(kIndent)
						.ShouldDrawWires(true)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 5.f, 0.f)
				[
					SNew(SBox).WidthOverride(16.f).HeightOverride(16.f)
					[ SNew(SImage).Image(Brush).ToolTipText(Tip) ]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[ Item->GenerateLabelWidget(Outliner, Row) ]
				// Godot flags a node that has something wrong with it right on the
				// row, and so does this: the Validate result, cached and refreshed
				// on a timer, without having to open a panel to find out.
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.f, 0.f, 4.f, 0.f)
				[
					SNew(SBox).WidthOverride(16.f).HeightOverride(16.f)
					.Visibility_Lambda([Item]{ return MarkBrushFor(Item) ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						SNew(SImage)
							.Image_Lambda([Item]{ return MarkBrushFor(Item); })
							.ToolTipText_Lambda([Item]{ return FText::FromString(MarkTextFor(Item)); })
					]
				];
		}
	};

	// Only what the creator can act on. The map's scenery, our drag handles and
	// whatever else lives in the level are noise here - the SDK's tree shows the
	// experience, not the world it sits in.
	bool IsOurs(const AActor* A)
	{
		if (!A) return false;
		if (A->Tags.Contains(FName("BF6Handle"))) return false;   // our own drag gizmos
		return A->Tags.Contains(FName("BF6Placed"))
			|| A->Tags.Contains(FName("BF6Base"))
			|| A->Tags.Contains(FName("BF6Group"))
			// the map's terrain and low-poly assets, under 'Static' as in the SDK.
			// They are locked and unselectable in the viewport, but listing them
			// gives the eye that switches one off to see inside a building.
			|| A->Tags.Contains(FName("BF6Context"));
	}

	TSharedRef<SWidget> BuildOutliner()
	{
		EnsureStyle();
		if (!GEditor) return SNullWidget::NullWidget;
		UWorld* W = GEditor->GetEditorWorldContext().World();

		FSceneOutlinerModule& Mod = FModuleManager::LoadModuleChecked<FSceneOutlinerModule>(TEXT("SceneOutliner"));

		FSceneOutlinerInitializationOptions Opts;
		// EVERYTHING this tool spawns is RF_Transient - the level is never saved -
		// and FActorMode::IsActorDisplayable drops transient actors unless the
		// outliner is told otherwise. Objects you place slipped through only
		// because their row is made from the actor-added event before the flag is
		// set; the map's scenery goes up during the map-open repopulate, is
		// re-tested, and vanished. This is the switch the engine provides for it.
		Opts.bShowTransient = true;
		Opts.bShowHeaderRow = false;         // Godot's tree carries no column headers
		Opts.bShowSearchBox = true;
		Opts.bShowCreateNewFolder = false;   // parents are nodes here, not folders
		Opts.bFocusSearchBoxWhenOpened = false;
		Opts.OutlinerIdentifier = kOutlinerTab;

		Opts.Filters = MakeShared<FSceneOutlinerFilters>();
		Opts.Filters->AddFilterPredicate<FActorTreeItem>(
			FActorTreeItem::FFilterPredicate::CreateStatic(&IsOurs));

		// symbol, name, then the eye out on the right - the SDK's order, which is
		// the opposite of Unreal's habit of leading with visibility
		Opts.ColumnMap.Add(FSceneOutlinerBuiltInColumnTypes::Gutter(),
			FSceneOutlinerColumnInfo(ESceneOutlinerColumnVisibility::Visible, 20, FCreateSceneOutlinerColumn(), false));
		Opts.ColumnMap.Add(kIconColumn,
			FSceneOutlinerColumnInfo(ESceneOutlinerColumnVisibility::Visible, 10,
				FCreateSceneOutlinerColumn::CreateLambda([](ISceneOutliner& O) -> TSharedRef<ISceneOutlinerColumn>
				{
					return MakeShared<FBF6IconColumn>(O);
				}), false));

		TSharedRef<ISceneOutliner> Outliner = Mod.CreateActorBrowser(Opts, W);

		// bShowTransient in the options above does not survive: FActorBrowsingMode
		// overwrites it from the editor's own "Hide Temporary Actors" setting the
		// moment it is constructed (ActorBrowsingMode.cpp - SetShowTransient at
		// construction). Everything this tool spawns is transient, so it has to be
		// set back afterwards, on the widget, or the map's scenery is dropped every
		// time the tree repopulates.
		StaticCastSharedRef<SSceneOutliner>(Outliner)->SetShowTransient(true);
		GLiveOutliner = Outliner;
		return Outliner;
	}
}

namespace BF6Api
{
	// This IS the outliner, not a second one beside it. The level editor's own
	// Outliner tab is handed over, so the panel already docked there - and the
	// Window menu entry, and any layout a creator saves - all open ours.
	void RegisterOutlinerTab()
	{
		FLevelEditorModule& LE = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
		TSharedPtr<FTabManager> TM = LE.GetLevelEditorTabManager();
		if (!TM.IsValid()) return;   // too early: the caller tries again once the level editor is up

		const FName OutlinerId = LevelEditorTabIds::LevelEditorSceneOutliner;
		if (TSharedPtr<SDockTab> Live = TM->FindExistingLiveTab(FTabId(OutlinerId)))
			Live->RequestCloseTab();
		TM->UnregisterTabSpawner(OutlinerId);
		TM->RegisterTabSpawner(OutlinerId, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
			{
				return SNew(SDockTab)
					.Label(LOCTEXT("BF6OutlinerTitle", "Scene"))
					[
						SNew(SVerticalBox)
						// Godot puts node creation at the top of the Scene dock, and the
						// tree is no use for building without it: a node to hang things
						// off, or a parent wrapped around what is already selected.
						+ SVerticalBox::Slot().AutoHeight().Padding(4.f, 4.f, 4.f, 2.f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
							[
								SNew(SButton)
									.IsEnabled_Lambda([]{ return BF6Api::IsEditing(); })
									.ToolTipText_Lambda([]{ return FText::FromString(BF6Api::IsEditing()
										? TEXT("Add an empty node. It lands beside the selection, so it sits with its siblings.")
										: TEXT("The base map is read only - create a custom level or resume one first.")); })
									.OnClicked_Lambda([]{ BF6Api::AddTreeNode(); return FReply::Handled(); })
									[ SNew(STextBlock).Text(FText::FromString(TEXT("+ Node"))) ]
							]
							+ SHorizontalBox::Slot().AutoWidth()
							[
								SNew(SButton)
									.IsEnabled_Lambda([]{ return BF6Api::IsEditing(); })
									.ToolTipText_Lambda([]{ return FText::FromString(BF6Api::IsEditing()
										? TEXT("Wrap the selected objects in a new parent node, so they move as one.")
										: TEXT("The base map is read only - create a custom level or resume one first.")); })
									.OnClicked_Lambda([]{ BF6Api::GroupSelectionUnderNode(); return FReply::Handled(); })
									[ SNew(STextBlock).Text(FText::FromString(TEXT("Group Selection"))) ]
							]
							// Zone walls and node markers are useful to see and awkward to
							// build through. Both start shown; these only hide them in the
							// viewport - the tree keeps them and nothing changes on export.
							+ SHorizontalBox::Slot().FillWidth(1.f)
							+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f, 0.f, 0.f)
							[
								SNew(SButton)
									.ToolTipText(FText::FromString(TEXT("Show or hide the zone walls in the viewport.")))
									.OnClicked_Lambda([]{ BF6Api::SetVolumesShown(!BF6Api::VolumesShown()); return FReply::Handled(); })
									[ SNew(STextBlock).Text_Lambda([]{ return FText::FromString(
										BF6Api::VolumesShown() ? TEXT("Volumes") : TEXT("Volumes (hidden)")); }) ]
							]
							+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f, 0.f, 0.f)
							[
								SNew(SButton)
									.ToolTipText(FText::FromString(TEXT("Show or hide the cyan node markers in the viewport. Their children stay visible.")))
									.OnClicked_Lambda([]{ BF6Api::SetNodesShown(!BF6Api::NodesShown()); return FReply::Handled(); })
									[ SNew(STextBlock).Text_Lambda([]{ return FText::FromString(
										BF6Api::NodesShown() ? TEXT("Nodes") : TEXT("Nodes (hidden)")); }) ]
							]
						]
						+ SVerticalBox::Slot().FillHeight(1.f)[ BuildOutliner() ]
					];
			}))
			.SetDisplayName(LOCTEXT("BF6OutlinerTitle", "Scene"))
			.SetTooltipText(LOCTEXT("BF6OutlinerTip", "The objects in this experience, as the tree you built them in."));
		TM->TryInvokeTab(FTabId(OutlinerId));
	}

	void UnregisterOutlinerTab()
	{
		if (GStyle.IsValid())
		{
			FSlateStyleRegistry::UnRegisterSlateStyle(*GStyle);
			GStyle.Reset();
		}
	}

	// The symbols legend lives on the viewport as a slide-out (see the panel in
	// BF6BuildMode.cpp), so it needs the same brushes the tree rows use.
	const FSlateBrush* NodeSymbolBrush(const FString& IconName)
	{
		EnsureStyle();
		if (!GStyle.IsValid()) return nullptr;
		return GStyle->GetBrush(FName(*(FString(TEXT("BF6.Node.")) + IconName)));
	}

	// Rebuild the tree from the world. Rows are normally added one at a time as
	// actors appear, and a child added before its parent's row exists is dropped
	// and never comes back - which is why a map's own objects arrived headless:
	// the node parents showed and everything hung under them did not. Anything
	// that spawns a batch and then re-parents it calls this when it is done.
	void RefreshSceneTree()
	{
		if (TSharedPtr<ISceneOutliner> Live = GLiveOutliner.Pin())
			Live->FullRefresh();
	}

	void OpenOutlinerTab()
	{
		if (FLevelEditorModule* LE = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor")))
			if (TSharedPtr<FTabManager> TM = LE->GetLevelEditorTabManager())
				TM->TryInvokeTab(FTabId(LevelEditorTabIds::LevelEditorSceneOutliner));
	}
}

#undef LOCTEXT_NAMESPACE
