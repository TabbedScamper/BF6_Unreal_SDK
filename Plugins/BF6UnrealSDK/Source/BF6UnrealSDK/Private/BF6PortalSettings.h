#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "BF6PortalSettings.generated.h"

// Forward declarations at GLOBAL scope, above the namespace: written inside it,
// "class SWidget" would declare BF6PortalSettings::SWidget instead.
class SWidget;

// ============================================================================
// The Portal SETTINGS side of the tool: everything the site's experience editor
// can set - mode, map rotation, teams, the five modifier pages, the four
// restriction pages and the publish steps - edited in the tool's own UI and
// applied through the site's own controls.
//
// THE TOOL NEVER COMPOSES A SAVE. The community's hard-won warning is that a
// PARTIAL updatePlayElement wipes an experience's attachments, so nothing in
// this feature builds one. Every change is made the way a user would make it:
// the panel puts the site on the right page, clicks the toggle, types into the
// slider's number box, picks the dropdown option, and reads the page back to
// see what it actually became. SAVE ON PORTAL presses the site's OWN save
// button and reports its verdict. Publishing is never automated: the publish
// button on step three is the user's click and only the user's click.
//
// NOTHING SECRET CROSSES THIS SEAM. No password, no token, no cookie and no
// request header. The page half decodes the mutator values out of a response
// the page already received, and only the decoded numbers come back.
//
// The catalogue of what the site can set ships with the tool
// (Resources/portal/settings_catalog.json, generated from the site mine) and is
// merged with a LIVE SCRAPE of whatever page the panel is on, cached under
// Saved/BF6UnrealSDK/portal/settings_catalog.live.json. A site update that adds
// or renames a setting therefore reaches the panel without a tool release.
// ============================================================================

// The page -> tool bridge, bound as window.ue.bf6portalsettings, so
// Resources/portal/settings.js calls window.ue.bf6portalsettings.report('<json>').
// One JSON string, and nothing else.
UCLASS()
class UBF6PortalSettingsBridge : public UObject
{
	GENERATED_BODY()

public:
	// Everything the page half says, discriminated by its "kind" field:
	//   page          the section the panel is on, its selector self-test, and a
	//                 scrape of every control on it
	//   mutators      the experience's live values, decoded in the page
	//   applied       one control was set (or was not), with what it now reads
	//   apply-done    the end of one page's batch
	//   save          the verdict of the site's own save
	//   restriction / restriction-bulk / map / selftest
	UFUNCTION()
	void report(FString Json);
};

// The two types the seam borrows, declared here so the header stays cheap. They
// have to be at file scope: inside the namespace they would name new types.
class FJsonObject;
class FJsonValue;

namespace BF6PortalSettings
{
	void Register();
	void Unregister();

	// The SETTINGS column, beside the profile column inside the Portal panel,
	// and the same widget in its own dock tab.
	TSharedRef<SWidget> MakeColumn();

	// Ask the page for a fresh scrape of whatever section it is on, and ask the
	// site for the experience's values again.
	void Refresh();

	// Queue one change. Team -1 means the setting has one control; otherwise it
	// is the team COLUMN index, 0 for team 1. Value is parsed against the
	// control's kind: on/off/true/false for a toggle, a number for a slider, an
	// option label (or its value) for a dropdown, the text itself for a field.
	bool Set(const FString& TestId, int32 Team, const FString& Value);

	// Reset one queued change back to what the site currently has.
	void Reset(const FString& TestId, int32 Team);

	// Walk the pending changes page by page: put the site on the page, set each
	// control there, verify, and report. Changes that failed stay pending with
	// the reason.
	void Apply();

	// Press the site's own save button on the page the panel is on and report
	// what it said.
	void SaveOnPortal();

	void LogStatus();

	// The live experience's mutator values, in the shape the site's own full
	// experience export uses: one key per mutator, a bare number when it has a
	// single value and, when it has one value per team, an array of [team,
	// value] PAIRS - which is how the site writes it, verified against a real
	// export ("FactionID_PerTeam": [[0, 607944106], [1, -1865993703]]).
	// False when nothing has been read yet, or when what was read belongs to a
	// different experience than the one asked for (pass an empty id to accept
	// whatever is loaded). The block editor fills a full export with this.
	bool ExperienceMutators(const FString& ExperienceId, TSharedPtr<FJsonObject>& Out);

	// The team composition as the site writes it, [[1, {"humanCapacity": n}], ...],
	// and the asset restrictions object. Both empty until something has been
	// read, from the site or from a file.
	bool ExperienceTeams(const FString& ExperienceId, TArray<TSharedPtr<FJsonValue>>& Out);
	bool ExperienceRestrictions(const FString& ExperienceId, TSharedPtr<FJsonObject>& Out);

	// Take the settings half of a whole-experience JSON export: the mutators,
	// the team composition and the asset restrictions become the tool's live
	// values for ExperienceId. NO NETWORK IS INVOLVED, which is what makes the
	// whole import path testable with the panel closed and the site unreachable.
	// Returns the number of mutators taken.
	int32 LoadExperienceJson(const FString& ExperienceId, const TSharedPtr<FJsonObject>& Root);

	// ---- WATCH THE SITE -----------------------------------------------------
	// Follow what a person does on the site's settings pages, live, so the
	// panel's values move as theirs move. It OBSERVES AND NEVER WRITES. Driven
	// by the profile's one switch, so this is only the settings half of it.
	void    SetWatching(bool bOn);
	FString WatchStatus();
}
