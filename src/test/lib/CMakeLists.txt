# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

set(SOURCES
    ApiTest.cpp
    BasicTest.cpp
    DatagramTest.cpp
    DataTest.cpp
    DrillDescriptor.cpp
    EventTest.cpp
    HandshakeTest.cpp
    IngressWindowE2ETest.cpp
    IwpairExpectationTest.cpp
    MtuTest.cpp
    OwnershipTest.cpp
    PathTest.cpp
    QuicDrill.cpp
    TestConnection.cpp
    TestListener.cpp
    TestStream.cpp
    TestHelpers.h
    TestUtility.h
)

add_library(testlib STATIC ${SOURCES})

target_include_directories(testlib PRIVATE ${PROJECT_SOURCE_DIR}/src/test)

target_link_libraries(testlib PRIVATE inc warnings iwpair_lib)

set_property(TARGET testlib PROPERTY FOLDER "${QUIC_FOLDER_PREFIX}tests")
