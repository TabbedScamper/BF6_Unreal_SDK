#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

// The feature's own log category, so a bundler run, an npm install and the
// page's own console are one filter away from the rest of the tool.
class SWidget;

DECLARE_LOG_CATEGORY_EXTERN(LogBF6Script, Log, All);

// ============================================================================
// BF6 SCRIPT: TypeScript for Portal, inside the tool.
//
// A dock tab with its own embedded browser on a local page, running Monaco and
// the TypeScript language service, so writing a Portal script here feels the
// way it feels in VS Code: completion, signature help, hover documentation,
// go to definition and live errors. On top of that sits the part that is
// actually the point - a beginner layer that explains every line in plain
// words, ships annotated recipes, carries the community's answered questions,
// and diagnoses what went wrong.
//
// A PROJECT IS MIKE DE LUCA'S TEMPLATE. The tool does not invent a project
// shape. It copies the installed template (minus node_modules and .git), does
// what the template's own init script does without asking the questions, and
// runs npm install. BUILD runs the template's own bundler. Anyone who later
// wants to work on the same project in VS Code just opens the folder.
//
// SIGN-IN IS NEVER OURS. The template can deploy from the command line using
// an EA session id; the tool does not use that path and must not. Publishing
// is the user pressing Save on the real site, in the panel, with their own
// session. PUSH TO PORTAL fills the site editor in for them. That is all.
// ============================================================================
namespace BF6Script
{
	// StartupModule / ShutdownModule.
	void Register();
	void Unregister();

	// Show the editor tab, creating it if it is not up yet. The dock tab is the
	// second way in; the SCRIPT button on the build screen's toolbar row is the
	// first.
	void Open();

	// ---- BF6EditorOverlay ----
	// THE editor page, made on the first ask and kept for the life of the
	// editor session. The full-screen host and the dock tab both show this one
	// widget, so the open project, the cursor and every unsaved keystroke
	// survive moving it. Handing it out detaches it from the dock tab, because
	// a Slate widget has exactly one parent.
	TSharedRef<SWidget> Widget();

	// The host is done with it: back into the dock tab if that tab is open, and
	// otherwise parked, still loaded, until something asks again.
	void ReleaseWidget();
	// ---- end BF6EditorOverlay ----

	// Create a project from the template. Empty Name asks the page for one.
	void NewProject(const FString& Name);

	// Run the bundler on the open project.
	void Build();

	// Write what the tool knows to the output log: node, template, project,
	// type files, the page's mode and whether the panel is on a Script page.
	void LogStatus();

	// True when a project is open.
	bool HasProject();

	// Take one file into the open script project: a .ts joins src, a strings
	// json becomes the project's strings file, anything else lands beside them.
	// With no project open it tries the one belonging to the open save and says
	// so when there is none. The block editor calls this for the script and
	// strings attachments of an imported experience.
	bool ImportFile(const FString& Path);

	// The folder a save's script project lives in, created or not:
	//   Saved/BF6UnrealSDK/portal/experiences/<uuid>/script  when linked
	//   Saved/BF6UnrealSDK/script/<name>                     when not
	FString ProjectDirForSave(const FString& Level, const FString& Save);

	// ---- BF6Project --------------------------------------------------------
	// The installed template, found on first ask. Empty when the Portal SDK is
	// not installed and [BF6UnrealSDK] ScriptTemplateDir names nothing. Call it
	// on the game thread: it reads GConfig.
	FString TemplateDir();

	// Copy the template into Dir and apply the non-interactive equivalent of
	// its own scripts/init.js, exactly as NEW PROJECT does. This is the ONE
	// definition of what a scripting project is, so BF6Project scaffolds every
	// save through it rather than growing a second copy that drifts.
	//
	// Boilerplate is "plain" or "example", matching init.js's two choices.
	// ExperienceId, when given, is written into .env as MOD_ID; the SESSION_ID
	// beside it is never written, because that one is a credential.
	//
	// Safe to call OFF the game thread once TemplateDir has been asked for
	// once: it touches nothing but the file system. It does NOT run npm.
	bool ScaffoldProject(const FString& Dir, const FString& ExperienceName,
		const FString& Description, const FString& Boilerplate,
		const FString& ExperienceId, FString& OutWhy);

	// THE EXPERIENCE'S PROJECT, AS A TEMPLATE PROJECT.
	//
	// Scaffolds the scripting template into the experience's project folder if
	// there is not one there already, and does nothing at all if there is. An
	// imported experience gets this whichever form its rules arrive in - blocks,
	// a bundle, or neither - because the template is what carries the bundler,
	// the strict tsc, the utility modules and the debug tool, and an experience
	// that arrived without one had none of them.
	//
	// OutDir is the project folder, set even when the scaffold was already
	// there. Synchronous: it copies files and runs no npm.
	bool EnsureTemplateProject(const FString& ExperienceId, const FString& ExperienceName,
		FString& OutDir, FString& OutWhy);

	// PUBLISH MODE: the upload without the workbench.
	//
	// The template's debug tool is worth having while building a mod and is
	// dead weight on the site. This leaves it out of the bundle that is
	// uploaded, and NOTHING ELSE: measured on a real 300 KB mod, the 1,304
	// lines of bf6-portal-utils in it were events, ui, raycast, sounds and
	// timers, every one of them called by the mod. The bundler already omits
	// what is never imported, so the workbench is the only honest saving.
	//
	// IT HAPPENS TO THE BUNDLE, NOT TO A COPY OF THE SOURCE.
	//
	// The first version copied src minus debug-tool into .bf6-publish and
	// nothing ever compiled that copy, so publish mode changed nothing while
	// saying it had. It could not have worked in any case: the bundler follows
	// imports from the entry point, so a file nobody imports never reaches the
	// bundle, and a file that IS imported cannot be deleted from a copy without
	// that copy failing to compile.
	//
	// MakeLeanBundle removes whole modules from the BUILT bundle, and only when
	// nothing outside them uses any name they declare. That is checked, not
	// assumed: a mod that calls its debug tool keeps it and is told which name
	// held it in. Toggleable at any time; the answer is a file in the project
	// so it travels with the folder.
	bool PublishMode(const FString& ProjectDir);
	void SetPublishMode(const FString& ProjectDir, bool bOn);
	bool MakeLeanBundle(const FString& ProjectDir, FString& OutWhy, int32& OutLinesRemoved);
	FString LeanBundlePath(const FString& ProjectDir);

	// A BUNDLE IS NOT SOURCE.
	//
	// Portal returns bf6-portal-bundler output: every module concatenated with
	// "@ts-nocheck" on top and EVERY IMPORT STRIPPED. It does keep a
	// "// --- SOURCE: <path> ---" line per module, so the file list and layout
	// are recoverable - the imports are not.
	//
	// Reconstructing them was tried and measured on a real 19-file mod: it
	// compiled and re-bundled, but produced 9,932 lines against the original
	// 7,937, because guessing which file exports a name pulls in library
	// modules the original never used. A different mod that happens to build.
	// So the tool asks for the real source instead of guessing.
	// The import says so; nothing else does. The build gate that stops an
	// imported mod being overwritten by the template example reads this rather
	// than guessing from the shape of the bundle, which every build produces.
	void MarkBundleImported(const FString& ProjectDir, const FString& Experience);
	void ClearBundleImported(const FString& ProjectDir);

	bool LooksBundled(const FString& BundlePath, int32& OutOwnModules);
	void BundledModuleNames(const FString& BundlePath, TArray<FString>& Out);
	bool UseSourceFolder(const FString& ProjectDir, const FString& SrcFolder, FString& OutWhy);

	// ---- sounds, shared with the block editor ------------------------------
	//
	// Both editors let somebody choose a sound, so both should be able to play
	// one, and neither should own a second copy of how that works.
	//
	// "addon"      the High Poly add-on resolves the placeable to its sound EBX
	//              and decodes the real asset. Played by the editor itself.
	// "soundboard" a folder of clips recorded from the game, returned as data
	//              for a page to play.
	// empty        neither is available.
	FString SoundPreviewSource();
	// Offer the object library a way to play a placeable's sound when no add-on
	// does. Call once at startup.
	void    RegisterSoundboardPreview();
	bool    PlayPlaceableSound(const FString& Name, FString& OutWhy);
	void    StopPlaceableSound();
	bool    LoadSoundClipBase64(const FString& Name, FString& OutBase64, FString& OutWhy);

	// SHOW THIS PROJECT IN THE SCRIPT PANEL.
	//
	// The panel restores whichever project it had last, so an experience that
	// has just been imported - with the site's own bundle written into it -
	// stayed invisible behind a leftover project from a previous session. The
	// import knows which project it just filled, so it says so.
	//
	// False when there is no project there. Does not open or focus the panel:
	// it sets what the panel will show, which is what somebody who then presses
	// SCRIPT expects to find.
	bool OpenProjectAt(const FString& Dir);

	// The file the panel should show when it next draws this project, for the
	// case where the page's own choice is wrong. An imported experience's
	// src/index.ts is the TEMPLATE'S example and its real script is the bundle,
	// so opening the tab showed sample code and looked like nothing had
	// imported. Honoured once, then forgotten.
	void ShowFileFirst(const FString& Rel);

	// A BLOCK EXPERIENCE, AS A TEMPLATE PROJECT.
	//
	// Most creators never open the TypeScript template: their experience is a
	// block workspace and nothing else, so importing one used to produce a map
	// and no code at all. This scaffolds the template into the experience's
	// project if it is not there yet, then runs the converter over the
	// workspace so its rules land in src/ as ordinary TypeScript - which means
	// the template's bundler, its strict tsc and its utility modules apply to a
	// block mod exactly as they do to a written one.
	//
	// Measured on a real 43-rule workspace: 43 rules, 34 subroutines, 104
	// variables, nothing unconvertible, and the result builds under the real
	// bf6-portal-bundler.
	//
	// Asynchronous: the conversion is a node run, and node is not something to
	// wait for on the game thread. False (with OutWhy) only when it could not
	// be STARTED; the outcome of the run itself is logged and reported through
	// the script page like any other run.
	//
	// A file the conversion replaces is kept beside itself as <name>.orig, so
	// re-importing never silently destroys edits.
	bool ConvertBlocksToTemplate(const FString& ExperienceId, const FString& ExperienceName,
		const FString& WorkspaceJsonPath, FString& OutWhy);
	// ---- end BF6Project ----

	// ---- the session hook ---------------------------------------------------
	// THE SITE SIGNS PEOPLE OUT. After a while idle, and after a site update,
	// the Portal page bounces to login and anything unsaved on it is gone.
	// Nothing of the user's may be lost when that happens, and nothing here
	// depends on it not happening:
	//
	//   the source     is already on disk, autosaved two seconds after the
	//                  last keystroke, with the last ten versions of every
	//                  file kept under <project>/.history;
	//   the bundle     is already on disk, and the fact that it was built and
	//                  not yet saved on Portal is written into the project's
	//                  own pending file, so it survives an editor restart too;
	//   the push       pauses, and comes back as PUSH AGAIN once the panel is
	//                  signed in on the same experience's Script page.
	//
	// THE HOOK. BF6PortalProfile owns the account state and is the only thing
	// that can say authoritatively that the session went. Until it exposes
	// OnSessionLost / OnSessionRestored delegates, the seam runs the other way
	// and these two are what it calls: one line each, from wherever the
	// profile decides the state changed. They are safe to call repeatedly and
	// safe to call before this module has a page.
	//
	// Without those two calls the feature still works: the site half reports
	// every page it lands on, and a bounce from a Script page to the login
	// page is read here as a sign-out. That fallback is a guess about a URL;
	// the calls below are the fact, so wire them when the profile has them.
	void NotifySessionLost(const FString& Why);
	void NotifySessionRestored();

	// Console-driven test of both paths, so the banner and the PUSH AGAIN
	// offer can be seen without waiting for the site to sign anyone out:
	//   BF6.Script.Session.Simulate lost
	//   BF6.Script.Session.Simulate restored
	// The profile module registers BF6.Portal.Session.Simulate for the same
	// purpose; when it does, it should route into the two calls above and this
	// one becomes an alias.
	void SimulateSession(const FString& What);
}
