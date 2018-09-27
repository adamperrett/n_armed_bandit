//
//  bkout.c
//  BreakOut
//
//  Created by Steve Furber on 26/08/2016.
//  Copyright © 2016 Steve Furber. All rights reserved.
//
// Standard includes
#include <stdbool.h>
#include <stdint.h>

// Spin 1 API includes
#include <spin1_api.h>

// Common includes
#include <debug.h>

// Front end common includes
#include <data_specification.h>
#include <simulation.h>

#include <recording.h>

//----------------------------------------------------------------------------
// Macros
//----------------------------------------------------------------------------
// **TODO** many of these magic numbers should be passed from Python
// Game dimension constants
#define GAME_WIDTH  160
#define GAME_HEIGHT 128

#define BRICK_WIDTH  10
#define BRICK_HEIGHT 6

#define BRICK_LAYER_OFFSET 30
#define BRICK_LAYER_HEIGHT 12
#define BRICK_LAYER_WIDTH 160


#define NUMBER_OF_LIVES 5
#define SCORE_DOWN_EVENTS_PER_DEATH 5


#define BRICKS_PER_ROW  (BRICK_LAYER_WIDTH / BRICK_WIDTH)
#define BRICKS_PER_COLUMN  (BRICK_LAYER_HEIGHT / BRICK_HEIGHT)



// Ball outof play time (frames)
#define OUT_OF_PLAY 100

// Frame delay (ms)
#define FRAME_DELAY 20 //14//20

// ball position and velocity scale factor
#define FACT 16

//----------------------------------------------------------------------------
// Enumerations
//----------------------------------------------------------------------------
typedef enum
{
  REGION_SYSTEM,
  REGION_BREAKOUT,
  REGION_RECORDING,
} region_t;

typedef enum
{
  COLOUR_HARD       = 0x8,
  COLOUR_SOFT       = 0x0,
  COLOUR_BRICK      = 0x10,

  COLOUR_BACKGROUND = COLOUR_SOFT | 0x1,
  COLOUR_BAT        = COLOUR_HARD | 0x6,
  COLOUR_BALL       = COLOUR_HARD | 0x7,
  COLOUR_SCORE      = COLOUR_SOFT | 0x6,
  COLOUR_BRICK_ON   = COLOUR_BRICK | 0x0,
  COLOUR_BRICK_OFF  = COLOUR_BRICK | 0x1
} colour_t;

typedef enum
{
  KEY_LEFT  = 0x0,
  KEY_RIGHT = 0x1,
} key_t;

typedef enum
{
  SPECIAL_EVENT_SCORE_UP,
  SPECIAL_EVENT_SCORE_DOWN,
  SPECIAL_EVENT_MAX,
} special_event_t;

//----------------------------------------------------------------------------
// Globals
//----------------------------------------------------------------------------


//! Should simulation run for ever? 0 if not
static uint32_t infinite_run;

static uint32_t _time;
uint32_t pkt_count;


// initial ball coordinates in fixed-point
static int x = (GAME_WIDTH / 4) * FACT;
static int y = (GAME_HEIGHT - GAME_HEIGHT /8) * FACT;

static int current_number_of_bricks;

static bool bricks[BRICKS_PER_COLUMN][BRICKS_PER_ROW];
bool print_bricks  = true;

int brick_corner_x=-1, brick_corner_y=-1;
int number_of_lives = NUMBER_OF_LIVES;

// initial ball velocity in fixed-point
static int u = 1 * FACT;
static int v = -1 * FACT;

// bat LHS x position
static int x_bat   = 40;

// bat length in pixels
static int bat_len = 16;

// frame buffer: 160 x 128 x 4 bits: [hard/soft, R, G, B]
static int frame_buff[GAME_WIDTH / 8][GAME_HEIGHT];

// control pause when ball out of play
static int out_of_play = 0;

// state of left/right keys
static int keystate = 0;

//! The upper bits of the key value that model should transmit with
static uint32_t key;


//! the number of timer ticks that this model should run for before exiting.
uint32_t simulation_ticks = 0;

//! How many ticks until next frame
static uint32_t tick_in_frame = 0;

uint32_t left_key_count = 0;
uint32_t right_key_count = 0;
uint32_t move_count_r = 0;
uint32_t move_count_l = 0;
uint32_t score_change_count=0;
int32_t current_score = 0;

//ratio used in randomising initial x coordinate
static uint32_t x_ratio=UINT32_MAX/(GAME_WIDTH);


//----------------------------------------------------------------------------
// Inline functions
//----------------------------------------------------------------------------
static inline void add_score_up_event()
{
  spin1_send_mc_packet(key | (SPECIAL_EVENT_SCORE_UP), 0, NO_PAYLOAD);
  log_debug("Score up");
  current_score++;
}

static inline void add_score_down_event()
{
  spin1_send_mc_packet(key | (SPECIAL_EVENT_SCORE_DOWN), 0, NO_PAYLOAD);
  log_debug("Score down");
  current_score--;
}

void add_event(int i, int j, colour_t col, bool bricked)
{
  const uint32_t colour_bit = (col == COLOUR_BACKGROUND) ? 0 : 1;
  const uint32_t spike_key = key | (SPECIAL_EVENT_MAX + (i << 10) + (j << 2) + (bricked<<1) + colour_bit);

  spin1_send_mc_packet(spike_key, 0, NO_PAYLOAD);
  log_debug("%d, %d, %u, %08x", i, j, col, spike_key);
}

// gets pixel colour from within word
static inline colour_t get_pixel_col (int i, int j)
{
  return (colour_t)(frame_buff[i / 8][j] >> ((i % 8)*4) & 0xF);
}

// inserts pixel colour within word
static inline void set_pixel_col (int i, int j, colour_t col, bool bricked)
{
    if (bricked) {
        add_event((brick_corner_x * BRICK_WIDTH),
                      (brick_corner_y* BRICK_HEIGHT + BRICK_LAYER_OFFSET),
                      COLOUR_BACKGROUND, bricked);
    }
    else if (col != get_pixel_col(i, j))
    {
      /*  //just update bat pixels in game frame
        if (j==GAME_HEIGHT-1)
        {
            frame_buff[i / 8][j] = (frame_buff[i / 8][j] & ~(0xF << ((i % 8) * 4))) | ((int)col << ((i % 8)*4));
        }
        else
        {
            frame_buff[i / 8][j] = (frame_buff[i / 8][j] & ~(0xF << ((i % 8) * 4))) | ((int)col << ((i % 8)*4));
            add_event (i, j, col);
        }*/
        frame_buff[i / 8][j] = (frame_buff[i / 8][j] & ~(0xF << ((i % 8) * 4))) | ((int)col << ((i % 8)*4));
        add_event (i, j, col, bricked);
    }
}

static inline bool is_a_brick(int x, int y) // x - width, y- height?
{
    int pos_x=0, pos_y=0;

    if ( y >= BRICK_LAYER_OFFSET && y < BRICK_LAYER_OFFSET + BRICK_LAYER_HEIGHT) {
        pos_x = x / BRICK_WIDTH;
        pos_y = (y - BRICK_LAYER_OFFSET) / BRICK_HEIGHT;
        bool val = bricks[pos_y][pos_x];
//        if (pos_y>= BRICKS_PER_COLUMN) {
//            log_error("%d", pos_y);
//            rt_error(RTE_SWERR);
//        }
//        if (pos_x>= BRICKS_PER_ROW) {
//            log_error("%d", pos_x);
//            rt_error(RTE_SWERR);
//        }
        bricks[pos_y][pos_x] = false;
        if (val) {
            brick_corner_x = pos_x;
            brick_corner_y = pos_y;
            current_number_of_bricks--;
        }
        else {
            brick_corner_x = -1;
            brick_corner_y = -1;
        }


//        log_info("%d %d %d %d", x, y, pos_x, pos_y);
        return val;
    }
    brick_corner_x = -1;
    brick_corner_y = -1;
    return false;
}



//----------------------------------------------------------------------------
// Static functions
//----------------------------------------------------------------------------
// initialise frame buffer to blue
static void init_frame ()
{
  for (int i=0; i<(GAME_WIDTH/8); i++)
  {
    for (int j=0; j<GAME_HEIGHT; j++)
    {
      frame_buff[i][j] = 0x11111111 * COLOUR_BACKGROUND;
    }
  }

  for (int i =0; i<BRICKS_PER_COLUMN; i++)
    for (int j=0; j<BRICKS_PER_ROW; j++) {
        bricks[i][j] = true;
        }
  current_number_of_bricks = BRICKS_PER_COLUMN * BRICKS_PER_ROW;
}

static void update_frame ()
{
// draw bat
  // Cache old bat position
  const uint32_t old_xbat = x_bat;
  int move_direction;
  if (right_key_count > left_key_count){
    move_direction = KEY_RIGHT;
    move_count_r++;
//    log_info("moved right");
  }
  else if (left_key_count > right_key_count){
    move_direction = KEY_LEFT;
    move_count_l++;
//    log_info("moved left");
  }
  else{
    move_direction = 2;
//    log_info("didn't move!");
  }


  // Update bat and clamp
  if (move_direction == KEY_LEFT && --x_bat < 0)
  {

    x_bat = 0;
  }
  else if (move_direction == KEY_RIGHT && ++x_bat > GAME_WIDTH-bat_len-1)
  {
    x_bat = GAME_WIDTH-bat_len-1;
  }



  // Clear keystate
  left_key_count = 0;
  right_key_count = 0;

  // If bat's moved
  if (old_xbat != x_bat)
  {
    // Draw bat pixels
    for (int i = x_bat; i < (x_bat + bat_len); i++)
    {
      set_pixel_col(i, GAME_HEIGHT-1, COLOUR_BAT, false);
    }



    // Remove pixels left over from old bat
    if (x_bat > old_xbat)
    {
      set_pixel_col(old_xbat, GAME_HEIGHT-1, COLOUR_BACKGROUND, false);
    }
    else if (x_bat < old_xbat)
    {
      set_pixel_col(old_xbat + bat_len, GAME_HEIGHT-1, COLOUR_BACKGROUND, false);
    }

   //only draw left edge of bat pixel
   // add_event(x_bat, GAME_HEIGHT-1, COLOUR_BAT);
   //send off pixel to network (ignoring game frame buffer update)
   // add_event (old_xbat, GAME_HEIGHT-1, COLOUR_BACKGROUND);
  }

// draw ball
  if (out_of_play == 0)
  {
    // clear pixel to background
    set_pixel_col(x/FACT, y/FACT, COLOUR_BACKGROUND, false);

    // move ball in x and bounce off sides
    x += u;
    if (x < -u)
    {
//      log_info("OUT 1");
      u = -u;
    }
    if (x >= ((GAME_WIDTH*FACT)-u))
    {
//      log_info("OUT 2 x = %d, u = %d, gw = %d, fact = %d", x, u, GAME_WIDTH, FACT);
      u = -u;
    }

    // move ball in y and bounce off top
    y += v;
    // if ball entering bottom row, keep it out XXX SD
    if (y == GAME_HEIGHT-1)
    {
      y = GAME_HEIGHT;
    }
    if (y < -v)
    {
      v = -v;
    }

//detect collision
    // if we hit something hard! -- paddle or brick
    bool bricked = is_a_brick(x/ FACT, y/ FACT);

    if ( bricked ) {
        int brick_x = brick_corner_x * BRICK_WIDTH;
        int brick_y = (brick_corner_y* BRICK_HEIGHT + BRICK_LAYER_OFFSET);
//        log_info("x-brick_x = %d, %d %d",x/FACT - brick_x, x/FACT, brick_x);
//        log_info("y-brick_y = %d, %d %d",y/FACT - brick_y, y/FACT, brick_y);

        if ( brick_x == x/FACT && u > 0){
            u = -u;
        }
        else if (x/FACT == brick_x + BRICK_WIDTH - 1 && u < 0){
            u = -u;
        }
        if (brick_y  == y/FACT && v > 0){
            v = -v;
        }
        if (y/FACT ==  brick_y + BRICK_HEIGHT - 1 && v < 0){
            v = -v;
        }

        set_pixel_col(x/FACT, y/FACT, COLOUR_BACKGROUND, bricked);

        bricked= false;
        // Increase score
        add_score_up_event();
    }


    if (get_pixel_col(x / FACT, y / FACT) & COLOUR_HARD && y > GAME_HEIGHT*(FACT / 2))
    {
        bool broke = false;
      if (x/FACT < (x_bat+bat_len/4))
      {
//        log_info("BAT 1");
        u = -FACT;
      }
      else if (x/FACT < (x_bat+bat_len/2))
      {
//        log_info("BAT 2");
        u = -FACT/2;
      }
      else if (x/FACT < (x_bat+3*bat_len/4))
      {
//        log_info("BAT 3");
        u = FACT/2;
      }
      else if (x/FACT < (x_bat+bat_len))
      {
//        log_info("BAT 4");
        u = FACT;
      }
      else
      {
        log_info("Broke bat 0x%x", (frame_buff[(x/FACT) / 8][y/FACT] >> ((x/FACT % 8)*4) & 0xF));
        broke = true;
//        u = FACT;
      }

//     if (bricked) {
//        set_pixel_col(x/FACT, y/FACT, COLOUR_BACKGROUND, bricked);
//     }
        if (broke == false)
        {
          v = -FACT;
          y -= FACT;
        }
      // Increase score
//      add_score_up_event();
    }

// lost ball
    if (y >= (GAME_HEIGHT*FACT-v))
    {
      v = -1 * FACT;
      y = (GAME_HEIGHT - GAME_HEIGHT /8) * FACT;

      if(mars_kiss32() > 0xFFFF){
//        log_info("MARS 1");
        u = -u;
      }

      //randomises initial x location
      x = GAME_WIDTH;

      while (x >= GAME_WIDTH)
         x = (int)(mars_kiss32()/x_ratio);
//      x = (int)(mars_kiss32()%GAME_WIDTH);
//      log_info("random x = %d", x);
      x *= FACT;

      out_of_play = OUT_OF_PLAY;
      // Decrease score
      number_of_lives--;
      if (!number_of_lives){
        for(int i=0; i<SCORE_DOWN_EVENTS_PER_DEATH;i++) {
            add_score_down_event();
        }
        number_of_lives = NUMBER_OF_LIVES;
      }
      else {
        add_score_down_event();
      }
    }
    // draw ball
    else
    {
      set_pixel_col(x/FACT, y/FACT, COLOUR_BALL, false);
    }
  }
  else
  {
    --out_of_play;
  }
}

static bool initialize(uint32_t *timer_period)
{
  log_info("Initialise breakout: started");

  // Get the address this core's DTCM data starts at from SRAM
  address_t address = data_specification_get_data_address();

  // Read the header
  if (!data_specification_read_header(address))
  {
      return false;
  }
/*
    simulation_initialise(
        address_t address, uint32_t expected_app_magic_number,
        uint32_t* timer_period, uint32_t *simulation_ticks_pointer,
        uint32_t *infinite_run_pointer, int sdp_packet_callback_priority,
        int dma_transfer_done_callback_priority)
*/
  // Get the timing details and set up thse simulation interface
  if (!simulation_initialise(data_specification_get_region(REGION_SYSTEM, address),
    APPLICATION_NAME_HASH, timer_period, &simulation_ticks,
    &infinite_run, 1, NULL))
  {
      return false;
  }
  log_info("simulation time = %u", simulation_ticks);


  // Read breakout region
  address_t breakout_region = data_specification_get_region(REGION_BREAKOUT, address);
  key = breakout_region[0];
  log_info("\tKey=%08x", key);
  log_info("\tTimer period=%d", *timer_period);

    //get recording region
   address_t recording_address = data_specification_get_region(
                                       REGION_RECORDING,address);
   // Setup recording
   uint32_t recording_flags = 0;
   if (!recording_initialize(recording_address, &recording_flags))
   {
       rt_error(RTE_SWERR);
       return false;
   }

  log_info("Initialise: completed successfully");

  return true;
}

//----------------------------------------------------------------------------
// Callbacks
//----------------------------------------------------------------------------
// incoming SDP message
/*void process_sdp (uint m, uint port)
*{
    sdp_msg_t *msg = (sdp_msg_t *) m;

    io_printf (IO_BUF, "SDP len %d, port %d - %s\n", msg->length, port, msg->data);
    // Port 1 - key data
    if (port == 1) spin1_memcpy(&keystate, msg->data, 4);
    spin1_msg_free (msg);
    if (port == 7) spin1_exit (0);
}*/

void resume_callback() {
    recording_reset();
}

void timer_callback(uint unused, uint dummy)
{
  use(unused);
  use(dummy);
  // If a fixed number of simulation ticks are specified and these have passed
  //
//  ticks++;
    //this makes it count twice, WTF!?

  _time++;
  score_change_count++;

//   if (!current_number_of_bricks) {
//        for (int i =0; i<BRICKS_PER_COLUMN; i++)
//            for (int j=0; j<BRICKS_PER_ROW; j++) {
//                bricks[i][j] = true;
//                }
//          current_number_of_bricks = BRICKS_PER_COLUMN * BRICKS_PER_ROW;
//          print_bricks = true;
//          v = -1 * FACT;
//      y = (GAME_HEIGHT - GAME_HEIGHT /8) * FACT;
//
//      if(mars_kiss32() > 0xFFFF){
//        u = -u;
//      }
//
//      //randomises initial x location
//      x = GAME_WIDTH;
//
//      while (x >= GAME_WIDTH)
//         x = (int)(mars_kiss32()/x_ratio);
////      x = (int)(mars_kiss32()%GAME_WIDTH);
////      log_info("random x = %d", x);
//      x *= FACT;
//   }
//
//   if (print_bricks) {
//    print_bricks = false;
//    for (int i =0; i<BRICKS_PER_COLUMN; i++)
//        for (int j=0; j<BRICKS_PER_ROW; j++) {
//            if (bricks[i][j]) {
//                add_event(j * BRICK_WIDTH,
//                              i* BRICK_HEIGHT + BRICK_LAYER_OFFSET,
//                              COLOUR_BRICK_ON, true);
//
//            }
//        }
//    log_info("printed bricks");
//   }

  if (!infinite_run && _time >= simulation_ticks)
  {
    //spin1_pause();
    recording_finalise();
    // go into pause and resume state to avoid another tick
    simulation_handle_pause_resume(resume_callback);
//    spin1_callback_off(MC_PACKET_RECEIVED);

    log_info("move count Left %u", move_count_l);
    log_info("move count Right %u", move_count_r);
    log_info("infinite_run %d; time %d",infinite_run, _time);
    log_info("simulation_ticks %d",simulation_ticks);
//    log_info("key count Left %u", left_key_count);
//    log_info("key count Right %u", right_key_count);

    log_info("Exiting on timer.");
    simulation_handle_pause_resume(NULL);

    _time -= 1;
    return;
  }
  // Otherwise
  else
  {
    // Increment ticks in frame counter and if this has reached frame delay
    tick_in_frame++;
    if(tick_in_frame == FRAME_DELAY)
    {
      if (!current_number_of_bricks) {
        for (int i =0; i<BRICKS_PER_COLUMN; i++)
            for (int j=0; j<BRICKS_PER_ROW; j++) {
                bricks[i][j] = true;
                }
          current_number_of_bricks = BRICKS_PER_COLUMN * BRICKS_PER_ROW;
//          print_bricks = true;
          v = -1 * FACT;
      y = (GAME_HEIGHT - GAME_HEIGHT /8) * FACT;

      if(mars_kiss32() > 0xFFFF){
//        log_info("MARS 2");
        u = -u;
      }

      //randomises initial x location
      x = GAME_WIDTH;

      while (x >= GAME_WIDTH)
         x = (int)(mars_kiss32()/x_ratio);
//      x = (int)(mars_kiss32()%GAME_WIDTH);
//      log_info("random x = %d", x);
      x *= FACT;
      }

//       if (print_bricks) {
//        print_bricks = false;
        for (int i =0; i<BRICKS_PER_COLUMN; i++)
            for (int j=0; j<BRICKS_PER_ROW; j++) {
                if (bricks[i][j]) {
                    add_event(j * BRICK_WIDTH,
                                  i* BRICK_HEIGHT + BRICK_LAYER_OFFSET,
                                  COLOUR_BRICK_ON, true);

                }
            }
//        log_info("printed bricks");
//       }
      //log_info("pkts: %u   L: %u   R: %u", pkt_count, move_count_l, move_count_r);
      // If this is the first update, draw bat as
      // collision detection relies on this
      if(_time == FRAME_DELAY)
      {
        // Draw bat
        for (int i = x_bat; i < (x_bat + bat_len); i++)
        {
          set_pixel_col(i, GAME_HEIGHT-1, COLOUR_BAT, false);
        }
      }

      // Reset ticks in frame and update frame
      tick_in_frame = 0;
      //print_bricks = true;
      update_frame();
      // Update recorded score every 10s
      if(score_change_count>=10000){
        recording_record(0, &current_score, 4);
        score_change_count=0;
      }
    }
  }
//  log_info("time %u", ticks);
//  log_info("time %u", _time);
}

void mc_packet_received_callback(uint key, uint payload)
{
  use(payload);
//  log_info("mc pack in %u", key);

 /* uint stripped_key = key & 0xFFFFF;
  pkt_count++;

  // Left
  if(stripped_key & KEY_LEFT)
  {
    left_key_count++;
  }
  // Right
  else if (stripped_key & KEY_RIGHT)
  {
    right_key_count++;
  }*/
/*
  if(key & KEY_RIGHT){
    right_key_count++;
  }
  // Left
//  if(key & KEY_LEFT){
//  else{
  else if(key & KEY_LEFT){
    left_key_count++;
  }
//  else
/*/
  // Right
  if(key & KEY_RIGHT){
    right_key_count++;
  }
  else {
//  else{
    left_key_count++;
  }
//*/
//  log_info("mc key %u, L %u, R %u", key, left_key_count, right_key_count);
}
//-------------------------------------------------------------------------------

INT_HANDLER sark_int_han (void);


void rte_handler (uint code)
{
  // Save code

  sark.vcpu->user0 = code;
  sark.vcpu->user1 = (uint) sark.sdram_buf;

  // Copy ITCM to SDRAM

  sark_word_cpy (sark.sdram_buf, (void *) ITCM_BASE, ITCM_SIZE);

  // Copy DTCM to SDRAM

  sark_word_cpy (sark.sdram_buf + ITCM_SIZE, (void *) DTCM_BASE, DTCM_SIZE);

  // Try to re-establish consistent SARK state

  sark_vic_init ();

  sark_vic_set ((vic_slot) sark_vec->sark_slot, CPU_INT, 1, sark_int_han);

  uint *stack = sark_vec->stack_top - sark_vec->svc_stack;

  stack = cpu_init_mode (stack, IMASK_ALL+MODE_IRQ, sark_vec->irq_stack);
  stack = cpu_init_mode (stack, IMASK_ALL+MODE_FIQ, sark_vec->fiq_stack);
  (void)  cpu_init_mode (stack, IMASK_ALL+MODE_SYS, 0);

  cpu_set_cpsr (MODE_SYS);

  // ... and sleep

  while (1)
    cpu_wfi ();
}

//-------------------------------------------------------------------------------

//----------------------------------------------------------------------------
// Entry point
//----------------------------------------------------------------------------
void c_main(void)
{
  // Load DTCM data
  uint32_t timer_period;
  if (!initialize(&timer_period))
  {
    log_error("Error in initialisation - exiting!");
    rt_error(RTE_SWERR);
    return;
  }

  init_frame();
  keystate = 0; // IDLE
  tick_in_frame = 0;
  pkt_count = 0;

  // Set timer tick (in microseconds)
  log_info("setting timer tick callback for %d microseconds",
              timer_period);
  spin1_set_timer_tick(timer_period);
  log_info("bricks %x", &bricks);

  log_info("simulation_ticks %d",simulation_ticks);

  // Register callback
  spin1_callback_on(TIMER_TICK, timer_callback, 2);
  spin1_callback_on(MC_PACKET_RECEIVED, mc_packet_received_callback, -1);

  _time = UINT32_MAX;

  simulation_run();




}
