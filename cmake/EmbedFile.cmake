# Script mode: cmake -DINPUT=<file> -DOUTPUT=<file.c> -DSYMBOL=<name> -P EmbedFile.cmake
# Writes a C source defining `const unsigned char SYMBOL[]` and `const size_t SYMBOL_len`.
# The data is NUL-terminated (not counted in _len) so text files can be used as C strings.
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_len)
math(EXPR byte_len "${hex_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x..,){32})" "\\1\n  " bytes "${bytes}")
file(WRITE "${OUTPUT}"
  "/* Generated from ${INPUT} - do not edit. */\n"
  "#include <stddef.h>\n"
  "const unsigned char ${SYMBOL}[] = {\n  ${bytes}0x00\n};\n"
  "const size_t ${SYMBOL}_len = ${byte_len};\n")
