#ifndef INTRAVENOUS_PRESENTATION_STATE_JSON_H
#define INTRAVENOUS_PRESENTATION_STATE_JSON_H

#include "presentation.h"

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

/* Save a finite double as a decimal significand and a base-ten exponent. */
static inline void iv_json_number(IvJsonWriter *writer, const char *key, double value)
{
    iv_json_key(writer, key);
    if (!iv_json_value(writer)) return;
    if (!__builtin_isfinite(value)) { writer->failed = true; return; }
    if (value == 0) { iv_json_byte(writer, '0'); return; }
    if (value < 0) { iv_json_byte(writer, '-'); value = -value; }
    int exponent = 0;
    while (value >= 10) { value /= 10; ++exponent; }
    while (value < 1) { value *= 10; --exponent; }
    uint64_t digits = (uint64_t)(value * 10000000000000000. + .5);
    uint64_t power = 10000000000000000u;
    if (digits >= 10 * power) { digits /= 10; ++exponent; }
    iv_json_byte(writer, (char)('0' + digits / power));
    digits %= power;
    if (digits) {
        iv_json_byte(writer, '.');
        for (power /= 10; digits && power; power /= 10) {
            iv_json_byte(writer, (char)('0' + digits / power));
            digits %= power;
        }
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
    return (IvJsonReader){start, input->begin};
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
    iv_json_skip_space(&object);
    if (object.begin == object.end || *object.begin++ != '{') return (IvJsonReader){0, 0};
    for (;;) {
        iv_json_skip_space(&object);
        if (object.begin == object.end || *object.begin != '"') return (IvJsonReader){0, 0};
        IvJsonReader key = iv_json_take(&object);
        iv_json_skip_space(&object);
        if (object.begin == object.end || *object.begin++ != ':') return (IvJsonReader){0, 0};
        IvJsonReader value = iv_json_take(&object);
        if (iv_json_token_is(key, name)) return value;
        iv_json_skip_space(&object);
        if (object.begin == object.end || *object.begin++ != ',') return (IvJsonReader){0, 0};
    }
}

static inline double iv_json_read_number(IvJsonReader value, double fallback)
{
    if (value.begin == value.end) return fallback;
    int negative = *value.begin == '-';
    if (negative) ++value.begin;
    if (value.begin == value.end || *value.begin < '0' || *value.begin > '9') return fallback;
    double result = 0;
    while (value.begin < value.end && *value.begin >= '0' && *value.begin <= '9')
        result = result * 10 + (*value.begin++ - '0');
    if (value.begin < value.end && *value.begin == '.') {
        ++value.begin;
        double place = .1;
        while (value.begin < value.end && *value.begin >= '0' && *value.begin <= '9') {
            result += (*value.begin++ - '0') * place;
            place *= .1;
        }
    }
    if (value.begin != value.end && (*value.begin == 'e' || *value.begin == 'E')) {
        ++value.begin;
        bool decrease = value.begin != value.end && *value.begin == '-';
        if (value.begin != value.end && (*value.begin == '+' || *value.begin == '-')) ++value.begin;
        if (value.begin == value.end || *value.begin < '0' || *value.begin > '9') return fallback;
        int exponent = 0;
        while (value.begin < value.end && *value.begin >= '0' && *value.begin <= '9') {
            exponent = exponent * 10 + (*value.begin++ - '0');
            if (exponent > 324) return fallback;
        }
        for (; exponent > 0; --exponent) result = decrease ? result / 10 : result * 10;
    }
    return value.begin == value.end && __builtin_isfinite(result) ? (negative ? -result : result) : fallback;
}


/* Read a decimal member with a simple field name from serialized state. */
static inline double iv_json_read_decimal_member(IvString json, const char *key, double fallback)
{
    if (!json.data || !json.size) return fallback;
    IvJsonReader object = {json.data, json.data + json.size};
    return iv_json_read_number(iv_json_find_field(object, key), fallback);
}

#endif
