# sharpie-usb-display
This is probably the ultimate Sharpie demo, a set of client and host
software to stream video in real time to Sharpie (and its display)
over USB. The host uses GStreamer to decode and convert a video,
then dithers and formats video frames before zstd-compressing them and
writing them to the client's USB endpoint. The client receives frames,
decompresses them, and puts them on the display.

This requires both cores on the RP2350. Instrumentation code is in
both source files if you're curious about how much time various steps
in the process require.

The host needs to be run with `--release` because the dithering is
slow (20-50 ms) in debug, but 4 ms in release.

# Notes
## Compression and transfer
lz4 is a good compression algorithm, but it doesn't get frames small
enough. zstd does, though, and it still runs plenty fast on the
RP2350.


## Video
I accidentally turned the system clock up to 200 MHz, and then I
realized that the display was still working even though the PIO and
PWM were running significantly faster. Running the PIO faster reduces
the time it takes to fill the display with new data, which helps
reduce screen tearing. Increasing the clock to 200 MHz means the
screen can theoretically run at ~24.8 fps, which beats the average
movie frame rate of 24 fps. The problem is that the USB link is
limited to about 22 fps, and increasing the frame rate also makes
flicker a little more visible (to my eyes, at least). From my testing,
the data link can always handle 21 fps, but no higher.

One change to potentially make in the future is some sort of smarter
dither algorithm that only dithers in changed regions, or something,
to reduce full-frame flicker. I think the reason that dither is so
visible is because the dithering causes every single frame to have
almost entirely different pixels. I'd also like to replace the
dithering with a SIMD implementation, and maybe these changes could
happen simultaneously.
