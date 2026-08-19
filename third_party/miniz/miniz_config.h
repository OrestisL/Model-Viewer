// Vendored miniz, inflate-only configuration for Model-Viewer.
//
// Only DEFLATE decompression (tinfl) is used, for decompressing gzip-wrapped
// SPZ Gaussian-splat files. All compression, ZIP-archive and stdio APIs are
// disabled so the vendored surface stays tiny and portable (no zlib dependency,
// which was fragile on Windows and conflicted with the USD/tinyusdz build).
#pragma once
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
