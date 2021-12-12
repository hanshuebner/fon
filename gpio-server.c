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

#define	BELL_PIN	RPI_BPLUS_GPIO_J8_32
uint8_t row_pins[4] = { RPI_BPLUS_GPIO_J8_31, RPI_BPLUS_GPIO_J8_33, RPI_BPLUS_GPIO_J8_35, RPI_BPLUS_GPIO_J8_37 };
uint8_t col_pins[3] = { RPI_BPLUS_GPIO_J8_36, RPI_BPLUS_GPIO_J8_38, RPI_BPLUS_GPIO_J8_40 };

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

int
scan_keyboard()
{
  int pressed = -1;
  for (int row = 0; row < 4; row++) {
    bcm2835_gpio_write(row_pins[row], LOW);
    for (int col = 0; col < 3; col++) {
      if (bcm2835_gpio_lev(col_pins[col]) == LOW) {
        pressed = row * 3 + col;
      }
    }
    bcm2835_gpio_write(row_pins[row], HIGH);
  }
  return pressed;
}

void
setup_pins()
{
  if (!bcm2835_init()) {
    perror("Cannot initialize BCM2835 GPIO library");
    exit(1);
  }

  for (int i = 0; i < 4; i++) {
    define_pin_as_output(row_pins[i]);
    bcm2835_gpio_write(row_pins[i], HIGH);
  }
  for (int i = 0; i < 3; i++) {
    define_pin_as_input(col_pins[i]);
  }

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

char
decode_key(int key)
{
  static const char keymap[12] = { '1', '2', '3', '4', '5', '6', '7', '8', '9', '*', '0', '#' };
  if (key == -1) {
    return ' ';
  } else if (key >= 0 && key < 12) {
    return keymap[key];
  } else {
    return '?';
  }
}

void
send_keypress_packet(int key)
{
  static struct sockaddr_in sockaddr;
  char c = decode_key(key);

  printf("Sending keypress %d => '%c'\n", key, c);

  sockaddr.sin_addr.s_addr = inet_addr("127.0.0.1");
  sockaddr.sin_port = htons(UDP_SEND_PORT);

  if (sendto(sock, &c, 1, 0, (const struct sockaddr*)&sockaddr, sizeof sockaddr) != 1) {
    perror("send");
    exit(1);
  }
}

int
receive_ring_command()
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
  static int previous_key = -1;
  static int ringing = 0;

  cycle_count++;

  int current_key = scan_keyboard();
  if (!(cycle_count % 4) && previous_key != current_key) {
    send_keypress_packet(current_key);
    previous_key = current_key;
  }

  switch (receive_ring_command()) {
  case 1:
    ringing = 1;
    break;
  case 0:
    ringing = 0;
    break;
  }

  if (ringing) {
    bcm2835_gpio_write(BELL_PIN, (cycle_count & 1) ? HIGH : LOW);
  } else {
    // Make sure the bell is not kept in HIGH state when ringing was stopped
    bcm2835_gpio_write(BELL_PIN, LOW);
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
  setup_pins();
  create_socket();
  setup_timer();

  printf("GPIO server is running\n");
  for (;;) {
    pause();
  }
}
