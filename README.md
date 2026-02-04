# Sharpie
This is the repository for the Sharpie project, which intends to drive
a color Sharp memory-in-pixel (MiP) display using only the PIO
hardware in the RP2350 microcontroller. For in-depth documentation and
discussion, see the relevant blog posts [on my
website](https://cryothene.neocities.org).

<img src="sharpie-pictures/sharpie-handheld.jpg" width=500>

## Repo contents
- `sharpie-cad`: a little bit of FreeCAD for a display/board carrier
- `sharpie-formatter`: an image formatter written in Rust that can
  dither and format images to binary data, ready to be written
  directly to the display
- `sharpie-hw`: the original revision of the hardware, with an extra
  voltage regulator and the display connector wired backward
- `sharpie-hw-rev2`: the second revision of the hardware, with that
  extra regulator removed and the display connector wired correctly
  (this is the revision that works)
- `sharpie-pictures`: assorted pictures of the project
- `sharpie-rp2040`: initial proof-of-concept for the PIO display
  interface, running on an RP2040 instead of an RP2350.
- `sharpie-sw`: the current demo software for a Sharpie rev2 board,
  and a nearly perfect **reference implementation** for using this
  work with a display
- `sharpie-usb-display`: an actually useful demo, which streams 4:3
  video over USB to a Sharpie board
- `vdp-simulator`: some proof-of-concept Rust code for how you could
  make a Sega-inspired tile-based video display processor in software
