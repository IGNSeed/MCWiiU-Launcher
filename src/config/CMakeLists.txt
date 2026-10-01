add_library(CemuConfig
    ActiveSettings.cpp
    ActiveSettings.h
    CemuConfig.cpp
    CemuConfig.h
    ConfigValue.h
    LaunchSettings.cpp
    LaunchSettings.h
    NetworkSettings.cpp
    NetworkSettings.h
    XMLConfig.h
)

cemu_set_msvc_runtime(CemuConfig)

cemu_use_precompiled_header(CemuConfig)

target_include_directories(CemuConfig PUBLIC "../")

target_link_libraries(CemuConfig PRIVATE
	CemuCommon
    CemuGui
    Boost::program_options
)
