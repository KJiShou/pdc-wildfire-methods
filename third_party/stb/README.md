# Image decoder dependency

Windows builds use Windows Imaging Component for PNG/BMP/JPEG input, while the
portable branch uses the vendored upstream `stb_image.h`. `stb_image_write.h`
is retained for a future PNG preview writer; the first vertical slice writes
decoded previews as BMP directly.
