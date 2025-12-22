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


#include "pencils.h"

const int led_pin = 14;
const int five_volt_en = 16;
const int va_pin = 12;
const int vb_vcom_pin = 13;

uint va_vb_vcom_tracker = 1;
uint pwm_slice;

uint32_t global_32bit_zero = 0;

uint8_t red_framebuffer[320][240];

uint8_t* red_framebuffer_1d = (uint8_t*)red_framebuffer;

uint8_t black_framebuffer[320][240];
uint8_t* black_framebuffer_1d = (uint8_t*)black_framebuffer;

uint32_t global_32bit_max = 0b111111;

void error_handler() {
  while (1) {
    gpio_put(led_pin, 1);
    sleep_ms(500);
    gpio_put(led_pin, 0);
    sleep_ms(500);
  }
}

//PIO pio = pio0;
uint vertical_sm = 0;
uint gen_sm = 1;
uint horiz_data_sm = 2;

uint vertical_offset = 0;
uint gen_offset = 0;
uint horiz_data_offset = 0;

void add_pio_programs(PIO pio) {
  vertical_offset = pio_add_program(pio, &sharpie_vertical_program);
  if (vertical_offset < 0) {
    printf("failed to add sharpie_vertical_program\n");
    error_handler();
  }
  
  // init GEN state machine
  gen_offset = pio_add_program(pio, &sharpie_gen_program);
  if (gen_offset < 0) {
    printf("failed to add sharpie_gen_program\n");
    error_handler();
  }
  
  
  // init horiz/data state machine
  horiz_data_offset = pio_add_program(pio, &sharpie_horiz_data_program);
  if (horiz_data_offset < 0) {
    printf("failed to add sharpie_horiz_data_program\n");
    error_handler();
  }

}

void restart_state_machines(PIO pio) {
  // init all three state machines

  // INTB on 0, GSP on 1, GCK on 2
  sharpie_vertical_pio_init(pio, vertical_sm, vertical_offset, 0);
  // GEN on pin 3, start state machine
  sharpie_gen_pio_init(pio, gen_sm, gen_offset, 3);
  // BSP on pin 4, BCK on pin 5, data from pin 6 to 11 inclusive
  sharpie_horiz_data_pio_init(pio, horiz_data_sm, horiz_data_offset, 4, 6);
  
  // charge the vertical state machine, it's waiting for irq 0
  // the number you put in its FIFO is the number of times the loop
  // will run, minus 1
  pio_sm_put(pio, vertical_sm, 321); // run 321 times for 648 h/l total
  
  // GEN: run 5 times (counter value + 1)
  pio_sm_put(pio, gen_sm, 639); // this should be 639 for 640 high pulses
  pio_sm_exec(pio, gen_sm, pio_encode_pull(false, false)); // just a basic pull
  pio_sm_exec(pio, gen_sm, pio_encode_mov(pio_x, pio_osr)); // mov x, osr
  // GEN counter is now charged
  
  // horiz-data: charge X, make a backup in ISR (unused by any other part of the code)
  pio_sm_put(pio, horiz_data_sm, 59); // we'll get a total of 2(x+1)+4 h/l so this should be 59 => 2(59+1)+4 = 124
  pio_sm_exec(pio, horiz_data_sm, pio_encode_pull(false, false));  // pull
  pio_sm_exec(pio, horiz_data_sm, pio_encode_out(pio_isr, 32)); // out isr, 32 (make backup of counter value and clear OSR for autopull)
  pio_sm_exec(pio, horiz_data_sm, pio_encode_mov(pio_x, pio_isr)); // mov x, isr (load X with counter)
  // charge Y for total loop counter
  pio_sm_put(pio, horiz_data_sm, 640); // should be 640 for 641 loops (see 6-3-2, the last loop has data all zeros, which is why we have a chained DMA channel below)
  pio_sm_exec(pio, horiz_data_sm, pio_encode_pull(false, false)); // pull
  pio_sm_exec(pio, horiz_data_sm, pio_encode_out(pio_y, 32)); // out y, 32 (also clears OSR)
  
}

void pwm_wrap_interrupt() {
  pwm_clear_irq(pwm_slice);
  gpio_put(va_pin, va_vb_vcom_tracker);
  gpio_put(vb_vcom_pin, !va_vb_vcom_tracker);

  if (va_vb_vcom_tracker == 0) {
    va_vb_vcom_tracker = 1;
  } else {
    va_vb_vcom_tracker = 0;
  }
}

uint partial_intb_gsp_sm = 0;
uint partial_horiz_data_sm = 1;

uint partial_gck_sm = 0;
uint partial_gck_end_sm = 1;


//// Initialize partial update stuff for one specific, predefined partial update.
#define SKIPS (200)
#define CHANGES (19)

// our objective is to send a partial frame consisting of 20 white
// lines starting at zero-indexed line 99.

// GCK control needs -1 for the counter to work correctly.
uint32_t gck_control_data[] = {SKIPS - 1, // skip 99 lines
			       CHANGES - 1, // send 19 lines of data
			       (320-SKIPS-CHANGES) - 1}; // skip the rest

// number of 1/32 GCK h/ls to wait until the GCK end SM activates
uint32_t gck_end_timeout = 2*32 + // 2 full GCK h/ls at start
  SKIPS*2 + // 99 skipped lines, short GCK h/ls
  (CHANGES*2 + 1)*32 + // 19 changed lines (add 1 extra h/l for the way GCK works)
  (319-SKIPS-CHANGES)*2 + 1; // first skip after changed is 2x as long, but
                     // the initial 2*32 includes the first line, so we use 319, not 320

uint32_t gsp_high_timeout = 53;

uint8_t partial_frame_pixels[CHANGES*240+120]; // +120 for final 1/2 line


void init_partial_update_pios(PIO intb_gsp_horiz_pio, PIO gck_gck_end_pio) {

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
  
  // push a value for how long the INTB/GSP SM should leave GSP high (this is
  // always the same, but it's too big for a set instruction)
  pio_sm_put(intb_gsp_horiz_pio, partial_intb_gsp_sm, gsp_high_timeout);
  // prepare GCK end SM for irq 1 countdown
  pio_sm_put(gck_gck_end_pio, partial_gck_end_sm, gck_end_timeout);
  // put two more values (the exact value of each doesn't matter) on the
  // stack so that the wrap repeats 3 times total
  pio_sm_put(gck_gck_end_pio, partial_gck_end_sm, 3);
  pio_sm_put(gck_gck_end_pio, partial_gck_end_sm, 3);
  // place the timeout counter in x
  pio_sm_exec(gck_gck_end_pio, partial_gck_end_sm, pio_encode_out(pio_x, 32));

  // charge horiz/data SM's inner loop counter
  // get the counter, put it in ISR (backup), then copy to x
  pio_sm_put(intb_gsp_horiz_pio, partial_horiz_data_sm, 59);
  pio_sm_exec(intb_gsp_horiz_pio, partial_horiz_data_sm, pio_encode_out(pio_isr, 32));
  pio_sm_exec(intb_gsp_horiz_pio, partial_horiz_data_sm, pio_encode_mov(pio_x, pio_isr));
  // leaving an `out y, 8` instruction stalled in the main program and
  // then pushing onto the FIFO definitely just absorbs that value.
  // since we can't put instructions after the `wait irq`, we have to
  // load the first y loop counter manually. after this
  pio_sm_put(intb_gsp_horiz_pio, partial_horiz_data_sm, CHANGES*2); // 38 + 1 => 39, for final 1/2 line of zeros
  pio_sm_exec(intb_gsp_horiz_pio, partial_horiz_data_sm, pio_encode_out(pio_y, 16)); // 16 bit!


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
//   As a result, we're going to not do that.
//
// - The first test will be the entire boot-up sequence, write the red image,
//   hold for 60 seconds, then do the shut-off sequence. This will get us as
//   close as possible to the prescribed usage pattern.

// The problem was caused by the zeros DMA stream being improperly
// configured to send 2x as many bytes as it is supposed to. The
// waveforms show that the horizontal control pulses continue for one
// GCK h/l after an entire frame, and one GCK h/l is one collection of
// MSB or LSB bytes, or 120 bytes. We were sending 240 bytes.
void main() {
  // set up the system: initialize pins and PIO
  stdio_init_all();

  memset(partial_frame_pixels, 0, sizeof(partial_frame_pixels));
  memset(partial_frame_pixels, 0b001100, CHANGES*240);

  /*while (!stdio_usb_connected()) {
    sleep_ms(50);
    }*/
  

  // red framebuffer gets white, actually, because we are more confident
  // in what white looks like than in what anything else looks like
  memset(red_framebuffer_1d, 0b110000, 320*240);

  // stuck bit tests
  /*for (uint32_t i = 0; i < 320; i++) {
    // set all pixels' red LSb to 1
    for (uint32_t j = 0; j < 120; j++) {
      red_framebuffer[i][j] = 0b000000;
    }
    for (uint32_t j = 120; j < 240; j++) {
      red_framebuffer[i][j] = 0b000001;
    }
    }*/
  memset(black_framebuffer_1d, 0, 320*240);
  
  gpio_init(five_volt_en);
  gpio_set_dir(five_volt_en, GPIO_OUT);

  gpio_init(led_pin);
  gpio_set_dir(led_pin, GPIO_OUT);

  gpio_init(va_pin);
  gpio_set_dir(va_pin, GPIO_OUT);

  gpio_init(vb_vcom_pin);
  gpio_set_dir(vb_vcom_pin, GPIO_OUT);

  // enable PWM wrap irq
  irq_set_exclusive_handler(PWM_IRQ_WRAP_0, pwm_wrap_interrupt);
  irq_set_enabled(PWM_IRQ_WRAP_0, true);
  
  // system clock is 150 MHz by default on RP2350, and all the
  // clock divider values configured in the PIO sources are tuned
  // for that speed

  add_pio_programs(pio0);
  restart_state_machines(pio0);

  
  // we found that the initial blank screen isn't necessary.


  // restart all state machine clocks
  pio_clkdiv_restart_sm_mask(pio0, 0b111); // restart state machines
  


  /*******************************************/
  // The system is now ready to actually do screen stuff.
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

  // transmit black screen
  //
  // this is pixel memory initialization, it's probably optional.
  //pio0->irq_force = 0b1;

  // wait for the frame to transmit (INTB max time is 57.47 ms, plus
  // min 163.68 μs between fall and next rise)
  sleep_ms(60);
  printf("transmitted first black screen\n");

  // wait mandated 30μs (this is already taken care of but the difference shouldn't matter)
  sleep_us(30);

  // configure VCOM, VB, VA
  // these are 60Hz signals, and VB and VCOM are the same signal
  //
  // the system clock runs at 150 MHz and we have an 8.4 divider
  //
  // let's use a divider of 200 to yield a 750 kHz signal.
  //
  // We can't use PWM to make these signals because the pins are on
  // the same slice, so the signals can't be out of phase.
  // We can use the PWM as a timer though.

  // these are on slice 6

  // clock math:
  // 150MHz sysclk -> [divider: /200] -> 750kHz PWM clk ->
  // 6250-cycle wrapper -> 60Hz signal
  pwm_slice = pwm_gpio_to_slice_num(12); // channel 6A

  // I need to check the VCOM frequency with a scope, but as far as I
  // can tell it doesn't change the appearence of the image.
  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_clkdiv_mode(&cfg, PWM_DIV_FREE_RUNNING);
  pwm_config_set_clkdiv(&cfg, 200);
  int wrap = 6250;
  pwm_config_set_wrap(&cfg, wrap);
  pwm_set_irq_enabled(pwm_slice, true); // interrupt generated on wrap
  pwm_set_chan_level(pwm_slice, PWM_CHAN_A, wrap-1); // immediately fire the irq
  pwm_init(pwm_slice, &cfg, true); // start now

  printf("VA/VB/VCOM running\n");
  // wait at least two VB/VCOM cycles (at 60 Hz one cycle is 16 ms)
  //
  // they show at least 1.5 cycles before sending data
  sleep_ms(60);

  
  // send red screen
  restart_state_machines(pio0);
  pio_clkdiv_restart_sm_mask(pio0, 0b111);
  
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
  
  dma_channel_config image_c = dma_channel_get_default_config(image_pixels_channel);
  channel_config_set_read_increment(&image_c, true); // increment reads
  channel_config_set_write_increment(&image_c, false); // no increment writes (into the FIFO)
  channel_config_set_transfer_data_size(&image_c, DMA_SIZE_32); // four byte transfers (one byte doesn't work)
  channel_config_set_dreq(&image_c, pio_get_dreq(pio0, horiz_data_sm, true)); // true for sending data to SM
  channel_config_set_chain_to(&image_c, image_pixels_zero_channel); // chain to zero channel to start zero channel when this finishes
  dma_channel_configure(image_pixels_channel, &image_c,
			&pio0->txf[horiz_data_sm], // destination (TX FIFO of SM 2)
		        pencils, // source 
			19200, // transfer size = 320*240/4 = 19200
			true); // start now



  dma_channel_config image_zero_c = dma_channel_get_default_config(image_pixels_zero_channel);
  channel_config_set_read_increment(&image_zero_c, false); // just send zeros
  channel_config_set_write_increment(&image_zero_c, false); // write into FIFO
  channel_config_set_transfer_data_size(&image_zero_c, DMA_SIZE_32);
  channel_config_set_dreq(&image_zero_c, pio_get_dreq(pio0, horiz_data_sm, true));
  dma_channel_configure(image_pixels_zero_channel, &image_zero_c,
			&pio0->txf[horiz_data_sm],
			&global_32bit_zero,
			120/4,
			false); // wait for chain start


  
  // transmit red framebuffer
  pio0->irq_force = 0b1;

  printf("send red framebuffer\n");
  // wait for the frame to transmit
  sleep_ms(60);

  printf("waiting...\n");
  
  // wait a few seconds and then send partial update
  sleep_ms(5000);

  // GPIO pins can only be mapped to one PIO at a time, so we have
  // to swap between PIOs.
  
  init_partial_update_pios(pio1, pio2);
  
  int gck_dma_channel = dma_claim_unused_channel(true);
  if (gck_dma_channel < 0) {
    printf("failed to claim GCK DMA channel\n");
    error_handler();
  }

  // then the data stream
  int pixel_data_dma_channel = dma_claim_unused_channel(true);
  if (pixel_data_dma_channel < 0) {
    printf("failed to claim partial pixel data DMA channel\n");
    error_handler();
  }

  dma_channel_config partial_gck_c = dma_channel_get_default_config(gck_dma_channel);
  channel_config_set_read_increment(&partial_gck_c, true);
  channel_config_set_write_increment(&partial_gck_c, false);
  channel_config_set_transfer_data_size(&partial_gck_c, DMA_SIZE_32); // we use the WHOLE width of the FIFO entry
  channel_config_set_dreq(&partial_gck_c, pio_get_dreq(pio2, partial_gck_sm, true)); // true for sending data to the SM
  dma_channel_configure(gck_dma_channel, &partial_gck_c,
			&pio2->txf[partial_gck_sm],
			gck_control_data,
			3,
			true);

  dma_channel_config partial_data_c = dma_channel_get_default_config(pixel_data_dma_channel);
  channel_config_set_read_increment(&partial_data_c, true);
  channel_config_set_write_increment(&partial_data_c, false);
  // we found originally that transfers have to be 32 bits, for some
  // reason, and that still holds true for slightly modified horiz/data.
  channel_config_set_transfer_data_size(&partial_data_c, DMA_SIZE_32);
  channel_config_set_dreq(&partial_data_c, pio_get_dreq(pio1, partial_horiz_data_sm, true));
  dma_channel_configure(pixel_data_dma_channel, &partial_data_c,
			&pio1->txf[partial_horiz_data_sm],
			partial_frame_pixels,
			19*60+30, // +30 for the final 1/2 line
			true);


  pio1->irq_force = 0b1;
  
  while (true);


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
