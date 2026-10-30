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
#include <signal.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include "miniaudio/miniaudio.h"

#define DEFAULT_UDP_PORT 6589
#define DEVICE "/dev/uinput"
#define MAX_LINE_LEN 512
#define HALF_WAY 50

const uint32_t UNINITIALIZED_ID = 0xFFFFFFFF;

enum Side { PORT_SIDE, STARBOARD_SIDE, NONE };
enum Joy  { JOY_THROTTLE, JOY_ROTARY };
enum ThrottleState { STATE_NEUTRAL, STATE_MIN, STATE_MAX, STATE_NONE };

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
  enum ThrottleState throttle_state;
  bool throttle_invert;
  uint16_t throttle_value;
  uint16_t throttle_deadband;
  uint16_t rotary_angle;
  int rotary_offset;
} starboard_azipod_state, port_azipod_state;

struct azipod_conf_t {
  int udp_port;
  int port_throttle_min;
  int port_throttle_max;
  int port_throttle_mid;
  int port_throttle_deadband;
  bool port_throttle_invert;
  int port_rotary_neutral;
  int sb_throttle_min;
  int sb_throttle_max;
  int sb_throttle_mid;
  int sb_throttle_deadband;
  int sb_throttle_invert;
  int sb_rotary_neutral;
  char neutral_snd[MAX_LINE_LEN];
  char boundary_snd[MAX_LINE_LEN];
  enum Side replicate;
} conf;

int joy_port_throttle = -1;
int joy_sb_throttle = -1;
int joy_port_rotary = -1;
int joy_sb_rotary = -1;

int sock = -1;
ma_engine audio_engine;
int audio_initialized = 0;

volatile sig_atomic_t running = 1;

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

void play_sound(const char* path) {
  if (audio_initialized && path[0] != 0) {
    ma_result result =
      ma_engine_play_sound(&audio_engine, path, NULL);
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

  printf("Listening on port %d...\n", port);
  return sock;
}

void parse_packet(uint32_t val, struct packet_t *packet) {
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
  if (joy_type == JOY_THROTTLE) {
    return side == PORT_SIDE ? joy_port_throttle : joy_sb_throttle;
  } else {
    return side == PORT_SIDE ? joy_port_rotary : joy_sb_rotary;
  }
}

uint8_t scale_to_pct(uint16_t value, uint16_t min_val,
                     uint16_t mid_val, uint16_t max_val) {
  if (value <= mid_val) {
    return (uint32_t)(value - min_val) * HALF_WAY
      / (mid_val - min_val);
  } else {
    return HALF_WAY + (uint32_t)(value - mid_val) * HALF_WAY
      / (max_val - mid_val);
  }
}

uint8_t throttle_pct(uint16_t val, const struct azipod_state_t *state) {
  uint8_t pct = scale_to_pct(
    val,
    state->throttle_min_val,
    state->throttle_mid_val,
    state->throttle_max_val);
  return state->throttle_invert ? 100 - pct : pct;
}

void replicate_throttle(uint8_t pct, struct azipod_state_t *state) {
  if (state->side == PORT_SIDE
      && conf.replicate == PORT_SIDE) {
    printf("Replicating Port to Starboard. Throttle: %u%%\n", pct);
    emit(joy_sb_throttle, pct);
  } else if (state->side == STARBOARD_SIDE
          && conf.replicate == STARBOARD_SIDE) {
    printf("Replicating Starboard to Port. Throttle: %u%%\n", pct);
    emit(joy_port_throttle, pct);
  }
}

void replicate_rotary(uint16_t angle, struct azipod_state_t *state) {
  if (state->side == PORT_SIDE
      && conf.replicate == PORT_SIDE) {
    printf("Replicating Port to Starboard. Rotary: %u%%\n", angle);
    emit(joy_sb_rotary, angle);
  } else if (state->side == STARBOARD_SIDE
          && conf.replicate == STARBOARD_SIDE) {
    printf("Replicating Starboard to Port. Rotary: %u%%\n", angle);
    emit(joy_port_rotary, angle);
  }
}

void play_controller_sounds(uint8_t throttle_pct, struct azipod_state_t *state) {
  if (throttle_pct == 0 && state->throttle_state != STATE_MIN) {
    play_sound(conf.boundary_snd);
    state->throttle_state = STATE_MIN;
  } else if (throttle_pct == 100 && state->throttle_state != STATE_MAX) {
    play_sound(conf.boundary_snd);
    state->throttle_state = STATE_MAX;
  } else if (throttle_pct == HALF_WAY && state->throttle_state != STATE_NEUTRAL) {
    play_sound(conf.neutral_snd);
    state->throttle_state = STATE_NEUTRAL;
  } else {
    state->throttle_state = STATE_NONE;
  }
}

void update_throttle(struct azipod_state_t *state, struct packet_t packet) {
  uint16_t throttle = throttle_clamp(packet.throttle, state);

  if (throttle != state->throttle_value) {
    uint8_t pct = throttle_pct(throttle, state);
    if (emit(select_joy(JOY_THROTTLE, state->side), pct)) {
      state->throttle_value = throttle;

      printf("%s Throttle: %u (%u%%)\n",
             state->side == PORT_SIDE ? "Port" : "Starboard",
             throttle, pct);

      replicate_throttle(pct, state);
      play_controller_sounds(pct, state);
    }
  }
}

void update_rotary(struct azipod_state_t *state, struct packet_t packet) {
  uint16_t angle = offset_angle(angle_clamp(packet.angle), state);

  if (angle != state->rotary_angle) {
    if (emit(select_joy(JOY_ROTARY, state->side), angle)) {
      state->rotary_angle = angle;

      printf("%s Rotary %u\n",
             state->side == PORT_SIDE ? "Port" : "Starboard",
             angle);
      
      replicate_rotary(angle, state);
    }
  }
}

uint16_t delta(uint16_t packet_id, uint32_t last_packet_id) {
  return last_packet_id == UNINITIALIZED_ID
    ? 1
    : (packet_id - (uint16_t)last_packet_id) & 0xFFF;
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
    update_throttle(state, packet);
    update_rotary(state, packet);
  }
}

void process(uint32_t value) {
  struct packet_t packet;
  parse_packet(value, &packet);

  if (packet.starboard) {
    on_data(packet, &starboard_azipod_state);
  } else {
    on_data(packet, &port_azipod_state);
  }
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

void init_joys() {
  joy_port_throttle = create_joystick("ESP Throttle Left", 0, 100, HALF_WAY);
  joy_sb_throttle = create_joystick("ESP Throttle Right", 0, 100, HALF_WAY);

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

int match_key(const char *line, const char *key) {
  size_t key_len = strlen(key);
  return strlen(line) > key_len
    && strncmp(line, key, key_len) == 0
    && (line[key_len] == ' '
        || line[key_len] == '\t'
        || line[key_len] == '=');
}

char *conf_val(char *p) {
  while (*p != ' '
         && *p != '\t'
         && *p != '='
         && *p != 0) p++;
  while ((*p == ' ' || *p == '\t' || *p == '=')
         && *p != 0) p++;
  return p;
}

void parse_conf_replicate(char *val, struct azipod_conf_t *cfg) {
  if (strcmp(val, "STARBOARD") == 0) {
    cfg->replicate = STARBOARD_SIDE;  
  } else if (strcmp(val, "PORT") == 0) {
    cfg->replicate = PORT_SIDE;  
  } else if (strcmp(val, "NONE") == 0) {
    cfg->replicate = NONE;
  } else {
    fprintf(stderr, "Invalid config value replicate = %s\n", val);
  }
}

int to_num(char *s, int default_val) {
  char *end;
  long value = strtol(s, &end, 10);
  if (end == s || *end != '\0') {
    fprintf(stderr, "Invalid number in config: %s\n", s);
    return default_val;
  } else {
    return value;
  }
}

bool to_bool(char *s) {
  if (strcmp(s, "true") == 0) {
    return true;
  }
  if (strcmp(s, "false") != 0) {
    fprintf(stderr, "Invalid bool in config: %s\n", s);
  }
  return false;
}

void set_config_defaults(struct azipod_conf_t *cfg) {
  cfg->udp_port = DEFAULT_UDP_PORT;
  cfg->neutral_snd[0] = 0;
  cfg->boundary_snd[0] = 0;
  cfg->replicate = NONE;

  cfg->sb_throttle_min = 0;
  cfg->sb_throttle_max = 1023;
  cfg->sb_throttle_mid = -1;
  cfg->sb_throttle_deadband = 0;
  cfg->sb_throttle_invert = false;
  cfg->sb_rotary_neutral = 0;

  cfg->port_throttle_min = 0;
  cfg->port_throttle_max = 1023;
  cfg->port_throttle_mid = -1;
  cfg->port_throttle_deadband = 0;
  cfg->port_throttle_invert = false;
  cfg->port_rotary_neutral = 0;
}

void parse_line(char *p, struct azipod_conf_t *cfg) {
  if (match_key(p, "port_throttle_min")) {
    cfg->port_throttle_min = to_num(conf_val(p), 0);
  } else if (match_key(p, "port_throttle_max")) {
    cfg->port_throttle_max = to_num(conf_val(p), 1023);
  } else if (match_key(p, "port_throttle_mid")) {
    cfg->port_throttle_mid = to_num(conf_val(p), -1);
  } else if (match_key(p, "port_throttle_deadband")) {
    cfg->port_throttle_deadband = to_num(conf_val(p), 0);
  } else if (match_key(p, "port_throttle_invert")) {
    cfg->port_throttle_invert = to_bool(conf_val(p));
  } else if (match_key(p, "port_rotary_neutral")) {
    cfg->port_rotary_neutral = to_num(conf_val(p), 0);
  } else if (match_key(p, "sb_throttle_min")) {
    cfg->sb_throttle_min = to_num(conf_val(p), 0);
  } else if (match_key(p, "sb_throttle_max")) {
    cfg->sb_throttle_max = to_num(conf_val(p), 1023);
  } else if (match_key(p, "sb_throttle_mid")) {
    cfg->sb_throttle_mid = to_num(conf_val(p), -1);
  } else if (match_key(p, "sb_throttle_deadband")) {
    cfg->sb_throttle_deadband = to_num(conf_val(p), 0);
  } else if (match_key(p, "sb_throttle_invert")) {
    cfg->sb_throttle_invert = to_bool(conf_val(p));
  } else if (match_key(p, "sb_rotary_neutral")) {
    cfg->sb_rotary_neutral = to_num(conf_val(p), 0);
  } else if (match_key(p, "neutral_snd")) {
    snprintf(cfg->neutral_snd, sizeof(cfg->neutral_snd), "%s", conf_val(p));
  } else if (match_key(p, "boundary_snd")) {
    snprintf(cfg->boundary_snd, sizeof(cfg->boundary_snd), "%s", conf_val(p));
  } else if (match_key(p, "replicate")) {
    parse_conf_replicate(conf_val(p), cfg);
  } else if (match_key(p, "udp_port")) {
    cfg->udp_port = to_num(conf_val(p), DEFAULT_UDP_PORT);
  } else {
    fprintf(stderr, "Invalid configuration: %s\n", p);
  }
}

void calculate_mid_vals(struct azipod_conf_t *cfg) {
  if (cfg->sb_throttle_mid == -1) {
    cfg->sb_throttle_mid =
      mid_val(cfg->sb_throttle_min, cfg->sb_throttle_max);
  }
  if (cfg->port_throttle_mid == -1) {
    cfg->port_throttle_mid =
      mid_val(cfg->port_throttle_min, cfg->port_throttle_max);
  }
}

int read_config(struct azipod_conf_t *cfg, char *path) {
  FILE *file = fopen(path, "r");

  if (!file) {
    perror(path);
    return 0;
  }

  set_config_defaults(cfg);

  char line[MAX_LINE_LEN];
  while (fgets(line, sizeof(line), file)) {
    line[strcspn(line, "\r\n")] = '\0';
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '\0' && *p != '#') {
      parse_line(p, cfg);
    }
  }

  calculate_mid_vals(cfg);

  fclose(file);
  return 1;
}

void print_conf(const struct azipod_conf_t cfg) {
  printf("Configuration:\n");
  printf(" * udp_port               = %d\n", cfg.udp_port);
  printf(" * port_throttle_min      = %d\n", cfg.port_throttle_min);
  printf(" * port_throttle_max      = %d\n", cfg.port_throttle_max);
  printf(" * port_throttle_mid      = %d\n", cfg.port_throttle_mid);
  printf(" * port_throttle_deadband = %d\n", cfg.port_throttle_deadband);
  printf(" * port_throttle_invert   = %s\n", cfg.port_throttle_invert ? "true" : "false");
  printf(" * port_rotary_neutral    = %d\n", cfg.port_rotary_neutral);
  printf(" * sb_throttle_min        = %d\n", cfg.sb_throttle_min);
  printf(" * sb_throttle_max        = %d\n", cfg.sb_throttle_max);
  printf(" * sb_throttle_mid        = %d\n", cfg.sb_throttle_mid);
  printf(" * sb_throttle_deadband   = %d\n", cfg.sb_throttle_deadband);
  printf(" * sb_rotary_neutral      = %d\n", cfg.sb_rotary_neutral);
  printf(" * sb_throttle_invert     = %s\n", cfg.sb_throttle_invert ? "true" : "false");
  printf(" * neutral_snd            = %s\n", cfg.neutral_snd);
  printf(" * boundary_snd           = %s\n", cfg.boundary_snd);
  printf(" * replicate              = %s\n", cfg.replicate == STARBOARD_SIDE
         ? "STARBOARD"
         : (cfg.replicate == PORT_SIDE ? "PORT" : "NONE"));
}

void register_signal() {
  struct sigaction sa = {0};
  sa.sa_handler = handle_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // Do not restart interrupted system calls
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
}

int exec_path(char *result, size_t size) {
  if (size == 0) return 0;

  ssize_t len = readlink("/proc/self/exe", result, size - 1);
  if (len < 0) {
    perror("readlink");
    return 0;
  }
  if (len == (ssize_t)(size - 1)) {
    fprintf(stderr, "Executable path too long\n");
    return 0;
  }

  result[len] = '\0';
  dirname(result);
  return 1;
}

int locate_config(char *config_path, size_t size) {
  char path[PATH_MAX];
  exec_path(path, sizeof(path));

  int n = snprintf(config_path, size, "%s/azipod.cfg", path);
  if (n < 0 || (size_t)n >= size) {
    fprintf(stderr, "Config path is too long\n");
    return 0;
  }

  return 1;
}

void initialize() {
  register_signal();

  char config_path[PATH_MAX];
  if (!locate_config(config_path, sizeof(config_path))) {
    fprintf(stderr, "Cannot locate executable path.");
    exit(EXIT_FAILURE);
  }

  if (!read_config(&conf, config_path)) {
    fprintf(stderr, "Config file azipod.cfg not found.");
    exit(EXIT_FAILURE);
  }
  print_conf(conf);

  init_joys();
  audio_initialized = audio_init();
}

void create_port_azipod() {
  port_azipod_state.side = PORT_SIDE;
  port_azipod_state.last_packet_id = UNINITIALIZED_ID;
  port_azipod_state.throttle_min_val = conf.port_throttle_min;
  port_azipod_state.throttle_max_val = conf.port_throttle_max;
  port_azipod_state.throttle_mid_val = conf.port_throttle_mid;
  port_azipod_state.throttle_value = port_azipod_state.throttle_mid_val;
  port_azipod_state.throttle_state = STATE_NEUTRAL;
  port_azipod_state.throttle_invert = conf.port_throttle_invert;
  port_azipod_state.throttle_deadband = conf.port_throttle_deadband;
  port_azipod_state.rotary_offset = 180 - conf.port_rotary_neutral;
  port_azipod_state.rotary_angle = 180;
}

void create_starboard_azipod() {
  starboard_azipod_state.side = STARBOARD_SIDE;
  starboard_azipod_state.last_packet_id = UNINITIALIZED_ID;
  starboard_azipod_state.throttle_min_val = conf.sb_throttle_min;
  starboard_azipod_state.throttle_max_val = conf.sb_throttle_max;
  starboard_azipod_state.throttle_mid_val = conf.sb_throttle_mid;
  starboard_azipod_state.throttle_value = starboard_azipod_state.throttle_mid_val;
  starboard_azipod_state.throttle_state = STATE_NEUTRAL;
  starboard_azipod_state.throttle_invert = conf.sb_throttle_invert;
  starboard_azipod_state.throttle_deadband = conf.sb_throttle_deadband;
  starboard_azipod_state.rotary_offset = 180 - conf.sb_rotary_neutral;
  starboard_azipod_state.rotary_angle = 180;
}

int main() {
  initialize();
  create_port_azipod();
  create_starboard_azipod();
  start_server(conf.udp_port);
  cleanup();
  return 0;
}
