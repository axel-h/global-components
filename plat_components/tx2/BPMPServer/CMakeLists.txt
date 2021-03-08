#
# Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
#
# SPDX-License-Identifier: BSD-2-Clause
#

cmake_minimum_required(VERSION 3.8.2)

# BPMPServer is a platform specific component for Nvidia Jetson TX2, so we
# ensure that is not visible on any other platform - and nothing get added to
# the include paths.
if(NOT KernelPlatformTx2)
    return()
endif()

# We have checked for KernelPlatformTx2 above, so here KernelArchARM should be
# set anyway. But we keep the check generic.
if(KernelArchARM AND NOT CAmkESDTS)
    message("BPMPServer not available, as CAmkESDTS is OFF")
    return()
endif()

project(BPMPServer C)

CAmkESAddImportPath(camkes-include)
CAmkESAddCPPInclude(camkes-include)
CAmkESAddTemplatesPath(templates)

DeclareCAmkESConnector(
    BPMPServerInit
    FROM
    BPMPServer.template.c
    FROM_LIBS
    BPMPServer-server
    TO
    empty.c
)

DeclareCAmkESConnector(
    BPMPServerClientInit
    FROM
    BPMPServer-client.template.c
    FROM_LIBS
    BPMPServer-client
    TO
    empty.c
)

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/libBPMPServer-client"
    ${CMAKE_CURRENT_BINARY_DIR}/libBPMPServer-client
)

add_library(BPMPServer-server EXCLUDE_FROM_ALL src/server.c)
target_link_libraries(BPMPServer-server sel4camkes platsupportports)
