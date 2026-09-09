#pragma once

#include "CoreMinimal.h"

class FWidgetPath;
class SWidget;

// ============================================================================
// THE FULL-SCREEN EDITOR HOST.
//
// BLOCKS, SCRIPT, UI and EXPERIENCE are four pages that want the whole screen,
// not a corner of it. Each one is a button in the build screen's top toolbar
// row: press it and its editor covers the level viewport, press it again and
// the world comes back. Only one is ever up.
//
// NOT ONLY BROWSER PAGES. The first three are the tool's own web pages; the
// fourth, EXPERIENCE, is a plain Slate screen. The host never cared: it places
// an SWidget and asks nothing about what is inside it.
//
// ONE WIDGET, MANY PLACEMENTS. The host never builds an editor. It asks the
// feature for the widget it already has (BF6Blocks::Widget and its three
// neighbours), places it, and hands it back when it hides. Nothing is
// destroyed on the way in or out, so the loaded workspace, the open script
// project and everything typed on the page survive being toggled all day.
//
// THE DOCK TABS STAY. Window > Tools still opens each editor as an ordinary
// dock tab for anyone who would rather have it beside the viewport. The two
// placements share one widget the way the Portal panel's do: whichever one
// takes it, the other gives it up. EXPERIENCE has no dock tab of its own; it
// is simply parked, still built, when the sheet comes down.
//
// WHAT IT COVERS. Everything below the toolbar row. The row itself stays
// visible above the sheet, which is what makes the highlighted button the way
// back out.
//
// AND WHEN IT COVERS NOTHING AT ALL. The sheet belongs to the BUILD SCREEN.
// The map selection screen has no viewport to cover and no toolbar row to get
// back out through, so a sheet there is a wall with no door: the tool looks
// like it has hung, and a map opened behind it looks like a click that did
// nothing. That is not a rule the callers keep any more, it is enforced here,
// twice: Show refuses off the build screen, and a watchdog takes any sheet
// down the moment the build screen goes away by any route at all, including
// the first frames after launch, before the tool UI even exists.
// ============================================================================
namespace BF6EditorOverlay
{
	enum class EEditor : uint8 { None, Blocks, Script, Log, Caps, Ui, Experience };

	// The height of the build screen's top toolbar row, in pixels. The host
	// leaves exactly this much of the viewport uncovered, so the row and the
	// sheet below it are laid out from one number.
	constexpr float TopBarHeight = 40.f;

	// Called from BF6Api::InstallInputHandler / RemoveInputHandler: console
	// commands and the remembered editor in, everything out.
	void Register();
	void Unregister();

	// Cover the viewport with one editor. None hides. Showing the editor that
	// is already up does nothing; use Toggle for the button behaviour.
	void Show(EEditor Which);
	void Hide();
	void Toggle(EEditor Which);

	// Take the sheet down WITHOUT forgetting which editor was up: what the map
	// screen calls when the build screen goes away. The page is not closed, and
	// RestoreRemembered puts it straight back.
	void Suspend();

	// Give the widget back and hide, but only if THIS editor is the one up.
	// What a dock tab calls when it wants its page in the tab instead.
	void HideIfShowing(EEditor Which);

	EEditor Active();
	bool    IsShown();
	bool    IsShowing(EEditor Which);

	// The editor the user left up last session, put back the first time the
	// build screen appears. Called by the build HUD, not by the user.
	void RestoreRemembered();

	// One line per fact, for BF6.Editors.Status.
	FString Status();

	// ---- keeping the tool's input handler off the page ----------------------
	// The same three questions the Portal panel answers, for the same reason:
	// the page renders in an SViewport, which is exactly what the tool reads as
	// "the 3D view", and the tool's single-key binds (Space, F1, T, Delete)
	// must never fire on what someone is typing here.
	bool WantsKeyboard();
	bool PathTouchesHost(const FWidgetPath& Path);
	bool CursorOverHost(const FVector2D& ScreenPos);
}
