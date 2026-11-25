cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pynq_z1_hello)

target_sources(app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_SMP app PRIVATE src/zynq7000_smp.c)
if(NOT CONFIG_SMP)
	target_sources(app PRIVATE
		src/cpu1_cmd.c
		src/cpu1_trampoline.S
	)
endif()
