#include "BF6SDKExtension.h"
#include "BF6BuildMode.h"
#include "BF6Blocks.h"
#include "BF6Script.h"
#include "BF6UiBuilder.h"
#include "BF6EditorOverlay.h"
#include "BF6Project.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "String/LexFromString.h"
#include "HAL/FileManager.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"

namespace BF6LootBindingDetail
{
 FString Json(const TSharedRef<FJsonObject>& O) { FString S; FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&S)); return S; }
 bool Write(const FString& Path,const FString& Text,FString& Why)
 {
  IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);
  const FString Temp=Path+TEXT(".pending");
  if (!FFileHelper::SaveStringToFile(Text,*Temp,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
   || !IFileManager::Get().Move(*Path,*Temp,true,true)) { Why=TEXT("Could not save the loot binding: ")+Path; return false; }
  return true;
 }
}

TArray<FString> BF6Ext::LootItems()
{
 static TArray<FString> Items;
 if (!Items.IsEmpty()) return Items;
 FString Text;
 if (!FFileHelper::LoadFileToString(Text,*FPaths::Combine(ToolPluginDir(),TEXT("Resources/blocks/offline/definitions_synth.json")))) return {};
 TSharedPtr<FJsonObject> Defs;
 if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Defs)||!Defs) return {};
 for (const TCHAR* Kind : {TEXT("Weapons"),TEXT("Gadgets"),TEXT("AmmoTypes"),TEXT("ArmorTypes")})
 {
  const TSharedPtr<FJsonObject>* Def=nullptr; const TArray<TSharedPtr<FJsonValue>>* Inputs=nullptr;
  if (!Defs->TryGetObjectField(FString(Kind)+TEXT("Item"),Def)||!(*Def)->TryGetArrayField(TEXT("inputs"),Inputs)) continue;
  for (const auto& Input:*Inputs)
  {
   const auto O=Input->AsObject(); const TArray<TSharedPtr<FJsonValue>>* Fields=nullptr;
   if (!O||!O->TryGetArrayField(TEXT("fields"),Fields)) continue;
   for (const auto& Field:*Fields)
   {
    const auto F=Field->AsObject(); FString Name; const TArray<TSharedPtr<FJsonValue>>* Options=nullptr;
    if (!F||!F->TryGetStringField(TEXT("name"),Name)||Name!=TEXT("VALUE-1")||!F->TryGetArrayField(TEXT("options"),Options)) continue;
    for (const auto& Option:*Options) { const auto& Pair=Option->AsArray(); if(Pair.Num()==2) Items.AddUnique(FString(Kind)+TEXT(".")+Pair[1]->AsString()); }
   }
  }
 }
 Items.Sort(); return Items;
}

TArray<FString> BF6Ext::WeaponAttachmentItems()
{
 TArray<FString> Items;
 FString Text;
 if (!FFileHelper::LoadFileToString(Text,*FPaths::Combine(ToolPluginDir(),TEXT("Resources/blocks/offline/definitions_synth.json")))) return {};
 TSharedPtr<FJsonObject> Defs;
 if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Defs)||!Defs) return {};
 for (const TCHAR* Kind : {TEXT("WeaponAttachments")})
 {
  const TSharedPtr<FJsonObject>* Def=nullptr; const TArray<TSharedPtr<FJsonValue>>* Inputs=nullptr;
  if (!Defs->TryGetObjectField(FString(Kind)+TEXT("Item"),Def)||!(*Def)->TryGetArrayField(TEXT("inputs"),Inputs)) continue;
  for (const auto& Input:*Inputs)
  {
   const auto O=Input->AsObject(); const TArray<TSharedPtr<FJsonValue>>* Fields=nullptr;
   if (!O||!O->TryGetArrayField(TEXT("fields"),Fields)) continue;
   for (const auto& Field:*Fields)
   {
    const auto F=Field->AsObject(); FString Name; const TArray<TSharedPtr<FJsonValue>>* Options=nullptr;
    if (!F||!F->TryGetStringField(TEXT("name"),Name)||Name!=TEXT("VALUE-1")||!F->TryGetArrayField(TEXT("options"),Options)) continue;
    for (const auto& Option:*Options) { const auto& Pair=Option->AsArray(); if(Pair.Num()==2) Items.AddUnique(Pair[1]->AsString()); }
   }
  }
 }
 Items.Sort(); return Items;
}

bool BF6Ext::OpenLootBinding(const FString& Id,const FString& Action,FString& OutWhy)
{
 using namespace BF6LootBindingDetail;
 if(Action!=TEXT("blocks")&&Action!=TEXT("script")&&Action!=TEXT("card")) { OutWhy=TEXT("Unknown loot binding action."); return false; }
 if (!IsEditing()||!GEditor||CurrentSave().IsEmpty()) { OutWhy=TEXT("Open a saved map first."); return false; }
 UWorld* World=GEditor->GetEditorWorldContext().World(); AActor* Actor=nullptr;
 if (World) for(TActorIterator<AActor> It(World);It;++It) if(It->GetPathName()==Id&&It->Tags.Contains(FName(TEXT("BF6Placed")))) { Actor=*It; break; }
 if(!Actor || (!Actor->Tags.Contains(FName(TEXT("type:LootSpawner")))&&!Actor->Tags.Contains(FName(TEXT("label:LootSpawner"))))) { OutWhy=TEXT("The loot spawner is no longer available."); return false; }
 FString Item,Preview=TEXT("carbine/m4a1");
 TMap<FString,FString> Fits;
 for(const FName& Tag:Actor->Tags)
 {
  const FString S=Tag.ToString();
  if(S.StartsWith(TEXT("preview:highpoly.portalitem="))) Item=S.Mid(28);
  if(S.StartsWith(TEXT("preview:highpoly.item="))) Preview=S.Mid(22);
  if(S.StartsWith(TEXT("preview:highpoly.attachment_")))
  {
   FString Slot,Value; if(S.Mid(FString(TEXT("preview:highpoly.attachment_")).Len()).Split(TEXT("="),&Slot,&Value)&&!Value.IsEmpty()) Fits.Add(Slot,Value);
  }
 }
 if(Item.IsEmpty()&&Preview==TEXT("carbine/m4a1")) Item=TEXT("Weapons.Carbine_M4A1");
 if(!LootItems().Contains(Item)) { OutWhy=TEXT("Choose the Portal gameplay item in the Loadout menu first."); return false; }
 int32 ObjId=0; const FString IdText=BF6Api::GetActorProp(Actor,TEXT("ObjId"));
 if (!LexTryParseString(ObjId,*IdText)||ObjId<1)
 {
  TSet<int32> Taken; for(const auto& R:BF6Api::GatherObjIds()) if(R.Id>=0) Taken.Add(R.Id);
  ObjId=1; while(Taken.Contains(ObjId)&&ObjId<MAX_int32) ++ObjId;
  if(Taken.Contains(ObjId)) { OutWhy=TEXT("No free object ID remains."); return false; }
  BF6Api::SetActorProp(Actor,TEXT("ObjId"),FString::FromInt(ObjId));
 }
 for(const auto& R:BF6Api::GatherObjIds()) if(R.Id==ObjId&&R.Actor.Get()!=Actor&&R.Type==TEXT("LootSpawner"))
 { OutWhy=TEXT("Two loot spawners share this ObjId. Assign a unique ID before connecting logic."); return false; }
 FString Kind,Member; Item.Split(TEXT("."),&Kind,&Member);
 TArray<FString> Slots; Fits.GetKeys(Slots); Slots.Sort();
 TArray<TSharedPtr<FJsonValue>> Attachments; TArray<FString> AttachmentCode;
 const TArray<FString> Allowed=WeaponAttachmentItems();
 for(const FString& Slot:Slots)
 {
  const FString& Fit=Fits[Slot];
  if(Kind!=TEXT("Weapons")||!Allowed.Contains(Fit)) { OutWhy=TEXT("The selected attachment is unavailable for export. Re-select it in Loadout."); return false; }
  Attachments.Add(MakeShared<FJsonValueString>(Fit)); AttachmentCode.Add(TEXT("mod.WeaponAttachments.")+Fit);
 }
 const FString Save=CurrentSave(), Level=CurrentLevel(), Base=BF6Project::DirFor(Save);
 const FString Stem=TEXT("loot_")+Level+TEXT("_")+FString::FromInt(ObjId);
 const FString BindingPath=FPaths::Combine(Base,TEXT("unreal/bindings"),Stem+TEXT(".json"));
 const auto Binding=MakeShared<FJsonObject>();
 Binding->SetStringField(TEXT("kind"),TEXT("loot")); Binding->SetNumberField(TEXT("version"),1);
 Binding->SetNumberField(TEXT("objId"),ObjId); Binding->SetStringField(TEXT("level"),Level);
 Binding->SetStringField(TEXT("enumType"),Kind); Binding->SetStringField(TEXT("member"),Member);
 Binding->SetStringField(TEXT("previewItem"),Preview);
 Binding->SetArrayField(TEXT("attachments"),Attachments);
 Binding->SetStringField(TEXT("blockId"),TEXT("bf6-loot-")+FString::FromInt(ObjId));
 Binding->SetStringField(TEXT("card"),TEXT("unreal/ui/")+Stem+TEXT(".design.json"));
 // A card is a reusable view of the item. The mode owns its trigger, price,
 // progression and interaction; opening the design never installs gameplay.
 Binding->SetStringField(TEXT("cardWidget"),Stem);
 Binding->SetStringField(TEXT("cardTitleWidget"),Stem+TEXT("_title"));
 Binding->SetStringField(TEXT("cardActionWidget"),Stem+TEXT("_action"));
 if(Action==TEXT("blocks"))
 {
  if(!Write(BindingPath,Json(Binding),OutWhy)) return false;
  BF6Blocks::ApplyLootBinding(Json(Binding)); OutWhy=Attachments.IsEmpty()?TEXT("Opening the linked spawn rule. It spawns once at game start; its event and conditions remain editable."):TEXT("Opening the base-item spawn rule. SpawnLoot cannot carry attachments. Use the Script pickup helper to remove the pickup and grant its configured package; the card exports its attachments to blocks."); return true;
 }
 if(Action==TEXT("script"))
 {
  const FString Dir=BF6Script::ProjectDirForSave(Level,Save);
  if(!BF6Script::OpenProjectAt(Dir)) { OutWhy=TEXT("Create or open this map's TypeScript project first."); return false; }
  const FString Rel=TEXT("src/generated/")+Stem+TEXT(".ts"), Path=FPaths::Combine(Dir,Rel);
  FString Text=FString::Printf(TEXT("// Generated item binding for %s. Import into your mode's event handlers.\n// The same item can feed proximity displays, buy stations or Gunmaster logic.\n// UI creation, player visibility, purchase validation and progression belong\n// to your mode. Importing this helper does not spawn loot or show a card.\nexport const loot%d = {\n  objId: %d,\n  item: mod.%s.%s,\n  cardName: \"%s\",\n  titleName: \"%s_title\",\n  actionName: \"%s_action\",\n};\n\nexport function spawnLoot%d(): void {\n  mod.SpawnLoot(mod.GetLootSpawner(loot%d.objId), loot%d.item);\n}\n"),*Level,ObjId,ObjId,*Kind,*Member,*Stem,*Stem,*Stem,ObjId,ObjId,ObjId);
  if(Kind==TEXT("Weapons"))
  {
   const FString Resources=FPaths::Combine(ToolPluginDir(),TEXT("Resources/loot"));
   FString Runtime;
   if(!FFileHelper::LoadFileToString(Text,*FPaths::Combine(Resources,TEXT("weapon-helper.ts.txt")))
    ||!FFileHelper::LoadFileToString(Runtime,*FPaths::Combine(Resources,TEXT("pickup-runtime.ts"))))
   { OutWhy=TEXT("The loot templates are missing. Repair the SDK installation."); return false; }
   Text.ReplaceInline(TEXT("$LEVEL"),*Level); Text.ReplaceInline(TEXT("$ID"),*FString::FromInt(ObjId));
   Text.ReplaceInline(TEXT("$WEAPON"),*Member); Text.ReplaceInline(TEXT("$ATTACHMENTS"),*FString::Join(AttachmentCode,TEXT(", ")));
   Text.ReplaceInline(TEXT("$STEM"),*Stem);
   const FString RuntimePath=FPaths::Combine(Dir,TEXT("src/generated/bf6-loot-runtime.ts"));
   FString Existing,Owned;
   if(FFileHelper::LoadFileToString(Existing,*RuntimePath)&&Existing!=Runtime)
   {
    FFileHelper::LoadFileToString(Owned,*(RuntimePath+TEXT(".owned")));
    if(Existing!=Owned) { OutWhy=TEXT("The shared loot runtime has custom edits. Preserve or move that file before regenerating helpers."); return false; }
   }
   if(!Write(RuntimePath,Runtime,OutWhy)||!Write(RuntimePath+TEXT(".owned"),Runtime,OutWhy)) return false;
  }
  FString Previous;
  if(FFileHelper::LoadFileToString(Previous,*Path)&&Previous!=Text)
  {
   FString Owned; FFileHelper::LoadFileToString(Owned,*(Path+TEXT(".owned")));
   if(Owned!=Previous) { OutWhy=TEXT("This loot helper has custom edits. Opening it without replacing those edits."); BF6Script::ShowFileFirst(Rel); BF6Script::Open(); return true; }
  }
  if(!Write(Path,Text,OutWhy)||!Write(Path+TEXT(".owned"),Text,OutWhy)||!Write(BindingPath,Json(Binding),OutWhy)) return false;
  BF6Script::ShowFileFirst(Rel); BF6Script::Open(); OutWhy=TEXT("Loot helper opened. Call spawn from your chosen event. For custom attachments, connect its pickup hook to your inventory tracker or arm its guarded watcher from your interaction."); return true;
 }
 if(Action==TEXT("card"))
 {
  const FString Path=FPaths::Combine(BF6Project::UiDesignDir(Save),Stem+TEXT(".design.json"));
  // Preserve drafts written before the UI builder's normal suffix was used.
  const FString Legacy=FPaths::Combine(BF6Project::UiDesignDir(Save),Stem+TEXT(".json"));
  if(!IFileManager::Get().FileExists(*Path)&&IFileManager::Get().FileExists(*Legacy))
  {
   FString Existing;
   if(!FFileHelper::LoadFileToString(Existing,*Legacy)||!Write(Path,Existing,OutWhy)) return false;
  }
  if(!IFileManager::Get().FileExists(*Path))
  {
   FString Template;
   const FString TemplatePath=FPaths::Combine(ToolPluginDir(),TEXT("Resources/loot/weapon-card.design.json"));
   if(!FFileHelper::LoadFileToString(Template,*TemplatePath)) { OutWhy=TEXT("The weapon card template is missing. Repair the SDK installation."); return false; }
   Template.ReplaceInline(TEXT("$LOOT_KEY"),*Stem);
   Template.ReplaceInline(TEXT("$LOOT_TITLE"),*Member.Replace(TEXT("_"),TEXT(" ")));
   TSharedPtr<FJsonObject> Card;
   if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Template),Card)||!Card) { OutWhy=TEXT("The weapon card template is invalid."); return false; }
   if(!Write(Path,Json(Card.ToSharedRef()),OutWhy)) return false;
  }
  // Refresh only the bound image's item fields; preserve all layout and text edits.
  FString CardText; TSharedPtr<FJsonObject> Card;
  if(!FFileHelper::LoadFileToString(CardText,*Path)||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(CardText),Card)||!Card)
  { OutWhy=TEXT("The existing card is not valid JSON. Repair it before updating the preset."); return false; }
  const TArray<TSharedPtr<FJsonValue>>* Roots=nullptr;
  if(!Card->TryGetArrayField(TEXT("widgets"),Roots)||Roots->IsEmpty()) { OutWhy=TEXT("The card has no root widget. Add a container in UI Builder first."); return false; }
  const FString ImageId=Stem+TEXT("_weapon");
  TSharedPtr<FJsonObject> Image;
  TArray<TSharedPtr<FJsonValue>> Pending=*Roots;
  while(!Pending.IsEmpty())
  {
   const auto O=Pending.Pop(EAllowShrinking::No)->AsObject(); if(!O) continue;
   FString WidgetId; O->TryGetStringField(TEXT("id"),WidgetId);
   if(WidgetId==ImageId) { Image=O; break; }
   const TArray<TSharedPtr<FJsonValue>>* Children=nullptr; if(O->TryGetArrayField(TEXT("children"),Children)) Pending.Append(*Children);
  }
  if(Kind==TEXT("Weapons")||Kind==TEXT("Gadgets"))
  {
   if(Image)
   {
    FString Type; Image->TryGetStringField(TEXT("type"),Type);
    if(Type!=TEXT("WeaponImage")&&Type!=TEXT("GadgetImage")) { OutWhy=TEXT("The bound equipment image was replaced with another widget type. Rename that widget to generate a new image."); return false; }
   }
   else
   {
    const auto Root=(*Roots)[0]->AsObject(); FString RootType;
    if(!Root||!Root->TryGetStringField(TEXT("type"),RootType)||RootType!=TEXT("Container")) { OutWhy=TEXT("The weapon card needs a root container."); return false; }
    Image=MakeShared<FJsonObject>(); Image->SetStringField(TEXT("id"),ImageId); Image->SetStringField(TEXT("name"),ImageId);
    Image->SetStringField(TEXT("type"),TEXT("WeaponImage")); Image->SetStringField(TEXT("anchor"),TEXT("Center"));
    Image->SetArrayField(TEXT("size"),{MakeShared<FJsonValueNumber>(336),MakeShared<FJsonValueNumber>(120)});
    const TArray<TSharedPtr<FJsonValue>>* Children=nullptr; TArray<TSharedPtr<FJsonValue>> Kids;
    if(Root->TryGetArrayField(TEXT("children"),Children)) Kids=*Children;
    Kids.Add(MakeShared<FJsonValueObject>(Image)); Root->SetArrayField(TEXT("children"),Kids);
    const TArray<TSharedPtr<FJsonValue>>* Size=nullptr;
    if(Root->TryGetArrayField(TEXT("size"),Size)&&Size->Num()==2&&(*Size)[0]->AsNumber()==360&&(*Size)[1]->AsNumber()==144)
     Root->SetArrayField(TEXT("size"),{MakeShared<FJsonValueNumber>(360),MakeShared<FJsonValueNumber>(280)});
   }
   Image->SetStringField(TEXT("type"),Kind==TEXT("Weapons")?TEXT("WeaponImage"):TEXT("GadgetImage"));
   bool HiddenByBinding=false;
   if(Image->TryGetBoolField(TEXT("hiddenByLootBinding"),HiddenByBinding)&&HiddenByBinding)
    Image->SetBoolField(TEXT("visible"),true);
   Image->RemoveField(TEXT("hiddenByLootBinding"));
   if(Kind==TEXT("Weapons"))
   {
    Image->SetStringField(TEXT("weapon"),Member); Image->SetArrayField(TEXT("attachments"),Attachments);
    Image->RemoveField(TEXT("gadget"));
   }
   else
   {
    Image->SetStringField(TEXT("gadget"),Member);
    Image->RemoveField(TEXT("weapon")); Image->RemoveField(TEXT("attachments"));
   }
  }
  else if(Image)
  {
   bool Visible=true; Image->TryGetBoolField(TEXT("visible"),Visible);
   if(Visible) Image->SetBoolField(TEXT("hiddenByLootBinding"),true);
   Image->SetBoolField(TEXT("visible"),false);
  }
  if(!Write(Path,Json(Card.ToSharedRef()),OutWhy)||!Write(BindingPath,Json(Binding),OutWhy)) return false;
  BF6Project::NoteArtefactWritten(Save,TEXT("unreal/ui/")+Stem+TEXT(".design.json"),TEXT("user"));
  if(!BF6UiBuilder::LoadFile(Path)) { OutWhy=TEXT("The card could not be opened."); return false; }
  BF6EditorOverlay::Show(BF6EditorOverlay::EEditor::Ui);
  OutWhy=TEXT("Reusable weapon card opened. Edit or remove its action button for your mode; connect display and interaction through the exported blocks or TypeScript."); return true;
 }
 OutWhy=TEXT("Unknown loot binding action."); return false;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBF6LootBindingWriteTest, "BF6.Loot.BindingFileWrite",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBF6LootBindingWriteTest::RunTest(const FString&)
{
 const FString Path=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("Automation"),FGuid::NewGuid().ToString()+TEXT(".loot.json"));
 FString Why,Actual;
 TestTrue(TEXT("Creating a binding reports success"),BF6LootBindingDetail::Write(Path,TEXT("{\"item\":1}"),Why));
 TestTrue(TEXT("Created binding is readable"),FFileHelper::LoadFileToString(Actual,*Path));
 TestEqual(TEXT("Created contents"),Actual,FString(TEXT("{\"item\":1}")));
 TestTrue(TEXT("Updating a binding reports success"),BF6LootBindingDetail::Write(Path,TEXT("{\"item\":2}"),Why));
 FFileHelper::LoadFileToString(Actual,*Path);
 TestEqual(TEXT("Updated contents"),Actual,FString(TEXT("{\"item\":2}")));
 TestFalse(TEXT("Atomic write leaves no pending file"),IFileManager::Get().FileExists(*(Path+TEXT(".pending"))));
 // A child of a regular file cannot be created. Failure must not claim that
 // the card opened, or alter the existing binding.
 TestFalse(TEXT("Unwritable destination reports failure"),BF6LootBindingDetail::Write(Path/TEXT("child.json"),TEXT("{}"),Why));
 FFileHelper::LoadFileToString(Actual,*Path);
 TestEqual(TEXT("Failure preserves existing contents"),Actual,FString(TEXT("{\"item\":2}")));
 IFileManager::Get().Delete(*Path); IFileManager::Get().Delete(*(Path+TEXT(".pending")));
 return true;
}
#endif
