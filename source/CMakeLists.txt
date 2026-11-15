cmake_minimum_required(VERSION 3.28.2)

include(../sdk/config.cmake)
project(radio C ASM)

# Fasoley Radio: internet radio elf (Fasoley Radio https stream + Radio Paradise + stations.txt). Network = ENIP sockets (bt-pan or GPRS).
# Common core (RADIO_CORE): player state machine + CSM + log (app.c), radio.cfg/stations.txt (cfg.c), own main
#   screen (screen.c), native menus (menus.c), HTTP/ICY (radio.c, icy.c), TLS (link.c).
# Two backends (backend.h), two elfs:
# SG (S65v58 / SK65v50) FasoleyRadio.elf: src/be_sg.c - firmware player on a virtual WAV (patch 10931), MP3
#   decoded by Helix (third_party/helix), src/mp3s.c + src/vstream.c.
# ELKA (EL71v45) FasoleyRadio_ELKA.elf: src/be_elka.c - the firmware plays MP3 itself: virtual .mp3 file whose
#   bytes are the stream's frames (src/mp3pass.c + src/vstream_nsg.c, RADIO_PASS), no Helix.
set(HELIX_DIR third_party/helix)
file(GLOB HELIX_SRC ${HELIX_DIR}/*.c ${HELIX_DIR}/real/*.c)
# BearSSL (third_party/bearssl, MIT) for https streams: src/link.c; --gc-sections keeps only what the
# few suites of link.c need
set(BEARSSL_DIR ${CMAKE_CURRENT_LIST_DIR}/third_party/bearssl)
file(GLOB_RECURSE BEARSSL_SRC ${BEARSSL_DIR}/src/*.c)
set_source_files_properties(${BEARSSL_SRC} PROPERTIES COMPILE_OPTIONS
	"-Os;-w;-include;${CMAKE_CURRENT_LIST_DIR}/src/bearssl_libc.h")
set(RADIO_CORE src/fwlib.c src/app.c src/cfg.c src/screen.c src/menus.c src/link.c src/radio.c src/icy.c src/mp3hdr.c src/libc.c)
set(RADIO_SRC ${RADIO_CORE} src/be_sg.c src/mp3s.c src/vstream.c)
set(RADIO_ELKA_SRC ${RADIO_CORE} src/be_elka.c src/mp3pass.c src/vstream_nsg.c src/dns.c)

# Helix: -O2 ARM (the decode runs in the Obs task, as in the s65-mp3 VKP); polyphase filter from Helix's ARM asm
# (arm/asmpoly_gcc.S, same output as the C one, ~25 % less time; HELIX_ASMPOLY drops the C versions)
set(HELIX_ASM ${HELIX_DIR}/arm/asmpoly_gcc.S)
set_source_files_properties(${HELIX_SRC} PROPERTIES COMPILE_OPTIONS "-O2;-w;-DHELIX_ASMPOLY;-include;swilib.h")
set_source_files_properties(src/mp3s.c PROPERTIES COMPILE_OPTIONS "-O2")
set_source_files_properties(src/libc.c PROPERTIES COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns;-fno-builtin")
# ELKA drive methods (D67): the wrappers take 8 args but the firmware passes 4..7; a sibling (tail) call
# stores its stack args into the CALLER's frame - w_fclose zeroed the DRM handler's saved r4 (data abort
# A0094338 in drm_file_system). No tail calls in this file; RE-EL71.txt "D66 on the phone -> D67".
set_source_files_properties(src/vstream_nsg.c PROPERTIES COMPILE_OPTIONS "-fno-optimize-sibling-calls")

function(radio_target name net)
	add_executable(${name} ${RADIO_SRC} ${net} ${HELIX_SRC} ${HELIX_ASM} ${BEARSSL_SRC})
	target_sdk_setup(${name} SG)
	target_include_directories(${name} PRIVATE src ${HELIX_DIR}/pub ${HELIX_DIR}/real ${BEARSSL_DIR}/inc ${BEARSSL_DIR}/src
		"${CMAKE_CURRENT_BINARY_DIR}")
	target_compile_definitions(${name} PRIVATE __NO_LIBC ARM ${ARGN})
	target_compile_options(${name} BEFORE PUBLIC -std=gnu11 -Wall -Werror)
	target_link_libraries(${name} PRIVATE crt crt_helper gcc)
	add_dependencies(${name} bump_build_id)
endfunction()

radio_target(radio_SG src/net.c)
set_target_properties(radio_SG PROPERTIES OUTPUT_NAME "FasoleyRadio")

# elfloader3-linux (elfloader3-sg): no firmware player (a timer takes the PCM at the real rate) and no
# sockets (src/net_emu.c serves the captured streams from files); tests/emu/run.sh
radio_target(radio_EMU src/net_emu.c RADIO_EMU)
set_target_properties(radio_EMU PROPERTIES OUTPUT_NAME "FasoleyRadio_EMU")

function(radio_elka_target name net)
	add_executable(${name} ${RADIO_ELKA_SRC} ${net} ${BEARSSL_SRC})
	target_sdk_setup(${name} ELKA)
	target_include_directories(${name} PRIVATE src ${BEARSSL_DIR}/inc ${BEARSSL_DIR}/src "${CMAKE_CURRENT_BINARY_DIR}")
	target_compile_definitions(${name} PRIVATE __NO_LIBC RADIO_PASS ${ARGN})
	target_compile_options(${name} BEFORE PUBLIC -std=gnu11 -Wall -Werror)
	target_link_libraries(${name} PRIVATE crt crt_helper gcc)
	add_dependencies(${name} bump_build_id)
endfunction()

radio_elka_target(radio_ELKA src/net_elka.c)
set_target_properties(radio_ELKA PROPERTIES OUTPUT_NAME "FasoleyRadio_ELKA")
# elfloader3-linux (ELKA, 240x320): src/net_emu.c + a timer that takes the MP3 at the real rate
radio_elka_target(radio_ELKA_EMU src/net_emu.c RADIO_EMU)
set_target_properties(radio_ELKA_EMU PROPERTIES OUTPUT_NAME "FasoleyRadio_ELKA_EMU")

set(BUILD_ID_HEADER "${CMAKE_CURRENT_BINARY_DIR}/build_id.h")
add_custom_target(bump_build_id ALL
	BYPRODUCTS "${BUILD_ID_HEADER}"
	COMMAND "${CMAKE_COMMAND}"
		"-DNUMFILE=${CMAKE_CURRENT_LIST_DIR}/.buildnum"
		"-DOUT=${BUILD_ID_HEADER}"
		-P "${CMAKE_CURRENT_LIST_DIR}/bump_build_id.cmake"
	VERBATIM
)
