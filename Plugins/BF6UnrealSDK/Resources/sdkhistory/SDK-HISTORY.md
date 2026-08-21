# Portal SDK version history

Generated from the community SDK archive (hoard.bfportal.gg) by
tools/sdk_history/build_sdk_history.py. Manifest added/removed is the
signal; changed counts are mostly regeneration noise.

## 1.4.2.0  (released 2026-08-18)

- Archive: 3.33 GB, 66115 entries (+1594 added, 16 removed vs 1.4.1.0)
- New maps: MP_Isolated
- New level scenes: MP_Isolated
- New placeable types: 259 (ACUnitWindow_01, ACUnitWindow_01_E, AirControlTower_01_Isolated, AirControlTower_01_PropsA_Isolated, AraliaJapanese_01_M, AraliaJapanese_01_S, BR_EAS_Pagoda_01, BR_PlasterWall_01_B_C10, BR_WallResidential_01_256, BR_WallResidential_01_512, BR_WallResidential_01_End, BananaWi01_M_B, Banana_01_S, Banana_01_S_B, Banana_01_S_C, Banana_01_S_D, BarbedWire_01_512_CV90, BarrackCylindrical_Props_D_MP_Isolated, Barrack_02_RadarComProps_A_MP_Isolated, BeechJapanese_01_L, BeechJapanese_01_L_B, BeechJapanese_01_M, BeechJapanese_01_M_B, Bridge_Facade_02, CementBags_01_256x60_Grain, ClamshellCabanas_01, ConcreteCurb_Large_256, ConcreteFoundation_01_256x128, ConcreteFoundation_01_256x128_Pickup, ConcreteFoundation_01_256x256_C90, ... and 229 more)
- Placeable types removed: 3 (AutoCapture_Terrain, InteriorVolume, ShadowVolume)
- New script enums: RuntimeSpawn_Isolated
- New script functions: SetAllObjectivesUIEnabled, SetObjectiveUIEnabled
- Playable bounds added for MP_Isolated: {"size": [2554.653, 393.4019, 2554.653], "position": [-417.04296, 100.5, -4.695374]}
- Entity scripts changed: 9 (attachment.gd, experience_exporter_export.gd, portal_tools_dock.gd, RingOfFire.gd, HQ_PlayerSpawner.gd, VehicleResupplyStation.gd, VehicleSpawner.gd, CapturePoint.gd, VEH_Stationary_AutomaticAA.gd)

## 1.4.1.0  (released 2026-07-21)

- Archive: 3.16 GB, 64537 entries (+41 added, 125 removed vs 1.3.3.0)
- New placeable types: 2 (InteriorVolume, ShadowVolume)
- New script functions: GetPlayer, GetVehicle
- playable bounds catalog introduced (24 maps)
- Entity scripts changed: 9 (experience_exporter.gd, experience_exporter_export.gd, experience_exporter_success.gd, modexportinfo.gd, modpackage.gd, portal_plugin.gd, portal_tools_dock.gd, SpawnPoint.gd, VEH_Stationary_AutomaticAA.gd)

## 1.3.3.0  (released 2026-06-30)

- Archive: 3.96 GB, 64621 entries (+2742 added, 8 removed vs 1.3.2.0)
- New maps: MP_Aftermath_Portal, MP_Plaza
- New level scenes: MP_Aftermath_Portal, MP_Plaza
- New placeable types: 450 (AppleClusterCardBoardBox_01, AppleClusterMilkCrate_01, AppleClusterPlateMetalOrnate_01, ArtichokeClusterCrateWoodLight_01, AwningPlastic_01_256_Plaza, AwningPlastic_01_256_Plaza_B, AwningPlastic_01_512_Plaza, AwningPlastic_01_512_Plaza_B, BDN_Building_01, BDN_Building_02, BDN_Building_02_MP_Plaza, BDN_Building_02_MP_Plaza_Optim, BDN_Building_03, BDN_Building_03_MP_Plaza, BR_SoukFacade_01_1024x1152, BR_SoukFacade_01_1024x768, BR_SoukFacade_01_768x1152, BR_SoukFacade_01_768x768, BR_SoukFacade_01_768x768_CV90, BR_SoukStorefront_01_768, BR_SoukStorefront_01_768_CV90, BarricadeboardsWood_01_B_MVault, BarrierStoneBlock_01_H_PortalPlatform, BasketDressing_02, Basket_01, Basket_01_B, BeddingRural_01, Billboard_01_Plaza, BillboardsRoof_01, BillboardsRoof_02, ... and 420 more)
- Placeable types removed: 1 (Portal_NAF_BarrierStoneBlock_01_H_PortalPlatform)
- New script enums: BombState, GameModeTicker, MCOMArmType, RuntimeSpawn_Plaza, ScoreCriteria
- New script functions: ApplyAreaImpulseAndDamage, ApplyImpulse, ForceBombDrop, ForceBombReset, ForceBombSpawn, ForceBombUnspawn, GetBomb, GetPortalAverageFrameTime, GetServerAverageFrameTime, GiveBombToPlayer, IsUndefined, IsValid, OnBombDropped, OnBombPickedUp, OnBombStateChanged, OngoingBomb, SetBombDropFuseTime, SetBombTeam, SetBombWorldIconGlobalVisibility, SetFreeCameraCollisionForAll, SetFreeCameraCollisionForPlayer, SetGameModeCriteria, SetGameModeInitialScore, SetHUDTicker, SetMCOMArmType, SetThirdPersonCameraPositionForAll, SetThirdPersonCameraPositionForPlayer
- New script types: Bomb

## 1.3.2.0  (released 2026-06-09)

- Archive: 2.97 GB, 61887 entries (+2477 added, 3 removed vs 1.3.1.1)
- New maps: MP_GolmudRailway
- New level scenes: MP_GolmudRailway
- New placeable types: 414 (AnimalDungLarge_01, BR_HouseRuralLarge_01, BR_PorchRuralPillar_01_A_320, BR_VillageShackWall_01_Golmud, BR_VillageShack_01_B_Closed_Golmud, BR_VillageShack_01_B_Golmud, BR_WallCompoundPillar_02_352, BR_WallCompound_02_352x1024, BR_WallCompound_02_352x2048, BR_WallCompound_02_352x256, BR_WallCompound_02_352x512, BackdropMountains_01, Barn_01, Barn_01_Mirrored, Barn_01_Props, Barn_01_Props_Mirrored, Barrack_01_A_Props_A_Golmud, Barrack_01_Props_B_Golmud, BarrelOil_01, BarrierJerseyFence_02, Bathtub_02, BeamsConcrete_01, BeamsConcrete_01_A_01, BeamsConcrete_01_A_02, BeamsConcrete_01_A_Ridge, BeamsConcrete_01_B, BeamsConcrete_01_B_01, BeamsConcrete_01_B_02, BeamsConcrete_01_B_Ridge, BeamsConcrete_01_C, ... and 384 more)
- New script enums: GolmudTrainMoveCommands, GolmudTrainStopReason, GolmudTrainVariants, RuntimeSpawn_GolmudRailway
- New script functions: GetGolmudTrainLocation, GolmudTrainSendMoveCommand, OnGolmudTrainStopped, SendPortalLogToAdmin
- Script functions removed: EnableSpatialObject
- New script types: MapSpecificFeature
- Entity scripts changed: 10019 (portal_tools.gd, AftermathDebrisPileBrickPlaster_120_A_1.gd, AftermathDebrisPileBrickPlaster_210_A_1.gd, AftermathDebrisPileRedBrick_01_A.gd, AftermathDebrisPileRedBrick_01_B.gd, Aftermath_DebrisPileConcrete_Skew_210_A_1.gd, Art_Structure_01.gd, Art_Structure_Bench_Divider_01.gd, Awning_02_A.gd, Awning_02_B.gd, ...)

## 1.3.1.1  (released 2026-05-19)

- Archive: 2.82 GB, 59413 entries (+12 added, 1 removed vs 1.3.1.0)
- Entity scripts changed: 10018 (AftermathDebrisPileBrickPlaster_120_A_1.gd, AftermathDebrisPileBrickPlaster_210_A_1.gd, AftermathDebrisPileRedBrick_01_A.gd, AftermathDebrisPileRedBrick_01_B.gd, Aftermath_DebrisPileConcrete_Skew_210_A_1.gd, Art_Structure_01.gd, Art_Structure_Bench_Divider_01.gd, Awning_02_A.gd, Awning_02_B.gd, Awning_02_C.gd, ...)

## 1.3.1.0  (released 2026-05-12)

- Archive: 2.81 GB, 59402 entries (+89 added, 1 removed vs 1.2.3.0)
- New script consts: strings
- Entity scripts changed: 6 (portal_plugin.gd, portal_tools_dock.gd, PolygonVolume.gd, CombatArea.gd, StationaryEmplacementSpawner.gd, VehicleSpawner.gd)

## 1.2.3.0  (released 2026-04-14)

- Archive: 2.81 GB, 59314 entries (+4286 added, 6 removed vs 1.2.2.0)
- New maps: MP_Granite_Underground_Portal, MP_Subsurface
- New level scenes: MP_Granite_Underground_Portal, MP_Subsurface
- New placeable types: 716 (AftermathDebrisPileConcrete_Skew_120_C, AftermathDebrisRocks_210, AftermathJet_Skew_210, AirDuctDamaged_B_1024, AirDuctPipeBrace_01, AirDuctPipe_256_02, AirDuct_01_A_1024, AirDuct_01_C135, AirDuct_02_A_256_B, AirDuct_02_A_512_NBRK, AirDuct_02_A_Corner_NBRK, AirDuct_02_A_End_NBRK, AirDuct_02_C135, AirDuct_1024_C, AirDuct_256_B, AirplaneJAS39Body_B, AirplaneJAS39Cab, AirplaneJAS39Cover, AirplaneJAS39Frame_01_B, AirplaneJAS39Nose, AirplaneJAS39Panel, AirplaneJAS39PanelFrame_01, AirplaneJAS39PanelFrame_01_B, AirplaneJAS39TailWing, Area_01_Base, Area_02_Base, Area_02_SetDressing, Area_03_base, Area_04_Base, AsphaltChunks_01_B, ... and 686 more)
- Placeable types removed: 1 (SurroundingCombatArea)
- New script enums: ResupplyTypes, RuntimeSpawn_Granite_Underground, RuntimeSpawn_Subsurface, SpectatingGroup, VehicleCategories
- New script functions: AutoBalanceTeams, EventWeaponCompare, GetVL7Cloud, OnPlayerEnterVL7Cloud, OnPlayerExitVL7Cloud, OnPortalGadgetAimStart, OnPortalGadgetAimStop, OnPortalGadgetFireStart, OnPortalGadgetFireStop, OnPortalGadgetLaserToggle, Resupply, SetAllVehiclesAllowedInSurroundingArea, SetMCOMOwner, SetSpectatingFiltersForAll, SetSpectatingFiltersForPlayer, SetVL7CloudEffects, SetVehicleAllowedInSurroundingArea, SetVehicleCategoryAllowedInSurroundingArea
- New script types: VL7Cloud
- Entity scripts changed: 9303 (portal_tools_dock.gd, AftermathDebrisPileBrickPlaster_120_A_1.gd, AftermathDebrisPileBrickPlaster_210_A_1.gd, AftermathDebrisPileRedBrick_01_A.gd, AftermathDebrisPileRedBrick_01_B.gd, Aftermath_DebrisPileConcrete_Skew_210_A_1.gd, Art_Structure_01.gd, Art_Structure_Bench_Divider_01.gd, Awning_02_A.gd, Awning_02_B.gd, ...)

## 1.2.2.0  (released 2026-03-17)

- Archive: 2.51 GB, 55034 entries (+26 added, 0 removed vs 1.2.1.0)
- New placeable types: 2 (FixedCamera, VEH_AH6M)
- New script enums: AiInput, RuntimeSpawn_Contaminated
- New script functions: GetFixedCamera, GetTransformPosition, GetTransformRotation, JsAction, JsValue, SetAiInput, SetSoundAmplitude
- New script types: FixedCamera
- Entity scripts changed: 2 (PolygonVolumeGizmo.gd, VehicleSpawner.gd)

## 1.2.1.0  (released 2025-12-09)

- Archive: 2.52 GB, 55008 entries (+20797 added, 24946 removed vs 1.1.3.0)
- New maps: MP_Contaminated
- New level scenes: MP_Contaminated
- New placeable types: 564 (AftermathDebrisPileMetal_210_01, AftermathDebrisRocks_310, AftermathDebrisRocks_310_B, AirDuctPipeCap_01, AirDuctPipe_01, AirDuctPipe_01_C90, AirDuct_02_A_Joint, AircraftWreckage_Jas39_01_Body, AirplaneJAS39Body, AirplaneJAS39Cab_01_Contaminated, AirplaneJAS39Cloth_01, AirplaneJAS39Cockpit_01, AirplaneJAS39EnginePlugsBack_01, AirplaneJAS39EnginePlugsLeft_01, AirplaneJAS39EnginePlugsRight_01, AirplaneJAS39Frame_01, AirplaneJAS39Frame_01_Contaminated, AirplaneJAS39Frame_02, AirplaneJAS39FuelTank_01, AirplaneJAS39LeftWing, AirplaneJAS39_01, AirplaneJAS39_01_B, AirplaneJAS39_01_C, AirplaneJAS39_Repair_01, AlleyTrash_02_WinterEvent, AlleyTrash_06, Antenna_01_B, Antenna_02_B, BR_ConstructionSetWallCinderblockDoorway_A_01_512x384, BR_ConstructionSetWallCinderblock_A_01_512x384, ... and 534 more)
- Models removed: AAGun_01, ACModule_01, ACModule_01_VFX, ACModule_02, ACModule_02_VFX, ACModule_03, ACModule_03_Running, ACModule_03_animated, ACModule_04, ACUnitInterior_01, ACUnitWindow_01_A, ACUnitWindow_01_B, ACUnitWindow_01_C, ACUnitWindow_01_D, ACUnitWindow_logo_01, ACUnit_01, ACUnit_03, ACUnit_03_Running, ACUnit_03_animated, ACUnit_03_cover
- New script consts: stringkeys
- New script enums: AmmoTypes, ArmorTypes, Cameras, CustomNotificationSlots, Factions, Gadgets, InventorySlots, Maps, MoveSpeed, MusicEvents, MusicPackages, MusicParams, PlayerDamageTypes, PlayerDeathTypes, PlayerFilterTypes, RestrictedInputs, RuntimeSpawn_Abbasid, RuntimeSpawn_Aftermath, RuntimeSpawn_Badlands, RuntimeSpawn_Battery, RuntimeSpawn_Capstone, RuntimeSpawn_Common, RuntimeSpawn_Dumbo, RuntimeSpawn_Eastwood, RuntimeSpawn_FireStorm, RuntimeSpawn_Granite_Downtown, RuntimeSpawn_Granite_Marina, RuntimeSpawn_Granite_MilitaryRnD, RuntimeSpawn_Granite_MilitaryStorage, RuntimeSpawn_Granite_ResidentialNorth, RuntimeSpawn_Granite_TechCenter, RuntimeSpawn_Limestone, RuntimeSpawn_Outskirts, RuntimeSpawn_Sand, RuntimeSpawn_Tungsten, ScoreboardType, ScreenEffects, SoldierClass, SoldierEffects, SoldierStateBool
- New script functions: AIBattlefieldBehavior, AIDefendPositionBehavior, AIEnableShooting, AIEnableTargeting, AIForceFire, AIGadgetSettings, AIIdleBehavior, AILOSMoveToBehavior, AIMoveToBehavior, AIParachuteBehavior, AISetFocusPoint, AISetMoveSpeed, AISetStance, AISetTarget, AISetUnspawnOnDead, AIStartUsingGadget, AIStopUsingGadget, AIValidatedMoveToBehavior, AIWaypointIdleBehavior, AbsoluteValue, Add, AddAttachmentToWeaponPackage, AddEquipment, AddUIButton, AddUIContainer, AddUIGadgetImage, AddUIIcon, AddUIImage, AddUIText, AddUIWeaponImage, AllCapturePoints, AllPlayers, AllVehicles, And, AngleBetweenVectors, AngleDifference, AppendToArray, ArccosineInDegrees, ArccosineInRadians, ArcsineInDegrees
- New script types: Any, AreaTrigger, Array, CapturePoint, DamageType, DeathType, EmplacementSpawner, HQ, InteractPoint, LootSpawner, MCOM, Message, Object, Player, PortalEnum, RingOfFire, SFX, Sector, SoldierKits, SpatialObject, SpawnPoint, Spawner, Squad, Team, Transform, UIWidget, VFX, VO, Variable, Vector, Vehicle, VehicleSpawner, WaypointPath, WeaponPackage, WeaponUnlock, WorldIcon
- Entity scripts changed: 7 (check_memory.gd, create_table.gd, table_container.gd, portal_tools_dock.gd, OBBVolumeGizmo.gd, PolygonVolume.gd, SpawnPoint.gd)

## 1.1.3.0  (released 2025-12-09)

- Archive: 6.53 GB, 59157 entries (+4253 added, 8770 removed vs 1.1.2.0)
- New maps: MP_Granite_MilitaryRnD_Portal, MP_Granite_MilitaryStorage_Portal
- New level scenes: MP_Granite_MilitaryRnD_Portal, MP_Granite_MilitaryStorage_Portal
- New placeable types: 538 (Abra01_Chassis_B, Abra01_Turret_B, AirDuct_Bend_T_90_128, AirDuct_TShape, AirDuct_Vent, AmmoStack_01, Antenna_02, AsphaltRubblePile_01, BR_Factory_02, BR_PlasterWallSlope_02_256_B, BR_ServiceBuilding_01, BR_StorageRoof_01_A_2048, BR_StorageWallCornerSmall_01_512x384_B, BR_StorageWallLarge_01_B_1024x768, BR_StorageWallLarge_01_C_512x768, BR_StorageWallSmall_01_1024x256, BR_StorageWallSmall_01_1024x384, BR_StorageWallSmall_01_B_512x384, BR_StorageWallSmall_01_B_512x384_B, BR_StorageWallSmall_01_C_512x384, BR_StorageWallSmall_01_C_512x384_B, BarrackCylindricalDoorSmallFrame_01, BarrackCylindricalDoorSmall_01, BarrackCylindrical_Airstrip_01_LightingProps, BarrackCylindrical_Airstrip_01_Props_B, BarrackStair_01_B, Barrack_01_A_Props_F, Barrack_02_A_Props, Barrack_02_A_SecurityCheckPoint, Barrack_02_B, ... and 508 more)
- New models: 537 (Abra01_Chassis_B, Abra01_Turret_B, AirDuct_Bend_T_90_128, AirDuct_TShape, AirDuct_Vent, AmmoStack_01, Antenna_02, AsphaltRubblePile_01, BR_Factory_02, BR_PlasterWallSlope_02_256_B, BR_ServiceBuilding_01, BR_StorageRoof_01_A_2048, ...)
- Entity scripts changed: 17 (level_validator.gd, check_memory.gd, col_toggles.gd, create_table.gd, drag.gd, memory_plugin.gd, search.gd, plugin.gd, table_container.gd, portal_plugin.gd, ...)

## 1.1.2.0  (released 2025-11-18)

- Archive: 5.30 GB, 63674 entries (+17989 added, 2290 removed vs 1.1.1.0)
- New maps: MP_Eastwood, MP_Granite_MainStreet_Portal, MP_Granite_Marina_Portal, MP_Portal_Sand
- New level scenes: MP_Eastwood, MP_Granite_MainStreet_Portal, MP_Granite_Marina_Portal, MP_Portal_Sand
- New placeable types: 1091 (ATMMachine_01, AdirondackChair_01, AirDuct_Bend_90_128, AirDuct_Bend_90_128_B, AirDuct_End, AirDuct_End_Bend_Up, Airconditioner_01_B, AlleyTrash_03, AlleyTrash_04, AlleyTrash_05, AlleyTrash_07, AlleyTrash_08, AluminumGangway_01_1024, AshTray_01_VFX, AsphaltBrokenSmall_01, AsphaltBrokenThick_01_512x512_CullSonner, BR_CommunityEntrance_01, BR_CountryClubBreezeway_01, BR_CountryClubFacade_01, BR_CountryClubFacade_01_B, BR_CountryClubFront_01, BR_CountryClubLoggia_01, BR_PlasterPillar_02_144, BR_PlasterWall_02_B_C10, BR_PlasterWall_02_C10, BR_PlasterWall_02_C22, BR_ResidentialGarage_02, BR_RestaurantWallExterior_01, BR_RestaurantWallExterior_02, BR_RestaurantWall_Interior_02, ... and 1061 more)
- New models: 1045 (ATMMachine_01, AdirondackChair_01, AirDuct_Bend_90_128, AirDuct_Bend_90_128_B, AirDuct_End, AirDuct_End_Bend_Up, Airconditioner_01_B, AlleyTrash_03, AlleyTrash_04, AlleyTrash_05, AlleyTrash_07, AlleyTrash_08, ...)
- Entity scripts changed: 17 (level_validator.gd, check_memory.gd, col_toggles.gd, create_table.gd, drag.gd, memory_plugin.gd, search.gd, plugin.gd, table_container.gd, portal_plugin.gd, ...)

## 1.1.1.0  (released 2025-11-18)

- Archive: 2.94 GB, 47975 entries (+5682 added, 739 removed vs 1.0.1.0)
- New maps: MP_Badlands, MP_Granite_ClubHouse_Portal, MP_Granite_TechCampus_Portal
- New level scenes: MP_Badlands, MP_Granite_ClubHouse_Portal, MP_Granite_TechCampus_Portal
- New placeable types: 824 (AP_Planter_Grass_02, AP_Planter_PurpleFlowers_01, AP_PolyPlane_01, Abra01_Chassis, Abra01_Tracks, Abra01_Turret, AbraCoveredTarp, AftermathDebrisPileConcrete_Center_120_B, AftermathDebrisPileConcrete_Center_60_B, AftermathDebrisPileConcrete_Skew_120_B, AftermathDebrisPileConcrete_Skew_210_E, AftermathDebrisPileDrywall_Center_120_01, AftermathDebrisPileDrywall_Center_120_01_B, AftermathDebrisPileDrywall_Center_60_01, AftermathDebrisPileDrywall_Center_60_01_B, AftermathDebrisPileDrywall_Ramp_210_01, AftermathDebrisPileDrywall_Ramp_210_01_B, AluminumBench_01, AntennaMast_01, ArtExhibitBase_128x128x128_01, AsphaltChunks_01_Snow, AutoCapture_Terrain, BR_PlasterWall_02_B_C22, Backpack_01_B, Backpack_02, Backpack_03, Badlands_Flankbus, BagTarp_01, BarGantry_01, BarTableLong_01, ... and 794 more)
- Placeable types removed: 5 (Area04_Building_09, OutskirtsHouseMediumRoof_01, PatioMetalChair_01_B, SodaCan_01_A, SodaCan_01_B)
- New models: 664 (AP_Planter_Grass_02, AP_Planter_PurpleFlowers_01, AP_PolyPlane_01, Abra01_Chassis, Abra01_Tracks, Abra01_Turret, AbraCoveredTarp, AftermathDebrisPileConcrete_Center_120_B, AftermathDebrisPileConcrete_Center_60_B, AftermathDebrisPileConcrete_Skew_120_B, AftermathDebrisPileConcrete_Skew_210_E, AftermathDebrisPileDrywall_Center_120_01, ...)
- Models removed: Area04_Building_09, OutskirtsHouseMediumRoof_01, PatioMetalChair_01_B, SodaCan_01_A, SodaCan_01_B
- Entity scripts changed: 1 (VehicleSpawner.gd)
