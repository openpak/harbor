# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only
#
# cmake -DINPUT=<file> -DOUTPUT=<cpp> -DSYMBOL=<name> -P embed.cmake
# Writes a C++ source defining `const unsigned char <SYMBOL>[]` and `const std::size_t <SYMBOL>_size`.

file(READ "${INPUT}" bytes HEX)
string(LENGTH "${bytes}" hex_length)
math(EXPR size "${hex_length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${bytes}")
string(REGEX REPLACE "(0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],)" "\\1\n" bytes "${bytes}")
file(WRITE "${OUTPUT}.tmp"
    "// Generated from ${INPUT}\n#include <cstddef>\n"
    "extern const unsigned char ${SYMBOL}[];\nextern const std::size_t ${SYMBOL}_size;\n"
    "alignas(16) const unsigned char ${SYMBOL}[] = {\n${bytes}};\n"
    "const std::size_t ${SYMBOL}_size = ${size};\n")
file(RENAME "${OUTPUT}.tmp" "${OUTPUT}")
