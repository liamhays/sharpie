// There's not very much code in this file, because a lot of the
// display signal complexity is taken care of in the state
// machines. That means, however, that what's in this file is actually
// quite abstract, so I recommend that you get yourself a good
// understanding of how this display works and how my code works
// before you try to modify it. See the Sharpie GitHub repository for
// more information.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "sharpie-vertical.pio.h"
#include "sharpie-gen.pio.h"
#include "sharpie-horiz-data.pio.h"

#include "sharpie-partial-gck.pio.h"
#include "sharpie-partial-intb-gsp.pio.h"
#include "sharpie-partial-gck-end.pio.h"
#include "sharpie-partial-horiz-data.pio.h"

// to generate this image, run `cargo run -- dither-format pencils.jpg
// pencils.raw`, then use xxd to make a header file
#include "pencils.h"

const int led_pin = 14;
const int five_volt_en = 16;
const int va_pin = 12;
const int vb_vcom_pin = 13;


uint pwm_slice;

uint32_t global_32bit_zero = 0;


void error_handler() {
  // super basic error loop that works even when a serial terminal
  // isn't connected
  while (1) {
    gpio_put(led_pin, 1);
    sleep_ms(500);
    gpio_put(led_pin, 0);
    sleep_ms(500);
  }
}



uint vertical_sm = 0;
uint gen_sm = 1;
uint horiz_data_sm = 2;

uint partial_intb_gsp_sm = 0;
uint partial_horiz_data_sm = 1;

uint partial_gck_sm = 0;
uint partial_gck_end_sm = 1;

PIO full_frame_pio = pio0;
PIO intb_gsp_horiz_pio = pio1;
PIO gck_gck_end_pio = pio2;


// "full frame" is not the best name for this, but it works. the full
// frame PIO sends a complete frame instead of a set of partial update
// regions.
void init_full_frame_pio() {
  // add programs for the full-frame (no partial update) PIO
  uint vertical_offset = pio_add_program(full_frame_pio, &sharpie_vertical_program);
  if (vertical_offset < 0) {
    printf("failed to add sharpie_vertical_program\n");
    error_handler();
  }
  
  // init GEN state machine
  uint gen_offset = pio_add_program(full_frame_pio, &sharpie_gen_program);
  if (gen_offset < 0) {
    printf("failed to add sharpie_gen_program\n");
    error_handler();
  }
  
  
  // init horiz/data state machine
  uint horiz_data_offset = pio_add_program(full_frame_pio,
					   &sharpie_horiz_data_program);
  if (horiz_data_offset < 0) {
    printf("failed to add sharpie_horiz_data_program\n");
    error_handler();
  }

  // init all three state machines

  // INTB on 0, GSP on 1, GCK on 2
  sharpie_vertical_pio_init(full_frame_pio, vertical_sm, vertical_offset, 0);
  // GEN on pin 3, start state machine
  sharpie_gen_pio_init(full_frame_pio, gen_sm, gen_offset, 3);
  // BSP on pin 4, BCK on pin 5, data from pin 6 to 11 inclusive
  sharpie_horiz_data_pio_init(full_frame_pio, horiz_data_sm, horiz_data_offset, 4, 6);
  
  // charge the vertical state machine, it's waiting for irq 0
  // the number you put in its FIFO is the number of times the loop
  // will run, minus 1
  pio_sm_put(full_frame_pio, vertical_sm, 321); // run 321 times for 648 h/l total
  
  // GEN: run 5 times (counter value + 1)
  pio_sm_put(full_frame_pio, gen_sm, 639); // this should be 639 for 640 high pulses
  pio_sm_exec(full_frame_pio, gen_sm, pio_encode_pull(false, false)); // just a basic pull
  pio_sm_exec(full_frame_pio, gen_sm, pio_encode_mov(pio_x, pio_osr)); // mov x, osr
  // GEN counter is now charged
  
  // horiz-data: charge X, make a backup in ISR (unused by any other part of the code)
  pio_sm_put(full_frame_pio, horiz_data_sm, 59); // we'll get a total of 2(x+1)+4 h/l so this should be 59 => 2(59+1)+4 = 124
  pio_sm_exec(full_frame_pio, horiz_data_sm, pio_encode_pull(false, false));  // pull
  pio_sm_exec(full_frame_pio, horiz_data_sm, pio_encode_out(pio_isr, 32)); // out isr, 32 (make backup of counter value and clear OSR for autopull)
  pio_sm_exec(full_frame_pio, horiz_data_sm, pio_encode_mov(pio_x, pio_isr)); // mov x, isr (load X with counter)
  // charge Y for total loop counter
  pio_sm_put(full_frame_pio, horiz_data_sm, 640); // should be 640 for 641 loops (see 6-3-2, the last loop has data all zeros, which is why we have a chained DMA channel below)
  pio_sm_exec(full_frame_pio, horiz_data_sm, pio_encode_pull(false, false)); // pull
  pio_sm_exec(full_frame_pio, horiz_data_sm, pio_encode_out(pio_y, 32)); // out y, 32 (also clears OSR)

  // restart all state machine clocks
  pio_clkdiv_restart_sm_mask(full_frame_pio, 0b111); // restart state machines
  
}


// The following macros and variables are predefined examples for
// different kinds of partial updates. The first (no numeric suffix on
// the variable names) skips SKIPS lines, then updates CHANGES
// lines. The second (suffix 2) skips SKIPS lines, updates CHANGES
// lines, skips SKIPS2 lines, then updates CHANGES2 lines. Finally,
// the third group of variables updates CHANGES lines at the very top
// of the screen, then skips SKIPS lines, then updates CHANGES2 lines.
//
// The data DMAed into the horiz/data state machine doesn't need to
// change for these, but if the stream is left incomplete after the
// end of the frame, you have to reset it and possibly the SM as
// well. The data stream is initialized in main().

#define SKIPS (200)
#define CHANGES (19)
#define SKIPS2 (20)
#define CHANGES2 (5)



// GCK control needs -1 for the counter to work correctly in all
// circumstances, due to how the loops in the state machines work.
uint32_t gck_control_data[] = {SKIPS - 1,
			       CHANGES - 1,
			       (320-SKIPS-CHANGES) - 1}; // skip the rest

// number of 1/32 GCK h/ls to wait until the GCK end SM activates
uint32_t gck_end_timeout = 2*32 + // 2 full GCK h/ls at start
  SKIPS*2 + // short GCK h/ls
  (CHANGES*2 + 1)*32 + // 19 changed lines (add 1 extra h/l for the way GCK works)
  (319-SKIPS-CHANGES)*2 + 1; // first skip after changed is 2x as long, but
// the initial 2*32 includes the first line, so we use 319, not 320



// just like before
uint32_t gck_control_data2[] = {SKIPS - 1,
				CHANGES - 1,
				SKIPS2 - 1,
				CHANGES2 - 1,
				(320 - SKIPS - CHANGES - SKIPS2 - CHANGES2) - 1};

// also just like before
uint32_t gck_end_timeout2 = 2*32 +
			    SKIPS*2 +
			    (CHANGES*2 + 1)*32 +
			    SKIPS2*2 +
			    (CHANGES2*2 + 1)*32 +
			    (319 - SKIPS - CHANGES - SKIPS2 - CHANGES2)*2 + 1;


// 0 at the start of a GCK control data block tells the SM to start
// sending changed lines beginning with the first line on the display,
// instead of skipping some quantity.
uint32_t gck_control_data3[] = {0,
				CHANGES - 1,
				SKIPS - 1,
				CHANGES2 - 1,
				(320 - SKIPS - CHANGES - SKIPS2 - CHANGES2) - 1};

// When the changes start at the top of the screen, they start on
// GCK2, so we have to open this with 1*32 instead of 2*32.
uint32_t gck_end_timeout3 = 1*32 + 
			    (CHANGES*2 + 1) * 32 +
			    SKIPS * 2 +
			    (CHANGES2*2 + 1) * 32 +
			    // we still use 319 because 1*32 includes the
			    // first line.
			    (319 - SKIPS - CHANGES - CHANGES2)*2 + 1; 


// this value never changes
const uint32_t gsp_high_timeout = 53;

// note the +120 for final 1/2 line of zeros, which is used on the
// extra GCK h/l during a changed section that we mentioned in the GCK
// end timeouts above.
// the +4s are for the 32-bit changed lines counters
uint8_t partial_frame_pixels[4 + CHANGES*240+120 + 4 + CHANGES2*240+120];



// partial display updates require two PIOs and a decent amount of configuration.
void init_partial_update_pios() {
  // add the programs
  uint intb_gsp_offset = pio_add_program(intb_gsp_horiz_pio, &sharpie_partial_intb_gsp_program);
  if (intb_gsp_offset < 0) {
    printf("failed to add partial_intb_gsp\n");
    error_handler();
  }

  uint horiz_data_offset = pio_add_program(intb_gsp_horiz_pio, &sharpie_partial_horiz_data_program);
  if (horiz_data_offset < 0) {
    printf("failed to add horiz_data_offset\n");
    error_handler();
  }
  
  uint gck_offset = pio_add_program(gck_gck_end_pio, &sharpie_partial_gck_program);
  if (gck_offset < 0) {
    printf("failed to add partial_gck\n");
    error_handler();
  }
  
  uint gck_end_offset = pio_add_program(gck_gck_end_pio, &sharpie_partial_gck_end_program);
  if (gck_end_offset < 0) {
    printf("failed to add partial_gck_end\n");
    error_handler();
  }
  
  // INTB on 0, GSP on 1
  sharpie_partial_intb_gsp_pio_init(intb_gsp_horiz_pio, partial_intb_gsp_sm, intb_gsp_offset, 0);

  // GCK on 2, GCK on 3
  sharpie_partial_gck_pio_init(gck_gck_end_pio, partial_gck_sm, gck_offset, 2);

  // GCK on 2 again.
  sharpie_partial_gck_end_pio_init(gck_gck_end_pio, partial_gck_end_sm, gck_end_offset, 2);

  // BSP on pin 4, BCK on pin 5, data on pins 6-11
  sharpie_partial_horiz_data_pio_init(intb_gsp_horiz_pio, partial_horiz_data_sm, horiz_data_offset, 4, 6);
  
  // push a value for how long the INTB/GSP SM should leave GSP
  // high. this value is a constant, but it's bigger than 5 bits so we
  // can't use a `set` instruction in the state machine.
  pio_sm_put(intb_gsp_horiz_pio, partial_intb_gsp_sm, gsp_high_timeout);
  
  // prepare GCK end SM by giving it the proper counter
  // important! make sure this is correctly calculated, otherwise the
  // signals at the end of the frame will be deformed
  pio_sm_put(gck_gck_end_pio, partial_gck_end_sm, gck_end_timeout3);
  
  // put two zeros---the value doesn't matter but the number of
  // numbers does---on the GCK end FIFO so that the wrap repeats 3
  // times total (this facilitates a code-saving measure)
  pio_sm_put(gck_gck_end_pio, partial_gck_end_sm, 0);
  pio_sm_put(gck_gck_end_pio, partial_gck_end_sm, 0);
  
  // place the GCK end timeout counter in GCK end SM's x register
  pio_sm_exec(gck_gck_end_pio, partial_gck_end_sm, pio_encode_out(pio_x, 32));

  // charge horiz/data SM's inner loop counter
  // get the counter, put it in ISR (backup), then copy to x
  // this will be used repeatedly throughout a frame (and
  // this is also exactly how the full-frame PIO works)
  pio_sm_put(intb_gsp_horiz_pio, partial_horiz_data_sm, 59);
  // we don't need to force any instructions into the PIO here like we
  // do with the full-frame program, because there's enough
  // instruction space left in the INTB/GSP/horiz/data PIO for the
  // `out`s and `mov` that charge registers appropriately. that also
  // means that the first changed lines counter for horiz/data can go
  // in the DMA stream.

  pio_clkdiv_restart_sm_mask(intb_gsp_horiz_pio, 0b1111);
  pio_clkdiv_restart_sm_mask(gck_gck_end_pio, 0b1111);


}



// The screen's basic flow looks like this, according to (6-2) in the
// datasheet:
// - 3.2V rises (this happens when we plug Sharpie in)
// - 5V rises at least 1ms later
// - you write the whole screen black at least 2 GCK cycles after 5V is fully risen
// - then after 30μs, VCOM, VB, and VA start cycling
// - at least 1 VCOM/VB cycle later, you can start sending frames to the screen
// - <>
// - to deinitialize, you write the whole screen black again, then turn
//   off VCOM/VB/VA, then after the same 30μs delay, you can turn off
//   5V, and then after 1ms, you turn off 3V2
//
// - The 5V on this board holds charge for a long time, and we don't
//   have an easy way other than serial messages to tell the board to shut off.
//   As a result, we're going to not do that. (fixed in sharpie rev2)
//
// - The first test will be the entire boot-up sequence, write the red image,
//   hold for 60 seconds, then do the shut-off sequence. This will get us as
//   close as possible to the prescribed usage pattern.

///////
// Here's the thing: you don't have to do the two black screens,
// they're optional (but probably a good idea). You do still have to
// turn on VCOM/VB/VA at the right time and leave enough time before
// you send data. The code in main() below is a trimmed, simplified
// version of the list of steps above, that displays a full-frame
// image, then updates regions of it.


void main() {
  // on RP2350, which this code requires, the default should be 150
  // MHz, but we set it just in case. all the SMs depend on a 150 MHz
  // clock, but their dividers are mostly nice numbers, so it
  // shouldn't be hard to recalculate. the biggest source of issues is
  // probably the 387.5 divider on most of the partial update SMs.
  set_sys_clock_khz(150000, true);

  stdio_init_all();

  // initialize the data to be sent to the horiz/data SM
  //
  // start by setting the whole buffer to zero, which is necessary for
  // the end-of-changed-region half-lines that have to have the data
  // lines at zero
  memset(partial_frame_pixels, 0, sizeof(partial_frame_pixels));

  // these individual statements are a poor man's 32-bit =>
  // little-endian 8-bit conversion for the first changed lines
  // counter. even though 9 bits is enough to cover the 320-line
  // screen, the DMA transfers 32 bits at a time, so the counter must
  // be 32 bits.
  partial_frame_pixels[0] = (CHANGES)*2; // *2 for 2x per line
  partial_frame_pixels[1] = 0;
  partial_frame_pixels[2] = 0;
  partial_frame_pixels[3] = 0;

  size_t offset = 4;
  
  // the first changed lines region is a block of green pixels
  memset(&partial_frame_pixels[4], 0b001100, CHANGES*240);

  offset += CHANGES*240 + 120;
  
  // repeat for the next changed lines counter
  partial_frame_pixels[offset] = (CHANGES2)*2;
  partial_frame_pixels[offset + 1] = 0;
  partial_frame_pixels[offset + 2] = 0;
  partial_frame_pixels[offset + 3] = 0;
  memset(&partial_frame_pixels[offset + 4], 0b110000, CHANGES2*240);
  
  gpio_init(five_volt_en);
  gpio_set_dir(five_volt_en, GPIO_OUT);

  gpio_init(led_pin);
  gpio_set_dir(led_pin, GPIO_OUT);

  
  /////////////////////////////////////////////
  // Now we can manipulate the display.
  /////////////////////////////////////////////

  // 3.2V is already up.
  // at least 1ms has already probably passed, but we will wait anyway.
  sleep_ms(2);

  // enable 5V
  gpio_put(five_volt_en, 1);
  // sleep 1ms for rise time
  sleep_ms(1);

  // two GCK cycles is 2*(2*83.08 μs) = 332.32 μs, more or less (our
  // GCK is not exactly the typical value in the datasheet)
  sleep_ms(1);

  // transmit black screen (this isn't necessary so we don't actually do it)
  //
  //pio0->irq_force = 0b1;

  // wait for the frame to transmit (INTB max time is 57.47 ms, plus
  // min 163.68 μs between fall and next rise)
  sleep_ms(60);
  //printf("transmitted first black screen\n");

  // wait mandated 30μs (this is already taken care of but the
  // difference shouldn't matter)
  sleep_us(30);

  // configure VCOM, VB, VA these are 60Hz signals, and VB and VCOM
  // are the same signal, with VA 180 degrees out of phase from
  // VB/VCOM.

  // clock math:
  // 150MHz sysclk -> [divider: /250] -> 750kHz PWM clk ->
  // 10000-cycle wrapper -> halfway level markers for
  // 50% duty -> 60Hz signal

  // VA/(VB/VCOM) are convienently (unintentionally designed, but it
  // worked out well) on the two outputs of slice 6
  pwm_slice = pwm_gpio_to_slice_num(va_pin); // channel 6A (slice 6)

  // using pwm_config doesn't seem to work
  gpio_set_function(va_pin, GPIO_FUNC_PWM);
  gpio_set_function(vb_vcom_pin, GPIO_FUNC_PWM);
  pwm_set_clkdiv(pwm_slice, 250);
  int wrap = 10000;
  pwm_set_wrap(pwm_slice, wrap);
  // VB/VCOM needs to start first, with VA 180 degrees out of phase,
  // so we set the counter halfway so that the outputs are initially
  // on. the second output is also inverted to make the phase shift.
  pwm_set_counter(pwm_slice, wrap/2);
  pwm_set_chan_level(pwm_slice, PWM_CHAN_A, wrap/2);
  pwm_set_chan_level(pwm_slice, PWM_CHAN_B, wrap/2);
  // invert output B (VB/VCOM)
  pwm_set_output_polarity(pwm_slice, false, true);
  pwm_set_enabled(pwm_slice, true);

  printf("VA/VB/VCOM running\n");
  // wait at least two VB/VCOM cycles (at 60 Hz one cycle is 16 ms)
  //
  // Datasheet shows at least 1.5 cycles before sending data
  sleep_ms(60);

  
  // send full-frame image
  init_full_frame_pio();
  
  int image_pixels_channel = dma_claim_unused_channel(true); // true -> required
  if (image_pixels_channel < 0) {
    printf("failed to claim red dma channel\n");
    error_handler();
  }

  int image_pixels_zero_channel = dma_claim_unused_channel(true);
  if (image_pixels_zero_channel < 0) {
    printf("failed to claim red dma zero channel\n");
    error_handler();
  }

  // this DMA stream sends the image, and chains to `image_zero_c`,
  // which sends 120 bytes of zeros to close out the image. a change
  // to make in the future is to include these zeros in the image
  // buffer, so a second stream isn't necessary.
  dma_channel_config image_c = dma_channel_get_default_config(image_pixels_channel);
  channel_config_set_read_increment(&image_c, true); // increment reads
  channel_config_set_write_increment(&image_c, false); // no increment writes (into the FIFO)
  channel_config_set_transfer_data_size(&image_c, DMA_SIZE_32); // four byte transfers (one byte doesn't work)
  channel_config_set_dreq(&image_c, pio_get_dreq(full_frame_pio, horiz_data_sm, true)); // true for sending data to SM
  channel_config_set_chain_to(&image_c, image_pixels_zero_channel); // chain to zero channel to start zero channel when this finishes
  dma_channel_configure(image_pixels_channel, &image_c,
			&full_frame_pio->txf[horiz_data_sm], // destination (TX FIFO of SM 2)
		        pencils, // source 
			19200, // transfer size = 320*240/4 = 19200
			true); // start now



  dma_channel_config image_zero_c = dma_channel_get_default_config(image_pixels_zero_channel);
  channel_config_set_read_increment(&image_zero_c, false); // just send zeros
  channel_config_set_write_increment(&image_zero_c, false); // write into FIFO
  channel_config_set_transfer_data_size(&image_zero_c, DMA_SIZE_32);
  channel_config_set_dreq(&image_zero_c, pio_get_dreq(full_frame_pio, horiz_data_sm, true));
  dma_channel_configure(image_pixels_zero_channel, &image_zero_c,
			&full_frame_pio->txf[horiz_data_sm],
			&global_32bit_zero,
			120/4,
			false); // wait for chain start


  
  // transmit image
  full_frame_pio->irq_force = 0b1;

  printf("send image\n");
  // wait for the frame to transmit
  sleep_ms(60);

  printf("waiting...\n");

  // wait a few seconds and then send partial update
  sleep_ms(2000);

  // GPIO pins can only be mapped to one PIO at a time, so now we have
  // to init the partial update PIOs and let them take over the GPIO
  // pins.
  
  init_partial_update_pios();
  
  int gck_dma_channel = dma_claim_unused_channel(true);
  if (gck_dma_channel < 0) {
    printf("failed to claim GCK DMA channel\n");
    error_handler();
  }

  int pixel_data_dma_channel = dma_claim_unused_channel(true);
  if (pixel_data_dma_channel < 0) {
    printf("failed to claim partial pixel data DMA channel\n");
    error_handler();
  }

  // this is the GCK control DMA stream
  dma_channel_config partial_gck_c = dma_channel_get_default_config(gck_dma_channel);
  channel_config_set_read_increment(&partial_gck_c, true);
  channel_config_set_write_increment(&partial_gck_c, false);
  channel_config_set_transfer_data_size(&partial_gck_c, DMA_SIZE_32); // we use the WHOLE width of the FIFO entry
  channel_config_set_dreq(&partial_gck_c, pio_get_dreq(gck_gck_end_pio, partial_gck_sm, true)); // true for sending data to the SM
  dma_channel_configure(gck_dma_channel, &partial_gck_c,
			&gck_gck_end_pio->txf[partial_gck_sm],
			// getting the number of transfers right would
			// be much better done in a higher-level
			// language with easy array size tracking
			gck_control_data3,
			5,
			true);

  // and this is the pixel data and changed lines counter for horiz/data DMA stream
  dma_channel_config partial_data_c = dma_channel_get_default_config(pixel_data_dma_channel);
  channel_config_set_read_increment(&partial_data_c, true);
  channel_config_set_write_increment(&partial_data_c, false);
  // we found originally that transfers have to be 32 bits, for some
  // reason, and that still holds true for slightly modified horiz/data.
  channel_config_set_transfer_data_size(&partial_data_c, DMA_SIZE_32);
  channel_config_set_dreq(&partial_data_c, pio_get_dreq(intb_gsp_horiz_pio, partial_horiz_data_sm, true));
  dma_channel_configure(pixel_data_dma_channel, &partial_data_c,
			&intb_gsp_horiz_pio->txf[partial_horiz_data_sm],
			partial_frame_pixels,
			1 + (CHANGES*60 + 30) + (CHANGES2*60 + 30) + 1,
			true);


  intb_gsp_horiz_pio->irq_force = 0b1;

  // wait forever, holding the image on the screen
  while (true);



  

  // below here is deinitialization code following the datasheet. it's
  // also optional (you can just pull the plug with no issues), but
  // it's probably a good idea to turn of VA/VB/VCOM and shut down
  // power supplies, etc.
  
  // wait for frame to transmit
  sleep_ms(60);

  // stop VCOM, VB, VA
  pwm_set_enabled(pwm_slice, false);
  // the datasheet seems to suggest that when an image is not being
  // transmitted, VA, VB, and VCOM can be held. this is something to
  // test.
  //
  // the datasheet also shows that all three Vx pins go low, so we
  // need to force them low since the PWM callback probably left them
  // on. 
  gpio_put(va_pin, 0);
  gpio_put(vb_vcom_pin, 0);
  
  // delay 30μs
  sleep_us(30);

  // and turn off 5V
  gpio_put(five_volt_en, 0);
  
  while (true);
}
