#include <sys/socket.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>

#include <bcm2835.h>

#define UDP_RECV_PORT	1777
#define UDP_SEND_PORT	1778
#define POLL_HZ		40

#define HOOK_PIN	24
#define	BELL_PIN	23

uint8_t keyboard_pins[7] = {
  19,
  16,
  13,
  12,
  6,
  5,
  25
};

int
disable_swapping()
{
  struct sched_param sp;
  memset(&sp, 0, sizeof(sp));

  sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
  if (sched_setscheduler(0, SCHED_FIFO, &sp)) {
    perror("sched_setscheduler");
    exit(1);
  }
  if (mlockall(MCL_CURRENT | MCL_FUTURE)) {
    perror("mlockall");
    exit(1);
  }
}

void
define_pin_as_input(uint8_t pin)
{
  // define pin as input
  bcm2835_gpio_fsel(pin, BCM2835_GPIO_FSEL_INPT);
  // enable pull-up
  bcm2835_gpio_set_pud(pin, BCM2835_GPIO_PUD_UP);
}

void
define_pin_as_output(uint8_t pin)
{
  bcm2835_gpio_fsel(pin, BCM2835_GPIO_FSEL_OUTP);
}

int keymap[128];

void
setup_keymap()
{
  keymap[0b0101111] = '1';
  keymap[0b0111011] = '4';
  keymap[0b0111101] = '7';
  keymap[0b0111110] = '*';
  keymap[0b1001111] = '2';
  keymap[0b1011011] = '5';
  keymap[0b1011101] = '8';
  keymap[0b1011110] = '0';
  keymap[0b1100111] = '3';
  keymap[0b1110011] = '6';
  keymap[0b1110101] = '9';
  keymap[0b1110110] = '#';
}

uint8_t
scan_keyboard()
{
  uint8_t keycode = 0;
  for (int i = 0; i < sizeof keyboard_pins; i++) {
    keycode <<= 1;
    keycode |= bcm2835_gpio_lev(keyboard_pins[i]);
  }
  return keycode;
}

void
setup_pins()
{
  if (!bcm2835_init()) {
    perror("Cannot initialize BCM2835 GPIO library");
    exit(1);
  }

  for (int i = 0; i < sizeof keyboard_pins; i++) {
    define_pin_as_input(keyboard_pins[i]);
  }
  define_pin_as_input(HOOK_PIN);

  define_pin_as_output(BELL_PIN);
  bcm2835_gpio_write(BELL_PIN, LOW);
}

int sock;

void
create_socket()
{
  static struct sockaddr_in sockaddr;
  int enable_broadcast = 1;

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    perror("socket");
    exit(1);
  }

  sockaddr.sin_family = AF_INET;
  sockaddr.sin_addr.s_addr = INADDR_ANY;
  sockaddr.sin_port = htons(UDP_RECV_PORT);
  if (bind(sock, (const struct sockaddr *)&sockaddr, sizeof sockaddr)) {
    perror("bind");
    exit(1);
  }
}

void
send_packet(char key, int off_hook)
{
  static struct sockaddr_in sockaddr;
  uint8_t state = key | off_hook ? 0x80 : 0;

  printf("Sending keypress '%c' and off_hook %d\n", key, off_hook);

  sockaddr.sin_addr.s_addr = inet_addr("127.0.0.1");
  sockaddr.sin_port = htons(UDP_SEND_PORT);

  if (sendto(sock, &key, 1, 0, (const struct sockaddr*)&sockaddr, sizeof sockaddr) != 1) {
    perror("send");
    exit(1);
  }
}

int
receive_packet()
{
  uint8_t buf;
  int recv_result = recv(sock, &buf, 1, MSG_DONTWAIT);

  switch (recv_result) {
  case 1: {
    int ring_state = buf == '1';
    printf("Setting ring state to %d\n", ring_state);
    return ring_state;
  }
  case 0:
    fprintf(stderr, "Empty packet ignored");
    return -1;
  default:
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return -1;
    }
    perror("recv");
    exit(1);
  }
}

// The handle_timer() function will be called at 40 Hz

void
handle_timer(int signal)
{
  static uint32_t cycle_count;
  static uint8_t previous_keycode = 0;
  static int previous_off_hook = 0;
  static int ringing = 0;

  cycle_count++;

  uint8_t current_keycode = scan_keyboard();
  int current_off_hook = bcm2835_gpio_lev(HOOK_PIN);
  if ((cycle_count & 1) && (previous_keycode != current_keycode || previous_off_hook != current_off_hook)) {
    send_packet(keymap[current_keycode], current_off_hook);
    previous_keycode = current_keycode;
    previous_off_hook = current_off_hook;
  }

  switch (receive_packet()) {
  case 1:
    ringing = 1;
    break;
  case 0:
    ringing = 0;
    break;
  }

  if (ringing) {
    bcm2835_gpio_write(BELL_PIN, cycle_count & 1);
  } else {
    // Make sure the bell is not kept in high state when ringing was stopped
    bcm2835_gpio_write(BELL_PIN, 0);
  }
}

void
setup_timer()
{
  static struct sigaction sa;
  static struct itimerval timer;

  sa.sa_handler = handle_timer;
  if (sigaction(SIGALRM, &sa, NULL)) {
    perror("sigaction");
    exit(1);
  }

  timer.it_value.tv_usec = 1000000 / POLL_HZ;
  timer.it_interval.tv_usec = 1000000 / POLL_HZ;
  setitimer (ITIMER_REAL, &timer, NULL);
}

int
main(int argc, char* argv[])
{
  disable_swapping();
  setup_keymap();
  setup_pins();
  create_socket();
  setup_timer();

  printf("GPIO server is running\n");
  for (;;) {
    pause();
  }
}
