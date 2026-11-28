#ifndef INTRAVENOUS_PRESENTATION_STATE_JSON_NUMBER_H
#define INTRAVENOUS_PRESENTATION_STATE_JSON_NUMBER_H

#include <stdbool.h>
#include <stdint.h>

/* Exact integer arithmetic for the binary64 and decimal conversion in saved
   state. 800 significant decimal digits cover every binary64 midpoint.
   4096 bits cover the coefficient and all shifts in the supported range. */
enum { iv_json_significant_digits = 800, iv_json_integer_words = 128 };
typedef struct {
    uint32_t words[iv_json_integer_words];
    unsigned size;
} IvJsonInteger;

static inline void iv_json_integer_append_digit(IvJsonInteger *integer, uint32_t base, uint32_t carry)
{
    for (unsigned i = 0; i < integer->size; ++i) {
        uint64_t word = (uint64_t)integer->words[i] * base + carry;
        integer->words[i] = (uint32_t)word;
        carry = (uint32_t)(word >> 32);
    }
    if (carry) integer->words[integer->size++] = carry;
}

static inline void iv_json_integer_shift_left(IvJsonInteger *integer, unsigned shift)
{
    if (integer->size == 0 || shift == 0) return;
    unsigned words = shift / 32, bits = shift % 32;
    uint32_t carry = bits ? integer->words[integer->size - 1] >> (32 - bits) : 0;
    if (carry) integer->words[integer->size + words] = carry;
    for (unsigned i = integer->size; i; --i) {
        uint32_t lower = bits && i > 1 ? integer->words[i - 2] >> (32 - bits) : 0;
        integer->words[i - 1 + words] = (integer->words[i - 1] << bits) | lower;
    }
    for (unsigned i = 0; i < words; ++i) integer->words[i] = 0;
    integer->size += words + (carry != 0);
}

static inline void iv_json_integer_shift_right_one(IvJsonInteger *integer)
{
    uint32_t carry = 0;
    for (unsigned i = integer->size; i; --i) {
        uint32_t word = integer->words[i - 1];
        integer->words[i - 1] = (word >> 1) | carry;
        carry = word << 31;
    }
    if (integer->size && !integer->words[integer->size - 1]) --integer->size;
}

static inline int iv_json_integer_compare(const IvJsonInteger *left, const IvJsonInteger *right)
{
    if (left->size != right->size) return left->size < right->size ? -1 : 1;
    for (unsigned i = left->size; i; --i) {
        uint32_t a = left->words[i - 1], b = right->words[i - 1];
        if (a != b) return a < b ? -1 : 1;
    }
    return 0;
}

static inline void iv_json_integer_subtract(IvJsonInteger *left, const IvJsonInteger *right)
{
    uint64_t borrow = 0;
    for (unsigned i = 0; i < left->size; ++i) {
        uint64_t a = left->words[i], b = (i < right->size ? (uint64_t)right->words[i] : 0) + borrow;
        left->words[i] = (uint32_t)(a - b);
        borrow = a < b;
    }
    while (left->size && !left->words[left->size - 1]) --left->size;
}

static inline unsigned iv_json_integer_bits(const IvJsonInteger *integer)
{
    return integer->size ? 32 * integer->size - __builtin_clz(integer->words[integer->size - 1]) : 0;
}

static inline uint32_t iv_json_integer_divide_decimal(IvJsonInteger *integer)
{
    uint64_t remainder = 0;
    for (unsigned i = integer->size; i; --i) {
        uint64_t word = (remainder << 32) | integer->words[i - 1];
        integer->words[i - 1] = (uint32_t)(word / 10);
        remainder = word % 10;
    }
    if (integer->size && !integer->words[integer->size - 1]) --integer->size;
    return (uint32_t)remainder;
}

/* Return the significand in reverse decimal order, with a separate scale.
   The exact decimal also round-trips through the host's JSON parser. */
static inline unsigned iv_json_binary64_digits(uint64_t magnitude, char *digits, int *decimal_exponent)
{
    uint64_t mantissa = magnitude & UINT64_C(0x000fffffffffffff);
    int biased_exponent = (int)(magnitude >> 52);
    if (biased_exponent) mantissa |= UINT64_C(1) << 52;
    int exponent = (biased_exponent ? biased_exponent - 1023 : -1022) - 52;
    while (exponent < 0 && !(mantissa & 1)) { mantissa >>= 1; ++exponent; }
    IvJsonInteger integer = {{(uint32_t)mantissa, (uint32_t)(mantissa >> 32)}, mantissa >> 32 ? 2u : 1u};
    if (exponent >= 0) iv_json_integer_shift_left(&integer, (unsigned)exponent);
    else for (int i = 0; i > exponent; --i) iv_json_integer_append_digit(&integer, 5, 0);
    unsigned size = 0;
    while (integer.size) digits[size++] = (char)('0' + iv_json_integer_divide_decimal(&integer));
    *decimal_exponent = (int)size - 1 + (exponent < 0 ? exponent : 0);
    return size;
}

/* Compute the nearest binary64 bit pattern from a nonzero decimal coefficient.
   Round the quotient once, with ties to even, including subnormal values. */
static inline uint64_t iv_json_decimal_binary64(IvJsonInteger *numerator, int decimal_exponent, bool sticky)
{
    IvJsonInteger denominator = {{1}, 1};
    IvJsonInteger *scale = decimal_exponent < 0 ? &denominator : numerator;
    unsigned power = (unsigned)(decimal_exponent < 0 ? -decimal_exponent : decimal_exponent);
    for (unsigned i = 0; i < power; ++i) iv_json_integer_append_digit(scale, 5, 0);
    iv_json_integer_shift_left(scale, power);

    int exponent = (int)iv_json_integer_bits(numerator) - (int)iv_json_integer_bits(&denominator);
    IvJsonInteger shifted;
    if (exponent >= 0) {
        shifted = denominator;
        iv_json_integer_shift_left(&shifted, (unsigned)exponent);
        if (iv_json_integer_compare(numerator, &shifted) < 0) --exponent;
    } else {
        shifted = *numerator;
        iv_json_integer_shift_left(&shifted, (unsigned)-exponent);
        if (iv_json_integer_compare(&shifted, &denominator) < 0) --exponent;
    }
    const uint64_t infinity = UINT64_C(0x7ff0000000000000);
    if (exponent > 1023) return infinity;
    if (exponent < -1075) return 0;
    int unit = (exponent < -1022 ? -1022 : exponent) - 52;
    if (unit < 0) iv_json_integer_shift_left(numerator, (unsigned)-unit);
    else iv_json_integer_shift_left(&denominator, (unsigned)unit);

    uint64_t mantissa = 0;
    int bit = (int)iv_json_integer_bits(numerator) - (int)iv_json_integer_bits(&denominator);
    if (bit >= 0) {
        shifted = denominator;
        iv_json_integer_shift_left(&shifted, (unsigned)bit);
        for (; bit >= 0; --bit) {
            if (iv_json_integer_compare(numerator, &shifted) >= 0) {
                iv_json_integer_subtract(numerator, &shifted);
                mantissa |= UINT64_C(1) << bit;
            }
            iv_json_integer_shift_right_one(&shifted);
        }
    }
    iv_json_integer_shift_left(numerator, 1);
    int half = iv_json_integer_compare(numerator, &denominator);
    if (half > 0 || (half == 0 && (sticky || (mantissa & 1)))) ++mantissa;
    if (exponent < -1022) return mantissa;
    if (mantissa == (UINT64_C(1) << 53)) { mantissa >>= 1; ++exponent; }
    return ((uint64_t)(exponent + 1023) << 52) | (mantissa & UINT64_C(0x000fffffffffffff));
}

#endif
