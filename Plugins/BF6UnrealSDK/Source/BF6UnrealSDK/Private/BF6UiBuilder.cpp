#include "BF6UiBuilder.h"
#include "BF6UiBuilderBridge.h"
#include "BF6Blocks.h"           // Open + HandleMessage: the INSERT INTO BLOCKS seam
#include "BF6Script.h"           // Open + HasProject: the INSERT INTO SCRIPT seam
#include "BF6SDKExtension.h"     // BF6Ext::ToolPluginDir / ToolSavedDir
#include "BF6PortalWeb.h"        // Open: the settings column lives in the Portal panel
#include "BF6PortalSettings.h"   // Refresh: the HIDE GAME HUD ELEMENTS link
#include "BF6EditorOverlay.h"    // ---- BF6EditorOverlay ---- the full-screen host
#include "BF6Project.h"          // UiDesignDir, NoteArtefactWritten: the project contract
#include "BF6BuildMode.h"        // BF6Api::CurrentSave: which project a design belongs to

#include "SWebBrowser.h"
#include "WebBrowserModule.h"

#include "Dom/JsonObject.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"   // which slide-out the creator left open
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

DEFINE_LOG_CATEGORY(LogBF6UiBuilder);

// ============================================================================
// State
// ============================================================================
namespace
{
	const FName kTabId(TEXT("BF6UiBuilder"));
	const TCHAR* kBridgeName = TEXT("bf6uibuilder");
	const int32  kChunk = 512 * 1024;      // one ExecuteJavascript beyond this is asking for trouble

	TSharedPtr<SWebBrowser> GBrowser;
	// ---- BF6EditorOverlay ----
	// The page outlives both of its homes. GHost is what the dock tab shows;
	// the page sits in it while docked and moves out of it while the
	// full-screen host has it, and is destroyed by neither.
	TSharedPtr<SBox>        GHost;
	TWeakPtr<SDockTab>      GTab;
	// ---- end BF6EditorOverlay ----
	TWeakObjectPtr<UBF6UiBuilderBridge> GBridge;
	TArray<IConsoleObject*> GCmds;

	bool    GPageReady = false;
	uint64  GPageGeneration = 0;
	int32   GIn = 0;
	int32   GOut = 0;
	FString GLastError;
	FString GLastExport;
	FString GLastDesign;

	// A load or an export asked for from the console, waiting on the page.
	FString GPendingExportFormat;
	FString GPendingExportPath;
	FString GPendingDesignMessage;

	// ---- paths -------------------------------------------------------------
	FString ResDir()
	{
		return FPaths::Combine(BF6Ext::ToolPluginDir(), TEXT("Resources"), TEXT("uibuilder"));
	}
	FString SavedDir()
	{
		return FPaths::Combine(BF6Ext::ToolSavedDir(), TEXT("uibuilder"));
	}
	FString SnippetDir()
	{
		return FPaths::Combine(BF6Ext::ToolPluginDir(), TEXT("Resources"), TEXT("blocks"),
			TEXT("snippets"), TEXT("uibuilder"));
	}
	FString EditorPageUrl()
	{
		FString Path = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(ResDir(), TEXT("editor.html")));
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		Path.ReplaceInline(TEXT(" "), TEXT("%20"));
		return TEXT("file:///") + Path;
	}

	// ---- JSON --------------------------------------------------------------
	TSharedPtr<FJsonObject> Parse(const FString& Text)
	{
		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(R, Obj) || !Obj.IsValid()) return nullptr;
		return Obj;
	}
	FString Write(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, W);
		return Out;
	}

	// A JSON document, already text, going into a JavaScript string literal.
	FString EscapeForJs(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len() + 32);
		for (const TCHAR C : In)
		{
			switch (C)
			{
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\''): Out += TEXT("\\'");  break;
			case TEXT('"'):  Out += TEXT("\\\""); break;
			case TEXT('\n'): Out += TEXT("\\n");  break;
			case TEXT('\r'): Out += TEXT("\\r");  break;
			case TEXT('\t'): Out += TEXT("\\t");  break;
			default:
				// Control characters, and the two separators JavaScript treats
				// as line breaks inside a string literal.
				if (C < 0x20 || C == 0x2028 || C == 0x2029)
					Out += FString::Printf(TEXT("\\u%04x"), (int32)C);
				else Out.AppendChar(C);
			}
		}
		return Out;
	}

	void ToPageRaw(const FString& JsonText)
	{
		if (!GBrowser.IsValid()) return;
		if (JsonText.Len() > kChunk)
		{
			// The page has no chunk reassembler, so refuse loudly rather than
			// hand it half a document and let it fail as a parse error.
			GLastError = FString::Printf(
				TEXT("a %d character message is too big for one call to the page"), JsonText.Len());
			UE_LOG(LogBF6UiBuilder, Warning, TEXT("%s"), *GLastError);
			return;
		}
		GOut++;
		GBrowser->ExecuteJavascript(FString::Printf(
			TEXT("try{window.BF6UiBuilder&&window.BF6UiBuilder.recv('%s');}catch(e){console.warn(e);}"),
			*EscapeForJs(JsonText)));
	}

	void ToPage(const TSharedRef<FJsonObject>& Obj) { ToPageRaw(Write(Obj)); }

	void Note(const FString& Text)
	{
		TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("op"), TEXT("status"));
		M->SetStringField(TEXT("text"), Text);
		ToPage(M);
	}

	// The palette lives beside the page, but a page on file:// cannot fetch a
	// sibling file, so the tool reads it and pushes it in on ready.
	void SendPalette()
	{
		FString Text;
		const FString Path = FPaths::Combine(ResDir(), TEXT("palette.json"));
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			UE_LOG(LogBF6UiBuilder, Display,
				TEXT("No palette.json at %s: the page keeps its built-in palette."), *Path);
			return;
		}
		Text.TrimStartAndEndInline();
		ToPageRaw(TEXT("{\"op\":\"palette\",\"json\":") + Text + TEXT("}"));
	}

	// ---- how the creator left the window -----------------------------------
	//
	// The builder is one canvas with a strip of slide-out panels down its left
	// edge, and which one was open is a preference, not a fact about the
	// design: it belongs to the person, so it is kept per project in the editor
	// ini beside every other panel state the tool remembers.
	//
	// Everything is a string. The page decides what "true" means for each key,
	// and a key nobody wrote yet does not come back at all, which is how the
	// page tells a first run from a return visit.
	const TCHAR* kPrefSection = TEXT("BF6UnrealSDK");
	const TCHAR* kUiPrefKeys[] = {
		TEXT("shelf"),        // which handle was last open
		TEXT("shelfOpen"),    // whether it was open at all
		TEXT("shelfPinned")   // whether it stays open while you work
	};

	FString UiPrefIniKey(const FString& Name) { return TEXT("UiBuilderUi_") + Name; }

	bool IsKnownUiPref(const FString& Name)
	{
		for (const TCHAR* K : kUiPrefKeys) if (Name == K) return true;
		return false;
	}

	void SendPrefs()
	{
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		for (const TCHAR* K : kUiPrefKeys)
		{
			FString V;
			if (GConfig && GConfig->GetString(kPrefSection, *UiPrefIniKey(K), V, GEditorPerProjectIni))
			{
				P->SetStringField(K, V);
			}
		}
		TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("op"), TEXT("prefs"));
		M->SetObjectField(TEXT("prefs"), P);
		ToPage(M);
	}

	// ---- writing -----------------------------------------------------------
	bool WriteText(const FString& Path, const FString& Text)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		const bool bOk = FFileHelper::SaveStringToFile(Text, *Path,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if (!bOk)
		{
			GLastError = FString::Printf(TEXT("could not write %s"), *Path);
			UE_LOG(LogBF6UiBuilder, Warning, TEXT("%s"), *GLastError);
		}
		return bOk;
	}

	// THE AUTHORED DESIGN IS NOT AN EXPORT, so it does not get an export's
	// write. Every other format here is generated output: lose one and the
	// builder regenerates it from the design. Lose the design and the work is
	// gone, and this used to write straight over the previous one, so a write
	// that failed part way replaced a whole design with a truncated file.
	//
	// Written beside it, read back whole, then swapped in, with the previous
	// version put back and the result of THAT checked as well. When neither
	// move works both documents are kept under their own names and named in
	// the log, because deleting either one is deleting the only copy of it.
	bool WriteDesignDurable(const FString& Path, const FString& Text)
	{
		IFileManager& FM = IFileManager::Get();
		FM.MakeDirectory(*FPaths::GetPath(Path), true);

		const FString Temp = Path + TEXT(".bf6tmp");
		FM.Delete(*Temp, false, true, true);
		if (!FFileHelper::SaveStringToFile(Text, *Temp,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			GLastError = FString::Printf(TEXT("could not write %s"), *Temp);
			UE_LOG(LogBF6UiBuilder, Warning, TEXT("%s"), *GLastError);
			return false;
		}
		{
			FString Back;
			if (!FFileHelper::LoadFileToString(Back, *Temp) || !Back.Equals(Text, ESearchCase::CaseSensitive))
			{
				FM.Delete(*Temp, false, true, true);
				GLastError = FString::Printf(
					TEXT("the new %s did not read back whole, so the design already on disk was left alone"),
					*FPaths::GetCleanFilename(Path));
				UE_LOG(LogBF6UiBuilder, Warning, TEXT("%s"), *GLastError);
				return false;
			}
		}

		const FString Prev = Path + TEXT(".bf6prev");
		const bool bHad = FPaths::FileExists(Path);
		if (bHad)
		{
			FM.Delete(*Prev, false, true, true);
			if (!FM.Move(*Prev, *Path, true, true))
			{
				FM.Delete(*Temp, false, true, true);
				GLastError = FString::Printf(TEXT("could not move the previous %s aside"),
					*FPaths::GetCleanFilename(Path));
				UE_LOG(LogBF6UiBuilder, Warning, TEXT("%s"), *GLastError);
				return false;   // the design already on disk is untouched
			}
		}
		if (!FM.Move(*Path, *Temp, true, true))
		{
			if (!bHad)
			{
				GLastError = FString::Printf(
					TEXT("could not put the design at %s; the new content is at %s"), *Path, *Temp);
				UE_LOG(LogBF6UiBuilder, Warning, TEXT("%s"), *GLastError);
				return false;
			}
			if (FM.Move(*Path, *Prev, true, true))
			{
				FM.Delete(*Temp, false, true, true);
				GLastError = FString::Printf(
					TEXT("could not replace %s, so the previous design was put back; nothing was lost"),
					*FPaths::GetCleanFilename(Path));
				UE_LOG(LogBF6UiBuilder, Warning, TEXT("%s"), *GLastError);
				return false;
			}
			GLastError = FString::Printf(
				TEXT("could not replace %s and could not put the previous design back; the previous one is at %s and the new one is at %s"),
				*FPaths::GetCleanFilename(Path), *Prev, *Temp);
			UE_LOG(LogBF6UiBuilder, Error,
				TEXT("Nothing was lost, but the design is not where it belongs: the previous version is at %s and the new one is at %s. Close anything holding that folder open and rename one of them to %s."),
				*Prev, *Temp, *Path);
			return false;
		}
		if (bHad) { FM.Delete(*Prev, false, true, true); }
		return true;
	}

	// A design name, made safe for a file name without becoming unrecognisable.
	FString SafeName(const FString& In)
	{
		FString Out;
		for (const TCHAR C : In)
		{
			if (FChar::IsAlnum(C) || C == TEXT('_') || C == TEXT('-')) Out.AppendChar(C);
			else if (C == TEXT(' ')) Out.AppendChar(TEXT('_'));
		}
		return Out.IsEmpty() ? TEXT("design") : Out;
	}

	// ---- WHERE AN AUTHORED DESIGN LIVES ------------------------------------
	//
	// It used to live wherever the person happened to point Save To File, and
	// nowhere at all if they never pressed it. So a design was not part of the
	// project it belonged to, was not in the project's snapshots, and did not
	// come back when the editor was restarted.
	//
	// The project contract already names the folder: unreal/ui, one json per
	// design, listed in the manifest as kind "ui". That is what this returns
	// whenever a save is open, so a design is project-owned by default and is
	// carried by every snapshot, move and sync the project gets. With no save
	// open there is no project to own it, and it goes to the tool's own
	// designs folder instead of being lost.
	FString DesignDir()
	{
		const FString Save = BF6Api::CurrentSave();
		if (!Save.IsEmpty())
		{
			const FString D = BF6Project::UiDesignDir(Save);
			if (!D.IsEmpty()) return D;
		}
		return FPaths::Combine(SavedDir(), TEXT("designs"));
	}

	// THE RECOVERY COPY, always outside the project.
	//
	// It is deliberately not the same file as the saved design: a recovery
	// candidate is what the page had when it last told us anything, which is
	// not necessarily what the person meant to keep. Keeping it apart is what
	// lets the tool say "there is newer work than the design you saved" instead
	// of having already overwritten one with the other.
	FString RecoveryDir()
	{
		return FPaths::Combine(SavedDir(), TEXT("recovery"));
	}

	FString DesignFileFor(const FString& Name)
	{
		return FPaths::Combine(DesignDir(), SafeName(Name) + TEXT(".design.json"));
	}
	// RECOVERY BELONGS TO A PROJECT, NOT JUST TO A NAME.
	//
	// This was one shared folder keyed by the design's name, so two projects
	// each holding a design called HUD wrote to the same file: one overwrote
	// the other's only unsaved copy, and recovery could offer somebody a design
	// belonging to a project they were not in. The project it came from is part
	// of the address.
	FString RecoveryFileFor(const FString& Name)
	{
		const FString Owner = BF6Api::CurrentSave();
		const FString Folder = Owner.IsEmpty()
			? FPaths::Combine(RecoveryDir(), TEXT("_unlinked"))
			: FPaths::Combine(RecoveryDir(), SafeName(Owner));
		return FPaths::Combine(Folder, SafeName(Name) + TEXT(".design.json"));
	}

	// Where a design saved before this change would have gone. Read when the
	// project-scoped one is absent, so nothing already on disk is stranded.
	FString LegacyRecoveryFileFor(const FString& Name)
	{
		return FPaths::Combine(RecoveryDir(), SafeName(Name) + TEXT(".design.json"));
	}

	// format -> the file a Save To File writes when the page did not name one.
	FString DefaultFileFor(const FString& Format, const FString& Name)
	{
		// The design is the authored document and goes to the project; every
		// other format is generated output and goes to the export folder.
		if (Format == TEXT("design") || Format.IsEmpty()) return DesignFileFor(Name);
		const FString Base = FPaths::Combine(SavedDir(), TEXT("export"), SafeName(Name));
		if (Format == TEXT("typescript")) return Base + TEXT(".ui.ts");
		if (Format == TEXT("strings"))    return FPaths::Combine(FPaths::GetPath(Base), TEXT("strings.json"));
		if (Format == TEXT("anim"))       return Base + TEXT(".anim.ts");
		if (Format == TEXT("deluca"))     return Base + TEXT(".ui-module.ts");
		if (Format == TEXT("solid"))      return Base + TEXT(".solid.ts");
		if (Format == TEXT("blocks"))     return Base + TEXT(".blocks.json");
		if (Format == TEXT("community"))  return Base + TEXT(".uibuilder.json");
		return Base + TEXT(".design.json");
	}

	// Write one authored design: the project copy, and a recovery copy beside
	// it so a design still has somewhere to come back from when the project
	// write is what failed. Returns whether the design is safely on disk in at
	// least one of the two places, and says which.
	bool SaveDesign(const FString& Name, const FString& Json, const FString& PathOrEmpty, FString& OutWhat)
	{
		const FString Path = PathOrEmpty.IsEmpty() ? DesignFileFor(Name) : PathOrEmpty;
		const bool bMain = WriteDesignDurable(Path, Json);
		const bool bCopy = WriteDesignDurable(RecoveryFileFor(Name), Json);

		if (bMain)
		{
			GLastDesign = Path;
			// Only a design inside a project belongs in that project's
			// manifest, and only the project knows which files it owns.
			const FString Save = BF6Api::CurrentSave();
			const FString Dir = Save.IsEmpty() ? FString() : BF6Project::UiDesignDir(Save);
			if (!Dir.IsEmpty() && Path.StartsWith(Dir))
			{
				BF6Project::NoteArtefactWritten(Save,
					FString(TEXT("unreal/ui/")) + FPaths::GetCleanFilename(Path), TEXT("user"));
			}
			OutWhat = FString::Printf(TEXT("Saved the design to %s"), *Path);
			return true;
		}
		if (bCopy)
		{
			OutWhat = FString::Printf(
				TEXT("The design could not be written to %s, so it was kept at %s instead. Move it there by hand once that folder is writable."),
				*Path, *RecoveryFileFor(Name));
			return true;
		}
		OutWhat = FString::Printf(TEXT("The design could not be written to %s or to %s, so it is still only in the page. Do not close the editor: fix the disk and press save again."),
			*Path, *RecoveryFileFor(Name));
		return false;
	}

	// A recovery candidate that is NEWER than the design it belongs to. That is
	// the only case worth telling anybody about: the page had work the saved
	// design does not carry.
	bool NewerRecovery(FString& OutPath, FString& OutName)
	{
		IFileManager& FM = IFileManager::Get();
		TArray<FString> Files;
		// THIS PROJECT'S recovery only. Scanning the shared root offered a
		// design belonging to whatever project happened to have saved one last,
		// which is how somebody gets handed another mod's HUD.
		const FString Mine = FPaths::GetPath(RecoveryFileFor(TEXT("x")));
		FM.FindFiles(Files, *(Mine / TEXT("*.design.json")), true, false);
		bool bFound = false;
		FDateTime Newest = FDateTime::MinValue();
		for (const FString& F : Files)
		{
			const FString Rec = FPaths::Combine(Mine, F);
			const FDateTime RecAt = FM.GetTimeStamp(*Rec);
			const FString Saved = FPaths::Combine(DesignDir(), F);
			const FDateTime SavedAt = FPaths::FileExists(Saved) ? FM.GetTimeStamp(*Saved) : FDateTime::MinValue();
			if (RecAt <= SavedAt) continue;
			if (RecAt <= Newest) continue;
			Newest = RecAt;
			OutPath = Rec;
			OutName = F.Replace(TEXT(".design.json"), TEXT(""));
			bFound = true;
		}
		return bFound;
	}
}

// ============================================================================
// Messages from the page
// ============================================================================
void BF6UiBuilder::HandleMessage(const FString& Json)
{
	TSharedPtr<FJsonObject> M = Parse(Json);
	if (!M.IsValid()) { GLastError = TEXT("the page sent something that is not JSON"); return; }
	GIn++;

	FString Op;
	M->TryGetStringField(TEXT("op"), Op);

	if (Op == TEXT("ready"))
	{
		++GPageGeneration;
		GPageReady = true;
		FString V;
		M->TryGetStringField(TEXT("v"), V);
		SendPalette();
		SendPrefs();
		{
			TSharedRef<FJsonObject> Catalogue = MakeShared<FJsonObject>();
			Catalogue->SetStringField(TEXT("op"), TEXT("equipmentCatalogue"));
			TArray<TSharedPtr<FJsonValue>> Items, Attachments;
			for (const FString& Item : BF6Ext::LootItems()) Items.Add(MakeShared<FJsonValueString>(Item));
			for (const FString& Attachment : BF6Ext::WeaponAttachmentItems()) Attachments.Add(MakeShared<FJsonValueString>(Attachment));
			Catalogue->SetArrayField(TEXT("items"), Items);
			Catalogue->SetArrayField(TEXT("attachments"), Attachments);
			ToPage(Catalogue);
		}
		if (!GPendingDesignMessage.IsEmpty()) { ToPageRaw(GPendingDesignMessage); GPendingDesignMessage.Reset(); }
		UE_LOG(LogBF6UiBuilder, Display, TEXT("UI builder page ready (core %s)."), *V);

		// A recovery candidate is offered, never applied. Loading it on its own
		// would replace whatever the person has open with work from a previous
		// session, which is the same mistake in the other direction.
		{
			FString RecPath, RecName;
			if (NewerRecovery(RecPath, RecName))
			{
				const FString Say = FString::Printf(
					TEXT("There is newer work for '%s' than the saved design. BF6.UI.Recover loads it."), *RecName);
				UE_LOG(LogBF6UiBuilder, Display, TEXT("%s It is at %s."), *Say, *RecPath);
				Note(Say);
			}
		}
		// A console command that arrived before the page was up.
		if (!GPendingExportFormat.IsEmpty())
		{
			TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("op"), TEXT("export"));
			E->SetStringField(TEXT("format"), GPendingExportFormat);
			E->SetStringField(TEXT("path"), GPendingExportPath);
			ToPage(E);
			GPendingExportFormat.Reset();
			GPendingExportPath.Reset();
		}
		return;
	}

	// One panel state, remembered. The key list is closed, so a page that sends
	// something unexpected is ignored rather than given a new line in the ini.
	if (Op == TEXT("equipmentPreview"))
	{
		FString RequestId;
		if (!M->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.Len() > 128) return;
		const uint64 Generation = GPageGeneration;
		const TWeakPtr<SWebBrowser> Browser = GBrowser;
		BF6Ext::RequestEquipmentPreview(Json, [Generation, Browser, RequestId](FString Result)
		{
			check(IsInGameThread());
			if (Generation != GPageGeneration || !Browser.IsValid() || Browser.Pin() != GBrowser) return;
			TSharedPtr<FJsonObject> Reply = Parse(Result);
			if (!Reply) { Reply = MakeShared<FJsonObject>(); Reply->SetStringField(TEXT("error"), TEXT("Equipment preview returned an invalid response.")); }
			Reply->SetStringField(TEXT("op"), TEXT("equipmentPreview"));
			Reply->SetStringField(TEXT("requestId"), RequestId);
			ToPage(Reply.ToSharedRef());
		});
		return;
	}
	if (Op == TEXT("pref"))
	{
		FString Name, Value;
		M->TryGetStringField(TEXT("name"), Name);
		M->TryGetStringField(TEXT("value"), Value);
		if (!IsKnownUiPref(Name))
		{
			UE_LOG(LogBF6UiBuilder, Verbose, TEXT("%s is not a panel state this editor keeps."), *Name);
			return;
		}
		if (GConfig)
		{
			GConfig->SetString(kPrefSection, *UiPrefIniKey(Name), *Value, GEditorPerProjectIni);
			GConfig->Flush(false, GEditorPerProjectIni);
		}
		return;
	}

	// THE AUTHORED DESIGN, HANDED OVER WITHOUT AN EXPORT SHEET.
	//
	// This is the seam the recovery half needs and the page does not use yet.
	// The editor page keeps the design in its own model and only ever hands it
	// out through the export sheet, so a design customised and not exported
	// exists nowhere but in the page, and a restarted editor or a reloaded
	// panel loses it. That is the whole of CORE-15 and the page owns half of
	// it: it needs to send {op:"designState", name, json} whenever the design
	// changes and has settled. This side is ready for it, and a message that
	// never arrives simply means no recovery candidate is ever written.
	if (Op == TEXT("designState"))
	{
		FString Name, Text;
		M->TryGetStringField(TEXT("name"), Name);
		M->TryGetStringField(TEXT("json"), Text);
		if (Text.IsEmpty()) return;
		if (Name.IsEmpty()) Name = TEXT("design");
		// A recovery copy ONLY. An autosave is not somebody pressing save, so
		// it must never replace the design they did press save on.
		if (WriteDesignDurable(RecoveryFileFor(Name), Text))
		{
			UE_LOG(LogBF6UiBuilder, Verbose, TEXT("Recovery copy of '%s' written."), *Name);
		}
		return;
	}

	if (Op == TEXT("exportFile"))
	{
		FString Format, Text, Name, Path;
		M->TryGetStringField(TEXT("format"), Format);
		M->TryGetStringField(TEXT("text"), Text);
		M->TryGetStringField(TEXT("name"), Name);
		M->TryGetStringField(TEXT("path"), Path);

		// The design is the authored document, not an export of one, so it goes
		// through the durable write and into the project rather than over the
		// top of whatever was at that path.
		if (Format == TEXT("design"))
		{
			FString What;
			const bool bSaved = SaveDesign(Name.IsEmpty() ? TEXT("design") : Name, Text, Path, What);
			// Braces are not optional here: UE_LOG expands to more than one
			// statement.
			if (bSaved)
			{
				UE_LOG(LogBF6UiBuilder, Display, TEXT("%s"), *What);
			}
			else
			{
				UE_LOG(LogBF6UiBuilder, Warning, TEXT("%s"), *What);
			}
			Note(What);
			return;
		}

		if (Path.IsEmpty()) Path = DefaultFileFor(Format, Name);
		const bool bOk = WriteText(Path, Text);
		if (bOk)
		{
			GLastExport = Path;
			UE_LOG(LogBF6UiBuilder, Display, TEXT("Wrote the %s export to %s (%d characters)."),
				*Format, *Path, Text.Len());
		}
		Note(bOk ? FString::Printf(TEXT("Wrote %s"), *Path) : TEXT("Could not write that path."));
		return;
	}

	if (Op == TEXT("insertBlocks"))
	{
		// The blocks editor already knows how to take a snippet by name, and
		// its snippet folder is on disk, so the whole seam is: write the file,
		// open the tab, ask for it. Nothing in BF6Blocks has to change.
		FString Name, Snippet;
		M->TryGetStringField(TEXT("name"), Name);
		M->TryGetStringField(TEXT("snippet"), Snippet);
		if (Snippet.IsEmpty()) { Note(TEXT("The page produced no block tree.")); return; }

		const FString File = SafeName(Name) + TEXT(".json");
		const FString Path = FPaths::Combine(SnippetDir(), File);
		if (!WriteText(Path, Snippet)) { Note(TEXT("Could not write the snippet.")); return; }

		BF6Blocks::Open();
		TSharedRef<FJsonObject> Ask = MakeShared<FJsonObject>();
		Ask->SetStringField(TEXT("op"), TEXT("snippetLoad"));
		Ask->SetStringField(TEXT("name"), TEXT("uibuilder/") + File);
		BF6Blocks::HandleMessage(Write(Ask));

		UE_LOG(LogBF6UiBuilder, Display,
			TEXT("Handed %s to the blocks editor as snippet uibuilder/%s."), *Name, *File);
		Note(FString::Printf(TEXT("Sent to the blocks editor as uibuilder/%s"), *File));
		return;
	}

	if (Op == TEXT("insertScript"))
	{
		// BF6Script has no text seam yet (see the note in BF6UiBuilder.h), so
		// the files are written where the user can pick them up and the script
		// editor is opened on top.
		FString Name, Ts, Anim;
		M->TryGetStringField(TEXT("name"), Name);
		M->TryGetStringField(TEXT("ts"), Ts);
		M->TryGetStringField(TEXT("anim"), Anim);

		const TSharedPtr<FJsonObject>* Strings = nullptr;
		FString StringsText = TEXT("{}");
		if (M->TryGetObjectField(TEXT("strings"), Strings) && Strings && (*Strings).IsValid())
		{
			TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&StringsText);
			FJsonSerializer::Serialize((*Strings).ToSharedRef(), W);
		}

		const FString Dir = FPaths::Combine(SavedDir(), TEXT("export"), SafeName(Name));
		bool bOk = true;
		bOk &= WriteText(FPaths::Combine(Dir, SafeName(Name) + TEXT(".ui.ts")), Ts);
		bOk &= WriteText(FPaths::Combine(Dir, TEXT("strings.json")), StringsText);
		if (!Anim.IsEmpty())
		{
			bOk &= WriteText(FPaths::Combine(Dir, SafeName(Name) + TEXT(".anim.ts")), Anim);
			// The runtime the animation export imports, copied in beside it so
			// the folder is complete on its own.
			FString Runtime;
			const FString RuntimeSrc = FPaths::Combine(ResDir(), TEXT("runtime"), TEXT("ui-anim.ts"));
			if (FFileHelper::LoadFileToString(Runtime, *RuntimeSrc))
			{
				bOk &= WriteText(FPaths::Combine(Dir, TEXT("ui-anim.ts")), Runtime);
			}
		}

		BF6Script::Open();
		UE_LOG(LogBF6UiBuilder, Display,
			TEXT("Wrote the script export for %s to %s. %s"), *Name, *Dir,
			BF6Script::HasProject()
				? TEXT("Copy the files into the open project's src folder.")
				: TEXT("Make or open a script project, then copy the files into its src folder."));
		Note(bOk
			? FString::Printf(TEXT("Wrote the TypeScript to %s"), *Dir)
			: TEXT("Some of the script files could not be written."));
		return;
	}

	if (Op == TEXT("openSettings"))
	{
		// HIDE GAME HUD ELEMENTS. The other half of the layering answer: when a
		// design and the game's own HUD both want the same corner, the fix is
		// often to switch that HUD element off for the experience rather than
		// to move the design. The site does that on Modifiers > UI, through
		// mutators such as CompassAllowed_PerTeam, MinimapAllowed_PerTeam and
		// limited_hud.
		//
		// BF6PortalSettings.h does not exist yet (another agent is writing it).
		// When it lands, this whole branch becomes one line:
		//
		//     #include "BF6PortalSettings.h"
		//     BF6PortalSettings::OpenPage(Page);      // Page is "ui" here
		//
		// and the include goes at the top of this file. Nothing else changes:
		// the page already sends {op:"openSettings", page:"ui"} from the link
		// in the LAYER control, and it sends it whether or not anything is
		// listening. Until then the tool says where to go, so the link is
		// useful rather than dead.
		// The settings module lives as a column inside the Portal panel; it has
		// no per-page open yet, so show the panel and refresh the column, and
		// say which section carries the HUD switches.
		FString Page;
		M->TryGetStringField(TEXT("page"), Page);
		BF6PortalWeb::Open();
		BF6PortalSettings::Refresh();
		UE_LOG(LogBF6UiBuilder, Display,
			TEXT("Hide game HUD elements: Portal panel opened; pick Modifiers > UI in the SETTINGS column (page '%s')."),
			Page.IsEmpty() ? TEXT("ui") : *Page);
		Note(TEXT("Portal panel opened. In SETTINGS pick Modifiers > UI to switch default HUD elements off."));
		return;
	}

	if (Op == TEXT("log"))
	{
		FString Text;
		M->TryGetStringField(TEXT("text"), Text);
		UE_LOG(LogBF6UiBuilder, Display, TEXT("[page] %s"), *Text);
		return;
	}

	GLastError = FString::Printf(TEXT("unknown message from the page: %s"), *Op);
}

void UBF6UiBuilderBridge::call(FString Json)
{
	BF6UiBuilder::HandleMessage(Json);
}

void UBF6UiBuilderBridge::log(FString Json)
{
	TSharedPtr<FJsonObject> M = Parse(Json);
	if (!M.IsValid()) return;
	FString Level, Msg;
	M->TryGetStringField(TEXT("level"), Level);
	M->TryGetStringField(TEXT("msg"), Msg);
	// Braces are not optional here: UE_LOG expands to more than one statement.
	if (Level == TEXT("error"))
	{
		UE_LOG(LogBF6UiBuilder, Warning, TEXT("[page] %s"), *Msg);
	}
	else
	{
		UE_LOG(LogBF6UiBuilder, Display, TEXT("[page] %s"), *Msg);
	}
}

// ============================================================================
// The tab
// ============================================================================
namespace
{
	bool WebAvailable()
	{
		FModuleManager::Get().LoadModulePtr<IWebBrowserModule>(TEXT("WebBrowser"));
		return IWebBrowserModule::IsAvailable() && IWebBrowserModule::Get().IsWebModuleAvailable();
	}

	TSharedRef<SWidget> MakeUnavailableView()
	{
		return SNew(SBorder)
			.Padding(16.f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(FText::FromString(TEXT(
					"The UI builder needs the engine's embedded browser, which this build does not have. "
					"Design your UI on tools.bfportal.gg and import the export here when you have a build "
					"with the CEF binaries.")))
			];
	}

	// ---- BF6EditorOverlay ----
	// THE PAGE IS MADE ONCE. It used to be built by the tab spawner and thrown
	// away by the tab's X, which meant an unexported design was lost every time
	// the tab was closed - and would have been lost on every move between the
	// tab and the full-screen host. Now the browser is created here, on the
	// first ask, and lives until the module shuts down.
	TSharedRef<SWidget> EnsurePage()
	{
		if (GBrowser.IsValid()) return GBrowser.ToSharedRef();
		if (!WebAvailable()) return MakeUnavailableView();

		GBrowser = SNew(SWebBrowser)
			.InitialURL(EditorPageUrl())
			.ShowControls(false)
			.ShowAddressBar(false)
			.ShowErrorMessage(true)
			.ShowInitialThrobber(true)
			.SupportsTransparency(false)
			.BackgroundColor(FColor(0x0D, 0x0F, 0x10));

		if (GBridge.IsValid())
		{
			GBrowser->BindUObject(kBridgeName, GBridge.Get(), true);
		}
		return GBrowser.ToSharedRef();
	}
	// ---- end BF6EditorOverlay ----

	TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs&)
	{
		TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab);
		// ---- BF6EditorOverlay ----
		// A tab asking for the page takes it off the full-screen host: the two
		// placements share one widget, and whichever one is asked for wins.
		BF6EditorOverlay::HideIfShowing(BF6EditorOverlay::EEditor::Ui);
		if (!GHost.IsValid()) GHost = SNew(SBox);
		GHost->SetContent(EnsurePage());
		Tab->SetContent(GHost.ToSharedRef());
		GTab = Tab;
		Tab->SetOnTabClosed(SDockTab::FOnTabClosedCallback::CreateLambda([](TSharedRef<SDockTab>)
		{
			// Closed by its own X: the page keeps running, unparented, and
			// comes straight back with the design still on it.
			if (GHost.IsValid()) GHost->SetContent(SNullWidget::NullWidget);
			GTab.Reset();
		}));
		// ---- end BF6EditorOverlay ----
		return Tab;
	}
}

// ============================================================================
// The public face
// ============================================================================
void BF6UiBuilder::Open()
{
	// ---- BF6EditorOverlay ----
	// Asking for the tab moves the page into it, so the host must let go first.
	BF6EditorOverlay::HideIfShowing(BF6EditorOverlay::EEditor::Ui);
	// ---- end BF6EditorOverlay ----
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(kTabId));
	// A tab that was already open does not spawn again, so fill it here too.
	if (GHost.IsValid() && GTab.IsValid()) GHost->SetContent(EnsurePage());
}

// ---- BF6EditorOverlay ----
TSharedRef<SWidget> BF6UiBuilder::Widget()
{
	TSharedRef<SWidget> W = EnsurePage();
	// One parent only: the dock tab gives it up while the host holds it, and
	// the tab is left empty rather than closed, so it is still there to go
	// back into.
	if (GHost.IsValid()) GHost->SetContent(SNullWidget::NullWidget);
	return W;
}

void BF6UiBuilder::ReleaseWidget()
{
	if (GHost.IsValid() && GTab.IsValid()) GHost->SetContent(EnsurePage());
}
// ---- end BF6EditorOverlay ----

FString BF6UiBuilder::ExportDir()
{
	return FPaths::Combine(SavedDir(), TEXT("export"));
}

bool BF6UiBuilder::LoadFile(const FString& Path)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		GLastError = FString::Printf(TEXT("cannot read %s"), *Path);
		UE_LOG(LogBF6UiBuilder, Warning, TEXT("%s"), *GLastError);
		return false;
	}
	Open();

	// A strings.json beside the design, when the file is a ParseUI tree that
	// needs one to say what its text actually reads.
	FString Strings;
	FFileHelper::LoadFileToString(Strings,
		*FPaths::Combine(FPaths::GetPath(Path), TEXT("strings.json")));

	TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
	M->SetStringField(TEXT("op"), TEXT("design"));
	M->SetStringField(TEXT("json"), Text);
	M->SetStringField(TEXT("name"), FPaths::GetBaseFilename(Path));
	if (!Strings.IsEmpty()) M->SetStringField(TEXT("strings"), Strings);
	if (GPageReady) ToPage(M);
	else GPendingDesignMessage = Write(M);

	GLastDesign = Path;
	UE_LOG(LogBF6UiBuilder, Display, TEXT("Loaded %s into the UI builder (%d characters)."),
		*Path, Text.Len());
	return true;
}

bool BF6UiBuilder::ExportTo(const FString& Format, const FString& Path)
{
	static const TCHAR* kFormats[] = { TEXT("typescript"), TEXT("strings"), TEXT("anim"),
		TEXT("deluca"), TEXT("solid"), TEXT("blocks"), TEXT("community"), TEXT("design") };
	bool bKnown = false;
	for (const TCHAR* F : kFormats) if (Format == F) bKnown = true;
	if (!bKnown)
	{
		UE_LOG(LogBF6UiBuilder, Warning,
			TEXT("Unknown format '%s'. Use one of: typescript, strings, anim, deluca, solid, blocks, community, design."),
			*Format);
		return false;
	}

	Open();
	if (!GPageReady)
	{
		// The page is still loading; do it the moment it says hello.
		GPendingExportFormat = Format;
		GPendingExportPath = Path;
		UE_LOG(LogBF6UiBuilder, Display, TEXT("Waiting for the builder page, then exporting %s."), *Format);
		return true;
	}

	TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
	M->SetStringField(TEXT("op"), TEXT("export"));
	M->SetStringField(TEXT("format"), Format);
	M->SetStringField(TEXT("path"), Path);
	ToPage(M);
	return true;
}

// ---- project-owned designs -------------------------------------------------
//
// A design used to leave this feature only as an export the person chose to
// write, which meant it belonged to no project, was in no snapshot, and did not
// survive a restart. These three are the host half of fixing that: save it
// where the project contract says designs live, list what is there, and put
// back what a previous session left behind.
//
// THE PAGE OWNS THE OTHER HALF and does not do it yet. It keeps the design in
// its own model and hands it out only through the export sheet, so a save
// asked for from here has to go through that sheet, and there is no automatic
// save at all. Resources/uibuilder/editor.js needs to send
// {op:"designState", name, json} when the design settles for the recovery copy
// to be written without the person pressing anything.
bool BF6UiBuilder::SaveDesignToProject()
{
	// THIS CANNOT WRITE THE FILE ON ITS OWN, and saying otherwise would be the
	// worst kind of wrong for a save command.
	//
	// The page hands the design out only through its export sheet, and it sends
	// it back only when the host named a path. The host does not know the
	// design's name until the page tells it one, so naming a path here would
	// save the design under a guessed name. What it can do is put the sheet on
	// the design format and say exactly where SAVE TO FILE will put it - that
	// button sends the design with its real name, and this side writes it into
	// the project.
	//
	// The page sending {op:"designState", name, json} would remove the button
	// press from this entirely. See the note in BF6UiBuilder.h.
	const bool bAsked = BF6UiBuilder::ExportTo(TEXT("design"), FString());
	UE_LOG(LogBF6UiBuilder, Display,
		TEXT("Opened the export sheet on the design format. Press SAVE TO FILE there and the design goes into %s, where the project's snapshots pick it up."),
		*DesignDir());
	return bAsked;
}

FString BF6UiBuilder::Designs()
{
	IFileManager& FM = IFileManager::Get();
	const FString Save = BF6Api::CurrentSave();
	const FString Dir = DesignDir();

	FString Out = FString::Printf(TEXT("UI designs\n  project : %s\n  folder  : %s\n"),
		Save.IsEmpty() ? TEXT("none open, so designs are not project-owned right now") : *Save, *Dir);

	TArray<FString> Files;
	FM.FindFiles(Files, *(Dir / TEXT("*.design.json")), true, false);
	Files.Sort();
	if (Files.Num() == 0) Out += TEXT("  (no designs saved here yet)\n");
	for (const FString& F : Files)
	{
		const FString P = FPaths::Combine(Dir, F);
		Out += FString::Printf(TEXT("  %-40s %s\n"), *F, *FM.GetTimeStamp(*P).ToString());
	}

	TArray<FString> Recs;
	FM.FindFiles(Recs, *(RecoveryDir() / TEXT("*.design.json")), true, false);
	Recs.Sort();
	if (Recs.Num())
	{
		Out += FString::Printf(TEXT("\n  work left behind by a previous session, at %s:\n"), *RecoveryDir());
		for (const FString& F : Recs)
		{
			const FString P = FPaths::Combine(RecoveryDir(), F);
			const FString Saved = FPaths::Combine(Dir, F);
			const bool bNewer = !FPaths::FileExists(Saved)
				|| FM.GetTimeStamp(*P) > FM.GetTimeStamp(*Saved);
			Out += FString::Printf(TEXT("  %-40s %s  %s\n"), *F, *FM.GetTimeStamp(*P).ToString(),
				bNewer ? TEXT("NEWER than the saved design") : TEXT("older than the saved design"));
		}
		Out += TEXT("  BF6.UI.Recover [name] loads one of these.\n");
	}
	return Out;
}

bool BF6UiBuilder::Recover(const FString& Name)
{
	FString Path, Which;
	if (Name.IsEmpty())
	{
		if (!NewerRecovery(Path, Which))
		{
			UE_LOG(LogBF6UiBuilder, Display,
				TEXT("There is no unsaved work left behind: every recovery copy is older than the design saved beside it. BF6.UI.Designs lists both."));
			return false;
		}
	}
	else
	{
		Path = RecoveryFileFor(Name);
		if (!FPaths::FileExists(Path))
		{
			// Anything written before recovery was project-scoped still lives
			// in the old shared folder. Read it rather than telling somebody
			// their work is not there.
			const FString Legacy = LegacyRecoveryFileFor(Name);
			if (FPaths::FileExists(Legacy))
			{
				Path = Legacy;
				UE_LOG(LogBF6UiBuilder, Display,
					TEXT("Recovering '%s' from the older shared folder. It is not tied to a project, so check it is the one you meant."),
					*Name);
			}
			else
			{
				UE_LOG(LogBF6UiBuilder, Warning,
					TEXT("There is no recovery copy called '%s' for this project at %s. BF6.UI.Designs lists what is there."),
					*Name, *FPaths::GetPath(Path));
				return false;
			}
		}
	}
	// Loaded into the page, NOT copied over the saved design. Recovering is a
	// thing the person then looks at and decides to keep; overwriting the saved
	// design on their behalf would be the same loss in the other direction.
	UE_LOG(LogBF6UiBuilder, Display,
		TEXT("Loading %s into the builder. It is not saved over your design until you save it."), *Path);
	return LoadFile(Path);
}

FString BF6UiBuilder::Status()
{
	return FString::Printf(TEXT(
		"BF6 UI Builder\n"
		"  page          : %s\n"
		"  messages      : %d in, %d out\n"
		"  page source   : %s\n"
		"  designs       : %s\n"
		"  recovery      : %s\n"
		"  exports       : %s\n"
		"  block snippets: %s\n"
		"  last design   : %s\n"
		"  last export   : %s\n"
		"  last error    : %s"),
		GPageReady ? TEXT("open") : TEXT("closed"),
		GIn, GOut,
		*ResDir(),
		*DesignDir(),
		*RecoveryDir(),
		*ExportDir(),
		*SnippetDir(),
		GLastDesign.IsEmpty() ? TEXT("none") : *GLastDesign,
		GLastExport.IsEmpty() ? TEXT("none") : *GLastExport,
		GLastError.IsEmpty() ? TEXT("none") : *GLastError);
}

// ============================================================================
// Register
// ============================================================================
void BF6UiBuilder::Register()
{
	UBF6UiBuilderBridge* Bridge = NewObject<UBF6UiBuilderBridge>(GetTransientPackage(),
		TEXT("BF6UiBuilderBridge"), RF_Transient);
	Bridge->AddToRoot();
	GBridge = Bridge;

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(kTabId, FOnSpawnTab::CreateStatic(&SpawnTab))
		.SetDisplayName(FText::FromString(TEXT("BF6 UI Builder")))
		.SetTooltipText(FText::FromString(TEXT(
			"Design and animate a Portal HUD on a 1920 by 1080 safe-area canvas, then export it as "
			"ParseUI TypeScript, a UI-module component, or a block tree.")))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

	IConsoleManager& CM = IConsoleManager::Get();
	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.UI.Open"),
		TEXT("Open the UI builder tab."),
		FConsoleCommandDelegate::CreateStatic(&BF6UiBuilder::Open)));

	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.UI.Load"),
		TEXT("BF6.UI.Load <design.json>  Load a design, a community builder export, or a ParseUI tree."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (!Args.Num())
			{
				UE_LOG(LogBF6UiBuilder, Warning, TEXT("Give a path to a design file."));
				return;
			}
			BF6UiBuilder::LoadFile(FString::Join(Args, TEXT(" ")).TrimQuotes());
		})));

	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.UI.Export"),
		TEXT("BF6.UI.Export <format> <path>  format is typescript, strings, anim, deluca, solid, blocks, community or design."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogBF6UiBuilder, Warning,
					TEXT("BF6.UI.Export <format> <path>. Formats: typescript, strings, anim, deluca, solid, blocks, community, design."));
				return;
			}
			TArray<FString> Rest = Args;
			const FString Format = Rest[0].TrimQuotes();
			Rest.RemoveAt(0);
			BF6UiBuilder::ExportTo(Format, FString::Join(Rest, TEXT(" ")).TrimQuotes());
		})));

	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.UI.Save"),
		TEXT("Put the export sheet on the design format, ready to save the design into the project's unreal/ui folder where the project's snapshots pick it up."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			BF6UiBuilder::SaveDesignToProject();
		})));

	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.UI.Designs"),
		TEXT("The designs this project holds, and any unsaved work left behind by a previous session."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			UE_LOG(LogBF6UiBuilder, Display, TEXT("%s"), *BF6UiBuilder::Designs());
		})));

	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.UI.Recover"),
		TEXT("BF6.UI.Recover [name]  Load work a previous session left behind. No name takes the newest."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			BF6UiBuilder::Recover(Args.Num() ? FString::Join(Args, TEXT(" ")).TrimQuotes() : FString());
		})));

	GCmds.Add(CM.RegisterConsoleCommand(TEXT("BF6.UI.Status"),
		TEXT("What the UI builder is holding and where it writes."),
		FConsoleCommandDelegate::CreateLambda([]
		{
			UE_LOG(LogBF6UiBuilder, Display, TEXT("%s"), *BF6UiBuilder::Status());
		})));

	IFileManager::Get().MakeDirectory(*ExportDir(), true);
	IFileManager::Get().MakeDirectory(*SnippetDir(), true);

	UE_LOG(LogBF6UiBuilder, Display,
		TEXT("BF6 UI Builder ready. BF6.UI.Open, or Window > Tools > BF6 UI Builder."));
}

void BF6UiBuilder::Unregister()
{
	for (IConsoleObject* C : GCmds) if (C) IConsoleManager::Get().UnregisterConsoleObject(C);
	GCmds.Reset();

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(kTabId);
	// ---- BF6EditorOverlay ----
	if (GHost.IsValid()) GHost->SetContent(SNullWidget::NullWidget);
	GHost.Reset();
	GTab.Reset();
	// ---- end BF6EditorOverlay ----
	GBrowser.Reset();

	if (GBridge.IsValid()) { GBridge->RemoveFromRoot(); GBridge.Reset(); }
	GPageReady = false;
}
