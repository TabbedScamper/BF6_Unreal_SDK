#pragma once

#include "CoreMinimal.h"

// ============================================================================
// Build Mode: the full-screen "building program" experience layered over
// Unreal's real level viewport. This header is the seam between the new Slate
// widgets (implemented in BF6BuildMode.cpp) and the engine/session logic
// (implemented in BF6UnrealSDK.cpp). The docked SBF6Browser tab stays as-is; the
// new system is parallel and self-contained.
// ============================================================================

namespace BF6Api
{
	// One placeable row, flattened so widgets don't touch engine types.
	struct FPlaceableInfo
	{
		FString Type;
		FString Directory;
		FString Mesh;
		int32   PhysicsCost = 0;
	};

	// ---- session state (implemented in BF6UnrealSDK.cpp) ----
	bool     IsEditing();
	FString  CurrentLevel();
	FString  CurrentSave();

	// ---- budget (physics cost) ----
	void         RecomputeBudget();
	FText        BudgetText();
	float        BudgetFrac();       // 0..1 of the level cap (0 if uncapped)
	FLinearColor BudgetColor();
	bool         BudgetOver();

	// ---- placeable catalogue for the current level ----
	// Top-level directory segments present in this level (the radial categories).
	TArray<FString>        Categories();
	int32                  CategoryCount(const FString& Category);
	// Placeables in a category, optional fuzzy filter, capped for the popup list.
	TArray<FPlaceableInfo> PlaceablesIn(const FString& Category, const FString& Query, int32 Max = 400);

	// ---- map list (for the selector) ----
	TArray<FString> AllLevels();
	FString         DisplayName(const FString& Level);   // "MP_Dumbo" -> "Manhattan Bridge"
	int32           PlaceableTotal(const FString& Level);
	TArray<FString> SavesFor(const FString& Level);
	// Thumbnail brush for a level card (may be null if the PNG is missing).
	const struct FSlateBrush* MapThumbnail(const FString& Level);

	// ---- actions ----
	// Load a map's world context (terrain/assets/base setup) + set session state.
	// SaveName empty = read-only base preview; non-empty = editable custom map.
	void    OpenMapWorld(const FString& Level, const FString& SaveName);
	// Place a placeable by type at a world position; tagged + budget-counted.
	AActor* PlaceType(const FString& Type, const FVector& WorldPos);
	void    ExportSpatial();
	// Opens a file dialog; detects the map from the file, loads it, and names the
	// session after the file. Returns true when a map was actually imported.
	bool    ImportSpatial();
	// Write the current custom map's session to disk.
	void    SaveCurrent();
	// Turn the read-only base preview into an editable custom map named Name.
	void    CreateCustom(const FString& Name);
	// Deproject the current viewport cursor to the ground plane. False if no viewport.
	bool    WorldFromViewportCursor(FVector& OutWorld);

	// ============================================================================
	// Entry points implemented in BF6BuildMode.cpp
	// ============================================================================

	// The whole tool as one embedded, dockable editor panel (legacy fallback).
	TSharedRef<class SWidget> MakeToolPanel();
	// Attach the tool UI directly onto the level viewport (the editor's whole
	// centre, resizes with it): selector screen first, build HUD after a pick.
	// Retries on a ticker until the viewport exists.
	void ShowStartupUI();
	// Remove the viewport UI entirely (module shutdown).
	void DetachUI();
	// Load the chosen map + switch the viewport UI to the build screen.
	void EnterBuild(const FString& Level, const FString& SaveName);
	// Add / remove the viewport overlay (budget bar + import/export + radial host).
	void ShowBuildOverlay();
	void HideBuildOverlay();
	// Register the space-bar pie-menu input handler (once, at startup).
	void InstallInputHandler();
	void RemoveInputHandler();
	void HideTransientMenus();   // dismiss the category object popup
	bool IsBuildOverlayActive();

	// ---- SDK data import (first-run setup + re-sync) ----
	// The tool generates its data packs from the user's own unzipped Portal SDK
	// download, driving the SDK's bundled Godot headlessly. No 7GB distribution.
	bool    IsDataInstalled();                  // do we have models to build with?
	bool    ValidateSdkRoot(const FString& Path, FString& OutError);
	// bFullResync wipes the converted data first, so CHANGED SDK content is
	// reconverted too (the default incremental sync only adds what's new).
	void    StartSdkImport(const FString& SdkRoot, bool bFullResync = false);
	// Bring the SDK Setup screen up (used by the new-SDK-version startup prompt).
	void    ShowSdkSetup();
	bool    IsImporting();
	FText   ImportStatus();                     // progress line for the setup screen
	float   ImportFrac();                       // 0..1 across the whole import
	bool    ImportDone();                       // finished (success) since Start
	FString StoredSdkRoot();                    // remembered SDK path ("" if none)

	// ---- versioning + updates (GitHub releases, staged like the Godot plugin) ----
	// The plugin's VersionName from the .uplugin (e.g. "0.1.0").
	FString PluginVersion();
	// Ask github.com/TabbedScamper/BF6_Unreal_SDK for the latest release. If it's
	// newer, offer to download and restart-to-apply (a compiled plugin can't be
	// swapped while the editor runs). bManual: also report "up to date"/errors.
	void CheckForUpdates(bool bManual);
}
