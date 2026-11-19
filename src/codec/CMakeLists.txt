# Codecs: checksums, and later compression and encodings.
set(TB_CODEC_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/base64.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/checksum.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/deflate.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/deflate_stream.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/inflate.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/inflate_stream.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tar_header.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tar_reader.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tar_writer.c"
    CACHE INTERNAL "Codec sources")
target_sources(badger PRIVATE ${TB_CODEC_SOURCES})
target_sources(badger_static PRIVATE ${TB_CODEC_SOURCES})
