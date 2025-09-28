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


const int led_pin = 14;
const int five_volt_en = 16;
const int va_pin = 12;
const int vb_vcom_pin = 13;

uint va_vb_vcom_tracker = 1;
uint pwm_slice;

uint32_t global_32bit_zero = 0;

uint8_t red_framebuffer[320][240];
uint8_t* red_framebuffer_1d = (uint8_t*)red_framebuffer;
uint32_t* red_framebuffer_32 = (uint32_t*)red_framebuffer;

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

PIO pio = pio0;
uint vertical_sm = 0;
uint gen_sm = 1;
uint horiz_data_sm = 2;

uint vertical_offset = 0;
uint gen_offset = 0;
uint horiz_data_offset = 0;

void init_sharpie_pios() {
  for (uint i = 0; i < 2; i++) {
    PIO pio = PIO_INSTANCE(i);
    vertical_offset = pio_add_program(pio, &sharpie_vertical_program);
    if (vertical_offset < 0) {
      printf("failed to add sharpie_vertical_program on PIO %u\n", i);
      error_handler();
    }
    
    // init GEN state machine
    gen_offset = pio_add_program(pio, &sharpie_gen_program);
    if (gen_offset < 0) {
      printf("failed to add sharpie_gen_program on PIO %u\n", i);
      error_handler();
    }
    
    
    // init horiz/data state machine
    horiz_data_offset = pio_add_program(pio, &sharpie_horiz_data_program);
    if (horiz_data_offset < 0) {
      printf("failed to add sharpie_horiz_data_program on PIO %u\n", i);
      error_handler();
    }
  }
}

void restart_state_machines() {
  // init all three state machines
  for (uint i = 0; i < 2; i++) {

    PIO pio = PIO_INSTANCE(i);
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

    /*pio_sm_restart(pio, vertical_sm);
  pio_sm_clear_fifos(pio, vertical_sm);
  pio_sm_restart(pio, gen_sm);
  pio_sm_clear_fifos(pio, gen_sm);
  pio_sm_restart(pio, horiz_data_sm);
  pio_sm_clear_fifos(pio, horiz_data_sm);*/

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


// The problem with the messed up waveforms seems to be related to
// stuck states in either the PIO or DMA. We should be able to work
// around this by using all three PIOs. We're also going to claim
// separate DMA channels for each transfer.
void main() {
  // set up the system: initialize pins and PIO
  stdio_init_all();

  // red framebuffer gets white, actually, because we are more confident
  // in what white looks like than in what anything else looks like
  memset(red_framebuffer_1d, 0b111111, 320*240);
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

  init_sharpie_pios();
  
  // all state machines have to be running in lockstep for
  // synchronization via interrupts to work
  // this also charges all appropriate registers
  restart_state_machines();

  
  // TODO: four transfers or less causes the last out data on the pins
  // to be weirdly skewed, something with just R1 and R2. Should we
  // preload the whole FIFO before starting?
  
  
  // configure DMA AFTER we charge the loop registers
  //

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


  // The problem with this code currently is that the first transfer
  // works perfectly, exactly as it did on the RP2040, but the second
  // transfer gets through one (slightly broken) iteration of the
  // outer loop of horiz-data, and then the third transfer mostly
  // works but leaves BCK high at the end.
  
  // this DMA is configured for the initial black screen, to send on PIO 0
  dma_channel_config c = dma_channel_get_default_config(dma_channel);
  channel_config_set_read_increment(&c, false);
  channel_config_set_write_increment(&c, false); // no increment writes (into the FIFO)
  channel_config_set_transfer_data_size(&c, DMA_SIZE_32); // four byte transfers (one byte doesn't work)
  channel_config_set_dreq(&c, pio_get_dreq(pio0, horiz_data_sm, true)); // true for sending data to SM
  channel_config_set_chain_to(&c, dma_channel_zero); // chain to zero channel to start zero channel when this finishes
  dma_channel_configure(dma_channel, &c,
			&pio0->txf[horiz_data_sm], // destination (TX FIFO of SM 2)
			&global_32bit_max,//zero,
			19200, // transfer size = 320*240/4 = 19200
			true); // start now
  


  dma_channel_config c_zero = dma_channel_get_default_config(dma_channel_zero);
  channel_config_set_read_increment(&c_zero, false); // just send zeros
  channel_config_set_write_increment(&c_zero, false); // write into FIFO
  channel_config_set_transfer_data_size(&c_zero, DMA_SIZE_32);
  channel_config_set_dreq(&c_zero, pio_get_dreq(pio0, horiz_data_sm, true));
  dma_channel_configure(dma_channel_zero, &c_zero,
			&pio0->txf[horiz_data_sm],
			&global_32bit_zero,
			240/4, // 240 bytes but 4-byte transfers
			false); // wait for chain start

  // restart all state machine clocks
  pio_clkdiv_restart_sm_mask(pio0, 0b111); // restart state machines
  pio_clkdiv_restart_sm_mask(pio1, 0b111);
  pio_clkdiv_restart_sm_mask(pio2, 0b111);
  
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
  pio0->irq_force = 0b1;

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
  
  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_clkdiv_mode(&cfg, PWM_DIV_FREE_RUNNING);
  pwm_config_set_clkdiv(&cfg, 200);
  pwm_config_set_wrap(&cfg, 6250);
  pwm_set_irq_enabled(pwm_slice, true); // interrupt generated on wrap
  pwm_set_chan_level(pwm_slice, PWM_CHAN_A, 6250-1); // immediately fire the irq
  pwm_init(pwm_slice, &cfg, true); // start now

  printf("VA/VB/VCOM running\n");
  // wait at least two VB/VCOM cycles (at 60 Hz one cycle is 16 ms)
  //
  // they show at least 1.5 cycles before sending data
  sleep_ms(60);

  
  // send red screen

  int dma_channel_red = dma_claim_unused_channel(true); // true -> required
  if (dma_channel_red < 0) {
    printf("failed to claim red dma channel\n");
    error_handler();
  }

  int dma_channel_red_zero = dma_claim_unused_channel(true);
  if (dma_channel_red_zero < 0) {
    printf("failed to claim red dma zero channel\n");
    error_handler();
  }
  
  dma_channel_config red_c = dma_channel_get_default_config(dma_channel_red);
  channel_config_set_read_increment(&red_c, true); // increment reads
  channel_config_set_write_increment(&red_c, false); // no increment writes (into the FIFO)
  channel_config_set_transfer_data_size(&red_c, DMA_SIZE_32); // four byte transfers (one byte doesn't work)
  channel_config_set_dreq(&red_c, pio_get_dreq(pio1, horiz_data_sm, true)); // true for sending data to SM
  channel_config_set_chain_to(&red_c, dma_channel_red_zero); // chain to zero channel to start zero channel when this finishes
  dma_channel_configure(dma_channel_red, &red_c,
			&pio1->txf[horiz_data_sm], // destination (TX FIFO of SM 2)
			red_framebuffer_32, // source 
			19200, // transfer size = 320*240/4 = 19200
			true); // start now



  dma_channel_config red_c_zero = dma_channel_get_default_config(dma_channel_red_zero);
  channel_config_set_read_increment(&red_c_zero, false); // just send zeros
  channel_config_set_write_increment(&red_c_zero, false); // write into FIFO
  channel_config_set_transfer_data_size(&red_c_zero, DMA_SIZE_32);
  channel_config_set_dreq(&red_c_zero, pio_get_dreq(pio1, horiz_data_sm, true));
  dma_channel_configure(dma_channel_red_zero, &red_c_zero,
			&pio1->txf[horiz_data_sm],
			&global_32bit_zero,
			240/4, // 240 bytes but 4-byte transfers
			false); // wait for chain start


  
  // transmit red framebuffer
  pio1->irq_force = 0b1;

  printf("send red framebuffer\n");
  // wait for the frame to transmit
  sleep_ms(60);

  printf("waiting...\n");
  // wait 60 seconds to show off what got transmitted
  //gpio_put(led_pin, 1);
  sleep_ms(1000);//60000);

  // reload state machines to run again
  //restart_state_machines();
  
  // configure final black DMA

  int dma_channel_final_black = dma_claim_unused_channel(true); // true -> required
  if (dma_channel_final_black < 0) {
    printf("failed to claim final black dma channel\n");
    error_handler();
  }

  int dma_channel_final_black_zero = dma_claim_unused_channel(true);
  if (dma_channel_final_black_zero < 0) {
    printf("failed to claim final black dma zero channel\n");
    error_handler();
  }
  
  dma_channel_config final_black_c = dma_channel_get_default_config(dma_channel_final_black);
  channel_config_set_read_increment(&final_black_c, false); // increment reads
  channel_config_set_write_increment(&final_black_c, false); // no increment writes (into the FIFO)
  channel_config_set_transfer_data_size(&final_black_c, DMA_SIZE_32); // four byte transfers (one byte doesn't work)
  channel_config_set_dreq(&final_black_c, pio_get_dreq(pio2, horiz_data_sm, true)); // true for sending data to SM
  channel_config_set_chain_to(&final_black_c, dma_channel_final_black_zero); // chain to zero channel to start zero channel when this finishes
  dma_channel_configure(dma_channel_final_black, &final_black_c,
			&pio2->txf[horiz_data_sm], // destination (TX FIFO of SM 2)
		        &global_32bit_zero,//black_framebuffer, // source 
			19200, // transfer size = 320*240/4 = 19200
			true); // start now



  dma_channel_config final_black_c_zero = dma_channel_get_default_config(dma_channel_final_black_zero);
  channel_config_set_read_increment(&final_black_c_zero, false); // just send zeros
  channel_config_set_write_increment(&final_black_c_zero, false); // write into FIFO
  channel_config_set_transfer_data_size(&final_black_c_zero, DMA_SIZE_32);
  channel_config_set_dreq(&final_black_c_zero, pio_get_dreq(pio2, horiz_data_sm, true));
  dma_channel_configure(dma_channel_final_black_zero, &final_black_c_zero,
			&pio2->txf[horiz_data_sm],
			&global_32bit_zero,
			240/4, // 240 bytes but 4-byte transfers
			false); // wait for chain start

  // and run again


  sleep_ms(4);
  pio2->irq_force = 0b1;
  
  printf("sending black screen\n");
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
