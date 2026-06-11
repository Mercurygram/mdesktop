# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

add_executable(test_text WIN32)
init_target(test_text "(tests)")

target_include_directories(test_text PRIVATE ${src_loc})

nice_target_sources(test_text ${src_loc}
PRIVATE
    tests/test_main.cpp
    tests/test_main.h
    tests/test_text.cpp
)

nice_target_sources(test_text ${res_loc}
PRIVATE
    qrc/emoji_1.qrc
    qrc/emoji_2.qrc
    qrc/emoji_3.qrc
    qrc/emoji_4.qrc
    qrc/emoji_5.qrc
    qrc/emoji_6.qrc
    qrc/emoji_7.qrc
    qrc/emoji_8.qrc
)

target_link_libraries(test_text
PRIVATE
    desktop-app::lib_base
    desktop-app::lib_crl
    desktop-app::lib_ui
    desktop-app::external_qt
    desktop-app::external_qt_static_plugins
)

set_target_properties(test_text PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_dependencies(Telegram test_text)

target_prepare_qrc(test_text)

# Secret-chat (MTProto end-to-end) crypto unit tests. Standalone, headless;
# links only the crypto core, the auth-key AES-IGE helpers and the generated
# decrypted-layer scheme -- no Main::Session, no network.
add_executable(test_secret_chat)
init_target(test_secret_chat "(tests)")

target_include_directories(test_secret_chat PRIVATE
    ${src_loc}
    ${CMAKE_BINARY_DIR}/Telegram/gen
)

# Same precompiled header the mtproto sources use in the main build; it pulls
# scheme.h (MTPint128/256), crl and range-v3 that mtproto_auth_key.h needs.
target_precompile_headers(test_secret_chat PRIVATE ${src_loc}/mtproto/mtproto_pch.h)

set_source_files_properties(
    ${CMAKE_BINARY_DIR}/Telegram/gen/secret_scheme.cpp
    PROPERTIES GENERATED TRUE
)

nice_target_sources(test_secret_chat ${src_loc}
PRIVATE
    tests/test_secret_chat.cpp
    mtproto/secret_chat/secret_chat_encryption.cpp
    mtproto/secret_chat/secret_chat_encryption.h
    mtproto/mtproto_auth_key.cpp
    mtproto/mtproto_auth_key.h
)

# The secret scheme is generated without a dump-to-text helper, so it links
# cleanly on its own -- unlike the full api scheme in td_scheme.
target_sources(test_secret_chat PRIVATE
    ${CMAKE_BINARY_DIR}/Telegram/gen/secret_scheme.cpp
)

target_link_libraries(test_secret_chat
PRIVATE
    desktop-app::lib_base
    desktop-app::lib_tl
    desktop-app::lib_crl
    desktop-app::lib_storage
    desktop-app::external_qt
    desktop-app::external_openssl
    desktop-app::external_ranges
)

set_target_properties(
    test_secret_chat
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_dependencies(Telegram test_secret_chat)
add_dependencies(test_secret_chat td_scheme_scheme td_scheme_secret_scheme)
