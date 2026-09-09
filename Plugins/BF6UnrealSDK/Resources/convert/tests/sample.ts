// Hand written 10 rule sample covering every construct the intermediate model
// supports. Used by tests\run.js case (d) to prove TypeScript -> blocks -> TypeScript
// reaches a fixed point.

export const ScoreGlobalVar = mod.GlobalVariable(0);
export const RoundActiveGlobalVar = mod.GlobalVariable(1);
export const SpawnersGlobalVar = mod.GlobalVariable(2);
export const IteratorGlobalVar = mod.GlobalVariable(3);
export const KillsPlayerVar = 0;
export const StatePlayerVar = 1;
export const TicketsTeamVar = 0;

// portal:subroutine {"name":"AwardPoints","x":4000,"y":100,"params":[{"name":"Target","type":"Player"},{"name":"Amount","type":"Number"}]}
export function AwardPoints(Target: mod.Player, Amount: number): void {
    mod.SetVariable(mod.ObjectVariable(Target, KillsPlayerVar), mod.Add(mod.GetVariable(mod.ObjectVariable(Target, KillsPlayerVar)), Amount));
    mod.SetVariable(ScoreGlobalVar, mod.Add(mod.GetVariable(ScoreGlobalVar), Amount));
}

// portal:subroutine {"name":"AnnounceRound","x":4000,"y":600,"params":[]}
export async function AnnounceRound(): Promise<void> {
    mod.DisplayNotificationMessage(mod.Message("Round starting"));
    await mod.Wait(3);
    AwardPoints(mod.GetPlayer(0), 1);
}

// portal:rule {"name":"Setup","event":"Ongoing","objectType":"Global","index":0}
function OngoingGlobal_Setup_Condition(eventInfo: any): boolean {
    return true;
}

function OngoingGlobal_Setup_Action(eventInfo: any): void {
    mod.SetVariable(RoundActiveGlobalVar, true);
    mod.SetVariable(ScoreGlobalVar, 0);
    mod.SetVariable(SpawnersGlobalVar, mod.EmptyArray());
    mod.SetGameModeTimeLimit(900);
    mod.SetUIWidgetBgFill(mod.FindUIWidgetWithName("hud"), mod.UIBgFill.Blur);
    AnnounceRound();
}

export function OngoingGlobal_Setup(conditionState: any, eventInfo: any): void {
    const newState = OngoingGlobal_Setup_Condition(eventInfo);
    if (!conditionState.update(newState)) {
        return;
    }
    OngoingGlobal_Setup_Action(eventInfo);
}

// portal:rule {"name":"Low Health Warning","event":"Ongoing","objectType":"Player","index":1}
function OngoingPlayer_Low_Health_Warning_Condition(eventInfo: any): boolean {
    return mod.And(mod.GetVariable(RoundActiveGlobalVar), mod.LessThan(mod.GetSoldierState(eventInfo.eventPlayer, mod.SoldierStateNumber.Health), 20));
}

function OngoingPlayer_Low_Health_Warning_Action(eventInfo: any): void {
    mod.DisplayNotificationMessage(mod.Message("Low health"), eventInfo.eventPlayer);
}

export function OngoingPlayer_Low_Health_Warning(conditionState: any, eventInfo: any): void {
    const newState = OngoingPlayer_Low_Health_Warning_Condition(eventInfo);
    if (!conditionState.update(newState)) {
        return;
    }
    OngoingPlayer_Low_Health_Warning_Action(eventInfo);
}

// portal:rule {"name":"Ticket Drain","event":"Ongoing","objectType":"Team","index":2}
function OngoingTeam_Ticket_Drain_Condition(eventInfo: any): boolean {
    return mod.GreaterThan(mod.GetVariable(mod.ObjectVariable(eventInfo.eventTeam, TicketsTeamVar)), 0);
}

function OngoingTeam_Ticket_Drain_Action(eventInfo: any): void {
    mod.SetVariable(mod.ObjectVariable(eventInfo.eventTeam, TicketsTeamVar), mod.Subtract(mod.GetVariable(mod.ObjectVariable(eventInfo.eventTeam, TicketsTeamVar)), 1));
}

export function OngoingTeam_Ticket_Drain(conditionState: any, eventInfo: any): void {
    const newState = OngoingTeam_Ticket_Drain_Condition(eventInfo);
    if (!conditionState.update(newState)) {
        return;
    }
    OngoingTeam_Ticket_Drain_Action(eventInfo);
}

// portal:rule {"name":"Prepare Objectives","event":"OnGameModeStarted","objectType":"Global","index":3}
function OnGameModeStarted_Prepare_Objectives_Condition(eventInfo: any): boolean {
    return true;
}

async function OnGameModeStarted_Prepare_Objectives_Action(eventInfo: any): Promise<void> {
    for (let IteratorVar = 0; IteratorVar < mod.CountOf(mod.AllCapturePoints()); IteratorVar += 1) {
        mod.SetVariable(IteratorGlobalVar, IteratorVar);
        mod.EnableGameModeObjective(mod.ValueInArray(mod.AllCapturePoints(), mod.GetVariable(IteratorGlobalVar)), true);
    }
    await mod.Wait(1);
    mod.EnableAllPlayerDeploy(true);
}

export function OnGameModeStarted_Prepare_Objectives(conditionState: any, eventInfo: any): void {
    const newState = OnGameModeStarted_Prepare_Objectives_Condition(eventInfo);
    if (!conditionState.update(newState)) {
        return;
    }
    void OnGameModeStarted_Prepare_Objectives_Action(eventInfo);
}

// portal:rule {"name":"Loadout","event":"OnPlayerDeployed","objectType":"Global","index":4}
function OnPlayerDeployed_Loadout_Condition(eventInfo: any): boolean {
    return mod.IsPlayerValid(eventInfo.eventPlayer);
}

function OnPlayerDeployed_Loadout_Action(eventInfo: any): void {
    if (mod.Equals(mod.GetSoldierState(eventInfo.eventPlayer, mod.SoldierStateBool.IsAISoldier), true)) {
        mod.AddEquipment(eventInfo.eventPlayer, mod.Gadgets.Gadget_Defibrillator);
    } else if (mod.Equals(mod.GetTeamId(mod.GetTeam(1)), 1)) {
        mod.AddEquipment(eventInfo.eventPlayer, mod.Gadgets.Gadget_MedicalCrate);
    } else {
        mod.AddEquipment(eventInfo.eventPlayer, mod.Gadgets.Gadget_AmmoCrate);
    }
}

export function OnPlayerDeployed_Loadout(conditionState: any, eventInfo: any): void {
    const newState = OnPlayerDeployed_Loadout_Condition(eventInfo);
    if (!conditionState.update(newState)) {
        return;
    }
    OnPlayerDeployed_Loadout_Action(eventInfo);
}

// portal:rule {"name":"Death Watch","event":"OnPlayerDied","objectType":"Global","index":5}
function OnPlayerDied_Death_Watch_Condition(eventInfo: any): boolean {
    return true;
}

async function OnPlayerDied_Death_Watch_Action(eventInfo: any): Promise<void> {
    while (mod.Not(mod.IsPlayerValid(eventInfo.eventPlayer))) {
        await mod.Wait(0.5);
    }
    AwardPoints(eventInfo.eventOtherPlayer, 10);
}

export function OnPlayerDied_Death_Watch(conditionState: any, eventInfo: any): void {
    const newState = OnPlayerDied_Death_Watch_Condition(eventInfo);
    if (!conditionState.update(newState)) {
        return;
    }
    void OnPlayerDied_Death_Watch_Action(eventInfo);
}

// portal:rule {"name":"Capture Reward","event":"OnCapturePointCaptured","objectType":"Global","index":6}
function OnCapturePointCaptured_Capture_Reward_Condition(eventInfo: any): boolean {
    return mod.Equals(mod.GetCurrentOwnerTeam(eventInfo.eventCapturePoint), mod.GetTeam(1));
}

function OnCapturePointCaptured_Capture_Reward_Action(eventInfo: any): void {
    mod.SetVariable(mod.ObjectVariable(mod.GetTeam(1), TicketsTeamVar), mod.Add(mod.GetVariable(mod.ObjectVariable(mod.GetTeam(1), TicketsTeamVar)), 50));
    mod.PlaySound(mod.GetVariable(ScoreGlobalVar), 0.8);
}

export function OnCapturePointCaptured_Capture_Reward(conditionState: any, eventInfo: any): void {
    const newState = OnCapturePointCaptured_Capture_Reward_Condition(eventInfo);
    if (!conditionState.update(newState)) {
        return;
    }
    OnCapturePointCaptured_Capture_Reward_Action(eventInfo);
}

// portal:rule {"name":"Join Setup","event":"OnPlayerJoinGame","objectType":"Global","index":7}
function OnPlayerJoinGame_Join_Setup_Condition(eventInfo: any): boolean {
    return true;
}

function OnPlayerJoinGame_Join_Setup_Action(eventInfo: any): void {
    mod.SetVariable(mod.ObjectVariable(eventInfo.eventPlayer, KillsPlayerVar), 0);
    mod.SetVariable(mod.ObjectVariable(eventInfo.eventPlayer, StatePlayerVar), "ready");
}

export function OnPlayerJoinGame_Join_Setup(conditionState: any, eventInfo: any): void {
    const newState = OnPlayerJoinGame_Join_Setup_Condition(eventInfo);
    if (!conditionState.update(newState)) {
        return;
    }
    OnPlayerJoinGame_Join_Setup_Action(eventInfo);
}

// portal:rule {"name":"Track Spawner","event":"OnVehicleSpawned","objectType":"Global","index":8}
function OnVehicleSpawned_Track_Spawner_Condition(eventInfo: any): boolean {
    return rt.IsTrueForAny(mod.AllVehicles(), (currentArrayElement: mod.Any) => mod.Equals(currentArrayElement, eventInfo.eventVehicle));
}

function OnVehicleSpawned_Track_Spawner_Action(eventInfo: any): void {
    mod.SetVariable(SpawnersGlobalVar, rt.FilteredArray(mod.GetVariable(SpawnersGlobalVar), (currentArrayElement: mod.Any) => mod.Not(mod.Equals(currentArrayElement, eventInfo.eventVehicle))));
    mod.SetVariable(SpawnersGlobalVar, mod.AppendToArray(mod.GetVariable(SpawnersGlobalVar), eventInfo.eventVehicle));
}

export function OnVehicleSpawned_Track_Spawner(conditionState: any, eventInfo: any): void {
    const newState = OnVehicleSpawned_Track_Spawner_Condition(eventInfo);
    if (!conditionState.update(newState)) {
        return;
    }
    OnVehicleSpawned_Track_Spawner_Action(eventInfo);
}

// portal:rule {"name":"Interact Slot","event":"OnPlayerInteract","objectType":"Global","index":9}
function OnPlayerInteract_Interact_Slot_Condition(eventInfo: any): boolean {
    return true;
}

function OnPlayerInteract_Interact_Slot_Action(eventInfo: any): void {
    for (let IteratorVar = 0; IteratorVar < mod.CountOf(mod.GetVariable(SpawnersGlobalVar)); IteratorVar += 1) {
        mod.SetVariable(IteratorGlobalVar, IteratorVar);
        if (mod.Equals(mod.ValueInArray(mod.GetVariable(SpawnersGlobalVar), mod.GetVariable(IteratorGlobalVar)), eventInfo.eventInteractPoint)) {
            break;
        }
        mod.SetVariableAtIndex(SpawnersGlobalVar, mod.GetVariable(IteratorGlobalVar), eventInfo.eventPlayer);
    }
}

export function OnPlayerInteract_Interact_Slot(conditionState: any, eventInfo: any): void {
    const newState = OnPlayerInteract_Interact_Slot_Condition(eventInfo);
    if (!conditionState.update(newState)) {
        return;
    }
    OnPlayerInteract_Interact_Slot_Action(eventInfo);
}

export function OngoingGlobal(): void {
    const eventInfo: any = {};
    OngoingGlobal_Setup(rt.getGlobalCondition(0), eventInfo);
}

export function OngoingPlayer(eventPlayer: mod.Player): void {
    const eventInfo: any = { eventPlayer: eventPlayer };
    OngoingPlayer_Low_Health_Warning(rt.getObjectCondition(eventPlayer, 1), eventInfo);
}

export function OngoingTeam(eventTeam: mod.Team): void {
    const eventInfo: any = { eventTeam: eventTeam };
    OngoingTeam_Ticket_Drain(rt.getObjectCondition(eventTeam, 2), eventInfo);
}

export function OnGameModeStarted(): void {
    const eventInfo: any = {};
    OnGameModeStarted_Prepare_Objectives(rt.getEventCondition(), eventInfo);
}

export function OnPlayerDeployed(eventPlayer: mod.Player): void {
    const eventInfo: any = { eventPlayer: eventPlayer };
    OnPlayerDeployed_Loadout(rt.getEventCondition(), eventInfo);
}

export function OnPlayerDied(eventPlayer: mod.Player, eventOtherPlayer: mod.Player, eventDeathType: mod.DeathType, eventWeaponUnlock: mod.WeaponUnlock): void {
    const eventInfo: any = { eventPlayer: eventPlayer, eventOtherPlayer: eventOtherPlayer, eventDeathType: eventDeathType, eventWeaponUnlock: eventWeaponUnlock };
    OnPlayerDied_Death_Watch(rt.getEventCondition(), eventInfo);
}

export function OnCapturePointCaptured(eventCapturePoint: mod.CapturePoint): void {
    const eventInfo: any = { eventCapturePoint: eventCapturePoint };
    OnCapturePointCaptured_Capture_Reward(rt.getEventCondition(), eventInfo);
}

export function OnPlayerJoinGame(eventPlayer: mod.Player): void {
    const eventInfo: any = { eventPlayer: eventPlayer };
    OnPlayerJoinGame_Join_Setup(rt.getEventCondition(), eventInfo);
}

export function OnVehicleSpawned(eventVehicle: mod.Vehicle): void {
    const eventInfo: any = { eventVehicle: eventVehicle };
    OnVehicleSpawned_Track_Spawner(rt.getEventCondition(), eventInfo);
}

export function OnPlayerInteract(eventPlayer: mod.Player, eventInteractPoint: mod.InteractPoint): void {
    const eventInfo: any = { eventPlayer: eventPlayer, eventInteractPoint: eventInteractPoint };
    OnPlayerInteract_Interact_Slot(rt.getEventCondition(), eventInfo);
}
