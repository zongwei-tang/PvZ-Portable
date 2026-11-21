cmake_minimum_required(VERSION 3.12...4.0)

project(pvz-portable LANGUAGES C CXX)

include(CMake/ProjectVersion.cmake)

option(BUILD_STATIC "Statically link dependencies for a standalone exe (Windows MinGW, Linux)" OFF)
if(BUILD_STATIC)
	if(WIN32 AND NOT MSVC)
		set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
		set(BUILD_SHARED_LIBS OFF)
		add_link_options(-static)
	elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
		set(BUILD_SHARED_LIBS OFF)
	elseif(MSVC)
		message(WARNING "BUILD_STATIC is not supported for MSVC. Use a vcpkg -static triplet instead.")
		set(BUILD_STATIC OFF)
	else()
		message(WARNING "BUILD_STATIC is only supported on Windows with MinGW-based toolchains and on Linux.")
		set(BUILD_STATIC OFF)
	endif()
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if (CMAKE_GENERATOR MATCHES "Ninja")
	set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
endif()

if(MSVC)
	if(DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "-static")
		set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	elseif(DEFINED VCPKG_TARGET_TRIPLET)
		set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
	endif()
endif()

file(GLOB SOURCES CONFIGURE_DEPENDS
	src/*.cpp
	src/SexyAppFramework/*.cpp
	src/SexyAppFramework/graphics/*.cpp
	src/SexyAppFramework/imagelib/*.cpp
	src/SexyAppFramework/misc/*.cpp
	src/SexyAppFramework/paklib/*.cpp
	src/SexyAppFramework/sound/*.cpp
	src/SexyAppFramework/widget/*.cpp
	src/SexyAppFramework/fcaseopen/*.c
	src/PvzpLib/*.cpp
	src/Lawn/*.cpp
	src/Lawn/Widget/*.cpp
	src/Lawn/System/*.cpp
)

option(LOW_MEMORY "Enable low memory mode" OFF)
if(VCPKG_TOOLCHAIN)
	find_package(libopenmpt CONFIG REQUIRED)
	set(OPENMPT_LIB libopenmpt::libopenmpt)
else()
	find_library(OPENMPT_LIBRARY NAMES openmpt libopenmpt REQUIRED)
	find_path(OPENMPT_INCLUDE_DIR libopenmpt/libopenmpt.h REQUIRED)
	set(OPENMPT_LIB ${OPENMPT_LIBRARY})
	if(NOT EMSCRIPTEN AND NOT NINTENDO_SWITCH AND OPENMPT_LIBRARY MATCHES "\\${CMAKE_STATIC_LIBRARY_SUFFIX}$")
		include(CheckCXXSourceCompiles)
		include(CMakePushCheckState)
		cmake_push_check_state()
		set(CMAKE_REQUIRED_INCLUDES ${OPENMPT_INCLUDE_DIR})
		set(CMAKE_REQUIRED_LIBRARIES ${OPENMPT_LIBRARY})
		check_cxx_source_compiles("#include <libopenmpt/libopenmpt.h>
int main() {
	return openmpt_module_create_from_memory2(0, 0, 0, 0, 0, 0, 0, 0, 0) != 0;
}" OPENMPT_LINKS_STANDALONE)
		cmake_pop_check_state()
		if(NOT OPENMPT_LINKS_STANDALONE)
			find_library(MPG123_LIBRARY NAMES mpg123 libmpg123)
			find_library(VORBISFILE_LIBRARY NAMES vorbisfile libvorbisfile)
			find_library(VORBIS_LIBRARY NAMES vorbis libvorbis)
			find_library(OGG_LIBRARY NAMES ogg libogg)
			if(NOT MPG123_LIBRARY OR NOT VORBISFILE_LIBRARY OR NOT VORBIS_LIBRARY OR NOT OGG_LIBRARY)
				message(FATAL_ERROR "Static libopenmpt does not link standalone and some of its dependencies were not found (see the configure log for details)")
			endif()
			list(APPEND OPENMPT_LIB ${MPG123_LIBRARY} ${VORBISFILE_LIBRARY} ${VORBIS_LIBRARY} ${OGG_LIBRARY})
		endif()
	endif()
endif()

if(NINTENDO_SWITCH)
	list(APPEND SOURCES
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/switch/Window.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/switch/Input.cpp
	)
	list(APPEND PLAT_INCLUDES
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/switch
	)
elseif(EMSCRIPTEN)
	list(APPEND SOURCES
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/emscripten/Window.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/default/Input.cpp
	)
	list(APPEND PLAT_INCLUDES
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/emscripten
	)
else()
	list(APPEND SOURCES
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/default/Window.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/default/Input.cpp
	)
	list(APPEND PLAT_INCLUDES
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/default
	)
endif()

if(CMAKE_BUILD_TYPE AND CMAKE_BUILD_TYPE MATCHES "^[Dd][Ee][Bb][Uu][Gg]$")
	set(WIN_CONSOLE_DEFAULT ON)
	set(PVZ_DEBUG_DEFAULT ON)
else()
	set(WIN_CONSOLE_DEFAULT OFF)
	set(PVZ_DEBUG_DEFAULT OFF)
endif()

option(PVZ_DEBUG "Enable PVZ_DEBUG macro to enable cheats, hidden features, etc." ${PVZ_DEBUG_DEFAULT})
option(CONSOLE "Show console on Windows" ${WIN_CONSOLE_DEFAULT})
option(DO_FIX_BUGS "Define DO_FIX_BUGS macro (Community fixes for original game bugs of 1.2.0.1073 GOTY Edition)" OFF)
# Cloud save (WebDAV, Google Drive, OneDrive) needs an HTTP client: libcurl on desktop systems, NSURLSession on iOS
# (CLOUD_SAVE_APPLE_HTTP). WebAssembly is left out because most WebDAV servers refuse the browser's cross-origin
# requests; Switch and Android have no libcurl set up here. BUILD_STATIC builds would pick up a shared system libcurl
# (and its TLS stack), so they leave it out too.
if(NINTENDO_SWITCH OR ANDROID OR EMSCRIPTEN OR BUILD_STATIC)
	set(CLOUD_SAVE_DEFAULT OFF)
else()
	set(CLOUD_SAVE_DEFAULT ON)
endif()
option(CLOUD_SAVE "Enable cloud save to WebDAV, Google Drive and OneDrive (needs libcurl, or Apple's NSURLSession)" ${CLOUD_SAVE_DEFAULT})
# iOS has no libcurl but its own HTTP client, which also checks certificates against the system's trust store. A Mac
# build can use it too, e.g. to run cloudsync-test against it.
if(IOS)
	set(CLOUD_SAVE_APPLE_HTTP ON)
elseif(APPLE)
	option(CLOUD_SAVE_APPLE_HTTP "Cloud save through NSURLSession instead of libcurl" OFF)
else()
	set(CLOUD_SAVE_APPLE_HTTP OFF)
endif()
# Google Drive and OneDrive sign in through an OAuth app registered by whoever ships the build. Without these, players
# enter the client of their own app in the cloud save settings (see the README).
# The Google client of this project is built in by default. For a Desktop app client Google does not treat the secret as
# confidential: it ships inside every build anyway. Pass empty values to build without it.
set(CLOUD_SAVE_GOOGLE_CLIENT_ID "885661944583-3vt1ujgog9hl0u5bd007t9cfvdr315ga.apps.googleusercontent.com" CACHE STRING
	"Google OAuth client ID (type: Desktop app) built in for Google Drive")
set(CLOUD_SAVE_GOOGLE_CLIENT_SECRET "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" CACHE STRING
	"Client secret that goes with CLOUD_SAVE_GOOGLE_CLIENT_ID")
set(CLOUD_SAVE_ONEDRIVE_CLIENT_ID "" CACHE STRING
	"Microsoft app registration ID (platform: Mobile and desktop applications, redirect URI http://localhost) built in for OneDrive")
option(CLOUD_SAVE_TESTS "Build cloudsync-test, offline tests of the cloud save protocol" OFF)

if(EMSCRIPTEN)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -sUSE_ZLIB=1 -sUSE_LIBPNG=1 -sUSE_LIBJPEG=1 -sUSE_VORBIS=1 -sUSE_MPG123=1")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -sUSE_ZLIB=1 -sUSE_LIBPNG=1 -sUSE_LIBJPEG=1 -sUSE_VORBIS=1 -sUSE_MPG123=1 -fexceptions")
else()
	find_package(Threads REQUIRED)
	find_package(ZLIB REQUIRED)
	find_package(JPEG REQUIRED)
	find_package(PNG REQUIRED)
	if(CLOUD_SAVE AND NOT CLOUD_SAVE_APPLE_HTTP)
		find_package(CURL)
		if(NOT CURL_FOUND)
			message(STATUS "libcurl not found, building without cloud save")
		endif()
	endif()
endif()
if(EMSCRIPTEN AND PVZP_SDL3_MIXER_SOURCE_DIR)
	include(CMake/EmscriptenSDL3.cmake)
	add_subdirectory("${PVZP_SDL3_MIXER_SOURCE_DIR}" "${PROJECT_BINARY_DIR}/SDL3_mixer")
else()
	find_package(SDL3 3.2.12 CONFIG REQUIRED)
	find_package(SDL3_mixer CONFIG REQUIRED)
endif()

set(PVZP_SDL3_TARGET SDL3::SDL3)
set(PVZP_SDL3_MIXER_TARGET SDL3_mixer::SDL3_mixer)
if(BUILD_STATIC)
	if(TARGET SDL3::SDL3-static)
		set(PVZP_SDL3_TARGET SDL3::SDL3-static)
	endif()
	if(TARGET SDL3_mixer::SDL3_mixer-static)
		set(PVZP_SDL3_MIXER_TARGET SDL3_mixer::SDL3_mixer-static)
	endif()
endif()

# Android requires a shared library loaded by SDLActivity; other platforms use an executable.
if(ANDROID)
	add_library(pvz-portable SHARED ${SOURCES})
elseif(IOS)
	add_executable(pvz-portable MACOSX_BUNDLE ${SOURCES})
else()
	add_executable(pvz-portable ${SOURCES})
endif()

target_include_directories(pvz-portable PRIVATE
	${PLAT_INCLUDES}
	${OPENMPT_INCLUDE_DIR}
	${PROJECT_BINARY_DIR}
	${PROJECT_SOURCE_DIR}/src
	${PROJECT_SOURCE_DIR}/src/SexyAppFramework
)
target_compile_definitions(pvz-portable PRIVATE
	$<$<BOOL:${PVZ_DEBUG}>:PVZ_DEBUG>
	$<$<BOOL:${DO_FIX_BUGS}>:DO_FIX_BUGS>
)
target_compile_features(pvz-portable PRIVATE cxx_std_20)
set_target_properties(pvz-portable PROPERTIES CXX_EXTENSIONS OFF)

if (LOW_MEMORY)
	target_compile_definitions(pvz-portable PRIVATE LOW_MEMORY)
endif()

target_link_libraries(pvz-portable PRIVATE
	${PVZP_SDL3_TARGET}
	${PVZP_SDL3_MIXER_TARGET}
	${OPENMPT_LIB}
)

if(CLOUD_SAVE AND (CURL_FOUND OR CLOUD_SAVE_APPLE_HTTP))
	set(_cloud_save_definitions PVZP_CLOUD_SAVE)
	# Google signs a Desktop app client in with its secret, and the secret belongs to that one client
	if(NOT "${CLOUD_SAVE_GOOGLE_CLIENT_ID}" STREQUAL "" AND "${CLOUD_SAVE_GOOGLE_CLIENT_SECRET}" STREQUAL "")
		message(FATAL_ERROR "CLOUD_SAVE_GOOGLE_CLIENT_ID needs CLOUD_SAVE_GOOGLE_CLIENT_SECRET, the secret of that Desktop app client")
	endif()
	if("${CLOUD_SAVE_GOOGLE_CLIENT_ID}" STREQUAL "" AND NOT "${CLOUD_SAVE_GOOGLE_CLIENT_SECRET}" STREQUAL "")
		message(FATAL_ERROR "CLOUD_SAVE_GOOGLE_CLIENT_SECRET is of no use without CLOUD_SAVE_GOOGLE_CLIENT_ID")
	endif()
	foreach(_client GOOGLE_CLIENT_ID GOOGLE_CLIENT_SECRET ONEDRIVE_CLIENT_ID)
		if(NOT "${CLOUD_SAVE_${_client}}" STREQUAL "")
			if(NOT "${CLOUD_SAVE_${_client}}" MATCHES "^[A-Za-z0-9._-]+$")
				message(FATAL_ERROR "CLOUD_SAVE_${_client} may only hold letters, digits, '.', '_' and '-'")
			endif()
			list(APPEND _cloud_save_definitions "PVZP_${_client}=\"${CLOUD_SAVE_${_client}}\"")
		endif()
	endforeach()
	if(CLOUD_SAVE_APPLE_HTTP)
		enable_language(OBJCXX)
		set(_cloud_save_apple_sources src/Lawn/System/CloudHttpApple.mm)
		list(APPEND _cloud_save_definitions PVZP_CLOUD_HTTP_APPLE)
		if(IOS)
			# the sign-in page in the system's sign-in sheet, and time to upload in the background
			list(APPEND _cloud_save_apple_sources src/Lawn/System/CloudSignInSheet.mm src/Lawn/System/CloudBackgroundTask.mm)
			list(APPEND _cloud_save_definitions PVZP_CLOUD_SIGN_IN_SHEET)
			target_link_libraries(pvz-portable PRIVATE "-framework AuthenticationServices")
		endif()
		set_source_files_properties(${_cloud_save_apple_sources} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
		target_sources(pvz-portable PRIVATE ${_cloud_save_apple_sources})
		target_link_libraries(pvz-portable PRIVATE "-framework Foundation")
	else()
		target_link_libraries(pvz-portable PRIVATE CURL::libcurl)
	endif()
	target_compile_definitions(pvz-portable PRIVATE ${_cloud_save_definitions})
	if(HAIKU)
		target_link_libraries(pvz-portable PRIVATE network)	# the sockets the browser sign-in listens on
	endif()

	if(CLOUD_SAVE_TESTS)
		add_executable(cloudsync-test tests/cloudsync_test.cpp src/Lawn/System/CloudSync.cpp src/Lawn/System/CloudOAuth.cpp
			src/Lawn/System/CloudLoopback.cpp)
		target_include_directories(cloudsync-test PRIVATE src/Lawn/System)
		target_compile_features(cloudsync-test PRIVATE cxx_std_20)
		target_compile_definitions(cloudsync-test PRIVATE PVZP_CLOUD_SAVE)
		target_link_libraries(cloudsync-test PRIVATE Threads::Threads)
		if(CLOUD_SAVE_APPLE_HTTP)
			target_sources(cloudsync-test PRIVATE src/Lawn/System/CloudHttpApple.mm)
			target_compile_definitions(cloudsync-test PRIVATE PVZP_CLOUD_HTTP_APPLE)
			target_link_libraries(cloudsync-test PRIVATE "-framework Foundation")
		else()
			target_link_libraries(cloudsync-test PRIVATE CURL::libcurl)
		endif()
		if(WIN32)
			target_link_libraries(cloudsync-test PRIVATE ws2_32)
		elseif(HAIKU)
			target_link_libraries(cloudsync-test PRIVATE network)
		endif()
	endif()
endif()

# Tests of the geometry of the phone layout of levels (src/Lawn/System/PhoneLayout.h)
option(PHONE_LAYOUT_TESTS "Build phone-layout-test, tests of the phone layout geometry" OFF)
if(PHONE_LAYOUT_TESTS)
	add_executable(phone-layout-test tests/phone_layout_test.cpp src/Lawn/System/PhoneLayout.cpp)
	target_include_directories(phone-layout-test PRIVATE src/Lawn/System)
	target_compile_features(phone-layout-test PRIVATE cxx_std_20)
endif()

# A test of the phone layout in the running game (src/Lawn/System/PhoneLayoutSelfTest.h, tests/phone_layout_selftest.sh):
# for test builds only
option(PHONE_LAYOUT_SELFTEST "Build in the self-test of the phone layout, run by tests/phone_layout_selftest.sh" OFF)
if(PHONE_LAYOUT_SELFTEST)
	target_compile_definitions(pvz-portable PRIVATE PVZP_PHONE_LAYOUT_SELFTEST)
endif()

# The Mac app updates itself through Sparkle, which the app made by macos/build-app.sh carries and the game loads at run
# time (see src/Lawn/System/AppUpdater.h), so building needs no Sparkle.
if(APPLE AND NOT IOS)
	option(MAC_UPDATER "Let the Mac app update itself through Sparkle" ON)
else()
	set(MAC_UPDATER OFF)
endif()
if(MAC_UPDATER)
	enable_language(OBJCXX)
	target_sources(pvz-portable PRIVATE src/Lawn/System/MacUpdater.mm)
	target_compile_definitions(pvz-portable PRIVATE PVZP_MAC_UPDATER)
	target_link_libraries(pvz-portable PRIVATE "-framework Foundation")
endif()

# The iOS app looks for updates in the AltStore source of the releases of IOS_UPDATE_REPOSITORY ("owner/name" on GitHub,
# set by the CI from the repository it builds), and points to AltStore or SideStore for them (src/Lawn/System/AppUpdater.h).
if(IOS)
	set(IOS_UPDATE_REPOSITORY "" CACHE STRING "GitHub repository (owner/name) whose releases the iOS app takes updates from")
	if(NOT "${IOS_UPDATE_REPOSITORY}" STREQUAL "")
		if(NOT "${IOS_UPDATE_REPOSITORY}" MATCHES "^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
			message(FATAL_ERROR "IOS_UPDATE_REPOSITORY must be owner/name")
		endif()
		set(PVZP_UPDATE_SOURCE_URL "https://github.com/${IOS_UPDATE_REPOSITORY}/releases/latest/download/altstore-source.json")
		set(PVZP_RELEASE_PAGE_URL "https://github.com/${IOS_UPDATE_REPOSITORY}/releases/latest")
	else()
		set(PVZP_UPDATE_SOURCE_URL "")
		set(PVZP_RELEASE_PAGE_URL "")
	endif()
	enable_language(OBJCXX)
	set_source_files_properties(src/Lawn/System/IosUpdater.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
	target_sources(pvz-portable PRIVATE src/Lawn/System/IosUpdater.mm)
	target_compile_definitions(pvz-portable PRIVATE PVZP_IOS_UPDATER)
endif()

if(EMSCRIPTEN)
	set_property(TARGET pvz-portable APPEND PROPERTY LINK_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/wasm/shell.html"
	)
	target_link_options(pvz-portable PRIVATE
		"SHELL:-sUSE_ZLIB=1"
		"SHELL:-sUSE_LIBPNG=1"
		"SHELL:-sUSE_LIBJPEG=1"
		"SHELL:-sUSE_VORBIS=1 -sUSE_MPG123=1"
		"SHELL:-fexceptions"
		"SHELL:-sASYNCIFY"
		"SHELL:-sALLOW_MEMORY_GROWTH=1"
		"SHELL:-sMAXIMUM_MEMORY=1073741824"
		"SHELL:-sGROWABLE_ARRAYBUFFERS=0"
		"SHELL:-sFORCE_FILESYSTEM=1"
		"SHELL:-sEXPORTED_RUNTIME_METHODS=[\"FS\",\"IDBFS\",\"callMain\"]"
		"SHELL:-sINVOKE_RUN=0"
		"SHELL:-lidbfs.js"
		"SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/wasm/shell.html"
	)
	set_target_properties(pvz-portable PROPERTIES SUFFIX ".html")
else()
	target_link_libraries(pvz-portable PRIVATE
		PNG::PNG
		JPEG::JPEG
		ZLIB::ZLIB
		Threads::Threads
	)
endif()

if (WIN32)
	if(MSVC)
		target_compile_options(pvz-portable PRIVATE /utf-8)
	endif()

	target_sources(pvz-portable PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/LawnProject.rc)

	set_source_files_properties(
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/LawnProject.rc
		PROPERTIES
			COMPILE_DEFINITIONS "APP_ICON_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/icon.ico\""
			OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/icon.ico"
	)

	target_link_libraries(pvz-portable PRIVATE ws2_32 user32 gdi32 winmm imm32 shlwapi)
	target_compile_definitions(pvz-portable PRIVATE WINDOWS)

	if(NOT CONSOLE)
		if(MSVC)
			set_target_properties(pvz-portable PROPERTIES WIN32_EXECUTABLE TRUE)
		else()
			target_link_options(pvz-portable PRIVATE -mwindows)
		endif()
	endif()
elseif (NINTENDO_SWITCH)
	target_link_libraries(pvz-portable PRIVATE GLESv2 EGL glapi drm_nouveau)

	nx_generate_nacp(pvz-portable.nacp
		NAME "Plants vs. Zombies"
		AUTHOR "wszqkzqk, etc., a community-driven port"
		VERSION "${PVZP_VERSION_PLAIN}"
	)

	nx_create_nro(pvz-portable
		NACP pvz-portable.nacp
		ICON ${CMAKE_CURRENT_SOURCE_DIR}/icon-switch.jpg
	)
elseif (ANDROID)
	target_link_libraries(pvz-portable PRIVATE GLESv2 EGL android log)
	set_target_properties(pvz-portable PROPERTIES OUTPUT_NAME "main")
elseif (IOS)
	# SDL2 defined __IPHONEOS__, which the game's iOS code still checks; SDL3 names it SDL_PLATFORM_IOS
	target_compile_definitions(pvz-portable PRIVATE __IPHONEOS__=1)
	target_link_libraries(pvz-portable PRIVATE
		"-framework Foundation"
		"-framework UIKit"
		"-framework OpenGLES"
		"-framework QuartzCore"
		"-framework CoreGraphics"
		"-framework CoreMotion"
		"-framework GameController"
		"-framework AVFoundation"
		"-framework AudioToolbox"
		"-framework Metal"
	)

	configure_file(ios/Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
	set_target_properties(pvz-portable PROPERTIES
		MACOSX_BUNDLE TRUE
		MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.wszqkzqk.pvz-portable"
		MACOSX_BUNDLE_BUNDLE_NAME "Plants vs. Zombies"
		MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
		XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
		XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "16.4"
		XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "io.github.wszqkzqk.pvz-portable"
		XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
		XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
	)

	# Add launch storyboard to the bundle
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ios/LaunchScreen.storyboard")
		target_sources(pvz-portable PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ios/LaunchScreen.storyboard")
		set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/ios/LaunchScreen.storyboard"
			PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
	endif()

	# The app icon, made from icon.png like that of every other version: an asset catalog in the build folder with the
	# image drawn onto an opaque 1024x1024 square (ios/flatten-icon.swift; sips alone, keeping transparency, without
	# Swift)
	set(_ios_icon_catalog "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.xcassets")
	set(_ios_icon "${_ios_icon_catalog}/AppIcon.appiconset/AppIcon.png")
	file(MAKE_DIRECTORY "${_ios_icon_catalog}/AppIcon.appiconset")
	file(WRITE "${_ios_icon_catalog}/Contents.json" "{\"info\":{\"author\":\"xcode\",\"version\":1}}\n")
	file(WRITE "${_ios_icon_catalog}/AppIcon.appiconset/Contents.json"
		"{\"images\":[{\"filename\":\"AppIcon.png\",\"idiom\":\"universal\",\"platform\":\"ios\",\"size\":\"1024x1024\"}],"
		"\"info\":{\"author\":\"xcode\",\"version\":1}}\n")
	execute_process(COMMAND xcrun swift "${CMAKE_CURRENT_SOURCE_DIR}/ios/flatten-icon.swift" "${CMAKE_CURRENT_SOURCE_DIR}/icon.png"
		"${_ios_icon}" RESULT_VARIABLE _ios_icon_result OUTPUT_QUIET ERROR_QUIET)
	if(NOT _ios_icon_result EQUAL 0)
		execute_process(COMMAND sips -s format png -z 1024 1024 "${CMAKE_CURRENT_SOURCE_DIR}/icon.png" --out "${_ios_icon}"
			RESULT_VARIABLE _ios_icon_result OUTPUT_QUIET ERROR_QUIET)
	endif()
	if(NOT _ios_icon_result EQUAL 0)
		message(FATAL_ERROR "Could not make the iOS app icon from icon.png")
	endif()
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/icon.png" "${CMAKE_CURRENT_SOURCE_DIR}/ios/flatten-icon.swift")
	target_sources(pvz-portable PRIVATE "${_ios_icon_catalog}")
	set_source_files_properties("${_ios_icon_catalog}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()
