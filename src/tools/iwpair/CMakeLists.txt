# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

#
# Shared protocol/codec/phase-math/metrics code of the ingress-window e2e
# test and the standalone iwpair tools (specs/ingress-window-e2e-test.md,
# S2). Pure codecs and arithmetic: no MsQuic API calls, no platform
# headers, so it links into both the tools and the test library.
#
# This directory is added to the build when (QUIC_BUILD_TOOLS OR
# QUIC_BUILD_TEST), so test-only configurations keep building.
#

add_library(iwpair_lib STATIC IwPairCommon.cpp)
target_include_directories(iwpair_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(iwpair_lib PUBLIC inc warnings)
set_property(TARGET iwpair_lib PROPERTY FOLDER "${QUIC_FOLDER_PREFIX}tools")

#
# The tools themselves are only built when the tools are enabled;
# add_quic_tool is defined by src/tools/CMakeLists.txt.
#
if(QUIC_BUILD_TOOLS)
    #
    # The pause/resume phase coverage (R14) uses the preview
    # receive-pause APIs; both tools are built with the preview macro so
    # the X script token and the suite's pause profile are exercised
    # (S12 items 16-17). A build without the macro rejects the X token
    # at parse time and skips the suite's pause profile (S-section,
    # pause support).
    #
    add_definitions(-DQUIC_API_ENABLE_PREVIEW_FEATURES)
    add_quic_tool(iwpair-server server.cpp)
    target_link_libraries(iwpair-server iwpair_lib)
    add_quic_tool(iwpair-client client.cpp)
    target_link_libraries(iwpair-client iwpair_lib)
endif()
