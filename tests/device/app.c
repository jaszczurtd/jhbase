/*
 * jhbase on a device: time, peak stack and scratch of the parser, number
 * reads, writer, C literals and UTF-8 validation. The firmware measures once
 * after start, checks results against independent values and then repeats
 * the report as "JBM" lines; scripts/measure_device.sh reads them and adds
 * the code size from the map file.
 *
 * LED: toggles after each measured case, then blinks once per second (PASS)
 * or five times per second (FAIL). An addressable LED is blue while
 * measuring, then green or red.
 */
#include <hal/core/hal_app.h>
#include <hal/core/hal_array.h>
#include <hal/gpio/hal_gpio.h>
#include <hal/gpio/hal_rgb_led.h>
#include <hal/serial/hal_serial.h>
#include <hal/system/hal_sync.h>
#include <hal/system/hal_system.h>

#include "jb/json_number.h"
#include "jb/json_writer.h"
#include "jb/literal.h"
#include "jb/utf8.h"
#include "jb_device_inputs.h"

#include <inttypes.h>
#include <string.h>

#define STACK_PAINT_BYTES 2048u
#define STACK_GUARD_BYTES 16u
#define BATCHES 5u
#define BATCH_US 40000u
#define REPORT_PERIOD_MS 3000u
#define LINE_GAP_MS 20u
#define CONFIG_TOKENS 192u
#define NUMBER_TOKENS 48u
#define MAX_WORK 1000000u

typedef hal_status_t (*workload_fn)(void);
typedef hal_status_t (*item_fn)(jb_json_node_t item);

typedef struct {
  const char *name;
  workload_fn run;
  size_t (*scratch_required)(void); /* NULL when the case uses no scratch */
} workload_t;

typedef struct {
  hal_status_t status;
  uint32_t ns, iterations, stack;
  size_t scratch_used, scratch_required;
} result_t;

static const jb_json_limits_t k_limits = {4096u, CONFIG_TOKENS, 8u, 256u, 64u, 64u, 65536u};
static const jb_json_float_options_t k_float = {true, false, MAX_WORK};
static jb_json_token_t s_config_tokens[CONFIG_TOKENS], s_number_tokens[NUMBER_TOKENS],
    s_check_tokens[NUMBER_TOKENS];
static jb_json_doc_t s_config, s_numbers;
static jb_json_node_t s_integers, s_floats, s_wide;
static unsigned char s_scratch[4096];
static char s_output[4096];
static const jb_json_writer_limits_t k_writer = {sizeof(s_output), 8u, 64u, 512u, 65536u};
static float s_f32[16];
static double s_f64[24];
static size_t s_f32_count, s_f64_count, s_accumulator, s_written;
static uintptr_t s_paint_top;

static jb_json_workspace_t scratch(void) {
  return (jb_json_workspace_t){s_scratch, sizeof(s_scratch)};
}

static jb_json_span_t text(const char *data, size_t length) {
  return (jb_json_span_t){data, length};
}

/* Calls fn for every item of an array; stops at the first failure. */
static hal_status_t each_item(jb_json_node_t array, item_fn fn) {
  jb_json_node_t item;
  hal_status_t status = jb_json_first(array, &item);
  while (status == HAL_OK) {
    const hal_status_t result = fn(item);
    if (result != HAL_OK) {
      return result;
    }
    status = jb_json_next(item, &item);
  }
  return status == HAL_ENOENT ? HAL_OK : status;
}

static hal_status_t keep_max(hal_status_t status, size_t value) {
  if (status == HAL_OK && value > s_accumulator) {
    s_accumulator = value;
  }
  return status;
}

static hal_status_t f32_size(jb_json_node_t item) {
  size_t bytes = 0u;
  return keep_max(jb_json_float_workspace_size(item, 32u, &bytes), bytes);
}

static hal_status_t f64_size(jb_json_node_t item) {
  size_t bytes = 0u;
  return keep_max(jb_json_float_workspace_size(item, 64u, &bytes), bytes);
}

static hal_status_t literal_f32_size(jb_json_node_t item) {
  size_t bytes = 0u;
  return keep_max(jb_c_literal_workspace_size(item, JB_C_F32, &bytes), bytes);
}

static hal_status_t literal_f64_size(jb_json_node_t item) {
  size_t bytes = 0u;
  return keep_max(jb_c_literal_workspace_size(item, JB_C_F64, &bytes), bytes);
}

/* Largest per-item scratch over one or two arrays (second may be NULL). */
static size_t items_max(jb_json_node_t first, const jb_json_node_t *second, item_fn size) {
  s_accumulator = 0u;
  if (each_item(first, size) != HAL_OK || (second != NULL && each_item(*second, size) != HAL_OK)) {
    return SIZE_MAX;
  }
  return s_accumulator;
}

static size_t parse_scratch(void) {
  size_t bytes = 0u;
  return jb_json_parse_workspace_size(&k_limits, &bytes) == HAL_OK ? bytes : SIZE_MAX;
}

static size_t writer_scratch(void) {
  size_t bytes = 0u;
  return jb_json_writer_workspace_size(&k_writer, &bytes) == HAL_OK ? bytes : SIZE_MAX;
}

static size_t f32_scratch(void) { return items_max(s_floats, NULL, f32_size); }

static size_t f64_scratch(void) { return items_max(s_floats, &s_wide, f64_size); }

static size_t literal_scratch(void) {
  const size_t narrow = items_max(s_floats, NULL, literal_f32_size);
  const size_t wide = items_max(s_floats, &s_wide, literal_f64_size);
  return narrow > wide ? narrow : wide;
}

static hal_status_t w_empty(void) { return HAL_OK; }

static hal_status_t w_parse(void) {
  return jb_json_parse(text(jb_input_config, JB_INPUT_CONFIG_LENGTH), &k_limits, s_config_tokens,
                       COUNTOF(s_config_tokens), scratch(), &s_config, NULL);
}

static hal_status_t w_measure(void) {
  jb_json_requirements_t needs;
  return jb_json_measure(text(jb_input_config, JB_INPUT_CONFIG_LENGTH), &k_limits, scratch(),
                         &needs, NULL);
}

/* Decodes every string and key, then follows one path by key and position. */
static hal_status_t w_read(void) {
  size_t required = 0u;
  hal_status_t status = HAL_OK;
  for (size_t i = 0u; status == HAL_OK && i < s_config.token_count; ++i) {
    const jb_json_node_t at = {&s_config, i};
    if (s_config_tokens[i].has_key) {
      status = jb_json_key_copy(at, s_output, sizeof(s_output), &required);
    }
    if (status == HAL_OK && s_config_tokens[i].type == JB_JSON_STRING) {
      status = jb_json_string_copy(at, s_output, sizeof(s_output), &required);
    }
  }
  jb_json_node_t node;
  status = status == HAL_OK ? jb_json_root(&s_config, &node) : status;
  status = status == HAL_OK ? jb_json_find(node, text("sensors", 7u), &node) : status;
  status = status == HAL_OK ? jb_json_at(node, 3u, &node) : status;
  status = status == HAL_OK ? jb_json_find(node, text("limits", 6u), &node) : status;
  return status == HAL_OK ? jb_json_find(node, text("warn", 4u), &node) : status;
}

static hal_status_t read_integer(jb_json_node_t item) {
  jb_json_span_t token;
  hal_status_t status = jb_json_number_token(item, &token);
  if (status == HAL_OK && token.data[0] == '-') {
    int64_t value;
    status = jb_json_get_i64(item, &value);
  } else if (status == HAL_OK) {
    uint64_t value;
    status = jb_json_get_u64(item, &value);
  }
  return status;
}

static hal_status_t read_f32(jb_json_node_t item) {
  uint32_t bits;
  return jb_json_get_f32_bits(item, &k_float, scratch(), &bits);
}

static hal_status_t read_f64(jb_json_node_t item) {
  uint64_t bits;
  return jb_json_get_f64_bits(item, &k_float, scratch(), &bits);
}

static hal_status_t literal_f32(jb_json_node_t item) {
  size_t required = 0u;
  return jb_c_literal(item, JB_C_F32, scratch(), MAX_WORK, s_output, sizeof(s_output), &required);
}

static hal_status_t literal_f64(jb_json_node_t item) {
  size_t required = 0u;
  return jb_c_literal(item, JB_C_F64, scratch(), MAX_WORK, s_output, sizeof(s_output), &required);
}

static hal_status_t w_integers(void) { return each_item(s_integers, read_integer); }

/* Calls fn for every number of the configuration: the everyday spellings. */
static hal_status_t each_config_number(item_fn fn) {
  hal_status_t status = HAL_OK;
  for (size_t i = 0u; status == HAL_OK && i < s_config.token_count; ++i) {
    if (s_config_tokens[i].type == JB_JSON_NUMBER) {
      status = fn((jb_json_node_t){&s_config, i});
    }
  }
  return status;
}

static hal_status_t w_config_f32(void) { return each_config_number(read_f32); }

static size_t config_f32_scratch(void) {
  s_accumulator = 0u;
  return each_config_number(f32_size) == HAL_OK ? s_accumulator : SIZE_MAX;
}

static hal_status_t w_f32(void) { return each_item(s_floats, read_f32); }

static hal_status_t w_f64(void) {
  const hal_status_t status = each_item(s_floats, read_f64);
  return status == HAL_OK ? each_item(s_wide, read_f64) : status;
}

static hal_status_t w_literals(void) {
  hal_status_t status = each_item(s_floats, literal_f32);
  status = status == HAL_OK ? each_item(s_floats, literal_f64) : status;
  return status == HAL_OK ? each_item(s_wide, literal_f64) : status;
}

static hal_status_t writer_start(jb_json_writer_t *writer, jb_json_buffer_t *buffer) {
  *buffer = (jb_json_buffer_t){s_output, sizeof(s_output), 0u};
  return jb_json_writer_init(writer, &k_writer, scratch(), jb_json_buffer_sink, buffer);
}

static hal_status_t w_write_config(void) {
  jb_json_writer_t writer;
  jb_json_buffer_t buffer;
  jb_json_node_t root;
  hal_status_t status = writer_start(&writer, &buffer);
  status = status == HAL_OK ? jb_json_root(&s_config, &root) : status;
  status = status == HAL_OK ? jb_json_writer_node(&writer, root) : status;
  return status == HAL_OK ? jb_json_writer_finish(&writer, &s_written) : status;
}

static hal_status_t w_write_floats(void) {
  jb_json_writer_t writer;
  jb_json_buffer_t buffer;
  hal_status_t status = writer_start(&writer, &buffer);
  status = status == HAL_OK ? jb_json_writer_array_begin(&writer) : status;
  for (size_t i = 0u; status == HAL_OK && i < s_f32_count; ++i) {
    status = jb_json_writer_f32(&writer, s_f32[i]);
  }
  for (size_t i = 0u; status == HAL_OK && i < s_f64_count; ++i) {
    status = jb_json_writer_f64(&writer, s_f64[i]);
  }
  status = status == HAL_OK ? jb_json_writer_array_end(&writer) : status;
  return status == HAL_OK ? jb_json_writer_finish(&writer, &s_written) : status;
}

static hal_status_t w_utf8(void) {
  size_t offset = 0u;
  return jb_utf8_validate(jb_input_config, JB_INPUT_CONFIG_LENGTH, &offset);
}

static const workload_t k_cases[] = {
    {"parse", w_parse, parse_scratch},
    {"measure", w_measure, parse_scratch},
    {"read", w_read, NULL},
    {"integers", w_integers, NULL},
    {"config_f32", w_config_f32, config_f32_scratch},
    {"f32", w_f32, f32_scratch},
    {"f64", w_f64, f64_scratch},
    {"literals", w_literals, literal_scratch},
    {"write_config", w_write_config, writer_scratch},
    {"write_floats", w_write_floats, writer_scratch},
    {"utf8", w_utf8, NULL},
};
static result_t s_results[COUNTOF(k_cases)];
static uint32_t s_baseline_stack;

/* Paints the free stack below this frame. The next call made by the same
 * caller starts at the same stack pointer, so the lowest overwritten byte
 * gives the depth of that call. */
static __attribute__((noinline)) void stack_paint(void) {
  volatile uint8_t anchor = 0u;
  s_paint_top = (uintptr_t)&anchor;
  for (uintptr_t at = s_paint_top - STACK_PAINT_BYTES; at < s_paint_top - STACK_GUARD_BYTES; ++at) {
    *(volatile uint8_t *)at = 0xA5u;
  }
}

static __attribute__((noinline)) hal_status_t stack_call(workload_fn run) { return run(); }

static uint32_t stack_depth(void) {
  uintptr_t at = s_paint_top - STACK_PAINT_BYTES;
  while (at < s_paint_top - STACK_GUARD_BYTES && *(const volatile uint8_t *)at == 0xA5u) {
    ++at;
  }
  return (uint32_t)(s_paint_top - at);
}

/* Interrupts stay masked so that handlers on the same stack do not count. */
static uint32_t measure_stack(workload_fn run, hal_status_t *status) {
  hal_critical_section_enter();
  stack_paint();
  *status = stack_call(run);
  const uint32_t depth = stack_depth();
  hal_critical_section_exit();
  return depth;
}

/* Highest scratch byte written, over two fill patterns. */
static size_t measure_scratch(workload_fn run) {
  static const uint8_t k_patterns[] = {0xA5u, 0x5Au};
  size_t used = 0u;
  for (size_t p = 0u; p < COUNTOF(k_patterns); ++p) {
    memset(s_scratch, k_patterns[p], sizeof(s_scratch));
    (void)run();
    size_t top = sizeof(s_scratch);
    while (top > 0u && s_scratch[top - 1u] == k_patterns[p]) {
      --top;
    }
    used = top > used ? top : used;
  }
  return used;
}

/* Fastest of several batches, each at least BATCH_US long. */
static uint32_t measure_time(workload_fn run, uint32_t *iterations) {
  uint64_t best = UINT64_MAX;
  *iterations = 0u;
  for (uint32_t batch = 0u; batch < BATCHES; ++batch) {
    uint32_t count = 0u;
    const uint32_t started = hal_micros();
    do {
      (void)run();
      ++count;
    } while (!hal_elapsed_u32(hal_micros(), started, BATCH_US));
    const uint64_t ns = (uint64_t)(hal_micros() - started) * 1000u / count;
    best = ns < best ? ns : best;
    *iterations += count;
  }
  return (uint32_t)best;
}

static bool s_led_on, s_measured, s_pass;

static void led_init(void) {
#if defined(HAL_LED_BUILTIN)
  hal_gpio_set_mode(HAL_LED_BUILTIN, HAL_GPIO_OUTPUT);
  hal_gpio_write(HAL_LED_BUILTIN, false);
#else
  (void)hal_rgb_led_init_board_status();
  hal_rgb_led_set_brightness(16u);
#endif
}

static void led_toggle(void) {
  s_led_on = !s_led_on;
#if defined(HAL_LED_BUILTIN)
  hal_gpio_write(HAL_LED_BUILTIN, s_led_on);
#else
  const hal_rgb_led_color_t lit =
      !s_measured ? HAL_RGB_LED_BLUE : (s_pass ? HAL_RGB_LED_GREEN : HAL_RGB_LED_RED);
  (void)hal_rgb_led_set_color(s_led_on ? lit : HAL_RGB_LED_NONE);
#endif
}

static bool check_f32(jb_json_node_t array, size_t position, uint32_t expected) {
  jb_json_node_t node;
  uint32_t bits = 0u;
  return jb_json_at(array, position, &node) == HAL_OK &&
         jb_json_get_f32_bits(node, &k_float, scratch(), &bits) == HAL_OK && bits == expected;
}

static bool check_f64(jb_json_node_t array, size_t position, uint64_t expected) {
  jb_json_node_t node;
  uint64_t bits = 0u;
  return jb_json_at(array, position, &node) == HAL_OK &&
         jb_json_get_f64_bits(node, &k_float, scratch(), &bits) == HAL_OK && bits == expected;
}

/* Written floats read back to the same bits: float items first, then double. */
static bool check_round_trip(void) {
  jb_json_doc_t doc;
  jb_json_node_t root, node;
  if (w_write_floats() != HAL_OK ||
      jb_json_parse(text(s_output, s_written), &k_limits, s_check_tokens, COUNTOF(s_check_tokens),
                    scratch(), &doc, NULL) != HAL_OK ||
      jb_json_root(&doc, &root) != HAL_OK) {
    return false;
  }
  for (size_t i = 0u; i < s_f32_count + s_f64_count; ++i) {
    float narrow;
    double wide;
    if (jb_json_at(root, i, &node) != HAL_OK) {
      return false;
    }
    if (i < s_f32_count ? (jb_json_get_f32(node, &k_float, scratch(), &narrow) != HAL_OK ||
                           memcmp(&narrow, &s_f32[i], sizeof(narrow)) != 0)
                        : (jb_json_get_f64(node, &k_float, scratch(), &wide) != HAL_OK ||
                           memcmp(&wide, &s_f64[i - s_f32_count], sizeof(wide)) != 0)) {
      return false;
    }
  }
  return true;
}

static uint32_t s_checks;

/* Independent expected values; each passed check sets one bit. */
static void run_checks(void) {
  jb_json_node_t node, other;
  uint64_t u64 = 0u;
  int64_t i64 = 0;
  int order = 0;
  jb_json_requirements_t needs = {0u, 0u};
  bool pass[8];
  pass[0] = check_f32(s_floats, 4u, 0x3f800001u) && check_f32(s_floats, 5u, 0x3f800001u);
  pass[1] =
      check_f64(s_floats, 6u, 0x4340000000000000u) && check_f64(s_floats, 7u, 0x4340000000000000u);
  pass[2] =
      jb_json_at(s_floats, 6u, &node) == HAL_OK && jb_json_at(s_floats, 7u, &other) == HAL_OK &&
      jb_json_number_compare(node, other, scratch(), MAX_WORK, &order) == HAL_OK && order == 1;
  pass[3] = jb_json_at(s_integers, 10u, &node) == HAL_OK && jb_json_get_u64(node, &u64) == HAL_OK &&
            u64 == UINT64_MAX;
  pass[4] = jb_json_at(s_integers, 11u, &node) == HAL_OK && jb_json_get_i64(node, &i64) == HAL_OK &&
            i64 == INT64_MIN;
  pass[5] = jb_json_measure(text(jb_input_config, JB_INPUT_CONFIG_LENGTH), &k_limits, scratch(),
                            &needs, NULL) == HAL_OK &&
            needs.node_count == s_config.token_count;
  pass[6] = w_utf8() == HAL_OK;
  pass[7] = check_round_trip();
  s_checks = 0u;
  for (size_t i = 0u; i < COUNTOF(pass); ++i) {
    s_checks |= pass[i] ? 1u << i : 0u;
  }
  s_pass = s_checks == (1u << COUNTOF(pass)) - 1u;
}

static hal_status_t store_f32(jb_json_node_t item) {
  return s_f32_count < COUNTOF(s_f32)
             ? jb_json_get_f32(item, &k_float, scratch(), &s_f32[s_f32_count++])
             : HAL_EOVERFLOW;
}

static hal_status_t store_f64(jb_json_node_t item) {
  return s_f64_count < COUNTOF(s_f64)
             ? jb_json_get_f64(item, &k_float, scratch(), &s_f64[s_f64_count++])
             : HAL_EOVERFLOW;
}

static hal_status_t prepare(void) {
  jb_json_node_t root;
  hal_status_t status = w_parse();
  status = status == HAL_OK ? jb_json_parse(text(jb_input_numbers, JB_INPUT_NUMBERS_LENGTH),
                                            &k_limits, s_number_tokens, COUNTOF(s_number_tokens),
                                            scratch(), &s_numbers, NULL)
                            : status;
  status = status == HAL_OK ? jb_json_root(&s_numbers, &root) : status;
  status = status == HAL_OK ? jb_json_find(root, text("integers", 8u), &s_integers) : status;
  status = status == HAL_OK ? jb_json_find(root, text("floats", 6u), &s_floats) : status;
  status = status == HAL_OK ? jb_json_find(root, text("wide", 4u), &s_wide) : status;
  status = status == HAL_OK ? each_item(s_floats, store_f32) : status;
  status = status == HAL_OK ? each_item(s_floats, store_f64) : status;
  return status == HAL_OK ? each_item(s_wide, store_f64) : status;
}

static void measure_all(void) {
  hal_status_t status = HAL_OK;
  s_baseline_stack = measure_stack(w_empty, &status);
  for (size_t i = 0u; i < COUNTOF(k_cases); ++i) {
    result_t *result = &s_results[i];
    result->stack = measure_stack(k_cases[i].run, &result->status);
    result->scratch_required = k_cases[i].scratch_required ? k_cases[i].scratch_required() : 0u;
    result->scratch_used = k_cases[i].scratch_required ? measure_scratch(k_cases[i].run) : 0u;
    result->ns = measure_time(k_cases[i].run, &result->iterations);
    led_toggle();
    hal_delay_ms(1u); /* lets lower-priority tasks (ESP-IDF idle) run */
  }
}

/* One report line per call; returns false after the last line. */
static bool report_line(size_t line) {
  hal_system_architecture_t arch;
  if (line == 0u) {
    const bool known = hal_system_get_current_architecture(&arch) == HAL_OK;
    deb("JBM begin target=%s cpu_hz=%" PRIu32 " token_bytes=%lu config_bytes=%lu config_nodes=%lu "
        "numbers_bytes=%lu baseline_stack=%" PRIu32 " paint=%lu",
        HAL_TARGET_NAME, known ? arch.cpu_clock_hz : 0u, (unsigned long)sizeof(jb_json_token_t),
        (unsigned long)JB_INPUT_CONFIG_LENGTH, (unsigned long)s_config.token_count,
        (unsigned long)JB_INPUT_NUMBERS_LENGTH, s_baseline_stack, (unsigned long)STACK_PAINT_BYTES);
    return true;
  }
  if (line <= COUNTOF(k_cases)) {
    const result_t *result = &s_results[line - 1u];
    deb("JBM case=%s status=%s ns=%" PRIu32 " iterations=%" PRIu32 " stack=%" PRIu32
        " scratch_used=%lu scratch_required=%lu",
        k_cases[line - 1u].name, hal_status_to_string(result->status), result->ns,
        result->iterations, result->stack, (unsigned long)result->scratch_used,
        (unsigned long)result->scratch_required);
    return true;
  }
  deb("JBM end checks=0x%02" PRIx32 " result=%s", s_checks, s_pass ? "PASS" : "FAIL");
  return false;
}

static uint32_t s_report_ms, s_line_ms, s_blink_ms;
static size_t s_line;
static bool s_reporting;

void app_start(void) {
#if defined(JH_HW_NODE_HAT_RELAY1_PIN_OUT)
  /* CAN-FD HAT relay drivers have no gate pull-down: drive them off. */
  static const uint8_t k_relays[] = {JH_HW_NODE_HAT_RELAY1_PIN_OUT, JH_HW_NODE_HAT_RELAY2_PIN_OUT,
                                     JH_HW_NODE_HAT_RELAY3_PIN_OUT, JH_HW_NODE_HAT_RELAY4_PIN_OUT};
  for (size_t i = 0u; i < COUNTOF(k_relays); ++i) {
    hal_gpio_set_mode(k_relays[i], HAL_GPIO_OUTPUT_LOW);
  }
#endif
  led_init();
}

void app_task0(void) {
  const uint32_t now = hal_millis();
  if (!s_measured) {
    s_pass = prepare() == HAL_OK;
    if (s_pass) {
      measure_all();
    }
    s_measured = true;
    if (s_pass) {
      run_checks();
    }
    s_report_ms = now - REPORT_PERIOD_MS;
    return;
  }
  if (!s_reporting && hal_elapsed_u32(now, s_report_ms, REPORT_PERIOD_MS)) {
    s_reporting = true;
    s_report_ms = now;
    s_line = 0u;
  }
  if (s_reporting && hal_elapsed_u32(now, s_line_ms, LINE_GAP_MS)) {
    s_line_ms = now;
    s_reporting = report_line(s_line++);
  }
  if (hal_elapsed_u32(now, s_blink_ms, s_pass ? 500u : 100u)) {
    s_blink_ms = now;
    led_toggle();
  }
}
