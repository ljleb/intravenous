#ifndef INTRAVENOUS_PRESENTATION_STATE_JSON_H
#define INTRAVENOUS_PRESENTATION_STATE_JSON_H

#include "presentation.h"
#include "state_json_number.h"

typedef struct IvJsonWriter {
    char *buffer;
    uint32_t capacity;
    uint32_t length;
    uint8_t depth;
    bool failed;
    uint8_t context[32];
} IvJsonWriter;

enum {
    iv_json_object_first, iv_json_object_next, iv_json_object_value,
    iv_json_array_first, iv_json_array_next,
};

static inline void iv_json_byte(IvJsonWriter *writer, char byte)
{
    if (writer->failed) return;
    if (writer->length == writer->capacity) {
        writer->failed = true;
        return;
    }
    writer->buffer[writer->length++] = byte;
}

static inline void iv_json_quoted(IvJsonWriter *writer, const char *text)
{
    iv_json_byte(writer, '"');
    for (; *text && !writer->failed; ++text) {
        uint8_t byte = (uint8_t)*text;
        if (byte == '"' || byte == '\\') iv_json_byte(writer, '\\');
        if (byte < 32) {
            const char *hex = "0123456789abcdef";
            iv_json_byte(writer, '\\');
            iv_json_byte(writer, 'u');
            iv_json_byte(writer, '0');
            iv_json_byte(writer, '0');
            iv_json_byte(writer, hex[byte >> 4]);
            iv_json_byte(writer, hex[byte & 15]);
        } else iv_json_byte(writer, *text);
    }
    iv_json_byte(writer, '"');
}

static inline bool iv_json_value(IvJsonWriter *writer)
{
    if (writer->failed) return false;
    if (!writer->depth) {
        writer->failed = writer->length != 0;
        return !writer->failed;
    }
    uint8_t *context = &writer->context[writer->depth - 1];
    switch (*context) {
    case iv_json_object_value: *context = iv_json_object_next; break;
    case iv_json_array_first: *context = iv_json_array_next; break;
    case iv_json_array_next: iv_json_byte(writer, ','); break;
    default: writer->failed = true; break;
    }
    return !writer->failed;
}

static inline void iv_json_begin(IvJsonWriter *writer, uint8_t context, char opening)
{
    if (!iv_json_value(writer)) return;
    if (writer->depth == sizeof(writer->context)) { writer->failed = true; return; }
    iv_json_byte(writer, opening);
    writer->context[writer->depth++] = context;
}

static inline void iv_json_object(IvJsonWriter *writer)
{
    iv_json_begin(writer, iv_json_object_first, '{');
}

static inline void iv_json_array(IvJsonWriter *writer)
{
    iv_json_begin(writer, iv_json_array_first, '[');
}

static inline void iv_json_end(IvJsonWriter *writer)
{
    if (writer->failed) return;
    if (!writer->depth || writer->context[writer->depth - 1] == iv_json_object_value) {
        writer->failed = true;
        return;
    }
    uint8_t context = writer->context[--writer->depth];
    iv_json_byte(writer, context == iv_json_array_first || context == iv_json_array_next ? ']' : '}');
}

static inline void iv_json_key(IvJsonWriter *writer, const char *key)
{
    if (writer->failed) return;
    if (!writer->depth) { writer->failed = true; return; }
    uint8_t *context = &writer->context[writer->depth - 1];
    if (*context == iv_json_object_next) iv_json_byte(writer, ',');
    else if (*context != iv_json_object_first) { writer->failed = true; return; }
    iv_json_quoted(writer, key);
    iv_json_byte(writer, ':');
    *context = iv_json_object_value;
}

static inline void iv_json_string(IvJsonWriter *writer, const char *key, const char *text)
{
    iv_json_key(writer, key);
    if (iv_json_value(writer)) iv_json_quoted(writer, text);
}

static inline void iv_json_array_member(IvJsonWriter *writer, const char *key)
{
    iv_json_key(writer, key);
    iv_json_array(writer);
}

static inline void iv_json_object_member(IvJsonWriter *writer, const char *key)
{
    iv_json_key(writer, key);
    iv_json_object(writer);
}

// Write a finite decimal in [-1e9, 1e9], rounded to six fractional digits.
static inline void iv_json_decimal(IvJsonWriter *writer, const char *key, double value)
{
    iv_json_key(writer, key);
    if (!iv_json_value(writer)) return;
    if (!(value >= -1e9 && value <= 1e9)) { writer->failed = true; return; }
    if (value < 0) {
        iv_json_byte(writer, '-');
        value = -value;
    }
    uint64_t scaled = (uint64_t)(value * 1000000 + .5);
    uint64_t whole = scaled / 1000000;
    char digits[10];
    uint32_t length = 0;
    do {
        digits[length++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole);
    while (length) iv_json_byte(writer, digits[--length]);
    uint32_t fraction = (uint32_t)(scaled % 1000000);
    if (!fraction) return;
    iv_json_byte(writer, '.');
    for (uint32_t place = 100000; place && fraction; place /= 10) {
        iv_json_byte(writer, (char)('0' + fraction / place));
        fraction %= place;
    }
}

/* Save a finite binary64 number as a decimal with no loss of precision. */
static inline void iv_json_number(IvJsonWriter *writer, const char *key, double value)
{
    iv_json_key(writer, key);
    if (!iv_json_value(writer)) return;
    if (!__builtin_isfinite(value)) { writer->failed = true; return; }
    uint64_t bits = __builtin_bit_cast(uint64_t, value);
    uint64_t magnitude = bits & UINT64_C(0x7fffffffffffffff);
    if (bits >> 63) iv_json_byte(writer, '-');
    if (!magnitude) { iv_json_byte(writer, '0'); return; }

    char digits[iv_json_significant_digits];
    int exponent = 0;
    unsigned size = iv_json_binary64_digits(magnitude, digits, &exponent);
    unsigned first = 0;
    while (first + 1 < size && digits[first] == '0') ++first;
    iv_json_byte(writer, digits[--size]);
    if (size > first) {
        iv_json_byte(writer, '.');
        while (size > first) iv_json_byte(writer, digits[--size]);
    }
    if (!exponent) return;
    iv_json_byte(writer, 'e');
    if (exponent < 0) { iv_json_byte(writer, '-'); exponent = -exponent; }
    if (exponent >= 100) iv_json_byte(writer, (char)('0' + exponent / 100));
    if (exponent >= 10) iv_json_byte(writer, (char)('0' + exponent / 10 % 10));
    iv_json_byte(writer, (char)('0' + exponent % 10));
}

static inline int32_t iv_json_finish(const IvJsonWriter *writer)
{
    return writer->failed || writer->depth || !writer->length || writer->length > INT32_MAX
        ? -1 : (int32_t)writer->length;
}

typedef struct { char const* begin; char const* end; } IvJsonReader;

static inline void iv_json_skip_space(IvJsonReader* input)
{
    while (input->begin < input->end && *input->begin <= ' ') ++input->begin;
}

static inline void iv_json_skip_string(IvJsonReader* input)
{
    while (++input->begin < input->end) {
        if (*input->begin == '"') { ++input->begin; return; }
        if (*input->begin == '\\' && input->begin + 1 < input->end) ++input->begin;
    }
}

static inline IvJsonReader iv_json_take(IvJsonReader* input)
{
    iv_json_skip_space(input);
    char const* start = input->begin;
    if (start == input->end) return *input;
    if (*start == '"') iv_json_skip_string(input);
    else if (*start == '{' || *start == '[') {
        unsigned depth = 1;
        ++input->begin;
        while (depth && input->begin < input->end) {
            char c = *input->begin;
            if (c == '"') { iv_json_skip_string(input); continue; }
            if (c == '{' || c == '[') ++depth;
            if (c == '}' || c == ']') --depth;
            ++input->begin;
        }
    } else {
        while (input->begin < input->end && *input->begin > ' '
            && *input->begin != ']' && *input->begin != '}' && *input->begin != ',')
            ++input->begin;
    }
    IvJsonReader token = {start, input->begin};
    return token;
}

static inline int iv_json_token_is(IvJsonReader value, char const* literal)
{
    if (!value.begin || value.end - value.begin < 2 || *value.begin++ != '"' || *--value.end != '"') return 0;
    for (; *literal && value.begin < value.end; ++literal, ++value.begin)
        if (*value.begin != *literal) return 0;
    return value.begin == value.end && !*literal;
}

static inline IvJsonReader iv_json_find_field(IvJsonReader object, char const* name)
{
    const IvJsonReader absent = {0, 0};
    iv_json_skip_space(&object);
    if (object.begin == object.end || *object.begin++ != '{') return absent;
    for (;;) {
        iv_json_skip_space(&object);
        if (object.begin == object.end || *object.begin != '"') return absent;
        IvJsonReader key = iv_json_take(&object);
        iv_json_skip_space(&object);
        if (object.begin == object.end || *object.begin++ != ':') return absent;
        IvJsonReader value = iv_json_take(&object);
        if (iv_json_token_is(key, name)) return value;
        iv_json_skip_space(&object);
        if (object.begin == object.end || *object.begin++ != ',') return absent;
    }
}

typedef struct {
    IvJsonInteger coefficient;
    unsigned digits, discarded;
    bool sticky;
} IvJsonDecimal;

static inline bool iv_json_read_digit(IvJsonReader *value, IvJsonDecimal *decimal)
{
    if (value->begin == value->end || *value->begin < '0' || *value->begin > '9') return false;
    uint32_t digit = (uint32_t)(*value->begin++ - '0');
    if (!digit && !decimal->digits) return true;
    if (decimal->digits < iv_json_significant_digits) {
        iv_json_integer_append_digit(&decimal->coefficient, 10, digit);
        ++decimal->digits;
    } else {
        ++decimal->discarded;
        decimal->sticky |= digit != 0;
    }
    return true;
}

static inline double iv_json_read_number(IvJsonReader value, double fallback)
{
    if (value.begin == value.end) return fallback;
    bool negative = *value.begin == '-';
    if (negative) ++value.begin;

    IvJsonDecimal decimal = {{{0}, 0}, 0, 0, false};
    if (!iv_json_read_digit(&value, &decimal)) return fallback;
    while (iv_json_read_digit(&value, &decimal)) {}
    int64_t fractional_digits = 0;
    if (value.begin != value.end && *value.begin == '.') {
        ++value.begin;
        if (!iv_json_read_digit(&value, &decimal)) return fallback;
        ++fractional_digits;
        while (iv_json_read_digit(&value, &decimal)) ++fractional_digits;
    }

    int64_t exponent = 0;
    if (value.begin != value.end && (*value.begin == 'e' || *value.begin == 'E')) {
        ++value.begin;
        bool decrease = value.begin != value.end && *value.begin == '-';
        if (value.begin != value.end && (*value.begin == '+' || *value.begin == '-')) ++value.begin;
        if (value.begin == value.end || *value.begin < '0' || *value.begin > '9') return fallback;
        while (value.begin != value.end && *value.begin >= '0' && *value.begin <= '9') {
            int digit = *value.begin++ - '0';
            // No accepted state string can offset an exponent this large.
            if (exponent <= UINT32_MAX) exponent = exponent * 10 + digit;
        }
        if (decrease) exponent = -exponent;
    }
    if (value.begin != value.end) return fallback;

    uint64_t sign = negative ? UINT64_C(1) << 63 : 0;
    if (!decimal.digits) return __builtin_bit_cast(double, sign);
    exponent += decimal.discarded - fractional_digits;
    int64_t magnitude = exponent + decimal.digits - 1;
    if (magnitude > 308) return fallback;
    if (magnitude < -324) return __builtin_bit_cast(double, sign);
    uint64_t bits = iv_json_decimal_binary64(&decimal.coefficient, (int)exponent, decimal.sticky);
    if (bits == UINT64_C(0x7ff0000000000000)) return fallback;
    return __builtin_bit_cast(double, bits | sign);
}

/* Read a decimal member with a simple field name from serialized state. */
static inline double iv_json_read_decimal_member(IvString json, const char *key, double fallback)
{
    if (!json.data || !json.size) return fallback;
    IvJsonReader object = {json.data, json.data + json.size};
    return iv_json_read_number(iv_json_find_field(object, key), fallback);
}

#endif
