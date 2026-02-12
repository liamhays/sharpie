#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"

#include "sharpie-vertical.pio.h"
#include "sharpie-gen.pio.h"
#include "sharpie-horiz-data.pio.h"
// tinyusb source

#include "RP2350.h"

// tusb_config.h is included by tusb.h. we've configured CMake so that
// it knows to make tusb_config.h available.
#include "bsp/board_api.h"
#include "tusb.h"

#include "zstd.h"

#define BUFSIZE (76800)
#define RUNS (300)

typedef struct compressed_buffer {
  uint8_t data[BUFSIZE];
  uint32_t compressed_size;
} compressed_buffer_t;

// TODO: write directly into the buffers instead of inputbuf
uint8_t inputbuf[BUFSIZE];
compressed_buffer_t compressed_buffer0 = {0};
compressed_buffer_t compressed_buffer1 = {0};
uint8_t framebuffer[BUFSIZE];


// USB RX buffer is 32768, TX buffer is 64

const int led_pin = 14;
const int five_volt_en = 16;
const int va_pin = 12;
const int vb_vcom_pin = 13;

uint pwm_slice;

void error_handler() {
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

PIO full_frame_pio = pio0;

uint vertical_offset;
uint gen_offset;
uint horiz_data_offset;


uint32_t global_32bit_zero = 0;
int image_pixels_channel;
int image_pixels_zero_channel;
int compressed_data_copy_channel;

void send_full_frame_image(const unsigned char* source) {
  
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
		        source, // source 
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
}

void init_full_frame_pio() {
  // add programs for the full-frame (no partial update) PIO
  vertical_offset = pio_add_program(full_frame_pio, &sharpie_vertical_program);
  if (vertical_offset < 0) {
    printf("failed to add sharpie_vertical_program\n");
    error_handler();
  }
  
  // init GEN state machine
  gen_offset = pio_add_program(full_frame_pio, &sharpie_gen_program);
  if (gen_offset < 0) {
    printf("failed to add sharpie_gen_program\n");
    error_handler();
  }
  
  
  // init horiz/data state machine
  horiz_data_offset = pio_add_program(full_frame_pio,
				      &sharpie_horiz_data_program);
  if (horiz_data_offset < 0) {
    printf("failed to add sharpie_horiz_data_program\n");
    error_handler();
  }

}

void reset_full_frame_pio() {
  // init all three state machines. we would prefer to not do this
  // every time but it takes a decent amount of consideration to make that work.

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
  //pio_clkdiv_restart_sm_mask(full_frame_pio, 0b111); // restart state machines
  
}



const uint32_t sys_clock_hz = 200000000;
// we know exactly how the PIO works, so we can use this for an easy
// final delay in the core1 loop
const uint32_t one_gck_hl_us = (1./((float)sys_clock_hz/3100.)) * 4e6;


int data_ready_doorbell;
// the framebuffer ID with the newest data in it
volatile int newest_compressed_buffer = 0;

void core1_entry() {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  char str[100];
  uint32_t count = 0;
  while (true) {
    if (multicore_doorbell_is_set_current_core(data_ready_doorbell)) {
      //gpio_put(led_pin, !gpio_get(led_pin));
      multicore_doorbell_clear_current_core(data_ready_doorbell);

      //multicore_doorbell_set_other_core(data_processing_doorbell);
      DWT->CYCCNT = 0;
      uint32_t compressed_size = 0;
      size_t dsize = 0;
      // the minute the frame size drops down to like 2000 bytes, the
      // screen goes awry, because the PIO is getting reset during a
      // frame (data transfer outpaces data transmission).
      if (newest_compressed_buffer == 0) {
	dsize = ZSTD_decompress(framebuffer, 76800,
				compressed_buffer0.data,
				compressed_buffer0.compressed_size);

	compressed_size = compressed_buffer0.compressed_size;
      } else if (newest_compressed_buffer == 1) {
	dsize = ZSTD_decompress(framebuffer, 76800,
				compressed_buffer1.data,
				compressed_buffer1.compressed_size);
	compressed_size = compressed_buffer1.compressed_size;
      }
      
      uint32_t c = DWT->CYCCNT;
      // if we get a really short frame, the decompression time is so
      // low that we end up resetting the PIO during a frame, and the
      // screen goes dark. if we instead always make sure that the PIO
      // is done before resetting it, then the screen always holds the
      // correct frame.
      while (dma_channel_is_busy(image_pixels_channel) ||
	     dma_channel_is_busy(image_pixels_zero_channel));
      // after the DMA ends, we have five GCK h/ls to wait for. add
      // one more for good measure
      sleep_us(one_gck_hl_us*6);
      reset_full_frame_pio();
      send_full_frame_image(framebuffer);

      /*sprintf(str, "frame %lu: decompression time for %lu bytes=>%lu bytes: %f\r\n",
	      count, compressed_size, dsize, ((float)c/200e6));
      uart_puts(uart1, str);*/
      //count++;
      //multicore_doorbell_clear_other_core(data_processing_doorbell);
    }
  }

}

void main() {
  // there doesn't appear to be any issues at 150 MHz except for
  // excessive screen tearing sometimes. upping this to 200 MHz means
  // that the display transmission is faster, so tearing is less
  // visible (also, that much of a change should put the display
  // signals out of spec, but that doesn't seem to happen).

  set_sys_clock_hz(sys_clock_hz, true);
  // enable cycle counter
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  // init multicore
  data_ready_doorbell = multicore_doorbell_claim_unused((1 << NUM_CORES) - 1, true);
  multicore_doorbell_clear_current_core(data_ready_doorbell);
  
  multicore_launch_core1(core1_entry);
  
  // init display stuff
  gpio_init(five_volt_en);
  gpio_set_dir(five_volt_en, GPIO_OUT);

  gpio_init(led_pin);
  gpio_set_dir(led_pin, GPIO_OUT);

  // this is a shortened version of the init procedure in sharpie-sw
  gpio_put(five_volt_en, 1);
  sleep_ms(3);

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

  // wait at least two VB/VCOM cycles (at 60 Hz one cycle is 16 ms)
  //
  // Datasheet shows at least 1.5 cycles before sending data
  sleep_ms(60);

  
  // set up debug uart
  gpio_set_function(26, UART_FUNCSEL_NUM(uart1, 26));
  gpio_set_function(27, UART_FUNCSEL_NUM(uart1, 27));
  uart_init(uart1, 115200);
  uart_puts(uart1, "init\r\n");

  char str[100];

  
  tusb_rhport_init_t dev_init = {
    .role = TUSB_ROLE_DEVICE,
    .speed = TUSB_SPEED_AUTO
  };
  board_init();
  tusb_init(0, &dev_init);

  bool frame_in_progress = false;
  uint32_t count = 0;
  uint32_t compressed_size = 0;
  
  init_full_frame_pio();
  image_pixels_channel = dma_claim_unused_channel(true); // true -> required
  if (image_pixels_channel < 0) {
    printf("failed to claim red dma channel\n");
    error_handler();
  }

  image_pixels_zero_channel = dma_claim_unused_channel(true);
  if (image_pixels_zero_channel < 0) {
    printf("failed to claim red dma zero channel\n");
    error_handler();
  }

  compressed_data_copy_channel = dma_claim_unused_channel(true);
  if (compressed_data_copy_channel < 0) {
    printf("failed to claim compressed data copy channel\n");
    error_handler();
  }

  // reset and send take 61 us
  /*DWT->CYCCNT = 0;
  uint32_t c = DWT->CYCCNT;

  sprintf(str, "finished, took %lu cycles = %f sec\r\n",
		c, ((float)c/sys_clock_hz));
  uart_puts(uart1, str);*/
  
  while (1) {
    tud_task();

    if (!tud_vendor_mounted()) {
      continue;
    }

    uint32_t avail = tud_vendor_available();
    
    if (avail != 0) {

      if (!frame_in_progress) {
	frame_in_progress = true;
	DWT->CYCCNT = 0;
	// read initial 4 byte size value
	compressed_size = 0;
	count = 0;
	// start by reading just the number of bytes in this compressed frame
	count += tud_vendor_read(inputbuf, 4);
	memcpy(&compressed_size, inputbuf, 4);
	/*sprintf(str, "going to read %lu bytes\r\n", compressed_size);
	uart_puts(uart1, str);*/
      }

      // then try to read as many as we can get
      count += tud_vendor_read(inputbuf + count, compressed_size);
      if (count == compressed_size + 4) {

	uint32_t end = DWT->CYCCNT;

	// this loop can always be at most one frame ahead of the
	// decompression loop
	
	if (newest_compressed_buffer == 0) {
	  // if we last wrote to 0, use 1
	  //
	  // note that we don't need to copy the count bytes.
	  //
	  // using DMA here would probably save about 20000 cycles,
	  // for maybe a .1-.2 difference in fps
	  memcpy(compressed_buffer1.data, &inputbuf[4], compressed_size);
	  compressed_buffer1.compressed_size = compressed_size;
	  newest_compressed_buffer = 1;
	  multicore_doorbell_set_other_core(data_ready_doorbell);
	  
	} else if (newest_compressed_buffer == 1) {
	  memcpy(compressed_buffer0.data, &inputbuf[4], compressed_size);
	  compressed_buffer0.compressed_size = compressed_size;
	  newest_compressed_buffer = 0;
	  multicore_doorbell_set_other_core(data_ready_doorbell);
	}
	
	uint32_t after_decomp = DWT->CYCCNT;
	/*sprintf(str, "finished, count = %lu, took %lu cycles = %f bytes/sec\r\n",
		count, end, count/((float)end/sys_clock_hz));
	uart_puts(uart1, str);
	 */
	/*sprintf(str, "after copy: %lu cycles (copy took %lu cycles = %f sec) = %f frames/sec\r\n",
		after_decomp, after_decomp - end, ((float)after_decomp - end)/sys_clock_hz, 1/((float)after_decomp/sys_clock_hz));
	uart_puts(uart1, str);*/
	//uart_default_tx_wait_blocking();

	frame_in_progress = false;
      }
    }
  }

  while(true);
  
}
