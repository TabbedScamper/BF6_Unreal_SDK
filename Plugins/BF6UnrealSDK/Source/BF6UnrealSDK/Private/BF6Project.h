#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

DECLARE_LOG_CATEGORY_EXTERN(LogBF6Project, Log, All);

// ============================================================================
// BF6 PROJECT: every save is a complete project on disk.
//
// A save used to be one JSON file. That file is enough to reopen a map and
// nothing else: it cannot be diffed against the site, it cannot be handed to
// another creator, it does not say which experience it belongs to, and none of
// the scripting toolchain the community already uses can be pointed at it.
//
// A PROJECT IS MIKE DE LUCA'S TEMPLATE PLUS WHAT THE UNREAL TOOL OWNS.
// The scripting half is his bf6-portal-scripting-template (MIT, Michael De
// Luca) copied verbatim, with the non-interactive equivalent of its own
// scripts/init.js applied, so npm run build / lint / prettier /
// export-thumbnail / minify-spatials / refresh-ai / update all work and anyone
// who later opens the folder in VS Code gets exactly the project they expect.
// The map half is ours: the session save, the exported spatials, the Godot
// scene, the block workspace, the UI designs, the settings snapshot and the
// map rotation. A manifest at the root, project.json, names all of it with a
// content hash per file, which is what makes two projects comparable.
//
// DEPLOY IS NOT OURS. scripts/deploy.js and @bf6mods/portal work from the
// user's EA session id. The tool never touches a credential, so the deploy
// script is removed from every project it creates and the deploy entries are
// taken out of package.json. Publishing stays what it has always been: the
// user pressing Save on the real site.
//
// NOTHING IS INVENTED. A field the tool cannot know is not written; it is
// named in the manifest's "missing" list instead, so a project is honest about
// what it does not have.
// ============================================================================
namespace BF6Project
{
	// StartupModule / ShutdownModule. Registers the BF6.Project.* commands.
	void Register();
	void Unregister();

	// The project folder for a save. This is the save folder the tool already
	// uses (Saved/BF6UnrealSDK/saves/<Save>), so an existing save becomes a
	// project where it already lives and no path anywhere else has to move.
	FString DirFor(const FString& Save);

	// ---- an experience is ONE project with MANY maps ------------------------
	//
	// An experience is one game mode: one script project, one settings set, one
	// thumbnail, one block workspace - and many maps. Eleven flat sibling saves
	// duplicated the shared half eleven times, so an experience gets ONE folder:
	//
	//   experiences/<experience>/                  project.json and the template
	//   experiences/<experience>/blocks/           workspace.json
	//   experiences/<experience>/settings/         mutators, teams, restrictions
	//   experiences/<experience>/maps/<MP_Level>/  <MP_Level>.json, .spatial.json,
	//                                              .tscn, backups
	//
	// THE SAVE NAME IS THE EXPERIENCE FOLDER NAME. Every path in the tool is
	// already (Level, SaveName), so a map of an experience is that map's level
	// plus the experience's name: RESUME under a map card lists the game mode,
	// the map switcher moves between levels of one save, and nothing above this
	// had to learn a new shape. A save belonging to no experience is untouched.
	// saves/experiences. Everything a creator saves lives under saves/, so that
	// is the only folder they ever have to look in.
	FString ExperiencesRoot();
	// Where an earlier build put them, beside saves/ instead of inside it.
	// Only MoveExperiencesIntoSaves may use this.
	FString LegacyExperiencesRoot();
	// Move anything left at the old sibling location into saves/experiences.
	// Runs at startup. Returns how many moved.
	int32 MoveExperiencesIntoSaves();
	// The folder for a name, or empty when no experience project is there.
	FString ExperienceDir(const FString& Folder);
	bool    IsExperience(const FString& Save);
	// experiences/<Save>/maps/<Level>, or empty when Save is not an experience.
	FString MapDir(const FString& Save, const FString& Level);
	TArray<FString> ExperienceFolders();
	TArray<FString> MapsIn(const FString& Save);
	// A display name made safe for the filesystem, with a short id appended
	// when a DIFFERENT experience already holds that name. The uuid in the
	// manifest is the identity; the folder is only how a person finds it.
	FString FolderNameFor(const FString& DisplayName, const FString& Id);
	FString FolderForExperience(const FString& Id);

	// Make (or find) the experience project for a uuid and name, with the
	// template and the shared folders in place. Returns its folder name, which
	// is the save name its maps are addressed by.
	FString EnsureExperience(const FString& Id, const FString& DisplayName);

	// ---- moving a map in and out of an experience ---------------------------
	//
	// The option the owner asked for, and its reverse so nobody is trapped.
	// Both are non-destructive: everything is copied and verified before
	// anything is removed, and a kept snapshot is taken first.
	bool MoveIntoExperience(const FString& Level, const FString& Save, const FString& ExperienceFolder, FString& OutWhat);
	bool MoveOutOfExperience(const FString& Level, const FString& Save, const FString& NewSaveName, FString& OutWhat);

	// MIGRATION. The flat sibling saves an earlier import wrote become the maps
	// of their experience. Verified before anything is removed; a migration
	// that cannot verify leaves the original exactly where it is and says so.
	// bDryRun reports what it would do and touches nothing.
	int32 MigrateFlatSaves(bool bDryRun);

	// REMOVE A WHOLE EXPERIENCE. Deleting a map takes one map and leaves the
	// game mode standing, which is right, but it left no way at all to get rid
	// of an imported experience: eleven map deletes emptied the rotation and
	// the experience still stood there with its script and its settings, still
	// listed, still reporting itself as imported. That is what this is for.
	//
	// A kept snapshot is taken first, outside the rotation so it cannot age
	// out, and the folder goes only if that snapshot was written. bDryRun
	// reports what would go and touches nothing.
	struct FRemoval
	{
		FString Save;          // the experience folder
		FString Dir;
		int32   Maps = 0;      // rotation maps that would go with it
		int64   Bytes = 0;
		FString Snapshot;      // where the copy was kept
		bool    bDeleted = false;
		FString Why;           // when it did not happen, in words
	};
	FRemoval DeleteExperience(const FString& Save, bool bDryRun);

	// THE ONE PLACE AN EXPERIENCE'S SCRIPT PROJECT LIVES: the experience folder
	// itself, which already carries De Luca's template once for all of its
	// maps. BF6Script::ProjectDirForSave still owns the decision and keeps its
	// signature; this is the answer it should give, put here because that file
	// is outside this pass and the change there is one line:
	//
	//   const FString E = BF6Project::ScriptDirForSave(Level, Save);
	//   if (!E.IsEmpty()) return E;
	//
	// Returns empty when the save belongs to no experience project, and then
	// nothing about the old behaviour changes.
	FString ScriptDirForSave(const FString& Level, const FString& Save);

	// The migration as a thing a person can press: what is there, what it
	// would become, and a preview that changes nothing.
	void ShowOrganiseDialog();

	// True when the folder holds a manifest, meaning the tool has already made
	// a project of it.
	bool HasManifest(const FString& Save);
	// True when the scripting half is there as well (package.json and src).
	bool HasTemplate(const FString& Save);

	// EVERY SAVE GETS ONE. Called from the save path, right after the session
	// file is written, and from the load path so an older save is upgraded the
	// first time it is opened.
	//
	// This call is CHEAP and synchronous: it makes the folders, writes the
	// readme and rebuilds the manifest, and that is all. The template copy is
	// hundreds of small files, so it is queued onto a background thread and the
	// manifest is refreshed on the game thread when it lands. npm install is
	// not run here at all: it is a network download of minutes, and the script
	// editor already installs on demand.
	void NoteSaved(const FString& Level, const FString& Save);

	// The same work, said out loud, for the console and for a migration the
	// user asked for. bWaitForTemplate copies the template inline instead of in
	// the background, so a script can rely on the folder being finished.
	void Ensure(const FString& Level, const FString& Save, bool bWaitForTemplate);

	// ---- compare and link ---------------------------------------------------
	//
	// The point of a project being a complete, self-describing folder: it can
	// be held up against the experiences the profile knows about and matched to
	// the one it came from, so a save built in the tool can be reattached to
	// work the user already has on the site.
	struct FMatch
	{
		FString Id;
		FString Name;
		float   Confidence = 0.f;   // 0..100, earned weight over attainable weight
		TArray<FString> Reasons;         // what agreed, and by how much
		TArray<FString> NotComparable;   // signals neither side could answer
	};

	// Every candidate, best first. Reads the profile's own cache on disk, so it
	// needs no network and no sign-in.
	TArray<FMatch> Compare(const FString& Save);
	void LogCompare(const FString& Save);

	// Write the experience into the project manifest AND onto the save, so the
	// next session write carries "portalExperience" the way an imported save
	// does. Returns false when the save or the experience is not known.
	bool Link(const FString& Save, const FString& ExperienceId);
	// Forget the link. The files stay.
	bool Unlink(const FString& Save);

	// The compare result as a dialog with a LINK offer, for the map screen.
	void ShowCompareDialog(const FString& Save);
	// Compare every project on disk and print one table. The experience screen's
	// entry point.
	void CompareAll();

	// ---- sync state, per artefact -------------------------------------------
	//
	// THE UNIT OF EVERY DECISION IS ONE ARTEFACT, never the whole experience.
	// An eleven map experience where one map diverged must not ask about the
	// other ten, and must not ask about the workspace at all.
	//
	// Each artefact carries its own canonical hash, the hash it was last in
	// step with, which side that agreement came from, and who wrote it last.
	// Comparing THOSE THREE is what makes the question rare.
	struct FArtefactState
	{
		FString Rel;          // path inside the project
		FString Kind;         // spatial, workspace, script, strings, settings, session
		FString Level;        // when it belongs to one map
		FString Canonical;    // hash of the NORMALISED content, not the bytes
		FString SyncHash;     // the canonical hash the two sides last agreed on
		FString SyncSide;     // tool or site: which side that agreement came from
		FString SyncAt;
		FString OriginBy;     // tool, site or user: who caused the last write
		FString OriginAt;
	};
	TArray<FArtefactState> ArtefactStates(const FString& Save);

	// What changed between two spatials, keyed on ObjId, because names are
	// minified on the site and mean nothing across a round trip.
	struct FSpatialDiff
	{
		FString Level;
		int32 Added = 0;        // has an ObjId there and not here
		int32 Removed = 0;      // has an ObjId here and not there
		int32 Moved = 0;        // same ObjId, different position
		int32 Changed = 0;      // same ObjId, same position, different properties
		int32 UntrackedHere = 0;
		int32 UntrackedThere = 0;
	};

	// THE DECISION RUN. Three way per artefact: the site's copy, ours, and the
	// hash the two last agreed on. Unchanged says nothing. Changed on one side
	// offers that side. Changed on both is the only case that asks, and it asks
	// with the real numbers from the diff.
	void Sync(const FString& Save);

	// ---- project backups ----------------------------------------------------
	//
	// These are PROJECT level and sit beside the LEVEL level temp backups the
	// autosave already keeps. The temp backups hold the session file, which is
	// the map you were building; these hold everything else a project owns
	// (the spatials, the workspace, the settings, the strings and the manifest),
	// which is what a sync can overwrite and the temp backups never covered.
	//
	// Five rotating snapshots taken on save, settable as
	// [BF6UnrealSDK] ProjectBackupMax, PLUS a pre-import snapshot taken
	// immediately before any import or overwrite, kept out of the rotation so
	// it can never age out.
	//
	// A SNAPSHOT COVERS THE WHOLE PROJECT: the maps, the spatials, everything
	// under unreal/ (the block workspace, the UI designs, the settings, the
	// scenes, the rotation), src/ and the template configuration that makes
	// src/ build. It used to cover four of those and report the number of files
	// it managed to copy, which is why a restore could hand back a project with
	// no maps in it and call that a success.
	int32 BackupMax();
	void  SetBackupMax(int32 N);

	// What a snapshot actually holds. The file count on its own was never
	// enough: nine of twelve copied and nine copied read exactly the same.
	struct FSnapshot
	{
		FString Stamp;                 // the folder name under .backups; empty when none was taken
		FString Dir;
		int32   Files = 0;             // files that arrived and hashed back
		int32   Expected = 0;          // files the project owned when it was taken
		int64   Bytes = 0;
		bool    bComplete = false;     // every owned file arrived; ONLY THIS may authorise a delete
		TArray<FString> Missing;
		FString Why;                   // when no snapshot was taken, in words
	};
	// Why empty makes a rotating snapshot; any other Why makes a kept one.
	FSnapshot SnapshotProjectChecked(const FString& Save, const FString& Why);
	// The same call for the places that only need the stamp for a log line.
	FString SnapshotProject(const FString& Save, const FString& Why);

	void LogBackups(const FString& Save);

	// What a restore actually did. A restore that put back some of the files is
	// not a success with a smaller number in it, so it does not report one.
	struct FRestore
	{
		bool  bRestored = false;   // the project IS that snapshot now, verified by hash
		bool  bPartial = false;    // some files came back, some did not, nothing was deleted
		int32 Restored = 0;
		int32 Expected = 0;
		int32 Removed = 0;         // owned files the snapshot did not have
		TArray<FString> Failed;
		FString Guard;             // the kept snapshot of what this replaced
		FString What;              // one plain sentence for the user
	};
	FRestore RestoreBackupChecked(const FString& Save, const FString& Stamp);
	// True ONLY for a complete, verified, reconciled restore.
	bool RestoreBackup(const FString& Save, const FString& Stamp);

	// Where the UI builder's authored designs live for a save:
	// <project>/unreal/ui. Empty when that save has no project folder. The UI
	// builder keeps its own copy of this decision out of the project contract
	// rather than inventing a second place to put designs.
	FString UiDesignDir(const FString& Save);

	// ORIGIN STAMP. Every write the tool makes to an artefact says who caused
	// it, so the sync that follows never re imports what the tool itself just
	// wrote. By is tool, site or user.
	void NoteArtefactWritten(const FString& Save, const FString& Rel, const FString& By);

	// ---- the Godot scene a spatial was authored from ------------------------
	//
	// A Portal .spatial.json carries the scene tree ONLY through each object's
	// id, which is its full Godot node path. A group pivot that holds no object
	// of its own exists only as a path segment and has no transform anywhere in
	// the file, and a minified spatial - which is what the site usually stores -
	// renames every segment to a short token. So a map imported from a spatial
	// arrives with its names and its grouping degraded or gone.
	//
	// The .tscn it was exported from still has all of it. When such a map is
	// opened the tool offers to bring that file in, and remembers the answer -
	// including "do not ask again" - here, beside the other per-artefact
	// records, so the question is asked once and never again.

	// Where a save came from. Written at the moment it is made, so the offer
	// can tell a map that arrived as a spatial from one the user built here.
	void    NoteOrigin(const FString& Level, const FString& Save, const FString& Kind, const FString& Source);
	FString OriginKind(const FString& Save);   // spatial, tscn, base-setup, tool, or empty

	struct FTscnRecord
	{
		// none: never asked. adopted: a scene file was merged in.
		// skipped: the user chose the spatial this time. never: do not ask again.
		FString State = TEXT("none");
		FString Path;         // where the scene file was when it was adopted
		FString Md5;          // its hash at that moment
		FString AdoptedAt;
		FString AskedAt;
		bool    bOnDisk = false;    // Path still exists
		bool    bChanged = false;   // it exists and its hash has moved since
	};
	FTscnRecord TscnFor(const FString& Save, const FString& Level);
	void NoteTscnAdopted(const FString& Level, const FString& Save, const FString& TscnPath);
	void NoteTscnSkipped(const FString& Level, const FString& Save, bool bForever);

	// THE ASK. Called when a map opens from an experience. It shows nothing
	// unless it is relevant: the save has to have come from a spatial and have
	// no answer recorded yet. The one exception is a scene file that WAS
	// adopted and has changed on disk since, which is mentioned rather than
	// ignored.
	void OfferTscn(const FString& Level, const FString& Save);

	// Bring a scene file in on demand. Empty path opens a picker that starts in
	// the SDK's own levels folder. Compares first, always snapshots first, and
	// merges rather than replacing unless the user asks for the replacement.
	bool ImportTscn(const FString& Save, const FString& Path);

	// ---- round trip ---------------------------------------------------------
	// The project written out as the site's own whole-experience JSON, and one
	// of those files read in as a new project. Empty Path opens a picker.
	bool ExportExperienceJson(const FString& Save, const FString& Path);
	bool ImportExperienceJson(const FString& Path, FString& OutSave);

	void LogStatus(const FString& Save);
}
