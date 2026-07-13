-- MetroVoiceFX core: talks to the native `metrovoice` module (gmsv_metro_voicefx_*)
-- and decides which preset applies to which player. Edit metro_voicefx_config.lua,
-- not this file, to configure rules.

-- Load the native binary module (gmsv_metro_voicefx_<platform>.dll in lua/bin).
pcall(require, "metro_voicefx")

if not metrovoice then
	ErrorNoHalt("[MetroVoiceFX] Native module 'metrovoice' isn't loaded - voice effects are disabled.\n")
	ErrorNoHalt("[MetroVoiceFX] Make sure gmsv_metro_voicefx_<platform>.dll is in garrysmod/lua/bin/ and restart.\n")
	return
end

MetroVoice = MetroVoice or {}
MetroVoice.Config = MetroVoice.Config or {}

MetroVoice.Presets = {
	None         = metrovoice.EFF_NONE,
	Radio        = metrovoice.EFF_RADIO,
	Phone        = metrovoice.EFF_PHONE,
	Stormtrooper = metrovoice.EFF_STORMTROOPER,
	Combine      = metrovoice.EFF_COMBINE,
	PA           = metrovoice.EFF_PA,
	Muffled      = metrovoice.EFF_MUFFLED,
	Masked       = metrovoice.EFF_MASKED,
}

local rules = {
	players    = {}, -- [steamid64] = presetName
	usergroups = {}, -- [usergroup] = presetName
	teams      = {}, -- [team index] = presetName
	models     = {}, -- [lowercase model path] = presetName
}

local function ToSteamID64(id)
	id = tostring(id)
	if id:find("^STEAM_") then
		return util.SteamIDTo64(id)
	end
	return id
end

local function AssertPreset(preset)
	assert(MetroVoice.Presets[preset] ~= nil, "MetroVoiceFX: unknown preset '" .. tostring(preset) .. "'")
end

-- MetroVoice.Config:AddPlayerFX("Combine", "STEAM_0:1:7099", "76561197960279927")
function MetroVoice.Config:AddPlayerFX(preset, ...)
	AssertPreset(preset)
	for _, id in ipairs({ ... }) do
		rules.players[ToSteamID64(id)] = preset
	end
end

-- MetroVoice.Config:AddUsergroupFX("Combine", "combine-fx", "stalker-fx")
function MetroVoice.Config:AddUsergroupFX(preset, ...)
	AssertPreset(preset)
	for _, grp in ipairs({ ... }) do
		rules.usergroups[grp] = preset
	end
end

-- MetroVoice.Config:AddTeamFX("Combine", TEAM_STALKER, TEAM_BANDIT)
function MetroVoice.Config:AddTeamFX(preset, ...)
	AssertPreset(preset)
	for _, team in ipairs({ ... }) do
		rules.teams[team] = preset
	end
end

-- MetroVoice.Config:AddModelFX("Stormtrooper", "models/player/combine_soldier.mdl")
function MetroVoice.Config:AddModelFX(preset, ...)
	AssertPreset(preset)
	for _, mdl in ipairs({ ... }) do
		rules.models[string.lower(mdl)] = preset
	end
end

-- Priority: explicit SteamID override > playermodel > usergroup > team > none.
local function ResolvePreset(ply)
	if not IsValid(ply) then return "None" end

	local sid = ToSteamID64(ply:SteamID64())
	if rules.players[sid] then return rules.players[sid] end

	local mdl = string.lower(ply:GetModel() or "")
	if rules.models[mdl] then return rules.models[mdl] end

	local grp = ply:GetUserGroup()
	if rules.usergroups[grp] then return rules.usergroups[grp] end

	local team = ply:Team()
	if rules.teams[team] then return rules.teams[team] end

	return "None"
end

local appliedState = {} -- [userid] = presetName currently set natively
local vbActive = {}     -- [ply] = { VoiceBox fx names }; the manual VoiceBox API below

function MetroVoice.Refresh(ply)
	if not IsValid(ply) or not ply:IsPlayer() then return end
	if vbActive[ply] and #vbActive[ply] > 0 then return end -- manual VoiceBox fx overrides config rules

	local preset = ResolvePreset(ply)
	local uid = ply:UserID()

	if appliedState[uid] == preset then return end

	metrovoice.EnableEffect(uid, MetroVoice.Presets[preset])
	appliedState[uid] = preset
end

function MetroVoice.RefreshAll()
	for _, ply in ipairs(player.GetAll()) do
		MetroVoice.Refresh(ply)
	end
end

hook.Add("PlayerInitialSpawn", "MetroVoiceFX_Spawn", MetroVoice.Refresh)
hook.Add("PlayerSpawn", "MetroVoiceFX_Respawn", MetroVoice.Refresh)
hook.Add("OnPlayerChangedTeam", "MetroVoiceFX_Team", function(ply) MetroVoice.Refresh(ply) end)

hook.Add("PlayerDisconnected", "MetroVoiceFX_Cleanup", function(ply)
	local uid = ply:UserID()
	if appliedState[uid] then
		metrovoice.EnableEffect(uid, metrovoice.EFF_NONE)
		appliedState[uid] = nil
	end
end)

-- Fallback poll: catches playermodel swaps and anything a custom faction/job
-- system doesn't route through the hooks above. Call MetroVoice.Refresh(ply)
-- directly from your faction-switch code for instant updates instead of
-- waiting up to 3 seconds for this to catch it.
timer.Create("MetroVoiceFX_Poll", 3, 0, MetroVoice.RefreshAll)

-- Superadmin console command for quick testing without changing team/model.
-- metrovoice_test Combine
concommand.Add("metrovoice_test", function(ply, _, args)
	if not IsValid(ply) or not ply:IsSuperAdmin() then return end

	local preset = args[1]
	if not preset or not MetroVoice.Presets[preset] then
		local names = {}
		for name in pairs(MetroVoice.Presets) do names[#names + 1] = name end
		ply:PrintMessage(HUD_PRINTCONSOLE, "Usage: metrovoice_test <" .. table.concat(names, "|") .. ">")
		return
	end

	metrovoice.EnableEffect(ply:UserID(), MetroVoice.Presets[preset])
	appliedState[ply:UserID()] = preset
	ply:PrintMessage(HUD_PRINTCONSOLE, "MetroVoiceFX: applied '" .. preset .. "'. Say something in voice chat.")
end)

-- ============================================================================
-- VoiceBox FX compatibility layer: exposes the SAME Lua API as the original
-- VoiceBox FX (ply:AddVoiceFX etc.) so existing server/gamemode code keeps
-- working with no changes. VoiceBox fx names are mapped to our presets; our
-- engine plays one effect at a time, so stacked fx resolve to the most recently
-- added valid one. Manual API use overrides the config rules above.
-- ============================================================================
local FX_ALIAS = {
	Radio = "Radio", Phone = "Phone", Stormtrooper = "Stormtrooper", Combine = "Combine",
	PA = "PA", PASystem = "PA", PASystemLoud = "PA", ["Public Address System"] = "PA",
	Muffled = "Muffled", Masked = "Masked",
}

local function vbResolve(ply)
	local list = vbActive[ply]
	if list and #list > 0 and not ply.__mvfx_disabled then
		for i = #list, 1, -1 do
			if FX_ALIAS[list[i]] then return list[i], FX_ALIAS[list[i]] end
		end
	end
	return nil, "None"
end

local function vbApply(ply)
	if not IsValid(ply) then return end
	local _, preset = vbResolve(ply)
	metrovoice.EnableEffect(ply:UserID(), MetroVoice.Presets[preset] or metrovoice.EFF_NONE)
	appliedState[ply:UserID()] = preset
	if preset == "None" and (not vbActive[ply] or #vbActive[ply] == 0) then
		MetroVoice.Refresh(ply) -- fall back to config rules once manual fx are cleared
	end
end

local PLY = FindMetaTable("Player")

function PLY:AddVoiceFX(fx)
	vbActive[self] = vbActive[self] or {}
	table.insert(vbActive[self], tostring(fx))
	vbApply(self)
end
function PLY:RemoveVoiceFX(fx)
	local list = vbActive[self]; if not list then return false end
	for i = #list, 1, -1 do
		if list[i] == fx then table.remove(list, i); vbApply(self); return true end
	end
	return false
end
function PLY:ClearVoiceFX() vbActive[self] = {}; vbApply(self) end
function PLY:GetVoiceFX()
	local out = {}
	for _, v in ipairs(vbActive[self] or {}) do out[#out + 1] = v end
	return out
end
function PLY:GetActiveVoiceFX() local name = vbResolve(self); return name end
function PLY:HasVoiceFX(fx)
	for _, v in ipairs(vbActive[self] or {}) do if v == fx then return true end end
	return false
end
function PLY:SetVoiceFXDisabled(disabled) self.__mvfx_disabled = disabled and true or false; vbApply(self) end
function PLY:GetVoiceFXDisabled() return self.__mvfx_disabled or false end

hook.Add("PlayerDisconnected", "MetroVoiceFX_VBCleanup", function(ply) vbActive[ply] = nil end)

VoiceBox = VoiceBox or {}
VoiceBox.FX = VoiceBox.FX or {}
function VoiceBox.FX.IsValidVoiceFX(fx) return FX_ALIAS[fx] ~= nil end
-- Radio static is baked into the preset; the native module has no runtime toggle,
-- so this stores the flag for API compatibility but does not change the sound.
function VoiceBox.FX.SetRadioStaticEnabled(enabled) VoiceBox.FX.__radioStatic = enabled and true or false end

-- ============================================================================
-- Debug / admin console commands. These work from the SERVER CONSOLE (the
-- Pterodactyl "Console" tab / RCON) as well as in-game for a superadmin:
--   metrovoice_presets                 - list available effect names
--   metrovoice_list                    - show module status + every player's effect
--   metrovoice_set <player> <preset>   - give a player an effect (player = name / #userid / steamid)
--   metrovoice_clear <player>          - remove a player's effect
-- Example from server console:  metrovoice_set Mayorov Combine
-- ============================================================================
local function mvAllowed(ply) return (not IsValid(ply)) or ply:IsSuperAdmin() end  -- server console = trusted
local function mvReply(ply, msg)
	if IsValid(ply) then ply:PrintMessage(HUD_PRINTCONSOLE, msg) else print(msg) end
end
local function mvPresetList()
	local names = {}
	for n in pairs(MetroVoice.Presets) do names[#names + 1] = n end
	table.sort(names)
	return table.concat(names, ", ")
end
local function mvFindPlayer(id)
	if not id or id == "" then return nil end
	id = tostring(id)
	for _, p in ipairs(player.GetAll()) do
		if tostring(p:UserID()) == id or ("#" .. p:UserID()) == id then return p end
		if p:SteamID() == id or p:SteamID64() == id then return p end
	end
	local low = id:lower()
	for _, p in ipairs(player.GetAll()) do
		if p:Nick():lower():find(low, 1, true) then return p end
	end
	return nil
end

concommand.Add("metrovoice_presets", function(ply)
	if not mvAllowed(ply) then return end
	mvReply(ply, "[MetroVoiceFX] presets: " .. mvPresetList())
end)

concommand.Add("metrovoice_list", function(ply)
	if not mvAllowed(ply) then return end
	mvReply(ply, "[MetroVoiceFX] module loaded: " .. tostring(metrovoice ~= nil) ..
		" | players: " .. #player.GetAll())
	for _, p in ipairs(player.GetAll()) do
		local fx = p:GetActiveVoiceFX() or appliedState[p:UserID()] or "None"
		mvReply(ply, string.format("  #%-4d %-24s -> %s", p:UserID(), p:Nick(), fx))
	end
end)

concommand.Add("metrovoice_set", function(ply, _, args)
	if not mvAllowed(ply) then return end
	local target = mvFindPlayer(args[1])
	local preset = args[2]
	if not target then
		mvReply(ply, "[MetroVoiceFX] player not found: '" .. tostring(args[1]) ..
			"'. Try name / #userid / steamid (see metrovoice_list).")
		return
	end
	if not preset or not MetroVoice.Presets[preset] then
		mvReply(ply, "[MetroVoiceFX] unknown preset '" .. tostring(preset) ..
			"'. Available: " .. mvPresetList())
		return
	end
	target:ClearVoiceFX()
	target:AddVoiceFX(preset)
	mvReply(ply, string.format("[MetroVoiceFX] %s -> %s. Have them speak in voice chat.",
		target:Nick(), preset))
end)

concommand.Add("metrovoice_clear", function(ply, _, args)
	if not mvAllowed(ply) then return end
	local target = mvFindPlayer(args[1])
	if not target then
		mvReply(ply, "[MetroVoiceFX] player not found: '" .. tostring(args[1]) .. "'")
		return
	end
	target:ClearVoiceFX()
	mvReply(ply, "[MetroVoiceFX] cleared effect from " .. target:Nick())
end)

include("metro_voicefx_config.lua")

local presetCount = 0
for _ in pairs(MetroVoice.Presets) do presetCount = presetCount + 1 end
print("[MetroVoiceFX] Loaded, " .. presetCount .. " presets available.")
