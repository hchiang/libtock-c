#include <stdbool.h>
#include <stdio.h>

#include <adc.h>
#include <timer.h>
#include <clock.h>

#include <ieee802154.h>
#include <udp.h>
#include "fft.h"
#define DEBUG 0
#include "gpio.h"
#include "clock.h"

typedef struct {
  bool fired;
  uint8_t channel;
  uint16_t sample;
  uint32_t length;
  uint16_t* buffer;
  int error;
} adc_data_t;

static void adc_cb(int callback_type,
                    int arg1,
                    int arg2,
                    void* callback_args) {
    adc_data_t* result = (adc_data_t*)callback_args;

  switch (callback_type) {
    case SingleSample:
      result->error   = TOCK_SUCCESS;
      result->channel = arg1;
      result->sample  = arg2;
      break;

    case ContinuousSample:
      result->error   = TOCK_SUCCESS;
      result->channel = arg1;
      result->sample  = arg2;
      break;

    case SingleBuffer:
      result->error   = TOCK_SUCCESS;
      result->channel = (arg1 & 0xFF);
      result->length  = ((arg1 >> 8) & 0xFFFFFF);
      result->buffer  = (uint16_t*)arg2;
      break;

    case ContinuousBuffer:
      result->error   = TOCK_SUCCESS;
      result->channel = (arg1 & 0xFF);
      result->length  = ((arg1 >> 8) & 0xFFFFFF);
      result->buffer  = (uint16_t*)arg2;
      break;

    default:
      result->error = TOCK_FAIL;
      break;
  }

  result->fired = true;
}

static unsigned char BUF_BIND_CFG[2 * sizeof(sock_addr_t)];
static bool timer_called;
static void timer_cb (__attribute__ ((unused)) int arg0,
                      __attribute__ ((unused)) int arg1,
                      __attribute__ ((unused)) int arg2,
                      __attribute__ ((unused)) void* userdata) {
    timer_called = true;
}

float movingAvg(float prev_avg, int num_samples, int new_val);
float movingAvg(float prev_avg, int num_samples, int new_val)
{
  return prev_avg + ((float)(new_val) - prev_avg) / num_samples;
}

#define ADC_SAMPLES 4000
static uint16_t adc_buffer[ADC_SAMPLES];
int main(void) {
  gpio_enable_output(0);
  gpio_enable_output(1);
  gpio_enable_output(2);

  if (DEBUG) {
    printf("Starting Power Clocks Test App.\n");
  }

  char packet[150];

  ieee802154_set_pan(0xABCD);
  ieee802154_config_commit();
  ieee802154_up();

  ipv6_addr_t ifaces[10];
  udp_list_ifaces(ifaces, 10);

  sock_handle_t handle;
  sock_addr_t addr = {
    ifaces[0],
    15123
  };

  if (DEBUG) {
    printf("Opening socket.\n");
  }
  int bind_return = udp_bind(&handle, &addr, BUF_BIND_CFG);

  if (bind_return < 0) {
    printf("Failed to bind to port: failure=%d\n", bind_return);
    return -1;
  } else if (DEBUG) {
    printf("Done binding. \n");
  }

  sock_addr_t destination = {
    ifaces[1],
    16123
  };

  tock_timer_t timer;
  timer_every(2000, timer_cb, NULL, &timer);

  // Setup ADC
  uint8_t channel = 0;
  uint32_t freq = 31500;
  uint16_t length = ADC_SAMPLES;
  adc_data_t result = {0};
  adc_set_callback(adc_cb, (void*) &result);
  adc_set_buffer(adc_buffer, length);


  uint16_t buffer_idx = 0;
  unsigned num_averages = 1;
  volatile double avg_buffer[25];
  int fft_buf[16];
  int fft_mag[8];
  float avg_fft_mag[8]; //For each frequency bin, keep moving average of magnitude

  //clock_set(DFLL);
  while (1) {
    if (DEBUG) {
      printf("About to sample...\n");
    }

    //clock_set(RCFAST4M);
    gpio_toggle(1);
    result.fired = false;
    int err = adc_buffered_sample(channel, freq);
    if (err < 0) {
      printf("Error sampling ADC: %d\n", err);
    } else {
      if (DEBUG) {
        printf("first sample: %d\n", adc_buffer[0]);
      }
    }
    yield_for(&result.fired);
    //clock_set(DFLL);
    gpio_toggle(1);
    adc_stop_sampling();


    //uint32_t before = alarm_read();
    // Begin computation of average
    // Basic Average code (proxy for magnitude of signal)
    int i;
    /*
    long sum = 0;
    for (i=0; i<length; i++) {
        //adc_buffer[i] = adc_buffer[i] * 2 - 200; // Mock conversion to real-world units
        sum += adc_buffer[i];
    }
    avg_buffer[buffer_idx++] = ((double)sum)/length;
    if (DEBUG) {
      printf("Average of last %d samples: %f\n", ADC_SAMPLES, avg_buffer[buffer_idx - 1]);
    }*/
    buffer_idx++;

    // Begin fft computation on sets of 16 samples
    int k;
    for (k=0; k<length/16; k++) {
      for (i=k*16; i<(k+1)*16; i++) {
        fft_buf[i % 16] = adc_buffer[i]; // Copy needed bc fft alg I found is int not uint16
      }
      fft(fft_buf, fft_mag);
      int l;
      // For each returned fft magnitude, update the moving average for that magnitude bin
      for (l=3; l<8; l++) {
        avg_fft_mag[l] = movingAvg(avg_fft_mag[l], k, fft_mag[l]);
      }
    }

    //uint32_t after = alarm_read();

    //uint32_t elapsed = after - before;
    //printf("Elapsed computation time: %d\n", elapsed);
    // End computation

    //Keep sampling, computing averages on new buffers until num_averages reached
    if (buffer_idx < num_averages)
        continue;
    buffer_idx = 0;

    // Payload: 6 averaged fft magnitudes + all sample average
    unsigned int max_tx_len = udp_get_max_tx_len();
    unsigned int payload_len = 6*sizeof(float) + sizeof(double);
    //payload_len = 50;  // tmp to extend packet size to multiple frames
    if (payload_len > sizeof(packet)){
        payload_len = sizeof(packet);
    }

    memcpy(packet, (void*)&avg_fft_mag[2], payload_len - sizeof(double));
    memcpy(packet, (void*)&avg_buffer, sizeof(double));

    if (payload_len > max_tx_len) {
      printf("Cannot send packets longer than %d bytes without changing"
             " constants in kernel\n", max_tx_len);
      return 0;
    }
    if (DEBUG) {
      printf("Sending packet (length %d) --> \n", payload_len);
    }
    //clock_set(RCFAST4M);
    gpio_toggle(1);
    ssize_t result = udp_send_to(packet, payload_len, &destination);
    //clock_set(DFLL);
    gpio_toggle(1);

    switch (result) {
      case TOCK_SUCCESS:
        if (DEBUG) {
          printf("Packet sent.\n\n");
        }
        break;
      default:
        if (DEBUG) {
          printf("Error sending packet %d\n\n", result);
        }
        break;
    }

    yield_for(&timer_called);
    timer_called = false;
  }
}