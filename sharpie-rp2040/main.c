#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "sharpie-vertical.pio.h"
#include "sharpie-gen.pio.h"
#include "sharpie-horiz-data.pio.h"

// Display is 320x240, one byte per pixel. A "formatted framebuffer"
// is data that can be DMAed directly to the horiz_data state machine,
// with data aligned like this:
//         MSBs of pixels                                     LSBs of pixels
// 0bB2B1G2G1R2R1    0bB4B3G4G3R4R3  (118 more)     0bB2B1G2G1R2R1    0bB4B3G4G3R4R3  (118 more)
uint8_t formatted_framebuffer[320][240];
uint32_t* formatted_framebuffer_32 = (uint32_t*)(formatted_framebuffer);

uint32_t global_32bit_zero = 0;

const int BUTTON_PIN = 16;
const int LED_PIN = 25;

void error_handler() {
  while (1) {
    gpio_put(LED_PIN, 1);
    sleep_ms(500);
    gpio_put(LED_PIN, 0);
    sleep_ms(500);
  }
}

int main() {
  stdio_init_all();

  memset(formatted_framebuffer, 0, sizeof(formatted_framebuffer));
  // init formatted framebuffer with some data that shows up nicely on the scope
  for (uint32_t i = 0; i < 320; i++) { // 10 rows is enough to start with
    for (uint32_t j = 0; j < 240; j += 2) { // whole width of row
      formatted_framebuffer[i][j] = 0xff;
      formatted_framebuffer[i][j+1] = 0xff;//0x00;
    }
  }
  // init detection button
  gpio_init(BUTTON_PIN);
  gpio_set_dir(BUTTON_PIN, false); // input
  gpio_pull_down(BUTTON_PIN);

  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, true); // output
  
  // set system clock to 150 MHz to simulate a RP2350
  set_sys_clock_khz(150000, true);

  PIO pio = pio0;
  uint vertical_sm = 0;
  uint gen_sm = 1;
  uint horiz_data_sm = 2;
  
  uint offset = pio_add_program(pio, &sharpie_vertical_program);
  if (offset < 0) {
    printf("failed to add sharpie_vertical_program");
    error_handler();
  }
  
  // INTB on pin 0, GSP on pin 1, GCK on pin 2
  sharpie_vertical_pio_init(pio, vertical_sm, offset, 0);

  // vertical SM is running now, it will wait until we enable irq 0
  
  // the number you put on the FIFO is the number of times the loop
  // will run minus 1
  pio_sm_put(pio0, vertical_sm, 321); // run 321 times for 648 h/l total


  // init GEN state machine
  offset = pio_add_program(pio, &sharpie_gen_program);
  if (offset < 0) {
    printf("failed to add sharpie_gen_program");
    error_handler();
  }
  // GEN on pin 3, start state machine
  sharpie_gen_pio_init(pio, gen_sm, offset, 3);

  // init horiz/data state machine
  offset = pio_add_program(pio, &sharpie_horiz_data_program);
  if (offset < 0) {
    printf("failed to add sharpie_horiz_data_program");
    error_handler();
  }
  



  
  // BSP on pin 4, BCK on pin 5, data from pin 6 to 11 inclusive
  sharpie_horiz_data_pio_init(pio, horiz_data_sm, offset, 4, 6);
  
 
  // TODO: four transfers or less causes the last out data on the pins
  // to be weirdly skewed, something with just R1 and R2. Should we
  // preload the whole FIFO before starting?
  
  // Configure counters for GEN state machine and horiz-data state machine

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


  int dma_channel = dma_claim_unused_channel(true); // true -> required
  if (dma_channel < 0) {
    printf("failed to claim dma channel\n");
    error_handler();
  }

  // DMA chaining is instantaneous, this does not affect timing.
  int dma_channel_zero = dma_claim_unused_channel(true);
  if (dma_channel_zero < 0) {
    printf("failed to claim dma zero channel\n");
    error_handler();
  }

  // configure DMA AFTER we charge the loop registers
  dma_channel_config c = dma_channel_get_default_config(dma_channel);
  channel_config_set_read_increment(&c, true); // increment reads (from the formatted framebuffer)
  channel_config_set_write_increment(&c, false); // no increment writes (into the FIFO)
  channel_config_set_transfer_data_size(&c, DMA_SIZE_32); // four byte transfers (one byte doesn't work)
  channel_config_set_dreq(&c, pio_get_dreq(pio, horiz_data_sm, true)); // true for sending data to SM
  channel_config_set_chain_to(&c, dma_channel_zero); // chain to zero channel to start zero channel when this finishes
  dma_channel_configure(dma_channel, &c,
			&pio->txf[horiz_data_sm], // destination (TX FIFO of SM 2)
			formatted_framebuffer_32, // source (formatted framebuffer)
			19200, // transfer size = 320*240/4 = 19200
			true); // start now



  dma_channel_config c_zero = dma_channel_get_default_config(dma_channel_zero);
  channel_config_set_read_increment(&c_zero, false); // just send zeros
  channel_config_set_write_increment(&c_zero, false); // write into FIFO
  channel_config_set_transfer_data_size(&c_zero, DMA_SIZE_32);
  channel_config_set_dreq(&c_zero, pio_get_dreq(pio, horiz_data_sm, true));
  dma_channel_configure(dma_channel_zero, &c_zero,
			&pio->txf[horiz_data_sm],
			&global_32bit_zero,
			240/4, // 240 bytes but 4-byte transfers
			false); // wait for chain start
  
  // all state machines have to be running in lockstep for
  // synchronization via interrupts to work
  pio_clkdiv_restart_sm_mask(pio, 0b111); // restart SM 0, 1, and 2
  while (true) {
    if (gpio_get(BUTTON_PIN)) {
      printf("setting irq\n");
      // enable irq 0 in pio 0 to start sm 0 running
      pio0->irq_force = 0b1;
      //pio0->irq_force = 0b100; // set IRQ 2
      break;
    }
  }

  while (true);
}

    
