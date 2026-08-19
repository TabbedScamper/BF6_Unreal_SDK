// Map manifest, captured from the REAL Portal choose-maps page (the codename,
// display name, image, size class, and paid flag all come from EA's own DOM:
// data-testid="add-<code>:<size>", #icon-map-size-*, #icon-price-tag).
// Thumbnails are the official tiles, saved as data/maps/<code>.jpg.
// Paid = shows the price tag on the site (requires the full game); the free
// RedSec maps and Portal Sandbox carry no tag.
#pragma once
struct FBF6MapCard { const TCHAR* Code; const TCHAR* Name; const TCHAR* Png; TCHAR Size; bool bPaid; };
static const FBF6MapCard GBF6MapCards[] = {
  { TEXT("MP_Abbasid"), TEXT("Siege of Cairo"), TEXT("MP_Abbasid.jpg"), TEXT('M'), true },
  { TEXT("MP_Aftermath"), TEXT("Empire State"), TEXT("MP_Aftermath.jpg"), TEXT('S'), true },
  { TEXT("MP_Aftermath_Portal"), TEXT("Bellum1988's Operation Metro"), TEXT("MP_Aftermath_Portal.jpg"), TEXT('S'), true },
  { TEXT("MP_Badlands"), TEXT("Blackwell Fields"), TEXT("MP_Badlands.jpg"), TEXT('L'), true },
  { TEXT("MP_Battery"), TEXT("Iberian Offensive"), TEXT("MP_Battery.jpg"), TEXT('M'), true },
  { TEXT("MP_Capstone"), TEXT("Liberation Peak"), TEXT("MP_Capstone.jpg"), TEXT('M'), true },
  { TEXT("MP_Contaminated"), TEXT("Contaminated"), TEXT("MP_Contaminated.jpg"), TEXT('M'), true },
  { TEXT("MP_Dumbo"), TEXT("Manhattan Bridge"), TEXT("MP_Dumbo.jpg"), TEXT('M'), true },
  { TEXT("MP_Eastwood"), TEXT("Eastwood"), TEXT("MP_Eastwood.jpg"), TEXT('M'), true },
  { TEXT("MP_FireStorm"), TEXT("Operation Firestorm"), TEXT("MP_FireStorm.jpg"), TEXT('L'), true },
  { TEXT("MP_GolmudRailway"), TEXT("Golmud Railway"), TEXT("MP_GolmudRailway.jpg"), TEXT('L'), true },
  { TEXT("MP_Granite_ClubHouse_Portal"), TEXT("Golf Course"), TEXT("MP_Granite_ClubHouse_Portal.jpg"), TEXT('M'), false },
  { TEXT("MP_Granite_MainStreet_Portal"), TEXT("Downtown"), TEXT("MP_Granite_MainStreet_Portal.jpg"), TEXT('M'), false },
  { TEXT("MP_Granite_Marina_Portal"), TEXT("Marina"), TEXT("MP_Granite_Marina_Portal.jpg"), TEXT('M'), false },
  { TEXT("MP_Granite_MilitaryRnD_Portal"), TEXT("Area 22B"), TEXT("MP_Granite_MilitaryRnD_Portal.jpg"), TEXT('M'), false },
  { TEXT("MP_Granite_MilitaryStorage_Portal"), TEXT("Redline Storage"), TEXT("MP_Granite_MilitaryStorage_Portal.jpg"), TEXT('M'), false },
  { TEXT("MP_Granite_TechCampus_Portal"), TEXT("Defense Nexus"), TEXT("MP_Granite_TechCampus_Portal.jpg"), TEXT('M'), false },
  { TEXT("MP_Granite_Underground_Portal"), TEXT("Complex 3"), TEXT("MP_Granite_Underground_Portal.jpg"), TEXT('S'), false },
  { TEXT("MP_Isolated"), TEXT("Tsuru Reef"), TEXT("MP_Isolated.jpg"), TEXT('L'), true },
  { TEXT("MP_Limestone"), TEXT("Saints Quarter"), TEXT("MP_Limestone.jpg"), TEXT('S'), true },
  { TEXT("MP_Outskirts"), TEXT("New Sobek City"), TEXT("MP_Outskirts.jpg"), TEXT('M'), true },
  { TEXT("MP_Plaza"), TEXT("Cairo Bazaar"), TEXT("MP_Plaza.jpg"), TEXT('S'), true },
  { TEXT("MP_Portal_Sand"), TEXT("Portal Sandbox"), TEXT("MP_Portal_Sand.jpg"), TEXT('L'), false },
  { TEXT("MP_Subsurface"), TEXT("Hagental Base"), TEXT("MP_Subsurface.jpg"), TEXT('M'), true },
  { TEXT("MP_Tungsten"), TEXT("Mirak Valley"), TEXT("MP_Tungsten.jpg"), TEXT('L'), true },
};
static const int GBF6MapCardCount = 25;
