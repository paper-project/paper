cmake_minimum_required (VERSION 2.8.11)
project (paper)

if (WIN32)
	set (PLATFORM_COMPILE_FLAGS "-D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -DWIN32_LEAN_AND_MEAN")
else (WIN32)
	set (PLATFORM_COMPILE_FLAGS "-fPIC -DBOOST_ASIO_HAS_STD_ARRAY")
endif (WIN32)

if (WIN32)
	set (PLATFORM_GUI_TARGET_PROPERTIES WIN32)
else (WIN32)
	set (PLATFORM_GUI_TARGET_PROPERTIES "")
endif (WIN32)

if (APPLE)
	set (PLATFORM_LINK_FLAGS "-framework Foundation")
else (APPLE)
	set (PLATFORM_LINK_FLAGS "")
endif (APPLE)

if (WIN32)
	set (PLATFORM_QT_PACKAGES WinExtras)
else (WIN32)
	set (PLATFORM_QT_PACKAGES)
endif (WIN32)

include_directories (${CMAKE_SOURCE_DIR})

find_package (Boost REQUIRED COMPONENTS filesystem system log log_setup thread program_options)
include_directories (${Boost_INCLUDE_DIR})

find_package (GTest REQUIRED)
include_directories (${GTEST_INCLUDE_DIR})

find_package (CryptoPP REQUIRED)
include_directories (${CRYPTOPP_INCLUDE_DIR})

find_package (Qt5 REQUIRED COMPONENTS Core Gui Widgets Test ${PLATFORM_QT_PACKAGES})
include_directories (${Qt5Core_INCLUDE_DIRS} ${Qt5Gui_INCLUDE_DIRS} ${Qt5Widgets_INCLUDE_DIRS} ${Qt5Test_INCLUDE_DIRS})

include_directories (${CPPNETLIB_INCLUDE_DIRS})

add_library (ed25519
	ed25519-donna/ed25519-donna-portable.h
	ed25519-donna/ed25519-hash-custom.h
	ed25519-donna/ed25519-randombytes-custom.h
	ed25519-donna/ed25519.h
	ed25519-donna/ed25519.c)

add_library (xxhash
	xxhash/xxhash.c
	xxhash/xxhash.h)

add_library (lmdb
	liblmdb/lmdb.h
	liblmdb/mdb.c
	liblmdb/midl.c)

if (WIN32)
	set (BLAKE2_IMPLEMENTATION "blake2/blake2b-ref.c")
else (WIN32)
	set (BLAKE2_IMPLEMENTATION "blake2/blake2b.c")
endif (WIN32)

add_library (blake2
	blake2/blake2-config.h
	blake2/blake2-impl.h
	blake2/blake2.h
	${BLAKE2_IMPLEMENTATION})

if (APPLE)
	set (PLATFORM_SECURE_SOURCE paper/plat/osx/working.mm)
elseif (WIN32)
	set (PLATFORM_SECURE_SOURCE paper/plat/windows/working.cpp)
elseif (UNIX)
	set (PLATFORM_SECURE_SOURCE paper/plat/posix/working.cpp)
endif (APPLE)

add_library (secure
	${PLATFORM_SECURE_SOURCE}
	paper/config.hpp
	paper/secure.cpp
	paper/secure.hpp
	paper/utility.cpp
	paper/utility.hpp)

add_library (node
	paper/working.hpp
	paper/node.hpp
	paper/node.cpp)

add_executable (core_test
	paper/core_test/block.cpp
	paper/core_test/block_store.cpp
	paper/core_test/block_synchronization.cpp
	paper/core_test/node.cpp
	paper/core_test/conflicts.cpp
	paper/core_test/daemon.cpp
	paper/core_test/entry.cpp
	paper/core_test/gap_cache.cpp
	paper/core_test/landing.cpp
	paper/core_test/ledger.cpp
	paper/core_test/message.cpp
	paper/core_test/message_parser.cpp
	paper/core_test/processor_service.cpp
	paper/core_test/peer_container.cpp
	paper/core_test/rpc.cpp
	paper/core_test/network.cpp
	paper/core_test/uint256_union.cpp
	paper/core_test/wallet.cpp
	paper/core_test/wallets.cpp)

add_executable (slow_test
	paper/slow_test/node.cpp)

add_executable (qt_test
	paper/qt_test/entry.cpp
	paper/qt_test/qt.cpp)

add_executable (cli
	paper/cli/daemon.cpp
	paper/cli/daemon.hpp
	paper/cli/entry.cpp)

add_library (qt
	paper/qt/qt.cpp
	paper/qt/qt.hpp)

add_executable (landing
	paper/landing/entry.cpp)

if (WIN32)
	set (PLATFORM_WALLET_SOURCE paper/plat/windows/icon.cpp Paper.rc)
else (WIN32)
	set (PLATFORM_WALLET_SOURCE paper/plat/unknown/icon.cpp)
endif (WIN32)

add_executable (qt_wallet ${PLATFORM_GUI_TARGET_PROPERTIES}
	${PLATFORM_WALLET_SOURCE}
	paper/qt_wallet/entry.cpp)

add_executable (qt_system
	paper/qt_system/entry.cpp)

set (CPACK_PACKAGE_VERSION_MAJOR "6")
set (CPACK_PACKAGE_VERSION_MINOR "1")
set (CPACK_PACKAGE_VERSION_PATCH "5")

SET (ACTIVE_NETWORK paper_active_network CACHE STRING "Selects which network parameters are used")
set_property (CACHE ACTIVE_NETWORK PROPERTY STRINGS paper_test_network paper_beta_network paper_live_network)

set_target_properties (blake2 PROPERTIES COMPILE_FLAGS "-msse2 -std=c11")
set_target_properties (ed25519 PROPERTIES COMPILE_FLAGS "-msse2 -std=c11 -DED25519_CUSTOMHASH -DED25519_CUSTOMRNG")
set_target_properties (secure node core_test slow_test qt_test cli qt qt_wallet qt_system landing PROPERTIES COMPILE_FLAGS "-msse2 -std=c++11 -Werror=switch ${PLATFORM_COMPILE_FLAGS} -DQT_NO_KEYWORDS -DACTIVE_NETWORK=${ACTIVE_NETWORK} -DPAPER_VERSION_MAJOR=${CPACK_PACKAGE_VERSION_MAJOR} -DPAPER_VERSION_MINOR=${CPACK_PACKAGE_VERSION_MINOR} -DPAPER_VERSION_PATCH=${CPACK_PACKAGE_VERSION_PATCH}")
set_target_properties (secure node core_test slow_test cli qt qt_system landing PROPERTIES LINK_FLAGS "${PLATFORM_LINK_FLAGS}")
set_target_properties (qt_wallet qt_test PROPERTIES LINK_FLAGS "${PLATFORM_LINK_FLAGS}")

if (WIN32)
	set (PLATFORM_LIBS ws2_32 mswsock)
else (WIN32)
	set (PLATFORM_LIBS)
endif (WIN32)

if (WIN32)
	set (PLATFORM_WALLET_LIBS Qt5::WinExtras)
else (WIN32)
	set (PLATFORM_WALLET_LIBS)
endif (WIN32)

target_link_libraries (core_test node secure lmdb xxhash ed25519 blake2 ${Boost_FILESYSTEM_LIBRARY} ${Boost_SYSTEM_LIBRARY} ${Boost_LOG_LIBRARY} ${Boost_LOG_SETUP_LIBRARY} ${Boost_THREAD_LIBRARY} ${GTEST_MAIN_LIBRARY} ${GTEST_LIBRARY} ${CRYPTOPP_LIBRARY} ${CPPNETLIB_LIBRARIES} pthread ${PLATFORM_LIBS})

target_link_libraries (slow_test node secure lmdb xxhash ed25519 blake2 ${Boost_FILESYSTEM_LIBRARY} ${Boost_SYSTEM_LIBRARY} ${Boost_LOG_LIBRARY} ${Boost_LOG_SETUP_LIBRARY} ${Boost_THREAD_LIBRARY} ${CRYPTOPP_LIBRARY} ${GTEST_MAIN_LIBRARY} ${GTEST_LIBRARY} ${CPPNETLIB_LIBRARIES} pthread ${PLATFORM_LIBS})

target_link_libraries (cli node secure lmdb xxhash ed25519 blake2 ${Boost_FILESYSTEM_LIBRARY} ${Boost_SYSTEM_LIBRARY} ${Boost_LOG_LIBRARY} ${Boost_LOG_SETUP_LIBRARY} ${Boost_THREAD_LIBRARY} ${Boost_PROGRAM_OPTIONS_LIBRARY} ${CRYPTOPP_LIBRARY} ${CPPNETLIB_LIBRARIES} pthread ${PLATFORM_LIBS})

target_link_libraries (landing node secure lmdb xxhash ed25519 blake2 ${Boost_FILESYSTEM_LIBRARY} ${Boost_SYSTEM_LIBRARY} ${Boost_LOG_LIBRARY} ${Boost_LOG_SETUP_LIBRARY} ${Boost_THREAD_LIBRARY} ${Boost_PROGRAM_OPTIONS_LIBRARY} ${CRYPTOPP_LIBRARY} ${CPPNETLIB_LIBRARIES} pthread ${PLATFORM_LIBS})

target_link_libraries (qt_test node secure lmdb xxhash ed25519 qt blake2 ${Boost_FILESYSTEM_LIBRARY} ${Boost_SYSTEM_LIBRARY} ${Boost_LOG_LIBRARY} ${Boost_LOG_SETUP_LIBRARY} ${Boost_THREAD_LIBRARY} ${GTEST_LIBRARY} ${CRYPTOPP_LIBRARY} ${CPPNETLIB_LIBRARIES} Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Test ${QT_QTGUI_LIBRARY} pthread ${PLATFORM_LIBS})

target_link_libraries (qt_wallet node secure lmdb xxhash ed25519 qt blake2 ${Boost_FILESYSTEM_LIBRARY} ${Boost_SYSTEM_LIBRARY} ${Boost_LOG_LIBRARY} ${Boost_LOG_SETUP_LIBRARY} ${Boost_THREAD_LIBRARY} ${CRYPTOPP_LIBRARY} ${CPPNETLIB_LIBRARIES} Qt5::Core Qt5::Gui Qt5::Widgets ${QT_QTGUI_LIBRARY} pthread ${PLATFORM_LIBS} ${PLATFORM_WALLET_LIBS})

target_link_libraries (qt_system node secure lmdb xxhash ed25519 qt blake2 ${Boost_FILESYSTEM_LIBRARY} ${Boost_SYSTEM_LIBRARY} ${Boost_LOG_LIBRARY} ${Boost_LOG_SETUP_LIBRARY} ${Boost_THREAD_LIBRARY} ${GTEST_LIBRARY} ${CRYPTOPP_LIBRARY} ${CPPNETLIB_LIBRARIES} Qt5::Core Qt5::Gui Qt5::Widgets ${QT_QTGUI_LIBRARY} pthread ${PLATFORM_LIBS})

if (APPLE)
	get_filename_component (Qt5Core_framework_DIR ${Qt5_DIR}/../../QtCore.framework ABSOLUTE)
	get_filename_component (Qt5Gui_framework_DIR ${Qt5_DIR}/../../QtGui.framework ABSOLUTE)
	get_filename_component (Qt5PrintSupport_framework_DIR ${Qt5_DIR}/../../QtPrintSupport.framework ABSOLUTE)
	get_filename_component (Qt5Test_framework_DIR ${Qt5_DIR}/../../QtTest.framework ABSOLUTE)
	get_filename_component (Qt5Widgets_framework_DIR ${Qt5_DIR}/../../QtWidgets.framework ABSOLUTE)
	get_filename_component (Qt5_platforms_DIR ${Qt5_DIR}/../../../plugins/platforms ABSOLUTE)

	file (COPY ${Qt5Core_framework_DIR} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/packaging)
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-id" "@executable_path/../Frameworks/QtCore.framework/Versions/5/QtCore" "${CMAKE_CURRENT_BINARY_DIR}/packaging/QtCore.framework/Versions/5/QtCore")

	file (COPY ${Qt5Gui_framework_DIR} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/packaging)
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-id" "@executable_path/../Frameworks/QtGui.framework/Versions/5/QtGui" "${CMAKE_CURRENT_BINARY_DIR}/packaging/QtGui.framework/Versions/5/QtGui")
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-change" "${Qt5Core_framework_DIR}/Versions/5/QtCore" "@executable_path/../Frameworks/QtCore.framework/Versions/5/QtCore" "${CMAKE_CURRENT_BINARY_DIR}/packaging/QtGui.framework/Versions/5/QtGui")

	file (COPY ${Qt5PrintSupport_framework_DIR} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/packaging)
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-id" "@executable_path/../Frameworks/QtPrintSupport.framework/Versions/5/QtPrintSupport" "${CMAKE_CURRENT_BINARY_DIR}/packaging/QtPrintSupport.framework/Versions/5/QtPrintSupport")
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-change" "${Qt5Core_framework_DIR}/Versions/5/QtCore" "@executable_path/../Frameworks/QtCore.framework/Versions/5/QtCore" "${CMAKE_CURRENT_BINARY_DIR}/packaging/QtPrintSupport.framework/Versions/5/QtPrintSupport")
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-change" "${Qt5Gui_framework_DIR}/Versions/5/QtGui" "@executable_path/../Frameworks/QtGui.framework/Versions/5/QtGui" "${CMAKE_CURRENT_BINARY_DIR}/packaging/QtPrintSupport.framework/Versions/5/QtPrintSupport")
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-change" "${Qt5Widgets_framework_DIR}/Versions/5/QtWidgets" "@executable_path/../Frameworks/QtWidgets.framework/Versions/5/QtWidgets" "${CMAKE_CURRENT_BINARY_DIR}/packaging/QtPrintSupport.framework/Versions/5/QtPrintSupport")

	file (COPY ${Qt5Test_framework_DIR} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/packaging)
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-id" "@executable_path/../Frameworks/QtTest.framework/Versions/5/QtTest" "${CMAKE_CURRENT_BINARY_DIR}/packaging/QtTest.framework/Versions/5/QtTest")
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-change" "${Qt5Core_framework_DIR}/Versions/5/QtCore" "@executable_path/../Frameworks/QtCore.framework/Versions/5/QtCore" "${CMAKE_CURRENT_BINARY_DIR}/packaging/QtTest.framework/Versions/5/QtTest")

	file (COPY ${Qt5Widgets_framework_DIR} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/packaging)
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-id" "@executable_path/../Frameworks/QtWidgets.framework/Versions/5/QtWidgets" "${CMAKE_CURRENT_BINARY_DIR}/packaging/QtWidgets.framework/Versions/5/QtWidgets")
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-change" "${Qt5Core_framework_DIR}/Versions/5/QtCore" "@executable_path/../Frameworks/QtCore.framework/Versions/5/QtCore" "${CMAKE_CURRENT_BINARY_DIR}/packaging/QtWidgets.framework/Versions/5/QtWidgets")
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-change" "${Qt5Gui_framework_DIR}/Versions/5/QtGui" "@executable_path/../Frameworks/QtGui.framework/Versions/5/QtGui" "${CMAKE_CURRENT_BINARY_DIR}/packaging/QtWidgets.framework/Versions/5/QtWidgets")

	file (COPY ${Qt5_DIR}/../../../plugins/platforms/libqcocoa.dylib DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/packaging)
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-change" "${Qt5Core_framework_DIR}/Versions/5/QtCore" "@executable_path/../Frameworks/QtCore.framework/Versions/5/QtCore" "${CMAKE_CURRENT_BINARY_DIR}/packaging/libqcocoa.dylib")
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-change" "${Qt5Gui_framework_DIR}/Versions/5/QtGui" "@executable_path/../Frameworks/QtGui.framework/Versions/5/QtGui" "${CMAKE_CURRENT_BINARY_DIR}/packaging/libqcocoa.dylib")
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-change" "${Qt5PrintSupport_framework_DIR}/Versions/5/QtPrintSupport" "@executable_path/../Frameworks/QtPrintSupport.framework/Versions/5/QtPrintSupport" "${CMAKE_CURRENT_BINARY_DIR}/packaging/libqcocoa.dylib")
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-change" "${Qt5Widgets_framework_DIR}/Versions/5/QtWidgets" "@executable_path/../Frameworks/QtWidgets.framework/Versions/5/QtWidgets" "${CMAKE_CURRENT_BINARY_DIR}/packaging/libqcocoa.dylib")

	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:qt_wallet> ${CMAKE_CURRENT_BINARY_DIR}/packaging)
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-change" "${Qt5Core_framework_DIR}/Versions/5/QtCore" "@executable_path/../Frameworks/QtCore.framework/Versions/5/QtCore" "${CMAKE_CURRENT_BINARY_DIR}/packaging/qt_wallet")
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-change" "${Qt5Gui_framework_DIR}/Versions/5/QtGui" "@executable_path/../Frameworks/QtGui.framework/Versions/5/QtGui" "${CMAKE_CURRENT_BINARY_DIR}/packaging/qt_wallet")
	add_custom_command (TARGET qt_wallet POST_BUILD COMMAND "install_name_tool" "-change" "${Qt5Widgets_framework_DIR}/Versions/5/QtWidgets" "@executable_path/../Frameworks/QtWidgets.framework/Versions/5/QtWidgets" "${CMAKE_CURRENT_BINARY_DIR}/packaging/qt_wallet")

	install (FILES ${CMAKE_CURRENT_BINARY_DIR}/packaging/qt_wallet DESTINATION Paper.app/Contents/MacOS PERMISSIONS OWNER_EXECUTE OWNER_READ)
	install (FILES Info.plist DESTINATION Paper.app/Contents)
	install (FILES qt.conf DESTINATION Paper.app/Contents/Resources)
	install (DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/packaging/QtCore.framework" DESTINATION Paper.app/Contents/Frameworks USE_SOURCE_PERMISSIONS)
	install (DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/packaging/QtGui.framework" DESTINATION Paper.app/Contents/Frameworks USE_SOURCE_PERMISSIONS)
	install (DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/packaging/QtPrintSupport.framework" DESTINATION Paper.app/Contents/Frameworks USE_SOURCE_PERMISSIONS)
	install (DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/packaging/QtTest.framework" DESTINATION Paper.app/Contents/Frameworks USE_SOURCE_PERMISSIONS)
	install (DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/packaging/QtWidgets.framework" DESTINATION Paper.app/Contents/Frameworks USE_SOURCE_PERMISSIONS)
	install (FILES "${CMAKE_CURRENT_BINARY_DIR}/packaging/libqcocoa.dylib" DESTINATION Paper.app/Contents/PlugIns/platforms PERMISSIONS OWNER_EXECUTE OWNER_READ)
	install (FILES Paper.icns DESTINATION Paper.app/Contents/Resources)
elseif (WIN32)
	get_filename_component (Qt5_bin_DIR ${Qt5_DIR}/../../../bin ABSOLUTE)
	install (TARGETS qt_wallet DESTINATION .)
	install (FILES ${Qt5_bin_DIR}/Qt5Core.dll DESTINATION .)
	install (FILES ${Qt5_bin_DIR}/Qt5Gui.dll DESTINATION .)
	install (FILES ${Qt5_bin_DIR}/Qt5Widgets.dll DESTINATION .)
	install (FILES ${Qt5_bin_DIR}/Qt5WinExtras.dll DESTINATION .)
	install (FILES ${Qt5_DIR}/../../../plugins/platforms/qwindows.dll DESTINATION platforms)
	install (FILES "../../../bin/libgcc_s_dw2-1.dll" DESTINATION .)
	install (FILES "../../../bin/libwinpthread-1.dll" DESTINATION .)
	install (FILES "../../../bin/libstdc++-6.dll" DESTINATION .)
else ()
	install (TARGETS qt_wallet DESTINATION .)
endif ()

include (CPack)
