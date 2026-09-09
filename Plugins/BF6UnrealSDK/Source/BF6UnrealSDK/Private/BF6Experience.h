#pragma once

#include "CoreMinimal.h"

class SWidget;

// ============================================================================
// THE EXPERIENCE SCREEN: the tool's own view of the Portal experience the open
// project belongs to.
//
// The Portal panel is the SITE and nothing else. Everything the tool knows
// ABOUT an experience - who it belongs to, what its rotation is, what its
// settings are, whether it has been imported here - used to ride along beside
// that page as two extra columns, which read as tabs inside a browser. It is
// not the site's screen, so it is not in the site's window: it is a fourth
// full-screen editor, opened by the EXPERIENCE button in the build screen's
// top toolbar row, beside BLOCKS, SCRIPT and UI.
//
// NOTHING HERE IS A SECOND COPY OF ANYTHING. The screen is a rail and a frame:
// every section it shows is a widget its own feature already builds -
// BF6PortalProfile for identity, maps and the account, BF6PortalSettings for
// mode, teams, modifiers, restrictions and publish. This file owns the layout,
// the rail, and the one question the button needs answered: is the open project
// an experience.
//
// WHEN IT IS AVAILABLE. The open save carries a Portal experience link, or the
// profile's model puts the open level and save in some experience's rotation.
// Neither is true and the button is present but off, saying why.
// ============================================================================
namespace BF6Experience
{
	// Console commands in, console commands out. Called beside
	// BF6EditorOverlay::Register / Unregister.
	void Register();
	void Unregister();

	// THE screen, made on the first ask and kept for the life of the editor
	// session, the way the other three full-screen editors keep theirs: what is
	// typed into the settings section survives being toggled away and back.
	TSharedRef<SWidget> Widget();

	// The full-screen host is done with it. There is no dock tab for this one,
	// so the widget is simply parked, still built, until something asks again.
	void ReleaseWidget();

	// Put the screen over the viewport (BF6.Experience.Open, the toolbar
	// button). Works whether or not an experience is detected: with none, the
	// screen says so rather than refusing to open.
	void Open();

	// ---- detection ----------------------------------------------------------
	// Is the open project an experience on the site. Answered from the save's
	// own link first, then from the profile's model. Throttled, because the
	// toolbar button asks every frame.
	bool IsExperienceOpen();

	// The experience id the open project belongs to, or empty.
	FString CurrentId();

	// Why the button is off, in one sentence for its tooltip. Empty when it is
	// on.
	FString WhyUnavailable();

	// One line per fact, for BF6.Experience.Status.
	FString Status();
}
