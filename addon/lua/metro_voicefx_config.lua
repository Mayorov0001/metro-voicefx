-- MetroVoiceFX config
-- Available presets: None, Radio, Phone, Stormtrooper, Combine, PA, Muffled, Masked
-- Test any preset instantly as superadmin with: metrovoice_test <PresetName>

-- Specific players (SteamID or SteamID64) always get this preset, regardless
-- of team/model/usergroup. Useful for a Ranger commander's radio, an NPC-ish
-- faction leader, etc.
-- MetroVoice.Config:AddPlayerFX("Combine", "STEAM_0:1:7099", "76561197960279927")

-- Usergroups (including secondary groups if you use an admin mod that grants
-- those, e.g. GmodAdminSuite).
-- MetroVoice.Config:AddUsergroupFX("Combine", "combine-fx")
-- MetroVoice.Config:AddUsergroupFX("Stormtrooper", "stalker-fx")

-- Teams/factions - replace with your gamemode's real TEAM_ constants.
-- MetroVoice.Config:AddTeamFX("Combine", TEAM_RANGER)
-- MetroVoice.Config:AddTeamFX("Radio", TEAM_STALKER)

-- Playermodels - anyone wearing this model gets the effect regardless of team.
-- MetroVoice.Config:AddModelFX("Combine", "models/player/combine_soldier_prisonguard.mdl")
-- MetroVoice.Config:AddModelFX("Masked", "models/player/gasmask.mdl")
