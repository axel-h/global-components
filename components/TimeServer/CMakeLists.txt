#
# Copyright 2018, Data61, CSIRO (ABN 41 687 119 230)
#
# SPDX-License-Identifier: BSD-2-Clause
#

cmake_minimum_required(VERSION 3.8.2)

if(KernelArchARM AND NOT CAmkESDTS)
    message("TimeServer not available, as CAmkESDTS is OFF")
    return()
endif()

if(NOT CAmkESCPP)
    message("TimeServer not available, as CAmkESCPP is OFF")
    return()
endif()

project(TimeServer C)

if("${KernelPlatform}" STREQUAL "pc99")
    list(APPEND TimeServerPlatSources src/plat/pc99/plat.c)
endif()

# The imx8 boards (imx8mm-evk, imx8mq-evk) share the same timer device
# configuration
if("${KernelPlatform}" MATCHES "imx8.*")
    set(PlatPrefix "imx8")
else()
    set(PlatPrefix "${KernelPlatform}")
endif()

DeclareCAmkESComponent(TimeServer SOURCES src/time_server.c ${TimeServerPlatSources})

CAmkESAddCPPInclude("${CMAKE_CURRENT_LIST_DIR}/include/plat/${PlatPrefix}/")
