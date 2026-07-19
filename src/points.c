/*
 * points.c - point table loading, value decoding and report decisions.
 *
 * CSV columns (see etc/points.csv.example):
 *   tag,device,func,addr,dtype,worder,scale,offset,unit,deadband
 */
#include "points.h"
#include "util.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------ */
/* enum helpers                                                        */
/* ------------------------------------------------------------------ */

bool mb_func_from_str(const char *s, mb_func_e *out)
{
    if (s == NULL)
        return false;
    if (strcasecmp(s, "coil") == 0 || strcmp(s, "1") == 0) {
        *out = MB_FC_COIL;
    } else if (strcasecmp(s, "discrete") == 0 ||
               strcasecmp(s, "discrete_input") == 0 || strcmp(s, "2") == 0) {
        *out = MB_FC_DISCRETE;
    } else if (strcasecmp(s, "holding") == 0 || strcmp(s, "3") == 0) {
        *out = MB_FC_HOLDING;
    } else if (strcasecmp(s, "input") == 0 ||
               strcasecmp(s, "input_register") == 0 || strcmp(s, "4") == 0) {
        *out = MB_FC_INPUT;
    } else {
        return false;
    }
    return true;
}

bool dtype_from_str(const char *s, dtype_e *out)
{
    if (s == NULL)
        return false;
    if (strcasecmp(s, "bit") == 0 || strcasecmp(s, "bool") == 0) {
        *out = DT_BIT;
    } else if (strcasecmp(s, "u16") == 0 || strcasecmp(s, "uint16") == 0) {
        *out = DT_U16;
    } else if (strcasecmp(s, "s16") == 0 || strcasecmp(s, "int16") == 0) {
        *out = DT_S16;
    } else if (strcasecmp(s, "u32") == 0 || strcasecmp(s, "uint32") == 0) {
        *out = DT_U32;
    } else if (strcasecmp(s, "s32") == 0 || strcasecmp(s, "int32") == 0) {
        *out = DT_S32;
    } else if (strcasecmp(s, "f32") == 0 || strcasecmp(s, "float") == 0 ||
               strcasecmp(s, "float32") == 0) {
        *out = DT_F32;
    } else {
        return false;
    }
    return true;
}

bool word_order_from_str(const char *s, word_order_e *out)
{
    if (s == NULL)
        return false;
    if (strcasecmp(s, "ABCD") == 0)      *out = WO_ABCD;
    else if (strcasecmp(s, "BADC") == 0) *out = WO_BADC;
    else if (strcasecmp(s, "CDAB") == 0) *out = WO_CDAB;
    else if (strcasecmp(s, "DCBA") == 0) *out = WO_DCBA;
    else return false;
    return true;
}

const char *mb_func_str(mb_func_e f)
{
    switch (f) {
    case MB_FC_COIL:     return "coil";
    case MB_FC_DISCRETE: return "discrete";
    case MB_FC_HOLDING:  return "holding";
    case MB_FC_INPUT:    return "input";
    default:             return "?";
    }
}

const char *dtype_str(dtype_e t)
{
    switch (t) {
    case DT_BIT: return "bit";
    case DT_U16: return "u16";
    case DT_S16: return "s16";
    case DT_U32: return "u32";
    case DT_S32: return "s32";
    case DT_F32: return "f32";
    default:     return "?";
    }
}

const char *quality_str(quality_e q)
{
    switch (q) {
    case QUALITY_GOOD:      return "good";
    case QUALITY_UNCERTAIN: return "uncertain";
    default:                return "bad";
    }
}

/* ------------------------------------------------------------------ */
/* decoding                                                            */
/* ------------------------------------------------------------------ */

size_t point_reg_span(const point_t *p)
{
    switch (p->dtype) {
    case DT_U32:
    case DT_S32:
    case DT_F32:
        return 2;
    default:
        return 1;
    }
}

/*
 * Reassemble a 32-bit image from two wire registers according to the
 * configured word/byte order. Modbus transmits each register big-endian;
 * everything else is a vendor permutation on top of that.
 */
static uint32_t assemble32(word_order_e wo, const uint16_t *regs)
{
    uint8_t hi0 = (uint8_t)(regs[0] >> 8), lo0 = (uint8_t)(regs[0] & 0xFF);
    uint8_t hi1 = (uint8_t)(regs[1] >> 8), lo1 = (uint8_t)(regs[1] & 0xFF);
    uint8_t b[4];

    switch (wo) {
    case WO_BADC: b[0] = lo0; b[1] = hi0; b[2] = lo1; b[3] = hi1; break;
    case WO_CDAB: b[0] = hi1; b[1] = lo1; b[2] = hi0; b[3] = lo0; break;
    case WO_DCBA: b[0] = lo1; b[1] = hi1; b[2] = lo0; b[3] = hi0; break;
    case WO_ABCD:
    default:      b[0] = hi0; b[1] = lo0; b[2] = hi1; b[3] = lo1; break;
    }
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}

void point_apply_regs(point_t *p, const uint16_t *regs, size_t nregs,
                      uint64_t now_ms)
{
    double raw;

    if (nregs < point_reg_span(p)) {
        p->quality = QUALITY_BAD;
        return;
    }

    switch (p->dtype) {
    case DT_U16:
        raw = (double)regs[0];
        break;
    case DT_S16:
        raw = (double)(int16_t)regs[0];
        break;
    case DT_U32:
        raw = (double)assemble32(p->worder, regs);
        break;
    case DT_S32:
        raw = (double)(int32_t)assemble32(p->worder, regs);
        break;
    case DT_F32: {
        uint32_t u = assemble32(p->worder, regs);
        float    f;
        memcpy(&f, &u, sizeof f);
        if (!isfinite((double)f)) {     /* NaN trap values from the PLC */
            p->quality = QUALITY_BAD;
            return;
        }
        raw = (double)f;
        break;
    }
    default:
        p->quality = QUALITY_BAD;
        return;
    }

    p->raw[0]     = regs[0];
    p->raw[1]     = (nregs > 1) ? regs[1] : 0;
    p->value      = raw * p->scale + p->offset;
    p->quality    = QUALITY_GOOD;
    p->updated_ms = now_ms;
}

void point_apply_bit(point_t *p, bool bit, uint64_t now_ms)
{
    p->raw[0]     = bit ? 1u : 0u;
    p->value      = (bit ? 1.0 : 0.0) * p->scale + p->offset;
    p->quality    = QUALITY_GOOD;
    p->updated_ms = now_ms;
}

/* ------------------------------------------------------------------ */
/* report decision (caller holds the table mutex)                      */
/* ------------------------------------------------------------------ */

bool point_should_report(const point_t *p, uint64_t now_ms,
                         uint32_t integrity_ms)
{
    if (!p->reported_once)
        return true;
    if (p->quality != p->reported_quality)
        return true;                        /* quality transition       */
    if (p->quality == QUALITY_GOOD) {
        double delta = fabs(p->value - p->reported_value);
        if (delta > p->deadband)
            return true;                    /* report-by-exception      */
    }
    if (integrity_ms > 0 && now_ms - p->reported_ms >= integrity_ms)
        return true;                        /* periodic integrity poll  */
    return false;
}

void point_mark_reported(point_t *p, uint64_t now_ms)
{
    p->reported_value   = p->value;
    p->reported_quality = p->quality;
    p->reported_ms      = now_ms;
    p->reported_once    = true;
}

/* ------------------------------------------------------------------ */
/* table management                                                    */
/* ------------------------------------------------------------------ */

void points_init(point_table_t *t)
{
    memset(t, 0, sizeof *t);
    pthread_mutex_init(&t->mtx, NULL);
}

point_t *points_find(point_table_t *t, const char *tag)
{
    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(t->items[i].tag, tag) == 0)
            return &t->items[i];
    }
    return NULL;
}

static int csv_err(const char *path, unsigned lineno, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, EICD_PROGNAME ": %s:%u: ", path, lineno);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return -1;
}

int points_load_csv(point_table_t *t, const char *path, const eicd_config_t *cfg)
{
    FILE    *fp;
    char     raw[512];
    unsigned lineno = 0;
    size_t   errors = 0;

    fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, EICD_PROGNAME ": cannot open points file '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    while (fgets(raw, sizeof raw, fp) != NULL) {
        char    *s;
        char    *fields[12];
        int      nf = 0;
        char    *cur;
        point_t *p;
        long     lnum;
        double   dnum;

        lineno++;
        if (strchr(raw, '\n') == NULL && !feof(fp)) {
            csv_err(path, lineno, "line too long");
            errors++;
            continue;
        }
        s = util_trim(raw);
        if (*s == '\0' || *s == '#')
            continue;

        /*
         * Split on commas preserving empty columns (strtok_r would
         * collapse ",," and silently shift every later column).
         */
        cur = s;
        while (nf < 12) {
            char *comma = strchr(cur, ',');
            if (comma != NULL)
                *comma = '\0';
            fields[nf++] = util_trim(cur);
            if (comma == NULL)
                break;
            cur = comma + 1;
        }
        if (nf == 0)
            continue;
        if (strcasecmp(fields[0], "tag") == 0)
            continue;                       /* header row */

        if (nf < 10) {
            csv_err(path, lineno, "expected 10 columns, got %d", nf);
            errors++;
            continue;
        }
        if (t->count >= EICD_MAX_POINTS) {
            csv_err(path, lineno, "point table full (%d max)", EICD_MAX_POINTS);
            errors++;
            break;
        }

        p = &t->items[t->count];
        memset(p, 0, sizeof *p);

        /* ---- tag ---- */
        if (!util_is_safe_tag(fields[0]) || strlen(fields[0]) >= EICD_TAG_MAX) {
            csv_err(path, lineno, "invalid tag '%s' ([A-Za-z0-9._-], <%d chars)",
                    fields[0], EICD_TAG_MAX);
            errors++;
            continue;
        }
        if (points_find(t, fields[0]) != NULL) {
            csv_err(path, lineno, "duplicate tag '%s'", fields[0]);
            errors++;
            continue;
        }
        util_strlcpy(p->tag, fields[0], sizeof p->tag);

        /* ---- device (must exist in the config) ---- */
        if (config_find_device(cfg, fields[1]) == NULL) {
            csv_err(path, lineno, "tag '%s': unknown device '%s'",
                    p->tag, fields[1]);
            errors++;
            continue;
        }
        util_strlcpy(p->device, fields[1], sizeof p->device);

        /* ---- function ---- */
        if (!mb_func_from_str(fields[2], &p->func)) {
            csv_err(path, lineno, "tag '%s': invalid func '%s' "
                    "(coil|discrete|holding|input)", p->tag, fields[2]);
            errors++;
            continue;
        }

        /* ---- address ---- */
        if (!util_parse_long(fields[3], &lnum) || lnum < 0 || lnum > 65535) {
            csv_err(path, lineno, "tag '%s': invalid address '%s'",
                    p->tag, fields[3]);
            errors++;
            continue;
        }
        p->addr = (uint16_t)lnum;

        /* ---- data type, cross-checked against the function ---- */
        if (!dtype_from_str(fields[4], &p->dtype)) {
            csv_err(path, lineno, "tag '%s': invalid dtype '%s' "
                    "(bit|u16|s16|u32|s32|f32)", p->tag, fields[4]);
            errors++;
            continue;
        }
        if ((p->func == MB_FC_COIL || p->func == MB_FC_DISCRETE) &&
            p->dtype != DT_BIT) {
            csv_err(path, lineno, "tag '%s': coil/discrete points require dtype bit",
                    p->tag);
            errors++;
            continue;
        }
        if ((p->func == MB_FC_HOLDING || p->func == MB_FC_INPUT) &&
            p->dtype == DT_BIT) {
            csv_err(path, lineno, "tag '%s': register points cannot use dtype bit",
                    p->tag);
            errors++;
            continue;
        }

        /* ---- word order (only meaningful for 32-bit types) ---- */
        p->worder = WO_ABCD;
        if (fields[5][0] != '\0' &&
            !word_order_from_str(fields[5], &p->worder)) {
            csv_err(path, lineno, "tag '%s': invalid word order '%s' "
                    "(ABCD|BADC|CDAB|DCBA)", p->tag, fields[5]);
            errors++;
            continue;
        }

        /* ---- scale / offset ---- */
        if (!util_parse_double(fields[6], &p->scale) || p->scale == 0.0) {
            csv_err(path, lineno, "tag '%s': invalid (zero) scale '%s'",
                    p->tag, fields[6]);
            errors++;
            continue;
        }
        if (!util_parse_double(fields[7], &p->offset)) {
            csv_err(path, lineno, "tag '%s': invalid offset '%s'",
                    p->tag, fields[7]);
            errors++;
            continue;
        }

        /* ---- unit (free text, ends up inside the JSON frame) ---- */
        if (strlen(fields[8]) >= EICD_UNIT_MAX ||
            !util_is_json_safe(fields[8])) {
            csv_err(path, lineno, "tag '%s': invalid unit '%s'",
                    p->tag, fields[8]);
            errors++;
            continue;
        }
        util_strlcpy(p->unit, fields[8], sizeof p->unit);

        /* ---- deadband ---- */
        if (!util_parse_double(fields[9], &dnum) || dnum < 0.0) {
            csv_err(path, lineno, "tag '%s': invalid deadband '%s'",
                    p->tag, fields[9]);
            errors++;
            continue;
        }
        p->deadband = dnum;

        p->quality = QUALITY_BAD;           /* until the first good read */
        t->count++;
    }

    fclose(fp);

    if (errors > 0) {
        fprintf(stderr, EICD_PROGNAME ": %zu error(s) in points file '%s'\n",
                errors, path);
        return -1;
    }
    return 0;
}
