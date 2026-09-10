#include "BF6PortalWeb.h"
#include "BF6PortalProfile.h"   // ---- BF6PortalProfile ----
#include "BF6PortalSettings.h"   // ---- BF6PortalSettings ----
#include "BF6BuildMode.h"
#include "BF6Internal.h"
#include "BF6Theme.h"
#include "BF6SDKExtension.h"

#include "SWebBrowser.h"
#include "IWebBrowserWindow.h"
#include "IWebBrowserSingleton.h"
#include "IWebBrowserCookieManager.h"
#include "WebBrowserModule.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Internationalization/Regex.h"
#include "Layout/WidgetPath.h"
#include "LevelEditor.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SLevelViewport.h"
#include "UObject/WeakObjectPtr.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

// ============================================================================
// See BF6PortalWeb.h for what this is. Layout of this file:
//   1. state and config
//   2. the browser window (one, ever) and the panel widget around it
//   3. placements: dock tab and viewport overlay
//   4. the Portal actions: experiences, linked experience, sign out, export path
//   5. console commands and the cookie self-test
//   6. module hooks and the input-handler seam
// ============================================================================

DEFINE_LOG_CATEGORY(LogBF6Portal);

// Declared HERE, at global scope, on purpose. The state block below holds a
// TSharedPtr to it, and inside an anonymous namespace an unseen class name
// would be declared as a DIFFERENT, anonymous-namespace type - which then
// never matches the real class defined further down, and SNew fails on an
// incomplete type. One forward declaration up here and both refer to one class.
class SBF6PortalPanel;

namespace
{
	// ---- 1. state ----------------------------------------------------------

	// ---- BF6PortalProfile ---- SignIn is the third placement: the panel over
	// the whole editor window while someone signs in. It is never remembered as
	// the user's placement, because nobody chooses it.
	// Offscreen is a REAL placement, not an absence of one: the panel is in the
	// Slate tree, ticking, one pixel wide and clipped, so the page loads and
	// the page->tool bridge runs while nothing is on screen. See AttachOffscreen.
	enum class EPlace : uint8 { Hidden, Overlay, Docked, SignIn, Offscreen };

	const TCHAR* PlaceName(EPlace P)
	{
		switch (P)
		{
		case EPlace::Overlay: return TEXT("viewport column");
		case EPlace::Docked:  return TEXT("dock tab");
		case EPlace::SignIn:  return TEXT("sign-in surface");
		case EPlace::Offscreen: return TEXT("offscreen (working, nothing on screen)");
		default:              return TEXT("hidden");
		}
	}

	const FName   kTabId(TEXT("BF6Portal"));
	const FString kContextId(TEXT("BF6Portal"));
	const TCHAR*  kIniSection = TEXT("BF6UnrealSDK");
	const TCHAR*  kDefaultBase = TEXT("https://portal.battlefield.com/bf6");
	// Served from memory by the browser itself (LoadString), never fetched:
	// an https origin of our own so the cookie self-test writes a real,
	// secure, persistent cookie into the same store the site uses.
	const TCHAR*  kSelfTestUrl = TEXT("https://selftest.bf6unrealsdk.invalid/cookie");

	EPlace  GPlace     = EPlace::Hidden;
	// Where OPEN puts it, remembered in config. The DOCK TAB is the default
	// because it is ordinary editor furniture: it can be resized, torn off to a
	// second monitor, and left open beside the viewport while the map is built.
	// The overlay is the same page in a column over the viewport, for people
	// who work full-screen in build mode; it is one button away, never the
	// first thing anyone meets.
	EPlace  GLastPlace = EPlace::Docked;

	TSharedPtr<IWebBrowserWindow> GWindow;   // THE browser window; both placements show it
	TSharedPtr<SWebBrowser>       GBrowser;  // the Slate view around GWindow
	TSharedPtr<SBF6PortalPanel>   GPanel;

	TSharedPtr<SWidget>        GOverlayRoot;     // full viewport, hit-test invisible
	TSharedPtr<SWidget>        GOverlayColumn;   // the right-hand column that IS hit-testable
	TSharedPtr<SBox>           GOverlayHost;     // where GPanel sits while overlaid
	TWeakPtr<SLevelViewport>   GOverlayVP;
	TSharedPtr<SBox>           GDockHost;        // where GPanel sits while docked
	TWeakPtr<SDockTab>         GDockTab;
	TSharedPtr<SWidget>        GOffRoot;         // the one-pixel host, see AttachOffscreen
	TSharedPtr<SBox>           GOffHost;
	// The box that is supposed to be one pixel. Kept so Status can report the
	// size it was ACTUALLY given rather than the size it asked for; those two
	// disagreeing is what hid this bug for so long.
	TSharedPtr<SBox>           GOffClip;
	TWeakPtr<SLevelViewport>   GOffVP;
	// How many pieces of work want the page kept alive. HIDE while one is in
	// flight drops to offscreen instead of tearing the panel down.
	int32                      GQuietHold = 0;
	// True until the editor has finished restoring its saved tab layout. A tab
	// spawned during that window was asked for by the layout, not by the user.
	bool                       GStartupGrace = true;

	// ---- the sign-in surface ------------------------------------------------
	// One widget in the EDITOR WINDOW's own overlay, not the level viewport's:
	// the viewport is a part of the window, and the map screen, the build HUD
	// and every editor panel are drawn inside the same window around it. This
	// is the only host that can cover the tool from either screen.
	TSharedPtr<SWidget>        GSignInRoot;      // fills the window under its title bar
	TSharedPtr<SWidget>        GSignInSheet;     // the opaque part, and what the hit test asks about
	TSharedPtr<SBox>           GSignInHost;      // where GPanel sits while signing in
	TWeakPtr<SWindow>          GSignInWindow;
	// Where the panel was when the surface went up, so it goes back there.
	// Hidden means it was not up at all, and then it does not come back: a
	// panel nobody opened has no business reappearing as a dock tab beside a
	// map screen the moment someone finishes signing in.
	EPlace                     GBeforeSignIn = EPlace::Hidden;
	// The frame on which Open() raised the panel from nothing, so the sign-in
	// surface can tell "the user had it up" from "the button that starts the
	// sign-in opened it half a line of code ago".
	uint64                     GOpenRaisedAtFrame = 0;

	TArray<IConsoleObject*>    GCmds;
	FDelegateHandle            GMapOpenedHandle, GMapClosingHandle;

	FString GBaseUrl   = kDefaultBase;
	FString GUserAgent;                      // optional header override, off by default
	float   GOverlayWidth = 560.f;
	FString GUrl, GTitle, GStatus, GLastExport;
	bool    GLoading = false;

	// Level|Save -> experience URL. Read from the session file on first ask,
	// dropped when a map opens or closes so a deleted-and-remade save cannot
	// inherit a stale link.
	TMap<FString, FString> GExpCache;
	// ---- BF6PortalProfile ----
	// Level|Save -> which slot of the experience's map rotation this save is,
	// or -1. Same lifetime and the same reset points as GExpCache above.
	TMap<FString, int32> GMapIdxCache;
	// ---- end BF6PortalProfile ----

	// ---- the script bridge seam --------------------------------------------
	// Bindings and injected bundles are held HERE rather than on the browser
	// window, because the window is recreated (BF6.Portal.Reset, a sign out, a
	// crashed renderer) and the bridge must survive that without re-registering.
	TMap<FString, TWeakObjectPtr<UObject>>  GBridgeObjects;
	TArray<TPair<FString, FString>>         GInjected;      // ordered: id -> source
	BF6PortalWeb::FBF6PortalUrlChanged      GOnUrlChanged;
	BF6PortalWeb::FBF6PortalPageLoaded      GOnPageLoaded;

	FString SavedRoot()  { return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"))); }

	// ---- WHERE CHROMIUM IS ALLOWED TO KEEP THE SESSION ----------------------
	//
	// CEF will not put a request context's cache just anywhere. The path has to
	// be the global root cache or a child of it, and a path outside that root
	// is REFUSED SILENTLY: the context is created, the browser works, every
	// page loads, and the whole session lives in memory and is gone at exit.
	// There is no warning and no log line. This is exactly what the tool was
	// doing, which is why the user signed in on every single launch.
	//
	// The engine never sets root_cache_path, so CEF defaults it to the global
	// cache_path, which WebBrowserSingleton builds as
	// <ApplicationCacheDir>/webcache_<chromium build>. The two lines below are
	// that same path, computed the same way, so our store is a child of it.
	//
	// The build number is READ, not hardcoded: it changes with the engine, and
	// a guess that goes stale would put us back outside the root and back to
	// signing in every launch, silently, all over again.
	FString EngineCacheDir()
	{
		const FString AppCache = FPaths::ShouldSaveToUserDir()
			? FPaths::ProjectSavedDir()
			: FPaths::Combine(FPlatformProcess::UserSettingsDir(),
				FApp::GetEpicProductIdentifier(),
				FApp::HasProjectName() ? FApp::GetProjectName() : TEXT("Editor"));
		const FString Full = FPaths::ConvertRelativePathToFull(AppCache);

		TArray<FString> Dirs;
		IFileManager::Get().FindFiles(Dirs, *(Full / TEXT("webcache_*")), false, true);
		// Newest build wins if an old one was left behind.
		Dirs.Sort([](const FString& A, const FString& B) { return A > B; });
		return Dirs.Num() ? (Full / Dirs[0]) : FString();
	}

	// The folder our store sits in, and the leaf we ask CEF for. CEF appends
	// "_<chromium build>" to the leaf, so the cookies really land in
	// <engine cache>/bf6portal_6613/. With no engine cache found we fall back
	// to the old project-local path, which is no worse than it was.
	FString StoreRoot() { return EngineCacheDir(); }

	// ---- WHICH STORE, AND WHY IT IS DECIDED AT RUNTIME ----------------------
	//
	// A login is kept by two different kinds of cookie and only one of them
	// survives on its own. A cookie with an expiry is written to disk by the
	// ordinary browser store, and that store demonstrably works: the database
	// holds real portal.battlefield.com and ea.com cookies. A SESSION cookie
	// has no expiry and Chromium drops it at shutdown unless it is told to keep
	// it, and the ONLY place Unreal exposes that switch is on a private browser
	// context. So if the site signs you in with a session cookie, the ordinary
	// store can never hold it, however well it works.
	//
	// A private context was tried before and wrote nothing at all. That may be
	// because its folder was nested inside the engine's own profile directory,
	// which Chromium will not do, or it may be that private contexts simply do
	// not work on this build. Rather than guess a fourth time, the tool tries
	// the private store once and then believes what it finds:
	//
	//   never tried            use the private store, with session cookies kept
	//   tried and it wrote     keep using it, it works
	//   tried and still empty  it does not work here; use the ordinary store
	//
	// The last case is the important one. It means a failure costs one session
	// and then heals itself, instead of leaving the panel permanently worse
	// than the ordinary store it replaced.
	FString PrivateStorePath()
	{
		const FString Root = EngineCacheDir();
		return Root.IsEmpty() ? FString() : FPaths::Combine(Root, TEXT("bf6portal"));
	}

	// The versioned folders CEF makes from that leaf.
	TArray<FString> PrivateStoreDirs()
	{
		TArray<FString> Out;
		const FString Root = EngineCacheDir();
		if (Root.IsEmpty()) return Out;
		TArray<FString> Dirs;
		IFileManager::Get().FindFiles(Dirs, *(Root / TEXT("bf6portal*")), false, true);
		for (const FString& D : Dirs) Out.Add(Root / D);
		return Out;
	}

	bool PrivateStoreTried() { return PrivateStoreDirs().Num() > 0; }

	bool PrivateStoreWorked()
	{
		for (const FString& D : PrivateStoreDirs())
		{
			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(Files, *D, TEXT("*"), true, false);
			if (Files.Num() > 0) return true;
		}
		return false;
	}

	// True when this session should ask for its own context.
	bool UsePrivateStore()
	{
		if (PrivateStorePath().IsEmpty()) return false;
		return !PrivateStoreTried() || PrivateStoreWorked();
	}

	// Where the browser really keeps the session. Chrome-runtime layout, and
	// the file whose existence is the only honest answer to "am I still signed
	// in": <store>/Default/Network/Cookies.
	FString StorePath()
	{
		return UsePrivateStore() ? PrivateStorePath() : EngineCacheDir();
	}
	FString CookieDb()
	{
		if (UsePrivateStore())
		{
			for (const FString& D : PrivateStoreDirs())
			{
				const FString P = FPaths::Combine(D, TEXT("Default"), TEXT("Network"), TEXT("Cookies"));
				if (FPaths::FileExists(P)) return P;
			}
		}
		const FString Root = EngineCacheDir();
		return Root.IsEmpty() ? FString()
			: FPaths::Combine(Root, TEXT("Default"), TEXT("Network"), TEXT("Cookies"));
	}
	// The wipe marker stays in OUR folder, never in the engine's cache, so a
	// sign out can never ask for the editor's own browser cache to be deleted.
	FString WipeMarker()
	{
		return FPaths::Combine(SavedRoot(), TEXT("portalweb"), TEXT("wipe.pending"));
	}

	FString LoginUrl()       { return GBaseUrl + TEXT("/login"); }
	FString ExperiencesUrl() { return GBaseUrl + TEXT("/experiences"); }
	// The site's editor pages. OBSERVED on the live site 2026-09-06 by opening an
	// experience from its own list and reading where it landed:
	//   /bf6/experience/settings/mode?id=<uuid>&teams=1%2C2
	// The teams parameter is the team list, not a count. It used to be written as
	// teams=0 here, which the site answers with the LOGIN page, and that single
	// wrong character is what made every import fail.
	FString ExperienceUrl(const FString& Id, const TCHAR* Section)
	{
		return FString::Printf(TEXT("%s/experience/%s?id=%s&teams=1%%2C2"), *GBaseUrl, Section, *Id);
	}

	// A pasted experience URL, or a bare id, gives up its uuid; empty if none.
	FString ExtractExperienceId(const FString& In)
	{
		const FRegexPattern P(TEXT("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}"));
		FRegexMatcher M(P, In);
		return M.FindNext() ? M.GetCaptureGroup(0).ToLower() : FString();
	}

	void LoadConfig()
	{
		FString S;
		if (GConfig->GetString(kIniSection, TEXT("PortalBaseUrl"), S, GEditorPerProjectIni) && !S.IsEmpty())
		{
			S.TrimStartAndEndInline();
			while (S.EndsWith(TEXT("/"))) S.LeftChopInline(1);
			GBaseUrl = S;
		}
		GConfig->GetString(kIniSection, TEXT("PortalUserAgent"), GUserAgent, GEditorPerProjectIni);
		float W = 0.f;
		if (GConfig->GetFloat(kIniSection, TEXT("PortalOverlayWidth"), W, GEditorPerProjectIni) && W >= 360.f) GOverlayWidth = W;
		FString Pl;
		if (GConfig->GetString(kIniSection, TEXT("PortalPlacement"), Pl, GEditorPerProjectIni) && !Pl.IsEmpty())
			GLastPlace = (Pl == TEXT("overlay")) ? EPlace::Overlay : EPlace::Docked;
	}
	void SavePlacement(EPlace P)
	{
		GLastPlace = P;
		GConfig->SetString(kIniSection, TEXT("PortalPlacement"), P == EPlace::Docked ? TEXT("dock") : TEXT("overlay"), GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}
	void SaveWidth()
	{
		GConfig->SetFloat(kIniSection, TEXT("PortalOverlayWidth"), GOverlayWidth, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	void SetStatus(const FString& S) { GStatus = S; }

	// ---- where the panel starts --------------------------------------------

	// Is there a cookie database on disk from a previous run? CEF versions its
	// own folder ("chromium_6613"), so the answer is "any child of the store
	// holds a Cookies file", not one fixed path.
	bool HasSavedSession()
	{
		const FString Db = CookieDb();
		return !Db.IsEmpty() && FPaths::FileExists(Db);
	}

	// Only real Portal pages are remembered. A self-test page, an EA login step
	// or an error page would all be a worse place to come back to than the
	// experiences list, and a stale login URL would hide a session that is
	// still perfectly good.
	bool IsPortalUrl(const FString& U)
	{
		return U.StartsWith(TEXT("https://")) && U.Contains(TEXT("portal.battlefield.com"));
	}

	void SaveLastUrl(const FString& U)
	{
		if (!IsPortalUrl(U)) return;
		GConfig->SetString(kIniSection, TEXT("PortalLastUrl"), *U, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	// No saved session at all: the login page, which is the only useful first
	// screen. Otherwise the page this editor was last left on, and failing that
	// the experiences list. The site itself decides whether the cookies are
	// still good and bounces to login if they are not; that is its job, not
	// ours, and it is the only party that can tell.
	FString StartUrl()
	{
		if (!HasSavedSession()) return LoginUrl();
		FString U;
		GConfig->GetString(kIniSection, TEXT("PortalLastUrl"), U, GEditorPerProjectIni);
		U.TrimStartAndEndInline();
		return IsPortalUrl(U) ? U : ExperiencesUrl();
	}

	// ---- can this engine embed a browser at all? ----------------------------
	//
	// Two things have to be true and they fail differently. The module can be
	// missing (a build configured without it), and the module can be present
	// while CEF's binaries are not (a trimmed engine install, an installed
	// build without Engine/Binaries/ThirdParty/CEF3). Neither may crash the
	// tool, so both are one bool and one sentence.
	FString GUnavailableWhy;

	bool WebAvailable()
	{
		// Nothing else in the editor loads WebBrowser before us, so IsAvailable()
		// is false at startup even on an install that has it. Load it first.
		FModuleManager::Get().LoadModulePtr<IWebBrowserModule>("WebBrowser");
		if (!IWebBrowserModule::IsAvailable())
		{
			GUnavailableWhy = TEXT("This editor build has no WebBrowser module, so the Portal site cannot be shown inside it.");
			return false;
		}
		if (!IWebBrowserModule::Get().IsWebModuleAvailable())
		{
			GUnavailableWhy = TEXT("The editor's web browser (CEF) is not available in this engine install, so the Portal site cannot be shown inside it.");
			return false;
		}
		GUnavailableWhy.Reset();
		return true;
	}

	// ---- the Portal kit, as this file needs it ------------------------------
	// Mirrors the styles in BF6BuildMode.cpp (file-local there): same
	// grounds, same hairline, same uppercase bold labels.
	FSlateFontInfo FontBold(int32 Size) { return FCoreStyle::GetDefaultFontStyle("Bold", Size); }
	FSlateFontInfo FontReg(int32 Size)  { return FCoreStyle::GetDefaultFontStyle("Regular", Size); }
	const FSlateBrush* InkBrush()   { static FSlateColorBrush B(BF6Theme::Ink);   return &B; }
	const FSlateBrush* PanelBrush() { static FSlateColorBrush B(BF6Theme::Panel); return &B; }
	const FSlateBrush* LineBrush()  { static FSlateColorBrush B(BF6Theme::Line);  return &B; }
	const FButtonStyle& GhostStyle()
	{
		static FButtonStyle S = FButtonStyle()
			.SetNormal (FSlateRoundedBoxBrush(BF6Theme::PanelLight, 0.f, BF6Theme::Line, 1.f))
			.SetHovered(FSlateRoundedBoxBrush(FLinearColor(FColor(0x24,0x28,0x2B)), 0.f, FLinearColor::White, 1.f))
			.SetPressed(FSlateRoundedBoxBrush(FLinearColor(FColor(0x2E,0x33,0x37)), 0.f, FLinearColor::White, 1.f))
			.SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0, 1, 0, -1));
		return S;
	}
	TSharedRef<SWidget> Btn(const FString& Label, TFunction<void()> Fn, const FString& Tip = FString())
	{
		return SNew(SButton).ButtonStyle(&GhostStyle()).ContentPadding(FMargin(10, 6))
			.ToolTipText(FText::FromString(Tip))
			.OnClicked_Lambda([Fn]{ if (Fn) Fn(); return FReply::Handled(); })
			[ SNew(STextBlock).Font(FontBold(9)).ColorAndOpacity(FSlateColor(BF6Theme::Text)).Text(FText::FromString(Label.ToUpper())) ];
	}
	TSharedRef<SWidget> BtnDyn(TFunction<FString()> Label, TFunction<void()> Fn, const FString& Tip = FString())
	{
		return SNew(SButton).ButtonStyle(&GhostStyle()).ContentPadding(FMargin(10, 6))
			.ToolTipText(FText::FromString(Tip))
			.OnClicked_Lambda([Fn]{ if (Fn) Fn(); return FReply::Handled(); })
			[ SNew(STextBlock).Font(FontBold(9)).ColorAndOpacity(FSlateColor(BF6Theme::Text))
				.Text_Lambda([Label]{ return FText::FromString(Label().ToUpper()); }) ];
	}
	TSharedRef<SWidget> Dim(TFunction<FString()> Text, int32 Size = 9)
	{
		return SNew(STextBlock).Font(FontReg(Size)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			.Text_Lambda([Text]{ return FText::FromString(Text()); });
	}

	// ---- 2. the browser window and the panel --------------------------------

	void Navigate(const FString& Url);
	void ApplyLink(const FString& Pasted);

	// Where the page goes when it asks for a new window: EA and the Portal
	// stay in this one window (a login popup in a second window would carry
	// the session nowhere); anything else is a real external link.
	bool RoutePopup(FString Url, FString /*Frame*/)
	{
		FString Host = Url;
		Host.RemoveFromStart(TEXT("https://")); Host.RemoveFromStart(TEXT("http://"));
		int32 Slash; if (Host.FindChar(TEXT('/'), Slash)) Host.LeftInline(Slash);
		if (Host.EndsWith(TEXT("battlefield.com")) || Host.EndsWith(TEXT("ea.com")))
		{
			if (GWindow.IsValid()) GWindow->LoadURL(Url);
		}
		else
		{
			FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
		}
		return true;   // handled: no popup window
	}

	// Create THE window. Its request context owns the on-disk store, which is
	// what makes the login persistent; nothing about the account passes
	// through here.
	// Apply everything the bridge registered to the window that exists now.
	// Permanent bindings, so they survive every navigation the site makes.
	void ApplyBridgeBindings()
	{
		if (!GWindow.IsValid()) return;
		for (TMap<FString, TWeakObjectPtr<UObject>>::TIterator It(GBridgeObjects); It; ++It)
		{
			if (UObject* O = It.Value().Get())
			{
				GWindow->BindUObject(It.Key(), O, true);
			}
			else
			{
				UE_LOG(LogBF6Portal, Warning, TEXT("Bridge object '%s' was collected; the page will not see window.ue.%s"), *It.Key(), *It.Key());
				It.RemoveCurrent();
			}
		}
	}

	void RunInjectedScripts()
	{
		if (!GWindow.IsValid()) return;
		for (const TPair<FString, FString>& P : GInjected)
		{
			if (!P.Value.IsEmpty()) GWindow->ExecuteJavascript(P.Value);
		}
	}

	bool EnsureWindow()
	{
		if (FParse::Param(FCommandLine::Get(), TEXT("bf6-offline")))
		{
			SetStatus(TEXT("Portal is disabled for this editor session (-bf6-offline). Local editors remain available."));
			return false;
		}
		if (GWindow.IsValid() && GWindow->IsValid() && !GWindow->IsClosing()) return true;
		GWindow.Reset();
		if (!WebAvailable())
		{
			SetStatus(GUnavailableWhy);
			return false;
		}
		IWebBrowserSingleton* S = IWebBrowserModule::Get().GetSingleton();
		if (!S) { SetStatus(TEXT("The web browser could not start.")); return false; }

		FCreateBrowserWindowSettings St;
		St.InitialURL = StartUrl();
		St.BackgroundColor = FColor(0x0D, 0x0F, 0x10);
		St.bUseTransparency = false;
		St.bThumbMouseButtonNavigation = true;
		St.bShowErrorMessage = true;
		// The user agent reads "... (KHTML, like Gecko) BF6UnrealSDK Chrome/128.x Safari/537.36":
		// a plain Chromium 128 with one product token, which is what the site
		// sees from any Chrome. The engine names the PROJECT there by default,
		// and "BF6_High_Poly/5.8.0-..." is not a browser any site has heard of.
		St.UserAgentApplication = FString(TEXT("BF6UnrealSDK"));
		// A private context is the only way to ask for SESSION cookies to be
		// kept, and a session cookie is what a sign-in that vanishes at exit
		// looks like. So it is asked for, once, and the result is believed:
		// UsePrivateStore reads what the last attempt actually wrote. Leaving
		// St.Context unset falls back to the browser's ordinary store, which is
		// proven to keep cookies that carry an expiry.
		if (UsePrivateStore())
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(PrivateStorePath()), true);
			FBrowserContextSettings Ctx(kContextId);
			Ctx.CookieStorageLocation = PrivateStorePath();
			Ctx.bPersistSessionCookies = true;
			St.Context = Ctx;
		}

		GWindow = S->CreateBrowserWindow(St);
		if (!GWindow.IsValid()) { SetStatus(TEXT("The web browser could not open a window.")); return false; }

		// Opt-in header override for the day the site refuses this Chromium's
		// version string. Off unless [BF6UnrealSDK] PortalUserAgent is set,
		// because a header that disagrees with navigator.userAgent is exactly
		// what bot checks look for.
		if (!GUserAgent.IsEmpty())
		{
			GWindow->OnBeforeResourceLoad().BindLambda(
				[](FString, FString, IWebBrowserWindow::FRequestHeaders& Headers, const bool)
				{ Headers.Add(TEXT("User-Agent"), GUserAgent); });
		}
		// The bridge may have registered before the panel was ever opened, and
		// it must not have to notice that a window was recreated under it.
		ApplyBridgeBindings();

		UE_LOG(LogBF6Portal, Display, TEXT("Browser window created. Store: %s. Saved session on disk: %s. Start URL: %s"),
			*StorePath(), HasSavedSession() ? TEXT("yes") : TEXT("no"), *St.InitialURL);
		return true;
	}

	// What the panel shows when there is no browser to show. A sentence that
	// says which of the two things is missing, and the one button that still
	// works: hand the page to the user's own browser.
	TSharedRef<SWidget> MakeUnavailableView()
	{
		return SNew(SBorder).BorderImage(PanelBrush()).Padding(20)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
			[
				SNew(STextBlock).Font(FontBold(11)).ColorAndOpacity(FSlateColor(BF6Theme::Accent))
				.Text(FText::FromString(TEXT("THE PORTAL SITE CANNOT BE EMBEDDED HERE")))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock).AutoWrapText(true).Font(FontReg(10)).ColorAndOpacity(FSlateColor(BF6Theme::Text))
				.Text_Lambda([]{ return FText::FromString(GUnavailableWhy.IsEmpty() ? GStatus : GUnavailableWhy); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 14)
			[
				SNew(STextBlock).AutoWrapText(true).Font(FontReg(9)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
				.Text(FText::FromString(TEXT("Everything else on this panel still works. LINK, THIS MAP and COPY EXPORT PATH do their jobs without a browser, and OPEN IN BROWSER sends the same address to the one you already use.")))
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left)
			[ Btn(TEXT("Open in browser"), []{ BF6PortalWeb::OpenInSystemBrowser(); }, TEXT("Open the Portal site in your own browser.")) ]
		];
	}

	TSharedRef<SWidget> MakeBrowserView()
	{
		if (!EnsureWindow()) return MakeUnavailableView();

		GBrowser = SNew(SWebBrowser, GWindow)
			.ShowControls(false)
			.ShowAddressBar(false)
			.ShowErrorMessage(true)
			.ShowInitialThrobber(true)
			.BackgroundColor(FColor(0x0D, 0x0F, 0x10))
			.OnUrlChanged_Lambda([](const FText& T)
			{
				const FString New = T.ToString();
				if (New == GUrl) return;
				GUrl = New;
				SaveLastUrl(GUrl);           // so the panel reopens where it was left
				GOnUrlChanged.Broadcast(GUrl);
			})
			.OnTitleChanged_Lambda([](const FText& T){ GTitle = T.ToString(); })
			.OnLoadStarted_Lambda([]
			{
				GLoading = true;
				SetStatus(TEXT("Loading..."));
				// THE HOOKS HAVE TO BE IN BEFORE THE SITE'S OWN CODE RUNS.
				//
				// The bundles used to be installed only when the page had
				// FINISHED loading, and by then the site had already asked for
				// everything it needed. The capture's own record of observed
				// calls came back empty on a fully rendered experiences list:
				// the page was complete, the hook was present, and every
				// request it existed to see had gone past before it arrived.
				//
				// Running them here as well puts them in at the start of the
				// document, before the site's scripts. They are written to be
				// installed twice, and the load-completed pass stays because a
				// document that replaces itself still has to be re-wired.
				RunInjectedScripts();
			})
			.OnLoadCompleted_Lambda([]
			{
				GLoading = false;
				SetStatus(GTitle.IsEmpty() ? TEXT("Ready.") : GTitle);
				// The injected bundles first, so anything listening to
				// OnPageLoaded finds the page already wired.
				RunInjectedScripts();
				GOnPageLoaded.Broadcast(GUrl);
			})
			.OnLoadError_Lambda([]{ GLoading = false; SetStatus(TEXT("The page did not load. Check the connection and press RELOAD.")); })
			.OnBeforePopup_Lambda([](FString Url, FString Frame){ return RoutePopup(Url, Frame); })
			.OnConsoleMessage_Lambda([](const FString& Msg, const FString& Src, int32 Line, EWebBrowserConsoleLogSeverity Sev)
			{
				// The page's console goes to the log at Display so the cookie
				// self-test, and a later script bridge, can be read there.
				// Anything OUR injected code prints announces itself with a
				// "BF6..." prefix (BF6SELFTEST, BF6ASSIST, and whatever a later
				// bundle picks). Those go to the log where they can be read;
				// the site's own console noise stays at Verbose.
				if (Msg.StartsWith(TEXT("BF6")))
				{
					UE_LOG(LogBF6Portal, Display, TEXT("page: %s"), *Msg);
				}
				else
				{
					UE_LOG(LogBF6Portal, Verbose, TEXT("page console: %s  (%s:%d)"), *Msg, *Src, Line);
				}
			});
		return GBrowser.ToSharedRef();
	}

	// A narrow vertical grip on the overlay's left edge: drag it to resize.
	class SBF6PortalGrip : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SBF6PortalGrip) {}
		SLATE_END_ARGS()
		void Construct(const FArguments&)
		{
			ChildSlot
			[
				SNew(SBox).WidthOverride(6.f)
				[ SNew(SBorder).BorderImage(LineBrush()).Padding(0).ColorAndOpacity_Lambda([this]{ return IsHovered() || HasMouseCapture() ? FLinearColor::White : FLinearColor(1,1,1,0.35f); }) ]
			];
		}
		virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& E) override
		{
			if (E.GetEffectingButton() != EKeys::LeftMouseButton) return FReply::Unhandled();
			LastX = E.GetScreenSpacePosition().X;
			return FReply::Handled().CaptureMouse(SharedThis(this));
		}
		virtual FReply OnMouseMove(const FGeometry&, const FPointerEvent& E) override
		{
			if (!HasMouseCapture()) return FReply::Unhandled();
			const float X = E.GetScreenSpacePosition().X;
			// the grip is on the LEFT edge: dragging left widens
			GOverlayWidth = FMath::Clamp(GOverlayWidth - (X - LastX), 360.f, 1600.f);
			LastX = X;
			return FReply::Handled();
		}
		virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent& E) override
		{
			if (!HasMouseCapture()) return FReply::Unhandled();
			SaveWidth();
			return FReply::Handled().ReleaseMouseCapture();
		}
		virtual FCursorReply OnCursorQuery(const FGeometry&, const FPointerEvent&) const override
		{
			return FCursorReply::Cursor(EMouseCursor::ResizeLeftRight);
		}
	private:
		float LastX = 0.f;
	};

	void CopyExportPath();
	void OpenThisMap();
	void OpenLinked(const TCHAR* Section);
	FString LinkedIdForCurrent();
	void Unlink();
	bool CanLinkCurrentPage();
}

// The panel: the tool's chrome around the page. One instance, moved between
// its two hosts; the browser view lives in BrowserHost so a reset can swap it.
class SBF6PortalPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6PortalPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		// A real panel: it covers the map on purpose and takes the clicks.
		SetVisibility(EVisibility::Visible);
		ChildSlot
		[
			SNew(SBorder).BorderImage(InkBrush()).Padding(0)
			[
				SNew(SVerticalBox)

				// navigation row
				+ SVerticalBox::Slot().AutoHeight().Padding(8, 8, 8, 4)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
					[ Btn(TEXT("<"), []{ if (GBrowser.IsValid() && GBrowser->CanGoBack()) GBrowser->GoBack(); }, TEXT("Back")) ]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
					[ Btn(TEXT(">"), []{ if (GBrowser.IsValid() && GBrowser->CanGoForward()) GBrowser->GoForward(); }, TEXT("Forward")) ]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
					[ Btn(TEXT("Reload"), []{ if (GBrowser.IsValid()) GBrowser->Reload(); }) ]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
					[ Btn(TEXT("Home"), []{ Navigate(ExperiencesUrl()); }, TEXT("Your experiences on the Portal site")) ]
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0, 0, 8, 0)
					[
						SNew(SBorder).BorderImage(PanelBrush()).Padding(FMargin(8, 5))
						[ Dim([]{ return GUrl.IsEmpty() ? FString(TEXT("about:blank")) : GUrl; }) ]
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
					[ BtnDyn([]{ return FString(GPlace == EPlace::Docked ? TEXT("Overlay") : TEXT("Dock")); },
						[]{ if (GPlace == EPlace::Docked) BF6PortalWeb::ShowOverlay(); else BF6PortalWeb::ShowDocked(); },
						TEXT("Move the panel: a dock tab beside the viewport, or a column over it. Same page either way.")) ]
					+ SHorizontalBox::Slot().AutoWidth()
					[ Btn(TEXT("Hide"), []{ BF6PortalWeb::Hide(); }, TEXT("Hide the panel. The page stays as it is; PORTAL on the ring brings it back.")) ]
				]

				// Portal actions
				+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 2)
				[
					SNew(SWrapBox).UseAllottedSize(true)
					+ SWrapBox::Slot().Padding(0, 0, 4, 4)
					[ Btn(TEXT("My experiences"), []{ Navigate(ExperiencesUrl()); }) ]
					+ SWrapBox::Slot().Padding(0, 0, 4, 4)
					[ BtnDyn([]
						{
							if (BF6Api::CurrentSave().IsEmpty())    return FString(TEXT("This map"));
							if (LinkedIdForCurrent().IsEmpty())     return FString(TEXT("Link this map"));
							return FString(TEXT("Open this save's Portal experience"));
						},
						[]{ OpenThisMap(); },
						TEXT("Open the experience this map is linked to. Not linked yet: opens your experiences so you can pick one.")) ]
					+ SWrapBox::Slot().Padding(0, 0, 4, 4)
					[ Btn(TEXT("Blocks"), []{ OpenLinked(TEXT("rules/blocks")); }, TEXT("The linked experience's block editor")) ]
					+ SWrapBox::Slot().Padding(0, 0, 4, 4)
					[ Btn(TEXT("Script"), []{ OpenLinked(TEXT("rules/script")); }, TEXT("The linked experience's script editor")) ]
					+ SWrapBox::Slot().Padding(0, 0, 4, 4)
					[ Btn(TEXT("Copy export path"), []{ CopyExportPath(); }, TEXT("Put the last exported .spatial.json path on the clipboard, ready to paste into the site's file picker.")) ]
					+ SWrapBox::Slot().Padding(0, 0, 4, 4)
					[ Btn(TEXT("Open in browser"), []{ BF6PortalWeb::OpenInSystemBrowser(); }, TEXT("Open the page showing here in your own browser. Your sign-in there is separate from this one.")) ]
					+ SWrapBox::Slot().Padding(0, 0, 4, 4)
					[ Btn(TEXT("Sign out"), []{ BF6PortalWeb::SignOut(); }, TEXT("Clear the saved session: cookies and site storage. You will need to sign in again.")) ]
				]

				// link this map to an experience
				+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 6)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
					[ Dim([]
						{
							if (BF6Api::CurrentSave().IsEmpty()) return FString(TEXT("No custom map open"));
							const FString Id = LinkedIdForCurrent();
							return Id.IsEmpty() ? FString(TEXT("Not linked")) : FString(TEXT("Linked: ")) + Id.Left(8);
						}) ]
					// The one-click link, and the ONLY thing on this row when it
					// applies. Standing on the experience page already says which
					// experience is meant; making the user copy the address out
					// of the panel and paste it back in would be a ritual.
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
					[
						SNew(SBox)
						.Visibility_Lambda([]{ return CanLinkCurrentPage() ? EVisibility::Visible : EVisibility::Collapsed; })
						[ Btn(TEXT("Link current page to this save"), []{ ApplyLink(GUrl); },
							TEXT("Remember this experience as the one this custom map belongs to. Stored in the save file, not on the site.")) ]
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0, 0, 4, 0)
					[
						SAssignNew(LinkBox, SEditableTextBox).Font(FontReg(9))
						.HintText(FText::FromString(TEXT("Link this map to a Portal experience: paste the experience URL and press Enter")))
						.OnTextCommitted_Lambda([this](const FText& T, ETextCommit::Type How)
						{
							if (How == ETextCommit::OnEnter) { ApplyLink(T.ToString()); LinkBox->SetText(FText::GetEmpty()); }
						})
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
					[ Btn(TEXT("Link"), [this]{ ApplyLink(LinkBox->GetText().ToString()); LinkBox->SetText(FText::GetEmpty()); }) ]
					+ SHorizontalBox::Slot().AutoWidth()
					[ Btn(TEXT("Unlink"), []{ Unlink(); }) ]
				]

				// THE PAGE, AND NOTHING BESIDE IT.
				//
				// ---- BF6PortalProfile ----
				// ---- BF6PortalSettings ----
				// The profile column and the settings column used to sit here,
				// left of the page, and they read as tabs inside a browser
				// window: the tool's own screens wearing the site's frame. They
				// are not the site, so they are not in the site's panel. Both
				// live on the EXPERIENCE screen now (BF6Experience.cpp), which
				// the toolbar row opens over the viewport, and both features are
				// otherwise untouched: their widgets, their dock tabs and their
				// registration are exactly where they were.
				// ---- end BF6PortalSettings ----
				// ---- end BF6PortalProfile ----
				+ SVerticalBox::Slot().FillHeight(1.f)
				[ SAssignNew(BrowserHost, SBox) ]

				// status
				+ SVerticalBox::Slot().AutoHeight().Padding(8, 4, 8, 2)
				[ Dim([]{ return GStatus; }) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 8)
				[
					SNew(STextBlock).AutoWrapText(true).Font(FontReg(8)).ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
					.Text(FText::FromString(TEXT("You sign in on the page itself. The tool never reads, stores or types your password. It keeps the site's own cookies under Saved/BF6UnrealSDK/portalweb so the sign-in survives an editor restart; SIGN OUT deletes them.")))
				]
			]
		];
		BrowserHost->SetContent(MakeBrowserView());
	}

	void ResetBrowserView()
	{
		BrowserHost->SetContent(SNullWidget::NullWidget);
		GBrowser.Reset();
		BrowserHost->SetContent(MakeBrowserView());
	}

private:
	TSharedPtr<SBox> BrowserHost;
	TSharedPtr<SEditableTextBox> LinkBox;
};

namespace
{
	void EnsurePanel()
	{
		if (!GPanel.IsValid()) GPanel = SNew(SBF6PortalPanel);
	}

	// After a move, tell the view which OS window it is in (popups, IME, focus
	// tracking all key off it) and hand it the keyboard. Next tick, once the
	// parent chain exists.
	void SettleIntoWindow()
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			// Offscreen is deliberately excluded: this call ends by taking the
			// keyboard, and background work must never take it from the user.
			if (GPlace != EPlace::Hidden && GPlace != EPlace::Offscreen
				&& GPanel.IsValid() && GBrowser.IsValid() && FSlateApplication::IsInitialized())
			{
				TSharedPtr<SWindow> Win = FSlateApplication::Get().FindWidgetWindow(GPanel.ToSharedRef());
				if (Win.IsValid()) GBrowser->SetParentWindow(Win);
				FSlateApplication::Get().SetKeyboardFocus(GBrowser, EFocusCause::SetDirectly);
			}
			return false;
		}), 0.f);
	}

	// ---- 3. placements ------------------------------------------------------

	void Detach()
	{
		const EPlace Was = GPlace;
		GPlace = EPlace::Hidden;
		if (Was == EPlace::Overlay)
		{
			if (GOverlayHost.IsValid()) GOverlayHost->SetContent(SNullWidget::NullWidget);
			if (GOverlayRoot.IsValid())
				if (TSharedPtr<SLevelViewport> VP = GOverlayVP.Pin()) VP->RemoveOverlayWidget(GOverlayRoot.ToSharedRef());
			GOverlayVP.Reset();
		}
		else if (Was == EPlace::Docked)
		{
			if (GDockHost.IsValid()) GDockHost->SetContent(SNullWidget::NullWidget);
			// GPlace is already Hidden, so the tab's close callback does nothing
			if (TSharedPtr<SDockTab> Tab = GDockTab.Pin()) Tab->RequestCloseTab();
			GDockTab.Reset();
		}
		else if (Was == EPlace::Offscreen)
		{
			if (GOffHost.IsValid()) GOffHost->SetContent(SNullWidget::NullWidget);
			if (GOffRoot.IsValid())
				if (TSharedPtr<SLevelViewport> VP = GOffVP.Pin()) VP->RemoveOverlayWidget(GOffRoot.ToSharedRef());
			GOffVP.Reset();
		}
		else if (Was == EPlace::SignIn)
		{
			if (GSignInHost.IsValid()) GSignInHost->SetContent(SNullWidget::NullWidget);
			if (GSignInRoot.IsValid())
			{
				if (TSharedPtr<SWindow> Win = GSignInWindow.Pin())
					Win->RemoveOverlaySlot(GSignInRoot.ToSharedRef());
			}
			GSignInWindow.Reset();
		}
	}

	TSharedRef<SWidget> MakeOverlayRoot()
	{
		// THE HIT-TEST LAW: the root spans the whole viewport, so it must let
		// clicks through everywhere it is not drawing. Only the column is real.
		TSharedRef<SHorizontalBox> Root = SNew(SHorizontalBox).Visibility(EVisibility::SelfHitTestInvisible);
		Root->AddSlot().FillWidth(1.f)
		[ SNew(SSpacer).Visibility(EVisibility::SelfHitTestInvisible) ];
		TSharedRef<SHorizontalBox> Column = SNew(SHorizontalBox).Visibility(EVisibility::Visible);
		Column->AddSlot().AutoWidth()[ SNew(SBF6PortalGrip) ];
		Column->AddSlot().AutoWidth()
		[
			SNew(SBox).WidthOverride_Lambda([]{ return FOptionalSize(GOverlayWidth); })
			[ SAssignNew(GOverlayHost, SBox) ]
		];
		Root->AddSlot().AutoWidth()[ Column ];
		GOverlayColumn = Column;
		return Root;
	}

	void AttachOverlay()
	{
		FLevelEditorModule& LE = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
		TSharedPtr<SLevelViewport> VP = LE.GetFirstActiveLevelViewport();
		if (!VP.IsValid()) { SetStatus(TEXT("No level viewport to overlay yet.")); return; }
		EnsurePanel();
		if (!GOverlayRoot.IsValid()) GOverlayRoot = MakeOverlayRoot();
		GOverlayHost->SetContent(GPanel.ToSharedRef());
		// Above the build HUD (50), below the radial (100) and the controls sheet (200).
		VP->AddOverlayWidget(GOverlayRoot.ToSharedRef(), 90);
		GOverlayVP = VP;
		GPlace = EPlace::Overlay;
		SettleIntoWindow();
	}

	// ---- offscreen: the site working with nothing on screen -----------------
	//
	// WHY THE WIDGET STAYS IN THE TREE, and it is not because CEF needs it.
	//
	// It does not. FWebBrowserSingleton::Tick drives the CEF message pump for
	// every browser window it knows about, whether or not a Slate widget shows
	// one; CreateBrowserWindow uses CreateBrowserSync, so the window exists the
	// moment EnsureWindow returns; LoadURL and ExecuteJavascript go straight to
	// the CEF frame; and CheckTickActivity's worst case is SetIsHidden(true),
	// which hides the native child window and stops rendering, never loading.
	// A detached browser really would navigate.
	//
	// What would NOT survive detaching is OUR OWN plumbing. Every callback the
	// tool depends on - OnLoadCompleted, which runs the injected bundles and
	// broadcasts OnPageLoaded, and OnUrlChanged - is bound on the SWebBrowser
	// WIDGET in MakeBrowserView, not on the browser window. Drop the widget and
	// pages would still load while the capture bridge, the page probe and every
	// piece of progress the tool reports would go silent, which is the exact
	// failure that is hardest to tell from a hang.
	//
	// So the widget stays: one pixel, clipped, hit-test invisible, in the level
	// viewport's overlay at a z-order under everything. It ticks, so the page
	// is never marked hidden, and nothing else about the placement changes.
	// THE PAGE STILL NEEDS A PAGE-SIZED PAGE.
	//
	// The host used to be one pixel all the way down, and the browser laid the
	// site out inside it. A site rendered at one pixel by one pixel does not
	// build its list, so the request the tool was waiting for was never made:
	// the offscreen check loaded the experiences page, reported the capture
	// ready, and then sat there having asked the site for nothing. Chromium
	// also throttles a renderer that size, which made it slower to do nothing.
	//
	// So there are two boxes. The INNER one is a normal browser viewport, big
	// enough that the site lays out and behaves exactly as it would on screen.
	// The OUTER one is still a single clipped pixel, so all of that lands one
	// pixel wide in the corner of the viewport and nobody ever sees it.
	static const float kOffscreenW = 1280.f;
	static const float kOffscreenH = 800.f;

	TSharedRef<SWidget> MakeOffscreenRoot()
	{
		// AN OVERLAY SLOT STRETCHES WHAT IT IS GIVEN, AND THAT BEAT THIS WIDGET.
		//
		// SLevelViewport::AddOverlayWidget does ViewportOverlay->AddSlot(Z)[W],
		// and an SOverlay slot defaults to HAlign_Fill and VAlign_Fill. So the
		// box that asked to be one pixel was stretched to the whole viewport,
		// and ClipToBounds then clipped to those stretched bounds, which clips
		// nothing. The panel drew over the entire screen while GPlace said
		// Offscreen and every status line in the tool agreed with GPlace.
		//
		// That is what the user saw as "I opened a new map and the Portal page
		// opened", and it is why it could never be found in the log: nothing
		// moved, so nothing was logged. The panel had been full size all along
		// and only became noticeable once a map was open behind it.
		//
		// The fix is a second box. The OUTER one is stretched, as it will be
		// whatever we ask; its alignment then places the INNER one at the size
		// the inner one actually wants, which is the pixel, and the clip on the
		// inner box now means something.
		TSharedRef<SWidget> Root =
			SNew(SBox)
			.Visibility(EVisibility::HitTestInvisible)
			.HAlign(HAlign_Left).VAlign(VAlign_Top)
			[
				SAssignNew(GOffClip, SBox)
				.WidthOverride(1.f).HeightOverride(1.f)
				.Clipping(EWidgetClipping::ClipToBounds)
				[
					SAssignNew(GOffHost, SBox)
					.WidthOverride(kOffscreenW).HeightOverride(kOffscreenH)
				]
			];
		return Root;
	}

	void AttachOffscreen()
	{
		FLevelEditorModule& LE = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
		TSharedPtr<SLevelViewport> VP = LE.GetFirstActiveLevelViewport();
		if (!VP.IsValid()) { SetStatus(TEXT("No level viewport to work behind yet.")); return; }
		EnsurePanel();
		if (!GOffRoot.IsValid()) GOffRoot = MakeOffscreenRoot();
		GOffHost->SetContent(GPanel.ToSharedRef());
		VP->AddOverlayWidget(GOffRoot.ToSharedRef(), 1);   // under the build HUD, under everything
		GOffVP = VP;
		GPlace = EPlace::Offscreen;
		// NO SettleIntoWindow HERE. That call ends by taking keyboard focus,
		// which is exactly what a piece of background work must never do to
		// someone typing in the tool.
		UE_LOG(LogBF6Portal, Display, TEXT("Portal panel: working offscreen, nothing on screen."));
	}

	// ---- the sign-in surface ------------------------------------------------

	// THE WINDOW THE TOOL IS DRAWN IN. The map screen and the build HUD are
	// both overlays on the level viewport, so the window holding that viewport
	// is the window a person is looking at. Before there is a viewport (or if
	// the level editor is not up yet) the editor's own root window is the
	// honest answer, and the last resort is whichever window is active.
	TSharedPtr<SWindow> ToolWindow()
	{
		if (!FSlateApplication::IsInitialized()) return nullptr;
		FSlateApplication& App = FSlateApplication::Get();
		if (FModuleManager::Get().IsModuleLoaded("LevelEditor"))
		{
			FLevelEditorModule& LE = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
			if (TSharedPtr<SLevelViewport> VP = LE.GetFirstActiveLevelViewport())
			{
				if (TSharedPtr<SWindow> W = App.FindWidgetWindow(VP.ToSharedRef())) return W;
			}
		}
		if (TSharedPtr<SWindow> Root = FGlobalTabmanager::Get()->GetRootWindow()) return Root;
		return App.GetActiveTopLevelWindow();
	}

	// The height of the window's own title bar, so the sheet stops below it.
	// Zero for a window wearing the OS border, which draws its own.
	float TitleBarClearance()
	{
		TSharedPtr<SWindow> Win = GSignInWindow.Pin();
		if (!Win.IsValid()) return 0.f;
		const FOptionalSize S = Win->GetTitleBarSize();
		return S.IsSet() ? S.Get() : 0.f;
	}

	TSharedRef<SWidget> MakeSignInRoot()
	{
		// THE HIT-TEST LAW, the same one both other placements keep: the root
		// spans the whole window, so it must let clicks through everywhere it
		// is not drawing. Here that is exactly one strip: the title bar, which
		// stays live so the editor window can still be moved, resized,
		// minimised and closed while the sheet is up.
		TSharedRef<SVerticalBox> Root = SNew(SVerticalBox).Visibility(EVisibility::SelfHitTestInvisible);
		Root->AddSlot().AutoHeight()
		[
			SNew(SBox)
			.HeightOverride_Lambda([]{ return FOptionalSize(TitleBarClearance()); })
			.Visibility(EVisibility::SelfHitTestInvisible)
			[ SNew(SSpacer).Visibility(EVisibility::SelfHitTestInvisible) ]
		];

		TSharedRef<SBorder> Sheet =
			SNew(SBorder)
			.BorderImage(InkBrush())
			.Padding(0.f)
			.Visibility(EVisibility::Visible)
			[
				SNew(SVerticalBox)

				// The one line that says what is happening, and the way out.
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(16, 12, 16, 0))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock).Font(FontBold(13)).ColorAndOpacity(FSlateColor(BF6Theme::Text))
						.Text(FText::FromString(TEXT("SIGN IN TO BATTLEFIELD PORTAL")))
					]
					+ SHorizontalBox::Slot().FillWidth(1.f)[ SNew(SSpacer) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						// CANCEL is not "do not sign in": it puts the panel back
						// where it was and leaves the profile exactly as it is,
						// so the page keeps whatever it had reached.
						Btn(TEXT("Cancel"), []{ BF6PortalWeb::EndSignIn(); },
							TEXT("Put the panel back where it was and carry on. Escape does the same. Your place on the page is kept."))
					]
				]

				// WHAT THE PAGE IS DOING, out of the reports the page already
				// sends, so a slow site reads as loading rather than as a blank
				// sheet with a browser in it.
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(16, 6, 16, 10))
				[
					SNew(STextBlock).AutoWrapText(true).Font(FontReg(9))
					.ColorAndOpacity(FSlateColor(BF6Theme::TextDim))
					.Text_Lambda([]
					{
						const FString S = BF6PortalProfile::PageStatus();
						return FText::FromString(S.IsEmpty()
							? FString(TEXT("Sign in on the page. The tool never sees your password."))
							: S);
					})
				]

				+ SVerticalBox::Slot().FillHeight(1.f)[ SAssignNew(GSignInHost, SBox) ]
			];

		Root->AddSlot().FillHeight(1.f)[ Sheet ];
		GSignInSheet = Sheet;
		return Root;
	}

	void AttachSignIn()
	{
		TSharedPtr<SWindow> Win = ToolWindow();
		if (!Win.IsValid())
		{
			SetStatus(TEXT("No editor window to sign in over yet."));
			UE_LOG(LogBF6Portal, Warning, TEXT("Portal sign-in surface: no editor window to attach to."));
			return;
		}
		EnsurePanel();
		GSignInWindow = Win;
		if (!GSignInRoot.IsValid()) GSignInRoot = MakeSignInRoot();
		GSignInHost->SetContent(GPanel.ToSharedRef());
		// THE Z ORDER, and which stack it is in. This is the WINDOW's overlay,
		// not the level viewport's, so it is a different stack from the build
		// HUD (50), the editor sheet (70), the Portal column (90), the radial
		// (100) and the controls sheet (200): every one of those is inside a
		// viewport that is itself inside this window's content. The window
		// builds all of its own slots at ZOrder 0, so 1 is above all of them,
		// and above anything the tool can draw in the viewport underneath.
		Win->AddOverlaySlot(1)
		[
			GSignInRoot.ToSharedRef()
		];
		GPlace = EPlace::SignIn;
		SettleIntoWindow();
	}

	void AttachDocked()
	{
		EnsurePanel();
		if (!GDockHost.IsValid()) GDockHost = SNew(SBox);
		TSharedPtr<SDockTab> Tab = FGlobalTabmanager::Get()->TryInvokeTab(FTabId(kTabId));
		if (!Tab.IsValid()) { SetStatus(TEXT("The dock tab could not be opened.")); return; }
		GDockTab = Tab;
		GDockHost->SetContent(GPanel.ToSharedRef());
		GPlace = EPlace::Docked;
		SettleIntoWindow();
	}

	TSharedRef<SDockTab> SpawnDockTab(const FSpawnTabArgs&)
	{
		if (!GDockHost.IsValid()) GDockHost = SNew(SBox);
		TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab)
			.Label(FText::FromString(TEXT("Portal")))
			.OnTabClosed_Lambda([](TSharedRef<SDockTab>)
			{
				// closed by its own X: the page keeps running, hidden
				if (GPlace == EPlace::Docked)
				{
					GPlace = EPlace::Hidden;
					if (GDockHost.IsValid()) GDockHost->SetContent(SNullWidget::NullWidget);
				}
				GDockTab.Reset();
			});
		Tab->SetContent(GDockHost.ToSharedRef());
		GDockTab = Tab;
		// Spawned by the Window menu or a restored layout rather than by us:
		// fill it next tick, when the tab exists to fill.
		//
		// EXCEPT AT STARTUP. Unreal restores whatever tabs were open when the
		// editor last closed, so a panel left docked came back on its own and
		// the tool opened on a small Portal window nobody asked for. A tab
		// restored by the layout is closed instead: the feature is one press of
		// PORTAL away, and the page is not built until something asks for it,
		// which also keeps a browser out of every cold start.
		if (GStartupGrace)
		{
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
			{
				if (GDockTab.IsValid() && GPlace != EPlace::Docked)
				{
					UE_LOG(LogBF6Portal, Display,
						TEXT("Portal panel: a docked tab from the last session was restored by the editor layout and has been closed. PORTAL opens it."));
					GDockTab.Pin()->RequestCloseTab();
				}
				return false;
			}), 0.f);
			return Tab;
		}
		if (GPlace != EPlace::Docked)
		{
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
			{
				if (GDockTab.IsValid() && GPlace != EPlace::Docked) BF6PortalWeb::ShowDocked();
				return false;
			}), 0.f);
		}
		return Tab;
	}

	// ---- 4. the Portal actions ---------------------------------------------

	// EVERY AUTOMATED NAVIGATION COMES THROUGH HERE, and none of them is a
	// reason to put a website in front of somebody. This used to call Open(),
	// which is why asking for something entirely local - an import, a settings
	// read, a thumbnail upload - dragged the site on screen.
	void Navigate(const FString& Url)
	{
		if (GPlace == EPlace::Hidden) AttachOffscreen();
		if (!EnsureWindow()) return;
		// Reusing the current document preserves captured requests and avoids
		// racing a card click against a redundant full-page reload.
		if (GWindow->GetUrl() == Url) return;
		GLoading = true; // before CEF can answer probes from the outgoing page
		GWindow->LoadURL(Url);
	}

	FString CacheKey(const FString& Level, const FString& Save) { return Level + TEXT("|") + Save; }

	// The session file, newest layout first (mirrors BF6_SessionPathFor, which is
	// file-local to BF6UnrealSDK.cpp).
	FString SessionPathAt(const FString& Root, const FString& Level, const FString& Save)
	{
		const FString Experience = FPaths::Combine(Root, TEXT("saves/experiences"), Save,
			TEXT("maps"), Level, Level + TEXT(".json"));
		if (FPaths::FileExists(Experience)) return Experience;
		const FString New = FPaths::Combine(Root, TEXT("saves"), Save, Level + TEXT(".json"));
		if (FPaths::FileExists(New)) return New;
		const FString Old = FPaths::Combine(Root, Level, Save + TEXT(".json"));
		return FPaths::FileExists(Old) ? Old : FString();
	}
	FString SessionPath(const FString& Level, const FString& Save)
	{
		return SessionPathAt(SavedRoot(), Level, Save);
	}

#if WITH_DEV_AUTOMATION_TESTS
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBF6PortalSessionPathTest, "BF6.Editor.PortalSessionLayouts",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
	bool FBF6PortalSessionPathTest::RunTest(const FString& Parameters)
	{
		const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),
			TEXT("Automation"), TEXT("PortalSession_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		const FString Old = Root / TEXT("MP_Test/Example.json");
		const FString Flat = Root / TEXT("saves/Example/MP_Test.json");
		const FString Experience = Root / TEXT("saves/experiences/Example/maps/MP_Test/MP_Test.json");
		ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
		TestTrue(TEXT("Missing session has no link source"), SessionPathAt(Root, TEXT("MP_Test"), TEXT("Example")).IsEmpty());
		for (const FString& Path : { Old, Flat, Experience })
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			if (!TestTrue(TEXT("Create isolated session fixture"), FFileHelper::SaveStringToFile(TEXT("{}"), *Path))) return false;
			TestEqual(TEXT("Newest existing layout wins"), SessionPathAt(Root, TEXT("MP_Test"), TEXT("Example")), Path);
		}
		TestTrue(TEXT("Another map cannot use this map's link"), SessionPathAt(Root, TEXT("MP_Other"), TEXT("Example")).IsEmpty());
		return true;
	}
#endif

	FString ReadExperienceFromFile(const FString& Level, const FString& Save)
	{
		const FString Path = SessionPath(Level, Save);
		if (Path.IsEmpty()) return FString();
		FString In;
		if (!FFileHelper::LoadFileToString(In, *Path)) return FString();
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
		if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) return FString();
		FString Exp; Root->TryGetStringField(TEXT("portalExperience"), Exp);
		return Exp;
	}

	// ---- BF6PortalProfile ----
	int32 ReadMapIdxFromFile(const FString& Level, const FString& Save)
	{
		const FString Path = SessionPath(Level, Save);
		if (Path.IsEmpty()) return -1;
		FString In;
		if (!FFileHelper::LoadFileToString(In, *Path)) return -1;
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
		if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) return -1;
		double Idx = -1;
		return Root->TryGetNumberField(TEXT("portalMapIdx"), Idx) ? (int32)Idx : -1;
	}
	// ---- end BF6PortalProfile ----

	FString LinkedIdForCurrent()
	{
		const FString Save = BF6Api::CurrentSave();
		if (Save.IsEmpty()) return FString();
		return ExtractExperienceId(BF6PortalWeb::ExperienceForSave(BF6Api::CurrentLevel(), Save));
	}

	// The one-click link. True only when the page showing IS an experience the
	// open save is not already linked to, so the button appears exactly when it
	// has something to do and there is nothing to read or decide.
	bool CanLinkCurrentPage()
	{
		if (BF6Api::CurrentSave().IsEmpty() || !BF6Api::IsEditing()) return false;
		if (!IsPortalUrl(GUrl) || !GUrl.Contains(TEXT("/experience"))) return false;
		const FString Id = ExtractExperienceId(GUrl);
		return !Id.IsEmpty() && Id != LinkedIdForCurrent();
	}

	void ApplyLink(const FString& Pasted)
	{
		const FString Save = BF6Api::CurrentSave();
		if (Save.IsEmpty() || !BF6Api::IsEditing())
		{
			SetStatus(TEXT("Open a custom map first - the link is saved with the map."));
			return;
		}
		FString Url = Pasted; Url.TrimStartAndEndInline();
		if (Url.IsEmpty()) return;
		const FString Id = ExtractExperienceId(Url);
		if (Id.IsEmpty())
		{
			SetStatus(TEXT("That does not look like an experience URL. Open the experience on the site and paste the address bar, which carries id=..."));
			return;
		}
		// A bare id gets a real page to open; a URL is kept exactly as pasted.
		if (!Url.StartsWith(TEXT("http"))) Url = ExperienceUrl(Id, TEXT("settings/mode"));
		GExpCache.Add(CacheKey(BF6Api::CurrentLevel(), Save), Url);
		BF6Api::SaveCurrent(true);   // writes portalExperience through SaveSession
		SetStatus(FString::Printf(TEXT("Linked '%s' to experience %s. THIS MAP, BLOCKS and SCRIPT open it."), *Save, *Id.Left(8)));
	}

	void Unlink()
	{
		const FString Save = BF6Api::CurrentSave();
		if (Save.IsEmpty() || !BF6Api::IsEditing()) return;
		GExpCache.Add(CacheKey(BF6Api::CurrentLevel(), Save), FString());
		BF6Api::SaveCurrent(true);
		SetStatus(FString::Printf(TEXT("'%s' is no longer linked to an experience."), *Save));
	}

	void OpenThisMap()
	{
		const FString Save = BF6Api::CurrentSave();
		if (Save.IsEmpty())
		{
			Navigate(ExperiencesUrl());
			SetStatus(TEXT("No custom map open. Showing your experiences."));
			return;
		}
		const FString Url = BF6PortalWeb::ExperienceForSave(BF6Api::CurrentLevel(), Save);
		if (Url.IsEmpty())
		{
			Navigate(ExperiencesUrl());
			SetStatus(FString::Printf(TEXT("'%s' is not linked yet. Open its experience here, then paste the address into the link field."), *Save));
			return;
		}
		Navigate(Url);
	}

	void OpenLinked(const TCHAR* Section)
	{
		const FString Id = LinkedIdForCurrent();
		if (Id.IsEmpty()) { OpenThisMap(); return; }
		Navigate(ExperienceUrl(Id, Section));
	}

	// The last export, or failing that the newest .spatial.json in the export
	// folder (this editor session may not have exported yet).
	FString LastExportPath()
	{
		if (!GLastExport.IsEmpty() && FPaths::FileExists(GLastExport)) return GLastExport;
		const FString Dir = FPaths::Combine(SavedRoot(), TEXT("export"));
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.spatial.json")), true, false);
		FString Best; FDateTime BestT = FDateTime::MinValue();
		for (const FString& F : Files)
		{
			const FString P = Dir / F;
			const FDateTime T = IFileManager::Get().GetTimeStamp(*P);
			if (T > BestT) { BestT = T; Best = P; }
		}
		return Best;
	}

	void CopyExportPath()
	{
		const FString P = LastExportPath();
		if (P.IsEmpty())
		{
			SetStatus(TEXT("Nothing exported yet. EXPORT from the build HUD writes the .spatial.json first."));
			return;
		}
		FString Win = FPaths::ConvertRelativePathToFull(P);
		FPaths::MakePlatformFilename(Win);
		FPlatformApplicationMisc::ClipboardCopy(*Win);
		SetStatus(FString::Printf(TEXT("Copied: %s. Paste it into the site's file picker."), *Win));
		BF6Api::Toast(TEXT("Export path copied."));
	}

	// ---- 5. console commands and the cookie self-test -----------------------

	// Two steps, in the log as "Portal page: BF6SELFTEST ...":
	//   BF6.Portal.CookieTest write   writes bf6selftest=stamp-<ms> (secure, one year)
	//   BF6.Portal.Reset              closes the browser window and makes a new one on the same store
	//   BF6.Portal.CookieTest read    a fresh window reads document.cookie back
	// Read after a full editor restart and it proves the on-disk store too.
	FString SelfTestHtml()
	{
		return TEXT("<!doctype html><html><head><meta charset='utf-8'><title>BF6 Portal panel self test</title></head>")
			TEXT("<body style='background:#0d0f10;color:#bfcad1;font-family:sans-serif;padding:24px'>")
			TEXT("<h3>BF6 Portal panel self test</h3><div id='out'></div><script>")
			TEXT("var write = location.search.indexOf('write') >= 0;")
			TEXT("var out = document.getElementById('out');")
			TEXT("if (write) { var stamp = 'stamp-' + Date.now();")
			TEXT("  document.cookie = 'bf6selftest=' + stamp + '; max-age=31536000; path=/; secure; samesite=lax';")
			TEXT("  console.log('BF6SELFTEST WRITE ' + stamp + ' -> document.cookie now: ' + document.cookie);")
			TEXT("  out.textContent = 'wrote cookie ' + stamp; }")
			TEXT("else { console.log('BF6SELFTEST READ ' + (document.cookie || '(no cookie)'));")
			TEXT("  out.textContent = 'document.cookie = ' + (document.cookie || '(none)'); }")
			TEXT("</script></body></html>");
	}

	void CookieTest(const FString& Mode)
	{
		if (GPlace == EPlace::Hidden) BF6PortalWeb::Open();
		if (!EnsureWindow()) return;
		const FString Url = FString(kSelfTestUrl) + (Mode == TEXT("write") ? TEXT("?write") : TEXT("?read"));
		GWindow->LoadString(SelfTestHtml(), Url);
		SetStatus(FString::Printf(TEXT("Cookie self test (%s): result in the Output Log as 'Portal page: BF6SELFTEST'."), Mode == TEXT("write") ? TEXT("write") : TEXT("read")));
	}

	// Close the browser window and open a new one on the same store, without
	// touching the panel. What the self-test uses to prove the cookie is in
	// the store and not merely in the old window's memory.
	void ResetWindow()
	{
		if (GWindow.IsValid()) GWindow->CloseBrowser(true, false);
		GWindow.Reset();
		GBrowser.Reset();
		if (GPanel.IsValid()) GPanel->ResetBrowserView();
		if (GPlace != EPlace::Hidden) SettleIntoWindow();
		SetStatus(TEXT("Browser window recreated on the same store."));
	}

	// Everything a "why is it not doing that" question needs, in one block in
	// the Output Log. Deliberately says nothing about the ACCOUNT: whether a
	// cookie file exists is a fact about this machine, its contents are not
	// ours to read and are never printed.
	void LogStatus()
	{
		const bool bWeb = WebAvailable();
		UE_LOG(LogBF6Portal, Display, TEXT("---- Portal panel status ----"));
		UE_LOG(LogBF6Portal, Display, TEXT("  embedded browser : %s"), bWeb ? TEXT("available") : *GUnavailableWhy);
		UE_LOG(LogBF6Portal, Display, TEXT("  panel            : %s"), PlaceName(GPlace));
		// THE PIXELS, NOT THE VARIABLE.
		//
		// "offscreen (working, nothing on screen)" is a reading of GPlace, and
		// GPlace was right while the panel covered the whole screen. A status
		// line that can only ever agree with the bug is worse than no line, so
		// this one measures what Slate actually gave the clip box. Anything
		// bigger than a pixel or two means it is being drawn.
		if (GPlace == EPlace::Offscreen && GOffClip.IsValid())
		{
			const FVector2D Got = GOffClip->GetTickSpaceGeometry().GetLocalSize();
			const bool bReallyHidden = Got.X <= 2.f && Got.Y <= 2.f;
			UE_LOG(LogBF6Portal, Display, TEXT("  offscreen clip   : %.0f x %.0f px%s"),
				Got.X, Got.Y,
				bReallyHidden ? TEXT("") :
				TEXT("  <-- NOT hidden. The overlay slot stretched it and the panel is on screen."));
		}
		if (GPlace == EPlace::SignIn)
		{
			UE_LOG(LogBF6Portal, Display, TEXT("  sign-in surface  : over %s, goes back to %s"),
				GSignInWindow.IsValid() ? TEXT("the editor window") : TEXT("nothing (the window went away)"),
				PlaceName(GBeforeSignIn));
		}
		UE_LOG(LogBF6Portal, Display, TEXT("  browser window   : %s"), GWindow.IsValid() ? TEXT("open") : TEXT("not created yet"));
		UE_LOG(LogBF6Portal, Display, TEXT("  address          : %s"), GUrl.IsEmpty() ? TEXT("(nothing loaded)") : *GUrl);
		UE_LOG(LogBF6Portal, Display, TEXT("  status line      : %s"), *GStatus);
		UE_LOG(LogBF6Portal, Display, TEXT("  base url         : %s"), *GBaseUrl);
		UE_LOG(LogBF6Portal, Display, TEXT("  session store    : %s"), *StorePath());
		UE_LOG(LogBF6Portal, Display, TEXT("  saved session    : %s"), HasSavedSession() ? TEXT("yes, a cookie store is on disk") : TEXT("no, the panel starts at the login page"));
		UE_LOG(LogBF6Portal, Display, TEXT("  wipe pending     : %s"), FPaths::FileExists(WipeMarker()) ? TEXT("yes, the store goes at next editor start") : TEXT("no"));

		const FString Save = BF6Api::CurrentSave();
		if (Save.IsEmpty())
		{
			UE_LOG(LogBF6Portal, Display, TEXT("  linked experience: no custom map open"));
		}
		else
		{
			const FString Exp = BF6PortalWeb::ExperienceForSave(BF6Api::CurrentLevel(), Save);
			UE_LOG(LogBF6Portal, Display, TEXT("  linked experience: %s -> %s"), *Save, Exp.IsEmpty() ? TEXT("(not linked)") : *Exp);
		}

		const FString Exp2 = LastExportPath();
		UE_LOG(LogBF6Portal, Display, TEXT("  last export      : %s"), Exp2.IsEmpty() ? TEXT("(nothing exported yet)") : *Exp2);

		// The bridge seam, so the other half of this feature can be debugged
		// from here rather than from guesswork.
		FString Bound;
		for (const TPair<FString, TWeakObjectPtr<UObject>>& P : GBridgeObjects)
			Bound += FString::Printf(TEXT("window.ue.%s%s "), *P.Key, P.Value.IsValid() ? TEXT("") : TEXT("(collected)"));
		UE_LOG(LogBF6Portal, Display, TEXT("  bridge objects   : %s"), Bound.IsEmpty() ? TEXT("(none)") : *Bound);
		FString Inj;
		for (const TPair<FString, FString>& P : GInjected)
			Inj += FString::Printf(TEXT("%s(%d chars) "), *P.Key, P.Value.Len());
		UE_LOG(LogBF6Portal, Display, TEXT("  injected scripts : %s"), Inj.IsEmpty() ? TEXT("(none)") : *Inj);
		UE_LOG(LogBF6Portal, Display, TEXT("  bridge listeners : %d url, %d page-loaded"),
			GOnUrlChanged.IsBound() ? 1 : 0, GOnPageLoaded.IsBound() ? 1 : 0);
		UE_LOG(LogBF6Portal, Display, TEXT("-----------------------------"));
	}

	void RegisterCommands()
	{
		IConsoleManager& CM = IConsoleManager::Get();
		GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Open"),
			TEXT("Show the Portal site panel; optional URL to open."),
			FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
				{ BF6PortalWeb::Open(A.Num() ? A[0] : FString()); })));
		GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Dock"),
			TEXT("Show the Portal site panel as a dock tab."),
			FConsoleCommandDelegate::CreateLambda([]{ BF6PortalWeb::ShowDocked(); })));
		GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Overlay"),
			TEXT("Show the Portal site panel over the level viewport."),
			FConsoleCommandDelegate::CreateLambda([]{ BF6PortalWeb::ShowOverlay(); })));
		// The sign-in surface, on demand: the presentation can be looked at and
		// taken down again without signing out and back in to see it.
		GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.SignInSurface"),
			TEXT("BF6.Portal.SignInSurface [on|off]  Put the Portal panel over the whole editor window, or take it back to where it was. No argument toggles."),
			FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
			{
				const bool bOff = A.Num() && (A[0].Equals(TEXT("off"), ESearchCase::IgnoreCase) || A[0] == TEXT("0"));
				const bool bOn  = A.Num() && (A[0].Equals(TEXT("on"),  ESearchCase::IgnoreCase) || A[0] == TEXT("1"));
				if (bOff || (!bOn && BF6PortalWeb::IsSignIn())) BF6PortalWeb::EndSignIn();
				else                                            BF6PortalWeb::ShowSignIn();
			})));
		GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Hide"),
			TEXT("Hide the Portal site panel. The page keeps running."),
			FConsoleCommandDelegate::CreateLambda([]{ BF6PortalWeb::Hide(); })));
		GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.SignOut"),
			TEXT("Clear the saved Portal session (cookies and site storage)."),
			FConsoleCommandDelegate::CreateLambda([]{ BF6PortalWeb::SignOut(); })));
		GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Status"),
			TEXT("Print the Portal panel's state to the Output Log (LogBF6Portal): browser availability, placement, address, session store, linked experience, bridge."),
			FConsoleCommandDelegate::CreateLambda([]{ LogStatus(); })));
		GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.OpenInBrowser"),
			TEXT("Open the page the panel is showing in your own browser."),
			FConsoleCommandDelegate::CreateLambda([]{ BF6PortalWeb::OpenInSystemBrowser(); })));
		GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Exec"),
			TEXT("Run JavaScript on the Portal page: BF6.Portal.Exec <js>"),
			FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
				{ BF6PortalWeb::Exec(FString::Join(A, TEXT(" "))); })));
		GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Link"),
			TEXT("With no argument: start linking your Portal profile (sign in on the page). With one: link the open custom map to a Portal experience, BF6.Portal.Link <experience url or id>"),
			FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
				{
					// ---- BF6PortalProfile ----
					// No argument used to do nothing at all. It now means the
					// other kind of link, which is the one most people want.
					if (A.Num() == 0) { BF6PortalProfile::StartLink(); return; }
					// ---- end BF6PortalProfile ----
					ApplyLink(FString::Join(A, TEXT("")));
				})));
		GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.CopyExportPath"),
			TEXT("Put the last exported .spatial.json path on the clipboard."),
			FConsoleCommandDelegate::CreateLambda([]{ CopyExportPath(); })));
		GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.CookieTest"),
			TEXT("Cookie persistence self test: BF6.Portal.CookieTest write | read (see the Output Log)."),
			FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
				{ CookieTest(A.Num() ? A[0].ToLower() : TEXT("read")); })));
		// Load a .js bundle off disk and register it as an injected script.
		// This is how the page-side half of a bridge is iterated on WITHOUT a
		// recompile: edit the file, run this again, reload the page.
		GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Inject"),
			TEXT("Register a JavaScript file to run after every page load: BF6.Portal.Inject <id> <path to .js>. Omit the path to remove that id."),
			FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& A)
			{
				if (A.Num() < 1)
				{
					UE_LOG(LogBF6Portal, Warning, TEXT("BF6.Portal.Inject <id> <path to .js>"));
					return;
				}
				const FString Id = A[0];
				if (A.Num() < 2) { BF6PortalWeb::RegisterInjectedScript(Id, FString()); return; }
				const FString Path = FString::Join(TArray<FString>(A.GetData() + 1, A.Num() - 1), TEXT(" "));
				FString Src;
				if (!FFileHelper::LoadFileToString(Src, *Path))
				{
					UE_LOG(LogBF6Portal, Error, TEXT("Could not read %s"), *Path);
					return;
				}
				BF6PortalWeb::RegisterInjectedScript(Id, Src);
			})));
		GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.Portal.Reset"),
			TEXT("Close the embedded browser window and open a new one on the same session store."),
			FConsoleCommandDelegate::CreateLambda([]{ ResetWindow(); })));
	}

	// Park a shared pointer forever: the same exit-time trick BF6BuildMode.cpp
	// uses, because destroying Slate widgets during engine teardown crashes.
	template <typename T>
	void LeakForExit(TSharedPtr<T>& P) { if (P.IsValid()) { new TSharedPtr<T>(P); P.Reset(); } }
}

// ---- 6. module hooks ---------------------------------------------------------

void BF6PortalWeb::Register()
{
	LoadConfig();

	// SIGN OUT asked for the whole store to go. It could not be deleted while
	// Chromium held it open, so it is done here, before the browser exists.
	// Say which store this session will use and why, because "signs in every
	// launch" has had three different causes and the log should name the one in
	// play rather than leave it to be worked out again.
	if (UsePrivateStore())
	{
		UE_LOG(LogBF6Portal, Display,
			TEXT("Portal session store: the panel's own, at %s. Session cookies are kept. %s"),
			*PrivateStorePath(),
			PrivateStoreTried()
				? TEXT("It held a session last time.")
				: TEXT("First attempt: if it stays empty the tool falls back on its own next start."));
	}
	else
	{
		UE_LOG(LogBF6Portal, Display,
			TEXT("Portal session store: the browser's ordinary one, at %s. The panel's own store was tried and stayed empty, so it is not used. Cookies with an expiry are kept; a session cookie cannot be."),
			*EngineCacheDir());
	}

	if (FPaths::FileExists(WipeMarker()))
	{
		// THE COOKIE DATABASE ONLY, never the cache around it. The panel shares
		// the editor's browser store, so a sign out deletes the cookie file and
		// leaves everything else alone. That does sign the editor's browser out
		// of other sites too, which is said out loud below rather than hidden.
		const FString Db = CookieDb();
		const bool bGone = !Db.IsEmpty() && IFileManager::Get().Delete(*Db, false, true, true);
		IFileManager::Get().Delete(*WipeMarker(), false, true, true);
		UE_LOG(LogBF6Portal, Display,
			TEXT("Signed out: %s"), bGone
				? TEXT("the browser's cookie database was deleted. Any other site the editor's browser was signed in to is signed out as well.")
				: TEXT("there was no cookie database to delete."));
	}

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(kTabId, FOnSpawnTab::CreateStatic(&SpawnDockTab))
		.SetDisplayName(FText::FromString(TEXT("BF6 Portal")))
		.SetTooltipText(FText::FromString(TEXT("The Portal site, docked beside the viewport. Same page as the PORTAL pill on the ring.")))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

	// The layout is restored during startup; after that, a tab appearing is the
	// user's own doing and is filled normally.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
	{
		GStartupGrace = false;
		return false;
	}), 8.f);

	RegisterCommands();

	GMapOpenedHandle  = BF6Ext::OnMapOpened().AddLambda([](const FString&, const FString&){ GExpCache.Reset(); GMapIdxCache.Reset(); });
	GMapClosingHandle = BF6Ext::OnMapClosing().AddLambda([](const FString&){ GExpCache.Reset(); GMapIdxCache.Reset(); });
	SetStatus(TEXT("Sign in on the page. The tool never sees your password."));

	// ---- BF6PortalProfile ----
	// After the panel's own registration, because it binds a bridge object and
	// an injected script onto the seam the lines above just set up.
	BF6PortalProfile::Register();
	// ---- end BF6PortalProfile ----

	// ---- BF6PortalSettings ----
	// After the profile: the settings panel shows the profile's rotation and
	// its thumbnail panel, and it binds a second bridge object and a second
	// injected script onto the same seam.
	BF6PortalSettings::Register();
	// ---- end BF6PortalSettings ----

	// Said at startup, not at first click, so a build that cannot embed a
	// browser is known before anyone goes looking for the panel.
	if (WebAvailable())
	{
		UE_LOG(LogBF6Portal, Display, TEXT("Portal panel ready. PORTAL on the ring, or BF6.Portal.Open. Store: %s"), *StorePath());
	}
	else
	{
		UE_LOG(LogBF6Portal, Warning, TEXT("Portal panel degraded: %s The panel opens with OPEN IN BROWSER instead."), *GUnavailableWhy);
	}
}

void BF6PortalWeb::Unregister()
{
	// ---- BF6PortalSettings ----
	// Before the profile, because its widgets call into it.
	BF6PortalSettings::Unregister();
	// ---- end BF6PortalSettings ----
	// ---- BF6PortalProfile ----
	// First: it holds a bridge object and a ticker that both reach back in here.
	BF6PortalProfile::Unregister();
	// ---- end BF6PortalProfile ----
	BF6Ext::OnMapOpened().Remove(GMapOpenedHandle);
	BF6Ext::OnMapClosing().Remove(GMapClosingHandle);
	for (IConsoleObject* C : GCmds) if (C) IConsoleManager::Get().UnregisterConsoleObject(C);
	GCmds.Reset();
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(kTabId);
	// The bridge's objects are not ours to keep alive past our own shutdown,
	// and a delegate still holding a lambda into an unloaded module is a crash.
	GBridgeObjects.Reset();
	GInjected.Reset();
	GOnUrlChanged.Clear();
	GOnPageLoaded.Clear();

	if (!FSlateApplication::IsInitialized() || IsEngineExitRequested())
	{
		// Too late to destroy widgets, and the WebBrowser module (loaded after
		// us, so shut down before us) closes its windows itself.
		LeakForExit(GPanel); LeakForExit(GBrowser); LeakForExit(GWindow);
		LeakForExit(GOverlayRoot); LeakForExit(GOverlayColumn); LeakForExit(GOverlayHost); LeakForExit(GDockHost);
		LeakForExit(GSignInRoot); LeakForExit(GSignInSheet); LeakForExit(GSignInHost);
		return;
	}
	Detach();
	if (GPanel.IsValid()) GPanel->ResetBrowserView();
	GPanel.Reset(); GBrowser.Reset();
	if (GWindow.IsValid() && IWebBrowserModule::IsAvailable()) GWindow->CloseBrowser(true, false);
	GWindow.Reset();
	GOverlayRoot.Reset(); GOverlayColumn.Reset(); GOverlayHost.Reset(); GDockHost.Reset();
	GSignInRoot.Reset(); GSignInSheet.Reset(); GSignInHost.Reset();
}

bool BF6PortalWeb::HasSavedSession() { return ::HasSavedSession(); }

void BF6PortalWeb::OpenQuiet(const FString& Url)
{
	// Already on screen because the user put it there: leave it there and just
	// go where it was asked to go.
	if (GPlace == EPlace::Overlay || GPlace == EPlace::Docked || GPlace == EPlace::SignIn)
	{
		BF6PortalWeb::Open(Url);
		return;
	}
	if (GPlace == EPlace::Hidden) AttachOffscreen();
	if (!Url.IsEmpty() && EnsureWindow())
	{
		FString U = Url; U.TrimStartAndEndInline();
		if (!U.Contains(TEXT("://"))) U = TEXT("https://") + U;
		Navigate(U);
	}
}

bool BF6PortalWeb::IsWorkingOffscreen() { return GPlace == EPlace::Offscreen; }

void BF6PortalWeb::ShowSite()
{
	// The one gesture that means "let me watch". An offscreen panel is already
	// loaded and bridged, so this only moves it into a placement that is drawn.
	if (GPlace == EPlace::Overlay || GPlace == EPlace::Docked || GPlace == EPlace::SignIn) return;
	if (GLastPlace == EPlace::Docked) ShowDocked(); else ShowOverlay();
}

void BF6PortalWeb::Open(const FString& Url)
{
	if (GPlace == EPlace::Offscreen)
	{
		// A deliberate OPEN over background work promotes it rather than
		// starting a second one: same window, same page, now visible.
		Detach();
	}
	if (GPlace == EPlace::Hidden)
	{
		if (GLastPlace == EPlace::Docked) ShowDocked(); else ShowOverlay();
		// WHO PUT IT THERE MATTERS. A panel this call raised from nothing was
		// not "up before", and a sign-in that starts in the same frame must not
		// treat it as somewhere to go back to. See ShowSignIn.
		if (GPlace != EPlace::Hidden) GOpenRaisedAtFrame = GFrameCounter;
	}
	if (!Url.IsEmpty() && EnsureWindow())
	{
		FString U = Url; U.TrimStartAndEndInline();
		if (!U.Contains(TEXT("://"))) U = TEXT("https://") + U;
		Navigate(U);
	}
}

// ONE LINE PER MOVE, THE SAME AS THE SIGN-IN SURFACE ALREADY DOES.
//
// ShowSignIn logs both directions and says so in its own comment, but these
// two, the placements a person actually uses, logged nothing at all. A whole
// investigation into "why did the Portal page appear" had no line to grep for
// because a move into the overlay left no trace. It does now.
void BF6PortalWeb::ShowDocked()
{
	if (GPlace == EPlace::Docked) { if (TSharedPtr<SDockTab> T = GDockTab.Pin()) T->DrawAttention(); return; }
	const EPlace Was = GPlace;
	Detach();
	SavePlacement(EPlace::Docked);
	AttachDocked();
	UE_LOG(LogBF6Portal, Display, TEXT("Portal panel: %s -> dock tab."), PlaceName(Was));
}

void BF6PortalWeb::ShowOverlay()
{
	if (GPlace == EPlace::Overlay) return;
	const EPlace Was = GPlace;
	Detach();
	SavePlacement(EPlace::Overlay);
	AttachOverlay();
	UE_LOG(LogBF6Portal, Display, TEXT("Portal panel: %s -> overlay over the viewport."), PlaceName(Was));
}

void BF6PortalWeb::Hide()
{
	const bool bHold = GQuietHold > 0;
	Detach();
	// TAKING IT OFF SCREEN IS NOT CANCELLING IT. Work that is mid-flight keeps
	// its page, its bridge and its callbacks; it just stops being visible.
	if (bHold) AttachOffscreen();
}
void BF6PortalWeb::Toggle() { if (IsShown()) Hide(); else Open(); }
bool BF6PortalWeb::IsShown() { return GPlace != EPlace::Hidden && GPlace != EPlace::Offscreen; }

void BF6PortalWeb::HoldOffscreen(bool bOn)
{
	GQuietHold = FMath::Max(0, GQuietHold + (bOn ? 1 : -1));
}

// ---- the sign-in surface -----------------------------------------------------
//
// ONE LINE IN THE LOG PER MOVE, both ways, naming the placement before and
// after. The last round of this bug was invisible in the log: the panel did not
// cover anything and did not close afterwards, and neither half said so. Now it
// is one grep.

void BF6PortalWeb::ShowSignIn()
{
	if (GPlace == EPlace::SignIn) return;
	const EPlace Was = GPlace;

	// WHERE TO PUT IT BACK, and the one distinction that decides it: a panel
	// the sign-in gesture itself raised (the link buttons call Open() and then
	// StartLink() in one frame) was NOT up before, so there is nothing to go
	// back to and it closes completely when the sign-in is done. Only a panel
	// the user genuinely had open is restored.
	const bool bRaisedByThisGesture = (Was != EPlace::Hidden) && (GOpenRaisedAtFrame == GFrameCounter);
	GBeforeSignIn = bRaisedByThisGesture ? EPlace::Hidden : Was;

	Detach();
	AttachSignIn();
	if (GPlace != EPlace::SignIn)
	{
		// The window went away, or there is not one yet. Better the panel where
		// it was than the panel nowhere and a user waiting at a screen that
		// never changed.
		UE_LOG(LogBF6Portal, Warning,
			TEXT("Portal sign-in surface: could not attach, falling back to the ordinary panel."));
		Open();
		return;
	}
	// The placement key is NOT written: nobody chooses the sign-in surface, so
	// it must never become the placement OPEN puts the panel in next time.
	UE_LOG(LogBF6Portal, Display,
		TEXT("Portal sign-in surface: shown. Placement %s -> %s. When it is done the panel will be %s."),
		PlaceName(Was), PlaceName(GPlace),
		GBeforeSignIn == EPlace::Hidden ? TEXT("closed (it was not open before)") : PlaceName(GBeforeSignIn));
}

bool BF6PortalWeb::IsSignIn() { return GPlace == EPlace::SignIn; }

FString BF6PortalWeb::EndSignIn()
{
	if (GPlace != EPlace::SignIn) return FString();
	const EPlace Back = GBeforeSignIn;
	GBeforeSignIn = EPlace::Hidden;
	Detach();
	// Back exactly where it was, or nowhere at all. The placement key was never
	// touched by the surface, so ShowDocked and ShowOverlay write the same value
	// it already holds and nothing the user chose is lost.
	if (Back == EPlace::Docked)       AttachDocked();
	else if (Back == EPlace::Overlay) AttachOverlay();
	UE_LOG(LogBF6Portal, Display,
		TEXT("Portal sign-in surface: taken down. Placement %s -> %s"),
		PlaceName(EPlace::SignIn), PlaceName(GPlace));

	// One phrase for the status line, because a panel that vanishes needs the
	// same one line of explanation as a panel that appears.
	switch (GPlace)
	{
	case EPlace::Docked:  return FString(TEXT("the Portal panel is back in its dock tab"));
	case EPlace::Overlay: return FString(TEXT("the Portal panel is back in its column over the viewport"));
	default:              return FString(TEXT("the Portal panel closed itself"));
	}
}

void BF6PortalWeb::SignOut()
{
	// Cookies now, through the store's own manager; the page's local and
	// session storage now, through the page; everything else (cache, IndexedDB,
	// service workers) when the editor next starts, because Chromium holds
	// those files open for as long as it runs.
	if (IWebBrowserModule::IsAvailable())
		if (IWebBrowserSingleton* S = IWebBrowserModule::Get().GetSingleton())
			if (TSharedPtr<IWebBrowserCookieManager> CM = S->GetCookieManager(TOptional<FString>(kContextId)))
				CM->DeleteCookies(TEXT(""), TEXT(""), [](int Removed)
					{ UE_LOG(LogBF6Portal, Display, TEXT("Sign out removed %d cookie(s)"), Removed); });
	if (GWindow.IsValid())
	{
		GWindow->ExecuteJavascript(TEXT("try { localStorage.clear(); sessionStorage.clear(); } catch (e) {}"));
		GWindow->LoadURL(LoginUrl());
	}
	// Forget where the panel was, too. Reopening on the last experience page
	// after a sign out would only be a redirect to login with the wrong thing
	// in the address bar.
	GConfig->RemoveKey(kIniSection, TEXT("PortalLastUrl"), GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);

	// The marker lives in the tool's own Saved folder, which may not exist yet
	// on a first run, and never in the engine's cache.
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(WipeMarker()), true);
	FFileHelper::SaveStringToFile(TEXT("delete the BF6 Portal session store at next editor start\n"), *WipeMarker());
	// Loud, because the deletion itself happens at the NEXT editor start, far
	// enough from the click that its log line reads like the site expiring the
	// session rather than something the tool was asked to do.
	UE_LOG(LogBF6Portal, Warning,
		TEXT("Portal SIGN OUT: cookies cleared now, and the whole session store at %s is deleted when the editor next starts. ")
		TEXT("You will have to sign in again. Nothing else asks for this."), *StoreRoot());
	SetStatus(TEXT("Signed out. Cookies and site storage are cleared; the rest of the store is removed when the editor next starts."));
	BF6Api::Toast(TEXT("Portal session cleared."));
}

bool BF6PortalWeb::IsAvailable() { return WebAvailable(); }

FString BF6PortalWeb::CurrentUrl() { return GUrl; }
bool BF6PortalWeb::IsLoading() { return !GWindow.IsValid() || GLoading || GWindow->IsLoading(); }

void BF6PortalWeb::OpenInSystemBrowser()
{
	const FString U = IsPortalUrl(GUrl) ? GUrl : StartUrl();
	FPlatformProcess::LaunchURL(*U, nullptr, nullptr);
	SetStatus(FString::Printf(TEXT("Opened in your browser: %s"), *U));
	UE_LOG(LogBF6Portal, Display, TEXT("Handed to the system browser: %s"), *U);
}

void BF6PortalWeb::Exec(const FString& Js)
{
	if (Js.IsEmpty()) return;
	if (!EnsureWindow()) return;
	GWindow->ExecuteJavascript(Js);
}

// ---- the script bridge seam -------------------------------------------------

void BF6PortalWeb::RegisterBridgeObject(const FString& Name, UObject* Object)
{
	if (Name.IsEmpty() || !Object) return;
	GBridgeObjects.Add(Name, Object);
	// Bind now if there is a window; otherwise EnsureWindow does it when one
	// is made. Either way the caller registers once and stops thinking about it.
	if (GWindow.IsValid()) GWindow->BindUObject(Name, Object, true);
	UE_LOG(LogBF6Portal, Display, TEXT("Bridge object '%s' registered: the page sees it as window.ue.%s"), *Name, *Name.ToLower());
}

void BF6PortalWeb::UnregisterBridgeObject(const FString& Name)
{
	TWeakObjectPtr<UObject> Was;
	if (!GBridgeObjects.RemoveAndCopyValue(Name, Was)) return;
	if (GWindow.IsValid() && Was.IsValid()) GWindow->UnbindUObject(Name, Was.Get(), true);
	UE_LOG(LogBF6Portal, Display, TEXT("Bridge object '%s' unregistered."), *Name);
}

void BF6PortalWeb::RegisterInjectedScript(const FString& Id, const FString& Source)
{
	if (Id.IsEmpty()) return;
	const int32 At = GInjected.IndexOfByPredicate([&Id](const TPair<FString, FString>& P){ return P.Key == Id; });
	if (Source.IsEmpty())
	{
		if (At != INDEX_NONE) GInjected.RemoveAt(At);
		UE_LOG(LogBF6Portal, Display, TEXT("Injected script '%s' removed. It stays on the page until the next load."), *Id);
		return;
	}
	if (At != INDEX_NONE) GInjected[At].Value = Source;
	else                  GInjected.Emplace(Id, Source);

	// A page is already up: run it now rather than making the caller wait for a
	// navigation that may never come (the site is a single-page app).
	if (GWindow.IsValid() && !GLoading) GWindow->ExecuteJavascript(Source);
	UE_LOG(LogBF6Portal, Display, TEXT("Injected script '%s' registered (%d chars); it runs after every page load."), *Id, Source.Len());
}

BF6PortalWeb::FBF6PortalUrlChanged& BF6PortalWeb::OnUrlChanged()  { return GOnUrlChanged; }
BF6PortalWeb::FBF6PortalPageLoaded& BF6PortalWeb::OnPageLoaded()  { return GOnPageLoaded; }

FString BF6PortalWeb::ExperienceForSave(const FString& Level, const FString& Save)
{
	if (Level.IsEmpty() || Save.IsEmpty()) return FString();
	const FString Key = CacheKey(Level, Save);
	if (const FString* Hit = GExpCache.Find(Key)) return *Hit;
	const FString Exp = ReadExperienceFromFile(Level, Save);
	GExpCache.Add(Key, Exp);
	return Exp;
}

// ---- BF6PortalProfile ----

FString BF6PortalWeb::BaseUrl() { return GBaseUrl; }

void BF6PortalWeb::SetSaveLink(const FString& Level, const FString& Save, const FString& Url, int32 MapIdx)
{
	if (Level.IsEmpty() || Save.IsEmpty()) return;
	const FString Key = CacheKey(Level, Save);
	GExpCache.Add(Key, Url);
	GMapIdxCache.Add(Key, MapIdx);
}

int32 BF6PortalWeb::MapIdxForSave(const FString& Level, const FString& Save)
{
	if (Level.IsEmpty() || Save.IsEmpty()) return -1;
	const FString Key = CacheKey(Level, Save);
	if (const int32* Hit = GMapIdxCache.Find(Key)) return *Hit;
	const int32 Idx = ReadMapIdxFromFile(Level, Save);
	GMapIdxCache.Add(Key, Idx);
	return Idx;
}

// ---- end BF6PortalProfile ----

void BF6PortalWeb::NoteExport(const FString& Path)
{
	GLastExport = FPaths::ConvertRelativePathToFull(Path);
}

bool BF6PortalWeb::WantsKeyboard()
{
	if (GPlace == EPlace::Hidden || !GPanel.IsValid() || !FSlateApplication::IsInitialized()) return false;
	TSharedPtr<SWidget> W = FSlateApplication::Get().GetKeyboardFocusedWidget();
	while (W.IsValid())
	{
		if (W == GPanel) return true;
		W = W->GetParentWidget();
	}
	return false;
}

bool BF6PortalWeb::PathTouchesPanel(const FWidgetPath& Path)
{
	if (GPlace == EPlace::Hidden || !Path.IsValid()) return false;
	if (GOverlayColumn.IsValid() && Path.ContainsWidget(GOverlayColumn.Get())) return true;
	// The sign-in sheet is the panel plus its own title row, and all of it
	// belongs to the page rather than to the viewport underneath.
	if (GPlace == EPlace::SignIn && GSignInSheet.IsValid() && Path.ContainsWidget(GSignInSheet.Get())) return true;
	return GPanel.IsValid() && Path.ContainsWidget(GPanel.Get());
}

bool BF6PortalWeb::CursorOverPanel(const FVector2D& ScreenPos)
{
	if (GPlace == EPlace::Hidden || !FSlateApplication::IsInitialized()) return false;
	FSlateApplication& App = FSlateApplication::Get();
	const FWidgetPath Path = App.LocateWindowUnderMouse(ScreenPos, App.GetInteractiveTopLevelWindows());
	return PathTouchesPanel(Path);
}
