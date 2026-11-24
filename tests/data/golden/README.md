# Golden images

Frames the renderer drew with D3D12 on WARP, looked at by eye and kept as
what the golden-image tests compare against (`tests/render/*_d3d12_test.c`).
Each is a 32-bit BMP, rows from the top, written by the test itself.

To make or update one, run its test: a frame with no golden image, or one
that differs from it, is written beside the tests as `<name>.actual.bmp`.
Look at it, then copy it here as `<name>.bmp`. See
[testing](../../../docs/testing.md#golden-images).
