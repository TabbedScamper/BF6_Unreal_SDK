#pragma once

#include "CoreMinimal.h"

class SWidget;
class AActor;
class USoundWave;
struct FSlateBrush;

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

	// ---- the object library's pictures -------------------------------------
	//
	// The library draws each placeable's card from the tool's own low-poly
	// thumbnail. An add-on that can show the real thing registers a provider:
	// asked for a placeable TYPE on the paint path, it returns a brush, or null
	// meaning "nothing yet, draw your own". Keep it cheap; it is asked once
	// per visible card per paint. Providers are asked in registration order
	// and the first brush wins. Re-registering an Id replaces it.
	BF6UNREALSDK_API void RegisterThumbnailProvider(FName Id,
		TFunction<const FSlateBrush*(const FString& /*Type*/)> Provider);
	BF6UNREALSDK_API void UnregisterThumbnailProvider(FName Id);

	// WHAT THAT PICTURE IS, in the add-on's own words: two or three lowercase
	// words the library prints on the card ("high poly", "clay", "low poly").
	// A picture that could be either detail level and says nothing about which
	// leaves the creator guessing whether the card is the real object or the
	// blockout standing in for it, and that guess is exactly what a detail
	// toggle exists to remove. Empty means "no label", which is what a tool
	// with no add-on shows.
	//
	// Asked on the paint path beside the brush, so it must be a map lookup.
	// The Id is the provider's, so registering both under one name keeps them
	// together and one Unregister call removes the pair.
	BF6UNREALSDK_API void RegisterThumbnailDetail(FName Id,
		TFunction<FString(const FString& /*Type*/)> Detail);
	BF6UNREALSDK_API void UnregisterThumbnailDetail(FName Id);

	// The object library's list for the open map, by type, in the order the
	// library shows it; the full catalogue when no map is open. What an
	// add-on iterates to prepare pictures for exactly the cards a user sees.
	BF6UNREALSDK_API void PlaceableTypes(TArray<FString>& Out);

	// ---- what the tool is doing --------------------------------------------

	// "MP_Badlands", or empty when no map is open.
	BF6UNREALSDK_API FString CurrentLevel();
	// Process monotonic time at the start of opening the current/requested map,
	// before SDK context and saved actors are loaded. Zero before the first open.
	BF6UNREALSDK_API double MapOpenStartedAt();
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
	// Create (or replace, after the tool's own confirmation) a custom map on the
	// open level and enter editing on it - the same action as the HUD's Create.
	BF6UNREALSDK_API void    CreateCustomMap(const FString& Name);
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

	// A PANEL THAT STAYS PUT.
	//
	// ShowPopup is right for something glanced at and dismissed, and wrong for
	// a panel somebody works in: a popup cannot be moved, does not stay on top,
	// and closes on the first click outside it - which is how a BUILD button
	// several sections down became unreachable.
	//
	// This is a real window instead. It floats above the editor, it can be
	// dragged anywhere, and dragging it against a screen edge snaps it to that
	// edge as a docked bar; dragging it away undocks it. Position, size and
	// dock edge are remembered per Key across editor sessions.
	//
	// Calling it again with the same Key brings the existing window forward and
	// replaces its contents rather than opening a second one.
	BF6UNREALSDK_API void ShowAddonWindow(const FString& Key, const FString& Title,
		TSharedRef<SWidget> Content, FVector2D DefaultSize);

	// Close it, if it is open. Safe when it is not.
	BF6UNREALSDK_API void CloseAddonWindow(const FString& Key);

	// Every add-on window at once, for module teardown. A floating window
	// outlives the tab that spawned it, and one still holding an add-on's
	// widgets while that add-on unloads is a crash on the next repaint.
	BF6UNREALSDK_API void CloseAllAddonWindows();

	// Is that window open right now?
	BF6UNREALSDK_API bool IsAddonWindowOpen(const FString& Key);
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
	// Visible perspective level views in the workspace world. A shared scene
	// representation must remain detailed when ANY of these views is nearby.
	// Excludes asset previews, hidden layout tabs and orthographic views.
	BF6UNREALSDK_API void GetBuildViewportLocations(TArray<FVector>& OutLocations);
	BF6UNREALSDK_API bool SetBuildViewportCamera(const FVector& Location, const FRotator& Rotation);
	BF6UNREALSDK_API bool RedrawBuildViewport();

	// ---- placing the user's content from an add-on --------------------------
	//
	// An add-on that rebuilds something from game data (the High Poly add-on's
	// game modes) wants what it makes to be the USER'S objects: placed through
	// the tool's own placement so they carry BF6Placed, sit in the scene tree,
	// count against the budget, save and export. These are that placement and
	// nothing an add-on could not do by hand through the library. Every call
	// refuses on a read-only base preview: null, false, or a no-op.
	//
	// The macro is how an add-on compiles against a tool with or without this
	// block: #if defined(BF6EXT_HAS_PLACEMENT).
	#define BF6EXT_HAS_PLACEMENT 1

	// The tool's PlaceType at the transform's location, then the full transform.
	// A volume keeps its loop in world space and takes no rotation.
	BF6UNREALSDK_API AActor* PlaceObject(const FString& Type, const FTransform& WorldXf);
	// An empty scene-tree node named Label, the thing objects are parked under.
	BF6UNREALSDK_API AActor* PlaceNode(const FString& Label, const FVector& WorldPos);
	// Is this type in the library at all, on any level?
	BF6UNREALSDK_API bool    HasPlaceable(const FString& Type);
	// The type's editable fields as (name, type) pairs. A link field's type is
	// "PolygonVolume", "Array[SpawnPoint]" and so on.
	BF6UNREALSDK_API void    ObjectPropDefs(const FString& Type, TArray<TPair<FString, FString>>& Out);
	// The "p:Key=Value" store the attributes panel edits. A link field takes
	// ObjectLinkName() values, comma-joined for an array.
	BF6UNREALSDK_API void    SetObjectProp(AActor* A, const FString& Key, const FString& Value);
	BF6UNREALSDK_API FString ObjectProp(AActor* A, const FString& Key);
	// Editor-only representation choices. They save and duplicate with the
	// object, ride undo, and are never emitted as Portal gameplay properties.
	struct FObjectPreview
	{
		FString Id, Type, Label;
		TMap<FString, FString> Values;
	};
	BF6UNREALSDK_API FObjectPreview SelectedObjectPreview();
	// Return true only when the provider opened an editor for the current selection.
	BF6UNREALSDK_API void RegisterObjectEditor(FName Id, TFunction<bool()> Open);
	BF6UNREALSDK_API void UnregisterObjectEditor(FName Id);
	BF6UNREALSDK_API bool OpenSelectedObjectEditor();
	BF6UNREALSDK_API bool SetObjectPreview(const FString& Id, const FString& Key, const FString& Value);
	BF6UNREALSDK_API bool SetObjectPreviews(const FString& Id, const TMap<FString, FString>& Values);
	// Portal enum identifiers, independently of the installed game's visual catalogue.
	BF6UNREALSDK_API TArray<FString> LootItems();
	BF6UNREALSDK_API TArray<FString> WeaponAttachmentItems();
	// Captured actor path is validated again when the menu action runs. Persistent
	// recipes use its unique ObjId. Action is blocks, script or card.
	BF6UNREALSDK_API bool OpenLootBinding(const FString& Id, const FString& Action, FString& OutWhy);
	// The label the scene tree shows; the tool makes it unique.
	BF6UNREALSDK_API void    SetObjectLabel(AActor* A, const FString& Label);
	// What a link field stores for this object.
	BF6UNREALSDK_API FString ObjectLinkName(AActor* A);
	// Replace a PolygonVolume's loop with world points (cm). Wind them the way
	// the tool's own square volumes are wound, or run FixSafeValidationIssues.
	BF6UNREALSDK_API bool    SetVolumeLoop(AActor* Volume, const TArray<FVector>& WorldPoints);
	// Attach Child under Parent in the scene tree, keeping its world transform.
	BF6UNREALSDK_API void    ParentUnder(AActor* Child, AActor* Parent);
	// PlaceObject selects what it placed; a batch clears that when it is done.
	BF6UNREALSDK_API void    SelectNone();
	BF6UNREALSDK_API void    RefreshSceneTree();

	// ---- giving the tool a voice --------------------------------------------
	//
	// The tool raises an EVENT every time the user does something: placed,
	// deleted, moved, linked, saved, refused. It ships no audio and reads no
	// game files, so with no add-on those events are silent and the tool is
	// exactly what it is today.
	//
	// An add-on that can read the player's install registers a provider and the
	// tool speaks with the game's own voice, out of the user's own files. The
	// provider is asked for a wave immediately before each play, not once, so a
	// provider whose waves are procedural can re-arm its buffer there; the
	// object itself should be created once and kept alive by the provider.
	// Registering null detaches and the tool goes quiet again.
	//
	// The second seam is the object library: every SFX_* placeable in Portal is
	// a sound the user is choosing blind. A preview provider lets a row play
	// the sound the game would play for that object. The tool guarantees one
	// preview at a time, stops the running one before starting another, and
	// stops on a second click or when the row stops being hovered.
	//
	// The macro is how an add-on compiles against a tool with or without this
	// block: #if defined(BF6EXT_HAS_UISOUND).
	#define BF6EXT_HAS_UISOUND 1

	enum class EUiSound : uint8
	{
		Place, Delete, MoveEnd, Assign, Link, Unlink, RingOpen, RingClose,
		Confirm, Cancel, Error, Save, Select, Hover, Undo, Redo, ImportDone,
		Count
	};

	class IUiSoundProvider
	{
	public:
		virtual ~IUiSoundProvider() {}
		// Null means this provider has no sound for that event; the event stays
		// silent and nothing is logged, because a missing UI sound is not worth
		// a message.
		virtual USoundWave* SoundFor(EUiSound Event) = 0;
		// One line for the log, e.g. "17 event(s) mapped, 15 decoded".
		virtual FString Describe() const { return FString(); }
	};

	class IPlaceableSoundPreview
	{
	public:
		virtual ~IPlaceableSoundPreview() {}
		// Asked while a library row is built, so answer from an index. Never
		// decode here.
		virtual bool    CanPreview(const FString& PlaceableType) const = 0;
		virtual void    Preview(const FString& PlaceableType) = 0;
		virtual void    Stop() = 0;
		// The type playing right now, empty when nothing is.
		virtual FString Playing() const = 0;
	};

	BF6UNREALSDK_API void RegisterUiSoundProvider(TSharedPtr<IUiSoundProvider> Provider);
	BF6UNREALSDK_API void RegisterPlaceableSoundPreview(TSharedPtr<IPlaceableSoundPreview> Preview);

	// What the user chose in the tool's own Sounds row. An add-on that decodes
	// on a worker thread can skip the work entirely when sounds are off.
	BF6UNREALSDK_API bool  UiSoundEnabled();
	BF6UNREALSDK_API float UiSoundVolume();
}
