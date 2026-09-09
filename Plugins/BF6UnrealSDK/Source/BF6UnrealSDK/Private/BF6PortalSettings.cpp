#include "BF6PortalSettings.h"
#include "BF6PortalWeb.h"
#include "BF6PortalProfile.h"   // the experience list, the rotation, the thumbnail panel
#include "BF6Internal.h"
#include "BF6Theme.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

// ============================================================================
// See BF6PortalSettings.h for what this is. Layout of this file:
//   1. the model: the catalogue, the live overlay, the experience's mutators
//   2. the page seam: the injected script, the bridge, the page check
//   3. apply and save
//   4. the tool's own widgets
//   5. console commands and module hooks
// ============================================================================

namespace
{
	const TCHAR* kBridgeName = TEXT("bf6portalsettings");
	const TCHAR* kScriptId   = TEXT("bf6portalsettings");
	const FName  kTabId      = FName(TEXT("BF6PortalSettings"));

	// ---- 1. the model --------------------------------------------------------

	struct FOption
	{
		double  Value = 0.0;
		FString Label;
	};

	struct FSetting
	{
		FString TestId;      // the mutator key, or "title:<LABEL>" for a control the site leaves untagged
		FString Title;
		FString Caption;
		FString Kind;        // toggle | slider | dropdown | text | thumbnail | action
		bool    bPerTeam  = false;
		bool    bByTitle  = false;
		bool    bLiveOnly = false;   // the page has it, the shipped catalogue does not

		bool    bHasRange = false;
		double  Min = 0.0, Max = 1.0, Step = 1.0;
		TArray<FOption> Options;

		bool    bHasDefault = false;
		double  DefaultNum  = 0.0;
		FString DefaultText;
	};

	struct FCategory
	{
		FString Name;
		int32   Count = 0;
		TArray<FString> Items;
	};

	struct FPage
	{
		FString Key, Tab, Title, Path, Url;
		TArray<FSetting>  Settings;
		TArray<FSetting>  Fields;       // the publish steps' own inputs
		TArray<FCategory> Categories;   // the restriction pages
		TArray<TPair<FString, FString>> Maps;   // choose-maps: codename, size
	};

	TArray<FPage> GPages;

	// The experience's own values, decoded in the page out of the getPlayElement
	// response it already received.
	struct FMut
	{
		FString Tags;
		bool    bHasDefault = false;
		double  Default = 0.0;
		TMap<int32, double> Teams;
	};
	TMap<FString, FMut> GMuts;
	FString GMutExpId;      // which experience those values belong to
	int32   GTeamCount = 2;

	// The other two halves of the site's own settings model, carried verbatim in
	// the shape the site's whole-experience export writes them: the team
	// composition ([[1, {"humanCapacity": n}], ...]) and the asset restrictions.
	// Filled from a file import today; a live decode can fill the same two
	// without anything downstream noticing.
	TArray<TSharedPtr<FJsonValue>> GTeams;
	TSharedPtr<FJsonObject>        GRestrictions;

	// What the page currently shows, per control, per team column. Filled by the
	// live scrape and used both for the "changed" markers and for RESET.
	struct FLive
	{
		FString Kind;
		TArray<FString> Values;
	};
	TMap<FString, FLive> GLive;
	FString GLiveSection;       // the page key the scrape came from

	// One queued change.
	struct FEdit
	{
		// WHICH EXPERIENCE THIS EDIT IS FOR. Stamped when the change is queued and
		// never inferred later.
		//
		// The queue used to be one flat list with no destination on it, and every
		// step downstream asked the BROWSER where it was: queue changes on
		// experience A, browse to B, press APPLY, and the tool navigated to B's
		// pages and set A's values there. The site accepted it, because from the
		// site's point of view a person had made those changes on B. Nothing in
		// the tool could tell afterwards that it had happened.
		//
		// Every read, write, match and clear below is scoped by this field. An
		// edit with no experience on it cannot be queued at all, because there is
		// no destination to compare against later.
		FString Experience;
		FString PageKey, TestId, Kind, Title;
		int32   Team = -1;          // -1 = the only control, else the column index
		FString Value;              // as typed: "on", "120", an option label, a string
		FString Label;              // the dropdown option's label, when that is what to click
		bool    bApplied = false;
		FString Reason;
	};
	TArray<FEdit> GEdits;

	TArray<IConsoleObject*>  GCmds;
	FTSTicker::FDelegateHandle GTick;
	TStrongObjectPtr<UBF6PortalSettingsBridge> GBridge;
	FDelegateHandle GUrlHandle;

	uint32  GFingerprint = 1;
	FString GStatus = TEXT("Open an experience on the site. The panel reads its settings from the page.");
	FString GSaveVerdict;
	FString GSelectorLine;     // the last selector self-test, for the panel and the log
	TSet<FString> GLoggedSelectors;

	void Bump() { GFingerprint++; }

	FString LiveCachePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("portal"), TEXT("settings_catalog.live.json")));
	}

	FPage* PageFor(const FString& Key)
	{
		for (FPage& P : GPages) if (P.Key == Key) return &P;
		return nullptr;
	}

	FSetting* SettingFor(const FString& TestId, FPage** OutPage = nullptr)
	{
		for (FPage& P : GPages)
		{
			for (FSetting& S : P.Settings) if (S.TestId == TestId) { if (OutPage) *OutPage = &P; return &S; }
			for (FSetting& S : P.Fields)   if (S.TestId == TestId) { if (OutPage) *OutPage = &P; return &S; }
		}
		return nullptr;
	}

	// Which experience the browser is on, out of the uuid in its url. Every
	// queued change, every apply step and every panel row is now scoped by this,
	// so it is asked for far more often than it used to be and the answer is
	// remembered until the url moves. Game thread only, like the rest of this
	// file.
	FString CurrentExperienceId()
	{
		static FString CachedUrl;
		static FString CachedId;
		const FString Url = BF6PortalWeb::CurrentUrl();
		if (Url == CachedUrl) return CachedId;

		const FRegexPattern P(TEXT("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}"));
		FRegexMatcher M(P, Url);
		CachedUrl = Url;
		CachedId = M.FindNext() ? M.GetCaptureGroup(0).ToLower() : FString();
		return CachedId;
	}

	// The catalogue's paths are the site's own ("/bf6/experience/..."), and
	// BF6PortalWeb::BaseUrl() already ends in "/bf6", so the overlap is dropped
	// rather than a second base url being invented here.
	//
	// THE EXPERIENCE IS PASSED IN, NEVER READ OFF THE BROWSER HERE. This used to
	// call CurrentExperienceId(), so a navigation part-way through an apply run
	// silently retargeted the remaining pages at whatever the browser had moved
	// to. The caller owns the destination and hands it down.
	FString UrlForPage(const FPage& P, const FString& ExperienceId)
	{
		const FString Base = BF6PortalWeb::BaseUrl();
		FString Path = P.Path;
		if (Base.EndsWith(TEXT("/bf6")) && Path.StartsWith(TEXT("/bf6"))) Path = Path.RightChop(4);
		// teams=1%2C2 is the shape the site itself uses; teams=0 bounces to login.
		return ExperienceId.IsEmpty() ? (Base + Path)
			: FString::Printf(TEXT("%s%s?id=%s&teams=1%%2C2"), *Base, *Path, *ExperienceId);
	}

	// A one-shot site action - add a map, flip a restriction, go to a publish
	// step - is aimed at the experience the panel was on when the button was
	// pressed. Getting to the page is asynchronous, so the browser can be on a
	// different experience by the time the callback runs. Nothing is written to
	// a page the action was not aimed at, and the refusal says which is which.
	bool StillOnExperience(const FString& ExperienceId, const TCHAR* What)
	{
		const FString Now = CurrentExperienceId();
		if (!ExperienceId.IsEmpty() && Now.Equals(ExperienceId, ESearchCase::IgnoreCase)) return true;
		UE_LOG(LogBF6Portal, Warning,
			TEXT("Portal settings: %s was for experience %s and the panel is on %s now, so nothing was changed."),
			What, ExperienceId.IsEmpty() ? TEXT("no experience") : *ExperienceId,
			Now.IsEmpty() ? TEXT("no experience") : *Now);
		GStatus = FString::Printf(
			TEXT("%s was not done. It was aimed at experience %s and the Portal panel is on %s now."),
			What, ExperienceId.IsEmpty() ? TEXT("no experience") : *ExperienceId,
			Now.IsEmpty() ? TEXT("a page with no experience") : *Now);
		Bump();
		return false;
	}

	// ---- reading the catalogue -----------------------------------------------

	bool ReadSetting(const TSharedPtr<FJsonObject>& O, FSetting& S)
	{
		if (!O.IsValid()) return false;
		if (!O->TryGetStringField(TEXT("testId"), S.TestId) || S.TestId.IsEmpty()) return false;
		O->TryGetStringField(TEXT("title"), S.Title);
		O->TryGetStringField(TEXT("caption"), S.Caption);
		O->TryGetStringField(TEXT("kind"), S.Kind);
		O->TryGetBoolField(TEXT("perTeam"), S.bPerTeam);
		O->TryGetBoolField(TEXT("byTitle"), S.bByTitle);
		if (S.Title.IsEmpty()) S.Title = S.TestId;

		double D = 0.0;
		if (O->TryGetNumberField(TEXT("min"), D)) { S.Min = D; S.bHasRange = true; }
		if (O->TryGetNumberField(TEXT("max"), D)) { S.Max = D; S.bHasRange = true; }
		if (O->TryGetNumberField(TEXT("step"), D)) S.Step = D;
		if (S.Step <= 0.0) S.Step = 1.0;

		const TArray<TSharedPtr<FJsonValue>>* Opts = nullptr;
		if (O->TryGetArrayField(TEXT("options"), Opts))
		{
			for (const TSharedPtr<FJsonValue>& OV : *Opts)
			{
				const TSharedPtr<FJsonObject> OO = OV->AsObject();
				if (!OO.IsValid()) continue;
				FOption Op;
				OO->TryGetNumberField(TEXT("value"), Op.Value);
				OO->TryGetStringField(TEXT("label"), Op.Label);
				if (!Op.Label.IsEmpty()) S.Options.Add(Op);
			}
		}

		const TSharedPtr<FJsonValue> Def = O->TryGetField(TEXT("default"));
		if (Def.IsValid() && Def->Type != EJson::Null)
		{
			S.bHasDefault = true;
			if (Def->Type == EJson::Boolean)      { S.DefaultNum = Def->AsBool() ? 1.0 : 0.0; S.DefaultText = Def->AsBool() ? TEXT("On") : TEXT("Off"); }
			else if (Def->Type == EJson::Number)  { S.DefaultNum = Def->AsNumber(); S.DefaultText = FString::SanitizeFloat(S.DefaultNum); }
			else                                  { S.DefaultText = Def->AsString(); S.DefaultNum = FCString::Atod(*S.DefaultText); }
		}
		if (S.Kind.IsEmpty()) S.Kind = S.Options.Num() ? TEXT("dropdown") : TEXT("slider");
		return true;
	}

	void ReadCatalogue(const FString& Json, bool bLiveOverlay)
	{
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) return;

		const TArray<TSharedPtr<FJsonValue>>* Pages = nullptr;
		if (!Root->TryGetArrayField(TEXT("pages"), Pages)) return;

		for (const TSharedPtr<FJsonValue>& PV : *Pages)
		{
			const TSharedPtr<FJsonObject> PO = PV->AsObject();
			if (!PO.IsValid()) continue;
			FString Key;
			if (!PO->TryGetStringField(TEXT("key"), Key) || Key.IsEmpty()) continue;

			FPage* Existing = PageFor(Key);
			if (bLiveOverlay && !Existing) continue;   // an overlay never invents a page
			FPage& P = Existing ? *Existing : GPages.AddDefaulted_GetRef();
			P.Key = Key;
			if (!bLiveOverlay)
			{
				PO->TryGetStringField(TEXT("tab"), P.Tab);
				PO->TryGetStringField(TEXT("title"), P.Title);
				PO->TryGetStringField(TEXT("path"), P.Path);
				PO->TryGetStringField(TEXT("url"), P.Url);
			}

			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (PO->TryGetArrayField(TEXT("settings"), Arr))
			{
				for (const TSharedPtr<FJsonValue>& SV : *Arr)
				{
					FSetting S;
					if (!ReadSetting(SV->AsObject(), S)) continue;
					bool bMerged = false;
					for (FSetting& Old : P.Settings)
						if (Old.TestId == S.TestId) { const bool bWasLive = Old.bLiveOnly; Old = S; Old.bLiveOnly = bWasLive; bMerged = true; break; }
					if (!bMerged) { S.bLiveOnly = bLiveOverlay; P.Settings.Add(S); }
				}
			}
			if (!bLiveOverlay)
			{
				if (PO->TryGetArrayField(TEXT("fields"), Arr))
					for (const TSharedPtr<FJsonValue>& SV : *Arr)
					{
						FSetting S;
						if (ReadSetting(SV->AsObject(), S)) P.Fields.Add(S);
					}
				if (PO->TryGetArrayField(TEXT("categories"), Arr))
				{
					P.Categories.Reset();
					for (const TSharedPtr<FJsonValue>& CV : *Arr)
					{
						const TSharedPtr<FJsonObject> CO = CV->AsObject();
						if (!CO.IsValid()) continue;
						FCategory C;
						CO->TryGetStringField(TEXT("name"), C.Name);
						int32 N = 0; CO->TryGetNumberField(TEXT("count"), N); C.Count = N;
						const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
						if (CO->TryGetArrayField(TEXT("items"), Items))
							for (const TSharedPtr<FJsonValue>& IV : *Items)
							{
								const TSharedPtr<FJsonObject> IO = IV->AsObject();
								FString L;
								if (IO.IsValid() && IO->TryGetStringField(TEXT("label"), L)) C.Items.Add(L);
							}
						P.Categories.Add(MoveTemp(C));
					}
				}
				if (PO->TryGetArrayField(TEXT("maps"), Arr))
				{
					P.Maps.Reset();
					for (const TSharedPtr<FJsonValue>& MV : *Arr)
					{
						const TSharedPtr<FJsonObject> MO = MV->AsObject();
						if (!MO.IsValid()) continue;
						FString M, Sz;
						MO->TryGetStringField(TEXT("map"), M);
						MO->TryGetStringField(TEXT("size"), Sz);
						if (!M.IsEmpty()) P.Maps.Emplace(M, Sz);
					}
				}
			}
		}
	}

	void LoadCatalogue()
	{
		GPages.Reset();
		const FString Shipped = FPaths::Combine(g_pluginDir, TEXT("Resources"), TEXT("portal"), TEXT("settings_catalog.json"));
		FString Json;
		if (FFileHelper::LoadFileToString(Json, *Shipped))
		{
			ReadCatalogue(Json, false);
			int32 N = 0; for (const FPage& P : GPages) N += P.Settings.Num();
			UE_LOG(LogBF6Portal, Display, TEXT("Portal settings catalogue: %d page(s), %d setting(s) from %s"),
				GPages.Num(), N, *Shipped);
		}
		else
		{
			UE_LOG(LogBF6Portal, Error, TEXT("Portal settings catalogue missing: %s. The panel can still show what it scrapes off the page."), *Shipped);
		}
		FString Live;
		if (FFileHelper::LoadFileToString(Live, *LiveCachePath()))
		{
			ReadCatalogue(Live, true);
			UE_LOG(LogBF6Portal, Display, TEXT("Portal settings live overlay applied from %s"), *LiveCachePath());
		}
	}

	// The live overlay, written whole every time a page is scraped, so a site
	// update that renames a setting survives an editor restart.
	void WriteLiveOverlay()
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("version"), 1);
		Root->SetStringField(TEXT("writtenBy"), TEXT("BF6PortalSettings live scrape"));
		TArray<TSharedPtr<FJsonValue>> Pages;
		for (const FPage& P : GPages)
		{
			bool bAny = false;
			for (const FSetting& S : P.Settings) if (S.bLiveOnly) { bAny = true; break; }
			if (!bAny) continue;
			TSharedPtr<FJsonObject> PO = MakeShared<FJsonObject>();
			PO->SetStringField(TEXT("key"), P.Key);
			TArray<TSharedPtr<FJsonValue>> Arr;
			for (const FSetting& S : P.Settings)
			{
				if (!S.bLiveOnly) continue;
				TSharedPtr<FJsonObject> SO = MakeShared<FJsonObject>();
				SO->SetStringField(TEXT("testId"), S.TestId);
				SO->SetStringField(TEXT("title"), S.Title);
				SO->SetStringField(TEXT("caption"), S.Caption);
				SO->SetStringField(TEXT("kind"), S.Kind);
				SO->SetBoolField(TEXT("perTeam"), S.bPerTeam);
				if (S.bHasRange) { SO->SetNumberField(TEXT("min"), S.Min); SO->SetNumberField(TEXT("max"), S.Max); }
				SO->SetNumberField(TEXT("step"), S.Step);
				if (S.Options.Num())
				{
					TArray<TSharedPtr<FJsonValue>> Opts;
					for (const FOption& O : S.Options)
					{
						TSharedPtr<FJsonObject> OO = MakeShared<FJsonObject>();
						OO->SetNumberField(TEXT("value"), O.Value);
						OO->SetStringField(TEXT("label"), O.Label);
						Opts.Add(MakeShared<FJsonValueObject>(OO));
					}
					SO->SetArrayField(TEXT("options"), Opts);
				}
				Arr.Add(MakeShared<FJsonValueObject>(SO));
			}
			PO->SetArrayField(TEXT("settings"), Arr);
			Pages.Add(MakeShared<FJsonValueObject>(PO));
		}
		if (Pages.Num() == 0) return;
		Root->SetArrayField(TEXT("pages"), Pages);
		FString Out;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root.ToSharedRef(), W);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(LiveCachePath()), true);
		FFileHelper::SaveStringToFile(Out, *LiveCachePath());
	}

	// ---- what a control currently reads --------------------------------------

	// The site value for one control, as text. The page's own scrape wins,
	// because it is what the user is looking at; the experience's mutators are
	// the fallback for a page the panel is not on; the catalogue default is the
	// last resort and is shown greyed.
	FString SiteValue(const FSetting& S, int32 Team, bool& bOutFromSite)
	{
		bOutFromSite = true;
		if (const FLive* L = GLive.Find(S.TestId))
		{
			const int32 Idx = (Team < 0) ? 0 : Team;
			if (L->Values.IsValidIndex(Idx) && !L->Values[Idx].IsEmpty()) return L->Values[Idx];
		}
		if (const FMut* M = GMuts.Find(S.TestId))
		{
			// The site numbers teams from 1; the panel's columns from 0.
			if (const double* V = M->Teams.Find(Team < 0 ? 1 : Team + 1))
			{
				if (S.Kind == TEXT("toggle")) return *V != 0.0 ? TEXT("On") : TEXT("Off");
				return FString::SanitizeFloat(*V);
			}
			if (M->bHasDefault)
			{
				if (S.Kind == TEXT("toggle")) return M->Default != 0.0 ? TEXT("On") : TEXT("Off");
				return FString::SanitizeFloat(M->Default);
			}
		}
		bOutFromSite = false;
		return S.bHasDefault ? S.DefaultText : FString(TEXT("not set"));
	}

	// An edit is only ever found for ONE experience. Passing the destination in
	// is what stops experience A's queued value from being shown as, applied to,
	// or cleared by, experience B. An empty ExperienceId matches nothing, because
	// "no destination" is not a destination.
	FEdit* EditFor(const FString& ExperienceId, const FString& TestId, int32 Team)
	{
		if (ExperienceId.IsEmpty()) return nullptr;
		for (FEdit& E : GEdits)
			if (E.Experience == ExperienceId && E.TestId == TestId && E.Team == Team) return &E;
		return nullptr;
	}

	FString DisplayValue(const FSetting& S, int32 Team, bool& bChanged, bool& bFromSite)
	{
		bChanged = false;
		if (const FEdit* E = EditFor(CurrentExperienceId(), S.TestId, Team))
		{
			bChanged = true;
			bFromSite = true;
			return E->Label.IsEmpty() ? E->Value : E->Label;
		}
		return SiteValue(S, Team, bFromSite);
	}

	double AsNumber(const FString& V)
	{
		const FString L = V.TrimStartAndEnd().ToLower();
		if (L == TEXT("on") || L == TEXT("true") || L == TEXT("yes")) return 1.0;
		if (L == TEXT("off") || L == TEXT("false") || L == TEXT("no")) return 0.0;
		return FCString::Atod(*V);
	}

	// ---- 2. the page seam ----------------------------------------------------

	FString JsLit(const FString& In)
	{
		FString S = In;
		S.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		S.ReplaceInline(TEXT("'"), TEXT("\\'"));
		S.ReplaceInline(TEXT("\r"), TEXT(""));
		S.ReplaceInline(TEXT("\n"), TEXT(" "));
		return S;
	}

	void Call(const FString& Js)
	{
		BF6PortalWeb::Exec(FString::Printf(
			TEXT("try { if (window.BF6PortalSettings) %s; else console.log('BF6SETTINGS not injected'); } catch (e) { console.log('BF6SETTINGS ' + e); }"), *Js));
	}

	// The tool's own page check, on the same shape as the profile's ExpectPage.
	// It waits for THIS feature's own probe rather than the profile's, because
	// the profile's coarse "editor" kind cannot tell the modifier pages apart
	// and the apply path has to know exactly which one it is on.
	//
	// ---- accessor wanted ----
	// If BF6PortalProfile ever exposes ExpectPage(key, url, timeout, done) and a
	// per-section page key, this whole block is deletable and the two page-check
	// tables become one.
	struct FExpect
	{
		FString Key;
		double  Deadline = 0.0;
		bool    bActive = false;
		TFunction<void(bool, const FString&)> Done;
	};
	FExpect GExpect;

	void ResolveExpect(bool bOk, const FString& What)
	{
		if (!GExpect.bActive) return;
		GExpect.bActive = false;
		TFunction<void(bool, const FString&)> Fn = MoveTemp(GExpect.Done);
		GExpect.Done = nullptr;
		if (Fn) Fn(bOk, What);
	}

	void ExpectPage(const FString& Key, const FString& NavUrl, float Timeout, TFunction<void(bool, const FString&)> Done)
	{
		ResolveExpect(false, TEXT("superseded by another page check"));
		GExpect.Key = Key;
		GExpect.Deadline = FPlatformTime::Seconds() + Timeout;
		GExpect.bActive = true;
		GExpect.Done = MoveTemp(Done);
		if (GLiveSection == Key)
		{
			// Already there. Ask for a fresh report rather than navigating, so a
			// half-typed change on the page is never thrown away by a reload.
			Call(TEXT("window.BF6PortalSettings.scrape()"));
			return;
		}
		if (!NavUrl.IsEmpty()) BF6PortalWeb::Open(NavUrl);
	}

	// ---- 3. apply and save ---------------------------------------------------

	TArray<FString> GApplyQueue;
	bool  GApplying = false;
	int32 GApplyOk = 0, GApplyFailed = 0;

	// THE DESTINATION OF THE RUN IN FLIGHT, pinned when APPLY is pressed.
	//
	// An apply run walks several pages and every step of it is asynchronous: a
	// navigation, a page report, a batch, an acknowledgment per control. The
	// browser can move to a different experience during any of those gaps. This
	// is the one experience the run is allowed to touch, and every step checks
	// it before doing anything to the page.
	FString GApplyExperience;

	void ApplyNextPage();

	// The run stops the moment the browser is no longer on the experience the
	// run was started for. Stopping is the only safe answer: the pages ahead
	// would be somebody else's experience, and the tool cannot ask.
	bool ApplyDriftedAway()
	{
		if (!GApplying) return false;
		const FString Now = CurrentExperienceId();
		if (Now.Equals(GApplyExperience, ESearchCase::IgnoreCase)) return false;

		for (FEdit& E : GEdits)
			if (E.Experience == GApplyExperience && !E.bApplied)
				E.Reason = TEXT("the Portal panel moved to another experience part-way through, so this was not set");

		UE_LOG(LogBF6Portal, Warning,
			TEXT("Portal settings apply stopped: it was started for experience %s and the panel is on %s now. Nothing was written to %s."),
			*GApplyExperience, Now.IsEmpty() ? TEXT("no experience") : *Now,
			Now.IsEmpty() ? TEXT("the other page") : *Now);
		GStatus = FString::Printf(
			TEXT("Apply stopped. It was queued for experience %s and the Portal panel moved to %s. ")
			TEXT("Nothing was changed on the page you moved to. Go back to %s and press APPLY ON SITE again."),
			*GApplyExperience, Now.IsEmpty() ? TEXT("a page with no experience") : *Now, *GApplyExperience);
		GApplying = false;
		GApplyQueue.Reset();
		GApplyExperience.Reset();
		Bump();
		return true;
	}

	void SendPageBatch(const FString& PageKey)
	{
		if (ApplyDriftedAway()) return;

		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FEdit& E : GEdits)
		{
			// Only this run's experience. Two drafts can be queued at once and
			// the other one must not travel out on this batch.
			if (E.Experience != GApplyExperience) continue;
			if (E.PageKey != PageKey || E.bApplied) continue;
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("testId"), E.TestId);
			O->SetNumberField(TEXT("team"), E.Team);
			O->SetStringField(TEXT("kind"), E.Kind);
			if (E.Kind == TEXT("toggle"))      O->SetBoolField(TEXT("value"), AsNumber(E.Value) != 0.0);
			else if (E.Kind == TEXT("slider")) O->SetNumberField(TEXT("value"), AsNumber(E.Value));
			else                               O->SetStringField(TEXT("value"), E.Value);
			if (!E.Label.IsEmpty()) O->SetStringField(TEXT("label"), E.Label);
			Arr.Add(MakeShared<FJsonValueObject>(O));
		}
		if (Arr.Num() == 0) { ApplyNextPage(); return; }

		FString Json;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Arr, W);
		GStatus = FString::Printf(TEXT("Applying %d change(s) on %s..."), Arr.Num(), *PageKey);
		Bump();
		Call(FString::Printf(TEXT("window.BF6PortalSettings.applySet('%s')"), *JsLit(Json)));
	}

	void ApplyNextPage()
	{
		if (!GApplying) return;
		if (ApplyDriftedAway()) return;

		if (GApplyQueue.Num() == 0)
		{
			GStatus = FString::Printf(TEXT("%d change(s) applied to experience %s, %d still pending. SAVE ON PORTAL presses the site's own save."),
				GApplyOk, *GApplyExperience, GApplyFailed);
			UE_LOG(LogBF6Portal, Display, TEXT("Portal settings apply finished for %s: %d applied, %d pending"),
				*GApplyExperience, GApplyOk, GApplyFailed);
			GApplying = false;
			GApplyExperience.Reset();
			Bump();
			return;
		}
		const FString Key = GApplyQueue[0];
		GApplyQueue.RemoveAt(0);
		FPage* P = PageFor(Key);
		if (!P) { ApplyNextPage(); return; }
		GStatus = FString::Printf(TEXT("Going to %s..."), *P->Title);
		Bump();
		// The url is built from the RUN's experience, not from wherever the
		// browser is by the time this navigates.
		const FString RunFor = GApplyExperience;
		ExpectPage(Key, UrlForPage(*P, RunFor), 25.f, [Key, RunFor](bool bOk, const FString& What)
		{
			// The run may have been stopped or restarted while this navigation
			// was in the air. A reply belonging to a previous run must not drive
			// the current one.
			if (!GApplying || GApplyExperience != RunFor) return;
			if (!bOk)
			{
				for (FEdit& E : GEdits)
					if (E.Experience == RunFor && E.PageKey == Key && !E.bApplied)
					{
						E.Reason = FString::Printf(TEXT("the tool could not get to that page (%s)"), *What);
						GApplyFailed++;
					}
				UE_LOG(LogBF6Portal, Warning, TEXT("Portal settings: page check for %s failed (%s)"), *Key, *What);
				ApplyNextPage();
				return;
			}
			SendPageBatch(Key);
		});
	}

	// ---- the bridge messages -------------------------------------------------

	FString ValueText(const TSharedPtr<FJsonValue>& V)
	{
		if (!V.IsValid() || V->Type == EJson::Null) return FString();
		if (V->Type == EJson::Boolean) return V->AsBool() ? TEXT("On") : TEXT("Off");
		if (V->Type == EJson::Number)
		{
			const double D = V->AsNumber();
			return FMath::IsNearlyEqual(D, FMath::RoundToDouble(D)) ? FString::FromInt((int32)FMath::RoundToDouble(D)) : FString::SanitizeFloat(D);
		}
		return V->AsString();
	}

	void HandlePage(const TSharedPtr<FJsonObject>& Root)
	{
		FString Section;
		Root->TryGetStringField(TEXT("section"), Section);
		GLiveSection = Section;

		// The selector self-test: said once per section, so a moved anchor is one
		// line in the Output Log rather than a feature that quietly stops working.
		const TSharedPtr<FJsonObject>* Sel = nullptr;
		if (Root->TryGetObjectField(TEXT("selectors"), Sel) && Sel && Sel->IsValid())
		{
			FString Line;
			for (const auto& Pair : (*Sel)->Values)
				Line += FString::Printf(TEXT("%s=%d "), *Pair.Key, (int32)Pair.Value->AsNumber());
			GSelectorLine = Line.TrimEnd();
			if (!Section.IsEmpty() && !GLoggedSelectors.Contains(Section))
			{
				GLoggedSelectors.Add(Section);
				UE_LOG(LogBF6Portal, Display, TEXT("Portal settings selectors on %s: %s"), *Section, *GSelectorLine);
			}
		}

		FPage* P = PageFor(Section);
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Root->TryGetArrayField(TEXT("settings"), Arr))
		{
			int32 Added = 0;
			int32 Changed = 0;
			for (const TSharedPtr<FJsonValue>& SV : *Arr)
			{
				const TSharedPtr<FJsonObject> O = SV->AsObject();
				if (!O.IsValid()) continue;
				FString TestId, Kind, Title, Caption;
				O->TryGetStringField(TEXT("testId"), TestId);
				O->TryGetStringField(TEXT("kind"), Kind);
				O->TryGetStringField(TEXT("title"), Title);
				O->TryGetStringField(TEXT("caption"), Caption);
				if (TestId.IsEmpty()) continue;

				FLive L;
				L.Kind = Kind;
				const TArray<TSharedPtr<FJsonValue>>* Vals = nullptr;
				if (O->TryGetArrayField(TEXT("values"), Vals))
					for (const TSharedPtr<FJsonValue>& VV : *Vals) L.Values.Add(ValueText(VV));
				GLive.Add(TestId, MoveTemp(L));

				int32 Teams = 0;
				if (O->TryGetNumberField(TEXT("teams"), Teams) && Teams > GTeamCount && Teams <= 8) GTeamCount = Teams;

				// Read what the page says this setting IS, separately from what
				// it is currently set to. The shape is read the same way whether
				// the setting is new or already known, because the site changes
				// existing settings too: a slider's range, a caption, the
				// options in a dropdown. This used to run only for settings the
				// catalogue had never seen, so a range that Portal widened kept
				// the shipped bounds for ever and the panel refused values the
				// site accepts.
				auto ReadShape = [&O](FSetting& S)
				{
					double D = 0.0;
					if (O->TryGetNumberField(TEXT("min"), D)) { S.Min = D; S.bHasRange = true; }
					if (O->TryGetNumberField(TEXT("max"), D)) { S.Max = D; S.bHasRange = true; }
					if (O->TryGetNumberField(TEXT("step"), D) && D > 0.0) S.Step = D;
					O->TryGetBoolField(TEXT("perTeam"), S.bPerTeam);
					const TArray<TSharedPtr<FJsonValue>>* Opts = nullptr;
					if (O->TryGetArrayField(TEXT("options"), Opts))
					{
						S.Options.Reset();
						for (const TSharedPtr<FJsonValue>& OV : *Opts)
						{
							const TSharedPtr<FJsonObject> OO = OV->AsObject();
							if (!OO.IsValid()) continue;
							FOption Op;
							OO->TryGetNumberField(TEXT("value"), Op.Value);
							OO->TryGetStringField(TEXT("label"), Op.Label);
							if (!Op.Label.IsEmpty()) S.Options.Add(Op);
						}
					}
				};

				FSetting* Known = SettingFor(TestId);
				if (P && !Known)
				{
					// A setting the page has and the shipped catalogue does not:
					// the site added or renamed one, and the panel shows it anyway.
					FSetting S;
					S.TestId = TestId;
					S.Title = Title.IsEmpty() ? TestId : Title;
					S.Caption = Caption;
					S.Kind = Kind;
					S.bLiveOnly = true;
					ReadShape(S);
					P->Settings.Add(MoveTemp(S));
					Added++;
				}
				else if (Known)
				{
					// Already catalogued. Take the page's word for its shape if
					// it differs, and say so: the shipped catalogue was mined
					// from one build and the site has moved on since.
					//
					// An unmounted dropdown reports no options at all, which is
					// "not observed" rather than "this setting lost its
					// options", so an empty list never overwrites a known one.
					FSetting Fresh = *Known;
					if (!Title.IsEmpty())   { Fresh.Title = Title; }
					if (!Caption.IsEmpty()) { Fresh.Caption = Caption; }
					if (!Kind.IsEmpty())    { Fresh.Kind = Kind; }
					ReadShape(Fresh);
					if (Fresh.Options.Num() == 0) { Fresh.Options = Known->Options; }

					auto SameOptions = [](const TArray<FOption>& A, const TArray<FOption>& B)
					{
						if (A.Num() != B.Num()) return false;
						for (int32 i = 0; i < A.Num(); i++)
							if (A[i].Value != B[i].Value || A[i].Label != B[i].Label) return false;
						return true;
					};
					const bool bDiffers =
						Fresh.Title != Known->Title || Fresh.Caption != Known->Caption ||
						Fresh.Kind != Known->Kind || Fresh.bPerTeam != Known->bPerTeam ||
						Fresh.bHasRange != Known->bHasRange ||
						(Fresh.bHasRange && (Fresh.Min != Known->Min || Fresh.Max != Known->Max)) ||
						Fresh.Step != Known->Step || !SameOptions(Fresh.Options, Known->Options);
					if (bDiffers)
					{
						UE_LOG(LogBF6Portal, Display,
							TEXT("Portal settings: %s has changed on the site since the catalogue was mined"), *TestId);
						*Known = MoveTemp(Fresh);
						Changed++;
					}
				}
			}
			if (Added > 0 || Changed > 0)
			{
				UE_LOG(LogBF6Portal, Display,
					TEXT("Portal settings on %s: %d not in the shipped catalogue, %d changed since it was mined; kept in the live overlay"),
					*Section, Added, Changed);
				WriteLiveOverlay();
			}
		}

		// the restriction pages, as the site currently has them
		if (P && Root->TryGetArrayField(TEXT("categories"), Arr))
		{
			for (const TSharedPtr<FJsonValue>& CV : *Arr)
			{
				const TSharedPtr<FJsonObject> CO = CV->AsObject();
				if (!CO.IsValid()) continue;
				FString Name;
				CO->TryGetStringField(TEXT("name"), Name);
				int32 On = 0, Count = 0;
				CO->TryGetNumberField(TEXT("on"), On);
				CO->TryGetNumberField(TEXT("count"), Count);
				for (FCategory& C : P->Categories)
					if (C.Name.ToUpper() == Name.ToUpper() && Count > 0) { C.Count = Count; break; }
			}
		}

		GStatus = Section.IsEmpty()
			? TEXT("The page is not an experience settings page.")
			: FString::Printf(TEXT("Read %s off the page."), *Section);
		Bump();

		if (GExpect.bActive && GExpect.Key == Section)
		{
			ResolveExpect(true, Section);
		}
	}

	// The experience's own values, decoded in the PAGE.
	//
	// ---- accessor wanted ----
	// BF6PortalProfile parses getPlayElement but keeps neither the bytes nor the
	// mutators, and its protobuf reader is file-local, so there is nothing here
	// to call. One accessor would let this move into C++ and share that reader:
	//
	//   bool BF6PortalProfile::PlayElementBytes(const FString& Id, TArray<uint8>& Out);
	//
	// Until then the decode happens in settings.js, which is the cheaper half of
	// the trade anyway: a getPlayElement body runs past a megabyte and would have
	// to be chunked back across the bridge, where the decoded values are a few KB.
	void HandleMutators(const TSharedPtr<FJsonObject>& Root)
	{
		const TSharedPtr<FJsonObject>* Muts = nullptr;
		if (!Root->TryGetObjectField(TEXT("mutators"), Muts) || !Muts || !Muts->IsValid()) return;
		GMuts.Reset();
		GTeamCount = 2;
		for (const auto& Pair : (*Muts)->Values)
		{
			const TSharedPtr<FJsonObject> O = Pair.Value->AsObject();
			if (!O.IsValid()) continue;
			FMut M;
			O->TryGetStringField(TEXT("tags"), M.Tags);
			const TSharedPtr<FJsonValue> Def = O->TryGetField(TEXT("def"));
			if (Def.IsValid() && Def->Type == EJson::Number) { M.bHasDefault = true; M.Default = Def->AsNumber(); }
			const TSharedPtr<FJsonObject>* Teams = nullptr;
			if (O->TryGetObjectField(TEXT("teams"), Teams) && Teams && Teams->IsValid())
				for (const auto& TP : (*Teams)->Values)
				{
					const int32 Id = FCString::Atoi(*TP.Key);
					M.Teams.Add(Id, TP.Value->AsNumber());
					if (Id > GTeamCount && Id <= 8) GTeamCount = Id;
				}
			// The json object's key type is the module's own shared string, not
			// FString, so it is converted rather than assumed.
			GMuts.Add(FString(*Pair.Key), MoveTemp(M));
		}
		GMutExpId = CurrentExperienceId();
		GStatus = FString::Printf(TEXT("Read %d live value(s) from the experience."), GMuts.Num());
		UE_LOG(LogBF6Portal, Display, TEXT("Portal settings: %d mutator(s) decoded, %d team(s)"), GMuts.Num(), GTeamCount);
		Bump();
	}

	// ---- WATCH THE SITE -----------------------------------------------------
	//
	// Somebody moving a control on the site, arriving as one burst rather than
	// one message per pixel of a slider drag. It updates what the panel shows
	// the site as having; it never queues a change and never writes anything
	// back, because the site is the one that just changed.
	FString GWatchLine;
	int32   GWatchBursts = 0, GWatchControls = 0;
	bool    GWatchOn = false;

	void HandleWatch(const TSharedPtr<FJsonObject>& Root)
	{
		bool bStarted = false, bStopped = false;
		Root->TryGetBoolField(TEXT("started"), bStarted);
		Root->TryGetBoolField(TEXT("stopped"), bStopped);
		FString Section;
		Root->TryGetStringField(TEXT("section"), Section);
		if (bStarted || bStopped)
		{
			GWatchLine = bStarted
				? FString::Printf(TEXT("watching %s"), Section.IsEmpty() ? TEXT("the page") : *Section)
				: FString(TEXT("not watching"));
			UE_LOG(LogBF6Portal, Display, TEXT("Portal settings watch: %s"), *GWatchLine);
			Bump();
			return;
		}
		const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
		if (!Root->TryGetArrayField(TEXT("changes"), Changes) || Changes->Num() == 0) return;

		TArray<FString> Named;
		for (const auto& CV : *Changes)
		{
			const TSharedPtr<FJsonObject> C = CV->AsObject();
			if (!C.IsValid()) continue;
			FString TestId, CKind;
			C->TryGetStringField(TEXT("testId"), TestId);
			C->TryGetStringField(TEXT("kind"), CKind);
			if (TestId.IsEmpty()) continue;
			const TArray<TSharedPtr<FJsonValue>>* Vals = nullptr;
			FLive& L = GLive.FindOrAdd(TestId);
			L.Kind = CKind;
			L.Values.Reset();
			if (C->TryGetArrayField(TEXT("values"), Vals))
				for (const auto& V : *Vals) L.Values.Add(ValueText(V));
			GWatchControls++;
			Named.Add(FString::Printf(TEXT("%s=%s"), *TestId,
				L.Values.Num() ? *FString::Join(L.Values, TEXT("/")) : TEXT("?")));
		}
		if (Named.Num() == 0) return;
		GWatchBursts++;
		// ONE LINE PER BURST, never one per keystroke.
		UE_LOG(LogBF6Portal, Display, TEXT("Portal settings watch: %d control(s) changed on the site: %s"),
			Named.Num(), *FString::Join(Named, TEXT(", ")).Left(400));
		GWatchLine = FString::Printf(TEXT("watching %s, last change %s"),
			Section.IsEmpty() ? TEXT("the page") : *Section, *FDateTime::Now().ToString(TEXT("%H:%M:%S")));
		GStatus = FString::Printf(TEXT("The site changed %d setting(s); the panel followed."), Named.Num());
		Bump();
	}

	void HandleApplied(const TSharedPtr<FJsonObject>& Root)
	{
		FString TestId, Reason;
		int32 Team = -1;
		bool bOk = false;
		Root->TryGetStringField(TEXT("testId"), TestId);
		Root->TryGetNumberField(TEXT("team"), Team);
		Root->TryGetBoolField(TEXT("ok"), bOk);
		Root->TryGetStringField(TEXT("reason"), Reason);
		const FString Now = ValueText(Root->TryGetField(TEXT("value")));

		// AN ACKNOWLEDGMENT BELONGS TO THE RUN THAT ASKED FOR IT.
		//
		// This used to find the queued edit by test id and team alone, so a reply
		// arriving from experience B's page marked experience A's draft applied,
		// wrote B's value into the panel as A's, and let the save path throw A's
		// edit away as done. It is matched against the run's own destination now,
		// and a reply from anywhere else is logged and dropped.
		const FString PageNow = CurrentExperienceId();
		if (!GApplying || GApplyExperience.IsEmpty() || !PageNow.Equals(GApplyExperience, ESearchCase::IgnoreCase))
		{
			UE_LOG(LogBF6Portal, Warning,
				TEXT("Portal settings: ignored a page reply for %s. The panel is on %s and no apply run is open for it."),
				*TestId, PageNow.IsEmpty() ? TEXT("no experience") : *PageNow);
			return;
		}

		FEdit* E = EditFor(GApplyExperience, TestId, Team);
		if (!E) return;
		E->bApplied = bOk;
		E->Reason = bOk ? FString() : Reason;
		if (bOk)
		{
			GApplyOk++;
			// The page is now the truth for this control.
			FLive& L = GLive.FindOrAdd(TestId);
			const int32 Idx = Team < 0 ? 0 : Team;
			while (L.Values.Num() <= Idx) L.Values.Add(FString());
			L.Values[Idx] = Now;
		}
		else
		{
			GApplyFailed++;
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal settings: %s team %d did not take (%s)"), *TestId, Team + 1, *Reason);
		}
		Bump();
	}

	void HandleSave(const TSharedPtr<FJsonObject>& Root)
	{
		bool bOk = false;
		int32 Status = 0, Grpc = -1;
		FString Msg, Toast, Reason;
		Root->TryGetBoolField(TEXT("ok"), bOk);
		Root->TryGetNumberField(TEXT("status"), Status);
		Root->TryGetNumberField(TEXT("grpcStatus"), Grpc);
		Root->TryGetStringField(TEXT("grpcMessage"), Msg);
		Root->TryGetStringField(TEXT("toast"), Toast);
		Root->TryGetStringField(TEXT("reason"), Reason);

		GSaveVerdict = bOk
			? FString::Printf(TEXT("Saved on Portal (http %d, grpc-status %d).%s"), Status, Grpc,
				Toast.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" The site said: %s"), *Toast))
			: FString::Printf(TEXT("The site did not save it: %s%s"),
				Reason.IsEmpty() ? *FString::Printf(TEXT("http %d, grpc-status %d %s"), Status, Grpc, *Msg) : *Reason,
				Toast.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Toast));
		GStatus = GSaveVerdict;
		if (bOk) { UE_LOG(LogBF6Portal, Display, TEXT("Portal settings save: %s"), *GSaveVerdict); }
		else     { UE_LOG(LogBF6Portal, Warning, TEXT("Portal settings save: %s"), *GSaveVerdict); }
		if (bOk)
		{
			// A save the site accepted clears what it carried, and leaves behind
			// anything that never made it onto the page.
			//
			// SCOPED TO THE EXPERIENCE THAT WAS SAVED. Saving experience B used
			// to drop every applied edit in the queue, experience A's included,
			// so A's draft silently reported itself as saved when it had never
			// been near the site.
			const FString SavedId = CurrentExperienceId();
			if (SavedId.IsEmpty())
			{
				UE_LOG(LogBF6Portal, Warning,
					TEXT("Portal settings: the save verdict arrived with no experience in the page url, so no queued change was cleared."));
			}
			else
			{
				GEdits.RemoveAll([&SavedId](const FEdit& E)
					{ return E.bApplied && E.Experience.Equals(SavedId, ESearchCase::IgnoreCase); });
			}
		}
		Bump();
	}
}

// ============================================================================
// The bridge object
// ============================================================================

void UBF6PortalSettingsBridge::report(FString Json)
{
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid())
	{
		UE_LOG(LogBF6Portal, Warning, TEXT("Portal settings: a page message did not parse (%d chars)"), Json.Len());
		return;
	}
	FString Kind;
	Root->TryGetStringField(TEXT("kind"), Kind);

	if (Kind == TEXT("watch"))         { HandleWatch(Root); return; }
	if (Kind == TEXT("page"))          { HandlePage(Root); return; }
	if (Kind == TEXT("mutators"))      { HandleMutators(Root); return; }
	if (Kind == TEXT("applied"))       { HandleApplied(Root); return; }
	if (Kind == TEXT("save"))          { HandleSave(Root); return; }
	if (Kind == TEXT("apply-done"))
	{
		int32 N = 0, Of = 0;
		Root->TryGetNumberField(TEXT("applied"), N);
		Root->TryGetNumberField(TEXT("of"), Of);
		UE_LOG(LogBF6Portal, Display, TEXT("Portal settings: %d of %d change(s) took on this page"), N, Of);
		ApplyNextPage();
		return;
	}
	if (Kind == TEXT("apply-error"))
	{
		FString Why; Root->TryGetStringField(TEXT("reason"), Why);
		GStatus = FString::Printf(TEXT("The page could not read the change list: %s"), *Why);
		UE_LOG(LogBF6Portal, Warning, TEXT("Portal settings apply: %s"), *Why);
		ApplyNextPage();
		return;
	}
	if (Kind == TEXT("map") || Kind == TEXT("restriction") || Kind == TEXT("restriction-bulk"))
	{
		bool bOk = true;
		FString Why;
		Root->TryGetBoolField(TEXT("ok"), bOk);
		Root->TryGetStringField(TEXT("reason"), Why);
		GStatus = bOk ? FString::Printf(TEXT("%s done on the page."), *Kind)
		              : FString::Printf(TEXT("%s did not go through: %s"), *Kind, *Why);
		if (!bOk) UE_LOG(LogBF6Portal, Warning, TEXT("Portal settings %s: %s"), *Kind, *Why);
		Bump();
		return;
	}
	if (Kind == TEXT("selftest"))
	{
		UE_LOG(LogBF6Portal, Display, TEXT("Portal settings selectors: %s"), *GSelectorLine);
		return;
	}
}

// ============================================================================
// 4. the tool's own widgets
// ============================================================================

namespace
{
	FSlateFontInfo FontBold(int32 Size) { return FCoreStyle::GetDefaultFontStyle("Bold", Size); }
	FSlateFontInfo FontReg(int32 Size)  { return FCoreStyle::GetDefaultFontStyle("Regular", Size); }
	const FSlateBrush* InkBrush()       { static FSlateColorBrush B(BF6Theme::Ink);        return &B; }
	const FSlateBrush* PanelBrush()     { static FSlateColorBrush B(BF6Theme::Panel);      return &B; }
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

	const FButtonStyle& AccentStyle()
	{
		static FButtonStyle S = FButtonStyle()
			.SetNormal (FSlateRoundedBoxBrush(BF6Theme::AccentDim, 0.f, BF6Theme::Accent, 1.f))
			.SetHovered(FSlateRoundedBoxBrush(BF6Theme::Accent, 0.f, FLinearColor::White, 1.f))
			.SetPressed(FSlateRoundedBoxBrush(BF6Theme::AccentDim, 0.f, FLinearColor::White, 1.f))
			.SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0, 1, 0, -1));
		return S;
	}

	TSharedRef<SWidget> Btn(const FString& Label, TFunction<void()> Fn, const FString& Tip = FString(), bool bAccent = false)
	{
		return SNew(SButton).ButtonStyle(bAccent ? &AccentStyle() : &GhostStyle()).ContentPadding(FMargin(9, 5))
			.ToolTipText(FText::FromString(Tip))
			.OnClicked_Lambda([Fn]{ if (Fn) Fn(); return FReply::Handled(); })
			[ SNew(STextBlock).Font(FontBold(9)).ColorAndOpacity(FSlateColor(bAccent ? FLinearColor::White : BF6Theme::Text))
				.Text(FText::FromString(Label.ToUpper())) ];
	}

	TSharedRef<SWidget> Line(const FString& Text, int32 Size, const FLinearColor& C, bool bBold = false)
	{
		return SNew(STextBlock).AutoWrapText(false).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			.Font(bBold ? FontBold(Size) : FontReg(Size)).ColorAndOpacity(FSlateColor(C))
			.Text(FText::FromString(Text));
	}
}

// The SETTINGS column: the tabs the site has, drawn from the catalogue, with
// the experience's own values in them.
class SBF6SettingsColumn : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6SettingsColumn) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		Tab = TEXT("MODE");
		SubPage = TEXT("mode");
		ChildSlot
		[
			SNew(SBorder).BorderImage(InkBrush()).Padding(FMargin(10, 8))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[ SAssignNew(Head, SBox) ]
				+ SVerticalBox::Slot().FillHeight(1.f).Padding(0, 6, 0, 0)
				[ SNew(SScrollBox) + SScrollBox::Slot()[ SAssignNew(Body, SBox) ] ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)[ SAssignNew(Foot, SBox) ]
			]
		];
		Rebuild();
	}

	virtual void Tick(const FGeometry& G, const double T, const float D) override
	{
		SCompoundWidget::Tick(G, T, D);
		if (GFingerprint != Sig) Rebuild();
	}

private:
	TSharedPtr<SBox> Head, Body, Foot;
	uint32  Sig = 0;
	FString Tab, SubPage, Filter;
	// The combo boxes' option arrays have to outlive the frame they are built in.
	TArray<TSharedPtr<TArray<TSharedPtr<FString>>>> ComboKeepAlive;

	static const TCHAR* TabsInOrder(int32 i)
	{
		static const TCHAR* T[] = { TEXT("MODE"), TEXT("MAP ROTATION"), TEXT("TEAMS"),
			TEXT("MODIFIERS"), TEXT("RESTRICTIONS"), TEXT("PUBLISH") };
		return (i >= 0 && i < 6) ? T[i] : nullptr;
	}

	TArray<FPage*> PagesInTab() const
	{
		TArray<FPage*> Out;
		for (FPage& P : GPages) if (P.Tab == Tab) Out.Add(&P);
		return Out;
	}

	void Rebuild()
	{
		if (!Head.IsValid() || !Body.IsValid() || !Foot.IsValid()) return;
		Sig = GFingerprint;
		ComboKeepAlive.Reset();

		// ---- head: the tabs, the sub-tabs and the filter ---------------------
		TSharedRef<SWrapBox> Tabs = SNew(SWrapBox).UseAllottedSize(true);
		for (int32 i = 0; TabsInOrder(i); i++)
		{
			const FString T = TabsInOrder(i);
			const bool bOn = (T == Tab);
			Tabs->AddSlot().Padding(0, 0, 4, 4)
			[ Btn(T, [this, T]{ Tab = T; TArray<FPage*> Ps = PagesInTab(); SubPage = Ps.Num() ? Ps[0]->Key : FString(); Bump(); },
				FString(), bOn) ];
		}

		TArray<FPage*> Ps = PagesInTab();
		TSharedRef<SWrapBox> Subs = SNew(SWrapBox).UseAllottedSize(true);
		if (Ps.Num() > 1)
			for (FPage* P : Ps)
			{
				const FString K = P->Key, Title = P->Title;
				const bool bOn = (K == SubPage);
				Subs->AddSlot().Padding(0, 0, 4, 4)
				[ Btn(Title, [this, K]{ SubPage = K; Bump(); }, FString(), bOn) ];
			}
		if (Ps.Num() && !PageFor(SubPage)) SubPage = Ps[0]->Key;

		// THE PANEL SHOWS ONE EXPERIENCE'S DRAFT: the one it is looking at.
		// Counting and listing every queued change regardless of destination is
		// what made it look as though experience B had A's changes waiting, which
		// is the state in which somebody presses APPLY.
		const FString PanelExperience = CurrentExperienceId();
		int32 Pending = 0;
		for (const FEdit& E : GEdits) if (E.Experience == PanelExperience) Pending++;
		Head->SetContent(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[ Line(TEXT("SETTINGS"), 11, BF6Theme::Accent, true) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 4)
			[ Line(GMuts.Num() ? FString::Printf(TEXT("%d live value(s), %d team(s)"), GMuts.Num(), GTeamCount)
			                   : FString(TEXT("No experience read yet. Open one on the site.")), 8, BF6Theme::TextDim) ]
			+ SVerticalBox::Slot().AutoHeight()[ Tabs ]
			+ SVerticalBox::Slot().AutoHeight()[ Subs ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 4, 0)
				[
					SNew(SEditableTextBox).Font(FontReg(9))
					.HintText(FText::FromString(TEXT("Filter")))
					.Text(FText::FromString(Filter))
					.OnTextChanged_Lambda([this](const FText& T){ Filter = T.ToString(); Bump(); })
					.OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type){ Filter = T.ToString(); Bump(); })
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[ Btn(TEXT("Refresh"), []{ BF6PortalSettings::Refresh(); },
					TEXT("Re-read the page the panel is on and ask the site for the experience's values again.")) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[ SNew(SBox).HeightOverride(1.f)[ SNew(SBorder).BorderImage(LineBrush()) ] ]
		);

		// ---- body ------------------------------------------------------------
		FPage* P = PageFor(SubPage);
		if (!P)
		{
			Body->SetContent(Line(TEXT("Nothing in the catalogue for this tab."), 9, BF6Theme::TextDim));
		}
		else if (P->Key == TEXT("map-rotation"))
		{
			Body->SetContent(MakeRotation(*P));
		}
		else if (P->Key.StartsWith(TEXT("restrict")))
		{
			Body->SetContent(MakeRestrictions(*P));
		}
		else if (P->Tab == TEXT("PUBLISH"))
		{
			Body->SetContent(MakePublish(*P));
		}
		else
		{
			Body->SetContent(MakeSettings(*P));
		}

		// ---- foot: the changes list, apply and save --------------------------
		TSharedRef<SVerticalBox> Changes = SNew(SVerticalBox);
		Changes->AddSlot().AutoHeight()
		[ Line(Pending ? FString::Printf(TEXT("CHANGES (%d)"), Pending) : FString(TEXT("NO CHANGES")), 9, BF6Theme::Accent, true) ];
		int32 Elsewhere = 0;
		for (const FEdit& E : GEdits)
		{
			if (E.Experience != PanelExperience) { Elsewhere++; continue; }
			const FString Who = E.Team < 0 ? E.Title : FString::Printf(TEXT("%s (team %d)"), *E.Title, E.Team + 1);
			const FString What = FString::Printf(TEXT("%s  ->  %s"), *Who, *(E.Label.IsEmpty() ? E.Value : E.Label));
			Changes->AddSlot().AutoHeight().Padding(0, 1, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[ Line(What, 8, E.bApplied ? BF6Theme::TextDim : BF6Theme::Text) ]
				+ SHorizontalBox::Slot().AutoWidth()
				[ Btn(TEXT("x"), [Id = E.TestId, Tm = E.Team]{ BF6PortalSettings::Reset(Id, Tm); },
					TEXT("Drop this change and go back to the site's value.")) ]
			];
			if (!E.Reason.IsEmpty())
				Changes->AddSlot().AutoHeight()[ Line(FString::Printf(TEXT("    %s"), *E.Reason), 8, FLinearColor(0.85f, 0.35f, 0.30f)) ];
		}
		if (Elsewhere > 0)
		{
			// Said, rather than hidden: a draft that is not on screen is still a
			// draft, and somebody who queued it deserves to know it is waiting.
			Changes->AddSlot().AutoHeight().Padding(0, 2, 0, 0)
			[ Line(FString::Printf(TEXT("%d change(s) are queued for another experience. Open it to see them."), Elsewhere),
				8, BF6Theme::TextDim) ];
		}

		Foot->SetContent(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ Changes ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[ Btn(TEXT("Apply on site"), []{ BF6PortalSettings::Apply(); },
					TEXT("Put the site on each page in turn and set the changed controls the way you would by hand."), true) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[ Btn(TEXT("Save on Portal"), []{ BF6PortalSettings::SaveOnPortal(); },
					TEXT("Press the site's own save button and report what it said. Nothing is composed by the tool.")) ]
				+ SHorizontalBox::Slot().AutoWidth()
				[ Btn(TEXT("Clear"), [PanelExperience]
					{
						// Only this experience's draft. Clearing the whole list
						// threw away a draft queued for an experience that was
						// not even on screen.
						if (PanelExperience.IsEmpty()) return;
						GEdits.RemoveAll([&PanelExperience](const FEdit& E){ return E.Experience == PanelExperience; });
						Bump();
					}, TEXT("Drop every pending change queued for the experience on screen. A draft for another experience is left alone.")) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[ SNew(STextBlock).AutoWrapText(true).Font(FontReg(8))
				.ColorAndOpacity(FSlateColor(BF6Theme::TextDim)).Text(FText::FromString(GStatus)) ]
		);
	}

	// ---- one settings page ---------------------------------------------------

	TSharedRef<SWidget> MakeSettings(FPage& P)
	{
		TSharedRef<SVerticalBox> V = SNew(SVerticalBox);
		int32 Shown = 0;
		for (FSetting& S : P.Settings)
		{
			if (!Filter.IsEmpty() && !S.Title.Contains(Filter) && !S.TestId.Contains(Filter)) continue;
			Shown++;
			V->AddSlot().AutoHeight().Padding(0, 3, 0, 0)[ MakeRow(P, S) ];
		}
		if (Shown == 0)
			V->AddSlot().AutoHeight()[ Line(TEXT("Nothing on this page matches the filter."), 9, BF6Theme::TextDim) ];
		return V;
	}

	TSharedRef<SWidget> MakeRow(FPage& P, FSetting& S)
	{
		const int32 Cols = S.bPerTeam ? FMath::Clamp(GTeamCount, 2, 8) : 1;
		TSharedRef<SHorizontalBox> Controls = SNew(SHorizontalBox);
		for (int32 c = 0; c < Cols; c++)
		{
			const int32 Team = S.bPerTeam ? c : -1;
			Controls->AddSlot().FillWidth(1.f).Padding(0, 0, c + 1 < Cols ? 4 : 0, 0)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[ SNew(SBox).Visibility(S.bPerTeam ? EVisibility::Visible : EVisibility::Collapsed)
					[ Line(FString::Printf(TEXT("TEAM %d"), c + 1), 7, BF6Theme::TextDim, true) ] ]
				+ SVerticalBox::Slot().AutoHeight()[ MakeControl(P, S, Team) ]
			];
		}

		bool bChanged = false, bFromSite = false;
		DisplayValue(S, S.bPerTeam ? 0 : -1, bChanged, bFromSite);
		const FString Mark = bChanged ? TEXT("* ") : TEXT("");

		return SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(6, 5))
			.ToolTipText(FText::FromString(S.Caption.IsEmpty()
				? FString::Printf(TEXT("%s\nkey: %s"), *S.Title, *S.TestId)
				: FString::Printf(TEXT("%s\n\nkey: %s"), *S.Caption, *S.TestId)))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[ Line(Mark + S.Title.ToUpper(), 9, bChanged ? BF6Theme::Accent : BF6Theme::TextBlue, true) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ SNew(SBox).Visibility(S.bLiveOnly ? EVisibility::Visible : EVisibility::Collapsed)
					[ Line(TEXT("NEW ON SITE"), 7, BF6Theme::Accent, true) ] ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)
				[ SNew(SBox).Visibility(bChanged ? EVisibility::Visible : EVisibility::Collapsed)
					[ Btn(TEXT("Reset"), [Id = S.TestId, bPT = S.bPerTeam, Cols]
						{
							for (int32 c = 0; c < (bPT ? Cols : 1); c++) BF6PortalSettings::Reset(Id, bPT ? c : -1);
						}, TEXT("Back to the value the site has.")) ] ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 3, 0, 0)[ Controls ]
		];
	}

	TSharedRef<SWidget> MakeControl(FPage& P, FSetting& S, int32 Team)
	{
		bool bChanged = false, bFromSite = false;
		const FString Now = DisplayValue(S, Team, bChanged, bFromSite);
		const FLinearColor C = bChanged ? BF6Theme::Accent : (bFromSite ? BF6Theme::Text : BF6Theme::TextDim);
		const FString Key = S.TestId;

		if (S.Kind == TEXT("toggle"))
		{
			const bool bOn = Now.Equals(TEXT("On"), ESearchCase::IgnoreCase) || Now == TEXT("1") || Now.Equals(TEXT("true"), ESearchCase::IgnoreCase);
			return SNew(SButton).ButtonStyle(&GhostStyle()).ContentPadding(FMargin(9, 5))
				.OnClicked_Lambda([Key, Team, bOn]
				{
					BF6PortalSettings::Set(Key, Team, bOn ? TEXT("off") : TEXT("on"));
					return FReply::Handled();
				})
				[ SNew(STextBlock).Font(FontBold(9)).ColorAndOpacity(FSlateColor(C))
					.Text(FText::FromString(bFromSite ? (bOn ? TEXT("ON") : TEXT("OFF")) : FString::Printf(TEXT("%s (default)"), bOn ? TEXT("ON") : TEXT("OFF")))) ];
		}

		if (S.Kind == TEXT("dropdown"))
		{
			TSharedRef<TArray<TSharedPtr<FString>>> Opts = MakeShared<TArray<TSharedPtr<FString>>>();
			for (const FOption& O : S.Options) Opts->Add(MakeShared<FString>(O.Label));
			if (Opts->Num() == 0) Opts->Add(MakeShared<FString>(Now));
			ComboKeepAlive.Add(Opts);
			TSharedPtr<FString> Sel;
			for (const TSharedPtr<FString>& O : *Opts) if (O.IsValid() && O->Equals(Now, ESearchCase::IgnoreCase)) { Sel = O; break; }
			return SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&Opts.Get())
				.InitiallySelectedItem(Sel)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> In)
					{ return SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(In.IsValid() ? *In : FString())); })
				.OnSelectionChanged_Lambda([Key, Team](TSharedPtr<FString> In, ESelectInfo::Type)
					{ if (In.IsValid()) BF6PortalSettings::Set(Key, Team, *In); })
				[ SNew(STextBlock).Font(FontReg(9)).ColorAndOpacity(FSlateColor(C))
					.Text(FText::FromString(bFromSite ? Now : FString::Printf(TEXT("%s (default)"), *Now))) ];
		}

		if (S.Kind == TEXT("text"))
		{
			return SNew(SEditableTextBox).Font(FontReg(9)).Text(FText::FromString(Now))
				.OnTextCommitted_Lambda([Key, Team](const FText& T, ETextCommit::Type How)
					{ if (How == ETextCommit::OnEnter || How == ETextCommit::OnUserMovedFocus) BF6PortalSettings::Set(Key, Team, T.ToString()); });
		}

		if (S.Kind == TEXT("thumbnail") || S.Kind == TEXT("action"))
		{
			return Line(Now, 9, C);
		}

		// slider: the bar and the number box beside it, the way the site draws it
		const float Min = S.bHasRange ? (float)S.Min : 0.f;
		const float Max = S.bHasRange ? (float)S.Max : 100.f;
		const float Val = (float)AsNumber(Now);
		const float Frac = (Max > Min) ? FMath::Clamp((Val - Min) / (Max - Min), 0.f, 1.f) : 0.f;
		const float Step = (float)S.Step;
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0, 0, 4, 0)
			[
				SNew(SSlider).Value(Frac)
				.SliderBarColor(FSlateColor(BF6Theme::PanelLight))
				.SliderHandleColor(FSlateColor(bChanged ? BF6Theme::Accent : BF6Theme::TextBlue))
				.OnValueChanged_Lambda([Key, Team, Min, Max, Step](float F)
				{
					float V = Min + F * (Max - Min);
					if (Step > 0.f) V = Min + FMath::RoundToFloat((V - Min) / Step) * Step;
					BF6PortalSettings::Set(Key, Team, FString::SanitizeFloat(V));
				})
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox).WidthOverride(64.f)
				[
					SNew(SEditableTextBox).Font(FontReg(9)).Text(FText::FromString(Now))
					.ForegroundColor(FSlateColor(C))
					.OnTextCommitted_Lambda([Key, Team](const FText& T, ETextCommit::Type How)
						{ if (How == ETextCommit::OnEnter) BF6PortalSettings::Set(Key, Team, T.ToString()); })
				]
			];
	}

	// ---- the map rotation ----------------------------------------------------

	TSharedRef<SWidget> MakeRotation(FPage& P)
	{
		TSharedRef<SVerticalBox> V = SNew(SVerticalBox);
		const FString Id = CurrentExperienceId();
		V->AddSlot().AutoHeight()[ Line(TEXT("THE ROTATION"), 9, BF6Theme::Accent, true) ];

		const TArray<BF6PortalProfile::FRotationRow> Rot = Id.IsEmpty()
			? TArray<BF6PortalProfile::FRotationRow>() : BF6PortalProfile::RotationFor(Id);
		if (Rot.Num() == 0)
			V->AddSlot().AutoHeight()[ Line(TEXT("No rotation read yet. Open the experience on the site."), 8, BF6Theme::TextDim) ];
		for (const BF6PortalProfile::FRotationRow& R : Rot)
		{
			const FString Map = R.Map;
			const int32 Idx = R.MapIdx;
			V->AddSlot().AutoHeight().Padding(0, 2, 0, 0)
			[
				SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(6, 4))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[ Line(FString::Printf(TEXT("%d.  %s%s"), Idx + 1, *Map, R.bHasSpatial ? TEXT("") : TEXT("  (no map data)")), 9, BF6Theme::Text) ]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0, 0, 0)
					[ Btn(TEXT("Open"), [Id, Idx]{ BF6PortalProfile::SwitchToMap(Id, Idx); },
						TEXT("Save what is open and open this slot's save instead.")) ]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0, 0, 0)
					[ Btn(TEXT("Remove"), [Map, Idx]{ BF6PortalSettings::Refresh(); RemoveMap(Map, Idx); },
						TEXT("Press the site's own remove button for this slot.")) ]
				]
			];
		}

		V->AddSlot().AutoHeight().Padding(0, 8, 0, 2)[ Line(TEXT("ADD A MAP"), 9, BF6Theme::Accent, true) ];
		TSharedRef<SWrapBox> Adds = SNew(SWrapBox).UseAllottedSize(true);
		for (const TPair<FString, FString>& M : P.Maps)
		{
			if (!Filter.IsEmpty() && !M.Key.Contains(Filter)) continue;
			const FString Key = FString::Printf(TEXT("%s:%s"), *M.Key, *M.Value);
			Adds->AddSlot().Padding(0, 0, 3, 3)
			[ Btn(M.Key, [Key]{ AddMap(Key); }, FString::Printf(TEXT("Press the site's own add button for %s."), *M.Key)) ];
		}
		V->AddSlot().AutoHeight()[ Adds ];
		V->AddSlot().AutoHeight().Padding(0, 6, 0, 0)
		[ SNew(STextBlock).AutoWrapText(true).Font(FontReg(8)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
			.Text(FText::FromString(TEXT("Adding, removing and reordering all press the site's own buttons on the map rotation page. The panel goes there first, so watch the page while it works."))) ];
		return V;
	}

	static void AddMap(const FString& MapKey)
	{
		FPage* P = PageFor(TEXT("map-rotation"));
		if (!P) return;
		const FString For = CurrentExperienceId();
		ExpectPage(TEXT("map-rotation"), UrlForPage(*P, For), 25.f, [MapKey, For](bool bOk, const FString& What)
		{
			if (!bOk) { GStatus = FString::Printf(TEXT("Could not get to the map rotation page (%s)."), *What); Bump(); return; }
			if (!StillOnExperience(For, TEXT("Adding that map"))) return;
			Call(FString::Printf(TEXT("window.BF6PortalSettings.mapAction('add', '%s', 0)"), *JsLit(MapKey)));
		});
	}

	static void RemoveMap(const FString& Map, int32 Idx)
	{
		FPage* P = PageFor(TEXT("map-rotation"));
		if (!P) return;
		// The site's remove button carries the map's size letter as well, so the
		// key is looked up in the catalogue rather than guessed.
		FString Key = Map;
		for (const TPair<FString, FString>& M : P->Maps) if (M.Key == Map) { Key = FString::Printf(TEXT("%s:%s"), *M.Key, *M.Value); break; }
		const FString For = CurrentExperienceId();
		ExpectPage(TEXT("map-rotation"), UrlForPage(*P, For), 25.f, [Key, Idx, For](bool bOk, const FString& What)
		{
			if (!bOk) { GStatus = FString::Printf(TEXT("Could not get to the map rotation page (%s)."), *What); Bump(); return; }
			if (!StillOnExperience(For, TEXT("Removing that map"))) return;
			Call(FString::Printf(TEXT("window.BF6PortalSettings.mapAction('remove', '%s', %d)"), *JsLit(Key), Idx));
		});
	}

	// ---- the restriction pages -----------------------------------------------

	TSharedRef<SWidget> MakeRestrictions(FPage& P)
	{
		TSharedRef<SVerticalBox> V = SNew(SVerticalBox);
		const FString PageKey = P.Key;
		const int32 Cols = FMath::Clamp(GTeamCount, 1, 8);
		for (const FCategory& C : P.Categories)
		{
			const FString Cat = C.Name;
			if (!Filter.IsEmpty() && !C.Name.Contains(Filter))
			{
				bool bAnyItem = false;
				for (const FString& I : C.Items) if (I.Contains(Filter)) { bAnyItem = true; break; }
				if (!bAnyItem) continue;
			}
			TSharedRef<SWrapBox> Bulk = SNew(SWrapBox).UseAllottedSize(true);
			for (int32 t = 0; t < Cols; t++)
			{
				const int32 Team = t;
				Bulk->AddSlot().Padding(0, 0, 3, 3)
				[ Btn(FString::Printf(TEXT("Team %d all"), t + 1), [PageKey, Cat, Team]{ SetCategory(PageKey, Cat, Team, true); },
					TEXT("Select every item in this category for this team, on the site's own page.")) ];
				Bulk->AddSlot().Padding(0, 0, 3, 3)
				[ Btn(FString::Printf(TEXT("Team %d none"), t + 1), [PageKey, Cat, Team]{ SetCategory(PageKey, Cat, Team, false); },
					TEXT("Clear every item in this category for this team, on the site's own page.")) ];
			}

			TSharedRef<SVerticalBox> Items = SNew(SVerticalBox);
			for (int32 i = 0; i < C.Items.Num(); i++)
			{
				const FString Item = C.Items[i];
				if (!Filter.IsEmpty() && !Item.Contains(Filter) && !C.Name.Contains(Filter)) continue;
				const int32 Index = i;
				TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
				Row->AddSlot().FillWidth(1.f).VAlign(VAlign_Center)[ Line(Item, 8, BF6Theme::Text) ];
				for (int32 t = 0; t < Cols; t++)
				{
					const int32 Team = t;
					Row->AddSlot().AutoWidth().Padding(2, 0, 0, 0)
					[ Btn(FString::Printf(TEXT("T%d on"), t + 1), [PageKey, Cat, Index, Team]{ SetItem(PageKey, Cat, Index, Team, true); }) ];
					Row->AddSlot().AutoWidth().Padding(2, 0, 0, 0)
					[ Btn(FString::Printf(TEXT("T%d off"), t + 1), [PageKey, Cat, Index, Team]{ SetItem(PageKey, Cat, Index, Team, false); }) ];
				}
				Items->AddSlot().AutoHeight().Padding(0, 1, 0, 0)[ Row ];
			}

			V->AddSlot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(6, 5))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[ Line(FString::Printf(TEXT("%s  %d/%d"), *C.Name.ToUpper(), C.Count, C.Items.Num()), 9, BF6Theme::TextBlue, true) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 3, 0, 0)[ Bulk ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)[ Items ]
				]
			];
		}
		if (P.Categories.Num() == 0)
			V->AddSlot().AutoHeight()[ Line(TEXT("No categories in the catalogue for this page."), 9, BF6Theme::TextDim) ];
		return V;
	}

	static void SetItem(const FString& PageKey, const FString& Cat, int32 Index, int32 Team, bool bOn)
	{
		FPage* P = PageFor(PageKey);
		if (!P) return;
		const FString For = CurrentExperienceId();
		ExpectPage(PageKey, UrlForPage(*P, For), 25.f, [Cat, Index, Team, bOn, For](bool bOk, const FString& What)
		{
			if (!bOk) { GStatus = FString::Printf(TEXT("Could not get to that restrictions page (%s)."), *What); Bump(); return; }
			if (!StillOnExperience(For, TEXT("That restriction change"))) return;
			Call(FString::Printf(TEXT("window.BF6PortalSettings.setRestriction('%s', %d, %d, %s)"),
				*JsLit(Cat), Index, Team, bOn ? TEXT("true") : TEXT("false")));
		});
	}

	static void SetCategory(const FString& PageKey, const FString& Cat, int32 Team, bool bOn)
	{
		FPage* P = PageFor(PageKey);
		if (!P) return;
		const FString For = CurrentExperienceId();
		ExpectPage(PageKey, UrlForPage(*P, For), 25.f, [Cat, Team, bOn, For](bool bOk, const FString& What)
		{
			if (!bOk) { GStatus = FString::Printf(TEXT("Could not get to that restrictions page (%s)."), *What); Bump(); return; }
			if (!StillOnExperience(For, TEXT("That restriction change"))) return;
			Call(FString::Printf(TEXT("window.BF6PortalSettings.setCategory('%s', %d, %s)"),
				*JsLit(Cat), Team, bOn ? TEXT("true") : TEXT("false")));
		});
	}

	// ---- publish -------------------------------------------------------------

	TSharedRef<SWidget> MakePublish(FPage& P)
	{
		TSharedRef<SVerticalBox> V = SNew(SVerticalBox);
		V->AddSlot().AutoHeight()[ Line(P.Title, 9, BF6Theme::Accent, true) ];
		for (FSetting& S : P.Fields)
		{
			if (S.Kind == TEXT("thumbnail"))
			{
				V->AddSlot().AutoHeight().Padding(0, 4, 0, 0)
				[ SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(4))[ BF6PortalProfile::MakeThumbnailPanel() ] ];
				continue;
			}
			if (S.Kind == TEXT("action"))
			{
				V->AddSlot().AutoHeight().Padding(0, 6, 0, 0)
				[ SNew(STextBlock).AutoWrapText(true).Font(FontReg(8)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
					.Text(FText::FromString(TEXT("Publishing is your click, on the site's own button. The panel will not press it."))) ];
				continue;
			}
			V->AddSlot().AutoHeight().Padding(0, 3, 0, 0)[ MakeRow(P, S) ];
		}
		const FString Key = P.Key;
		V->AddSlot().AutoHeight().Padding(0, 8, 0, 0)
		[ Btn(TEXT("Go to this step"), [Key]
			{
				FPage* Pg = PageFor(Key);
				if (Pg) BF6PortalWeb::Open(UrlForPage(*Pg, CurrentExperienceId()));
			}, TEXT("Put the site on this publish step.")) ];
		return V;
	}
};

// ============================================================================
// 5. the namespace surface, console commands and module hooks
// ============================================================================

TSharedRef<SWidget> BF6PortalSettings::MakeColumn()
{
	return SNew(SBF6SettingsColumn);
}

void BF6PortalSettings::Refresh()
{
	Call(TEXT("window.BF6PortalSettings.scrape()"));
	Call(TEXT("window.BF6PortalSettings.selftest()"));
	// The experience's own values come out of a getPlayElement the page makes.
	// The profile already knows how to ask for one without a credential of ours.
	const FString Id = CurrentExperienceId();
	if (!Id.IsEmpty())
		BF6PortalWeb::Exec(FString::Printf(
			TEXT("try { window.BF6PortalCapture.getPlayElement('%s'); } catch (e) {}"), *Id));
	GStatus = TEXT("Asked the page for a fresh read.");
	Bump();
}

bool BF6PortalSettings::Set(const FString& TestId, int32 Team, const FString& Value)
{
	// A CHANGE WITHOUT A DESTINATION CANNOT BE QUEUED.
	//
	// Every check further down compares an edit against the experience it was
	// queued for. An edit queued with no experience would satisfy none of them
	// and could never be applied anywhere, so the honest answer is to refuse it
	// here with the reason, rather than to park it in the list looking valid.
	const FString ExperienceId = CurrentExperienceId();
	if (ExperienceId.IsEmpty())
	{
		UE_LOG(LogBF6Portal, Warning,
			TEXT("Portal settings: refused to queue %s. The Portal panel is not on an experience, so there is no destination for it."), *TestId);
		GStatus = TEXT("Open the experience you want to change on the site first. A change is queued for one experience and the panel is not on one.");
		Bump();
		return false;
	}

	FPage* P = nullptr;
	FSetting* S = SettingFor(TestId, &P);
	if (!S || !P)
	{
		UE_LOG(LogBF6Portal, Warning, TEXT("Portal settings: nothing called %s in the catalogue or on the page"), *TestId);
		GStatus = FString::Printf(TEXT("No setting called %s."), *TestId);
		Bump();
		return false;
	}
	if (!S->bPerTeam) Team = -1;

	FString Val = Value.TrimStartAndEnd();
	FString Label;
	if (S->Kind == TEXT("dropdown"))
	{
		// The site is driven by the option's LABEL, because that is what a user
		// clicks. A value is accepted too and turned into its label.
		for (const FOption& O : S->Options)
			if (O.Label.Equals(Val, ESearchCase::IgnoreCase)) { Label = O.Label; break; }
		if (Label.IsEmpty())
			for (const FOption& O : S->Options)
				if (FMath::IsNearlyEqual(O.Value, FCString::Atod(*Val))) { Label = O.Label; break; }
		if (Label.IsEmpty() && S->Options.Num())
		{
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal settings: %s has no option called %s"), *TestId, *Val);
			GStatus = FString::Printf(TEXT("%s has no option called %s."), *S->Title, *Val);
			Bump();
			return false;
		}
		if (Label.IsEmpty()) Label = Val;
	}
	else if (S->Kind == TEXT("slider") && S->bHasRange)
	{
		const double N = FMath::Clamp(AsNumber(Val), S->Min, S->Max);
		Val = FMath::IsNearlyEqual(N, FMath::RoundToDouble(N)) ? FString::FromInt((int32)FMath::RoundToDouble(N)) : FString::SanitizeFloat(N);
	}
	else if (S->Kind == TEXT("toggle"))
	{
		Val = AsNumber(Val) != 0.0 ? TEXT("on") : TEXT("off");
	}

	FEdit* E = EditFor(ExperienceId, TestId, Team);
	if (!E)
	{
		E = &GEdits.AddDefaulted_GetRef();
		E->Experience = ExperienceId;
		E->TestId = TestId;
		E->Team = Team;
	}
	E->PageKey = P->Key;
	E->Kind = S->Kind;
	E->Title = S->Title;
	E->Value = Val;
	E->Label = Label;
	E->bApplied = false;
	E->Reason.Reset();
	GStatus = FString::Printf(TEXT("%s queued for experience %s. APPLY ON SITE sets it on the page."), *S->Title, *ExperienceId);
	Bump();
	return true;
}

void BF6PortalSettings::Reset(const FString& TestId, int32 Team)
{
	// Dropping a change drops it for the experience the panel is showing, so
	// pressing x beside one experience's change cannot delete the identically
	// named change queued for another.
	const FString ExperienceId = CurrentExperienceId();
	if (ExperienceId.IsEmpty()) return;
	const int32 Removed = GEdits.RemoveAll([&](const FEdit& E)
		{ return E.Experience == ExperienceId && E.TestId == TestId && E.Team == Team; });
	if (Removed) { GStatus = FString::Printf(TEXT("%s is back to the site's value."), *TestId); Bump(); }
}

void BF6PortalSettings::Apply()
{
	if (GApplying) { GStatus = TEXT("Already applying. Watch the page."); Bump(); return; }

	// AN APPLY RUN IS FOR THE EXPERIENCE THE PANEL IS ON, AND ONLY THAT ONE.
	//
	// The run used to take every queued change regardless of where it came from
	// and navigate using whatever experience the browser was on, so a draft for
	// A applied onto B without a word. The destination is read once, here, and
	// carried for the whole run; changes belonging to any other experience are
	// left exactly where they are.
	const FString ExperienceId = CurrentExperienceId();
	if (ExperienceId.IsEmpty())
	{
		GStatus = TEXT("The Portal panel is not on an experience. Open the one you queued these changes for, then press APPLY ON SITE.");
		UE_LOG(LogBF6Portal, Warning, TEXT("Portal settings: apply refused, the panel is not on an experience."));
		Bump();
		return;
	}

	GApplyQueue.Reset();
	GApplyOk = 0; GApplyFailed = 0;
	int32 Elsewhere = 0;
	for (const FEdit& E : GEdits) if (E.Experience != ExperienceId && !E.bApplied) Elsewhere++;

	for (const FPage& P : GPages)
	{
		bool bAny = false;
		for (const FEdit& E : GEdits)
			if (E.Experience == ExperienceId && E.PageKey == P.Key && !E.bApplied) { bAny = true; break; }
		if (bAny) GApplyQueue.Add(P.Key);
	}
	if (GApplyQueue.Num() == 0)
	{
		GStatus = Elsewhere > 0
			? FString::Printf(TEXT("Nothing to apply to experience %s. %d change(s) are queued for a different experience and were left alone."),
				*ExperienceId, Elsewhere)
			: FString(TEXT("Nothing to apply."));
		Bump();
		return;
	}
	GApplying = true;
	GApplyExperience = ExperienceId;
	UE_LOG(LogBF6Portal, Display,
		TEXT("Portal settings: applying changes to experience %s across %d page(s); %d change(s) for other experiences left alone"),
		*ExperienceId, GApplyQueue.Num(), Elsewhere);
	ApplyNextPage();
}

void BF6PortalSettings::SaveOnPortal()
{
	GSaveVerdict.Reset();
	GStatus = TEXT("Pressing the site's own save...");
	Bump();
	Call(TEXT("window.BF6PortalSettings.save()"));
}

// The live values, in the site's own export shape. A mutator with per-team
// values is an array ordered by team number, which is how the site writes it;
// one with a single value is a bare number. A mutator that was read with
// neither is left out rather than written as a guess.
bool BF6PortalSettings::ExperienceMutators(const FString& ExperienceId, TSharedPtr<FJsonObject>& Out)
{
	if (GMuts.Num() == 0) return false;
	if (!ExperienceId.IsEmpty() && !GMutExpId.IsEmpty() && GMutExpId != ExperienceId) return false;

	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	for (const TPair<FString, FMut>& Pair : GMuts)
	{
		const FMut& M = Pair.Value;
		if (M.Teams.Num() > 0)
		{
			// A PAIR per team, [team, value], which is what a real export holds:
			//   "FactionID_PerTeam": [[0, 607944106], [1, -1865993703]]
			// A bare array of the values reads back as team 0 and team 1 having
			// the values 607944106 and -1865993703 in some other order, which is
			// exactly the kind of silent corruption an import cannot notice.
			TArray<int32> Ids;
			M.Teams.GetKeys(Ids);
			Ids.Sort();
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const int32 Id : Ids)
			{
				TArray<TSharedPtr<FJsonValue>> PairOut;
				PairOut.Add(MakeShared<FJsonValueNumber>((double)Id));
				PairOut.Add(MakeShared<FJsonValueNumber>(M.Teams[Id]));
				Values.Add(MakeShared<FJsonValueArray>(PairOut));
			}
			O->SetArrayField(Pair.Key, Values);
		}
		else if (M.bHasDefault)
		{
			O->SetNumberField(Pair.Key, M.Default);
		}
	}
	if (O->Values.Num() == 0) return false;
	Out = O;
	return true;
}

bool BF6PortalSettings::ExperienceTeams(const FString& ExperienceId, TArray<TSharedPtr<FJsonValue>>& Out)
{
	if (GTeams.Num() == 0) return false;
	if (!ExperienceId.IsEmpty() && !GMutExpId.IsEmpty() && GMutExpId != ExperienceId) return false;
	Out = GTeams;
	return true;
}

bool BF6PortalSettings::ExperienceRestrictions(const FString& ExperienceId, TSharedPtr<FJsonObject>& Out)
{
	if (!GRestrictions.IsValid()) return false;
	if (!ExperienceId.IsEmpty() && !GMutExpId.IsEmpty() && GMutExpId != ExperienceId) return false;
	Out = GRestrictions;
	return true;
}

// The settings half of a whole-experience export, with no network at all.
//
// The site writes a per-team mutator as an array of [team, value] pairs and a
// single-valued one as a bare number, so both are read here and nothing else
// is: a shape that is neither is skipped and counted, rather than guessed at.
int32 BF6PortalSettings::LoadExperienceJson(const FString& ExperienceId, const TSharedPtr<FJsonObject>& Root)
{
	if (!Root.IsValid()) return 0;

	int32 Taken = 0, Skipped = 0;
	const TSharedPtr<FJsonObject>* Muts = nullptr;
	if (Root->TryGetObjectField(TEXT("mutators"), Muts) && Muts && Muts->IsValid())
	{
		GMuts.Reset();
		GTeamCount = 2;
		for (const auto& Pair : (*Muts)->Values)
		{
			const TSharedPtr<FJsonValue>& V = Pair.Value;
			if (!V.IsValid()) continue;
			FMut M;
			if (V->Type == EJson::Number)
			{
				M.bHasDefault = true;
				M.Default = V->AsNumber();
			}
			else if (V->Type == EJson::Boolean)
			{
				M.bHasDefault = true;
				M.Default = V->AsBool() ? 1.0 : 0.0;
			}
			else if (V->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& Arr = V->AsArray();
				bool bPairs = Arr.Num() > 0;
				for (const TSharedPtr<FJsonValue>& E : Arr)
					if (!E.IsValid() || E->Type != EJson::Array || E->AsArray().Num() != 2) { bPairs = false; break; }
				if (!bPairs) { Skipped++; continue; }
				for (const TSharedPtr<FJsonValue>& E : Arr)
				{
					const TArray<TSharedPtr<FJsonValue>>& P = E->AsArray();
					const int32 Team = (int32)P[0]->AsNumber();
					const double Val = P[1]->Type == EJson::Boolean ? (P[1]->AsBool() ? 1.0 : 0.0) : P[1]->AsNumber();
					M.Teams.Add(Team, Val);
					// Mutator teams are numbered from zero; the team composition
					// numbers them from one, so the count is one past the top.
					if (Team + 1 > GTeamCount && Team < 8) GTeamCount = Team + 1;
				}
			}
			else { Skipped++; continue; }
			GMuts.Add(FString(*Pair.Key), MoveTemp(M));
			Taken++;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Teams = nullptr;
	if (Root->TryGetArrayField(TEXT("teamComposition"), Teams) && Teams)
	{
		GTeams = *Teams;
		if (GTeams.Num() > 0 && GTeams.Num() <= 8) GTeamCount = GTeams.Num();
	}

	const TSharedPtr<FJsonObject>* Restrict = nullptr;
	if (Root->TryGetObjectField(TEXT("assetRestrictions"), Restrict) && Restrict && Restrict->IsValid())
	{
		GRestrictions = *Restrict;
	}

	GMutExpId = ExperienceId;
	GStatus = FString::Printf(TEXT("Read %d value(s), %d team(s) and %d restriction group(s) from a file."),
		GMuts.Num(), GTeams.Num() > 0 ? GTeams.Num() : GTeamCount,
		GRestrictions.IsValid() ? GRestrictions->Values.Num() : 0);
	UE_LOG(LogBF6Portal, Display,
		TEXT("Portal settings: %d mutator(s) taken from the file, %d skipped as an unknown shape, %d team(s), %d restriction group(s)"),
		Taken, Skipped, GTeams.Num(), GRestrictions.IsValid() ? GRestrictions->Values.Num() : 0);
	Bump();
	return Taken;
}

void BF6PortalSettings::SetWatching(bool bOn)
{
	GWatchOn = bOn;
	BF6PortalWeb::Exec(bOn
		? TEXT("try { window.BF6PortalSettings.startWatch(); } catch (e) {}")
		: TEXT("try { window.BF6PortalSettings.stopWatch(); } catch (e) {}"));
	if (!bOn) GWatchLine = TEXT("not watching");
	Bump();
}

FString BF6PortalSettings::WatchStatus()
{
	return FString::Printf(TEXT("%s (%d burst(s), %d control change(s) seen)"),
		GWatchLine.IsEmpty() ? TEXT("not watching") : *GWatchLine, GWatchBursts, GWatchControls);
}

void BF6PortalSettings::LogStatus()
{
	int32 N = 0;
	for (const FPage& P : GPages) N += P.Settings.Num() + P.Fields.Num();
	UE_LOG(LogBF6Portal, Display, TEXT("---- Portal settings ----"));
	UE_LOG(LogBF6Portal, Display, TEXT("catalogue: %d page(s), %d setting(s)"), GPages.Num(), N);
	for (const FPage& P : GPages)
		UE_LOG(LogBF6Portal, Display, TEXT("  %-14s %-14s %3d setting(s) %2d categor(y/ies) %2d map(s)"),
			*P.Key, *P.Tab, P.Settings.Num(), P.Categories.Num(), P.Maps.Num());
	UE_LOG(LogBF6Portal, Display, TEXT("page: %s   experience: %s"),
		GLiveSection.IsEmpty() ? TEXT("not a settings page") : *GLiveSection,
		GMutExpId.IsEmpty() ? TEXT("none read") : *GMutExpId);
	UE_LOG(LogBF6Portal, Display, TEXT("live values: %d mutator(s), %d team(s)"), GMuts.Num(), GTeamCount);
	UE_LOG(LogBF6Portal, Display, TEXT("selectors: %s"), GSelectorLine.IsEmpty() ? TEXT("no page scraped yet") : *GSelectorLine);
	UE_LOG(LogBF6Portal, Display, TEXT("panel is on experience: %s"),
		CurrentExperienceId().IsEmpty() ? TEXT("none") : *CurrentExperienceId());
	UE_LOG(LogBF6Portal, Display, TEXT("apply in flight: %s"),
		GApplying ? *FString::Printf(TEXT("yes, for %s"), *GApplyExperience) : TEXT("no"));
	// The experience is printed with every change, because "which experience is
	// this queued for" is the only question that matters about a pending change.
	UE_LOG(LogBF6Portal, Display, TEXT("pending changes: %d"), GEdits.Num());
	for (const FEdit& E : GEdits)
		UE_LOG(LogBF6Portal, Display, TEXT("  [%s] %-34s team %-2d -> %-24s %s"),
			*E.Experience, *E.TestId, E.Team + 1, *(E.Label.IsEmpty() ? E.Value : E.Label),
			E.bApplied ? TEXT("(on the page)") : (E.Reason.IsEmpty() ? TEXT("(waiting)") : *E.Reason));
	if (!GSaveVerdict.IsEmpty()) UE_LOG(LogBF6Portal, Display, TEXT("last save: %s"), *GSaveVerdict);
	UE_LOG(LogBF6Portal, Display, TEXT("status: %s"), *GStatus);
}

namespace
{
	TSharedRef<SDockTab> SpawnSettingsTab(const FSpawnTabArgs&)
	{
		return SNew(SDockTab).TabRole(ETabRole::NomadTab)
			[ BF6PortalSettings::MakeColumn() ];
	}
}

void BF6PortalSettings::Register()
{
	LoadCatalogue();

	GBridge.Reset(NewObject<UBF6PortalSettingsBridge>(GetTransientPackage(), FName(TEXT("BF6PortalSettingsBridge"))));
	BF6PortalWeb::RegisterBridgeObject(kBridgeName, GBridge.Get());

	// The page half, off disk, so the selectors can be iterated on without a
	// recompile the way capture.js already is.
	const FString JsPath = FPaths::Combine(g_pluginDir, TEXT("Resources"), TEXT("portal"), TEXT("settings.js"));
	FString Js;
	if (FFileHelper::LoadFileToString(Js, *JsPath))
	{
		BF6PortalWeb::RegisterInjectedScript(kScriptId, Js);
		UE_LOG(LogBF6Portal, Display, TEXT("Portal settings script injected from %s (%d chars)"), *JsPath, Js.Len());
	}
	else
	{
		UE_LOG(LogBF6Portal, Error, TEXT("Portal settings script missing: %s. The panel can show the catalogue but cannot read or set the page."), *JsPath);
	}

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(kTabId, FOnSpawnTab::CreateStatic(&SpawnSettingsTab))
		.SetDisplayName(FText::FromString(TEXT("BF6 Portal Settings")))
		.SetTooltipText(FText::FromString(TEXT("Everything the Portal site's experience editor can set, in the tool's own UI.")))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

	GUrlHandle = BF6PortalWeb::OnUrlChanged().AddLambda([](const FString&)
	{
		// The site is a single-page app, so a section change is a router move
		// with no load. Ask for a fresh read rather than waiting for the timer.
		Call(TEXT("window.BF6PortalSettings.scrape()"));
		// A navigation replaces the document, and the observer with it, so the
		// watch is re-armed on the new one rather than quietly stopping.
		if (GWatchOn) BF6PortalWeb::Exec(TEXT("try { window.BF6PortalSettings.startWatch(); } catch (e) {}"));
	});

	GTick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
	{
		if (GExpect.bActive && FPlatformTime::Seconds() > GExpect.Deadline)
		{
			const FString What = FString::Printf(TEXT("expected %s got %s"), *GExpect.Key,
				GLiveSection.IsEmpty() ? TEXT("no settings page") : *GLiveSection);
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal settings page check timed out: %s"), *What);
			ResolveExpect(false, What);
		}
		return true;
	}), 0.3f);

	IConsoleManager& CM = IConsoleManager::Get();
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Settings"),
		TEXT("Print the Portal settings panel's state: the catalogue, the page it last read, the experience's live values, the selectors and the pending changes."),
		FConsoleCommandDelegate::CreateLambda([]{ BF6PortalSettings::LogStatus(); })));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Settings.Refresh"),
		TEXT("Re-read the settings page the panel is on and ask the site for the experience's values again."),
		FConsoleCommandDelegate::CreateLambda([]{ BF6PortalSettings::Refresh(); })));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Settings.Set"),
		TEXT("Queue one change: BF6.Portal.Settings.Set <testId> [team] <value>. Team is 1 or 2 (or higher) and is left out for a setting that is not per team. Value is on/off, a number, or a dropdown option's label."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
		{
			if (A.Num() < 2)
			{
				UE_LOG(LogBF6Portal, Warning, TEXT("BF6.Portal.Settings.Set <testId> [team] <value>"));
				return;
			}
			const FString Id = A[0];
			int32 Team = -1;
			int32 First = 1;
			// A bare integer in the second slot is the team, unless it is the only
			// thing left - then it is the value.
			if (A.Num() >= 3 && A[1].IsNumeric() && !A[1].Contains(TEXT(".")))
			{
				Team = FCString::Atoi(*A[1]) - 1;
				First = 2;
			}
			TArray<FString> Rest;
			for (int32 i = First; i < A.Num(); i++) Rest.Add(A[i]);
			BF6PortalSettings::Set(Id, Team, FString::Join(Rest, TEXT(" ")).TrimQuotes());
		})));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Settings.Apply"),
		TEXT("Put the site on each page in turn and set every queued change there the way you would by hand, then report what took."),
		FConsoleCommandDelegate::CreateLambda([]{ BF6PortalSettings::Apply(); })));
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Settings.Save"),
		TEXT("Press the site's own save button on the page the panel is on and report the verdict."),
		FConsoleCommandDelegate::CreateLambda([]{ BF6PortalSettings::SaveOnPortal(); })));

	int32 N = 0;
	for (const FPage& P : GPages) N += P.Settings.Num() + P.Fields.Num();
	UE_LOG(LogBF6Portal, Display, TEXT("Portal settings ready. %d page(s), %d setting(s). BF6.Portal.Settings prints the rest."),
		GPages.Num(), N);
}

void BF6PortalSettings::Unregister()
{
	for (IConsoleObject* C : GCmds) if (C) IConsoleManager::Get().UnregisterConsoleObject(C);
	GCmds.Reset();
	if (GTick.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(GTick); GTick.Reset(); }
	BF6PortalWeb::OnUrlChanged().Remove(GUrlHandle);
	BF6PortalWeb::UnregisterBridgeObject(kBridgeName);
	GBridge.Reset();
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(kTabId);
	GExpect.bActive = false;
	GExpect.Done = nullptr;
	GApplying = false;
	GApplyQueue.Reset();
	GApplyExperience.Reset();
	GEdits.Reset();
	GPages.Reset();
	GMuts.Reset();
	GTeams.Reset();
	GRestrictions.Reset();
	GLive.Reset();
	GLoggedSelectors.Reset();
}
