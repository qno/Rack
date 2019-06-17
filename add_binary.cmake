# References binary files compiled into the program.
# see - https://github.com/VCVRack/Rack/blob/bc06efd9ab14e48684f32e608b13a76ecb7367bd/include/common.hpp#L109
#
# This replaces the approach to embedd binary files with objcopy and xxd, see definition beginning at
# https://github.com/VCVRack/Rack/blob/bc06efd9ab14e48684f32e608b13a76ecb7367bd/compile.mk#L74
#
# Usage example in CMakeList.txt:
#
# include(add_binary)
# add_binary(binaryfile.bin BINARY_ASM_OUTPUT)
#
# Then add the BINARY_ASM_OUTPUT variable to the sources of the library target.
#
# This CMake macro embedds the content of a binary file into an assembler file.
#
# param in:  BINARY_INPUT - path to the binary file to reference (needs an file extension, e.g. mybinary.bin)
# param out: ASM_OUTPUT   - variable that holds the path to the generated assembler file
macro(ADD_BINARY BINARY_INPUT ASM_OUTPUT)

  set(BINARY_INPUT_FILE ${PROJECT_SOURCE_DIR}/${BINARY_INPUT})

  if (NOT EXISTS ${BINARY_INPUT_FILE})
    message (FATAL_ERROR "ADD_BINARY - BINARY_INPUT file '${BINARY_INPUT_FILE}' doesn't exist")
  endif ()

  message (STATUS "ADD_BINARY - BINARY_INPUT: ${BINARY_INPUT_FILE}")

  get_filename_component(BINARY_INPUT_EXT ${BINARY_INPUT_FILE} EXT)
  get_filename_component(BINARY_INPUT_NAME_WE ${BINARY_INPUT_FILE} NAME_WE)
  string(REPLACE "." "" BINARY_INPUT_EXT ${BINARY_INPUT_EXT})

  message(STATUS "ADD_BINARY - BINARY_INPUT name: ${BINARY_INPUT_NAME_WE}")
  message(STATUS "ADD_BINARY - BINARY_INPUT extension: ${BINARY_INPUT_EXT}")

  set (ASM_GENERATED_FILE ${PROJECT_BINARY_DIR}/${BINARY_INPUT_NAME_WE}.asm)
  set(${ASM_OUTPUT} ${ASM_GENERATED_FILE})
  message(STATUS "ADD_BINARY - ASM_OUTPUT: ${ASM_OUTPUT}")
  message(STATUS "ADD_BINARY - ASM_GENERATED_FILE: ${ASM_GENERATED_FILE}")

  set (BINARY_IDENTIFIER ${BINARY_INPUT_NAME_WE}_${BINARY_INPUT_EXT})
  message(STATUS "ADD_BINARY - BINARY_IDENTIFIER: ${BINARY_IDENTIFIER}")

  file(GENERATE OUTPUT ${ASM_GENERATED_FILE} CONTENT
"bits 64
section .rodata

global _binary_src_${BINARY_IDENTIFIER}_start
global _binary_src_${BINARY_IDENTIFIER}_end
global _binary_src_${BINARY_IDENTIFIER}_size

_binary_src_${BINARY_IDENTIFIER}_start:   incbin \"${BINARY_INPUT_FILE}\"
_binary_src_${BINARY_IDENTIFIER}_end:
_binary_src_${BINARY_IDENTIFIER}_size:    dd $-_binary_src_${BINARY_IDENTIFIER}_start")

endmacro()
