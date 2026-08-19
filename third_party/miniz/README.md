# miniz (vendored, inflate-only)

Source: https://github.com/richgel999/miniz
License: Unlicense / public domain (see the statement at the end of `miniz.h`).

Only the DEFLATE **decompression** path (`miniz_tinfl.c` + headers) is vendored,
used to decompress gzip-wrapped `.spz` Gaussian-splat files in
`src/scene/SplatLoader.cpp`. The gzip (RFC 1952) wrapper is parsed by hand there;
miniz performs the raw inflate.

Built as a small static library by the top-level CMake with all compression,
ZIP-archive and stdio APIs disabled (`MINIZ_NO_DEFLATE_APIS`,
`MINIZ_NO_ARCHIVE_APIS`, `MINIZ_NO_STDIO`, `MINIZ_NO_TIME`). This removes the
previous dependency on Assimp's bundled zlib, which did not export its headers
reliably on MSVC and conflicted with the USD/tinyusdz build.

Only `miniz_tinfl.c` is compiled; the other headers are present because
`miniz.h` includes them.
