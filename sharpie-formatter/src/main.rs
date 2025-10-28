use std::path::PathBuf;
use std::fs;

use image::{ImageReader, Pixel, ImageBuffer, Rgb, RgbImage};
use image::metadata::{CicpTransferCharacteristics};
use clap::{Parser, Subcommand};

#[derive(Subcommand, Debug)]
enum Commands {
    Format {
	input: PathBuf,
	output: PathBuf,
    },
    Unformat {
	input: PathBuf,
	output: PathBuf,
    },
    GenLuts {
	msb_output: PathBuf,
	lsb_output: PathBuf,
    },
    UnformatRaw {
	input: PathBuf,
	output: PathBuf,
    },
    Dither {
	input: PathBuf,
	output: PathBuf,
    },
    DitherFormat {
	input: PathBuf,
	output: PathBuf,
    }

    
}

#[derive(Parser, Debug)]
struct Args {
    #[command(subcommand)]
    command: Commands,
}

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


fn format_image(img: RgbImage) -> [u8; 240*320] {
    // formatted image buffer
    let mut formatted: [[u8; 240]; 320] = [[0; 240]; 320];
    for y in 0..320 {
	let mut formatted_index = 0;
	for x in (0..240).step_by(2) {
	    //println!("y: {}, x: {}, formatted_index: {}", y, x, formatted_index);
	    // get msb of color components from pixels 1 and 2
	    let p1 = img.get_pixel(x, y);
	    let p1_components = p1.channels();
	    let p2 = img.get_pixel(x+1, y);
	    let p2_components = p2.channels();
	    
	    // apply a linear mapping from 24-bit color to 6-bit color
	    // (this is not actually correct, the display has skew
	    // toward the MSB due to the way subpixels are arranged)
	    let p1red_2bit = p1_components[0] >> 6;
	    let p1green_2bit = p1_components[1] >> 6;
	    let p1blue_2bit = p1_components[2] >> 6;
	    let p2red_2bit = p2_components[0] >> 6;
	    let p2green_2bit = p2_components[1] >> 6;
	    let p2blue_2bit = p2_components[2] >> 6;

	    // reassemble pixels as 0bBBGGRR
	    let p1 = (p1blue_2bit << 4) | (p1green_2bit << 2) | (p1red_2bit);
	    let p2 = (p2blue_2bit << 4) | (p2green_2bit << 2) | (p2red_2bit);
	    let (msb, lsb) = two_pixels_to_msb_lsb(p1, p2);

	    formatted[y as usize][formatted_index as usize] = msb;

	    formatted[y as usize][(formatted_index + 120) as usize] = lsb;
	    
	    formatted_index += 1;
	}
    }

    let mut formatted_flattened: [u8; 320*240] = [0; 320*240];
    for (rowcount, row) in formatted.iter().enumerate() {
	for (colcount, px) in row.iter().enumerate() {
	    //println!("rowcount: {}, colcount: {}", rowcount, colcount);
	    formatted_flattened[rowcount*240 + colcount] = *px;
	}
    }
    
    formatted_flattened
}

fn unformat_image(input: PathBuf, output: PathBuf) {
    let formatted = fs::read(input).expect("Failed to read input file");

    if formatted.len() != 320*240 {
	panic!("Formatted data read from file is not {} bytes!", 320*240);
    }

    let mut imgbuf = ImageBuffer::new(240, 320);
    
    for row in 0..320 {
	let mut formatted_col = 0;
	for col in (0..240).step_by(2) {
	    let msb = formatted[row*240 + formatted_col];
	    let lsb = formatted[row*240 + (formatted_col+120)];
	    let p1_red = (msb & 0b10) | ((lsb & 0b10) >> 1);
	    let p1_green = ((msb & 0b1000) >> 2) | ((lsb & 0b1000) >> 3);
	    let p1_blue = ((msb & 0b100000) >> 4) | ((lsb & 0b100000) >> 5);

	    let p0_red = (msb & 0b1) << 1 | (lsb & 0b1);
	    let p0_green = (((msb & 0b100) >> 2) << 1) | ((lsb & 0b100) >> 2);
	    let p0_blue = ((msb & 0b10000) >> 3) | ((lsb & 0b10000) >> 4);
	    
	    let p0: [u8; 3] = [p0_red << 6, p0_green << 6, p0_blue << 6];
	    let p1: [u8; 3] = [p1_red << 6, p1_green << 6, p1_blue << 6];
	    
	    imgbuf.put_pixel(col, row as u32, Rgb(p0));
	    imgbuf.put_pixel(col+1, row as u32, Rgb(p1));
	    
	    formatted_col += 1;
	}
    }

    imgbuf.save(output).expect("Failed to write output file");
}

/// You can left-shift a 2-bit color into an 8-bit color, but you
/// lose the total dynamic range, because 0b11000000 is not full
/// brightness. This function maps the 4 possible values of a
/// 2-bit color into correctly ranged 8-bit colors.
///
/// We can tell that doing it with just a shift is wrong because the
/// dithered result looks mega weird.
fn color_2bit_to_8bit(color_2bit: u8) -> u8 {
    match color_2bit {
	0b00 => 0u8,
	0b01 => 85u8,
	0b10 => 170u8,
	0b11 => 255u8,
	_ => 255u8,
    }

}

/// Returns the 2bpc (2 bits per color) equivalent of an Rgb pixel and
/// the correctly scaled Rgb pixel that represents that 2bpp pixel.
fn rgb8_to_2bpc_pixel(input_pixel: &[u8]) -> [u8; 3] {
    [input_pixel[0] >> 6, input_pixel[1] >> 6, input_pixel[2] >> 6]
}


fn rgb8_quant_error(pixel: &[u8], difference: [i16; 3], quant_num: i16) -> Rgb<u8> {
    // we have to clamp overflows to 255 with a saturating signed add,
    // otherwise channels that saturate will underflow, causing (for
    // example) some bright white pixels in the original image to
    // appear as a full blue pixel in the dithered output. the max
    // difference is +/- 63 and quant_num is always less than 16, so
    // we can safely cast to i8 without losing anything.
    Rgb([pixel[0].saturating_add_signed((difference[0] * quant_num/16) as i8),
	 pixel[1].saturating_add_signed((difference[1] * quant_num/16) as i8),
	 pixel[2].saturating_add_signed((difference[2] * quant_num/16) as i8)])
}

/// Dither an image (specified in `input_file`) with Floyd-Steinberg
/// dithering. This returns an RgbImage which can then be formatted or
/// saved.
fn floyd_steinberg_dither(input: RgbImage) -> RgbImage {
    let mut img_buffer = input.clone();
    //img_buffer.copy_from_color_space(input, ConvertColorOptions::
    for y in 0..img_buffer.height() {
	for x in 0..img_buffer.width() {
	    // for each pixel, get its color channels, then convert
	    // it to 2bpc and calculate the quantization error.
	    let oldpixel = img_buffer.get_pixel(x, y);
	    let oldpixel_slice = oldpixel.channels();
	    let newpixel_2bpc = rgb8_to_2bpc_pixel(oldpixel_slice);
	    // make an 8bpc version of the new pixel to put back in
	    // the original image, where we keep the initial state and
	    // diffused error (which gets transformed into a dithered
	    // image in 8bpc format)
	    let newpixel_8bpc = Rgb([color_2bit_to_8bit(newpixel_2bpc[0]),
				     color_2bit_to_8bit(newpixel_2bpc[1]),
				     color_2bit_to_8bit(newpixel_2bpc[2])]);
	    
	    // we need to use a signed size bigger than u8 to hold the
	    // quantization error, but i16 is probably not the most
	    // optimized size to use here
	    let quant_error: [i16; 3] =
		[i16::from(oldpixel_slice[0]) - i16::from(newpixel_8bpc[0]),
		 i16::from(oldpixel_slice[1]) - i16::from(newpixel_8bpc[1]),
		 i16::from(oldpixel_slice[2]) - i16::from(newpixel_8bpc[2])];

	    // put the quantized pixel back into the original image
	    img_buffer.put_pixel(x, y, newpixel_8bpc);

	    // and then diffuse the error over adjacent pixels
	    
	    // if we're in a position that would write to pixels outside the image,
	    // just don't write them.
	    if x + 1 < img_buffer.width() {
		img_buffer.put_pixel(
		    x + 1,
		    y,
		    rgb8_quant_error(img_buffer.get_pixel(x + 1, y).channels(),
				     quant_error, 7));
	    }

	    if x != 0 && y + 1 < img_buffer.height() {
		img_buffer.put_pixel(
		    x - 1,
		    y + 1,
		    rgb8_quant_error(img_buffer.get_pixel(x - 1, y + 1).channels(),
				     quant_error, 3));
	    }

	    if y + 1 < img_buffer.height() {
		img_buffer.put_pixel(
		    x,
		    y + 1,
		    rgb8_quant_error(img_buffer.get_pixel(x, y + 1).channels(),
				     quant_error, 5));
	    }

	    if x + 1 < img_buffer.width() && y + 1 < img_buffer.height() {
		img_buffer.put_pixel(
		    x + 1,
		    y + 1,
		    rgb8_quant_error(img_buffer.get_pixel(x + 1, y + 1).channels(),
				     quant_error, 1));
	    }
	}

    }

    img_buffer
}

// Generate MSB and LSB LUTs and write them to raw files
fn gen_luts(msb_output: PathBuf, lsb_output: PathBuf) {
    // Generate a MSB lookup table for all possible pixels
    let mut msb_lut: [u8; 4096] = [0; 4096];
    let mut lsb_lut: [u8; 4096] = [0; 4096];
    for i in 0..=0b111111 {
	for j in 0..=0b111111 {
	    // direct as u8 is fine, only six bits
	    let v = two_pixels_to_msb_lsb(i as u8, j as u8);
	    msb_lut[(i << 6) | j] = v.0;
	    lsb_lut[(i << 6) | j] = v.1;
	}
    }

    fs::write(msb_output, msb_lut).expect("Failed to write MSB LUT");
    fs::write(lsb_output, lsb_lut).expect("Failed to write LSB LUT");
}

fn unformat_image_raw(input: PathBuf, output: PathBuf) {
    // unformat an image, but save it as raw bytes, not an image format
    let formatted = fs::read(input).expect("Failed to read input file");

    if formatted.len() != 320*240 {
	panic!("Formatted data read from file is not {} bytes!", 320*240);
    }

    let mut imgbuf: [[u8; 240]; 320] = [[0u8; 240]; 320];
    
    for row in 0..320 {
	let mut formatted_col = 0;
	for col in (0..240).step_by(2) {
	    let msb = formatted[row*240 + formatted_col];
	    let lsb = formatted[row*240 + (formatted_col+120)];
	    
	    let p0_red = (msb & 0b10) | ((lsb & 0b10) >> 1);
	    let p0_green = ((msb & 0b1000) >> 2) | ((lsb & 0b1000) >> 3);
	    let p0_blue = ((msb & 0b100000) >> 4) | ((lsb & 0b100000) >> 5);

	    let p1_red = (msb & 0b1) << 1 | (lsb & 0b1);
	    let p1_green = (((msb & 0b100) >> 2) << 1) | ((lsb & 0b100) >> 2);
	    let p1_blue = ((msb & 0b10000) >> 3) | ((lsb & 0b10000) >> 4);

	    let p0 = p0_red | p0_green | p0_blue;
	    let p1 = p1_red | p1_green | p1_blue;
	    print!("{:x}, {:x}, ", p0, p1);
	    imgbuf[row][col] = p0;
	    imgbuf[row][col+1] = p1;
	    
	    formatted_col += 1;
	}
	println!();
    }

    // reformat 2d to a 1d array
    let mut imgbuf_1d: [u8; 76800] = [0u8; 76800];
    let mut i = 0;
    for row in 0..320 {
	for col in 0..240 {
	    imgbuf_1d[i] = imgbuf[row][col];
	    i += 1;
	}
    }
    fs::write(output, imgbuf_1d).expect("failed to write output!");
}
/// Read a 240x320 image into an RgbImage, including configuring the
/// color transfer function to linearize out of non-linear sRGB, and
/// error if the image is the wrong size.
fn load_240x320_image(input: PathBuf) -> RgbImage {
    let mut img = ImageReader::open(input).expect("Failed to read image")
	.decode().expect("Failed to decode image")
	.into_rgb8();
    
    img.set_transfer_function(CicpTransferCharacteristics::Linear);
    
    if img.width() != 240 && img.height() != 320 {
	panic!("Expected image size 240x320, got image size {}x{}",
	       img.width(), img.height());
    }

    img
}
	    
fn main() {
    let args = Args::parse();
    match args.command {
	Commands::Format { input, output } => {
	    let img = load_240x320_image(input);
	    let formatted = format_image(img);
	    
	    fs::write(output, formatted).expect("Failed to write output file");
	},
	
	Commands::Unformat { input, output } => {
	    unformat_image(input, output);
	},
	
	Commands::GenLuts { msb_output, lsb_output } => {
	    gen_luts(msb_output, lsb_output);
	},
	
	Commands::UnformatRaw { input, output } => {
	    unformat_image_raw(input, output);
	},
	
	Commands::Dither { input, output } => {
	    let img = load_240x320_image(input);

	    floyd_steinberg_dither(img).save(output).unwrap();
	},
	Commands::DitherFormat { input, output } => {
	    let img = load_240x320_image(input);

	    let dithered = floyd_steinberg_dither(img);
	    let formatted = format_image(dithered);
	    fs::write(output, formatted).expect("Failed to write output file");
	}
    };
    
}
