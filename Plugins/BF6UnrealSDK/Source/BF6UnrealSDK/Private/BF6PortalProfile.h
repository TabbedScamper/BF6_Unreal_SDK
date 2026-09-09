#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "BF6PortalProfile.generated.h"

// Forward declarations at GLOBAL scope, above the namespace: written inside it,
// "class SWidget" would declare BF6PortalProfile::SWidget instead.
class SWidget;
class FJsonObject;
struct FSlateBrush;

// ============================================================================
// The Portal PROFILE: the tool's own read of the account the user signed into
// on the page, and everything that follows from it - the experience list, their
// map rotations, their spatial attachments, and importing one as a custom map.
//
// NOTHING SECRET CROSSES THIS SEAM. The user signs in on the real site inside
// the panel; the tool never reads, stores, logs or forwards a password, a
// token, a cookie or a request header. The injected page script may reuse the
// page's OWN request headers inside the page to replay a request the site
// already made, but only RESPONSE BODIES ever reach this file.
//
// Persistence of the sign-in is the browser's cookie store, which BF6PortalWeb
// already owns. All this file persists is the state name and the display name
// the page showed.
// ============================================================================

// The page -> tool bridge. Bound as window.ue.bf6portal, so Resources/portal/
// capture.js calls window.ue.bf6portal.capture('<json>') and .pagestate('<json>').
// Both take one JSON string and nothing else.
UCLASS()
class UBF6PortalBridge : public UObject
{
	GENERATED_BODY()

public:
	// A gRPC-web response body the page saw, base64 in a JSON envelope. Bodies
	// over half a megabyte arrive as sequential chunks carrying {id, seq, of}.
	UFUNCTION()
	void capture(FString Json);

	// The page probe after every load and every URL change:
	// {v, kind, url, hasBlockly, signedInHint, thumbs:[{id,name,src}]}
	UFUNCTION()
	void pagestate(FString Json);
};

namespace BF6PortalProfile
{
	enum class EState : uint8
	{
		Unlinked,    // never linked, or unlinked by hand
		SigningIn,   // the panel is on the login page, waiting for evidence
		Linked,      // proved: the experiences page AND a parsed 200 from the API
		Expired      // was linked; the page bounced to login, or the API refused
	};

	// One experience as the tool knows it. Everything here came out of a
	// response body the page received.
	struct FExperienceRow
	{
		FString Id;
		FString Name;
		FString Description;
		FString ThumbUrl;        // resolved by the page where possible
		FString Maps;            // the rotation, joined for display
		int64   UpdatedUnix = 0;
		bool    bFetched = false;    // a getPlayElement has been parsed for it
		bool    bNoMapData = false;  // fetched, and no spatial attachment at all
		bool    bImported = false;   // at least one save on disk belongs to it
	};

	// One slot of an experience's map rotation. Blocks, script and settings are
	// per experience and shared by every map; only the map differs.
	struct FRotationRow
	{
		int32   MapIdx = 0;
		FString Map;           // the codename, e.g. MP_Capstone
		FString SaveName;      // the tool's save for this slot, or empty
		bool    bHasSpatial = false;
	};

	void Register();
	void Unregister();

	// ---- session loss and recovery ------------------------------------------
	//
	// The site signs people out after a while, and on its own updates. NOTHING
	// THE USER IS EDITING MAY BE LOST when that happens, so the tool detects it
	// the moment it happens, says so, and gets the session back on its own -
	// without ever touching a credential, because the browser's persistent
	// cookie store usually still carries the EA sign-in and a reload is enough.
	//
	// The blocks and script editors subscribe to these two. On lost they must
	// stop pushing to the site and journal locally instead; on restored they
	// re-apply what they journaled. Both fire on the game thread.
	DECLARE_MULTICAST_DELEGATE(FBF6PortalSessionEvent);
	FBF6PortalSessionEvent& OnSessionLost();
	FBF6PortalSessionEvent& OnSessionRestored();

	bool    IsSessionLost();
	FString LastLossReason();       // why the tool decided the session was gone
	FString LastRecoveryResult();   // what the last recovery attempt did
	FString Banner();               // the panel's warning line, or empty

	// The keep-alive. On by default, remembered per project as
	// [BF6UnrealSDK] PortalKeepAlive.
	bool KeepAliveEnabled();
	void SetKeepAliveEnabled(bool bOn);

	// Pretend the session was lost, or restored, so the editors' journaling can
	// be tested without waiting for a real logout. Simulated loss does NOT try
	// to recover: it is there to watch the subscribers, not the reload.
	void SimulateSession(bool bLost);
	void LogSession();

	// ---- the state machine --------------------------------------------------
	EState  State();
	FString StateLabel();      // the pill's text
	FString AccountName();     // as the PAGE reported it, or empty
	FString PageStatus();      // the page-check line ("expected X got Y")
	FString WorkStatus();      // what the importer is doing

	void StartLink();          // covers the editor window with the login page and waits
	// The way out of the sign-in surface that is not signing in: the panel goes
	// back where it was and the profile state is left alone. CANCEL on the
	// surface and Escape both call this; it does nothing when the surface is
	// not up.
	void CancelLink();
	void Unlink();             // BF6PortalWeb::SignOut() plus a state reset

	// ---- the API path -------------------------------------------------------
	//
	// Reading and writing go through the site's OWN api, replayed from inside
	// the page with the page's own headers. Driving the site's pages is the last
	// resort, kept for the case where the site has not yet made the call the
	// tool wants to imitate. Nothing here ever sees a credential: the page does
	// the replay, and only response bodies come back.
	bool    ApiReady();      // a WebPlay request has been seen and can be imitated
	FString ApiStatus();     // one line for the panel and the log
	void    RefreshList();   // ask for the owned list again, now

	// ---- the model ----------------------------------------------------------
	bool                   HasExperiences();
	// Whether the map screen should draw the experience list at all, and what to
	// call it. While signed out only the experiences imported onto this machine
	// are shown: the rest cannot be opened and are not ours to advertise.
	bool                   HasImportedExperiences();
	bool                   ShowExperienceList();
	FString                ExperienceListHeading();
	// The list the panel shows, which is the search filter applied to the whole
	// list. ListAll ignores the filter.
	TArray<FExperienceRow> List();
	TArray<FExperienceRow> ListAll();

	// THE CARD FINGERPRINT, and why it is not the UI one.
	//
	// This advances ONLY when something a card actually shows changes: the set
	// of ids, the names, the map summary, the updated date, the imported mark,
	// the thumbnail. It is deliberately deaf to everything else the panel does -
	// status lines, page probes, the recovery ticker - because the map screen
	// shows 38 cards and rebuilding all of them to change one sentence of status
	// text is what made that screen crawl.
	//
	// UiFingerprint is the old behaviour, for the parts of the panel that really
	// do change with any of that.
	uint32 ListFingerprint();
	uint32 UiFingerprint();
	// How many times the grid has rebuilt this session, so the cost is a number
	// rather than a feeling. Printed by BF6.Portal.Profile.
	void   NoteGridRebuilt();
	// What a card draws, out of the cache the fingerprint recompute fills, so a
	// bound attribute is a map lookup and never a walk of the list or a question
	// to the disk.
	FString CardSubtitle(const FString& Id);
	bool    CardImported(const FString& Id);
	// True only when the experience has a thumbnail of its OWN. An experience
	// with none draws the site's own game mode art from the local site mirror,
	// with its name over the top the way the site does it, so the card is never
	// a blank panel with a word in it.
	bool    HasOwnThumbnail(const FString& Id);
	uint32                 ListFingerprint();   // changes when a card would look different
	const FSlateBrush*     ThumbnailFor(const FString& Id);   // null until downloaded
	bool                   IsImported(const FString& Id);
	TArray<FRotationRow>   RotationFor(const FString& Id);

	// ---- search -------------------------------------------------------------
	//
	// Finding an experience should never mean reading a uuid out of a cache
	// folder. Every place that takes an experience takes a name, a short id or
	// a full uuid, and says what it matched.
	FString SearchText();
	void    SetSearchText(const FString& Text);
	TArray<FExperienceRow> Search(const FString& Text);
	// One experience id, or empty when nothing or more than one matched.
	// OutWhat says what happened either way, in words fit for a log line.
	FString  Resolve(const FString& Text, FString& OutWhat);
	void     LogFind(const FString& Text);

	// ---- the whole experience, as one file ----------------------------------
	//
	// The site imports and exports a whole experience as one JSON file, and that
	// file carries everything the tool holds: the settings and toggles and teams
	// (mutators, teamComposition, assetRestrictions), the map rotation, the
	// block workspace, the script and strings attachments, and one spatial
	// attachment per map. Reading one needs no network at all.
	bool ImportExperienceFile(const FString& Path);   // empty path opens a picker
	bool ExportExperienceFile(const FString& Path);   // empty path opens a picker

	// THE SITE'S OWN EXPORT, caught before it reaches disk. The experience
	// tile's menu has an Export item that builds the same document entirely in
	// the page and hands it to a download. The page half catches the blob and
	// sends the text over the bridge, so nothing is written to the user's
	// downloads folder and nothing has to be picked out of it by hand. This is
	// the complete route: it carries the spatial, script and workspace data
	// whole. Empty id means the experience the open save belongs to.
	void ExportFromSite(const FString& Id);

	// Rebuild the site's whole-experience document from what the tool holds.
	// Every field of it comes out of the captured getPlayElement, which is the
	// same document in another encoding.
	bool ExperienceJson(const FString& Id, TSharedPtr<FJsonObject>& Out);
	// Compare that rebuild against a file the site produced, field by field, and
	// print every difference. This is the proof that the tool can write the
	// site's export without the site.
	void VerifyAgainstFile(const FString& Path);

	// ---- keeping the two in step -------------------------------------------
	// After the tool saves a map that belongs to an experience, and after a save
	// on the site, fetch that experience again and re-import it in place: the
	// settings, the rotation, the workspace and the attachments. A spatial that
	// did not change is left alone rather than rewritten. On by default,
	// remembered per project as [BF6UnrealSDK] PortalAutoSync, and it stands
	// down while the session is signed out.
	// ---- WATCH THE SITE -----------------------------------------------------
	//
	// One switch for following what a person does on the site while they do it:
	// the settings pages' controls, and the script editor's text. Default OFF.
	// It OBSERVES AND NEVER WRITES: no click, no value set, no request of any
	// kind goes out because of it.
	//
	// The blocks editor already syncs both ways and is not driven from here.
	bool    WatchEnabled();
	void    SetWatchEnabled(bool bOn);
	FString WatchStatus();     // what is being watched, and when it last saw a change
	// The script the site last showed, adopted into the tool's own script
	// project. Never automatic: watching journals it to disk and says so, and
	// this is the deliberate step that overwrites what the tool has.
	void    AdoptWatchedScript();

	bool AutoSyncEnabled();
	void SetAutoSyncEnabled(bool bOn);
	// The tool saved a map. Called from the save path; does nothing when the
	// save does not belong to an experience.
	void NoteToolSaved(const FString& Level, const FString& Save);
	// Send the whole experience back through the site's own updatePlayElement,
	// as the site's OWN last message with only the changed fields different. A
	// message composed from scratch would wipe the attachments, so one is never
	// composed. With no such message to work from this says so and offers the
	// file export instead.
	void    PushExperience();
	FString PushStatus();

	// ---- actions ------------------------------------------------------------
	void ImportOne(const FString& Id);
	void ImportAll();
	void CancelImport();
	bool IsBusy();
	void OpenOnSite(const FString& Id);
	// Import if it has to, open the save, then put the panel on the experience's
	// editor so blocks and settings edit live while the tool feeds it.
	void OpenForEditing(const FString& Id);
	// One click from one map of an experience to another map of the SAME
	// experience: saves the open map, opens the sibling save.
	void SwitchToMap(const FString& Id, int32 MapIdx);

	// The site is a data source as much as a page: it can sit behind the tool's
	// own column while it feeds it.
	bool IsSiteShown();
	void SetSiteShown(bool bShown);

	// ---- the experience thumbnail -------------------------------------------
	//
	// Publish step two, "Select an image to represent your experience", takes
	// either one of the site's pre-approved generic images or a custom one that
	// has to fit the requirements. The requirements are verified from De Luca's
	// template, scripts\export-thumbnail.js: exactly 352 x 248 pixels, JPEG (or
	// PNG), at most 78 KB, produced by scaling to fit, centre-cropping, then
	// dropping JPEG quality from 90 until it is under the cap. The tool does
	// exactly that, so what it hands the site is always accepted.
	static const int32 kThumbW        = 352;
	static const int32 kThumbH        = 248;
	static const int32 kThumbMaxBytes = 78 * 1024;

	// SCREENSHOT MAP: the rendered 3D view, with none of the tool's own chrome.
	bool CaptureViewportForThumbnail();
	// UPLOAD IMAGE: a jpg / png / bmp off disk, or a file picker.
	bool LoadImageForThumbnail(const FString& Path);
	bool PickThumbnailFile();
	// Fit, crop to 352 x 248, then the quality ladder until it is under 78 KB.
	bool BuildThumbnail();

	FString ThumbnailPath();
	FString ThumbnailStatus();
	void    CopyThumbnailPath();
	// Upload through the page and set it on the experience. Never a credential
	// in the tool: the page replays the site's own call with the site's own
	// headers, and the tool only ever hands over the JPEG bytes.
	void    SendThumbnailToSite();
	void    LogThumbnail();

	// ---- the tool's own UI --------------------------------------------------
	// ---- progress, because the site is no longer on screen ------------------
	//
	// Every automated thing the tool does with the site now happens with
	// nothing drawn (BF6PortalWeb::OpenQuiet). That removes the only feedback
	// there was, so these put it back in the tool, in plain words, out of the
	// SAME page reports the tool already receives.
	bool    IsWorking();       // any site work in flight
	FString ProgressLine();    // "Reading map 3 of 11 for Night Ops (Empire State)"
	bool    CanCancelWork();   // there is something CANCEL can stop

	TSharedRef<SWidget> MakeColumn();           // the Portal panel's profile column
	TSharedRef<SWidget> MakeExperienceGrid();   // MY EXPERIENCES on the map screen
	// One line saying what the site work is doing, with SHOW SITE and CANCEL.
	// Collapses to nothing when there is no work. Belongs on the map screen and
	// on the experience screen.
	TSharedRef<SWidget> MakeWorkStrip();
	TSharedRef<SWidget> MakeMapSwitcher();      // "<map> n of m" in the build HUD
	TSharedRef<SWidget> MakeThumbnailPanel();   // THUMBNAIL, in the profile column

	void LogStatus();
}

// Import a .spatial.json straight off a path, with no dialog. Extracted from
// the dialog importer so the Portal profile can import what it downloaded.
//
// SaveNameOverride empty  = exactly the old behaviour: the name comes from the
//                           file, and NO session file is written.
// SaveNameOverride given  = that name is used, PortalExperienceUrl and
//                           PortalMapIdx are attached, and the session is saved.
bool BF6_ImportSpatialFile(const FString& File, const FString& SaveNameOverride,
	const FString& PortalExperienceUrl, int32 PortalMapIdx);

// A rotation slot with no spatial attachment still deserves a save: the map's
// own base setup, named and linked like its siblings, marked as having no map
// data yet. Returns false if the map could not be opened.
bool BF6_CreatePortalBaseSave(const FString& Level, const FString& SaveName,
	const FString& PortalExperienceUrl, int32 PortalMapIdx);
