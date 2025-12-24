# Sharpie
This is the repository for the Sharpie project, which intends to drive
a color Sharp memory display using only the PIO hardware in the RP2350
microcontroller. For in-depth documentation and discussion, see the
relevant blog posts [https://cryothene.neocities.org](on my website).

<img src="sharpie-pictures/sharpie-handheld.jpg" width=300>

## Repo contents
- `sharpie-cad`: a little bit of FreeCAD for a display/board carrier
- `sharpie-formatter`: an image formatter written in Rust that can do
  Floyd-Steinberg dithering and format images to a format ready to
  be written to the display
- `sharpie-hw`: the original revision of the hardware, with an extra
  voltage regulator and the display connector wired backward
- `sharpie-hw-rev2`: the second revision of the hardware, with that
  extra regulator removed and the display connector wired correctly
  (this is the revision that works)
- `sharpie-pictures`: assorted pictures of the project
- `sharpie-rp2040`: initial proof-of-concept for the PIO display
  interface, running on an RP2040 instead of an RP2350.
- `sharpie-sw`: the current demo software for a Sharpie rev2 board
- `vdp-simulator`: some proof-of-concept Rust code for how you could
  make a Sega-inspired tile-based video display processor in software
