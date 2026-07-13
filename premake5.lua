newoption({
	trigger = "gmcommon",
	description = "Sets the path to the garrysmod_common (https://github.com/danielga/garrysmod_common) directory",
	value = "path to garrysmod_common directory"
})

local gmcommon = assert(_OPTIONS.gmcommon or os.getenv("GARRYSMOD_COMMON"),
	"you didn't provide a path to your garrysmod_common (https://github.com/danielga/garrysmod_common) directory")
include(gmcommon .. "/generator.v3.lua")

CreateWorkspace({name = "metro_voicefx"})
	CreateProject({serverside = true})
		IncludeSDKCommon()
		IncludeSDKTier0()
		IncludeSDKTier1()
		IncludeDetouring()
		IncludeScanning()
		IncludeLuaShared()
		IncludeHelpersExtended()

		links("opus")
		includedirs("opus/include")

		filter({"platforms:x86_64"})
			libdirs {"opus/lib64"}

		filter({"platforms:x86"})
			libdirs {"opus/lib32"}

		-- Bundle the C++ runtime into the module so it doesn't depend on the
		-- server container's libstdc++/libgcc version (fixes the
		-- "GLIBCXX_3.4.32 not found" load error when built on a newer distro).
		filter("system:linux")
			-- -pthread: the worker pool uses std::thread/mutex/condition_variable.
			buildoptions({"-pthread"})
			linkoptions({"-static-libstdc++", "-static-libgcc", "-pthread"})

		filter("system:windows")
			links("ws2_32")
