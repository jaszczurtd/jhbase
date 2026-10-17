#ifndef JB_NUMBER_INTERNAL_H
#define JB_NUMBER_INTERNAL_H
#include "jb/json_number.h"

typedef struct {
  size_t remaining;
} jb_budget_t;
typedef struct {
  unsigned char *data;
  size_t used, capacity;
  jb_budget_t *budget;
} jb_big_t;

/* All multi-byte scratch accesses copy object representations, never overlay. */
bool jb_charge(jb_budget_t *budget, size_t amount);
uint32_t jb_big_get(const jb_big_t *a, size_t at);
void jb_big_set(jb_big_t *a, size_t at, uint32_t value);
bool jb_big_small(jb_big_t *a, uint64_t value);
bool jb_big_mul_add(jb_big_t *a, uint32_t factor, uint32_t add);
bool jb_big_shift(jb_big_t *out, const jb_big_t *a, size_t bits);
bool jb_big_compare(const jb_big_t *a, const jb_big_t *b, int *out);
bool jb_big_sub(jb_big_t *a, const jb_big_t *b);
size_t jb_big_bits(const jb_big_t *a);
bool jb_big_div_small(jb_big_t *a, uint32_t divisor, uint32_t *remainder);
hal_status_t jb_number_validate(jb_json_span_t token);
hal_status_t jb_number_scratch_size(size_t length, size_t *out);
hal_status_t jb_number_bits(jb_json_span_t token, unsigned width,
                            const jb_json_float_options_t *options, jb_json_workspace_t work,
                            uint64_t *out);
hal_status_t jb_number_bits_budget(jb_json_span_t token, unsigned width,
                                   const jb_json_float_options_t *options, jb_json_workspace_t work,
                                   jb_budget_t *budget, uint64_t *out);
/* A finite IEEE value needs at most 1074 decimal places, stored in caller work. */
#define JB_FLOAT_FORMAT_SCRATCH 2048u
hal_status_t jb_float_shortest(uint64_t bits, unsigned width, char *out, size_t *length);
hal_status_t jb_float_format(uint64_t bits, unsigned width, unsigned precision,
                             jb_json_workspace_t work, jb_budget_t *budget, char *out,
                             size_t *length);
size_t jb_u64_digits(uint64_t value, char *out);

#endif
