#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <linux/input-event-codes.h>
#include "miniaudio/miniaudio.h"
#include <signal.h>
#include <errno.h>

#define DEVICE "/dev/uinput"
#define MAX_LINE_LEN 512

volatile sig_atomic_t running = 1;

const uint16_t UDP_PORT = 6589;
const uint32_t UNINITIALIZED_ID = 0xFFFFFFFF;
enum Side { PORT_SIDE = 1, STARBOARD_SIDE = 2 };
enum Joy  { THROTTLE = 1, ROTARY = 2 };

struct packet_t {
  uint16_t throttle;
  uint16_t angle;
  uint16_t id;
  bool starboard;
  uint32_t timestamp;
};

struct azipod_state_t {
  enum Side side;
  uint32_t last_packet_id;
  uint16_t throttle_min_val;
  uint16_t throttle_max_val;
  uint16_t throttle_mid_val;
  bool throttle_is_neutral;
  bool throttle_invert;
  uint16_t throttle_value;
  uint16_t throttle_deadband;
  uint16_t rotary_angle;
  int rotary_offset;
} starboard_azipod_state, port_azipod_state;

struct azipod_conf_t {
  int port_throttle_min;
  int port_throttle_max;
  int sb_throttle_min;
  int sb_throttle_max;
  char neutral_snd[MAX_LINE_LEN];
} conf;

int joy_port_throttle = -1;
int joy_sb_throttle = -1;
int joy_port_rotary = -1;
int joy_sb_rotary = -1;

int sock = -1;
ma_engine audio_engine;
int audio_initialized = 0;

void handle_signal(int signal) {
  (void)signal;
  running = 0;
}

int audio_init(void) {
  ma_result result = ma_engine_init(NULL, &audio_engine);

  if (result != MA_SUCCESS) {
    fprintf(stderr, "Failed to initialize audio: %d\n", result);
    return 0;
  }

  return 1;
}

void play_click() {
  if (audio_initialized && conf.neutral_snd[0] != 0) {
    ma_result result =
      ma_engine_play_sound(&audio_engine, conf.neutral_snd, NULL);
    if (result != MA_SUCCESS) {
      fprintf(stderr, "Failed to play sound: %s (%d)\n",
        ma_result_description(result), result);
    }
  }
}

void audio_cleanup(void) {
  if (audio_initialized) {
    ma_engine_uninit(&audio_engine);
  }
}

int make_socket() {
  sock = socket(AF_INET, SOCK_DGRAM, 0);        
  if (sock < 0) {
    perror("Socket creation failed");
    exit(EXIT_FAILURE);
  }
  return sock; 
}

void make_address(uint16_t port, struct sockaddr_in *address) {
  memset(address, 0, sizeof(*address));
  address->sin_family = AF_INET; 
  address->sin_addr.s_addr = htonl(INADDR_ANY);
  address->sin_port = htons(port);
}

int listen_on(uint16_t port) {
  struct sockaddr_in address;
  make_address(port, &address);
  
  sock = make_socket();
  if (bind(sock, (struct sockaddr*)&address, sizeof(address)) < 0) {
    perror("Bind failed");
    close(sock);
    exit(EXIT_FAILURE);
  }

  printf("Server listening on port %d... Waiting for packet.\n", port);
  return sock;
}

void parse(uint32_t val, struct packet_t *packet) {
  packet->throttle = val & 0x3FF;
  packet->angle = (val >> 10) & 0x1FF;
  packet->id = (val >> 19) & 0xFFF;
  packet->starboard = (val & 0x80000000u) != 0;
  packet->timestamp = time(NULL);
}

uint16_t min(uint16_t a, uint16_t b) { return a < b ? a : b; }
uint16_t max(uint16_t a, uint16_t b) { return a > b ? a : b; }

uint16_t throttle_clamp(uint16_t val, const struct azipod_state_t *state) {
  uint16_t new_val = max(state->throttle_min_val, min(val, state->throttle_max_val));

  uint16_t distance = new_val > state->throttle_mid_val
    ? new_val - state->throttle_mid_val
    : state->throttle_mid_val - new_val;
  
  if (distance <= state->throttle_deadband) {
    new_val = state->throttle_mid_val;  
  }
  return new_val;
}

uint16_t angle_clamp(uint16_t val) {
  return min(val, 359);
}

uint16_t offset_angle(uint16_t val, const struct azipod_state_t *state) {
  int angle = (val + state->rotary_offset) % 360;
  return (angle + 360) % 360;
}

int emit(int joy, int value) {
  struct input_event events[2] = {0};

  events[0].type = EV_ABS;
  events[0].code = ABS_X;
  events[0].value = value;

  events[1].type = EV_SYN;
  events[1].code = SYN_REPORT;

  ssize_t n = write(joy, events, sizeof(events));
  if (n < 0) {
    perror("write uinput");
    return 0;
  } else if ((size_t)n != sizeof(events)) {
    fprintf(stderr, "Short write to uinput: %zd bytes\n", n);
    return 0;
  } else {
    return 1;
  }
}

int select_joy(enum Joy joy_type, enum Side side) {
  if (joy_type == THROTTLE) {
    return side == PORT_SIDE ? joy_port_throttle : joy_sb_throttle; 
  } else {
    return side == PORT_SIDE ? joy_port_rotary : joy_sb_rotary; 
  }
}

uint16_t may_invert(uint16_t val, const struct azipod_state_t *state) {
  return state->throttle_invert
    ? state->throttle_min_val + state->throttle_max_val - val
    : val;
}

void update_state(struct azipod_state_t *state, struct packet_t packet) {
  uint16_t throttle = throttle_clamp(packet.throttle, state);
  bool neutral = throttle == state->throttle_mid_val;
  if (neutral && !state->throttle_is_neutral) {
    play_click();
  }
  state->throttle_is_neutral = neutral;

  if (throttle != state->throttle_value) {
    if (emit(select_joy(THROTTLE, state->side), may_invert(throttle, state))) {
      state->throttle_value = throttle;
      printf("%s Throttle %u\n",
             state->side == PORT_SIDE ? "Port" : "Starboard",
             may_invert(throttle, state));
    }
  }

  uint16_t angle = offset_angle(angle_clamp(packet.angle), state);
  if (angle != state->rotary_angle) {
    if (emit(select_joy(ROTARY, state->side), angle)) {
      state->rotary_angle = angle;
      printf("%s Rotary %u\n",
             state->side == PORT_SIDE ? "Port" : "Starboard",
             angle);
    }
  }
}

uint16_t delta(uint16_t packet_id, uint32_t last_packet_id) {
  if (last_packet_id == UNINITIALIZED_ID) {
    return 1;
  }
  return (packet_id - (uint16_t)last_packet_id) & 0xFFF;
}

bool check_delta(uint16_t packet_id, uint32_t last_packet_id) {
  uint16_t d = delta(packet_id, last_packet_id);
  if (d == 1) {
    return true;
  } else if (d > 1 && d < 0x800) {
    fprintf(stderr, "Missing %d packet(s)\n", d-1);
    return true;
  } else {
    return false; // out of order / too old
  }
}

void on_data(struct packet_t packet, struct azipod_state_t *state) {
  if (check_delta(packet.id, state->last_packet_id)) {
    state->last_packet_id = packet.id;
    update_state(state, packet);
  }
}

void dispatch(struct packet_t packet) {
  if (packet.starboard) {
    on_data(packet, &starboard_azipod_state);
  } else {
    on_data(packet, &port_azipod_state);
  }
}

void process(uint32_t value) {
  struct packet_t packet;
  parse(value, &packet);
  dispatch(packet);
}

void start_server(uint16_t port) {
  sock = listen_on(port);
  while (running) {
    uint8_t value[4];
    ssize_t n = recvfrom(sock, &value, sizeof(value), 0, NULL, NULL);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      } else {
        perror("recvfrom");
        break;
      }
    } else if (n != sizeof(value)) {
      fprintf(stderr, "Unexpected packet size: %zd\n", n);
    } else {
      process(
        (uint32_t)value[0]       |
        (uint32_t)value[1] << 8  |
        (uint32_t)value[2] << 16 |
        (uint32_t)value[3] << 24);
    }
  }
}

uint16_t mid_val(uint16_t min_val, uint16_t max_val) {
  return (max_val - min_val) / 2 + min_val;
}

int create_joystick(const char *name, int min_val, int max_val, int start_val) {
  int fd = open(DEVICE, O_WRONLY | O_NONBLOCK);
  if (fd < 0) {
    perror("open uinput device");
    return -1;
  }

  if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
    ioctl(fd, UI_SET_KEYBIT, BTN_JOYSTICK) < 0 ||
    ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
    ioctl(fd, UI_SET_ABSBIT, ABS_X) < 0) {
    perror("Configure joystick capabilities");
    close(fd);
    return -1;
  }

  struct uinput_abs_setup abs_setup = {
    .code = ABS_X,
    .absinfo = {
        .value = start_val,
        .minimum = min_val,
        .maximum = max_val,
        .fuzz = 0,
        .flat = 0,
        .resolution = 0,
    },
  };

  if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0) {
    perror("UI_ABS_SETUP");
    close(fd);
    return -1;
  }

  struct uinput_setup setup = {0};
  snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "%s", name);
  setup.id.bustype = BUS_VIRTUAL;
  setup.id.vendor = 0x1234;
  setup.id.product = 0x5678;
  setup.id.version = 1;

  if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 ||
    ioctl(fd, UI_DEV_CREATE) < 0) {
    perror("Create joystick");
    close(fd);
    return -1;
  }

  return fd;
}

void destroy_joy(int joy) {
  if (joy >= 0) {
    ioctl(joy, UI_DEV_DESTROY);
    close(joy);
  }
}

void destroy_joys() {
 destroy_joy(joy_port_throttle);
 destroy_joy(joy_sb_throttle);
 destroy_joy(joy_port_rotary);
 destroy_joy(joy_sb_rotary);
}

void init_joys(int port_throttle_min,
               int port_throttle_max,
               int sb_throttle_min,
               int sb_throttle_max)
{
  joy_port_throttle = create_joystick(
    "ESP Throttle Left",
    port_throttle_min,
    port_throttle_max,
    mid_val(port_throttle_min, port_throttle_max));

  joy_sb_throttle = create_joystick(
    "ESP Throttle Right",
    sb_throttle_min,
    sb_throttle_max,
    mid_val(sb_throttle_min, sb_throttle_max));

  joy_port_rotary = create_joystick("ESP Azipod Left", 0, 359, 180);
  joy_sb_rotary = create_joystick("ESP Azipod Right", 0, 359, 180);

  if (joy_port_throttle < 0
      || joy_sb_throttle < 0
      || joy_port_rotary < 0
      || joy_sb_rotary < 0) {
   destroy_joys();
   exit(EXIT_FAILURE);
  }
}

void cleanup() {
  printf("Cleaning things up..\n");
  audio_cleanup();
  destroy_joys();
  if (sock >= 0) {
    close(sock);
  }
}

int has_key(const char *line, const char *key) {
  size_t key_len = strlen(key);
  return strlen(line) > key_len
    && strncmp(line, key, key_len) == 0
    && (line[key_len] == ' '
        || line[key_len] == '\t'
        || line[key_len] == '=');
}

char *trim_key(char *p) {
  while (*p != ' ' && *p != '\t' && *p != '=' && *p != 0) p++;
  while ((*p == ' ' || *p == '\t' || *p == '=') && *p != 0) p++;
  return p;
}

int read_config(struct azipod_conf_t *cfg, char *path) {
  FILE *file = fopen(path, "r");

  if (!file) {
    perror(path);
    return 0;
  }

  cfg->neutral_snd[0] = 0;

  char line[MAX_LINE_LEN];
  while (fgets(line, sizeof(line), file)) {
    line[strcspn(line, "\r\n")] = '\0';

    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == '#') {
      continue;
    }

    if (has_key(p, "port_throttle_min")) {
      cfg->port_throttle_min = atoi(trim_key(p));
    } else if (has_key(p, "port_throttle_max")) {
      cfg->port_throttle_max = atoi(trim_key(p));
    } else if (has_key(p, "sb_throttle_min")) {
      cfg->sb_throttle_min = atoi(trim_key(p));
    } else if (has_key(p, "sb_throttle_max")) {
      cfg->sb_throttle_max = atoi(trim_key(p));
    } else if (has_key(p, "neutral_snd")) {
      snprintf(cfg->neutral_snd, sizeof(cfg->neutral_snd), "%s", trim_key(p));
    } else {
      fprintf(stderr, "Invalid configuration: %s\n", p);
    }
  }

  fclose(file);
  return 1;
}

void print_conf(const struct azipod_conf_t cfg) {
  printf("Configuration:\n");
  printf(" * port_throttle_min = %d\n", cfg.port_throttle_min);
  printf(" * port_throttle_max = %d\n", cfg.port_throttle_max);
  printf(" * sb_throttle_min   = %d\n", cfg.sb_throttle_min);
  printf(" * sb_throttle_max   = %d\n", cfg.sb_throttle_max);
  printf(" * neutral_snd       = %s\n", cfg.neutral_snd);
}

void register_signal() {
  struct sigaction sa = {0};
  sa.sa_handler = handle_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // Do not restart interrupted system calls
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
}

int main() {    
  register_signal();

  if (!read_config(&conf, "azipod.cfg")) {
    fprintf(stderr, "azipod.cfg not found");
    exit(EXIT_FAILURE);
  }

  print_conf(conf);

  init_joys(conf.port_throttle_min,
            conf.port_throttle_max,
            conf.sb_throttle_min,
            conf.sb_throttle_max);

  audio_initialized = audio_init();
  
  port_azipod_state.side = PORT_SIDE;
  port_azipod_state.last_packet_id = UNINITIALIZED_ID;
  port_azipod_state.throttle_min_val = conf.port_throttle_min;
  port_azipod_state.throttle_max_val = conf.port_throttle_max;
  port_azipod_state.throttle_mid_val = mid_val(conf.port_throttle_min, conf.port_throttle_max);
  port_azipod_state.throttle_value = port_azipod_state.throttle_mid_val;
  port_azipod_state.throttle_is_neutral = true;
  port_azipod_state.throttle_invert = true;
  port_azipod_state.throttle_deadband = 25;
  port_azipod_state.rotary_offset = 180 - 340;
  port_azipod_state.rotary_angle = 180;

  starboard_azipod_state.side = STARBOARD_SIDE;
  starboard_azipod_state.last_packet_id = UNINITIALIZED_ID;
  starboard_azipod_state.throttle_min_val = conf.sb_throttle_min;
  starboard_azipod_state.throttle_max_val = conf.sb_throttle_max;
  starboard_azipod_state.throttle_mid_val = mid_val(conf.sb_throttle_min, conf.sb_throttle_max);
  starboard_azipod_state.throttle_value = starboard_azipod_state.throttle_mid_val;
  starboard_azipod_state.throttle_is_neutral = true;
  starboard_azipod_state.throttle_invert = false;
  starboard_azipod_state.throttle_deadband = 25;
  starboard_azipod_state.rotary_offset = 180 - 160;
  starboard_azipod_state.rotary_angle = 180;
  
  start_server(UDP_PORT);

  cleanup();
  return 0;
}
