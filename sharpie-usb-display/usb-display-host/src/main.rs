use std::sync::mpsc;
use std::thread;
use std::time::Duration;

use rusb;
use zstd;
use anyhow::Error;
// these seem to be standard abbreviations
use gstreamer_app as gst_app;
use gstreamer as gst;
use gstreamer::prelude::*;
use glib;

const FRAMESIZE: usize = 240*320;
// remember: USB endpoint names are relative to the host
const SHARPIE_EP_OUT: u8 = 0x01;
const SHARPIE_VID: u16 = 0x2e8a;
const SHARPIE_PID: u16 = 0xa1b1;

// for use with 8-bits-per-color data. it might seem excessive to use
// an i32, but it makes sure that anything we want to do will never overflow
#[derive(Copy, Clone, Debug)]
struct RgbPixel {
    red: i32,
    green: i32,
    blue: i32
}

#[allow(dead_code)]
impl RgbPixel {
    fn new() -> RgbPixel {
        RgbPixel {
            red: 0,
            green: 0,
            blue: 0
        }
    }

    /// create a RgbPixel from a ABGR u32 pixel
    fn from_abgr32_pixel(p: u32) -> RgbPixel {
        RgbPixel {
            red: (p & 0xff) as i32,
            green: ((p >> 8) & 0xff) as i32,
            blue: ((p >> 16) & 0xff) as i32,
        }
    }

    /// create a RgbPixel from RGBA u8 values, but we don't actually
    /// use the alpha channel.
    fn from_rgba_u8s(r: u8, g: u8, b: u8, _a: u8) -> RgbPixel {
        RgbPixel {
            red: r as i32,
            green: g as i32,
            blue: b as i32,
        }
    }

    /// convert this RgbPixel to 2bpc color, using just the lowest 2
    /// bits of every field. the array is in RGB order.
    fn to_2bpc_color_array(&self) -> [u8; 3] {
        // assuming you made the RgbPixel from the functions
        // above, there's no need to AND with 0xff.
        [
            (self.red >> 6) as u8,
            (self.green >> 6) as u8,
            (self.blue >> 6) as u8
        ]
    }

    fn color_2bpc_to_8bpc(color_2bit: u8) -> i32 {
        match color_2bit {
	    0b00 => 0,
	    0b01 => 85,
	    0b10 => 170,
	    0b11 => 255,
	    _ => 255,
        }
    }
    
    /// convert this RgbPixel to 2bpc color, but converted back to
    /// full range RGB.
    fn to_2bpc_color_full_range(&self) -> RgbPixel {
        let this_pixel_2bpc = self.to_2bpc_color_array();
        RgbPixel {
            red: RgbPixel::color_2bpc_to_8bpc(this_pixel_2bpc[0]),
            green: RgbPixel::color_2bpc_to_8bpc(this_pixel_2bpc[1]),
            blue: RgbPixel::color_2bpc_to_8bpc(this_pixel_2bpc[2]),
        }
    }

    /// convert this RgbPixel to a 6bpp (2bpc, 6 total bits used) pixel
    fn to_6bpp_pixel(&self) -> u8 {
        let px = self.to_2bpc_color_array();
        px[0] | (px[1] << 2) | (px[2] << 4)
    }
    
    fn to_u8_slice(&self) -> [u8; 3] {
        [self.red as u8, self.green as u8, self.blue as u8]
    }

    
}

fn main() -> Result<(), Error> {
    /*let args: Vec<String> = env::args().collect();
    let frame = fs::read(args[1].clone()).unwrap();
    //println!("{}", frame.len());
    let mut compressed = compress(&frame);
    // append the length of the compressed data (0..0 just inserts)
    compressed.splice(0..0, u32::to_le_bytes(compressed.len() as u32));
    //fs::write("frame.lz4", &compressed).unwrap();

    println!("{}", compressed.len());
    // this works but it isn't right yet
    
    // we know we want endpoint 0x01
    device.write_bulk(0x01, &compressed, Duration::from_millis(1000)).unwrap();
     */

    gst::init()?;

    // start by trying to open the device
    let sharpie_usb = rusb::open_device_with_vid_pid(SHARPIE_VID, SHARPIE_PID)
        .expect("Failed to open Sharpie USB device!");
    
    let main_loop = glib::MainLoop::new(None, false);
    // uridecodebin3 works, uridecodebin doesn't. we're using a string
    // launcher instead of manual pipeline assembly because
    // uridecodebin3 doesn't initially know what its capabilities or
    // pads are (it has to parse the file).

    // emit-signals=True is necessary

    // GStreamer docs say that normal bins don't have a bus or handle
    // clock distribution on their own, but a Pipeline does. thus,
    // just a bin runs at the wrong speed (getting data as fast as it
    // can), but a pipeline runs at the correct speed. the problem is
    // that launch() returns an Element, which can't be searched,
    // while bin_from_description() returns a Bin, which can be
    // searched. the easy solution is to make a Bin from a pipeline
    // description, then add that to a real Pipeline, and run the
    // Pipeline.
    //
    // now, one might reasonably ask why it's that simple, given that
    // the Pipeline has to link clocks and other things to the Bin. I
    // don't know why, but I know it works.

    // videoflip needs to come first
    let launched_bin  = gst::parse::bin_from_description("uridecodebin3 uri=file:///home/liam/rei_ii.mp4 ! videoflip method=clockwise ! videoconvert ! videorate ! videoscale ! video/x-raw,width=240,height=320,framerate=18/1,format=RGBA ! appsink name=sink emit-signals=True",
        // the bool here is to "automatically create ghost pads for
        // unlinked pads". it also seems to make the pipeline work?
        false
    )?;

    let pipeline = gst::Pipeline::new();
    pipeline.add(&launched_bin)?;
    // get access to the appsink so we can listen for its signals
    let appsink = pipeline.by_name("sink").unwrap();


    

    let (tx, rx) = mpsc::channel();
    let mut count = 0;
    // have to move the rx handle and the device
    thread::spawn(move || {

        loop {
            // if the other thread dies (because the GStreamer main
            // loop exists), the receiver here loses its link and
            // throws an error.
            let received: Result<Vec<RgbPixel>, _> = rx.recv();
            if let Ok(frame_rgbpixel) = received {
                //let start = SystemTime::now();
                
                let dithered = floyd_steinberg_dither(&frame_rgbpixel);
                let formatted = format_image(&dithered);
                // default compression level (convert to slice)
                let mut compressed = zstd::encode_all(&formatted[..], 6).unwrap();
                /*let end = SystemTime::now();
                
                let duration = end.duration_since(start).unwrap();*/

                // append the length of the compressed data to the
                // start as a little-endian u32. zstd includes the
                // decompressed length in its frame format but Sharpie
                // needs to know how much to read on the fly.
                compressed.splice(0..0, u32::to_le_bytes(compressed.len() as u32));
                sharpie_usb.write_bulk(
                    SHARPIE_EP_OUT,
                    &compressed,
                    // 1000 ms timeout is plenty
                    Duration::from_millis(1000)).unwrap();
                println!("wrote frame {}, size = {}", count, compressed.len());
                count += 1;
            } else {
                println!("main loop disconnected");
                // leave the thread
                break;
            }
            
        }
    });
    
    appsink.connect("new-sample",
        true, // "after"
        move |arg| {
            
            let aps = arg[0].get::<gst_app::AppSink>().unwrap();
            let sample = aps.pull_sample().unwrap();
            let buffer = sample.buffer().unwrap();
            let map = buffer.map_readable().unwrap();

            // the data in the vec is a 240x320 RGBA image (4 bytes
            // per pixel in RGBA order), but it's in bytes, which
            // makes iteration a lot harder. let's convert it to
            // something really convenient, which will also be on the
            // heap to make sending easier. we do this here because
            // the map is local to this closure.

            // this takes like 3 ms in debug mode and ~400 us in release
            let data = map.as_slice();
            let mut frame_rgbpixel: Vec<RgbPixel> =
                Vec::with_capacity(FRAMESIZE);

            for i in (0..data.len()).step_by(4) {
                frame_rgbpixel.push(RgbPixel::from_rgba_u8s(
                    data[i], data[i+1], data[i+2], data[i+3]));
            }
            
            tx.send(frame_rgbpixel).unwrap();
            
            
            Some(gst::FlowReturn::Ok.into())
        }
    );

    // we do actually want a bus watch, so that we can stop the
    // mainloop on EOS and also print errors if we get them, but it
    // can wait.
    let bus = pipeline.bus().unwrap();
    let main_loop_clone = main_loop.clone();
    
    let _bus_watch = bus
        .add_watch(move |_, msg| {
            use gst::MessageView;

            let main_loop = &main_loop_clone;
            match msg.view() {
                MessageView::Eos(..) => main_loop.quit(),
                MessageView::Error(err) => {
                    println!(
                        "Error from {:?}: {} ({:?})",
                        err.src().map(|s| s.path_string()),
                        err.error(),
                        err.debug()
                    );
                    main_loop.quit();
                }
                _ => (),
            };

            glib::ControlFlow::Continue
        })
        .expect("Failed to add bus watch");

    
    pipeline.set_state(gst::State::Playing)?;

    main_loop.run();

    println!("after mainloop");
    pipeline.set_state(gst::State::Null)?;
    
    Ok(())
}

// this is an adaptation of the dithering code in sharpie-formatter to
// work with straight ABGR u32s and assume that images are 240x320.


fn rgb8_quant_error(pixel: RgbPixel, difference: [i32; 3], quant_num: i32) -> RgbPixel {
    // we have to clamp overflows to 255 with a saturating signed add,
    // otherwise channels that saturate will underflow, causing (for
    // example) some bright white pixels in the original image to
    // appear as a full blue pixel in the dithered output. the max
    // difference is +/- 63 and quant_num is always less than 16, so
    // we can safely cast to i8 without losing anything.
    
    let pixel_as_u8 = pixel.to_u8_slice();

    RgbPixel {
        red: pixel_as_u8[0].saturating_add_signed((difference[0] * quant_num/16) as i8) as i32,
        green: pixel_as_u8[1].saturating_add_signed((difference[1] * quant_num/16) as i8) as i32,
        blue: pixel_as_u8[2].saturating_add_signed((difference[2] * quant_num/16) as i8) as i32,
    }
    

    
}

// this function would appear to take the bulk of the time during a
// frame. the dithering itself also looks slightly different from the
// manual (ffmpeg->shell scripts/sharpie-formatter) frames, but that
// might be an effect of GStreamer's decoding.

/// Floyd-Steinberg dither an image (a 240x320 array of RgbPixels) to
/// another array of the same type.
fn floyd_steinberg_dither(input: &[RgbPixel]) -> Vec<RgbPixel> {
    let mut dithered_image: Vec<RgbPixel> = input.into();
    
    for y in 0..320 {
	for x in 0..240 {
	    // for each pixel, get its color channels, then convert
	    // it to 2bpc and calculate the quantization error.
	    let pixel = dithered_image[y*240 + x];
	    let pixel_color_reduced = pixel.to_2bpc_color_full_range();
	    // make an 8bpc version of the new pixel to put back in
	    // the original image, where we keep the initial state and
	    // diffused error (which gets transformed into a dithered
	    // image in 8bpc format)
	    
	    // RgbPixel components are i32, so there's no risk of
	    // overflow on numbers that are 255 at most
	    let quant_error: [i32; 3] = [
                pixel.red - pixel_color_reduced.red,
                pixel.green - pixel_color_reduced.green,
                pixel.blue - pixel_color_reduced.blue
            ];
            
	    // put the quantized pixel back into the original image
	    dithered_image[y*240 + x] = pixel_color_reduced;

	    // and then diffuse the error over adjacent pixels
	    
	    // if we're in a position that would write to pixels outside the image,
	    // just don't write them.
	    if x + 1 < 240 {
		dithered_image[y * 240 + (x + 1)] = 
		    rgb8_quant_error(dithered_image[y * 240 + (x + 1)],
				     quant_error, 7);
	    }

	    if x != 0 && y + 1 < 320 {
                dithered_image[(y + 1)*240 + (x - 1)] = 
		    rgb8_quant_error(dithered_image[(y + 1) * 240 + (x - 1)],
				     quant_error, 3);
	    }

	    if y + 1 < 320 {
                dithered_image[(y + 1)*240 + x] = 
		    rgb8_quant_error(dithered_image[(y + 1) * 240 + x],
				     quant_error, 5);
	    }

	    if x + 1 < 240 && y + 1 < 320 {
                dithered_image[(y + 1)*240 + (x + 1)] = 
		    rgb8_quant_error(dithered_image[(y + 1) * 240 + (x + 1)],
				     quant_error, 1);
	    }
	}

    }

    dithered_image
}

// this is code directly from sharpie-formatter, which probably means
// it should be an importable crate

/// Convert two pixels in 0bBBGGRR format to their respective MSb and
/// LSb bytes.
fn two_pixels_to_msb_lsb(p1: u8, p2: u8) -> (u8, u8) {
    // apply a linear mapping from 24-bit color to 6-bit color
    let p1red = p1 & 0b11;
    let p1green = (p1 & 0b1100) >> 2;
    let p1blue = (p1 & 0b110000) >> 4;
    let p2red = p2 & 0b11;
    let p2green = (p2 & 0b1100) >> 2;
    let p2blue = (p2 & 0b110000) >> 4;

    // get the MSbs of both pixels, each of these values is 0
    // or 1
    let p1red_m = p1red >> 1;
    let p1green_m = p1green >> 1;
    let p1blue_m = p1blue >> 1;
    let p2red_m = p2red >> 1;
    let p2green_m = p2green >> 1;
    let p2blue_m = p2blue >> 1;

    // assemble the MSb byte: the second pixel goes in the higher
    // position while the first goes in the lower (this was wrong
    // before)
    let msb = (p2blue_m << 5) | (p2green_m << 3) | (p2red_m << 1)
	| (p1blue_m << 4) | (p1green_m << 2) | (p1red_m);
    
    // now find LSBs from both pixels
    let p1red_l = p1red & 1;
    let p1green_l = p1green & 1;
    let p1blue_l = p1blue & 1;
    let p2red_l = p2red & 1;
    let p2green_l = p2green & 1;
    let p2blue_l = p2blue & 1;
    
    // and write to formatted array as LSBs in 0bB1B2G1G2R1R2
    let lsb = (p2blue_l << 5) | (p2green_l << 3) | (p2red_l << 1)
	| (p1blue_l << 4) | (p1green_l << 2) | (p1red_l);

    (msb, lsb)
}

// this is taken from sharpie-formatter but lightly adjusted to work
// with a flat slice of u32s in ABGR format (generated from the
// callback above)
fn format_image(img: &[RgbPixel]) -> [u8; FRAMESIZE] {
    // formatted image buffer
    let mut formatted: [u8; FRAMESIZE] = [0u8; FRAMESIZE];
    
    for y in 0..320 {
        // take the input two pixels at a time
	for x in 0..120 {
	    //println!("y: {}, x: {}, formatted_index: {}", y, x, formatted_index);
            
	    // get pixels
            let p1 = &img[y*240 + x*2];
            let p2 = &img[y*240 + x*2 + 1];

            let p1_2bpc = p1.to_6bpp_pixel();
            let p2_2bpc = p2.to_6bpp_pixel();
            
            // names: byte of most significant bits of colors, byte of
            // least significant bits of colors
	    let (msb_byte, lsb_byte) = two_pixels_to_msb_lsb(p1_2bpc, p2_2bpc);

            formatted[y * 240 + x] = msb_byte;
            formatted[y * 240 + x + 120] = lsb_byte;
	}
    }
    formatted
}
