use std::fs;


use error_iter::ErrorIter as _;
use log::error;
use pixels::{Error, Pixels, SurfaceTexture};
use winit::dpi::LogicalSize;
use winit::event::{Event, WindowEvent};
use winit::event_loop::EventLoop;
use winit::keyboard::KeyCode;
use winit::window::WindowBuilder;
use winit_input_helper::WinitInputHelper;

const DISPLAY_WIDTH: u32 = 320;
const DISPLAY_HEIGHT: u32 = 240;
const VRAM_SIZE_BYTES: usize = 98304; /// 96 KB of VRAM total

enum TilemapMode {
    TilemapMode320x240,
    TilemapMode512x512,
}

struct VDP {
    vram: [u8; VRAM_SIZE_BYTES], 
    tilemap_0_mode: TilemapMode,
    tilemap_0_scroll_x: u32,
    tilemap_0_scroll_y: u32,
    tilemap_0_contents: [[u16; 64]; 64]
}

fn main() {
    let event_loop = EventLoop::new().unwrap();
    let mut input = WinitInputHelper::new();
    let window = {
        let size = LogicalSize::new(DISPLAY_WIDTH as f64, DISPLAY_HEIGHT as f64);
        WindowBuilder::new()
            .with_title("VDP Simulator")
            .with_inner_size(size)
            .with_min_inner_size(size)
            .build(&event_loop)
            .unwrap()
    };

    let mut pixels = {
        let window_size = window.inner_size();
        let surface_texture = SurfaceTexture::new(window_size.width, window_size.height, &window);
        Pixels::new(DISPLAY_WIDTH, DISPLAY_HEIGHT, surface_texture).unwrap()
    };
    let mut vdp = VDP::new();
    
    // load the test tile into vram at tile position 0
    let contents = fs::read("test_tile.bin").unwrap();

    let mut i = 0;
    for byte in contents {
	vdp.vram[i] = byte;
	i += 1;
    }

    // now set the tilemap to be 512x512 pixels (64x64 tiles) and set
    // all tiles to index 0
    vdp.tilemap_0_mode = TilemapMode::TilemapMode512x512;
    for x in 0..64 {
	for y in 0..64 {
	    vdp.tilemap_0_contents[y][x] = 0;
	}
    }
    vdp.tilemap_0_scroll_x = 4;
    vdp.tilemap_0_scroll_y = 1;
    vdp.render(pixels.frame_mut());
    let res = event_loop.run(|event, elwt| {
        // Draw the current frame
        if let Event::WindowEvent {
            event: WindowEvent::RedrawRequested,
            ..
        } = event
        {

            if let Err(err) = pixels.render() {
                log_error("pixels.render", err);
                elwt.exit();
                return;
            }
        }

        // Handle input events
        if input.update(&event) {
            // Close events
            if input.key_pressed(KeyCode::Escape) || input.close_requested() {
                elwt.exit();
                return;
            }

            // Resize the window
            if let Some(size) = input.window_resized() {
                if let Err(err) = pixels.resize_surface(size.width, size.height) {
                    log_error("pixels.resize_surface", err);
                    elwt.exit();
                    return;
                }
            }

            // Update internal state and request a redraw
            window.request_redraw();
        }
    });
    //res.map_err(|e| Error::UserDefined(Box::new(e)))
}

impl VDP {
    /// Initialize a new simulated VDP
    pub fn new() -> Self {
        Self {
	    vram: [0u8; VRAM_SIZE_BYTES],
	    tilemap_0_mode: TilemapMode::TilemapMode320x240,
            tilemap_0_scroll_x: 0,
	    tilemap_0_scroll_y: 0,
	    tilemap_0_contents: [[0u16; 64]; 64]
        }
    }


    pub fn render(&self, framebuffer: &mut [u8]) {
	// the first and last rows are likely to not be on a tile
	// boundary, so we have to compute them separately.

	// there are always 40 tiles by 30 tiles on the screen, but
	// depending on the scroll values they can be aligned
	// differently. if the scroll cuts halfway through a tile,
	// then there's four pixels on the left and four pixels on the
	// right.

	// these values indicate what tile in the tilemap the
	// display's upper left corner sits on. 
	let upper_left_tilemap_tile_x = self.tilemap_0_scroll_x / 8;
	let upper_left_tilemap_tile_y = self.tilemap_0_scroll_y / 8;
	// these two values indicate how many pixels in from the edge
	// of the tiles calculated above the display sits.
	let display_tile_pixel_offset_x = self.tilemap_0_scroll_x % 8;
	let display_tile_pixel_offset_y = self.tilemap_0_scroll_y % 8;

	let mut pixel_columns_rendered = 0;
	let mut pixel_rows_rendered = 0;

	let mut current_tilemap_tile_x = upper_left_tilemap_tile_x;
	let mut current_tilemap_tile_y = upper_left_tilemap_tile_y;
	let mut current_tilemap_tile = self.tilemap_0_contents[current_tilemap_tile_y as usize][current_tilemap_tile_x as usize];
	
	let mut current_tile_pixel_x = display_tile_pixel_offset_x;
	let mut current_tile_pixel_y = display_tile_pixel_offset_y;
	let mut pixels_pushed = 0;
	//let mut current_tilemap_pixel_x = 
	while pixel_rows_rendered < 240 {
	    pixel_columns_rendered = 0;
	    while pixel_columns_rendered < 320 {
		let mut tile: [u8; 64] = [0u8; 64];
		let vram_offset: usize = current_tilemap_tile as usize * 64 as usize;
		
		tile.copy_from_slice(&self.vram[vram_offset..vram_offset+64]);

		// now that the current tile has been copied (this is
		// a process which can and should be optimized away
		// when possible in the final implementation), we can
		// start copying pixels out of it and into the framebuffer.

		// get the current pixel and copy it
		let pixel = tile[(current_tile_pixel_y * 8 + current_tile_pixel_x) as usize];
		let rgba = VDP::px_to_rgba(pixel);

		let fb_offset = (pixel_rows_rendered * DISPLAY_WIDTH + pixel_columns_rendered) as usize * 4;
		framebuffer[fb_offset] = rgba.0;
		framebuffer[fb_offset + 1] = rgba.1;
		framebuffer[fb_offset + 2] = rgba.2;
		framebuffer[fb_offset + 3] = rgba.3;
		pixels_pushed += 1;
		
		println!("accessing tile {current_tilemap_tile}, grabbing pixel (x,y): ({current_tile_pixel_x}, {current_tile_pixel_y})");
		println!("  write ({}, {}, {}, {}) to framebuffer pixel ({pixel_columns_rendered},{pixel_rows_rendered}, array loc {fb_offset})", rgba.0, rgba.1, rgba.2, rgba.3);
		current_tile_pixel_x += 1;
		pixel_columns_rendered += 1;
		if current_tile_pixel_x == 8 {
		    // we finished this tile, set the next tilemap tile
		    current_tile_pixel_x = 0; // reset the x address within this tile
		    current_tilemap_tile_x += 1; // advance to the next tile in the tilemap
		    current_tilemap_tile = self.tilemap_0_contents[current_tilemap_tile_y as usize][current_tilemap_tile_x as usize]; // and fetch the next tile ID
		}
	    }
	    
	    pixel_rows_rendered += 1;
	    current_tilemap_tile_x = 0;
	    current_tile_pixel_y += 1;
	    if current_tile_pixel_y == 8 {
		current_tile_pixel_y = 0;
		current_tilemap_tile_y += 1;
		current_tilemap_tile = self.tilemap_0_contents[current_tilemap_tile_y as usize][current_tilemap_tile_x as usize]; // and fetch the next tile ID
	    }
	}

	println!("{pixels_pushed} pixels pushed");
    }

    pub fn render2(&self, framebuffer: &mut [u8]) {
	for pixel in framebuffer.chunks_exact_mut(4) {
	    let color: [u8; 4] = [0xab, 0x00, 0xab, 0xff];
	    pixel.copy_from_slice(&color);
	}
    }
    fn px_to_rgba(px: u8) -> (u8, u8, u8, u8) {
	// expand RGB back to full 8-bit from 2-bit, and if high bit
	// set, then full transparency.

	((px & 0b11) << 6, ((px & 0b1100) >> 2) << 6, ((px & 0b110000) >> 4) << 6, if px & 0b1000000 == 0 { 0xff } else { 0 })
    }

}


fn log_error<E: std::error::Error + 'static>(method_name: &str, err: E) {
    error!("{method_name}() failed: {err}");
    for source in err.sources().skip(1) {
        error!("  Caused by: {source}");
    }
}
