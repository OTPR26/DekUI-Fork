# cmake -DINPUT=<pack> -DOUTPUT=<list> -P spirv_pack_to_inc.cmake
# The bytes of a SPIR-V pack as a C initializer list, 32 per line, for
# compilers without #embed.
file(READ "${INPUT}" hex HEX)
string(REGEX REPLACE "(................................................................)" "\\1\n" hex "${hex}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUTPUT}.tmp" "${bytes}\n")
file(RENAME "${OUTPUT}.tmp" "${OUTPUT}")
