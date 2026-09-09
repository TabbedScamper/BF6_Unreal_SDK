#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

class FWidgetPath;
class UObject;

// The panel's own log category, so a page that misbehaves is one filter away
// from the rest of the tool: "LogBF6Portal" in the Output Log.
DECLARE_LOG_CATEGORY_EXTERN(LogBF6Portal, Log, All);

// ============================================================================
// The Portal site, inside the editor.
//
// One embedded Chromium window (the engine's WebBrowser module, CEF) showing
// portal.battlefield.com, wrapped in the tool's own chrome. It has three homes
// the user switches between - a resizable dock tab beside the viewport for
// scripting and block editing, a full-height column over the level viewport,
// and the sign-in surface that fills the editor window - and ALL THREE show
// the same browser window: switching never reloads the page or starts a
// second session.
//
// SIGN-IN IS THE USER'S. The tool never reads, stores or types a password or
// a token. "Stay signed in" is nothing more than the browser's own cookie and
// storage files kept under Saved/BF6UnrealSDK/portalweb instead of a temp
// folder, so the EA login survives an editor restart the way it does in
// Chrome. SIGN OUT deletes them.
// ============================================================================
namespace BF6PortalWeb
{
	// StartupModule / ShutdownModule. Registers the dock tab, the console
	// commands and the config; tears everything down in reverse.
	void Register();
	void Unregister();

	// Show the panel in its last placement (overlay the first time), and
	// navigate if a URL is given. Toggle hides it when it is up.
	void Open(const FString& Url = FString());
	void ShowDocked();
	void ShowOverlay();
	void Hide();
	void Toggle();
	bool IsShown();

	// ---- work with nothing on screen ---------------------------------------
	//
	// THE SITE IS FOR SIGNING IN. Everything else the tool does with it - going
	// through an experience's maps, reading the settings pages, pushing,
	// sending a thumbnail - is work the user asked for somewhere else, and none
	// of it is a reason to put a website in front of them.
	//
	// So there is a fourth placement: the panel goes into the level viewport's
	// overlay one pixel wide, clipped and hit-test invisible. It is in the
	// Slate tree and it ticks, so the page loads and, more to the point, the
	// SWebBrowser widget's OnLoadCompleted still runs the injected bundles and
	// still broadcasts OnPageLoaded - which is where every capture the tool
	// lives on comes from. CEF itself would navigate with no widget at all;
	// the tool's own bridge would not.
	//
	// Navigate the page without showing anything. A panel the user already put
	// on screen is left where it is.
	void OpenQuiet(const FString& Url = FString());
	// True while the panel is working offscreen.
	bool IsWorkingOffscreen();
	// SHOW SITE: move an offscreen (or closed) panel into a placement that is
	// drawn. The page, the session and the work in flight are untouched.
	void ShowSite();
	// Is there a browser session on disk from a previous run? True does NOT
	// mean signed in: only the site can say that. It means there is something
	// worth quietly asking about before making the user click anything.
	bool HasSavedSession();

	// Ref-counted: while any hold is on, HIDE drops the panel to offscreen
	// instead of tearing it down, so hiding the site never cancels the work.
	void HoldOffscreen(bool bOn);

	// ---- the sign-in surface ------------------------------------------------
	// A THIRD PLACEMENT, for the one moment a person must touch the site.
	//
	// The dock tab is a tab beside the viewport and the overlay is a column
	// over it; neither covers the tool, and the map screen is not a viewport
	// the tool can be reached around. So signing in has its own placement: the
	// panel fills the EDITOR WINDOW, edge to edge, above everything the tool
	// draws, with a title line saying what is happening and a way out.
	//
	// THE WINDOW, NOT THE MONITOR. The window's own title bar is left clear, so
	// the editor can still be moved, resized, minimised and alt-tabbed away
	// from while the sheet is up. Nothing goes true full screen and no second
	// always-on-top window is created.
	//
	// It is a placement like the other two: the SAME browser window moves into
	// it, so nothing reloads and no second session starts.
	void ShowSignIn();
	bool IsSignIn();
	// Take the sign-in surface down and put the panel back exactly where it was
	// when the surface went up: its dock tab, its column, or NOWHERE. A panel
	// that was not open before the sign-in gesture is closed completely rather
	// than restored, because a dock tab appearing beside the map screen on its
	// own reads as the site failing to close.
	//
	// Returns one phrase saying what it did, for a status line ("the Portal
	// panel closed itself"), or empty when the surface was not up. Safe to call
	// at any time.
	FString EndSignIn();

	// Clear the persistent session: cookies and site storage go now, the rest
	// of the store goes when the editor next starts.
	void SignOut();

	// True when this engine build can actually embed a browser (the WebBrowser
	// module is present AND its CEF binaries are). False means the panel opens
	// as a plain message with OPEN IN BROWSER, and never as a crash.
	bool IsAvailable();

	// The address the page is on, or empty before the first load.
	FString CurrentUrl();

	// Hand the current page (or the Portal front door, if there is none) to the
	// user's real browser. Always available, CEF or not.
	void OpenInSystemBrowser();

	// Run JavaScript on the page (BF6.Portal.Exec). The seam a later bridge
	// into the site's script and block editors hangs off.
	void Exec(const FString& Js);

	// ---- the script bridge seam ---------------------------------------------
	// Everything below exists so a second feature (the live Blockly bridge) can
	// own its own UObject and its own JS, and still ride this one browser
	// window. Nothing here knows what the bridge does.

	// Expose a UObject's UFUNCTIONs to the page as window.ue.<Name>. Register
	// with Name "bf6" and the page calls window.ue.bf6.<function>(...). The
	// binding is re-applied automatically every time the browser window is
	// created or recreated, so registering before the panel has ever been shown
	// is fine. The caller keeps the object alive (root it, or make it a
	// UObject on a rooted outer); a collected object is silently dropped.
	void RegisterBridgeObject(const FString& Name, UObject* Object);
	void UnregisterBridgeObject(const FString& Name);

	// A JS bundle run after EVERY page load, in registration order, before
	// OnPageLoaded fires. Register once under a stable Id; the same Id again
	// replaces the source, and an empty Source removes it. If a page is already
	// loaded the new bundle runs immediately as well, so the bridge does not
	// have to wait for a navigation to come alive.
	void RegisterInjectedScript(const FString& Id, const FString& Source);

	// The page's address changed: a navigation, or the site's own router
	// swapping sections without a reload.
	DECLARE_MULTICAST_DELEGATE_OneParam(FBF6PortalUrlChanged, const FString& /*Url*/);
	FBF6PortalUrlChanged& OnUrlChanged();

	// A page finished loading and the injected bundles have just run.
	DECLARE_MULTICAST_DELEGATE_OneParam(FBF6PortalPageLoaded, const FString& /*Url*/);
	FBF6PortalPageLoaded& OnPageLoaded();

	// The Portal experience URL a save is linked to, or empty. SaveSession
	// writes it into the session file as "portalExperience".
	FString ExperienceForSave(const FString& Level, const FString& Save);

	// ---- BF6PortalProfile ----
	// The site's base address, so the profile builds the same URLs this panel
	// navigates to and honours [BF6UnrealSDK] PortalBaseUrl with it.
	FString BaseUrl();

	// Remember which experience, and which slot of that experience's map
	// rotation, a save belongs to. SaveSession writes both into the session
	// file ("portalExperience" and "portalMapIdx"). MapIdx -1 = not a rotation
	// save, only a link.
	void SetSaveLink(const FString& Level, const FString& Save, const FString& Url, int32 MapIdx);

	// The rotation slot recorded for a save, or -1. Read from the session file
	// on first ask, like the experience link beside it.
	int32 MapIdxForSave(const FString& Level, const FString& Save);
	// ---- end BF6PortalProfile ----

	// BF6_ExportSpatial tells us what it just wrote, so COPY EXPORT PATH is
	// never a guess.
	void NoteExport(const FString& Path);

	// ---- keeping the tool's input handler off the page ----------------------
	// True while keyboard focus is on the page: the tool's single-key binds
	// (Space, F1, T, Delete) must not fire on what the user is typing there.
	bool WantsKeyboard();
	// True when the hit-test path passes through the panel. The page renders
	// in an SViewport, which is exactly what the tool reads as "the 3D view",
	// so without this a click on the site would start a box select.
	bool PathTouchesPanel(const FWidgetPath& Path);
	bool CursorOverPanel(const FVector2D& ScreenPos);
}
