#pragma once

#include "CoreMinimal.h"

class SWidget;
class AActor;

// ============================================================================
// The add-on seam.
//
// Everything an add-on plugin needs to attach itself to the tool, and nothing
// else. An add-on is an ordinary Unreal plugin dropped anywhere under the
// project's Plugins folder (Plugins/Add-Ons/<Name>/<Name>.uplugin is picked up
// by the engine's own recursive scan, so no loader lives here); it depends on
// the BF6UnrealSDK module, registers what it wants from StartupModule, and
// unregisters in ShutdownModule. Removing the folder removes the add-on with
// no trace left in the tool.
//
// THE CONTRACT, both directions:
//
//  - The tool never reaches into an add-on. It calls only what was registered.
//  - An add-on never edits the tool's files, its levels, or its saves. Objects
//    it spawns carry AddonTag() and none of the tool's own tags, which is what
//    keeps them out of the export, the physics budget, the scene tree, the
//    placement rays and the save file. Use MarkAddonActor() and that is true
//    by construction.
//  - Anything here is versioned by ApiVersion(). It only ever grows: existing
//    signatures do not change meaning, so an add-on built against an older
//    version keeps working. Check it at startup if you need a newer field.
// ============================================================================

namespace BF6Ext
{
	// Bumped when something is ADDED. Never when something changes meaning,
	// because that is not allowed to happen.
	BF6UNREALSDK_API int32 ApiVersion();

	// ---- the radial's front ring -------------------------------------------
	//
	// One pill, sitting alongside OBJECTS, VALIDATE and the rest. The add-on
	// owns whatever it opens from there; submenus are its own popups, and the
	// tool's own wheel is not touched.
	struct FPieEntry
	{
		// Unique, and how the entry is removed again. Prefix it with the
		// add-on's name so two add-ons cannot collide: "HighPoly.Root".
		FName   Id;

		// The pill's text, and the small line under it. Uppercase reads right
		// on the ring. No emoji, no em dashes: the tool's UI voice is plain.
		FString Label;
		FString Sub;

		// Where it sits, low first. The tool's own entries are 100 apart
		// starting at 100, so 250 lands between the second and third.
		int32   Order = 1000;

		// Optional. Return false to leave the pill off the ring for now (no
		// map open, data still downloading). Null means always shown.
		TFunction<bool()> IsAvailable;

		// Clicked. The argument is the wheel's screen centre, which is where a
		// popup should open so it lands under the cursor.
		TFunction<void(FVector2D /*ScreenCenter*/)> OnPick;
	};

	BF6UNREALSDK_API void RegisterPieEntry(const FPieEntry& Entry);
	BF6UNREALSDK_API void UnregisterPieEntry(FName Id);

	// ---- a sub-ring on the tool's own wheel --------------------------------
	//
	// One page of an add-on's pills, drawn by the wheel itself: same geometry,
	// same hub, same confirmation, so the add-on's controls read as the
	// tool's. Sub is read LIVE - it is where a toggle answers ("on", "still",
	// "3 built") - and a toggle (bCloses false) runs its OnPick and the wheel
	// REBUILDS in place so that answer shows immediately. An action
	// (bCloses true) closes the wheel like any other pick. The tool appends
	// < BACK itself; do not add one.
	struct FPieSubEntry
	{
		FString              Label;    // uppercase reads right on the ring
		TFunction<FString()> Sub;      // the small line under it, read live
		TFunction<void()>    OnPick;
		bool                 bCloses = false;
	};

	// Call from a front-ring pill's OnPick, handing it the ScreenCenter the
	// pick gave you: the wheel reopens there showing these entries.
	BF6UNREALSDK_API void OpenPieSubRing(const TArray<FPieSubEntry>& Entries,
	                                     FVector2D ScreenCenter);

	// ---- what the tool is doing --------------------------------------------

	// "MP_Badlands", or empty when no map is open.
	BF6UNREALSDK_API FString CurrentLevel();
	// The custom map's name, empty while a base map is being previewed.
	BF6UNREALSDK_API FString CurrentSave();
	// True once a custom map exists. False on a read-only base preview, where
	// an add-on may show things but must not change anything.
	BF6UNREALSDK_API bool    IsEditing();
	// On foot rather than flying. An overlay may want a different budget here.
	BF6UNREALSDK_API bool    IsWalking();

	// Open one of the SDK's map sessions through the tool's normal loader. This
	// exists for deterministic add-on benches and automation: it performs the
	// same validation, context load, save restore and map-open broadcasts as a
	// click on the map card. Empty SaveName opens the read-only base map.
	BF6UNREALSDK_API void    OpenMap(const FString& Level, const FString& SaveName);
	BF6UNREALSDK_API void    ShowBuildOverlay();

	// The exact Display/Sun -> Map image control.  State values match the
	// workspace button: 0 off, 1 hidden, 2 downloading, 3 shown.  SetVisible
	// is idempotent so automation never has to guess which way a toggle moves.
	BF6UNREALSDK_API int32   MapImageState();
	BF6UNREALSDK_API void    SetMapImageVisible(bool bVisible);

	// Apply only validator fixes explicitly marked safe by the SDK (currently
	// polygon winding), then persist the current editable save when requested.
	// Returns the number of distinct actors changed; read-only base previews are
	// never modified.
	BF6UNREALSDK_API int32   FixSafeValidationIssues(bool bSave);

	// A map finished loading (Level, SaveName), and the map is being torn down.
	// Both fire on the game thread. Closing fires before the actors go.
	DECLARE_MULTICAST_DELEGATE_TwoParams(FBF6MapOpened, const FString&, const FString&);
	DECLARE_MULTICAST_DELEGATE_OneParam(FBF6MapClosing, const FString&);
	BF6UNREALSDK_API FBF6MapOpened&  OnMapOpened();
	BF6UNREALSDK_API FBF6MapClosing& OnMapClosing();

	// ---- where things are --------------------------------------------------

	// The Battlefield 6 install, or empty if the tool has not found one. Read
	// only: an add-on decodes from here, it never writes.
	BF6UNREALSDK_API FString GameInstallDir();
	// The Portal SDK the tool is using, or empty.
	BF6UNREALSDK_API FString SdkRoot();
	// The tool's own plugin folder, for reading its shipped data.
	BF6UNREALSDK_API FString ToolPluginDir();
	// Saved/BF6UnrealSDK. An add-on should keep its caches in a subfolder of
	// its own name so uninstalling it leaves nothing behind.
	BF6UNREALSDK_API FString ToolSavedDir();

	// ---- staying out of the tool's way -------------------------------------

	// The tag that marks an actor as belonging to an add-on. The tool skips
	// these everywhere it walks its own objects.
	BF6UNREALSDK_API FName AddonTag();
	// Stamp it, plus an owner tag ("addon:HighPoly") so a crash or a reload can
	// still tell whose actor it is. Call this on everything you spawn.
	BF6UNREALSDK_API void  MarkAddonActor(AActor* A, const FString& AddonName);
	// Remove every actor stamped by this add-on, in one pass. The tool calls
	// nothing of yours during teardown, so this is how an overlay clears itself.
	BF6UNREALSDK_API int32 ClearAddonActors(const FString& AddonName);

	// ---- borrowing the tool's chrome ---------------------------------------

	// A popup with the tool's own lifetime handling: it closes when the user
	// clicks away, and the radial knows it is up. Add-on panels should use it
	// rather than PushMenu, or the wheel and the popup fight over the mouse.
	BF6UNREALSDK_API void ShowPopup(TSharedRef<SWidget> Content, FVector2D ScreenPos);
	// The tool's toast, bottom right.
	BF6UNREALSDK_API void Notify(const FString& Message);

	// Hide or show the tool's own low-poly map: the terrain and the asset mesh
	// it draws so you always know where you are. An add-on that draws the real
	// thing in the same place wants these out of the way, and asks rather than
	// reaching in, so the tool keeps owning its own actors and can put them back.
	// Returns how many it changed.
	BF6UNREALSDK_API int32 SetLowPolyMapHidden(bool bHidden);
	BF6UNREALSDK_API bool  IsLowPolyMapHidden();

	// The current selection, with nodes expanded to the objects underneath, and
	// add-on actors left out. This is what "the selection" means to the tool.
	BF6UNREALSDK_API void  Selection(TArray<AActor*>& Out);
	// The surface point straight ahead of the camera, on terrain or on a placed
	// object. False when there is no viewport or nothing was hit.
	BF6UNREALSDK_API bool  WorldAheadOfCamera(FVector& OutWorld);

	// The exact level viewport carrying the BF6 workspace overlay. Generic
	// editor viewport globals can point at hidden preview clients, so add-ons
	// and MCP automation must use this seam when camera/clipmap agreement
	// matters.
	BF6UNREALSDK_API bool GetBuildViewportCamera(FVector& OutLocation, FRotator& OutRotation);
	BF6UNREALSDK_API bool SetBuildViewportCamera(const FVector& Location, const FRotator& Rotation);
	BF6UNREALSDK_API bool RedrawBuildViewport();
}
