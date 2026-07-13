-- MetroVoiceFX core: talks to the native `metrovoice` module (gmsv_metro_voicefx_*)
-- and decides which preset applies to which player. Edit metro_voicefx_config.lua,
-- not this file, to configure rules.

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

function MetroVoice.Refresh(ply)
	if not IsValid(ply) or not ply:IsPlayer() then return end

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

include("metro_voicefx_config.lua")

local presetCount = 0
for _ in pairs(MetroVoice.Presets) do presetCount = presetCount + 1 end
print("[MetroVoiceFX] Loaded, " .. presetCount .. " presets available.")
